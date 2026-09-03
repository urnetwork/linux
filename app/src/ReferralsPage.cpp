// SPDX-License-Identifier: MPL-2.0
#include "ReferralsPage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "I18n.hpp"
#include "ReferralPanel.hpp"
#include "Ui.hpp"
#include "UrTheme.hpp"

namespace urnw {
namespace {

constexpr int kApiTimeoutMs = 20000;       // CONTRACT rule 3: a plain api call
constexpr int kSheetWidth = 400;           // the referral-network sheet (MinWidth 400)
constexpr int kMinReferralCodeLength = 6;  // ReferralNetworkSheet gate
constexpr int kRowTall = 44;               // two-line rows
constexpr int kCardPad = 12;               // the shared referral pieces sit on the pane's inset
constexpr int kReferralValueChars = 24;    // referral-network value (MaxWidth 240)

// The same tone + FieldState writers AccountPage carries (see there for why a
// pango run rather than a swapped CSS class).
void SetToned(Gtk::Label& line, const Rgba& color, const Glib::ustring& text) {
  line.set_markup("<span foreground='" + HexForMarkup(color) + "'>" +
                  Glib::Markup::escape_text(text) + "</span>");
}

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

// Two more of AccountPage's file-local helpers the sheet is written against.
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

std::string TrimSpace(const std::string& text) {
  static const char* kWs = " \t\n\v\f\r";
  const auto begin = text.find_first_not_of(kWs);
  if (begin == std::string::npos) return {};
  const auto end = text.find_last_not_of(kWs);
  return text.substr(begin, end - begin + 1);
}

// Points in the app's own unit. NOTE the SDK naming trap EarningsPage documents:
// nanoPointsToPoints divides by 1e6, not 1e9 — always the SDK helper.
double ReferralPointsOf(const urnet::AccountPointsList& points) {
  double total = 0;
  for (const auto& point : points) {
    if (point.event == "payout_linked_account") {
      total += urnet::nanoPointsToPoints(point.point_value);
    }
  }
  return total;
}

// The Earnings page's points formatter, verbatim: integer when it rounds
// clean, else two decimals, hand-grouped thousands.
std::string FormatPointsValue(double value) {
  char buffer[64];
  if (std::fabs(value - std::round(value)) < 0.005) {
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
  }
  std::string text = buffer;
  const size_t start = (!text.empty() && text[0] == '-') ? 1u : 0u;
  size_t end = text.find('.');
  if (end == std::string::npos) end = text.size();
  for (size_t insertAt = end; insertAt > start + 3;) {
    insertAt -= 3;
    text.insert(insertAt, ",");
  }
  return text;
}

}  // namespace

// =============================================================================
// §3.3.3 ReferralNetworkSheet — set or unlink the referral network
// =============================================================================
// The unlink is a TWO-STEP ON DIFFERENT CONTROLS because a dialog cannot open
// a second dialog: the inline button ARMS (and the error line states the
// consequence), the sheet's own primary COMMITS, and Enter is bound to Close
// throughout.
class ReferralNetworkSheet : public Gtk::Window {
 public:
  ReferralNetworkSheet(Gtk::Window& parent, SdkHost& host,
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

  ~ReferralNetworkSheet() override { ++*epoch_; }

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
// ReferralsPage
// =============================================================================

ReferralsPage::ReferralsPage(SdkHost& host, SubscriptionBalanceStore& balance)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0), host_(host), balance_(balance) {
  EnsureBrandCss();   // the pane-shell vocabulary
  EnsureDrawerCss();  // .ur-caption / .ur-key and the sheet styles
  set_hexpand(true);
  set_vexpand(true);
  BuildPane();
  append(*pane_.root);
  SettleNoSession();
}

ReferralsPage::~ReferralsPage() {
  ++*epoch_;        // orphan every in-flight completion
  *alive_ = false;  // ... and every marshaled cleanup
  referralSheet_.reset();
}

void ReferralsPage::BuildPane() {
  pane_ = kit::MakePane(T_("refer_and_earn", "Refer and earn"));
  pane_.root->set_hexpand(true);
  kit::SetAccessibleLabel(*pane_.root, T_("refer_and_earn", "Refer and earn"));

  // "‹ Account" in the pane header: the page is reached from Account and has
  // no rail item of its own.
  auto* back = Gtk::make_managed<Gtk::Button>();
  back->add_css_class("flat");
  back->set_label(Glib::ustring("‹ ") + T_("account", "Account"));
  kit::SetAccessibleLabel(*back, T_("account", "Account"));
  back->signal_clicked().connect([this] {
    if (on_back) on_back();
  });
  pane_.header->append(*back);

  auto* content = pane_.content;

  // 1. the shared referral pieces (the onboarding "Refer friends" card + panel)
  progress_ = Gtk::make_managed<ReferralProgressBox>();
  progress_->set_margin(kCardPad);
  content->append(*progress_);
  panel_ = Gtk::make_managed<ReferralPanel>();
  panel_->set_margin_start(kCardPad);
  panel_->set_margin_end(kCardPad);
  panel_->set_margin_bottom(kCardPad);
  content->append(*panel_);

  // 2. the figures
  content->append(*kit::MakePaneGroupHeader(T_("referrals", "Referrals")).root);
  {
    auto row = kit::MakePaneKeyValueRow(T_("total_referrals", "Total referrals"));
    totalValue_ = row.value;
    content->append(*row.root);
  }
  {
    auto row = kit::MakePaneKeyValueRow(T_("referral_points", "Referral points"));
    pointsValue_ = row.value;
    content->append(*row.root);
  }

  // 3. the referral network (opens the sheet)
  referralNetworkRow_ =
      kit::MakePaneTwoLineRowButton(T_("referral_network", "Referral network"), {}, kRowTall);
  referralNetworkRow_.value->set_max_width_chars(kReferralValueChars);
  referralNetworkRow_.root->signal_clicked().connect([this] { ShowReferralNetworkSheet(); });
  content->append(*referralNetworkRow_.root);
}

// ---- gates -------------------------------------------------------------------

bool ReferralsPage::CanCallApi() {
  // preview first (a preview build must never reach production authenticated),
  // then the SESSION test — never apiReady(), which is true from SDK init
  return !previewMode_ && host_.IsLoggedIn();
}

ReferralTerms ReferralsPage::Terms() const {
  return ReferralTerms{balance_.MaxReferrals(), balance_.BonusGibPerDay(),
                       balance_.ReferredBonusGibPerDay()};
}

void ReferralsPage::SetPreviewMode(bool on) { previewMode_ = on; }

void ReferralsPage::ShowPreviewState() { SettleNoSession(); }

// ---- lifecycle ---------------------------------------------------------------

void ReferralsPage::Load() {
  ++*epoch_;  // drop every completion armed for the previous session
  pointsFlow_.Abandon();
  referralNetworkFlow_.Abandon();
  if (!CanCallApi()) {
    SettleNoSession();
    return;
  }
  ApplyCard();
  ApplyTotal(AccountFieldState::Loaded);
  LoadPoints();
  LoadReferralNetwork();
}

void ReferralsPage::OnBalanceChanged() {
  ApplyCard();
  if (CanCallApi()) ApplyTotal(AccountFieldState::Loaded);
}

void ReferralsPage::ResetForSignOut() {
  ++*epoch_;
  pointsFlow_.Abandon();
  referralNetworkFlow_.Abandon();
  if (referralSheet_) referralSheet_->set_visible(false);
  SettleNoSession();
}

void ReferralsPage::SettleNoSession() {
  ApplyCard();
  ApplyTotal(AccountFieldState::NoSession);
  ApplyPoints(AccountFieldState::NoSession, 0);
  ApplyReferralNetworkValue(AccountFieldState::NoSession, {});
}

// ---- painters ----------------------------------------------------------------

// The card is painted from the store on every change. With no session it
// renders the uncrowned, code-less state rather than a departed account's code.
void ReferralsPage::ApplyCard() {
  const ReferralTerms terms = Terms();
  const bool session = CanCallApi();
  const int64_t total = session ? balance_.TotalReferrals() : 0;
  const std::string code = session ? balance_.ReferralCode() : std::string();
  if (progress_) progress_->Update(total, terms);
  if (panel_) panel_->Update(code, total, terms);
}

void ReferralsPage::ApplyTotal(AccountFieldState state) {
  if (state == AccountFieldState::Loaded) {
    // the store's figure, "0" until its referral read lands
    ApplyFieldState(*totalValue_, state, std::to_string(balance_.TotalReferrals()));
  } else {
    ApplyFieldState(*totalValue_, state);
  }
  kit::SetAccessibleLabel(*totalValue_, Glib::ustring(T_("total_referrals", "Total referrals")) +
                                            ", " + totalValue_->get_text());
}

void ReferralsPage::ApplyPoints(AccountFieldState state, double points) {
  if (state == AccountFieldState::Loaded) {
    ApplyFieldState(*pointsValue_, state, FormatPointsValue(points));
  } else {
    ApplyFieldState(*pointsValue_, state);
  }
  kit::SetAccessibleLabel(*pointsValue_, Glib::ustring(T_("referral_points", "Referral points")) +
                                             ", " + pointsValue_->get_text());
}

void ReferralsPage::ApplyReferralNetworkValue(AccountFieldState state,
                                              const std::string& name) {
  ApplyFieldState(*referralNetworkRow_.value, state, name);
}

// ---- loads -------------------------------------------------------------------

void ReferralsPage::LoadPoints() {
  pointsFlow_.Abandon();
  if (!CanCallApi()) {
    ApplyPoints(AccountFieldState::NoSession, 0);
    return;
  }
  ApplyPoints(AccountFieldState::Loading, 0);
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  const uint32_t flow =
      pointsFlow_.Begin(kApiTimeoutMs, [this] { ApplyPoints(AccountFieldState::Failed, 0); });
  host_.api().getAccountPoints(
      [this, epoch, seen, flow](std::optional<urnet::AccountPointsResult> result,
                                std::optional<std::string> err) {
        PostToMain([this, epoch, seen, flow, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;
          if (!pointsFlow_.Settle(flow, "referral points load")) return;
          if (err || !result) {
            g_warning("referrals: getAccountPoints failed: %s",
                      err ? err->c_str() : "(no result)");
            ApplyPoints(AccountFieldState::Failed, 0);
            return;
          }
          ApplyPoints(AccountFieldState::Loaded,
                      ReferralPointsOf(result->network_points.value_or(urnet::AccountPointsList{})));
        });
      });
}

void ReferralsPage::LoadReferralNetwork() {
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
            g_warning("referrals: getReferralNetwork failed: %s",
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

Gtk::Window* ReferralsPage::RootWindow() { return dynamic_cast<Gtk::Window*>(get_root()); }

bool ReferralsPage::BeginSheet(const char* what) {
  if (sheetShowing_ || (sheet_open && sheet_open())) {
    // a click that opens nothing stays a mystery: logged, never silent
    g_message("referrals: %s suppressed — a modal is already open", what);
    return false;
  }
  sheetShowing_ = true;
  if (on_sheet_open_changed) on_sheet_open_changed(true);
  return true;
}

void ReferralsPage::EndSheet() {
  if (!sheetShowing_) return;
  sheetShowing_ = false;
  if (on_sheet_open_changed) on_sheet_open_changed(false);
}

void ReferralsPage::WireSheet(Gtk::Window& sheet) {
  auto alive = alive_;
  sheet.signal_hide().connect([this, alive] {
    // never tear anything down inside the widget's own signal
    PostToMain([this, alive] {
      if (!*alive) return;  // the page is gone; nothing to clear
      EndSheet();
    });
  });
}

void ReferralsPage::ShowReferralNetworkSheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("referrals: no window root; the referral-network sheet was not opened");
    return;
  }
  if (!BeginSheet("referral network")) return;
  if (!referralSheet_) {
    // The sheet asks the PAGE whether a server touch is allowed, every time —
    // preview mode is a page-level switch that can flip after construction.
    referralSheet_ = std::make_unique<ReferralNetworkSheet>(
        *root, host_, [this] { return CanCallApi(); });
    referralSheet_->on_changed = [this] { LoadReferralNetwork(); };
    referralSheet_->on_royal = [root] { ShowRoyalWelcomeSheet(*root); };
    WireSheet(*referralSheet_);
  }
  referralSheet_->Open();
}

}  // namespace urnw
