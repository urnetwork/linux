// SPDX-License-Identifier: MPL-2.0
#include "AccountPage.hpp"
#include "AccountPlanRows.hpp"

#include "ReferralRoyalty.hpp"

#include <glib.h>
#include <adwaita.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "AddSignInFlow.hpp"
#include "AuthViews.hpp"
#include "BittensorManualSheet.hpp"
#include "BrandIcons.hpp"
#include "BittensorWalletFlow.hpp"
#include "WalletBridgeRoute.hpp"
#include "DeleteAccountOutcome.hpp"
#include "DisplayText.hpp"
#include "Formatters.hpp"
#include "GuestConversionSheet.hpp"
#include "I18n.hpp"
#include "ManageSubscription.hpp"
#include "PaneKit.hpp"
#include "RuntimePaths.hpp"
#include "ServerRefusalText.hpp"
#include "Ui.hpp"
#include "UrTheme.hpp"

// The installed data dir (meson passes -DUR_PKGDATADIR); the fallback keeps
// this TU self-contained the way SettingsPage.cpp's version stamp does.
#ifndef UR_PKGDATADIR
#define UR_PKGDATADIR "/usr/share/urnetwork"
#endif

namespace urnw {
namespace {

// ---- key numbers (spec §0.3, §1.1, §4) --------------------------------------
constexpr int kPaneAWidth = 360;
constexpr int kPaneCWidth = 380;
constexpr int kThreePaneDip = 1500;  // plan | account | codes
constexpr int kTwoPaneDip = 900;     // plan | account
// CONTRACT rule 3: a plain api call gets 20 s and then a verdict. There is no
// browser-bridge flow on this destination — the customer portal hands off to
// the system browser and never reports back — so 20 s is the only budget here.
constexpr int kApiTimeoutMs = 20000;
constexpr int kRowTall = 44;         // two-line rows
constexpr int kRowSingle = 40;       // single-line rows (Redeem)
constexpr int kRowKeyValue = 34;     // key/value rows
constexpr int kRowAuthLine = 38;     // the auth line (collapsed when empty)
constexpr int kRowCode = 36;         // table rows
constexpr int kProsePadY = 10;       // supporting prose: padding 12,10
constexpr int kStatePadY = 8;        // a state line: padding 12,8
constexpr int kPlanPadY = 14;        // the plan value row: padding 12,14
constexpr int kPlanValuePx = 22;
constexpr int kRingPx = 14;              // the confirmation ring
constexpr int kSheetWidth = 400;         // the delete sheet (MinWidth 400)
constexpr int kSheetWidthNarrow = 380;   // add-auth / auth-code sheets
constexpr int kConfirmWidth = 320;       // the remove-method confirm (MinWidth 320)
constexpr int kMinPasswordLength = 12;   // AddAuthSheet gate
constexpr double kAuthCodeMinutes = 5.0;   // matches the caption
constexpr int64_t kAuthCodeUses = 1;
constexpr int kAuthCodeAbbreviateAbove = 14;  // longer than this -> 6…6
constexpr int kAuthCodeShoulder = 6;
constexpr int kMaskShoulder = 3;      // balance-code secret: first 3 + last 3
constexpr int kIsoDatePrefix = 10;    // YYYY-MM-DD
constexpr int kValueChars = 22;       // client id / bonus code readouts (MaxWidth 220)

// Pro gold now lives in Ui.hpp's palette beside the referral gold ramp (the
// TODO(theme) that used to sit here).
// Body text is off-white, never pure white (Ui.hpp's kUrText is 0xFFFFFF).
constexpr Rgba kOffWhite{0xF8 / 255.0, 0xF8 / 255.0, 0xF8 / 255.0, 1.0};

// ---- tone -------------------------------------------------------------------
// A line's colour is a pango attribute, not a swapped CSS class. WHY (the same
// reason SettingsPage carries this helper): UrTheme's provider is APPLICATION+1
// and Ui.cpp's is APPLICATION, so a single-class tone (.ur-value-on,
// .dim-label) layered on a row class (.ur-value, .ur-caption) loses the
// cascade unless UrTheme carries an explicit two-class rule for that pair. A
// markup foreground beats the CSS colour for the run it covers, which is
// exactly what a state change needs.
void SetToned(Gtk::Label& line, const Rgba& color, const Glib::ustring& text) {
  line.set_markup("<span foreground='" + HexForMarkup(color) + "'>" +
                  Glib::Markup::escape_text(text) + "</span>");
}

// ---- FieldState (§0.4) ------------------------------------------------------
// One writer for a value line's TEXT and its COLOUR. All six renders are
// textually distinct, so a 401 can never look like an empty answer.
void ApplyFieldState(Gtk::Label& line, AccountFieldState state,
                     const Glib::ustring& loadedText = {}) {
  Glib::ustring text;
  Rgba tone = kUrTextMuted;
  switch (state) {
    case AccountFieldState::Loaded:
      text = loadedText;
      break;
    case AccountFieldState::Loading:
      text = T_("loading", "Loading...");
      tone = kUrTextFaint;
      break;
    case AccountFieldState::Empty:
      text = T_("none", "None");
      tone = kUrTextFaint;
      break;
    case AccountFieldState::NoSession:
      text = T_("please_login_to_urnetwork", "Please login to URnetwork");
      tone = kUrTextFaint;
      break;
    case AccountFieldState::NoDevice:
      // signed in, the daemon is not attached — never "please login"
      text = T_("site_app_device_attaching", "Attaching device controls…");
      tone = kUrTextFaint;
      break;
    case AccountFieldState::Failed:
      text = T_("something_went_wrong", "Something went wrong.");
      tone = kUrDanger;
      break;
  }
  SetToned(line, tone, text);
}

// A pane A balance figure: the compact byte count once a snapshot has landed,
// its state before (AccountPlanRows.hpp), announced as "<key>, <value>".
void ApplyBalanceFigure(Gtk::Label& value, const Glib::ustring& key, int64_t byteCount,
                        bool snapshotLoaded, bool canCallApi) {
  switch (account_plan::FigureFor(snapshotLoaded, canCallApi)) {
    case account_plan::FigureView::Figure:
      SetToned(value, kOffWhite, FormatByteCountCompact(byteCount));
      break;
    case account_plan::FigureView::Loading:
      ApplyFieldState(value, AccountFieldState::Loading);
      break;
    case account_plan::FigureView::NoSession:
      ApplyFieldState(value, AccountFieldState::NoSession);
      break;
  }
  kit::SetAccessibleLabel(value, key + ", " + value.get_text());
}

// ---- ValidationState + supporting line (§0.5) -------------------------------
// Empty text STILL applies the colour, so clearing a line cannot flash the
// previous verdict's colour on the next write. Deliberately not
// kit::ApplySupportingText: that one swaps single CSS classes, which lose the
// cascade against `.ur-caption` (see SetToned above).
void ApplySupporting(Gtk::Label& line, const Glib::ustring& text,
                     kit::ValidationState state) {
  Rgba tone = kUrTextMuted;
  if (state == kit::ValidationState::Invalid) tone = kUrDanger;
  if (state == kit::ValidationState::Valid) tone = kUrGreen;
  SetToned(line, tone, text);
}

// ---- pane-mode row species the kit does not carry ---------------------------
// Prose in pane mode: 12px text that is ALLOWED to be taller than a row, still
// on the pane's grid — the 12px inset and the bottom hairline come from
// kit::MakePaneRow so it cannot drift from the rows above and below it.
struct ProseRow {
  Gtk::Widget* root = nullptr;
  Gtk::Label* line = nullptr;
};

ProseRow MakeProseRow(const Glib::ustring& text, int padY) {
  ProseRow out;
  auto* host = kit::MakePaneRow(0);  // height 0: the content sets the height
  out.line = Gtk::make_managed<Gtk::Label>(text);
  out.line->add_css_class("ur-caption");
  out.line->set_xalign(0);
  out.line->set_wrap(true);
  out.line->set_hexpand(true);
  out.line->set_margin_top(padY);
  out.line->set_margin_bottom(padY);
  if (auto* inner = dynamic_cast<Gtk::Box*>(host->get_first_child())) {
    inner->append(*out.line);
  }
  out.root = host;
  return out;
}

// A pane row whose height is its CONTENT (windows MinHeight=0 + Padding 12,N).
struct PaddedRow {
  Gtk::Box* root = nullptr;
  Gtk::Box* content = nullptr;
};

PaddedRow MakePaddedRow(int padY) {
  PaddedRow out;
  out.root = kit::MakePaneRow(0);
  out.content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  out.content->set_hexpand(true);
  out.content->set_margin_top(padY);
  out.content->set_margin_bottom(padY);
  if (auto* inner = dynamic_cast<Gtk::Box*>(out.root->get_first_child())) {
    inner->append(*out.content);
  }
  return out;
}

// The leading icon of an Account pane row (REVOKE-UI-FINAL.md §1.2: every row
// of the Account list has one, the Sessions row's face profile among them).
// Drawn from Material Design Icons path data (MdiGlyphs.hpp), never themed:
// a themed icon a theme lacks renders as a blank.
Gtk::Widget* RowIcon(MdiGlyph glyph) {
  return Gtk::make_managed<GlyphIcon>(glyph, kit::kRowIconPx);
}

// The two-line row with an action verb on the right. GTK carries no separate
// FullDescription channel, so the button announces as "verb. row label".
Gtk::Button* AddButtonRow(Gtk::Box& host, const Glib::ustring& label,
                          const Glib::ustring& note, const Glib::ustring& action,
                          MdiGlyph glyph) {
  auto row = kit::MakePaneTwoLineRow(label, note, kRowTall, RowIcon(glyph));
  auto* button = Gtk::make_managed<Gtk::Button>(action);
  button->set_valign(Gtk::Align::CENTER);
  kit::SetAccessibleLabel(*button, action + ". " + label);
  row.trailing->append(*button);
  host.append(*row.root);
  return button;
}

// The two-line row with a muted readout AND a copy button (§3.2.5, §3.3.2).
struct ValueActionRow {
  Gtk::Widget* root = nullptr;
  Gtk::Label* value = nullptr;
  Gtk::Button* action = nullptr;
};

ValueActionRow AddValueActionRow(Gtk::Box& host, const Glib::ustring& label,
                                 const Glib::ustring& action, MdiGlyph glyph) {
  ValueActionRow out;
  auto row = kit::MakePaneTwoLineRow(label, {}, kRowTall, RowIcon(glyph));
  out.value = Gtk::make_managed<Gtk::Label>();
  out.value->add_css_class("ur-value");  // family + size; the tone is a markup run
  out.value->set_xalign(1.f);
  out.value->set_valign(Gtk::Align::CENTER);
  out.value->set_ellipsize(Pango::EllipsizeMode::END);
  out.value->set_max_width_chars(kValueChars);
  row.trailing->append(*out.value);
  out.action = Gtk::make_managed<Gtk::Button>(action);
  out.action->set_valign(Gtk::Align::CENTER);
  kit::SetAccessibleLabel(*out.action, action + ". " + label);
  row.trailing->append(*out.action);
  out.root = row.root;
  host.append(*row.root);
  return out;
}

// §3.1.2 — the network-name row's trailing glyph is a PENCIL (windows U+E70F),
// not the chevron the kit draws on a row that navigates away: this row edits
// IN PLACE, and a row that looks exactly like "Referral network", "Manage
// Subscription", "Sign out" and "Delete account" teaches that it opens
// something else. The kit's row button takes no glyph parameter yet
// (TODO(kit): MakePaneTwoLineRowButton(glyph)), so the row is still BUILT by
// the kit — metrics, hairline, pressed feedback and accessible name all come
// from there — and only its trailing image is retargeted here. Hand-building
// the row would fork the row species for one icon.
void RetargetRowGlyph(Gtk::Button& row, const char* iconName) {
  auto* grid = dynamic_cast<Gtk::Box*>(row.get_child());
  if (grid == nullptr) return;
  if (auto* image = dynamic_cast<Gtk::Image*>(grid->get_last_child())) {
    image->set_from_icon_name(iconName);
  }
}

// A body-face line at an explicit size (sheet headings, values, warnings).
Gtk::Label* MakeSizedLabel(const Glib::ustring& text, int sizePx, const char* cssClass) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class(cssClass);
  label->set_xalign(0);
  label->set_wrap(true);
  Pango::AttrList attrs;
  auto size = Pango::Attribute::create_attr_size_absolute(sizePx * PANGO_SCALE);
  attrs.insert(size);
  label->set_attributes(attrs);
  return label;
}

// ---- text helpers -----------------------------------------------------------
// Go strings.TrimSpace parity — a box of spaces is not a name and not a code.
std::string TrimSpace(const std::string& text) {
  static const char* kWs = " \t\n\v\f\r";
  const auto begin = text.find_first_not_of(kWs);
  if (begin == std::string::npos) return {};
  const auto end = text.find_last_not_of(kWs);
  return text.substr(begin, end - begin + 1);
}

std::string Lower(const std::string& text) {
  std::string out = text;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// The server's auth type rendered as DATA with its first letter uppercased
// ("Email", "Google", "Solana"). Deliberately NOT localized: no per-provider
// store keys exist, by design.
std::string UpperFirst(const std::string& text) {
  if (text.empty()) return text;
  std::string out = text;
  out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}

// §4 — the masked balance-code secret: first 3 + "..." + last 3; a secret of
// six characters or fewer renders as that many dots (never a partial secret).
std::string MaskSecret(const std::string& secret) {
  if (secret.size() <= static_cast<size_t>(kMaskShoulder * 2)) {
    return std::string(secret.size(), '.');
  }
  return secret.substr(0, kMaskShoulder) + "..." +
         secret.substr(secret.size() - kMaskShoulder);
}

// The YYYY-MM-DD prefix of an ISO timestamp; empty when absent.
std::string DatePrefix(const std::optional<std::string>& timestamp) {
  if (!timestamp || timestamp->size() < static_cast<size_t>(kIsoDatePrefix)) return {};
  return timestamp->substr(0, kIsoDatePrefix);
}

// §3.2.4 — shoulder-surfer defense: a long auth code DISPLAYS abbreviated. The
// clipboard always gets the whole code (§7).
std::string AbbreviateCode(const std::string& code) {
  if (code.size() <= static_cast<size_t>(kAuthCodeAbbreviateAbove)) return code;
  return code.substr(0, kAuthCodeShoulder) + "..." +
         code.substr(code.size() - kAuthCodeShoulder);
}

// §3.1.1 — needsNameClaim: TRUE when the account carries NONE of the identity
// methods. Seedphrase deliberately does NOT count: a seedphrase-only account
// still claims its auto-generated name.
bool IsIdentityMethod(const std::string& method) {
  const std::string lower = Lower(method);
  return lower == "email" || lower == "phone" || lower == "google" ||
         lower == "apple" || lower == "solana";
}

bool ComputeNeedsNameClaim(const urnet::NetworkUser& user) {
  if (user.auth_types && !user.auth_types->empty()) {
    for (const auto& type : *user.auth_types) {
      if (IsIdentityMethod(type)) return false;
    }
    return true;
  }
  // the legacy single-auth_type shape is handled identically
  return !IsIdentityMethod(user.auth_type);
}

// §3.2.2 — the login methods: auth_types (skipping empties), else the single
// auth_type, else derived from user_auth ("email" when it contains @, else the
// raw value). Deduped, order preserved.
std::vector<std::string> ParseAuthMethods(const urnet::NetworkUser& user) {
  std::vector<std::string> methods;
  auto push = [&methods](const std::string& value) {
    if (value.empty()) return;
    for (const auto& existing : methods) {
      if (Lower(existing) == Lower(value)) return;
    }
    methods.push_back(value);
  };
  if (user.auth_types) {
    for (const auto& type : *user.auth_types) push(type);
  }
  if (methods.empty()) push(user.auth_type);
  if (methods.empty()) {
    const std::string auth = user.user_auth.value_or(std::string());
    if (!auth.empty()) {
      push(auth.find('@') != std::string::npos ? std::string("email") : auth);
    }
  }
  return methods;
}

// The server's own words first (they say WHAT was wrong), then the transport
// error, then nothing — the caller decides the fallback line.
std::string FirstMessage(const std::string& serverMessage,
                         const std::optional<std::string>& err) {
  if (!serverMessage.empty()) return serverMessage;
  if (err && !err->empty()) return *err;
  return {};
}

}  // namespace

// =============================================================================
// AccountFlow — the 20 s give-up every request on this destination carries
// =============================================================================

uint32_t AccountFlow::Begin(int timeoutMs, std::function<void()> onTimeout) {
  timer.disconnect();
  ++generation;
  const uint32_t armed = generation;
  timer = Glib::signal_timeout().connect(
      [this, armed, onTimeout = std::move(onTimeout)]() -> bool {
        if (generation != armed) return false;  // already settled or abandoned
        // Bump AGAIN so the give-up is FINAL: an answer that arrives after the
        // user has been told "something went wrong" must not quietly undo it.
        ++generation;
        g_warning("account: a request never answered — giving up on it");
        if (onTimeout) onTimeout();
        return false;  // one shot
      },
      timeoutMs);
  return armed;
}

bool AccountFlow::Settle(uint32_t armed, const char* what) {
  if (generation != armed) {
    g_message("account: dropping a result for an abandoned %s", what);
    return false;  // superseded or timed out: the caller must do NOTHING
  }
  timer.disconnect();
  return true;
}

void AccountFlow::Abandon() {
  timer.disconnect();
  ++generation;
}

// =============================================================================
// The usage bar (§2.2.2)
// =============================================================================
// Three star-weighted segments used -> pending -> available, every non-zero
// segment floored at 1.5% so a sliver still reads, all-zero rendering as one
// faint track (never an empty box that reads as "broken").
//
// TODO(kit): hoist the SEGMENT BAR out of urnw::UsageBar. That widget bundles
// the bar with a legend, a daily-balance row and a referral row in card
// typography; this destination renders those two figures as its own 34px pane
// rows, so reusing it whole would put a card inside a pane and duplicate both
// rows. Splitting it means editing a shared file, which this wave forbids.
class AccountUsageBar : public Gtk::Box {
 public:
  AccountUsageBar() : Gtk::Box(Gtk::Orientation::VERTICAL, 8) {
    bar_.set_content_height(kBarHeight);
    bar_.set_hexpand(true);
    kit::MarkDecorative(bar_);  // the legend beside it carries the words
    bar_.set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
      Draw(cr, w, h);
    });
    append(bar_);

    auto* legend = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    legend->append(*MakeKey(T_("used_data_key", "Used"), kUrElectricBlue));
    legend->append(*MakeKey(T_("pending_data_key", "Pending"), kUrCoral));
    legend->append(*MakeKey(T_("available_data_key", "Available"), kUrTextFaint));
    append(*legend);
  }

  void SetData(int64_t used, int64_t pending, int64_t available) {
    used_ = std::max<int64_t>(0, used);
    pending_ = std::max<int64_t>(0, pending);
    available_ = std::max<int64_t>(0, available);
    bar_.queue_draw();
  }

 private:
  static constexpr int kBarHeight = 32;
  static constexpr double kBarRadius = 12.0;
  static constexpr double kMinFraction = 0.015;  // mac minNonZeroValue
  // §2.2.2 "1px left margin between segments". The tree's other UsageBar
  // overshoots each segment by half a pixel to KILL the seam instead
  // (linux-reuse §2.19 calls that REUSE-AS-IS); the two docs disagree and
  // account.md is the source of truth for this destination. The margin is the
  // right call here anyway: without it two adjacent segments of similar weight
  // read as one block rather than as three measured quantities.
  static constexpr double kSegmentGap = 1.0;

  static Gtk::Box* MakeKey(const Glib::ustring& label, const Rgba& color) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* dot = Gtk::make_managed<Gtk::Label>();
    dot->set_markup("<span size='" + std::to_string(8 * PANGO_SCALE) + "' foreground='" +
                    HexForMarkup(color) + "'>●</span>");
    dot->set_valign(Gtk::Align::CENTER);
    kit::MarkDecorative(*dot);  // the colour restates the adjacent word
    box->append(*dot);
    auto* text = Gtk::make_managed<Gtk::Label>(label);
    text->add_css_class("ur-caption");
    box->append(*text);
    return box;
  }

  void Draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
    const double w = width;
    const double h = std::min<double>(height, kBarHeight);
    const double r = std::min(kBarRadius, h / 2.0);
    cr->begin_new_path();
    cr->arc(r, r, r, G_PI, 3 * G_PI / 2);
    cr->arc(w - r, r, r, 3 * G_PI / 2, 2 * G_PI);
    cr->arc(w - r, h - r, r, 0, G_PI / 2);
    cr->arc(r, h - r, r, G_PI / 2, G_PI);
    cr->close_path();
    cr->clip();

    const double total = static_cast<double>(used_ + pending_ + available_);
    if (total <= 0) {
      cr->set_source_rgba(kUrTextFaint.r, kUrTextFaint.g, kUrTextFaint.b, 0.35);
      cr->paint();
      return;
    }
    double fractions[3] = {used_ / total, pending_ / total, available_ / total};
    const Rgba colors[3] = {kUrElectricBlue, kUrCoral, kUrTextFaint};
    double sum = 0;
    for (double& f : fractions) {
      if (f > 0 && f < kMinFraction) f = kMinFraction;
      sum += f;
    }
    // The LAST drawn segment runs to the right edge: the fractions renormalize
    // to 1, but accumulated rounding would otherwise leave a hairline of bare
    // track outside the capsule's last fill.
    int last = -1;
    for (int i = 0; i < 3; ++i) {
      if (fractions[i] > 0) last = i;
    }
    double x = 0;
    for (int i = 0; i < 3; ++i) {
      const double segment = w * (fractions[i] / sum);
      if (segment <= 0) continue;
      // 1px left margin between segments (never before the first, which starts
      // flush inside the radius-12 clip).
      const double left = x > 0 ? x + kSegmentGap : x;
      const double right = i == last ? w : x + segment;
      x += segment;
      if (right <= left) continue;  // a floored sliver the gap has eaten
      cr->set_source_rgba(colors[i].r, colors[i].g, colors[i].b, colors[i].a);
      cr->rectangle(left, 0, right - left, h);
      cr->fill();
    }
  }

  Gtk::DrawingArea bar_;
  int64_t used_ = 0;
  int64_t pending_ = 0;
  int64_t available_ = 0;
};

// =============================================================================
// §3.2.3 AddAuthSheet — add a login method
// =============================================================================
// EVERY sheet on this destination takes the PAGE's gate (CanCallApi) rather
// than re-deriving host_.IsLoggedIn(): the page's gate puts previewMode_
// first, and a preview build that reached production authenticated is exactly
// how a review harness mints a real auth code, adds a real login method,
// unlinks a real referral network or destroys a real network from a screen
// that says "Please login to URnetwork". EarningsPage hands its sheet the same
// gate for the same reason.
//
// An email or phone counts as added only once it is verified (GuestConversion,
// the flow the guest conversion uses too): addAuth -> send a code ->
// authVerify. The page reloads the methods only after the code is accepted;
// closing on the code page leaves an unverified sign-in that the next password
// sign-in verifies. The jwt authVerify returns is not installed (the session
// already holds this network's).
//
// Apple, Google and wallets (AddSignInFlow.hpp) run the login's own browser
// and wallet flows through SdkHost::AddSignIn*, which end in add-auth on this
// session: never a sign-in, never a new jwt. They have no code step.
class AccountAddAuthSheet : public Gtk::Window {
  // The SDK side of the flow, gated by the page and guarded by the sheet's
  // epoch; every answer is posted to the main loop.
  class Session : public GuestConversionSession {
   public:
    explicit Session(AccountAddAuthSheet& sheet) : sheet_(sheet) {}

    void AddSignIn(const std::string& userAuth, const std::string& password,
                   std::function<void(std::string error)> done) override {
      AccountAddAuthSheet& sheet = sheet_;
      if (!sheet.CanAct()) {
        done(T_("please_login_to_urnetwork", "Please login to URnetwork"));
        return;
      }
      urnet::AddAuthArgs args{};
      args.user_auth = userAuth;
      args.password = password;

      auto epoch = sheet.epoch_;
      const uint64_t seen = *sheet.epoch_;
      // 20 s: an answer that never comes must not leave Add greyed out forever
      // with no line of explanation under it.
      const uint32_t flow = sheet.flow_.Begin(kApiTimeoutMs, [done] {
        done(T_("something_went_wrong", "Something went wrong."));
      });
      sheet.host_.api().addAuth(
          std::optional<urnet::AddAuthArgs>(args),
          [&sheet, epoch, seen, flow, done](std::optional<urnet::AddAuthResult> result,
                                            std::optional<std::string> err) {
            PostToMain([&sheet, epoch, seen, flow, done, result = std::move(result),
                        err = std::move(err)] {
              if (*epoch != seen) return;  // a newer Open() (or teardown) owns it
              if (!sheet.flow_.Settle(flow, "add login method")) return;
              const bool ok = result.has_value() && !err.has_value() && !result->error;
              if (ok) {
                done(std::string());
                return;
              }
              std::string message = FirstMessage(
                  result && result->error ? result->error->message : std::string(), err);
              g_warning("account: addAuth failed: %s",
                        message.empty() ? "(no error text)" : message.c_str());
              if (message.empty()) message = T_("something_went_wrong", "Something went wrong.");
              done(message);
            });
          });
    }

    // this network was never a guest: nothing to lift
    void RefreshJwt() override {}
    void RefreshBalance() override {}

    void SendCode(const std::string& userAuth,
                  std::function<void(VerifySendNotice notice)> done) override {
      auto epoch = sheet_.epoch_;
      const uint64_t seen = *epoch;
      sheet_.host_.ResendVerifyCode(userAuth, [epoch, seen, done](VerifySendNotice notice) {
        PostToMain([epoch, seen, done, notice] {
          if (*epoch == seen) done(notice);
        });
      });
    }

    // authVerify without SdkHost::VerifyCode's sign-in: the returned jwt is
    // for this same network, and the session already holds one.
    void VerifyCode(const std::string& userAuth, const std::string& code,
                    std::function<void(std::string error)> done) override {
      urnet::AuthVerifyArgs args{};
      args.user_auth = userAuth;
      args.verify_code = code;
      auto epoch = sheet_.epoch_;
      const uint64_t seen = *epoch;
      sheet_.host_.api().authVerify(
          std::optional<urnet::AuthVerifyArgs>(args),
          [epoch, seen, done](std::optional<urnet::AuthVerifyResult> result,
                              std::optional<std::string> err) {
            std::string error;
            if (err || !result || result->error) {
              error = FirstMessage(
                  result && result->error ? result->error->message : std::string(), err);
              if (error.empty()) error = T_("something_went_wrong", "Something went wrong.");
            }
            PostToMain([epoch, seen, done, error] {
              if (*epoch == seen) done(error);
            });
          });
    }

   private:
    AccountAddAuthSheet& sheet_;
  };

 public:
  AccountAddAuthSheet(Gtk::Window& parent, SdkHost& host, std::function<bool()> canAct)
      : host_(host), canAct_(std::move(canAct)), session_(*this), conversion_(session_) {
    set_transient_for(parent);
    set_modal(true);
    set_title(T_("site_app_login_methods", "Login methods"));
    set_default_size(kSheetWidthNarrow, -1);
    set_resizable(false);
    set_hide_on_close(true);
    add_css_class("ur-sheet");  // sheets sit ABOVE the page: #151515
    AddEscapeToClose(*this);

    // ---- page 1: the sign-in to add ----
    // The same options as every app (AddSignInFlow.hpp): Apple, Google, a
    // Solana or Bittensor wallet, and an email or phone.
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);
    box->set_size_request(kSheetWidthNarrow, -1);

    auto* heading =
        Gtk::make_managed<Gtk::Label>(T_("add_a_sign_in_method", "Add a sign-in method"));
    heading->add_css_class("ur-step-heading");
    heading->set_xalign(0);
    box->append(*heading);

    auto* methods = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    methods->add_css_class("linked");
    kit::SetAccessibleLabel(*methods, T_("method", "Method"));
    Gtk::ToggleButton* group = nullptr;
    for (const addsignin::Method method : addsignin::Methods()) {
      auto* toggle = Gtk::make_managed<Gtk::ToggleButton>(Tr(addsignin::MethodLabel(method)));
      toggle->set_hexpand(true);
      if (group) toggle->set_group(*group);
      else group = toggle;
      toggle->signal_toggled().connect([this, toggle, method] {
        if (!toggle->get_active()) return;
        method_ = method;
        providerError_.clear();
        Render();
      });
      methods->append(*toggle);
      methodButtons_.emplace_back(method, toggle);
    }
    box->append(*methods);

    // apple and google: the provider's web flow in the browser
    auto providerPage = [this](addsignin::Method method, Glib::ustring hint,
                               Glib::ustring button) {
      auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
      auto* note = Gtk::make_managed<Gtk::Label>(hint);
      note->add_css_class("ur-caption");
      note->set_xalign(0);
      note->set_wrap(true);
      page->append(*note);
      auto* start = Gtk::make_managed<Gtk::Button>(button);
      start->add_css_class("ur-btn");
      start->add_css_class("ur-btn-primary");
      start->signal_clicked().connect([this, method] { StartProvider(method); });
      page->append(*start);
      providerButtons_.push_back(start);
      return page;
    };
    methodStack_.add(*providerPage(addsignin::Method::Apple,
                                   T_("sign_in_with_your_apple_id_to",
                                      "Sign in with your Apple ID to add it as a sign-in method."),
                                   T_("sign_in_with_apple", "Sign in with Apple")),
                     "apple");
    methodStack_.add(*providerPage(addsignin::Method::Google,
                                   T_("sign_in_with_google_to_add_it",
                                      "Sign in with Google to add it as a sign-in method."),
                                   T_("sign_in_with_google", "Sign in with Google")),
                     "google");

    // wallet: a Solana wallet (Phantom / Solflare on the ur.io bridge) or a
    // Bittensor wallet (the shared chooser: the SDK's wallets for linux)
    auto* walletPage = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    for (const addsignin::WalletChain chain : addsignin::WalletChains()) {
      auto* note = Gtk::make_managed<Gtk::Label>(Tr(addsignin::ChainHint(chain)));
      note->add_css_class("ur-caption");
      note->set_xalign(0);
      note->set_wrap(true);
      walletPage->append(*note);
      auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
      auto* label = Gtk::make_managed<Gtk::Label>(Tr(addsignin::ChainLabel(chain)));
      label->add_css_class("ur-input-label");
      label->set_xalign(0);
      walletPage->append(*label);
      if (chain == addsignin::WalletChain::Solana) {
        // wallet names are product names: never translated
        for (const auto& [name, provider] :
             {std::pair<const char*, WalletConnect::Provider>{"Phantom",
                                                              WalletConnect::Provider::Phantom},
              std::pair<const char*, WalletConnect::Provider>{"Solflare",
                                                              WalletConnect::Provider::Solflare}}) {
          auto* wallet = Gtk::make_managed<Gtk::Button>(name);
          wallet->signal_clicked().connect([this, provider = provider] { StartSolana(provider); });
          row->append(*wallet);
          providerButtons_.push_back(wallet);
        }
      } else {
        // the shared chooser (Talisman, TAO.com and others, WalletConnect)
        auto* choose = Gtk::make_managed<Gtk::Button>(
            T_("bittensor_choose_wallet", "Choose your Bittensor wallet"));
        choose->signal_clicked().connect([this] { ChooseBittensorWallet(); });
        row->append(*choose);
        providerButtons_.push_back(choose);
      }
      walletPage->append(*row);
    }
    methodStack_.add(*walletPage, "wallet");

    // email or phone + password, then a code
    auto* emailPage = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    auto* authLabel = Gtk::make_managed<Gtk::Label>(T_("your_email", "Your email"));
    authLabel->add_css_class("ur-input-label");
    authLabel->set_xalign(0);
    emailPage->append(*authLabel);
    auth_ = Gtk::make_managed<Gtk::Entry>();
    auth_->add_css_class("ur-input");
    kit::SetAccessibleLabel(*auth_, T_("your_email", "Your email"));
    auth_->signal_changed().connect([this] { Render(); });
    emailPage->append(*auth_);

    auto* passwordLabel = Gtk::make_managed<Gtk::Label>(T_("password_label", "Password"));
    passwordLabel->add_css_class("ur-input-label");
    passwordLabel->set_xalign(0);
    emailPage->append(*passwordLabel);
    password_ = Gtk::make_managed<Gtk::PasswordEntry>();
    password_->add_css_class("ur-input");
    password_->set_show_peek_icon(true);
    kit::SetAccessibleLabel(*password_, T_("password_label", "Password"));
    password_->signal_changed().connect([this] { Render(); });
    emailPage->append(*password_);

    auto* rule = Gtk::make_managed<Gtk::Label>(
        T_("password_must_be_at_least_12_characters_long",
           "Password must be at least 12 characters long"));
    rule->add_css_class("ur-caption");
    rule->set_xalign(0);
    rule->set_wrap(true);
    emailPage->append(*rule);
    methodStack_.add(*emailPage, "email");
    methodStack_.set_vhomogeneous(false);
    box->append(methodStack_);

    error_ = MakeSizedLabel({}, 12, "ur-caption");
    error_->set_visible(false);
    error_->set_wrap(true);
    box->append(*error_);

    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    actions->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
    cancel->signal_clicked().connect([this] { set_visible(false); });
    actions->append(*cancel);
    add_ = Gtk::make_managed<Gtk::Button>(T_("add", "Add"));
    add_->add_css_class("suggested-action");  // the dialog-primary role
    add_->set_sensitive(false);
    add_->signal_clicked().connect([this] {
      conversion_.SubmitSignIn(auth_->get_text().raw(), password_->get_text().raw());
    });
    actions->append(*add_);
    box->append(*actions);
    stack_.add(*box, "sign-in");

    // ---- page 2: verify the added sign-in ----
    auto* verifyPage = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    verifyPage->set_margin(24);
    verifyPage->set_size_request(kSheetWidthNarrow, -1);
    auto* verifyHeading =
        Gtk::make_managed<Gtk::Label>(T_("login_verify_header", "You've got mail"));
    verifyHeading->add_css_class("ur-step-heading");
    verifyHeading->set_xalign(0);
    verifyPage->append(*verifyHeading);
    auto* explanation = Gtk::make_managed<Gtk::Label>(
        T_("verify_explanation",
           "Tell us who you really are. Enter the code we sent you to verify your identity."));
    explanation->add_css_class("dim-label");
    explanation->set_xalign(0);
    explanation->set_wrap(true);
    verifyPage->append(*explanation);
    code_ = Gtk::make_managed<Gtk::Entry>();
    code_->add_css_class("ur-otp");
    code_->set_placeholder_text(T_("verify_input_label", "Verification code"));
    kit::SetAccessibleLabel(*code_, T_("verify_input_label", "Verification code"));
    code_->signal_changed().connect([this] { Render(); });
    code_->signal_activate().connect([this] { conversion_.SubmitCode(code_->get_text().raw()); });
    verifyPage->append(*code_);
    codeError_ = MakeSizedLabel({}, 12, "ur-caption");
    codeError_->set_visible(false);
    verifyPage->append(*codeError_);
    notice_ = MakeSizedLabel({}, 12, "ur-caption");
    notice_->set_wrap(true);
    verifyPage->append(*notice_);
    auto* verifyActions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    verifyActions->set_halign(Gtk::Align::END);
    resend_ = Gtk::make_managed<Gtk::Button>(T_("resend_verify_code", "Resend code"));
    resend_->add_css_class("flat");
    resend_->signal_clicked().connect([this] { conversion_.Resend(); });
    verifyActions->append(*resend_);
    verify_ = Gtk::make_managed<Gtk::Button>(T_("verify", "Verify"));
    verify_->add_css_class("suggested-action");
    verify_->signal_clicked().connect([this] { conversion_.SubmitCode(code_->get_text().raw()); });
    verifyActions->append(*verify_);
    verifyPage->append(*verifyActions);
    stack_.add(*verifyPage, "verify");

    // ---- page 3: an Apple, Google or wallet sign-in was added ----
    auto* addedPage = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    addedPage->set_margin(24);
    addedPage->set_size_request(kSheetWidthNarrow, -1);
    added_ = Gtk::make_managed<Gtk::Label>();
    added_->add_css_class("ur-step-heading");
    added_->set_xalign(0);
    added_->set_wrap(true);
    addedPage->append(*added_);
    auto* done = Gtk::make_managed<Gtk::Button>(T_("done", "Done"));
    done->add_css_class("suggested-action");
    done->set_halign(Gtk::Align::END);
    done->signal_clicked().connect([this] { set_visible(false); });
    addedPage->append(*done);
    stack_.add(*addedPage, "added");

    set_child(stack_);
    // DefaultButton = Primary here (spec): this sheet ADDS, it never destroys.
    add_->set_receives_default(true);
    set_default_widget(*add_);
    conversion_.on_changed = [this] { Render(); };
    // closing drops a browser or wallet add still waiting: its late return is
    // ignored (SdkHost::CancelAddSignIn)
    signal_hide().connect([this] {
      if (!providerBusy_) return;
      providerBusy_ = false;
      ++*epoch_;
      host_.CancelAddSignIn();
    });
  }

  ~AccountAddAuthSheet() override {
    ++*epoch_;  // orphan every in-flight call
    if (providerBusy_) host_.CancelAddSignIn();
    cooldownTick_.disconnect();
    conversion_.on_changed = nullptr;
  }

  // The page reloads the network user so the new, verified method appears.
  std::function<void()> on_changed;

  void Open() {
    ++*epoch_;
    flow_.Abandon();  // a previous submit's watchdog may still be armed
    cooldownTick_.disconnect();
    conversion_.Reset();
    if (providerBusy_) host_.CancelAddSignIn();
    providerBusy_ = false;
    providerError_.clear();
    addedMethod_.reset();
    SelectMethod(addsignin::kDefaultMethod);
    auth_->set_text("");
    password_->set_text("");
    code_->set_text("");
    Render();
    present();
  }

 private:
  bool CanAct() const { return canAct_ && canAct_(); }

  static Glib::ustring Tr(addsignin::Text text) {
    return g_dpgettext2(GETTEXT_PACKAGE, std::string(text.key).c_str(),
                        std::string(text.english).c_str());
  }

  static const char* MethodPage(addsignin::Method method) {
    switch (method) {
      case addsignin::Method::Apple: return "apple";
      case addsignin::Method::Google: return "google";
      case addsignin::Method::Wallet: return "wallet";
      case addsignin::Method::Email: return "email";
    }
    return "email";
  }

  void SelectMethod(addsignin::Method method) {
    method_ = method;
    for (auto& [m, toggle] : methodButtons_) {
      if (m == method && !toggle->get_active()) toggle->set_active(true);
    }
  }

  // The answer of an Apple, Google or wallet add, on whatever thread it
  // arrives: posted to the main loop and dropped once the sheet moved on.
  // `bittensorWalletId` is the Bittensor wallet that signed ("" for none).
  std::function<void(SdkHost::AddSignInResult)> Answer(addsignin::Method method,
                                                       std::string bittensorWalletId = {}) {
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    return [this, epoch, seen, method, bittensorWalletId](SdkHost::AddSignInResult result) {
      PostToMain([this, epoch, seen, method, bittensorWalletId, result] {
        if (*epoch != seen) return;
        providerBusy_ = false;
        if (result.ok) {
          addedMethod_ = method;
          Render();
          if (on_changed) on_changed();  // the new method appears
          return;
        }
        // the user closed the manual sheet, or started another method
        if (bittensor::IsCancelled(result.error) || bridge::IsSuperseded(result.error)) {
          providerError_.clear();
        } else if (const std::string words =
                       WalletProofRefusalText(result.code, result.error, bittensorWalletId);
                   words != result.error) {
          // a pasted signature from another account than the entered address:
          // the user's to fix in the wallet, not a failure to log
          providerError_ = words;
        } else {
          g_warning("account: add sign-in failed: %s",
                    result.error.empty() ? "(no error text)" : result.error.c_str());
          providerError_ = result.error.empty()
                               ? std::string(T_("something_went_wrong", "Something went wrong."))
                               : result.error;
        }
        Render();
      });
    };
  }

  bool BeginProvider() {
    if (providerBusy_) return false;
    if (!CanAct()) {
      Render();
      return false;
    }
    providerBusy_ = true;
    providerError_.clear();
    Render();
    return true;
  }

  void StartProvider(addsignin::Method method) {
    if (!BeginProvider()) return;
    host_.AddSignInWithSso(std::string(addsignin::SsoProvider(method)), Answer(method));
  }

  void StartSolana(WalletConnect::Provider provider) {
    if (!BeginProvider()) return;
    host_.AddSignInWithSolana(provider, Answer(addsignin::Method::Wallet));
  }

  void ChooseBittensorWallet() {
    if (providerBusy_) return;
    GtkWidget* dialog = NewBittensorWalletChooser(GTK_WINDOW(gobj()));
    g_signal_connect(dialog, "response",
                     G_CALLBACK(+[](AdwMessageDialog*, const char* response, gpointer data) {
                       auto* self = static_cast<AccountAddAuthSheet*>(data);
                       const std::string_view walletId =
                           bittensor::ChosenWallet(response ? response : "");
                       if (!walletId.empty()) self->StartBittensor(std::string(walletId));
                     }),
                     this);
    gtk_window_present(GTK_WINDOW(dialog));
  }

  void StartBittensor(const std::string& walletId) {
    if (!BeginProvider()) return;
    host_.AddSignInWithBittensor(walletId, Answer(addsignin::Method::Wallet, walletId));
  }

  void Render() {
    if (addedMethod_) {
      added_->set_text(Tr(addsignin::AddedMessage(*addedMethod_)));
      stack_.set_visible_child("added");
      return;
    }
    const GuestConversionStep step = conversion_.Step();
    const bool busy = conversion_.Busy() || providerBusy_;
    switch (step) {
      case GuestConversionStep::EnterSignIn:
      case GuestConversionStep::AddingSignIn: {
        stack_.set_visible_child("sign-in");
        methodStack_.set_visible_child(MethodPage(method_));
        const bool email = method_ == addsignin::Method::Email;
        add_->set_visible(email);
        const bool session = CanAct();
        for (auto& [m, toggle] : methodButtons_) toggle->set_sensitive(!busy);
        for (Gtk::Button* button : providerButtons_) button->set_sensitive(session && !busy);
        auth_->set_sensitive(session && !busy);
        password_->set_sensitive(session && !busy);
        if (!session) {
          // never a disabled field with no explanation
          ApplyFieldState(*error_, AccountFieldState::NoSession);
          error_->set_visible(true);
        } else if (!email) {
          if (providerError_.empty()) {
            error_->set_visible(false);
          } else {
            SetToned(*error_, kUrDanger, providerError_);
            error_->set_visible(true);
          }
        } else if (!conversion_.Error().empty()) {
          SetToned(*error_, kUrDanger, conversion_.Error());
          error_->set_visible(true);
        } else {
          error_->set_visible(false);
        }
        add_->set_sensitive(!busy && session &&
                            GuestConversion::CanSubmitSignIn(auth_->get_text().raw(),
                                                             password_->get_text().raw()));
        return;
      }
      case GuestConversionStep::EnterCode:
      case GuestConversionStep::Verifying:
        stack_.set_visible_child("verify");
        if (conversion_.Error().empty()) {
          codeError_->set_visible(false);
        } else {
          SetToned(*codeError_, kUrDanger, conversion_.Error());
          codeError_->set_visible(true);
        }
        ShowVerifySendNotice(*notice_, conversion_.ShownNotice());
        TickCooldown(cooldownTick_, conversion_, [this] { Render(); });
        code_->set_sensitive(!busy);
        resend_->set_sensitive(conversion_.CanResend());
        verify_->set_sensitive(!busy && !GuestConversion::Trim(code_->get_text().raw()).empty());
        return;
      case GuestConversionStep::Done:
        // added only now that the code was accepted
        cooldownTick_.disconnect();
        set_visible(false);
        if (on_changed) on_changed();
        return;
    }
  }

  SdkHost& host_;
  std::function<bool()> canAct_;  // the PAGE's gate: preview first, then session
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);
  AccountFlow flow_;
  Session session_;
  GuestConversion conversion_;
  sigc::connection cooldownTick_;
  Gtk::Stack stack_;
  // the sign-in page's method (AddSignInFlow.hpp) and its pages
  addsignin::Method method_ = addsignin::kDefaultMethod;
  std::vector<std::pair<addsignin::Method, Gtk::ToggleButton*>> methodButtons_;
  Gtk::Stack methodStack_;
  std::vector<Gtk::Button*> providerButtons_;
  // an Apple, Google or wallet add in flight (the browser or the wallet has it)
  bool providerBusy_ = false;
  std::string providerError_;
  // set once an Apple, Google or wallet sign-in was added
  std::optional<addsignin::Method> addedMethod_;
  Gtk::Label* added_ = nullptr;
  Gtk::Entry* auth_ = nullptr;
  Gtk::PasswordEntry* password_ = nullptr;
  Gtk::Label* error_ = nullptr;
  Gtk::Button* add_ = nullptr;
  Gtk::Entry* code_ = nullptr;
  Gtk::Label* codeError_ = nullptr;
  Gtk::Label* notice_ = nullptr;
  Gtk::Button* resend_ = nullptr;
  Gtk::Button* verify_ = nullptr;
};

// =============================================================================
// §3.2.4 AuthCodeSheet — create a 5-minute auth code
// =============================================================================
class AccountAuthCodeSheet : public Gtk::Window {
 public:
  AccountAuthCodeSheet(Gtk::Window& parent, SdkHost& host, std::function<bool()> canAct)
      : host_(host), canAct_(std::move(canAct)) {
    set_transient_for(parent);
    set_modal(true);
    set_title(T_("auth_code_create", "Auth Code Create"));
    set_default_size(kSheetWidthNarrow, -1);
    set_resizable(false);
    set_hide_on_close(true);
    add_css_class("ur-sheet");
    AddEscapeToClose(*this);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);
    box->set_size_request(kSheetWidthNarrow, -1);

    auto* heading =
        Gtk::make_managed<Gtk::Label>(T_("auth_code_create", "Auth Code Create"));
    heading->add_css_class("ur-step-heading");
    heading->set_xalign(0);
    box->append(*heading);

    auto* note = Gtk::make_managed<Gtk::Label>(
        T_("created_auth_codes_expire_after_5_minutes",
           "Created auth codes expire after 5 minutes"));
    note->add_css_class("ur-caption");
    note->set_xalign(0);
    note->set_wrap(true);
    box->append(*note);

    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    create_ = Gtk::make_managed<Gtk::Button>(
        T_("site_app_create_auth_code", "Create auth code"));
    // the URButton PRIMARY role (blue fill, bit face) the spec asks for
    create_->add_css_class("ur-btn");
    create_->add_css_class("ur-btn-primary");
    create_->signal_clicked().connect([this] { Create(); });
    actions->append(*create_);
    spinner_ = Gtk::make_managed<Gtk::Spinner>();
    spinner_->set_size_request(16, 16);
    spinner_->set_valign(Gtk::Align::CENTER);
    spinner_->set_visible(false);
    actions->append(*spinner_);
    box->append(*actions);

    codePanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    codePanel_->set_visible(false);
    codeText_ = MakeSizedLabel({}, 14, "ur-mono-13");
    codeText_->set_selectable(true);
    codeText_->set_wrap_mode(Pango::WrapMode::CHAR);
    codePanel_->append(*codeText_);
    copy_ = Gtk::make_managed<Gtk::Button>(T_("copy_auth_code", "Copy Auth Code"));
    copy_->set_halign(Gtk::Align::START);
    copy_->signal_clicked().connect([this] {
      // the WHOLE code, never the abbreviation (§7)
      get_clipboard()->set_text(code_);
      SetToned(*status_, kUrTextMuted, T_("site_app_copied", "Copied"));
      status_->set_visible(true);
    });
    codePanel_->append(*copy_);
    box->append(*codePanel_);

    status_ = MakeSizedLabel({}, 12, "ur-caption");
    status_->set_visible(false);
    box->append(*status_);

    auto* close = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
    close->set_halign(Gtk::Align::END);
    close->signal_clicked().connect([this] { set_visible(false); });
    box->append(*close);

    set_child(*box);
  }

  ~AccountAuthCodeSheet() override { ++*epoch_; }

  void Open() {
    ++*epoch_;
    flow_.Abandon();
    creating_ = false;
    code_.clear();
    codePanel_->set_visible(false);
    spinner_->stop();
    spinner_->set_visible(false);
    status_->set_visible(false);
    const bool session = CanAct();
    create_->set_sensitive(session);
    if (!session) {
      ApplyFieldState(*status_, AccountFieldState::NoSession);
      status_->set_visible(true);
    }
    present();
  }

 private:
  bool CanAct() const { return canAct_ && canAct_(); }

  void Fail(const Glib::ustring& message) {
    SetToned(*status_, kUrDanger, message);
    status_->set_visible(true);
  }

  void Create() {
    if (creating_ || !CanAct()) return;
    creating_ = true;
    create_->set_sensitive(false);
    status_->set_visible(false);
    spinner_->set_visible(true);
    spinner_->start();

    urnet::AuthCodeCreateArgs args{};
    args.duration_minutes = kAuthCodeMinutes;  // 5 matches the caption
    args.uses = kAuthCodeUses;

    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    // 20 s: a ring that spins forever over a dead Create button is the hang
    // §0.4's terminal states exist to prevent.
    const uint32_t flow = flow_.Begin(kApiTimeoutMs, [this] {
      creating_ = false;
      create_->set_sensitive(CanAct());
      spinner_->stop();
      spinner_->set_visible(false);
      Fail(T_("auth_code_error",
              "Error authenticating code. Please try again or generate a new code."));
    });
    host_.api().authCodeCreate(
        std::optional<urnet::AuthCodeCreateArgs>(args),
        [this, epoch, seen, flow](std::optional<urnet::AuthCodeCreateResult> result,
                                  std::optional<std::string> err) {
          PostToMain([this, epoch, seen, flow, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (!flow_.Settle(flow, "auth code create")) return;
            creating_ = false;
            create_->set_sensitive(CanAct());
            spinner_->stop();
            spinner_->set_visible(false);

            // (a) the limit is its OWN case: it is not a failure the user can
            //     retry away, it is a wait.
            if (result && result->error &&
                result->error->auth_code_limit_exceeded.value_or(false)) {
              Fail(T_("site_app_auth_code_limit",
                      "Auth code limit reached. Try again later."));
              return;
            }
            const std::string message = FirstMessage(
                result && result->error ? result->error->message.value_or(std::string())
                                        : std::string(),
                err);
            const std::string code =
                result ? result->auth_code.value_or(std::string()) : std::string();
            // (b) any other error, or a success with no code, is a failure
            if (!result || err || result->error || code.empty()) {
              g_warning("account: authCodeCreate failed: %s",
                        message.empty() ? "(no code returned)" : message.c_str());
              Fail(message.empty()
                       ? Glib::ustring(T_("auth_code_error",
                                          "Error authenticating code. Please try again or "
                                          "generate a new code."))
                       : Glib::ustring(message));
              return;
            }
            // (c) success
            code_ = code;
            codeText_->set_text(AbbreviateCode(code_));
            kit::SetAccessibleLabel(*codeText_, T_("auth_code", "Auth code"));
            codePanel_->set_visible(true);
            SetToned(*status_, kUrTextMuted, T_("auth_code_created", "Auth code created"));
            status_->set_visible(true);
          });
        });
  }

  SdkHost& host_;
  std::function<bool()> canAct_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);
  AccountFlow flow_;
  Gtk::Button* create_ = nullptr;
  Gtk::Spinner* spinner_ = nullptr;
  Gtk::Box* codePanel_ = nullptr;
  Gtk::Label* codeText_ = nullptr;
  Gtk::Button* copy_ = nullptr;
  Gtk::Label* status_ = nullptr;
  std::string code_;  // the FULL code; the display is abbreviated
  bool creating_ = false;
};

// =============================================================================
// §3.4 DeleteAccountSheet — typed confirmation
// =============================================================================
// FRESHNESS RULE: the sheet takes NO cached name. Api::networkDelete takes no
// arguments and acts on the CURRENT jwt, so a cached name that survived a
// sign-out could gate the destruction of the WRONG network. The gate fails
// CLOSED: before the read lands, and on any failure, the primary stays off.
class AccountDeleteSheet : public Gtk::Window {
 public:
  AccountDeleteSheet(Gtk::Window& parent, SdkHost& host, std::function<bool()> canAct)
      : host_(host), canAct_(std::move(canAct)) {
    set_transient_for(parent);
    set_modal(true);
    set_title(T_("are_you_sure_delete_account",
                 "Are you sure you want to delete your account?"));
    set_default_size(kSheetWidth, -1);
    set_resizable(false);
    set_hide_on_close(true);
    add_css_class("ur-sheet");
    AddEscapeToClose(*this);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);
    box->set_size_request(kSheetWidth, -1);

    auto* heading = Gtk::make_managed<Gtk::Label>(
        T_("are_you_sure_delete_account", "Are you sure you want to delete your account?"));
    heading->add_css_class("ur-step-heading");
    heading->set_xalign(0);
    heading->set_wrap(true);
    box->append(*heading);

    auto* warning = MakeSizedLabel(
        T_("site_app_delete_warning",
           "This permanently deletes your network, its data, and its earnings. This cannot "
           "be undone."),
        13, "ur-body");
    SetToned(*warning, kUrDanger,
             T_("site_app_delete_warning",
                "This permanently deletes your network, its data, and its earnings. This "
                "cannot be undone."));
    box->append(*warning);

    // WHICH network goes, on its own line, carrying its own FieldState.
    nameLine_ = MakeSizedLabel({}, 14, "ur-value");
    box->append(*nameLine_);

    auto* confirmLabel = Gtk::make_managed<Gtk::Label>(
        T_("site_app_delete_confirm", "Type your network name to confirm"));
    confirmLabel->add_css_class("ur-input-label");
    confirmLabel->set_xalign(0);
    box->append(*confirmLabel);
    confirm_ = Gtk::make_managed<Gtk::Entry>();
    confirm_->add_css_class("ur-input");
    confirm_->set_sensitive(false);  // until a fresh name lands
    kit::SetAccessibleLabel(*confirm_,
                            T_("site_app_delete_confirm", "Type your network name to confirm"));
    confirm_->signal_changed().connect([this] { Gate(); });
    box->append(*confirm_);

    error_ = MakeSizedLabel({}, 12, "ur-caption");
    error_->set_visible(false);
    box->append(*error_);

    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    actions->set_halign(Gtk::Align::END);
    cancel_ = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
    cancel_->signal_clicked().connect([this] { set_visible(false); });
    actions->append(*cancel_);
    deleteButton_ = Gtk::make_managed<Gtk::Button>(T_("delete_account_2", "Delete account"));
    deleteButton_->add_css_class("destructive-action");  // red IS the confirmation context
    deleteButton_->set_sensitive(false);
    deleteButton_->signal_clicked().connect([this] { Submit(); });
    actions->append(*deleteButton_);
    box->append(*actions);

    set_child(*box);
    // DefaultButton = Cancel: Enter can never destroy a network.
    cancel_->set_receives_default(true);
    set_default_widget(*cancel_);
  }

  ~AccountDeleteSheet() override { ++*epoch_; }

  // The page signs out the sign-in the deletion was made in (`signIn`, read as
  // the request went out): the session is meaningless once the network is
  // gone, and a sign-in made since belongs to another network.
  std::function<void(const auth_logout::Report& signIn)> on_deleted;

  void Open() {
    ++*epoch_;
    nameFlow_.Abandon();
    deleteFlow_.Abandon();
    deleting_ = false;
    name_.clear();
    confirm_->set_text("");
    confirm_->set_sensitive(false);
    error_->set_visible(false);
    Gate();
    ReadName();
    present();
  }

 private:
  bool CanAct() const { return canAct_ && canAct_(); }

  void ApplyName(AccountFieldState state, const std::string& name) {
    name_ = state == AccountFieldState::Loaded ? name : std::string();
    ApplyFieldState(*nameLine_, state, name);
    confirm_->set_sensitive(!name_.empty());
    // The placeholder is the FRESHLY read name and nothing else — cleared on
    // every non-Loaded state. §3.4's freshness rule covers what this sheet
    // DISPLAYS as much as what it gates on: a placeholder that survived a
    // sign-out would prompt the new session with the previous account holder's
    // network name and invite them to type it.
    confirm_->set_placeholder_text(name_);
    Gate();
  }

  void ReadName() {
    nameFlow_.Abandon();
    if (!CanAct()) {
      ApplyName(AccountFieldState::NoSession, {});
      return;
    }
    ApplyName(AccountFieldState::Loading, {});
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    // 20 s, and the give-up lands on Failed — which is also the CLOSED gate.
    const uint32_t flow = nameFlow_.Begin(
        kApiTimeoutMs, [this] { ApplyName(AccountFieldState::Failed, {}); });
    host_.api().getNetworkUser(
        [this, epoch, seen, flow](std::optional<urnet::GetNetworkUserResult> result,
                                  std::optional<std::string> err) {
          PostToMain([this, epoch, seen, flow, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (!nameFlow_.Settle(flow, "delete-sheet name read")) return;
            if (!result || err || result->error || !result->network_user ||
                result->network_user->network_name.empty()) {
              g_warning("account: delete-sheet name read failed: %s",
                        err ? err->c_str() : "(no network user)");
              ApplyName(AccountFieldState::Failed, {});  // the gate fails CLOSED
              return;
            }
            ApplyName(AccountFieldState::Loaded, result->network_user->network_name);
          });
        });
  }

  void Gate() {
    const std::string typed = TrimSpace(confirm_->get_text().raw());
    deleteButton_->set_sensitive(!deleting_ && !name_.empty() && typed == name_);
  }

  void Submit() {
    // belt and braces: the gate is re-checked at the commit
    if (deleting_ || name_.empty() || TrimSpace(confirm_->get_text().raw()) != name_) return;
    // ...and so is the SESSION (§7: the gate is IsLoggedIn() EVERYWHERE). This
    // modal outlives the session under it whenever the tray signs out or the
    // SDK's auth-invalid handler fires: name_ and the typed text are untouched
    // by either, so the gate above would still stand open. Api::networkDelete
    // takes NO arguments and acts on whatever jwt is current — an
    // unauthenticated destructive request is the one thing the gate exists to
    // stop, and its 401 would print raw on the error line instead of the
    // NoSession state the rest of this page shows.
    if (!CanAct()) {
      ApplyName(AccountFieldState::NoSession, {});  // the gate closes with it
      return;
    }
    deleting_ = true;
    deleteButton_->set_sensitive(false);
    error_->set_visible(false);

    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    // 20 s. A destructive modal that never answers is the worst hang on this
    // destination: the user cannot tell whether their network, its data and
    // its earnings are gone. A verdict they can act on beats a frozen dialog.
    const uint32_t flow = deleteFlow_.Begin(kApiTimeoutMs, [this] {
      deleting_ = false;
      SetToned(*error_, kUrDanger,
               T_("error_deleting_account",
                  "Sorry, there was an error deleting your account."));
      error_->set_visible(true);
      Gate();
    });
    // The sign-in being deleted, read now: by the answer a server sign-out
    // and another sign-in may have landed, and only this one is signed out.
    const auth_logout::Report signIn = host_.SignInNow("account deleted");
    host_.api().networkDelete(
        [this, epoch, seen, flow, signIn](std::optional<urnet::NetworkDeleteResult> result,
                                          std::optional<std::string> err) {
          PostToMain([this, epoch, seen, flow, signIn, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (!deleteFlow_.Settle(flow, "network delete")) return;
            deleting_ = false;
            // A refused deletion is a result with an error (HTTP 200): the
            // account still exists, so only a result with no error signs out.
            const bool serverError = result && result->error.has_value();
            const auto outcome = account::DecideDeleteAccount(
                err ? &*err : nullptr, result.has_value(), serverError,
                serverError ? result->error->message : std::string());
            if (!outcome.deleted) {
              g_warning("account: networkDelete failed: %s",
                        err ? err->c_str()
                            : (serverError ? result->error->message.c_str() : "(no result)"));
              // still signed in: the sheet stays open and the gate re-arms for a retry
              SetToned(*error_, kUrDanger,
                       Glib::ustring(account::DeleteAccountErrorText(
                           T_("error_deleting_account",
                              "Sorry, there was an error deleting your account."),
                           outcome.detail)));
              error_->set_visible(true);
              Gate();
              return;
            }
            set_visible(false);
            if (on_deleted) on_deleted(signIn);
          });
        });
  }

  SdkHost& host_;
  std::function<bool()> canAct_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);
  AccountFlow nameFlow_;    // the fresh-name read
  AccountFlow deleteFlow_;  // the commit
  Gtk::Label* nameLine_ = nullptr;
  Gtk::Entry* confirm_ = nullptr;
  Gtk::Label* error_ = nullptr;
  Gtk::Button* deleteButton_ = nullptr;
  Gtk::Button* cancel_ = nullptr;
  std::string name_;  // the FRESHLY read name; empty means the gate is closed
  bool deleting_ = false;
};

// =============================================================================
// AccountPage
// =============================================================================

AccountPage::AccountPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0), host_(host) {
  EnsureBrandCss();   // the pane-shell vocabulary
  EnsureDrawerCss();  // .ur-caption / .ur-label-faint / the card + input styles
  set_hexpand(true);
  set_vexpand(true);

  BuildPlanPane();
  append(*paneA_.root);
  ruleB_ = kit::MakePaneVRule();
  append(*ruleB_);

  BuildAccountPane();
  append(*paneB_.root);
  ruleC_ = kit::MakePaneVRule();
  append(*ruleC_);

  BuildCodesPane();
  append(*paneC_.root);

  // ApplyStrings parity (§3.1.3): the one-shot initial paint. Before any load
  // the account card, the referral summary and the codes table already carry
  // NoSession, so the screen is never blank and never a dash.
  SettleNoSession();
  // ...and the plan pane seeds "Free" before the first balance snapshot.
  ApplyBalance(balance_);
}

AccountPage::~AccountPage() {
  ++*epoch_;       // orphan every in-flight completion
  *alive_ = false;  // ...and every marshaled cleanup
  // ...and disarm every watchdog before the widgets they would write to go.
  // (AccountFlow's own destructor does this too; doing it here makes the order
  // explicit rather than dependent on member-declaration order.) Deliberately
  // NOT ReleaseInFlight(): that one touches a widget, and a destructor is the
  // wrong place to paint.
  accountFlow_.Abandon();
  codesFlow_.Abandon();
  referralFlow_.Abandon();
  nameFlow_.Abandon();
  resetFlow_.Abandon();
  portalFlow_.Abandon();
  removeFlow_.Abandon();
  resetRateLimitTick_.disconnect();
}

// ---- gating + messaging -----------------------------------------------------

bool AccountPage::CanCallApi() {
  // The preview gate is FIRST for a reason: a preview build once reached
  // production authenticated. The session test is IsLoggedIn(), never
  // apiReady() — that is true from SDK init, long before a login, and its
  // 401 renders as an empty account.
  return !previewMode_ && host_.IsLoggedIn();
}

void AccountPage::Snack(const Glib::ustring& message, bool error) {
  if (on_snackbar) {
    on_snackbar(message, error);
    return;
  }
  g_warning("account: snackbar unbound; dropping message: %s", message.c_str());
}

void AccountPage::CopyFull(const std::string& value, const Glib::ustring& confirmation) {
  if (value.empty()) return;  // a copy affordance over nothing is disabled anyway
  get_clipboard()->set_text(value);
  Snack(confirmation, false);
}

// ---- lifecycle ---------------------------------------------------------------

void AccountPage::Load() {
  ++*epoch_;  // drop every completion armed for the previous session
  // ...and release what those dropped completions were going to release. A
  // completion the epoch guard discards is STILL a completion for flag
  // purposes: without this, a nav-select (or an auth-state change) landing
  // between a mutation and its answer strands the flag true, and the control
  // that flag disabled is dead for the rest of the process.
  ReleaseInFlight();

  // Local first: the client id is a DEVICE read, correct with no session and
  // no round trip, and so are the extender settings (the SDK's view
  // controller, no api call).
  ApplyClientId();
  if (extenderSection_) extenderSection_->Load();

  // Each load gates itself on the session and settles its own panel on
  // NoSession WITHOUT a request — a 401 must never arrive to be mistaken for
  // an empty account, and nothing may sit on a spinner.
  LoadAccount();
  LoadReferralInfo();
  LoadBalanceCodes();
}

void AccountPage::ApplyBreakpoint(int widthDip) {
  const int lanes = widthDip >= kThreePaneDip ? 3 : (widthDip >= kTwoPaneDip ? 2 : 1);
  if (lanes_ == lanes) return;  // no-op unless the fold actually changes
  lanes_ = lanes;
  // The CODES pane folds first (a record, nothing in it actionable); below 900
  // the PLAN pane folds, not the account pane — 360 beside ~340 is two
  // unreadable half-columns. A folded pane hides WITH its rule.
  paneC_.root->set_visible(lanes >= 3);
  ruleC_->set_visible(lanes >= 3);
  paneA_.root->set_visible(lanes >= 2);
  ruleB_->set_visible(lanes >= 2);
}

void AccountPage::SetPreviewMode(bool on) { previewMode_ = on; }

void AccountPage::ShowPreviewState() { SettleNoSession(); }

void AccountPage::ResetForSignOut() {
  ++*epoch_;    // a stale answer can never repopulate the next session
  CloseSheets();  // an open modal is a surface too (§1)
  SettleNoSession();
}

void AccountPage::ReleaseInFlight() {
  // EVERY watchdog, loads included: an epoch bump means this page is about to
  // repaint from scratch (a fresh Load, or the no-session settle), and a give-up
  // still armed from the previous session would fire up to 20 s later and paint
  // Failed over whatever replaced it. Each load re-arms its own flow.
  accountFlow_.Abandon();
  codesFlow_.Abandon();
  referralFlow_.Abandon();
  nameFlow_.Abandon();
  resetFlow_.Abandon();
  portalFlow_.Abandon();
  removeFlow_.Abandon();
  savingName_ = false;
  sendingReset_ = false;
  openingPortal_ = false;
  removingAuth_ = false;
  portalRow_->set_sensitive(true);
  // saveNameButton_ and sendResetButton_ are re-gated by the ApplyAccountState
  // that every caller of this runs next, and they read these flags — which is
  // exactly why the flags have to be false BEFORE that gate runs.
}

void AccountPage::CloseSheets() {
  // Hiding is the whole re-gate: every sheet's Open() re-reads, re-arms and
  // clears its own cached name/code from scratch, and the hide edge reports
  // the modal closed through WireSheet. Left up instead, the referral sheet
  // keeps the previous account's network on screen with the Update button
  // still lit, the auth-code sheet keeps a live credential visible, and the
  // delete sheet keeps the cached name §3.4 exists to forbid.
  auto hide = [](Gtk::Window* sheet) {
    if (sheet != nullptr && sheet->get_visible()) sheet->set_visible(false);
  };
  hide(addAuthSheet_.get());
  hide(authCodeSheet_.get());
  hide(deleteSheet_.get());
  // the share code is this network's extender list and the import sheet may
  // hold a pasted payload: neither belongs to the next account
  hide(extenderShareSheet_.get());
  hide(extenderImportSheet_.get());
  hide(confirmDialog_.get());
}

void AccountPage::SettleNoSession() {
  ReleaseInFlight();
  // Every per-account value is dropped. userAuth_ is the dangerous one: the
  // password reset mails a link to it, so a leftover value would mail the
  // PREVIOUS owner.
  userAuth_.clear();
  userAuthVerified_ = false;
  needsNameClaim_ = false;
  acknowledgedName_.clear();
  referralCode_.clear();
  referralCount_.Reset();
  authMethods_.clear();
  codes_.clear();

  CloseNameEditor();
  nameBox_->set_text("");
  ApplyNetworkName({});
  ApplyAuthLine();
  ApplyAccountState(AccountFieldState::NoSession);

  methodsState_ = AccountFieldState::NoSession;
  RenderAuthMethods();
  ApplyReferralsRow(AccountFieldState::NoSession);
  codesState_ = AccountFieldState::NoSession;
  RenderBalanceCodes();
  ApplyClientId();
  // Pane A is per-account state too (§1 wipes ALL of it). Repainting from the
  // retained snapshot left the departed account's entitlement, usage bar and
  // daily figure on screen beside a pane B correctly reading "Please login" —
  // on a shared machine that is the previous user's plan and consumption,
  // readable by whoever sits down next. A default snapshot re-seeds "Free",
  // the faint full-width track and the not-yet-known daily figure.
  ApplyBalance(AccountBalance{});
}

// ---- §2.3 the balance relay: the ONE painter of pane A's figures -------------

void AccountPage::ApplyBalance(const AccountBalance& snapshot) {
  balance_ = snapshot;

  // 1. the plan value. Gold iff Pro — the ONE entitlement colour.
  const Glib::ustring plan = balance_.guest ? Glib::ustring(T_("guest", "Guest"))
                             : balance_.isPro ? Glib::ustring(T_("supporter", "Pro"))
                                              : Glib::ustring(T_("free", "Free"));
  SetToned(*planValue_, balance_.isPro ? kProGold : kOffWhite, plan);
  kit::SetAccessibleLabel(*planValue_, Glib::ustring(T_("plan", "Plan")) + ", " + plan);
  // the Pro label is a button that replays the celebration: say so with the
  // cursor (free and guest labels stay plain text)
  if (balance_.isPro && !balance_.guest) {
    SetPointerCursor(*planValue_);
  } else {
    planValue_->set_cursor();
  }

  // the Stripe customer portal only manages a Stripe subscription: a free
  // network, or a store or crypto subscription, got the portal's raw error
  if (portalRow_) portalRow_->set_visible(ShowsManageSubscription(balance_.subscriptionStoreFamily));

  // 2. the confirmation ring: active AND visible exactly while the poll runs.
  planRing_->set_visible(balance_.confirming);
  if (balance_.confirming) {
    planRing_->start();
  } else {
    planRing_->stop();
  }

  // 3. the usage bar.
  usageBar_->SetData(balance_.usedByteCount, balance_.pendingByteCount,
                     balance_.availableByteCount);

  // 4. the primary action: guests ALWAYS get "Create an account"; a signed-in
  //    free account gets "Upgrade"; Pro gets nothing to buy.
  upgradeButton_->set_visible(balance_.guest || !balance_.isPro);
  upgradeButton_->set_label(balance_.guest
                                ? T_("create_an_account", "Create an account")
                                : T_("upgrade", "Upgrade"));

  // 5. the daily figure and the bar's three. AccountBalance::loaded is
  //    HasFetched, and this is where it earns its keep: before any snapshot has
  //    landed there is no balance to report, and printing "0 B" asserts a
  //    figure the server never gave (§0.4 — an async field renders its state,
  //    never a fabricated value). The plan value above deliberately still seeds
  //    "Free": the spec says so in as many words.
  const bool canCallApi = CanCallApi();
  ApplyBalanceFigure(*dailyValue_, T_("daily_data_balance", "Daily Data Balance"),
                     balance_.startBalanceByteCount, balance_.loaded, canCallApi);
  ApplyBalanceFigure(*usedValue_, T_("used_data_key", "Used"), balance_.usedByteCount,
                     balance_.loaded, canCallApi);
  ApplyBalanceFigure(*pendingValue_, T_("pending_data_key", "Pending"),
                     balance_.pendingByteCount, balance_.loaded, canCallApi);
  ApplyBalanceFigure(*availableValue_, T_("available_data_key", "Available"),
                     balance_.availableByteCount, balance_.loaded, canCallApi);

  // 6. the referral pair (repainted whenever the referral load lands, which
  //    calls back through here): the count once the read lands. It used to
  //    start at 0 and stay 0 on a failed read; like the daily figure it now
  //    renders its state until then, and a failed read offers Try again.
  const ReferralTotalsView referralView = referralCount_.View();
  if (referralView == ReferralTotalsView::Count) {
    const int64_t totalReferrals = referralCount_.Total();
    referralTotals_->set_text(
        Format(T_("total_referrals_lld", "Total Referrals: {}"), totalReferrals));
    const ReferralTerms& terms = CurrentReferralTerms();
    const Glib::ustring bonus =
        Format(T_("referral_bonus", "+{} GiB/Day"),
               terms.PaidReferrals(totalReferrals) * terms.bonusGibPerDay);
    SetToned(*referralBonus_, kOffWhite, bonus);
  } else {
    referralTotals_->set_text(T_("total_referrals", "Total referrals"));
    ApplyFieldState(*referralBonus_, referralView == ReferralTotalsView::Unavailable
                                         ? AccountFieldState::Failed
                                     : CanCallApi() ? AccountFieldState::Loading
                                                    : AccountFieldState::NoSession);
  }
  kit::SetAccessibleLabel(*referralBonus_,
                          referralTotals_->get_text() + ", " + referralBonus_->get_text());
  referralRetry_->set_visible(referralView == ReferralTotalsView::Unavailable);
  // the program's rule under its numbers, from the same terms as the bonus
  referralDetail_->set_text(Format(T_("referral_panel_detail",
                                      "Every verified referral gives each of you {} GiB/day for "
                                      "free, for life."),
                                   CurrentReferralTerms().bonusGibPerDay));
}

// ---- PANE A: PLAN (360) ------------------------------------------------------

void AccountPage::BuildPlanPane() {
  paneA_ = kit::MakePane(T_("plan", "Plan"));
  paneA_.root->set_size_request(kPaneAWidth, -1);
  paneA_.root->set_hexpand(false);
  kit::SetAccessibleLabel(*paneA_.root, T_("plan", "Plan"));

  // The header's right slot. AccountPaneAMeta exists in the windows markup with
  // NO writer anywhere (spec flag) — the kit already builds that empty meta
  // label, so only the ring is added here. It is inserted after the title so
  // the two share the right slot in the spec's order.
  planRing_ = Gtk::make_managed<Gtk::Spinner>();
  planRing_->set_size_request(kRingPx, kRingPx);
  planRing_->set_valign(Gtk::Align::CENTER);
  planRing_->set_visible(false);
  kit::SetAccessibleLabel(*planRing_, T_("loading", "Loading..."));
  paneA_.header->insert_child_after(*planRing_, *paneA_.title);

  Gtk::Box* content = paneA_.content;

  // 1. the plan value (22 SemiBold; gold iff Pro).
  {
    auto row = MakePaddedRow(kPlanPadY);
    planValue_ = Gtk::make_managed<Gtk::Label>();
    planValue_->add_css_class("ur-value");
    planValue_->set_xalign(0);
    Pango::AttrList attrs;
    auto size = Pango::Attribute::create_attr_size_absolute(kPlanValuePx * PANGO_SCALE);
    attrs.insert(size);
    auto weight = Pango::Attribute::create_attr_weight(Pango::Weight::SEMIBOLD);
    attrs.insert(weight);
    // set_attributes rides ON TOP of the markup SetToned writes, so the size
    // and weight survive every colour change.
    planValue_->set_attributes(attrs);
    // a Pro network's label replays the Pro celebration on a tap; the gesture
    // is inert for free and guest labels (ApplyBalance keeps the cursor honest)
    auto planTap = Gtk::GestureClick::create();
    planTap->signal_released().connect([this](int, double, double) {
      if (balance_.isPro && !balance_.guest && on_plan_label_tap) on_plan_label_tap();
    });
    planValue_->add_controller(planTap);
    row.content->append(*planValue_);
    content->append(*row.root);
  }

  // 2. the usage bar + legend.
  {
    auto row = MakePaddedRow(kProsePadY);
    usageBar_ = Gtk::make_managed<AccountUsageBar>();
    row.content->append(*usageBar_);
    content->append(*row.root);
  }

  // 3. the pane's primary action.
  upgradeButton_ = Gtk::make_managed<Gtk::Button>(T_("upgrade", "Upgrade"));
  upgradeButton_->add_css_class("ur-pane-primary");
  upgradeButton_->signal_clicked().connect([this] {
    if (on_open_upgrade) {
      // the window forks it: guests into the create-account (guest upgrade)
      // flow, everyone else into the UpgradeSheet (linux-reuse §2.14)
      on_open_upgrade();
      return;
    }
    g_warning("account: upgrade route unbound; the button opened nothing");
  });
  content->append(*upgradeButton_);

  // 4. the data-usage group. Its info button, right above the daily balance,
  //    opens "About your data": what Used, Pending and Available mean and when
  //    the free data refreshes.
  {
    auto header = kit::MakePaneGroupHeader(T_("data_usage", "Data usage"));
    auto* info = Gtk::make_managed<Gtk::Button>();
    info->set_icon_name("help-about-symbolic");
    info->add_css_class("ur-pane-action");
    info->set_tooltip_text(T_("data_info_title", "About your data"));
    kit::SetAccessibleLabel(*info, T_("data_info_title", "About your data"));
    info->signal_clicked().connect([this] {
      if (on_open_data_info) on_open_data_info();
    });
    header.trailing->append(*info);
    content->append(*header.root);
  }

  // 5. daily balance, then the usage bar's figures under the legend's own
  //    words: the bar shows the shape, these carry the numbers. Neutral text:
  //    the legend's dots own the colours.
  {
    auto row = kit::MakePaneKeyValueRow(
        T_("daily_data_balance", "Daily Data Balance"), {}, kRowKeyValue);
    dailyValue_ = row.value;
    content->append(*row.root);
  }
  for (const auto& [key, value] :
       {std::pair{Glib::ustring(T_("used_data_key", "Used")), &usedValue_},
        std::pair{Glib::ustring(T_("pending_data_key", "Pending")), &pendingValue_},
        std::pair{Glib::ustring(T_("available_data_key", "Available")), &availableValue_}}) {
    auto row = kit::MakePaneKeyValueRow(key, {}, kRowKeyValue);
    *value = row.value;
    content->append(*row.root);
  }

  // 6. referrals (both halves repainted by ApplyBalance).
  {
    auto row = kit::MakePaneKeyValueRow({}, {}, kRowKeyValue);
    referralTotals_ = row.key;
    referralBonus_ = row.value;
    referralRetry_ = Gtk::make_managed<Gtk::Button>(T_("try_again", "Try again"));
    referralRetry_->set_valign(Gtk::Align::CENTER);
    referralRetry_->set_margin_start(8);
    referralRetry_->set_visible(false);
    referralRetry_->signal_clicked().connect([this] { RetryReferralInfo(); });
    if (auto* inner = dynamic_cast<Gtk::Box*>(referralBonus_->get_parent())) {
      inner->append(*referralRetry_);
    }
    content->append(*row.root);
  }
  // what the bonus means, in the program's terms (ApplyBalance)
  {
    auto row = MakeProseRow({}, kProsePadY);
    referralDetail_ = row.line;
    content->append(*row.root);
  }

  // 7. the redeemed codes' count, one glance at pane C from here; Redeem below
  //    stays the action. Hidden until a fetch answers (RenderBalanceCodes).
  {
    auto row = kit::MakePaneKeyValueRow(T_("balance_codes_title", "Balance Codes"), {},
                                        kRowKeyValue);
    codesCountRow_ = row.root;
    codesCountValue_ = row.value;
    codesCountRow_->set_visible(false);
    content->append(*row.root);
  }

  // 8. Redeem Balance Code -> the existing RedeemCodeSheet.
  {
    auto row = kit::MakePaneTwoLineRowButton(
        T_("redeem_balance_code", "Redeem Balance Code"), {}, kRowSingle);
    row.root->signal_clicked().connect([this] {
      if (on_open_redeem) {
        on_open_redeem();
        return;
      }
      g_warning("account: redeem route unbound; the row opened nothing");
    });
    content->append(*row.root);
  }

  // 9. Manage Subscription (the Stripe customer portal).
  {
    auto row = kit::MakePaneTwoLineRowButton(
        T_("site_app_manage_subscription", "Manage Subscription"), {}, kRowTall);
    portalRow_ = row.root;
    portalRow_->signal_clicked().connect([this] { OpenCustomerPortal(); });
    content->append(*row.root);
  }
}

void AccountPage::OpenCustomerPortal() {
  if (openingPortal_) return;
  // CanCallApi(), not IsLoggedIn(): the preview harness must not open the REAL
  // Stripe customer portal for whatever account's jwt happens to be on disk.
  if (!CanCallApi()) {
    // Warning severity: it persists, because it is the answer to a click.
    Snack(T_("please_login_to_urnetwork", "Please login to URnetwork"), true);
    return;
  }
  openingPortal_ = true;
  portalRow_->set_sensitive(false);

  urnet::StripeCreateCustomerPortalArgs args{};
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // 20 s: a row greyed out forever, with no browser and no message, is
  // indistinguishable from a broken app.
  const uint32_t flow = portalFlow_.Begin(kApiTimeoutMs, [this] {
    openingPortal_ = false;
    portalRow_->set_sensitive(true);
    Snack(T_("something_went_wrong", "Something went wrong."), true);
  });
  host_.api().stripeCreateCustomerPortal(
      std::optional<urnet::StripeCreateCustomerPortalArgs>(args),
      [this, epoch, seen, flow](
          std::optional<urnet::StripeCreateCustomerPortalResult> result,
          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, flow, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;
          if (!portalFlow_.Settle(flow, "customer portal")) return;
          openingPortal_ = false;
          portalRow_->set_sensitive(true);  // on EVERY path
          const std::string url = result ? result->url.value_or(std::string()) : std::string();
          const std::string message = FirstMessage(
              result && result->error ? result->error->message : std::string(), err);
          if (url.empty()) {
            g_warning("account: stripeCreateCustomerPortal failed: %s",
                      message.empty() ? "(no url)" : message.c_str());
            if (result && result->error) {
              // a refusal reads as its code's line, or as the row's own line with
              // the server's words under it (ServerRefusalText.hpp)
              const ServerRefusalText refusal =
                  ServerRefusalTextFor(result->error->code.value_or(std::string()),
                                       result->error->message, kManageSubscriptionRefusalLine);
              Snack(Glib::ustring(
                        ServerRefusalDisplay(T_(refusal.line.key, refusal.line.english), refusal)),
                    true);
              return;
            }
            Snack(message.empty()
                      ? Glib::ustring(T_("something_went_wrong", "Something went wrong."))
                      : Glib::ustring(message),
                  true);
            return;
          }
          // OBSERVE the launch verdict: a browser that never opened must not
          // look like a portal that did.
          GError* launchError = nullptr;
          if (!g_app_info_launch_default_for_uri(url.c_str(), nullptr, &launchError)) {
            g_warning("account: customer portal launch failed: %s",
                      launchError ? launchError->message : "(no error text)");
            Snack(T_("something_went_wrong", "Something went wrong."), true);
          }
          g_clear_error(&launchError);
        });
      });
}

// ---- PANE B: ACCOUNT (star) --------------------------------------------------

void AccountPage::BuildAccountPane() {
  paneB_ = kit::MakePane(T_("account", "Account"));
  paneB_.root->set_hexpand(true);
  kit::SetAccessibleLabel(*paneB_.root, T_("account", "Account"));
  BuildProfileGroup(*paneB_.content);
  BuildSecurityGroup(*paneB_.content);
  BuildReferralsRow(*paneB_.content);
  BuildExtendersGroup(*paneB_.content);
  BuildDangerGroup(*paneB_.content);
}

// EXTENDER.md K6: "under account, a section named Extenders". It goes in the
// ACCOUNT pane rather than in a fourth column of its own -- the fold table
// above hides the codes pane below 1500 dip and the plan pane below 900, so a
// fourth pane would put these settings out of reach on an ordinary laptop,
// and the account pane is the one that is always on screen.
//
// Placed after the referrals row and before the DANGER group: sign out and
// delete account stay the last things on the pane.
void AccountPage::BuildExtendersGroup(Gtk::Box& host) {
  extenderSection_ = Gtk::make_managed<ExtenderSection>(host_);
  extenderSection_->on_snackbar = [this](const Glib::ustring& message, bool error) {
    Snack(message, error);
  };
  extenderSection_->on_share = [this] { ShowExtenderShareSheet(); };
  extenderSection_->on_import = [this] { ShowExtenderImportSheet(); };
  extenderSection_->on_reset = [this] { ConfirmResetExtenders(); };
  host.append(*extenderSection_);
}

void AccountPage::BuildProfileGroup(Gtk::Box& host) {
  host.append(*kit::MakePaneGroupHeader(T_("profile", "Profile")).root);

  // 1. the network-name VIEW row. Exactly one of {row, edit panel} shows. Its
  //    icon is the profile's (android nav_list_item_user, apple
  //    ur.symbols.user.circle: a user in a circle).
  nameRow_ = kit::MakePaneTwoLineRowButton(T_("network_name_label", "Network name"), {},
                                           kRowTall, RowIcon(MdiGlyph::AccountCircle));
  // §3.1.2 + the spec's glyph map: U+E70F pencil, NOT the chevron every other
  // row on this pane carries. This row is the only one that edits in place.
  RetargetRowGlyph(*nameRow_.root, "document-edit-symbolic");
  nameRow_.root->signal_clicked().connect([this] { OpenNameEditor(); });
  host.append(*nameRow_.root);

  // 2. the EDIT panel (collapsed by default). #151515 like the windows panel —
  // .ur-sheet is that fill, applied to the row host rather than a new class.
  {
    auto row = MakePaddedRow(kProsePadY);
    row.root->add_css_class("ur-sheet");
    row.content->set_spacing(10);
    auto* label = Gtk::make_managed<Gtk::Label>(T_("network_name_label", "Network name"));
    label->add_css_class("ur-input-label");
    label->set_xalign(0);
    row.content->append(*label);
    nameBox_ = Gtk::make_managed<Gtk::Entry>();
    nameBox_->add_css_class("ur-input");
    kit::SetAccessibleLabel(*nameBox_, T_("network_name_label", "Network name"));
    nameBox_->signal_activate().connect([this] { OnSaveNetworkName(); });
    row.content->append(*nameBox_);
    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    saveNameButton_ = Gtk::make_managed<Gtk::Button>(T_("save", "Save"));
    saveNameButton_->add_css_class("suggested-action");
    saveNameButton_->signal_clicked().connect([this] { OnSaveNetworkName(); });
    actions->append(*saveNameButton_);
    auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
    cancel->signal_clicked().connect([this] {
      CloseNameEditor();
      // the view row still shows the saved name; the status line is cleared
      ApplySupporting(*statusLine_, {}, kit::ValidationState::NotChecked);
    });
    actions->append(*cancel);
    row.content->append(*actions);
    nameEditPanel_ = row.root;
    nameEditPanel_->set_visible(false);
    host.append(*row.root);
  }

  // 3. the auth line — COLLAPSED whenever its text is empty (an empty
  //    fixed-height row is a 38px hole). It says more about the row above
  //    and has no icon of its own, so it sits in the titles' column.
  {
    auto* row = kit::MakePaneRow(kRowAuthLine);
    authText_ = Gtk::make_managed<Gtk::Label>();
    authText_->add_css_class("ur-key");
    authText_->set_xalign(0);
    authText_->set_hexpand(true);
    authText_->set_ellipsize(Pango::EllipsizeMode::END);
    authText_->set_margin_start(kit::kRowIconInset);
    if (auto* inner = dynamic_cast<Gtk::Box*>(row->get_first_child())) {
      inner->append(*authText_);
    }
    authRow_ = row;
    authRow_->set_visible(false);
    host.append(*row);
  }

  // 4. the status line: the account load state, the save verdict, or the
  //    password-reset outcome — one line, three writers, one voice. In the
  //    titles' column, like the auth line.
  {
    auto prose = MakeProseRow({}, kStatePadY);
    statusLine_ = prose.line;
    statusLine_->set_margin_start(kit::kRowIconInset);
    host.append(*prose.root);
  }

  // 5. update password: the action resets it by mail.
  sendResetButton_ = AddButtonRow(host, T_("update_password", "Update password"), {},
                                  T_("send", "Send"), MdiGlyph::LockReset);
  sendResetButton_->set_sensitive(false);
  sendResetButton_->signal_clicked().connect([this] { SendPasswordReset(); });

  // 6. Sessions (REVOKE-UI-FINAL.md §1): the account's signed-in sessions, on
  //    a page of their own, one row directly after the profile with the face
  //    profile glyph. It always shows and asks the server nothing: the page
  //    says when sessions are not available yet.
  {
    auto row = kit::MakePaneTwoLineRowButton(T_("sessions_title", "Sessions"), {}, kRowTall,
                                             RowIcon(MdiGlyph::SessionFace));
    row.root->signal_clicked().connect([this] {
      if (on_open_sessions) {
        on_open_sessions();
        return;
      }
      g_warning("account: sessions route unbound; the row opened nothing");
    });
    host.append(*row.root);
  }
}

void AccountPage::BuildSecurityGroup(Gtk::Box& host) {
  // The store has no plain "Security" key — secure_your_account is reused
  // (flagged upstream in the spec; do NOT invent a key).
  host.append(*kit::MakePaneGroupHeader(T_("secure_your_account", "Secure Your Account")).root);

  // 1. the login-methods list (rebuilt whole by RenderAuthMethods).
  authMethodsPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  host.append(*authMethodsPanel_);

  // 2. add a method.
  auto* add = AddButtonRow(host, T_("site_app_login_methods", "Login methods"), {},
                           T_("add", "Add"), MdiGlyph::Login);
  add->signal_clicked().connect([this] { ShowAddAuthSheet(); });

  // 3. auth code: the barcode, the glyph the login stack's auth-code button
  //    wears.
  auto* create = AddButtonRow(
      host, T_("auth_code", "Auth code"),
      T_("created_auth_codes_expire_after_5_minutes",
         "Created auth codes expire after 5 minutes"),
      T_("site_app_create_auth_code", "Create auth code"), MdiGlyph::Barcode);
  create->signal_clicked().connect([this] { ShowAuthCodeSheet(); });

  // 4. client id — off the DEVICE, not the api.
  {
    auto row = AddValueActionRow(host, T_("client_id", "Client ID"), T_("copy", "Copy"),
                                 MdiGlyph::Identifier);
    clientIdValue_ = row.value;
    clientIdCopy_ = row.action;
    clientIdCopy_->signal_clicked().connect([this] {
      CopyFull(clientId_,
               T_("client_id_copied_to_clipboard", "Client ID copied to clipboard"));
    });
  }
}

void AccountPage::BuildReferralsRow(Gtk::Box& host) {
  // Referrals are their own section now (the "Refer and earn" page); this
  // destination keeps one row that opens it, with the count on its second
  // line. The row is live signed out too — the page settles on no-session.
  host.append(*kit::MakePaneGroupHeader(T_("refer_and_earn", "Refer and earn")).root);
  // the heart android (nav_list_item_refer) and apple (ur.symbols.heart) give
  // their Referrals row
  referralsRow_ = kit::MakePaneTwoLineRowButton(T_("referrals", "Referrals"), {}, kRowTall,
                                                RowIcon(MdiGlyph::Heart));
  referralsRow_.root->signal_clicked().connect([this] {
    if (on_open_referrals) on_open_referrals();
  });
  host.append(*referralsRow_.root);
}

void AccountPage::BuildDangerGroup(Gtk::Box& host) {
  // The windows heading here is `account` again — the danger rows live under
  // the account's own name, not under a "Danger" word the store lacks.
  host.append(*kit::MakePaneGroupHeader(T_("account", "Account")).root);

  // Whole-row buttons with chevrons — never "Sign out [Sign out]".
  {
    auto row = kit::MakePaneTwoLineRowButton(T_("sign_out", "Sign out"), {}, kRowTall,
                                             RowIcon(MdiGlyph::Logout));
    row.root->signal_clicked().connect([this] {
      // no confirm: signing out costs nothing and is instantly reversible
      host_.Logout();
    });
    host.append(*row.root);
  }
  {
    // NOT red here: red belongs to the confirmation context (the icon is the
    // muted tint of every row).
    auto row = kit::MakePaneTwoLineRowButton(T_("delete_account_2", "Delete account"), {},
                                             kRowTall, RowIcon(MdiGlyph::Delete));
    row.root->signal_clicked().connect([this] { ShowDeleteAccountSheet(); });
    host.append(*row.root);
  }
}

// ---- PANE C: BALANCE CODES (380) ---------------------------------------------

void AccountPage::BuildCodesPane() {
  paneC_ = kit::MakePane(T_("balance_codes_title", "Balance Codes"));
  paneC_.root->set_size_request(kPaneCWidth, -1);
  paneC_.root->set_hexpand(false);
  kit::SetAccessibleLabel(*paneC_.root, T_("balance_codes_title", "Balance Codes"));

  auto* host = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  host->set_vexpand(true);
  codesPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  host->append(*codesPanel_);
  // ONE centred line in the FULL-HEIGHT pane, never a short card at the top of
  // a tall column.
  codesEmpty_ = kit::MakePaneEmptyLine({});
  host->append(*codesEmpty_);
  paneC_.content->append(*host);
}

// RenderBalanceCodes is the ONLY writer of pane C (the load, the initial paint
// and sign-out all come through here — scattered writers used to disagree).
void AccountPage::RenderBalanceCodes() {
  // pane A's count row: every answered fetch has a count, zero included
  const std::optional<std::string> count = account_plan::CodeCountText(
      codesState_ == AccountFieldState::Loaded || codesState_ == AccountFieldState::Empty,
      codes_.size());
  if (codesCountRow_) {
    codesCountRow_->set_visible(count.has_value());
    SetToned(*codesCountValue_, kOffWhite, count.value_or(std::string()));
    kit::SetAccessibleLabel(*codesCountValue_,
                            Glib::ustring(T_("balance_codes_title", "Balance Codes")) + ", " +
                                count.value_or(std::string()));
  }
  RemoveAllChildren(*codesPanel_);
  const bool loaded = codesState_ == AccountFieldState::Loaded && !codes_.empty();
  codesEmpty_->set_visible(!loaded);
  if (!loaded) {
    // All four renders are textually DISTINCT: a failed fetch never reads as
    // the reassuring "No balance codes found".
    switch (codesState_) {
      case AccountFieldState::Failed:
        SetToned(*codesEmpty_, kUrDanger, T_("something_went_wrong", "Something went wrong."));
        break;
      case AccountFieldState::Loading:
        SetToned(*codesEmpty_, kUrTextFaint, T_("loading", "Loading..."));
        break;
      case AccountFieldState::NoSession:
        SetToned(*codesEmpty_, kUrTextFaint,
                 T_("please_login_to_urnetwork", "Please login to URnetwork"));
        break;
      case AccountFieldState::NoDevice:
        SetToned(*codesEmpty_, kUrTextFaint,
                 T_("site_app_device_attaching", "Attaching device controls…"));
        break;
      case AccountFieldState::Empty:
      case AccountFieldState::Loaded:
        // the SPECIFIC line beats the generic "None"
        SetToned(*codesEmpty_, kUrTextFaint,
                 T_("no_balance_codes_found", "No balance codes found"));
        break;
    }
    kit::SetTextOrCollapse(*paneC_.meta, {});
    return;
  }

  kit::SetTextOrCollapse(*paneC_.meta, std::to_string(codes_.size()));
  // THREE columns, not four: a 380 pane, and a redeemed code's expiry is
  // already spent on the plan. Weights carry minimums so the table NARROWS
  // instead of clipping.
  const std::vector<int> weights{14, 10, 12};
  codesPanel_->append(*kit::MakePaneTableHeader(
      weights, {T_("code", "Code"), T_("data", "Data"), T_("redeemed", "Redeemed")}, 1));
  for (const auto& code : codes_) {
    auto row = kit::MakePaneTableRow(weights, kRowCode, 1);
    row.cells[0]->set_text(MaskSecret(code.secret));
    // '+' + the compact byte count. Deliberately a literal concatenation and
    // NOT a store key: the spec's string table for this destination carries no
    // "+{}" entry, and inventing one (or borrowing Earnings' plus_amount)
    // would put a key on this surface the spec does not have.
    row.cells[1]->set_text("+" + FormatByteCountCompact(code.balance_byte_count));
    row.cells[2]->set_text(DatePrefix(code.redeem_time));
    codesPanel_->append(*row.root);
  }
}

void AccountPage::LoadBalanceCodes() {
  codesFlow_.Abandon();
  if (!CanCallApi()) {
    // signed out: NO request. A 401 arriving as an empty list would render the
    // reassuring "No balance codes found".
    codes_.clear();
    codesState_ = AccountFieldState::NoSession;
    RenderBalanceCodes();
    return;
  }
  codes_.clear();
  codesState_ = AccountFieldState::Loading;
  RenderBalanceCodes();

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // 20 s: the whole pane may not sit on "Loading..." forever.
  const uint32_t flow = codesFlow_.Begin(kApiTimeoutMs, [this] {
    codes_.clear();
    codesState_ = AccountFieldState::Failed;
    RenderBalanceCodes();
  });
  host_.api().getNetworkRedeemedBalanceCodes(
      [this, epoch, seen, flow](
          std::optional<urnet::GetNetworkRedeemedBalanceCodesResult> result,
          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, flow, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;
          if (!codesFlow_.Settle(flow, "balance codes load")) return;
          // The spec's rule is "missing result OR transport err"; this SDK's
          // result ALSO carries an error field, so a server-declared failure
          // joins the same branch rather than being read as an empty list.
          if (!result || err || result->error) {
            g_warning("account: getNetworkRedeemedBalanceCodes failed: %s",
                      err ? err->c_str() : "(no result)");
            codes_.clear();
            codesState_ = AccountFieldState::Failed;
            RenderBalanceCodes();
            return;
          }
          codes_.clear();
          if (result->balance_codes) codes_ = *result->balance_codes;
          codesState_ = codes_.empty() ? AccountFieldState::Empty : AccountFieldState::Loaded;
          RenderBalanceCodes();
        });
      });
}

// ---- §3.1.3 the account load + the gate --------------------------------------

void AccountPage::ApplyAccountState(AccountFieldState state) {
  accountState_ = state;
  ApplyFieldState(*statusLine_, state);

  const bool loaded = state == AccountFieldState::Loaded;
  nameRow_.root->set_sensitive(loaded);
  nameBox_->set_sensitive(loaded);
  saveNameButton_->set_sensitive(loaded && !savingName_);
  sendResetButton_->set_sensitive(loaded && !userAuth_.empty() && !sendingReset_ &&
                                  resetRateLimit_.CanResend(MonotonicSeconds()));
  // if the state leaves Loaded while the editor is open, the editor is forced
  // closed — an editor over a field nobody can save is a trap
  if (!loaded && editingName_) CloseNameEditor();
}

void AccountPage::ApplyNetworkName(const std::string& name) {
  acknowledgedName_ = name;  // the server-acknowledged name; the box is never truth
  // the row shows the name filtered for display; the editor is seeded with it
  // as the server has it
  const std::string shown = SanitizeExternalDisplayText(name);
  kit::SetTextOrCollapse(*nameRow_.value, shown);
  if (!shown.empty()) SetToned(*nameRow_.value, kUrTextMuted, shown);
}

void AccountPage::ApplyAuthLine() {
  if (userAuth_.empty()) {
    // hidden for accounts with no user_auth (e.g. seedphrase-only)
    authText_->set_text({});
    authRow_->set_visible(false);
    return;
  }
  const Glib::ustring line =
      userAuthVerified_
          ? Format(T_("account_auth_verified", "{} (verified)"), userAuth_)
          : Format(T_("account_auth_unverified", "{} (unverified)"), userAuth_);
  authText_->set_text(line);
  authRow_->set_visible(true);
}

void AccountPage::LoadAccount() {
  accountFlow_.Abandon();
  if (!CanCallApi()) {
    userAuth_.clear();
    userAuthVerified_ = false;
    needsNameClaim_ = false;
    ApplyNetworkName({});
    ApplyAuthLine();
    ApplyAccountState(AccountFieldState::NoSession);
    authMethods_.clear();
    methodsState_ = AccountFieldState::NoSession;
    RenderAuthMethods();
    return;
  }
  ApplyAccountState(AccountFieldState::Loading);
  methodsState_ = AccountFieldState::Loading;
  RenderAuthMethods();

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // 20 s. This one gates the WHOLE profile group — the name row, the name box,
  // Save and Send all read state==Loaded — so a dropped callback would leave
  // pane B permanently insensitive under a status line reading "Loading...".
  // Failed is the honest terminal state: it says "ask again", not "empty".
  const uint32_t flow = accountFlow_.Begin(kApiTimeoutMs, [this] {
    ApplyAccountState(AccountFieldState::Failed);
    authMethods_.clear();
    methodsState_ = AccountFieldState::Failed;
    RenderAuthMethods();
  });
  host_.api().getNetworkUser(
      [this, epoch, seen, flow](std::optional<urnet::GetNetworkUserResult> result,
                                std::optional<std::string> err) {
        PostToMain([this, epoch, seen, flow, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;  // a newer Load() owns the page
          if (!accountFlow_.Settle(flow, "account load")) return;
          if (!result || err || result->error || !result->network_user) {
            g_warning("account: getNetworkUser failed: %s",
                      err ? err->c_str()
                          : (result && result->error ? result->error->message.c_str()
                                                     : "(no network user)"));
            ApplyAccountState(AccountFieldState::Failed);
            authMethods_.clear();
            methodsState_ = AccountFieldState::Failed;
            RenderAuthMethods();
            return;
          }
          const urnet::NetworkUser& user = *result->network_user;
          userAuth_ = user.user_auth.value_or(std::string());
          userAuthVerified_ = user.verified;
          needsNameClaim_ = ComputeNeedsNameClaim(user);
          ApplyNetworkName(user.network_name);
          ApplyAuthLine();
          authMethods_ = ParseAuthMethods(user);
          methodsState_ =
              authMethods_.empty() ? AccountFieldState::Empty : AccountFieldState::Loaded;
          RenderAuthMethods();
          ApplyAccountState(AccountFieldState::Loaded);  // also clears the status line
        });
      });
}

// ---- §3.1 the name editor ----------------------------------------------------

void AccountPage::OpenNameEditor() {
  // second guard for the keyboard path (the row is also disabled unless Loaded)
  if (!CanCallApi() || accountState_ != AccountFieldState::Loaded) return;
  editingName_ = true;
  nameRow_.root->set_visible(false);
  nameEditPanel_->set_visible(true);
  nameBox_->set_text(acknowledgedName_);  // seeded from the SERVER-acknowledged name
  nameBox_->grab_focus();
}

void AccountPage::CloseNameEditor() {
  editingName_ = false;
  nameEditPanel_->set_visible(false);
  nameRow_.root->set_visible(true);
}

void AccountPage::OnSaveNetworkName() {
  if (savingName_ || !CanCallApi()) return;
  const std::string name = TrimSpace(nameBox_->get_text().raw());
  if (name.empty()) {
    ApplySupporting(*statusLine_,
                    T_("network_name_length_error",
                       "Network names must be 6 characters or more"),
                    kit::ValidationState::Invalid);
    return;
  }
  savingName_ = true;
  saveNameButton_->set_sensitive(false);
  ApplySupporting(*statusLine_, T_("loading", "Loading..."),
                  kit::ValidationState::Validating);

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // 20 s: without it a dropped callback leaves Save disabled AND
  // OnSaveNetworkName early-returning on savingName_ forever — typing a name
  // and pressing Enter would do nothing, silently, for the rest of the
  // process. FinishNameSave(false, {}, {}) is the give-up: "Something went
  // wrong." in the status line, editor still open, Save live again.
  const uint32_t flow =
      nameFlow_.Begin(kApiTimeoutMs, [this] { FinishNameSave(false, {}, {}); });
  // CLAIM vs CHANGE: claiming an auto-generated name puts no reclaim cooldown
  // on it; a change applies the server's 24h cooldown to the name being given
  // up. The cooldown has NO client-side UI — it renders only as the server's
  // refusal, verbatim, in the status line. (networkUserUpdate is neither.)
  if (needsNameClaim_) {
    urnet::ClaimNetworkNameArgs args{};
    args.new_name = name;
    host_.api().claimNetworkName(
        std::optional<urnet::ClaimNetworkNameArgs>(args),
        [this, epoch, seen, flow](std::optional<urnet::ClaimNetworkNameResult> result,
                                  std::optional<std::string> err) {
          const bool ok = result.has_value() && !err.has_value() && !result->error;
          const std::string accepted = ok ? result->network_name : std::string();
          const std::string message = FirstMessage(
              result && result->error ? result->error->message : std::string(), err);
          PostToMain([this, epoch, seen, flow, ok, accepted, message] {
            if (*epoch != seen) return;
            if (!nameFlow_.Settle(flow, "network name claim")) return;
            FinishNameSave(ok, accepted, message);
          });
        });
    return;
  }
  urnet::ChangeNetworkNameArgs args{};
  args.new_name = name;
  host_.api().changeNetworkName(
      std::optional<urnet::ChangeNetworkNameArgs>(args),
      [this, epoch, seen, flow](std::optional<urnet::ChangeNetworkNameResult> result,
                                std::optional<std::string> err) {
        const bool ok = result.has_value() && !err.has_value() && !result->error;
        const std::string accepted = ok ? result->network_name : std::string();
        const std::string message = FirstMessage(
            result && result->error ? result->error->message : std::string(), err);
        PostToMain([this, epoch, seen, flow, ok, accepted, message] {
          if (*epoch != seen) return;
          if (!nameFlow_.Settle(flow, "network name change")) return;
          FinishNameSave(ok, accepted, message);
        });
      });
}

void AccountPage::FinishNameSave(bool ok, const std::string& acceptedName,
                                 const std::string& message) {
  savingName_ = false;
  saveNameButton_->set_sensitive(accountState_ == AccountFieldState::Loaded);
  // §3.1.1 — the ACCEPTED NAME is the acknowledgement, so an acknowledgement
  // carrying no name is not a success. network_name is a plain std::string in
  // the SDK result, so a 200 with the field missing yields "": adopting it
  // would blank the view row, close the editor on an empty green line and read
  // as "saving deleted my network name", with nothing to retry against.
  if (!ok || acceptedName.empty()) {
    // the server's own refusal, verbatim ("already taken", "too similar", the
    // cooldown text) — none of it is localizable, all of it is the answer
    g_warning("account: network name save failed: %s",
              message.empty() ? "(no error text)" : message.c_str());
    ApplySupporting(
        *statusLine_,
        message.empty() ? Glib::ustring(T_("something_went_wrong", "Something went wrong."))
                        : Glib::ustring(message),
        kit::ValidationState::Invalid);
    return;  // the editor STAYS OPEN
  }
  ApplyNetworkName(acceptedName);  // repaints the row AND the editor's seed
  CloseNameEditor();
  // Deliberate: the store has no "Network name changed to {}" string — the
  // ACCEPTED NAME in brand green IS the acknowledgement.
  ApplySupporting(*statusLine_, acceptedName, kit::ValidationState::Valid);
}

// ---- §3.1.2 password reset ---------------------------------------------------

void AccountPage::SendPasswordReset() {
  if (sendingReset_ || userAuth_.empty() || !CanCallApi() ||
      !resetRateLimit_.CanResend(MonotonicSeconds())) {
    return;
  }
  sendingReset_ = true;
  sendResetButton_->set_sensitive(false);

  const std::string target = userAuth_;

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // 20 s: a Send that never answers must not stay greyed out with a status
  // line that says nothing happened.
  const uint32_t flow = resetFlow_.Begin(kApiTimeoutMs, [this] {
    sendingReset_ = false;
    sendResetButton_->set_sensitive(accountState_ == AccountFieldState::Loaded &&
                                    !userAuth_.empty());
    ApplySupporting(*statusLine_,
                    T_("error_sending_password_reset_link",
                       "Error sending password reset link"),
                    kit::ValidationState::Invalid);
  });
  // the server reports a link it did not send (failed, rate limited) in the
  // result's error; a result alone is not a sent link
  host_.SendPasswordResetLink(target, [this, epoch, seen, flow, target](VerifySendNotice notice) {
    PostToMain([this, epoch, seen, flow, target, notice] {
      if (*epoch != seen) return;
      if (!resetFlow_.Settle(flow, "password reset")) return;
      sendingReset_ = false;
      resetRateLimit_.Start(notice, MonotonicSeconds());
      ApplyResetRateLimit();
      if (notice.kind == VerifySendNoticeKind::Sent) {
        ApplySupporting(
            *statusLine_,
            Format(T_("password_reset_link_sent_to", "Password reset link sent to {}."),
                   target),
            kit::ValidationState::Valid);
        return;
      }
      g_warning("account: authPasswordReset not sent (%s)",
                notice.transportError ? "request failed" : "server reported");
      ApplySupporting(*statusLine_,
                      notice.transportError ? T_("error_sending_password_reset_link",
                                                 "Error sending password reset link")
                                            : ResetSendNoticeText(notice),
                      kit::ValidationState::Invalid);
      if (!resetRateLimit_.CanResend(MonotonicSeconds())) {
        resetRateLimitTick_.disconnect();
        resetRateLimitTick_ = Glib::signal_timeout().connect_seconds(
            [this]() -> bool {
              const bool limited = !resetRateLimit_.CanResend(MonotonicSeconds());
              if (limited) {
                ApplySupporting(*statusLine_,
                                ResetSendNoticeText(resetRateLimit_.NoticeAt(MonotonicSeconds())),
                                kit::ValidationState::Invalid);
              }
              ApplyResetRateLimit();
              return limited;
            },
            1);
      }
    });
  });
}

// Send stays off while a reset rate limit lasts.
void AccountPage::ApplyResetRateLimit() {
  sendResetButton_->set_sensitive(accountState_ == AccountFieldState::Loaded &&
                                  !userAuth_.empty() && !sendingReset_ &&
                                  resetRateLimit_.CanResend(MonotonicSeconds()));
}

// ---- §3.2 the login methods --------------------------------------------------

void AccountPage::RenderAuthMethods() {
  RemoveAllChildren(*authMethodsPanel_);
  if (methodsState_ != AccountFieldState::Loaded || authMethods_.empty()) {
    // ONE state line instead of rows, on the pane grid (12,8 + hairline) —
    // each of Loading / NoSession / Empty / Failed says WHICH it is — in the
    // column the method rows' titles take.
    auto prose = MakeProseRow({}, kStatePadY);
    prose.line->set_margin_start(kit::kRowIconInset);
    ApplyFieldState(*prose.line, methodsState_ == AccountFieldState::Loaded
                                     ? AccountFieldState::Empty
                                     : methodsState_);
    authMethodsPanel_->append(*prose.root);
    return;
  }
  for (const std::string& method : authMethods_) {
    const Glib::ustring label = UpperFirst(method);
    // a key: one of the ways in
    auto* remove = AddButtonRow(*authMethodsPanel_, label, {}, T_("remove", "Remove"),
                                MdiGlyph::Key);
    // NOT red in pane mode: red belongs to the confirmation context.
    remove->signal_clicked().connect(
        [this, method, label] { ConfirmRemoveAuth(method, label); });
  }
}

void AccountPage::ConfirmRemoveAuth(const std::string& authType, const Glib::ustring& label) {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("account: no window root; the remove confirmation did not open");
    return;
  }
  if (confirmDialog_ && confirmDialog_->get_visible()) return;  // one modal at a time
  if (!BeginSheet("remove login method")) return;

  // A MODAL CONFIRM, never a two-click arm: a login method is how you get back
  // in, and removing the wrong one locks the account out.
  confirmDialog_ = std::make_unique<Gtk::Window>();
  confirmDialog_->set_transient_for(*root);
  confirmDialog_->set_modal(true);
  confirmDialog_->set_title(T_("site_app_login_methods", "Login methods"));
  confirmDialog_->set_resizable(false);
  // hide-on-close, not destroy: this window is owned by a unique_ptr, and a
  // window-manager close that DESTROYED it would leave that pointer dangling
  confirmDialog_->set_hide_on_close(true);
  confirmDialog_->add_css_class("ur-sheet");
  AddEscapeToClose(*confirmDialog_);
  WireSheet(*confirmDialog_);

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  box->set_size_request(kConfirmWidth, -1);

  auto* heading =
      Gtk::make_managed<Gtk::Label>(T_("site_app_login_methods", "Login methods"));
  heading->add_css_class("ur-step-heading");
  heading->set_xalign(0);
  box->append(*heading);
  box->append(*MakeSizedLabel(label, 14, "ur-body"));

  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  actions->set_halign(Gtk::Align::END);
  auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
  cancel->signal_clicked().connect([this] { confirmDialog_->set_visible(false); });
  actions->append(*cancel);
  auto* remove = Gtk::make_managed<Gtk::Button>(T_("remove", "Remove"));
  remove->add_css_class("destructive-action");  // red IS the confirmation context
  remove->signal_clicked().connect([this, authType] {
    confirmDialog_->set_visible(false);
    RemoveAuth(authType);
  });
  actions->append(*remove);
  box->append(*actions);

  confirmDialog_->set_child(*box);
  // DEFAULT IS CANCEL: Enter must NOT remove a login method.
  cancel->set_receives_default(true);
  confirmDialog_->set_default_widget(*cancel);
  confirmDialog_->present();
}

void AccountPage::RemoveAuth(const std::string& authType) {
  // The confirmation dialog is ALREADY gone by the time this runs, so a silent
  // early return reads as "the server refused": a confirmed destructive action
  // must always end in a visible error or a refreshed list.
  if (removingAuth_) return;  // unreachable while the list repaints below
  if (!CanCallApi()) {
    Snack(T_("please_login_to_urnetwork", "Please login to URnetwork"), true);
    return;
  }
  removingAuth_ = true;
  // §7: a control disabled in flight. This list has no single button to
  // disable, so the LIST itself goes to its Loading render — which both shows
  // the flight and removes the second Remove button a slow link would
  // otherwise invite the user to press.
  methodsState_ = AccountFieldState::Loading;
  RenderAuthMethods();

  urnet::RemoveAuthArgs args{};
  args.auth_type = authType;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // 20 s. §3.2.2 reloads the network user EITHER WAY — a give-up is a way.
  const uint32_t flow = removeFlow_.Begin(kApiTimeoutMs, [this] {
    removingAuth_ = false;
    Snack(T_("something_went_wrong", "Something went wrong."), true);
    LoadAccount();
  });
  host_.api().removeAuth(
      std::optional<urnet::RemoveAuthArgs>(args),
      [this, epoch, seen, flow](std::optional<urnet::RemoveAuthResult> result,
                                std::optional<std::string> err) {
        PostToMain([this, epoch, seen, flow, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;
          if (!removeFlow_.Settle(flow, "remove login method")) return;
          removingAuth_ = false;
          const std::string message = FirstMessage(
              result && result->error ? result->error->message : std::string(), err);
          const bool ok = result.has_value() && !err.has_value() && !result->error;
          if (!ok) {
            g_warning("account: removeAuth failed: %s",
                      message.empty() ? "(no error text)" : message.c_str());
            Snack(message.empty()
                      ? Glib::ustring(T_("something_went_wrong", "Something went wrong."))
                      : Glib::ustring(message),
                  true);
          }
          // Reload EITHER WAY: the server owns the list, and a failed remove
          // must not leave the row it refused to delete looking gone.
          LoadAccount();
        });
      });
}

// ---- §3.2.5 the client id (a DEVICE read) ------------------------------------

// The window's DrawerEvent::DeviceLifecycle hook. NoDevice is a TRANSITIONAL
// state — "Attaching device controls…" is a promise that the row resolves —
// and the device attaches on a different event from the auth change that
// drives Load(). Without this the row keeps saying "attaching" minutes after
// the daemon attached and the tunnel is carrying traffic. Deliberately not
// Load(): re-reading the client id must not re-fire four API round trips or
// repaint the profile status line.
void AccountPage::RefreshClientId() {
  ApplyClientId();
  // The Extenders section's view controller is opened with the device, so the
  // DeviceLifecycle edge is exactly when its form stops being (or becomes) the
  // no-session state.
  if (extenderSection_) extenderSection_->Load();
}

void AccountPage::ApplyClientId() {
  clientId_ = host_.ClientId();  // "" with no device
  if (!clientId_.empty()) {
    ApplyFieldState(*clientIdValue_, AccountFieldState::Loaded, clientId_);
    kit::SetAccessibleLabel(*clientIdValue_,
                            Glib::ustring(T_("client_id", "Client ID")) + ", " + clientId_);
    clientIdCopy_->set_sensitive(true);
    return;
  }
  // signed in but the daemon is not attached is NOT "please login"
  ApplyFieldState(*clientIdValue_, host_.IsLoggedIn() && !previewMode_
                                       ? AccountFieldState::NoDevice
                                       : AccountFieldState::NoSession);
  clientIdCopy_->set_sensitive(false);
}

// ---- §3.3 referrals ----------------------------------------------------------

void AccountPage::ApplyReferralsRow(AccountFieldState state) {
  if (state == AccountFieldState::Loaded) {
    ApplyFieldState(*referralsRow_.value, state,
                    Format(T_("total_referrals_lld", "Total Referrals: {}"),
                           referralCount_.Total()));
  } else {
    ApplyFieldState(*referralsRow_.value, state);
  }
  kit::SetAccessibleLabel(*referralsRow_.root, Glib::ustring(T_("referrals", "Referrals")) +
                                                   ", " + referralsRow_.value->get_text());
}

void AccountPage::LoadReferralInfo() {
  referralFlow_.Abandon();
  if (!CanCallApi()) {
    referralCode_.clear();
    referralCount_.Reset();
    ApplyReferralsRow(AccountFieldState::NoSession);
    ApplyBalance(balance_);  // pane A's referral rows follow the same figure
    return;
  }
  ApplyReferralsRow(AccountFieldState::Loading);

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // 20 s: the Referrals row and pane A's referral lines both hang off this one.
  const uint32_t flow = referralFlow_.Begin(kApiTimeoutMs, [this] {
    referralCode_.clear();
    referralCount_.Fail();
    ApplyReferralsRow(AccountFieldState::Failed);
    ApplyBalance(balance_);
  });
  host_.api().getNetworkReferralCode(
      [this, epoch, seen, flow](std::optional<urnet::GetNetworkReferralCodeResult> result,
                                std::optional<std::string> err) {
        PostToMain([this, epoch, seen, flow, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;
          if (!referralFlow_.Settle(flow, "referral code load")) return;
          if (!result || err || result->error) {
            g_warning("account: getNetworkReferralCode failed: %s",
                      err ? err->c_str()
                          : (result && result->error ? result->error->message.c_str()
                                                     : "(no result)"));
            referralCode_.clear();
            referralCount_.Fail();  // a count already shown stays
            ApplyReferralsRow(AccountFieldState::Failed);
            ApplyBalance(balance_);
            return;
          }
          referralCode_ = result->referral_code.value_or(std::string());
          referralCount_.Succeed(result->total_referrals);
          // §3.3.4: a successful read gets exactly ONE render, the count —
          // a network with referrals but no code still earned them.
          ApplyReferralsRow(AccountFieldState::Loaded);
          // the referral load repaints pane A's two referral lines
          ApplyBalance(balance_);
        });
      });
}

void AccountPage::RetryReferralInfo() {
  referralCount_.Retry();
  ApplyBalance(balance_);
  LoadReferralInfo();
}

// ---- sheets ------------------------------------------------------------------

Gtk::Window* AccountPage::RootWindow() { return dynamic_cast<Gtk::Window*>(get_root()); }

bool AccountPage::BeginSheet(const char* what) {
  if (sheetShowing_ || (sheet_open && sheet_open())) {
    // a click that opens nothing stays a mystery: logged, never silent
    g_message("account: %s suppressed — a modal is already open", what);
    return false;
  }
  sheetShowing_ = true;
  if (on_sheet_open_changed) on_sheet_open_changed(true);
  return true;
}

void AccountPage::EndSheet() {
  if (!sheetShowing_) return;
  sheetShowing_ = false;
  if (on_sheet_open_changed) on_sheet_open_changed(false);
}

void AccountPage::WireSheet(Gtk::Window& sheet) {
  auto alive = alive_;
  sheet.signal_hide().connect([this, alive] {
    // never tear anything down inside the widget's own signal
    PostToMain([this, alive] {
      if (!*alive) return;  // the page is gone; nothing to clear
      EndSheet();
    });
  });
}

void AccountPage::ShowAddAuthSheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("account: no window root; the add-login-method sheet was not opened");
    return;
  }
  if (!BeginSheet("add login method")) return;
  if (!addAuthSheet_) {
    // The sheet asks the PAGE whether a server touch is allowed, every time —
    // preview mode is a page-level switch that can flip after construction.
    // The page owns the sheet, so capturing `this` cannot outlive it.
    addAuthSheet_ =
        std::make_unique<AccountAddAuthSheet>(*root, host_, [this] { return CanCallApi(); });
    addAuthSheet_->on_changed = [this] { LoadAccount(); };  // the new method appears
    WireSheet(*addAuthSheet_);
  }
  addAuthSheet_->Open();
}

void AccountPage::ShowAuthCodeSheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("account: no window root; the auth-code sheet was not opened");
    return;
  }
  if (!BeginSheet("auth code")) return;
  if (!authCodeSheet_) {
    authCodeSheet_ = std::make_unique<AccountAuthCodeSheet>(
        *root, host_, [this] { return CanCallApi(); });
    WireSheet(*authCodeSheet_);
  }
  authCodeSheet_->Open();
}

void AccountPage::ShowExtenderShareSheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("account: no window root; the share-extenders sheet was not opened");
    return;
  }
  if (!BeginSheet("share extenders")) return;
  // Rebuilt each time rather than cached: the payload is a snapshot of the
  // directory, and a sheet that reopened on a stale one would hand out
  // addresses this device no longer believes in.
  extenderShareSheet_ = std::make_unique<ExtenderShareSheet>(*root, host_);
  extenderShareSheet_->on_message = [this](const Glib::ustring& message, bool error) {
    Snack(message, error);
  };
  WireSheet(*extenderShareSheet_);
  extenderShareSheet_->set_visible(true);
}

void AccountPage::ShowExtenderImportSheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("account: no window root; the import-extenders sheet was not opened");
    return;
  }
  if (!BeginSheet("import extenders")) return;
  extenderImportSheet_ = std::make_unique<ExtenderImportSheet>(*root, host_);
  extenderImportSheet_->on_message = [this](const Glib::ustring& message, bool error) {
    Snack(message, error);
  };
  // an import carrying settings replaces the dns name, gossip url and root
  // keys, so the form behind the sheet is stale the moment one lands
  extenderImportSheet_->on_imported = [this] {
    if (extenderSection_) extenderSection_->Load();
  };
  WireSheet(*extenderImportSheet_);
  extenderImportSheet_->set_visible(true);
}

void AccountPage::ConfirmResetExtenders() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("account: no window root; the reset extenders confirmation did not open");
    return;
  }
  if (confirmDialog_ && confirmDialog_->get_visible()) return;  // one modal at a time
  if (!BeginSheet("reset extenders")) return;

  // The page's modal confirm, as for removing a login method: a reset removes
  // the extenders the user added and clears everything learned, and nothing
  // brings either back.
  confirmDialog_ = std::make_unique<Gtk::Window>();
  confirmDialog_->set_transient_for(*root);
  confirmDialog_->set_modal(true);
  confirmDialog_->set_title(T_("reset_extenders", "Reset extenders"));
  confirmDialog_->set_resizable(false);
  confirmDialog_->set_hide_on_close(true);
  confirmDialog_->add_css_class("ur-sheet");
  AddEscapeToClose(*confirmDialog_);
  WireSheet(*confirmDialog_);

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  box->set_size_request(kConfirmWidth, -1);

  auto* heading = Gtk::make_managed<Gtk::Label>(T_("reset_extenders", "Reset extenders"));
  heading->add_css_class("ur-step-heading");
  heading->set_xalign(0);
  box->append(*heading);
  box->append(*MakeSizedLabel(
      T_("reset_extenders_confirm",
         "This removes the extenders you added and clears everything learned about extenders, "
         "which are then discovered again from scratch."),
      14, "ur-body"));

  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  actions->set_halign(Gtk::Align::END);
  auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
  cancel->signal_clicked().connect([this] { confirmDialog_->set_visible(false); });
  actions->append(*cancel);
  auto* reset = Gtk::make_managed<Gtk::Button>(T_("reset_extenders", "Reset extenders"));
  reset->add_css_class("destructive-action");  // red is the confirmation context
  reset->signal_clicked().connect([this] {
    confirmDialog_->set_visible(false);
    if (extenderSection_) extenderSection_->ResetExtenders();
  });
  actions->append(*reset);
  box->append(*actions);

  confirmDialog_->set_child(*box);
  // the default is Cancel: Enter must not reset
  cancel->set_receives_default(true);
  confirmDialog_->set_default_widget(*cancel);
  confirmDialog_->present();
}

void AccountPage::ShowDeleteAccountSheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("account: no window root; the delete-account sheet was not opened");
    return;
  }
  if (!BeginSheet("delete account")) return;
  if (!deleteSheet_) {
    deleteSheet_ = std::make_unique<AccountDeleteSheet>(
        *root, host_, [this] { return CanCallApi(); });
    deleteSheet_->on_deleted = [this](const auth_logout::Report& signIn) {
      // the session is meaningless once the network is gone, and only the
      // deleted network's sign-in is signed out (android signOutApplies)
      host_.LogoutIfStill(signIn);
    };
    WireSheet(*deleteSheet_);
  }
  deleteSheet_->Open();
}

}  // namespace urnw
