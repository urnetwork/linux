// SPDX-License-Identifier: MPL-2.0
#include "AuthViews.hpp"


#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

constexpr int kMinNetworkNameLength = 6;   // mac checkNetworkName
constexpr int kMinPasswordLength = 12;     // mac ViewModel.minPasswordLength
constexpr unsigned kNameDebounceMs = 250;  // mac networkCheckWorkItem delay
constexpr int kVerifyCodeLength = 6;       // mac codeCount
constexpr unsigned kResendCooldownSeconds = 15;  // mac startResendButtonTimer

Gtk::Label* MakePageCaption(const std::string& text) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class("ur-input-label");
  label->set_xalign(0);
  label->set_wrap(true);
  return label;
}

// back chevron row shared by the three pages
Gtk::Button* MakeBackButton() {
  auto* back = Gtk::make_managed<Gtk::Button>();
  back->set_icon_name("go-previous-symbolic");
  back->add_css_class("flat");
  back->set_halign(Gtk::Align::START);
  return back;
}

// The step's fields sit on a hairlined card (windows UrCardStyle): a bare
// column floating on the page reads as an accident. The page itself stays the
// stack child; the card is what the fields append into.
Gtk::Box* MakeStepCard(Gtk::Box& page) {
  auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  card->add_css_class("ur-card-bordered");
  page.append(*card);
  return card;
}

// the step heading: the display face at 20 (windows UrHeadingFontFamily)
Gtk::Label* MakeStepHeading(const Glib::ustring& text) {
  auto* heading = Gtk::make_managed<Gtk::Label>(text);
  heading->add_css_class("ur-step-heading");
  heading->set_xalign(0);
  heading->set_wrap(true);
  return heading;
}

}  // namespace

// ---- CreateNetworkPage -------------------------------------------------------

CreateNetworkPage::CreateNetworkPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 12),
      host_(host),
      nameChecker_(
          [this](const std::string& name, NetworkNameChecker::CheckDone done) {
            auto epoch = epoch_;
            const uint64_t issued = *epoch;
            host_.CheckNetworkName(name, [this, epoch, issued, done](bool ok, bool available) {
              PostToMain([epoch, issued, done, ok, available] {
                if (*epoch != issued) return;
                done(ok, available);
              });
            });
          },
          [this](unsigned delayMs, std::function<void()> task) {
            nameDebounce_.disconnect();
            nameDebounce_ = Glib::signal_timeout().connect(
                [task]() -> bool {
                  task();
                  return false;
                },
                delayMs);
          },
          [this] { nameDebounce_.disconnect(); },
          [this](NetworkNameState state) { OnNameStateChanged(state); },
          kNameDebounceMs) {
  EnsureDrawerCss();
  set_margin(24);
  set_valign(Gtk::Align::CENTER);
  BuildUi();
}

void CreateNetworkPage::BuildUi() {
  auto* back = MakeBackButton();
  back->signal_clicked().connect([this] {
    if (on_back) on_back();
  });
  append(*back);

  auto* card = MakeStepCard(*this);

  title_ = MakeStepHeading(T_("join_urnetwork", "Join URnetwork"));
  card->append(*title_);

  emailCaption_ = MakePageCaption(T_("user_auth_input_label", "Email or phone number"));
  card->append(*emailCaption_);
  email_ = Gtk::make_managed<Gtk::Entry>();
  email_->add_css_class("ur-input");
  email_->set_placeholder_text(T_("user_auth_placeholder", "Enter your phone number or email"));
  email_->signal_changed().connect([this] {
    errorLabel_->set_text("");
    UpdateFormValid();
  });
  card->append(*email_);

  card->append(*MakePageCaption(T_("network_name_label", "Network name")));
  networkName_ = Gtk::make_managed<Gtk::Entry>();
  networkName_->add_css_class("ur-input");
  networkName_->set_placeholder_text(
      T_("enter_a_name_for_your_network", "Enter a name for your network"));
  networkName_->signal_changed().connect([this] { OnNetworkNameChanged(); });
  card->append(*networkName_);
  nameSupporting_ = MakePageCaption("");
  card->append(*nameSupporting_);

  passwordCaption_ = MakePageCaption(T_("password_label", "Password"));
  card->append(*passwordCaption_);
  password_ = Gtk::make_managed<Gtk::PasswordEntry>();
  password_->add_css_class("ur-input");
  password_->set_show_peek_icon(true);
  password_->property_placeholder_text() = T_("password_must_be_at_least_12_characters_long",
                                              "Password must be at least 12 characters long");
  password_->property_text().signal_changed().connect([this] {
    errorLabel_->set_text("");
    UpdateFormValid();
  });
  card->append(*password_);

  // terms: switch + linked terms/privacy text (pango links open the browser)
  auto* termsRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  termsSwitch_ = Gtk::make_managed<Gtk::Switch>();
  termsSwitch_->set_valign(Gtk::Align::CENTER);
  termsSwitch_->property_active().signal_changed().connect([this] {
    errorLabel_->set_text("");
    UpdateFormValid();
  });
  termsRow->append(*termsSwitch_);
  auto* termsLabel = Gtk::make_managed<Gtk::Label>();
  termsLabel->set_markup(MarkdownLinksToPango(
      T_("i_agree_to_urnetwork_s_terms_and_services_https",
         "I agree to URnetwork's [Terms and Services](https://ur.io/terms) and "
         "[Privacy Policy](https://ur.io/privacy)")));
  termsLabel->add_css_class("dim-label");
  termsLabel->add_css_class("caption");
  termsLabel->set_wrap(true);
  termsLabel->set_xalign(0);
  termsLabel->set_hexpand(true);
  termsRow->append(*termsLabel);
  card->append(*termsRow);

  // the marketing opt-out, on by default: one short row under the terms
  auto* updatesRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  productUpdates_ = Gtk::make_managed<Gtk::Switch>();
  productUpdates_->set_valign(Gtk::Align::CENTER);
  productUpdates_->set_active(true);
  updatesRow->append(*productUpdates_);
  auto* updatesLabel = Gtk::make_managed<Gtk::Label>(
      T_("periodic_product_updates", "Periodic product updates"));
  updatesLabel->add_css_class("dim-label");
  updatesLabel->add_css_class("caption");
  updatesLabel->set_wrap(true);
  updatesLabel->set_xalign(0);
  updatesLabel->set_hexpand(true);
  updatesRow->append(*updatesLabel);
  card->append(*updatesRow);

  // the optional referral code, always visible above Continue (the Windows
  // sign-up's bonus code box): typing checks it, Continue carries it
  referralCode_ = Gtk::make_managed<ReferralCodeBox>(host_);
  referralCode_->on_edit = [this] { errorLabel_->set_text(""); };
  card->append(*referralCode_);

  continueBtn_ = Gtk::make_managed<Gtk::Button>();
  auto* continueContent = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  continueContent->set_halign(Gtk::Align::CENTER);
  spinner_ = Gtk::make_managed<Gtk::Spinner>();
  spinner_->set_visible(false);
  continueContent->append(*spinner_);
  continueContent->append(*Gtk::make_managed<Gtk::Label>(T_("continue_txt", "Continue")));
  continueBtn_->set_child(*continueContent);
  continueBtn_->add_css_class("suggested-action");
  continueBtn_->add_css_class("pill");
  continueBtn_->set_sensitive(false);
  continueBtn_->signal_clicked().connect([this] { OnContinue(); });
  card->append(*continueBtn_);

  errorLabel_ = Gtk::make_managed<Gtk::Label>();
  errorLabel_->add_css_class("ur-error-text");
  errorLabel_->set_wrap(true);
  card->append(*errorLabel_);
}

void CreateNetworkPage::Configure(Mode mode, const std::string& userAuth) {
  ++*epoch_;
  mode_ = mode;
  creating_ = false;
  nameChecker_.Clear();

  email_->set_text(userAuth);
  networkName_->set_text("");
  password_->set_text("");
  termsSwitch_->set_active(false);
  referralCode_->Reset();
  errorLabel_->set_text("");
  SetNameSupporting(T_("network_name_length_error", "Network names must be 6 characters or more"),
                    nullptr);
  SetCreating(false);

  // wallet and sso sign-ups authenticate with the signed challenge / the
  // identity token: no email/password
  const bool wantsUserAuth = mode != Mode::Wallet && mode != Mode::Sso;
  email_->set_visible(wantsUserAuth);
  emailCaption_->set_visible(wantsUserAuth);
  password_->set_visible(wantsUserAuth);
  passwordCaption_->set_visible(wantsUserAuth);
  UpdateFormValid();
}

void CreateNetworkPage::FocusFirstField() {
  if (networkName_) networkName_->grab_focus();
}

void CreateNetworkPage::SetNameSupporting(const char* text, const char* cssClass) {
  nameSupporting_->set_text(text);
  nameSupporting_->remove_css_class("ur-error-text");
  nameSupporting_->remove_css_class("ur-value-on");
  if (cssClass) nameSupporting_->add_css_class(cssClass);
}

void CreateNetworkPage::OnNetworkNameChanged() {
  errorLabel_->set_text("");
  const std::string name = TrimWhitespace(networkName_->get_text());
  if (static_cast<int>(name.size()) < kMinNetworkNameLength) {
    nameChecker_.Clear();
  } else {
    // debounced (mac delays the work item 250ms)
    nameChecker_.SetName(name);
  }
}

// A failed check is not a verdict on the name (see NetworkNameCheck.hpp): it
// says so and keeps Continue usable; the checker re-runs it.
void CreateNetworkPage::OnNameStateChanged(NetworkNameState state) {
  switch (state) {
    case NetworkNameState::NotChecked:
      SetNameSupporting(
          T_("network_name_length_error", "Network names must be 6 characters or more"), nullptr);
      break;
    case NetworkNameState::Validating:
      break;
    case NetworkNameState::Valid:
      SetNameSupporting(
          T_("nice_this_network_name_is_available", "Nice! This network name is available"),
          "ur-value-on");
      break;
    case NetworkNameState::Taken:
      SetNameSupporting(T_("network_name_taken", "This network name is already taken"),
                        "ur-error-text");
      break;
    case NetworkNameState::CheckFailed:
      SetNameSupporting(T_("there_was_an_error_checking_the_network_name",
                           "There was an error checking the network name"),
                        "ur-error-text");
      break;
  }
  UpdateFormValid();
}

void CreateNetworkPage::UpdateFormValid() {
  // mac validateForm: name available (or its check failed; the server
  // re-checks on create), terms agreed, and (for the password auth type) a
  // 12+ character password
  bool valid = NetworkNameAllowsCreate(nameChecker_.State()) && termsSwitch_->get_active();
  if (mode_ != Mode::Wallet && mode_ != Mode::Sso) {
    valid = valid && !TrimWhitespace(email_->get_text()).empty() &&
            std::string(password_->get_text()).size() >= kMinPasswordLength;
  }
  continueBtn_->set_sensitive(valid && !creating_);
}

void CreateNetworkPage::SetCreating(bool creating) {
  creating_ = creating;
  spinner_->set_visible(creating);
  if (creating) {
    spinner_->start();
  } else {
    spinner_->stop();
  }
  networkName_->set_sensitive(!creating);
  email_->set_sensitive(!creating);
  password_->set_sensitive(!creating);
  termsSwitch_->set_sensitive(!creating);
  referralCode_->set_sensitive(!creating);
  UpdateFormValid();
}

void CreateNetworkPage::OnContinue() {
  if (creating_) return;
  SetCreating(true);
  errorLabel_->set_text("");

  const std::string userAuth = TrimWhitespace(email_->get_text());
  const std::string password(password_->get_text());
  const std::string networkName = TrimWhitespace(networkName_->get_text());
  const std::string referralCode = referralCode_->CreateCode();

  // the marketing opt-out rides on the create call (absent = opted in)
  host_.SetProductUpdatesOptOut(productUpdates_ && !productUpdates_->get_active());

  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  auto done = [this, epoch, issued, userAuth](AuthResult r) {
    PostToMain([this, epoch, issued, userAuth, r] {
      if (*epoch != issued) return;
      SetCreating(false);
      if (r.verification_required) {
        if (on_verify) on_verify(userAuth, r.sendNotice);
        return;
      }
      if (!r.ok) {
        errorLabel_->set_text(
            r.error.empty()
                ? std::string(T_("error_creating_network",
                                 "There was an error creating your network. Please try again later."))
                : r.error);
        return;
      }
      if (on_success) on_success();
    });
  };

  switch (mode_) {
    case Mode::Password:
      host_.CreateNetwork(networkName, userAuth, password, referralCode, done);
      break;
    case Mode::Wallet:
      host_.CreateNetworkWithPendingWallet(networkName, referralCode, done);
      break;
    case Mode::Sso:
      host_.CreateNetworkWithPendingSso(networkName, referralCode, done);
      break;
  }
}

// ---- send notices ---------------------------------------------------------------

int64_t MonotonicSeconds() {
  return g_get_monotonic_time() / G_USEC_PER_SEC;
}

std::string ResetSendNoticeText(const VerifySendNotice& notice) {
  switch (notice.kind) {
    case VerifySendNoticeKind::Sent:
      return std::string();
    case VerifySendNoticeKind::RateLimited:
      return Format(TN_("reset_link_rate_limited",
                        "Too many attempts. You can request a new reset link in {} minute.",
                        "Too many attempts. You can request a new reset link in {} minutes.",
                        static_cast<unsigned long>(notice.minutes)),
                    notice.minutes);
    case VerifySendNoticeKind::SendFailed:
      return T_("error_sending_password_reset_link", "Error sending password reset link");
    case VerifySendNoticeKind::ServerMessage:
      return notice.message;
  }
  return std::string();
}

// ---- VerifyPage ---------------------------------------------------------------

VerifyPage::VerifyPage(SdkHost& host) : Gtk::Box(Gtk::Orientation::VERTICAL, 12), host_(host) {
  EnsureDrawerCss();
  set_margin(24);
  set_valign(Gtk::Align::CENTER);
  BuildUi();
}

VerifyPage::~VerifyPage() {
  resendCooldown_.disconnect();
  rateLimitTick_.disconnect();
}

void VerifyPage::BuildUi() {
  auto* back = MakeBackButton();
  back->signal_clicked().connect([this] {
    if (on_back) on_back();
  });
  append(*back);

  auto* card = MakeStepCard(*this);

  title_ = MakeStepHeading(T_("login_verify_header", "You've got mail"));
  card->append(*title_);

  auto* body = Gtk::make_managed<Gtk::Label>(
      T_("verify_explanation",
         "Tell us who you really are. Enter the code we sent you to verify your identity."));
  body->add_css_class("dim-label");
  body->set_wrap(true);
  body->set_justify(Gtk::Justification::CENTER);
  card->append(*body);

  code_ = Gtk::make_managed<Gtk::Entry>();
  code_->set_max_length(kVerifyCodeLength);
  code_->add_css_class("ur-otp");
  code_->set_alignment(0.5f);
  code_->set_placeholder_text(T_("verify_input_label", "Verification code"));
  code_->set_halign(Gtk::Align::CENTER);
  code_->set_width_chars(10);
  code_->signal_changed().connect([this] {
    errorLabel_->set_text("");
    // auto-submit on the sixth digit (mac parity)
    if (static_cast<int>(std::string(code_->get_text()).size()) == kVerifyCodeLength &&
        !submitting_ && !sending_) {
      Submit();
    }
  });
  card->append(*code_);

  spinner_ = Gtk::make_managed<Gtk::Spinner>();
  spinner_->set_visible(false);
  spinner_->set_halign(Gtk::Align::CENTER);
  card->append(*spinner_);

  errorLabel_ = Gtk::make_managed<Gtk::Label>();
  errorLabel_->add_css_class("ur-error-text");
  errorLabel_->set_wrap(true);
  card->append(*errorLabel_);

  auto* resendRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  resendRow->set_halign(Gtk::Align::CENTER);
  auto* dontSee = Gtk::make_managed<Gtk::Label>(T_("dont_see_it", "Don't see it?"));
  dontSee->add_css_class("dim-label");
  resendRow->append(*dontSee);
  resendBtn_ = Gtk::make_managed<Gtk::Button>(T_("resend_verify_code", "Resend code"));
  resendBtn_->add_css_class("flat");
  resendBtn_->signal_clicked().connect([this] { Resend(); });
  resendRow->append(*resendBtn_);
  card->append(*resendRow);

  resendStatus_ = Gtk::make_managed<Gtk::Label>();
  resendStatus_->add_css_class("dim-label");
  resendStatus_->add_css_class("caption");
  resendStatus_->set_wrap(true);
  card->append(*resendStatus_);
}

void VerifyPage::ShowSendNotice(const VerifySendNotice& notice) {
  ShowNoticeText(notice);
  // a rate limit holds Resend off until the server will send again
  rateLimitTick_.disconnect();
  rateLimit_.Start(notice, MonotonicSeconds());
  if (!rateLimit_.CanResend(MonotonicSeconds())) {
    resendBtn_->set_sensitive(false);
    rateLimitTick_ = Glib::signal_timeout().connect_seconds(
        [this]() -> bool { return TickRateLimit(); }, 1);
  }
}

bool VerifyPage::TickRateLimit() {
  const int64_t now = MonotonicSeconds();
  if (!rateLimit_.CanResend(now)) {
    ShowNoticeText(rateLimit_.NoticeAt(now));
    return true;
  }
  resendStatus_->set_text("");
  resendStatus_->remove_css_class("ur-error-text");
  if (!sending_) resendBtn_->set_sensitive(true);
  return false;
}

void VerifyPage::ShowNoticeText(const VerifySendNotice& notice) {
  std::string text;
  switch (notice.kind) {
    case VerifySendNoticeKind::Sent:
      resendStatus_->remove_css_class("ur-error-text");
      resendStatus_->set_text(T_("verification_code_sent",
                                 "Check your email/phone for a verification code."));
      return;
    case VerifySendNoticeKind::RateLimited:
      text = Format(TN_("verify_code_rate_limited",
                        "Too many attempts. You can request a new code in {} minute.",
                        "Too many attempts. You can request a new code in {} minutes.",
                        static_cast<unsigned long>(notice.minutes)),
                    notice.minutes);
      break;
    case VerifySendNoticeKind::SendFailed:
      text = T_("error_sending_verification_code",
                "There was an error sending the verification code.");
      break;
    case VerifySendNoticeKind::ServerMessage:
      text = notice.message;
      break;
  }
  resendStatus_->add_css_class("ur-error-text");
  resendStatus_->set_text(text);
}

void VerifyPage::Configure(const std::string& userAuth) {
  ++*epoch_;
  userAuth_ = userAuth;
  submitting_ = false;
  sending_ = false;
  resendCooldown_.disconnect();
  rateLimitTick_.disconnect();
  rateLimit_.Clear();
  code_->set_text("");
  errorLabel_->set_text("");
  resendStatus_->set_text("");
  resendBtn_->set_sensitive(true);
  SetSubmitting(false);
  // "You've got mail" for an email auth, "Check your phone" for a number
  title_->set_text(userAuth.find('@') != std::string::npos
                       ? T_("login_verify_header", "You've got mail")
                       : T_("login_verify_check_phone", "Check your phone"));
  code_->grab_focus();
}

void VerifyPage::SetSubmitting(bool submitting) {
  submitting_ = submitting;
  spinner_->set_visible(submitting);
  if (submitting) {
    spinner_->start();
  } else {
    spinner_->stop();
  }
  code_->set_sensitive(!submitting);
}

void VerifyPage::Submit() {
  if (submitting_) return;
  const std::string code(code_->get_text());
  if (static_cast<int>(code.size()) != kVerifyCodeLength) return;
  SetSubmitting(true);
  errorLabel_->set_text("");

  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.VerifyCode(userAuth_, code, [this, epoch, issued](AuthResult r) {
    PostToMain([this, epoch, issued, r] {
      if (*epoch != issued) return;
      SetSubmitting(false);
      if (!r.ok) {
        // clear the code so retyping re-triggers the auto-submit (mac parity;
        // the changed handler clears the error, so set the message after)
        code_->set_text("");
        errorLabel_->set_text(T_("verify_input_invalid",
                                 "There was a problem authenticating your verification code."));
        return;
      }
      if (on_success) on_success();
    });
  });
}

void VerifyPage::Resend() {
  if (sending_ || submitting_ || !resendBtn_->get_sensitive()) return;
  sending_ = true;
  resendBtn_->set_sensitive(false);
  resendStatus_->set_text("");
  resendStatus_->remove_css_class("ur-error-text");

  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.ResendVerifyCode(userAuth_, [this, epoch, issued](VerifySendNotice notice) {
    PostToMain([this, epoch, issued, notice] {
      if (*epoch != issued) return;
      sending_ = false;
      ShowSendNotice(notice);
      if (notice.kind == VerifySendNoticeKind::Sent) {
        StartResendCooldown();
      } else if (rateLimit_.CanResend(MonotonicSeconds())) {
        // a failed resend must leave the button usable (no timer re-enables it);
        // a rate limit keeps it off until the retry time (ShowSendNotice)
        resendBtn_->set_sensitive(true);
      }
    });
  });
}

void VerifyPage::StartResendCooldown() {
  resendCooldown_.disconnect();
  resendCooldown_ = Glib::signal_timeout().connect_seconds(
      [this]() -> bool {
        resendBtn_->set_sensitive(true);
        return false;
      },
      kResendCooldownSeconds);
}

// ---- ResetPasswordPage ---------------------------------------------------------

ResetPasswordPage::ResetPasswordPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 12), host_(host) {
  EnsureDrawerCss();
  set_margin(24);
  set_valign(Gtk::Align::CENTER);
  BuildUi();
}

ResetPasswordPage::~ResetPasswordPage() {
  rateLimitTick_.disconnect();
}

void ResetPasswordPage::BuildUi() {
  auto* back = MakeBackButton();
  back->signal_clicked().connect([this] {
    if (on_back) on_back();
  });
  append(*back);

  auto* card = MakeStepCard(*this);

  // ---- form -------------------------------------------------------------------
  formBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);

  auto* title = MakeStepHeading(T_("forgot_password", "Forgot your password?"));
  formBox_->append(*title);

  email_ = Gtk::make_managed<Gtk::Entry>();
  email_->add_css_class("ur-input");
  email_->set_placeholder_text(T_("user_auth_placeholder", "Enter your phone number or email"));
  email_->signal_changed().connect([this] { errorLabel_->set_text(""); });
  email_->signal_activate().connect([this] { Send(); });
  formBox_->append(*email_);

  auto* spamHint = Gtk::make_managed<Gtk::Label>(
      T_("you_may_need_to_your_check_spam_folder_or",
         "You may need to your check spam folder or unblock no-reply@ur.io"));
  spamHint->add_css_class("dim-label");
  spamHint->add_css_class("caption");
  spamHint->set_wrap(true);
  spamHint->set_xalign(0);
  formBox_->append(*spamHint);

  sendBtn_ = Gtk::make_managed<Gtk::Button>();
  auto* sendContent = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  sendContent->set_halign(Gtk::Align::CENTER);
  spinner_ = Gtk::make_managed<Gtk::Spinner>();
  spinner_->set_visible(false);
  sendContent->append(*spinner_);
  sendContent->append(*Gtk::make_managed<Gtk::Label>(T_("send_reset_link_2", "Send reset link")));
  sendBtn_->set_child(*sendContent);
  sendBtn_->add_css_class("suggested-action");
  sendBtn_->add_css_class("pill");
  sendBtn_->signal_clicked().connect([this] { Send(); });
  formBox_->append(*sendBtn_);

  errorLabel_ = Gtk::make_managed<Gtk::Label>();
  errorLabel_->add_css_class("ur-error-text");
  errorLabel_->set_wrap(true);
  formBox_->append(*errorLabel_);

  card->append(*formBox_);

  // ---- sent confirmation ---------------------------------------------------------
  sentBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  sentBox_->set_visible(false);
  auto* sentTitle = MakeStepHeading(T_("reset_link_sent", "Reset link sent"));
  sentBox_->append(*sentTitle);
  auto* sentToLabel = Gtk::make_managed<Gtk::Label>(T_("reset_link_sent_to", "Reset link sent to"));
  sentToLabel->add_css_class("dim-label");
  sentBox_->append(*sentToLabel);
  sentTo_ = Gtk::make_managed<Gtk::Label>();
  sentTo_->add_css_class("ur-mono-13");
  sentTo_->set_wrap(true);
  sentBox_->append(*sentTo_);
  card->append(*sentBox_);
}

void ResetPasswordPage::Configure(const std::string& userAuth) {
  ++*epoch_;
  sending_ = false;
  rateLimitTick_.disconnect();
  rateLimit_.Clear();
  // prefill from the login page but keep it editable (arriving with an empty
  // field must not be a dead end; the mac view has the value already)
  email_->set_text(userAuth);
  errorLabel_->set_text("");
  spinner_->set_visible(false);
  spinner_->stop();
  sendBtn_->set_sensitive(true);
  email_->set_sensitive(true);
  formBox_->set_visible(true);
  sentBox_->set_visible(false);
  email_->grab_focus();
}

void ResetPasswordPage::Send() {
  if (sending_ || !rateLimit_.CanResend(MonotonicSeconds())) return;
  const std::string userAuth = TrimWhitespace(email_->get_text());
  if (userAuth.empty()) return;
  sending_ = true;
  errorLabel_->set_text("");
  spinner_->set_visible(true);
  spinner_->start();
  sendBtn_->set_sensitive(false);
  email_->set_sensitive(false);

  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.SendPasswordResetLink(userAuth, [this, epoch, issued, userAuth](VerifySendNotice notice) {
    PostToMain([this, epoch, issued, userAuth, notice] {
      if (*epoch != issued) return;
      sending_ = false;
      spinner_->set_visible(false);
      spinner_->stop();
      sendBtn_->set_sensitive(true);
      email_->set_sensitive(true);
      if (notice.transportError) {
        errorLabel_->set_text(T_("something_went_wrong_please_try_again_later",
                                 "Something went wrong. Please try again later."));
        return;
      }
      if (notice.kind != VerifySendNoticeKind::Sent) {
        // the server did not send the link: say why, never "sent"
        errorLabel_->set_text(ResetSendNoticeText(notice));
        rateLimit_.Start(notice, MonotonicSeconds());
        if (!rateLimit_.CanResend(MonotonicSeconds())) {
          sendBtn_->set_sensitive(false);
          rateLimitTick_ = Glib::signal_timeout().connect_seconds(
              [this]() -> bool { return TickRateLimit(); }, 1);
        }
        return;
      }
      sentTo_->set_text(userAuth);
      formBox_->set_visible(false);
      sentBox_->set_visible(true);
    });
  });
}

bool ResetPasswordPage::TickRateLimit() {
  const int64_t now = MonotonicSeconds();
  if (!rateLimit_.CanResend(now)) {
    errorLabel_->set_text(ResetSendNoticeText(rateLimit_.NoticeAt(now)));
    return true;
  }
  errorLabel_->set_text("");
  if (!sending_) sendBtn_->set_sensitive(true);
  return false;
}

}  // namespace urnw
