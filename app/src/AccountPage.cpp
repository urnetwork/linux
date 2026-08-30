// SPDX-License-Identifier: MPL-2.0
#include "AccountPage.hpp"

#include "ReferralRoyalty.hpp"

#include <glib.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "PaneKit.hpp"
#include "RuntimePaths.hpp"
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
constexpr int kSheetWidth = 400;         // referral / delete sheets (MinWidth 400)
constexpr int kSheetWidthNarrow = 380;   // add-auth / auth-code sheets
constexpr int kConfirmWidth = 320;       // the remove-method confirm (MinWidth 320)
constexpr int kMinPasswordLength = 12;   // AddAuthSheet gate
constexpr int kMinReferralCodeLength = 6;  // ReferralNetworkSheet gate
constexpr double kAuthCodeMinutes = 5.0;   // matches the caption
constexpr int64_t kAuthCodeUses = 1;
constexpr int kAuthCodeAbbreviateAbove = 14;  // longer than this -> 6…6
constexpr int kAuthCodeShoulder = 6;
constexpr int kMaskShoulder = 3;      // balance-code secret: first 3 + last 3
constexpr int kIsoDatePrefix = 10;    // YYYY-MM-DD
constexpr int kFrogPx = 40;
constexpr int kValueChars = 22;       // client id / bonus code readouts (MaxWidth 220)
constexpr int kReferralValueChars = 24;  // referral-network value (MaxWidth 240)

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

// The two-line row with an action verb on the right. GTK carries no separate
// FullDescription channel, so the button announces as "verb. row label".
Gtk::Button* AddButtonRow(Gtk::Box& host, const Glib::ustring& label,
                          const Glib::ustring& note, const Glib::ustring& action) {
  auto row = kit::MakePaneTwoLineRow(label, note, kRowTall);
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
                                 const Glib::ustring& action) {
  ValueActionRow out;
  auto row = kit::MakePaneTwoLineRow(label, {}, kRowTall);
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

// The frog mascot beside "You're referral royalty!". Resolved through the
// RuntimePaths ladder; a miss hides the image and keeps the line. (The asset
// IS in meson's install_data set now, so installed builds resolve it too —
// the old flag about a missing install entry is obsolete.)
std::string ReferralFrogPath() {
  static const std::string path = ResolveRuntimePath(
      UR_PKGDATADIR "/ReferralFrog.png", G_FILE_TEST_IS_REGULAR, "assets/ReferralFrog.png");
  return path;
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
class AccountAddAuthSheet : public Gtk::Window {
 public:
  AccountAddAuthSheet(Gtk::Window& parent, SdkHost& host, std::function<bool()> canAct)
      : host_(host), canAct_(std::move(canAct)) {
    set_transient_for(parent);
    set_modal(true);
    set_title(T_("site_app_login_methods", "Login methods"));
    set_default_size(kSheetWidthNarrow, -1);
    set_resizable(false);
    set_hide_on_close(true);
    add_css_class("ur-sheet");  // sheets sit ABOVE the page: #151515
    AddEscapeToClose(*this);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);
    box->set_size_request(kSheetWidthNarrow, -1);

    auto* heading =
        Gtk::make_managed<Gtk::Label>(T_("site_app_login_methods", "Login methods"));
    heading->add_css_class("ur-step-heading");
    heading->set_xalign(0);
    box->append(*heading);

    auto* authLabel = Gtk::make_managed<Gtk::Label>(T_("your_email", "Your email"));
    authLabel->add_css_class("ur-input-label");
    authLabel->set_xalign(0);
    box->append(*authLabel);
    auth_ = Gtk::make_managed<Gtk::Entry>();
    auth_->add_css_class("ur-input");
    kit::SetAccessibleLabel(*auth_, T_("your_email", "Your email"));
    auth_->signal_changed().connect([this] { Validate(); });
    box->append(*auth_);

    auto* passwordLabel = Gtk::make_managed<Gtk::Label>(T_("password_label", "Password"));
    passwordLabel->add_css_class("ur-input-label");
    passwordLabel->set_xalign(0);
    box->append(*passwordLabel);
    password_ = Gtk::make_managed<Gtk::PasswordEntry>();
    password_->add_css_class("ur-input");
    password_->set_show_peek_icon(true);
    kit::SetAccessibleLabel(*password_, T_("password_label", "Password"));
    password_->signal_changed().connect([this] { Validate(); });
    box->append(*password_);

    auto* rule = Gtk::make_managed<Gtk::Label>(
        T_("password_must_be_at_least_12_characters_long",
           "Password must be at least 12 characters long"));
    rule->add_css_class("ur-caption");
    rule->set_xalign(0);
    rule->set_wrap(true);
    box->append(*rule);

    error_ = MakeSizedLabel({}, 12, "ur-caption");
    error_->set_visible(false);
    box->append(*error_);

    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    actions->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
    cancel->signal_clicked().connect([this] { set_visible(false); });
    actions->append(*cancel);
    add_ = Gtk::make_managed<Gtk::Button>(T_("add", "Add"));
    add_->add_css_class("suggested-action");  // the dialog-primary role
    add_->set_sensitive(false);
    add_->signal_clicked().connect([this] { Submit(); });
    actions->append(*add_);
    box->append(*actions);

    set_child(*box);
    // DefaultButton = Primary here (spec): this sheet ADDS, it never destroys.
    add_->set_receives_default(true);
    set_default_widget(*add_);
  }

  ~AccountAddAuthSheet() override { ++*epoch_; }  // orphan every in-flight submit

  // The page reloads the network user so the new method appears in the list.
  std::function<void()> on_changed;

  void Open() {
    ++*epoch_;
    flow_.Abandon();  // a previous submit's watchdog may still be armed
    submitting_ = false;
    auth_->set_text("");
    password_->set_text("");
    error_->set_visible(false);
    const bool session = CanAct();
    auth_->set_sensitive(session);
    password_->set_sensitive(session);
    if (!session) {
      // never a disabled field with no explanation
      ApplyFieldState(*error_, AccountFieldState::NoSession);
      error_->set_visible(true);
    }
    Validate();
    present();
  }

 private:
  bool CanAct() const { return canAct_ && canAct_(); }

  void Validate() {
    const bool session = CanAct();
    const std::string auth = TrimSpace(auth_->get_text().raw());
    const std::string password = password_->get_text().raw();
    add_->set_sensitive(!submitting_ && session && !auth.empty() &&
                        password.size() >= static_cast<size_t>(kMinPasswordLength));
  }

  void ShowError(const Glib::ustring& message) {
    SetToned(*error_, kUrDanger, message);
    error_->set_visible(true);
  }

  void Submit() {
    if (submitting_ || !CanAct()) return;
    const std::string auth = TrimSpace(auth_->get_text().raw());
    const std::string password = password_->get_text().raw();
    if (auth.empty() || password.size() < static_cast<size_t>(kMinPasswordLength)) return;

    submitting_ = true;
    add_->set_sensitive(false);
    error_->set_visible(false);

    urnet::AddAuthArgs args{};
    args.user_auth = auth;
    args.password = password;

    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    // 20 s: an answer that never comes must not leave Add greyed out forever
    // with no line of explanation under it.
    const uint32_t flow = flow_.Begin(kApiTimeoutMs, [this] {
      submitting_ = false;
      ShowError(T_("something_went_wrong", "Something went wrong."));
      Validate();
    });
    host_.api().addAuth(
        std::optional<urnet::AddAuthArgs>(args),
        [this, epoch, seen, flow](std::optional<urnet::AddAuthResult> result,
                                  std::optional<std::string> err) {
          PostToMain([this, epoch, seen, flow, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;  // a newer Open() (or teardown) owns it
            if (!flow_.Settle(flow, "add login method")) return;
            submitting_ = false;
            const std::string message = FirstMessage(
                result && result->error ? result->error->message : std::string(), err);
            const bool ok = result.has_value() && !err.has_value() && !result->error;
            if (!ok) {
              g_warning("account: addAuth failed: %s",
                        message.empty() ? "(no error text)" : message.c_str());
              ShowError(message.empty()
                            ? Glib::ustring(T_("something_went_wrong", "Something went wrong."))
                            : Glib::ustring(message));
              Validate();  // re-enable per the gate, never blindly
              return;
            }
            if (on_changed) on_changed();
            set_visible(false);
          });
        });
  }

  SdkHost& host_;
  std::function<bool()> canAct_;  // the PAGE's gate: preview first, then session
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);
  AccountFlow flow_;
  Gtk::Entry* auth_ = nullptr;
  Gtk::PasswordEntry* password_ = nullptr;
  Gtk::Label* error_ = nullptr;
  Gtk::Button* add_ = nullptr;
  bool submitting_ = false;
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
// §3.3.3 ReferralNetworkSheet — set or unlink the referral network
// =============================================================================
// The unlink is a TWO-STEP ON DIFFERENT CONTROLS because a dialog cannot open
// a second dialog: the inline button ARMS (and the error line states the
// consequence), the sheet's own primary COMMITS, and Enter is bound to Close
// throughout.
class AccountReferralNetworkSheet : public Gtk::Window {
 public:
  AccountReferralNetworkSheet(Gtk::Window& parent, SdkHost& host,
                              std::function<bool()> canAct)
      : host_(host), canAct_(std::move(canAct)) {
    set_transient_for(parent);
    set_modal(true);
    set_title(T_("update_referral_network", "Update referral network"));
    set_default_size(kSheetWidth, -1);
    set_resizable(false);
    set_hide_on_close(true);
    add_css_class("ur-sheet");
    AddEscapeToClose(*this);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);
    box->set_size_request(kSheetWidth, -1);

    auto* heading = Gtk::make_managed<Gtk::Label>(
        T_("update_referral_network", "Update referral network"));
    heading->add_css_class("ur-step-heading");
    heading->set_xalign(0);
    box->append(*heading);

    auto* currentLabel = Gtk::make_managed<Gtk::Label>(
        T_("current_referral_network", "Current referral network"));
    currentLabel->add_css_class("ur-caption");
    currentLabel->set_xalign(0);
    box->append(*currentLabel);
    current_ = MakeSizedLabel({}, 14, "ur-value");
    ApplyFieldState(*current_, AccountFieldState::Loading);
    box->append(*current_);

    auto* codeLabel = Gtk::make_managed<Gtk::Label>(
        T_("enter_network_referral_code", "Enter network referral code"));
    codeLabel->add_css_class("ur-input-label");
    codeLabel->set_xalign(0);
    box->append(*codeLabel);
    code_ = Gtk::make_managed<Gtk::Entry>();
    code_->add_css_class("ur-input");
    kit::SetAccessibleLabel(
        *code_, T_("enter_network_referral_code", "Enter network referral code"));
    code_->signal_changed().connect([this] { Gate(); });
    box->append(*code_);

    unlink_ = Gtk::make_managed<Gtk::Button>(
        T_("unlink_referral_network", "Unlink referral network"));
    unlink_->add_css_class("destructive-action");
    unlink_->set_halign(Gtk::Align::START);
    unlink_->set_visible(false);  // never offer to unlink a guess
    unlink_->signal_clicked().connect([this] { Arm(); });
    box->append(*unlink_);

    error_ = MakeSizedLabel({}, 12, "ur-caption");
    error_->set_visible(false);
    box->append(*error_);

    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    actions->set_halign(Gtk::Align::END);
    close_ = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
    close_->signal_clicked().connect([this] { set_visible(false); });
    actions->append(*close_);
    primary_ = Gtk::make_managed<Gtk::Button>(T_("update", "Update"));
    primary_->add_css_class("suggested-action");
    primary_->set_sensitive(false);
    primary_->signal_clicked().connect([this] {
      if (armed_) {
        CommitUnlink();
      } else {
        Update();
      }
    });
    actions->append(*primary_);
    box->append(*actions);

    set_child(*box);
    // DefaultButton = Close: Enter must never commit an unlink.
    close_->set_receives_default(true);
    set_default_widget(*close_);
  }

  ~AccountReferralNetworkSheet() override { ++*epoch_; }

  // The page's referral-network row re-reads after any change.
  std::function<void()> on_changed;
  // fired after a successful link: the caller shows the royal welcome
  std::function<void()> on_royal;

  void Open() {
    ++*epoch_;
    loadFlow_.Abandon();
    actionFlow_.Abandon();
    busy_ = false;
    Disarm();
    code_->set_text("");
    error_->set_visible(false);
    LoadCurrent();
    present();
  }

 private:
  bool CanAct() const { return canAct_ && canAct_(); }

  void ApplyCurrent(AccountFieldState state, const std::string& name) {
    state_ = state;
    name_ = name;
    ApplyFieldState(*current_, state, name);
    // Unlink is offered ONLY against a loaded, non-empty name.
    unlink_->set_visible(state == AccountFieldState::Loaded && !name.empty());
    Gate();
  }

  void LoadCurrent() {
    loadFlow_.Abandon();
    const bool session = CanAct();
    code_->set_sensitive(session);
    if (!session) {
      ApplyCurrent(AccountFieldState::NoSession, {});
      return;
    }
    ApplyCurrent(AccountFieldState::Loading, {});
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    // 20 s: the current line may not sit on "Loading..." forever — the Unlink
    // button's whole gate hangs off it.
    const uint32_t flow = loadFlow_.Begin(
        kApiTimeoutMs, [this] { ApplyCurrent(AccountFieldState::Failed, {}); });
    host_.api().getReferralNetwork(
        [this, epoch, seen, flow](std::optional<urnet::GetReferralNetworkResult> result,
                                  std::optional<std::string> err) {
          PostToMain([this, epoch, seen, flow, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (!loadFlow_.Settle(flow, "referral network read")) return;
            // CRITICAL semantics (§3.3.3): the server answers "no referral
            // network found" on the ERROR channel of a lookup that SUCCEEDED.
            // Only a transport failure or a missing result is Failed.
            if (!result || err) {
              g_warning("account: getReferralNetwork failed: %s",
                        err ? err->c_str() : "(no result)");
              ApplyCurrent(AccountFieldState::Failed, {});
              return;
            }
            const std::string name = result->network ? result->network->name : std::string();
            ApplyCurrent(name.empty() ? AccountFieldState::Empty : AccountFieldState::Loaded,
                         name);
          });
        });
  }

  void Gate() {
    if (armed_) {
      primary_->set_sensitive(!busy_);
      return;
    }
    const std::string code = TrimSpace(code_->get_text().raw());
    primary_->set_sensitive(!busy_ && CanAct() &&
                            code.size() >= static_cast<size_t>(kMinReferralCodeLength));
  }

  void ShowError(const Glib::ustring& message) {
    SetToned(*error_, kUrDanger, message);
    error_->set_visible(true);
  }

  void Arm() {
    armed_ = true;
    ShowError(Format(T_("when_unlinking_your_referral_network_you_will_no",
                        "When unlinking your referral network, you will no longer be able "
                        "to earn points from {}."),
                     name_));
    code_->set_sensitive(false);
    unlink_->set_sensitive(false);
    primary_->set_label(T_("unlink_referral_network", "Unlink referral network"));
    Gate();
  }

  void Disarm() {
    armed_ = false;
    code_->set_sensitive(CanAct());
    unlink_->set_sensitive(true);
    primary_->set_label(T_("update", "Update"));
    Gate();
  }

  void Update() {
    if (busy_ || !CanAct()) return;
    const std::string code = TrimSpace(code_->get_text().raw());
    if (code.size() < static_cast<size_t>(kMinReferralCodeLength)) return;
    busy_ = true;
    Gate();
    error_->set_visible(false);

    urnet::SetNetworkReferralArgs args{};
    args.referral_code = code;
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    // 20 s. The timeout line is NOT "Invalid referral code": the server never
    // answered, so nothing was decided about the code — saying it was rejected
    // would be a verdict the client made up.
    const uint32_t flow = actionFlow_.Begin(kApiTimeoutMs, [this] {
      busy_ = false;
      ShowError(T_("something_went_wrong", "Something went wrong."));
      Gate();
    });
    host_.api().setNetworkReferral(
        std::optional<urnet::SetNetworkReferralArgs>(args),
        [this, epoch, seen, flow](std::optional<urnet::SetNetworkReferralResult> result,
                                  std::optional<std::string> err) {
          PostToMain([this, epoch, seen, flow, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (!actionFlow_.Settle(flow, "referral network update")) return;
            busy_ = false;
            const bool ok = result.has_value() && !err.has_value() && !result->error;
            if (!ok) {
              g_warning("account: setNetworkReferral failed: %s",
                        err ? err->c_str()
                            : (result && result->error ? result->error->message.c_str()
                                                       : "(no result)"));
              ShowError(T_("invalid_referral_code_please_try_again",
                           "Invalid referral code. Please try again."));
              Gate();
              return;
            }
            code_->set_text("");
            LoadCurrent();
            if (on_changed) on_changed();
            // linking a referral network is the royal-welcome moment
            close();
            if (on_royal) on_royal();
          });
        });
  }

  void CommitUnlink() {
    if (busy_ || !CanAct()) return;
    busy_ = true;
    Gate();
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    const uint32_t flow = actionFlow_.Begin(kApiTimeoutMs, [this] {
      busy_ = false;
      Disarm();  // back to the normal shape either way
      ShowError(T_("something_went_wrong", "Something went wrong."));
    });
    host_.api().unlinkReferralNetwork(
        [this, epoch, seen, flow](std::optional<urnet::UnlinkReferralNetworkResult> result,
                                  std::optional<std::string> err) {
          PostToMain([this, epoch, seen, flow, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (!actionFlow_.Settle(flow, "referral network unlink")) return;
            busy_ = false;
            // No error field: success is strictly "a result AND no transport
            // error" — anything else is a failure, never a silent success.
            const bool ok = result.has_value() && !err.has_value();
            Disarm();  // either way, back to the normal shape
            if (!ok) {
              g_warning("account: unlinkReferralNetwork failed: %s",
                        err ? err->c_str() : "(no result)");
              ShowError(err && !err->empty()
                            ? Glib::ustring(*err)
                            : Glib::ustring(T_("something_went_wrong",
                                               "Something went wrong.")));
              return;
            }
            error_->set_visible(false);
            LoadCurrent();
            if (on_changed) on_changed();
          });
        });
  }

  SdkHost& host_;
  std::function<bool()> canAct_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);
  AccountFlow loadFlow_;    // the current-network read
  AccountFlow actionFlow_;  // update / unlink (never both at once)
  Gtk::Label* current_ = nullptr;
  Gtk::Entry* code_ = nullptr;
  Gtk::Button* unlink_ = nullptr;
  Gtk::Label* error_ = nullptr;
  Gtk::Button* primary_ = nullptr;
  Gtk::Button* close_ = nullptr;
  AccountFieldState state_ = AccountFieldState::Loading;
  std::string name_;
  bool busy_ = false;
  bool armed_ = false;
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

  // The page signs out: the session is meaningless once the network is gone.
  std::function<void()> on_deleted;

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
    host_.api().networkDelete(
        [this, epoch, seen, flow](std::optional<urnet::NetworkDeleteResult> result,
                                  std::optional<std::string> err) {
          PostToMain([this, epoch, seen, flow, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (!deleteFlow_.Settle(flow, "network delete")) return;
            deleting_ = false;
            // No error field: success is a result AND no transport error.
            const bool ok = result.has_value() && !err.has_value();
            if (!ok) {
              g_warning("account: networkDelete failed: %s",
                        err ? err->c_str() : "(no result)");
              SetToned(*error_, kUrDanger,
                       err && !err->empty()
                           ? Glib::ustring(*err)
                           : Glib::ustring(T_("error_deleting_account",
                                              "Sorry, there was an error deleting your "
                                              "account.")));
              error_->set_visible(true);
              Gate();
              return;
            }
            set_visible(false);
            if (on_deleted) on_deleted();
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
  referralNetworkFlow_.Abandon();
  nameFlow_.Abandon();
  resetFlow_.Abandon();
  portalFlow_.Abandon();
  removeFlow_.Abandon();
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
  // no round trip.
  ApplyClientId();

  // Each load gates itself on the session and settles its own panel on
  // NoSession WITHOUT a request — a 401 must never arrive to be mistaken for
  // an empty account, and nothing may sit on a spinner.
  LoadAccount();
  LoadReferralInfo();
  LoadReferralNetwork();
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
  referralNetworkFlow_.Abandon();
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
  hide(referralSheet_.get());
  hide(deleteSheet_.get());
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
  totalReferrals_ = 0;
  authMethods_.clear();
  codes_.clear();

  CloseNameEditor();
  nameBox_->set_text("");
  ApplyNetworkName({});
  ApplyAuthLine();
  ApplyAccountState(AccountFieldState::NoSession);

  methodsState_ = AccountFieldState::NoSession;
  RenderAuthMethods();
  ApplyReferralCode(AccountFieldState::NoSession);
  ApplyReferralSummary(AccountFieldState::NoSession);
  ApplyReferralNetworkValue(AccountFieldState::NoSession, {});
  royaltyBadge_->set_visible(false);
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

  // 5. the daily figure. AccountBalance::loaded is HasFetched, and this is the
  //    one place it earns its keep: before any snapshot has landed there is no
  //    daily balance to report, and printing "0 B" asserts a figure the server
  //    never gave (§0.4 — an async field renders its STATE, never a fabricated
  //    value). The plan value above deliberately still seeds "Free": the spec
  //    says so in as many words.
  if (balance_.loaded) {
    const Glib::ustring daily = FormatByteCountCompact(balance_.startBalanceByteCount);
    SetToned(*dailyValue_, kOffWhite, daily);
    kit::SetAccessibleLabel(
        *dailyValue_,
        Glib::ustring(T_("daily_data_balance_label", "Daily Data Balance:")) + ", " + daily);
  } else {
    ApplyFieldState(*dailyValue_, CanCallApi() ? AccountFieldState::Loading
                                               : AccountFieldState::NoSession);
    kit::SetAccessibleLabel(
        *dailyValue_,
        Glib::ustring(T_("daily_data_balance_label", "Daily Data Balance:")) + ", " +
            dailyValue_->get_text());
  }

  // 6. the referral pair (repainted whenever the referral load lands, which
  //    calls back through here). These two DO start at zero by spec: pane A
  //    reads AccountPage::totalReferrals(), "0 until the referral load lands".
  referralTotals_->set_text(
      Format(T_("total_referrals_lld", "Total Referrals: {}"), totalReferrals_));
  const Glib::ustring bonus =
      Format(T_("referral_bonus", "+{} GiB/Day"), totalReferrals_ * kReferralGiBPerDay);
  SetToned(*referralBonus_, kOffWhite, bonus);
  kit::SetAccessibleLabel(*referralBonus_, referralTotals_->get_text() + ", " + bonus);
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

  // 4. the data-usage group.
  content->append(*kit::MakePaneGroupHeader(T_("data_usage", "Data usage")).root);

  // 5. daily balance.
  {
    auto row = kit::MakePaneKeyValueRow(
        T_("daily_data_balance_label", "Daily Data Balance:"), {}, kRowKeyValue);
    dailyValue_ = row.value;
    content->append(*row.root);
  }

  // 6. referrals (both halves repainted by ApplyBalance).
  {
    auto row = kit::MakePaneKeyValueRow({}, {}, kRowKeyValue);
    referralTotals_ = row.key;
    referralBonus_ = row.value;
    content->append(*row.root);
  }

  // 7. Redeem Balance Code -> the existing RedeemCodeSheet.
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

  // 8. Manage Subscription (the Stripe customer portal).
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
  BuildReferralGroup(*paneB_.content);
  BuildDangerGroup(*paneB_.content);
}

void AccountPage::BuildProfileGroup(Gtk::Box& host) {
  host.append(*kit::MakePaneGroupHeader(T_("profile", "Profile")).root);

  // 1. the network-name VIEW row. Exactly one of {row, edit panel} shows.
  nameRow_ = kit::MakePaneTwoLineRowButton(T_("network_name_label", "Network name"), {},
                                           kRowTall);
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
  //    fixed-height row is a 38px hole).
  {
    auto* row = kit::MakePaneRow(kRowAuthLine);
    authText_ = Gtk::make_managed<Gtk::Label>();
    authText_->add_css_class("ur-key");
    authText_->set_xalign(0);
    authText_->set_hexpand(true);
    authText_->set_ellipsize(Pango::EllipsizeMode::END);
    if (auto* inner = dynamic_cast<Gtk::Box*>(row->get_first_child())) {
      inner->append(*authText_);
    }
    authRow_ = row;
    authRow_->set_visible(false);
    host.append(*row);
  }

  // 4. the status line: the account load state, the save verdict, or the
  //    password-reset outcome — one line, three writers, one voice.
  {
    auto prose = MakeProseRow({}, kStatePadY);
    statusLine_ = prose.line;
    host.append(*prose.root);
  }

  // 5. update password.
  sendResetButton_ =
      AddButtonRow(host, T_("update_password", "Update password"), {}, T_("send", "Send"));
  sendResetButton_->set_sensitive(false);
  sendResetButton_->signal_clicked().connect([this] { SendPasswordReset(); });
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
                           T_("add", "Add"));
  add->signal_clicked().connect([this] { ShowAddAuthSheet(); });

  // 3. auth code.
  auto* create = AddButtonRow(
      host, T_("auth_code", "Auth code"),
      T_("created_auth_codes_expire_after_5_minutes",
         "Created auth codes expire after 5 minutes"),
      T_("site_app_create_auth_code", "Create auth code"));
  create->signal_clicked().connect([this] { ShowAuthCodeSheet(); });

  // 4. client id — off the DEVICE, not the api.
  {
    auto row = AddValueActionRow(host, T_("client_id", "Client ID"), T_("copy", "Copy"));
    clientIdValue_ = row.value;
    clientIdCopy_ = row.action;
    clientIdCopy_->signal_clicked().connect([this] {
      CopyFull(clientId_,
               T_("client_id_copied_to_clipboard", "Client ID copied to clipboard"));
    });
  }
}

void AccountPage::BuildReferralGroup(Gtk::Box& host) {
  host.append(*kit::MakePaneGroupHeader(T_("referrals", "Referrals")).root);

  // 1. the bonus referral code.
  {
    auto row = AddValueActionRow(host, T_("bonus_referral_code_label", "Bonus referral code"),
                                 T_("copy", "Copy"));
    bonusCodeValue_ = row.value;
    bonusCodeCopy_ = row.action;
    bonusCodeCopy_->signal_clicked().connect([this] {
      CopyFull(referralCode_, T_("bonus_referral_code_copied_to_clipboard",
                                 "Bonus referral code copied to clipboard"));
    });
  }

  // 2. the referral network (opens the sheet).
  referralNetworkRow_ =
      kit::MakePaneTwoLineRowButton(T_("referral_network", "Referral network"), {}, kRowTall);
  referralNetworkRow_.value->set_max_width_chars(kReferralValueChars);
  referralNetworkRow_.root->signal_clicked().connect([this] { ShowReferralNetworkSheet(); });
  host.append(*referralNetworkRow_.root);

  // 2b. refer friends: opens the gold king-frog refer panel (parity with the
  // account rows on android/apple).
  {
    auto referRow =
        kit::MakePaneTwoLineRowButton(T_("refer_and_earn", "Refer and earn"), {}, kRowTall);
    referRow.root->signal_clicked().connect([this] {
      Gtk::Window* root = RootWindow();
      if (root == nullptr) return;
      ShowReferSheet(*root, totalReferrals_, referralCode_);
    });
    host.append(*referRow.root);
  }

  // 3. the referral summary (wraps; no trimming).
  {
    auto prose = MakeProseRow({}, kProsePadY);
    prose.line->remove_css_class("ur-caption");
    prose.line->add_css_class("ur-key");  // 13px muted, the key voice
    referralSummary_ = prose.line;
    host.append(*prose.root);
  }

  // 4. the royalty badge — visible iff totalReferrals > 0.
  {
    auto row = MakePaddedRow(kProsePadY);
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    const std::string frog = ReferralFrogPath();
    if (!frog.empty()) {
      auto* image = Gtk::make_managed<Gtk::Image>();
      image->set(frog);
      image->set_pixel_size(kFrogPx);
      kit::MarkDecorative(*image);  // the crowned frog mascot says nothing new
      line->append(*image);
    } else {
      // resolver miss (unusual — the asset is installed): the badge keeps its
      // sentence rather than reserving a hole for a picture that never arrives
      g_message("account: ReferralFrog.png not found; the royalty badge runs text-only");
    }
    auto* text = Gtk::make_managed<Gtk::Label>(
        T_("referral_royalty", "You're referral royalty!"));
    text->add_css_class("ur-key");
    text->set_xalign(0);
    text->set_wrap(true);
    text->set_valign(Gtk::Align::CENTER);
    line->append(*text);
    row.content->append(*line);
    royaltyBadge_ = row.root;
    royaltyBadge_->set_visible(false);
    host.append(*row.root);
  }
}

void AccountPage::BuildDangerGroup(Gtk::Box& host) {
  // The windows heading here is `account` again — the danger rows live under
  // the account's own name, not under a "Danger" word the store lacks.
  host.append(*kit::MakePaneGroupHeader(T_("account", "Account")).root);

  // Whole-row buttons with chevrons — never "Sign out [Sign out]".
  {
    auto row = kit::MakePaneTwoLineRowButton(T_("sign_out", "Sign out"), {}, kRowTall);
    row.root->signal_clicked().connect([this] {
      // no confirm: signing out costs nothing and is instantly reversible
      host_.Logout();
    });
    host.append(*row.root);
  }
  {
    // NOT red here: red belongs to the confirmation context.
    auto row =
        kit::MakePaneTwoLineRowButton(T_("delete_account_2", "Delete account"), {}, kRowTall);
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
  sendResetButton_->set_sensitive(loaded && !userAuth_.empty() && !sendingReset_);
  // if the state leaves Loaded while the editor is open, the editor is forced
  // closed — an editor over a field nobody can save is a trap
  if (!loaded && editingName_) CloseNameEditor();
}

void AccountPage::ApplyNetworkName(const std::string& name) {
  acknowledgedName_ = name;  // the server-acknowledged name; the box is never truth
  kit::SetTextOrCollapse(*nameRow_.value, name);
  if (!name.empty()) SetToned(*nameRow_.value, kUrTextMuted, name);
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
  if (sendingReset_ || userAuth_.empty() || !CanCallApi()) return;
  sendingReset_ = true;
  sendResetButton_->set_sensitive(false);

  urnet::AuthPasswordResetArgs args{};
  args.user_auth = userAuth_;
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
  host_.api().authPasswordReset(
      std::optional<urnet::AuthPasswordResetArgs>(args),
      [this, epoch, seen, flow, target](
          std::optional<urnet::AuthPasswordResetResult> result,
          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, flow, target, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;
          if (!resetFlow_.Settle(flow, "password reset")) return;
          sendingReset_ = false;
          sendResetButton_->set_sensitive(accountState_ == AccountFieldState::Loaded &&
                                          !userAuth_.empty());
          // No error field: success is a result AND no transport error.
          const bool ok = result.has_value() && !err.has_value();
          if (!ok) {
            g_warning("account: authPasswordReset failed: %s",
                      err ? err->c_str() : "(no result)");
            ApplySupporting(*statusLine_,
                            T_("error_sending_password_reset_link",
                               "Error sending password reset link"),
                            kit::ValidationState::Invalid);
            return;
          }
          ApplySupporting(
              *statusLine_,
              Format(T_("password_reset_link_sent_to", "Password reset link sent to {}."),
                     target),
              kit::ValidationState::Valid);
        });
      });
}

// ---- §3.2 the login methods --------------------------------------------------

void AccountPage::RenderAuthMethods() {
  RemoveAllChildren(*authMethodsPanel_);
  if (methodsState_ != AccountFieldState::Loaded || authMethods_.empty()) {
    // ONE state line instead of rows, on the pane grid (12,8 + hairline) —
    // each of Loading / NoSession / Empty / Failed says WHICH it is.
    auto prose = MakeProseRow({}, kStatePadY);
    ApplyFieldState(*prose.line, methodsState_ == AccountFieldState::Loaded
                                     ? AccountFieldState::Empty
                                     : methodsState_);
    authMethodsPanel_->append(*prose.root);
    return;
  }
  for (const std::string& method : authMethods_) {
    const Glib::ustring label = UpperFirst(method);
    auto* remove = AddButtonRow(*authMethodsPanel_, label, {}, T_("remove", "Remove"));
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
void AccountPage::RefreshClientId() { ApplyClientId(); }

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

void AccountPage::ApplyReferralCode(AccountFieldState state) {
  ApplyFieldState(*bonusCodeValue_, state, referralCode_);
  kit::SetAccessibleLabel(
      *bonusCodeValue_,
      Glib::ustring(T_("bonus_referral_code_label", "Bonus referral code")) + ", " +
          bonusCodeValue_->get_text());
  bonusCodeCopy_->set_sensitive(state == AccountFieldState::Loaded && !referralCode_.empty());
}

void AccountPage::ApplyReferralSummary(AccountFieldState state) {
  switch (state) {
    case AccountFieldState::Loaded:
      SetToned(*referralSummary_, kUrTextMuted,
               Format(T_("referral_summary",
                         "Code: {0} · Referrals: {1} · Friends enter the code when they "
                         "sign up"),
                      referralCode_, totalReferrals_));
      break;
    case AccountFieldState::Failed:
      SetToned(*referralSummary_, kUrDanger,
               T_("something_went_wrong", "Something went wrong."));
      break;
    default:
      ApplyFieldState(*referralSummary_, state);
      break;
  }
  // §3.3.5 — the badge follows the COUNT, not the code: a network with
  // referrals but no referral code still earned it. "Hidden on failure and
  // sign-out" comes free, because both zero the count; the extra gate is only
  // there so a Loading state cannot leave the badge asserting a figure the
  // current read has not confirmed.
  const bool answered = state == AccountFieldState::Loaded ||
                        state == AccountFieldState::Empty;
  royaltyBadge_->set_visible(answered && totalReferrals_ > 0);
}

void AccountPage::ApplyReferralNetworkValue(AccountFieldState state,
                                            const std::string& name) {
  // The name is passed in rather than read back off the label: a label that
  // has just been written with a state line would otherwise hand its own
  // "Loading..." back as the loaded value.
  ApplyFieldState(*referralNetworkRow_.value, state, name);
}

void AccountPage::LoadReferralInfo() {
  referralFlow_.Abandon();
  if (!CanCallApi()) {
    referralCode_.clear();
    totalReferrals_ = 0;
    ApplyReferralCode(AccountFieldState::NoSession);
    ApplyReferralSummary(AccountFieldState::NoSession);
    ApplyBalance(balance_);  // pane A's referral rows follow the same figure
    return;
  }
  ApplyReferralCode(AccountFieldState::Loading);
  ApplyReferralSummary(AccountFieldState::Loading);

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // 20 s: the bonus-code row and the referral summary both hang off this one.
  const uint32_t flow = referralFlow_.Begin(kApiTimeoutMs, [this] {
    referralCode_.clear();
    totalReferrals_ = 0;
    ApplyReferralCode(AccountFieldState::Failed);
    ApplyReferralSummary(AccountFieldState::Failed);
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
            totalReferrals_ = 0;
            ApplyReferralCode(AccountFieldState::Failed);
            ApplyReferralSummary(AccountFieldState::Failed);
            ApplyBalance(balance_);
            return;
          }
          referralCode_ = result->referral_code.value_or(std::string());
          totalReferrals_ = result->total_referrals;
          // The CODE ROW distinguishes "no code" — Empty renders the generic
          // "None" (§3.3.2). The SUMMARY does not: §3.3.4 gives a successful
          // read exactly ONE render, the referral sentence with its count.
          // Routing an empty code through Empty here dropped a network's
          // referral count on the floor and hid the badge it had earned.
          ApplyReferralCode(referralCode_.empty() ? AccountFieldState::Empty
                                                  : AccountFieldState::Loaded);
          ApplyReferralSummary(AccountFieldState::Loaded);
          // the referral load repaints pane A's two referral lines
          ApplyBalance(balance_);
        });
      });
}

void AccountPage::LoadReferralNetwork() {
  referralNetworkFlow_.Abandon();
  if (!CanCallApi()) {
    ApplyReferralNetworkValue(AccountFieldState::NoSession, {});
    return;
  }
  ApplyReferralNetworkValue(AccountFieldState::Loading, {});

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  const uint32_t flow = referralNetworkFlow_.Begin(
      kApiTimeoutMs, [this] { ApplyReferralNetworkValue(AccountFieldState::Failed, {}); });
  host_.api().getReferralNetwork(
      [this, epoch, seen, flow](std::optional<urnet::GetReferralNetworkResult> result,
                                std::optional<std::string> err) {
        PostToMain([this, epoch, seen, flow, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;
          if (!referralNetworkFlow_.Settle(flow, "referral network load")) return;
          // The server answers "no referral network found" on the ERROR
          // channel of a lookup that SUCCEEDED: a structured response, error
          // or not, renders name-or-"None"; only transport failure is Failed.
          if (!result || err) {
            g_warning("account: getReferralNetwork failed: %s",
                      err ? err->c_str() : "(no result)");
            ApplyReferralNetworkValue(AccountFieldState::Failed, {});
            return;
          }
          const std::string name = result->network ? result->network->name : std::string();
          ApplyReferralNetworkValue(
              name.empty() ? AccountFieldState::Empty : AccountFieldState::Loaded, name);
        });
      });
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

void AccountPage::ShowReferralNetworkSheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("account: no window root; the referral-network sheet was not opened");
    return;
  }
  if (!BeginSheet("referral network")) return;
  if (!referralSheet_) {
    referralSheet_ = std::make_unique<AccountReferralNetworkSheet>(
        *root, host_, [this] { return CanCallApi(); });
    referralSheet_->on_changed = [this] { LoadReferralNetwork(); };
    referralSheet_->on_royal = [this, root] { ShowRoyalWelcomeSheet(*root); };
    WireSheet(*referralSheet_);
  }
  referralSheet_->Open();
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
    deleteSheet_->on_deleted = [this] {
      // the session is meaningless once the network is gone
      host_.Logout();
    };
    WireSheet(*deleteSheet_);
  }
  deleteSheet_->Open();
}

}  // namespace urnw
