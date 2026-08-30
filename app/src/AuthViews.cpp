// SPDX-License-Identifier: MPL-2.0
#include "AuthViews.hpp"

#include "ReferralRoyalty.hpp"

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
    : Gtk::Box(Gtk::Orientation::VERTICAL, 12), host_(host) {
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

  // bonus referral code: a flat toggle revealing the entry + apply button
  referralToggle_ = Gtk::make_managed<Gtk::Button>(T_("add_referral_code", "Add referral code"));
  referralToggle_->add_css_class("flat");
  referralToggle_->set_halign(Gtk::Align::START);
  referralToggle_->signal_clicked().connect([this] {
    referralRevealer_->set_reveal_child(!referralRevealer_->get_reveal_child());
  });
  card->append(*referralToggle_);

  referralRevealer_ = Gtk::make_managed<Gtk::Revealer>();
  auto* referralBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  referralBox->append(*MakePageCaption(
      T_("add_referral_extra_rewards", "Add referral code to earn extra rewards")));
  auto* referralRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  referralEntry_ = Gtk::make_managed<Gtk::Entry>();
  referralEntry_->set_placeholder_text(
      T_("enter_a_bonus_referral_code", "Enter a bonus referral code"));
  referralEntry_->set_hexpand(true);
  referralEntry_->signal_changed().connect([this] {
    // retyping invalidates the previous validation (mac didSet)
    referralValid_ = false;
    referralCapped_ = false;
    referralSupporting_->set_text("");
    referralAppliedRow_->set_visible(false);
    referralApply_->set_sensitive(!validatingReferral_ &&
                                  !TrimWhitespace(referralEntry_->get_text()).empty());
  });
  referralRow->append(*referralEntry_);
  referralApply_ = Gtk::make_managed<Gtk::Button>(T_("apply_bonus", "Apply bonus"));
  referralApply_->set_sensitive(false);
  referralApply_->signal_clicked().connect([this] { OnValidateReferral(); });
  referralRow->append(*referralApply_);
  referralBox->append(*referralRow);
  referralSupporting_ = MakePageCaption("");
  referralSupporting_->add_css_class("ur-error-text");
  referralBox->append(*referralSupporting_);
  referralRevealer_->set_child(*referralBox);
  card->append(*referralRevealer_);

  // referral accepted: the gold king-frog line (referral royalty, matching
  // the ur.io referral panel and the android/apple gold chips)
  referralAppliedRow_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* appliedLabel = Gtk::make_managed<Gtk::Label>();
  appliedLabel->set_markup("<span foreground='" + HexForMarkup(kReferralGoldLight) + "'>" +
                           Glib::Markup::escape_text(
                               T_("referral_bonus_applied_2", "Referral Bonus applied")) +
                           "</span>");
  appliedLabel->add_css_class("caption");
  referralAppliedRow_->append(*appliedLabel);
  referralAppliedRow_->set_visible(false);
  card->append(*referralAppliedRow_);

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
  nameState_ = NameState::NotChecked;
  referralValid_ = false;
  referralCapped_ = false;
  validatingReferral_ = false;
  nameDebounce_.disconnect();
  ++nameCheckGeneration_;

  email_->set_text(userAuth);
  networkName_->set_text("");
  password_->set_text("");
  termsSwitch_->set_active(false);
  referralEntry_->set_text("");
  referralRevealer_->set_reveal_child(false);
  referralSupporting_->set_text("");
  referralAppliedRow_->set_visible(false);
  errorLabel_->set_text("");
  SetNameSupporting(T_("network_name_length_error", "Network names must be 6 characters or more"),
                    nullptr);
  SetCreating(false);

  // wallet sign-up authenticates with the signed challenge: no email/password
  const bool wantsUserAuth = mode != Mode::Wallet;
  email_->set_visible(wantsUserAuth);
  emailCaption_->set_visible(wantsUserAuth);
  password_->set_visible(wantsUserAuth);
  passwordCaption_->set_visible(wantsUserAuth);
  UpdateFormValid();
}

void CreateNetworkPage::FocusFirstField() {
  if (mode_ == Mode::UpgradeGuest && email_) {
    email_->grab_focus();
  } else if (networkName_) {
    networkName_->grab_focus();
  }
}

void CreateNetworkPage::SetNameSupporting(const char* text, const char* cssClass) {
  nameSupporting_->set_text(text);
  nameSupporting_->remove_css_class("ur-error-text");
  nameSupporting_->remove_css_class("ur-value-on");
  if (cssClass) nameSupporting_->add_css_class(cssClass);
}

void CreateNetworkPage::OnNetworkNameChanged() {
  errorLabel_->set_text("");
  nameDebounce_.disconnect();
  ++nameCheckGeneration_;

  const std::string name = TrimWhitespace(networkName_->get_text());
  if (static_cast<int>(name.size()) < kMinNetworkNameLength) {
    nameState_ = NameState::NotChecked;
    SetNameSupporting(
        T_("network_name_length_error", "Network names must be 6 characters or more"), nullptr);
    UpdateFormValid();
    return;
  }

  nameState_ = NameState::Validating;
  UpdateFormValid();
  // debounce the availability check (mac delays the work item 250ms)
  nameDebounce_ = Glib::signal_timeout().connect(
      [this, name]() -> bool {
        RunNetworkCheck(name);
        return false;
      },
      kNameDebounceMs);
}

void CreateNetworkPage::RunNetworkCheck(const std::string& name) {
  const uint64_t generation = nameCheckGeneration_;
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.CheckNetworkName(name, [this, epoch, issued, generation](bool ok, bool available) {
    PostToMain([this, epoch, issued, generation, ok, available] {
      if (*epoch != issued) return;
      if (generation != nameCheckGeneration_) return;  // the field changed since
      if (!ok) {
        nameState_ = NameState::Invalid;
        SetNameSupporting(T_("there_was_an_error_checking_the_network_name",
                             "There was an error checking the network name"),
                          "ur-error-text");
      } else if (available) {
        nameState_ = NameState::Valid;
        SetNameSupporting(
            T_("nice_this_network_name_is_available", "Nice! This network name is available"),
            "ur-value-on");
      } else {
        nameState_ = NameState::Invalid;
        SetNameSupporting(T_("network_name_taken", "This network name is already taken"),
                          "ur-error-text");
      }
      UpdateFormValid();
    });
  });
}

void CreateNetworkPage::OnValidateReferral() {
  if (validatingReferral_) return;
  const std::string code = TrimWhitespace(referralEntry_->get_text());
  if (code.empty()) return;
  validatingReferral_ = true;
  referralApply_->set_sensitive(false);
  referralSupporting_->set_text("");

  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.ValidateReferralCode(code, [this, epoch, issued](bool ok, bool valid, bool capped) {
    PostToMain([this, epoch, issued, ok, valid, capped] {
      if (*epoch != issued) return;
      validatingReferral_ = false;
      referralApply_->set_sensitive(!TrimWhitespace(referralEntry_->get_text()).empty());
      referralValid_ = ok && valid;
      referralCapped_ = ok && capped;
      if (referralValid_ && !referralCapped_) {
        referralRevealer_->set_reveal_child(false);
        referralAppliedRow_->set_visible(true);
        referralToggle_->set_label(T_("edit_referral_code", "Edit referral code"));
        // the royal welcome: the gold king-frog moment for the referred
        if (auto* root = dynamic_cast<Gtk::Window*>(get_root())) {
          ShowRoyalWelcomeSheet(*root);
        }
      } else if (referralCapped_) {
        referralSupporting_->set_text(
            T_("referral_code_capped", "This code has been used up"));
      } else {
        referralSupporting_->set_text(T_("invalid_referral_code", "This code is not valid"));
      }
    });
  });
}

void CreateNetworkPage::UpdateFormValid() {
  // mac validateForm: name available, terms agreed, and (for the password auth
  // type) a 12+ character password
  bool valid = nameState_ == NameState::Valid && termsSwitch_->get_active();
  if (mode_ != Mode::Wallet) {
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
  referralToggle_->set_sensitive(!creating);
  UpdateFormValid();
}

void CreateNetworkPage::OnContinue() {
  if (creating_) return;
  SetCreating(true);
  errorLabel_->set_text("");

  const std::string userAuth = TrimWhitespace(email_->get_text());
  const std::string password(password_->get_text());
  const std::string networkName = TrimWhitespace(networkName_->get_text());
  const std::string referralCode =
      (referralValid_ && !referralCapped_) ? TrimWhitespace(referralEntry_->get_text())
                                           : std::string();

  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  auto done = [this, epoch, issued, userAuth](AuthResult r) {
    PostToMain([this, epoch, issued, userAuth, r] {
      if (*epoch != issued) return;
      SetCreating(false);
      if (r.verification_required) {
        if (on_verify) on_verify(userAuth);
        return;
      }
      if (!r.ok) {
        errorLabel_->set_text(
            r.error.empty()
                ? T_("error_creating_network",
                     "There was an error creating your network. Please try again later.")
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
    case Mode::UpgradeGuest:
      // UpgradeGuestArgs has no referral_code field — the bonus only applies
      // to a fresh create (the sdk/api shape, not a UI choice)
      host_.UpgradeGuest(networkName, userAuth, password, done);
      break;
  }
}

// ---- VerifyPage ---------------------------------------------------------------

VerifyPage::VerifyPage(SdkHost& host) : Gtk::Box(Gtk::Orientation::VERTICAL, 12), host_(host) {
  EnsureDrawerCss();
  set_margin(24);
  set_valign(Gtk::Align::CENTER);
  BuildUi();
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

void VerifyPage::ShowNotice(const std::string& text) {
  resendStatus_->remove_css_class("ur-error-text");
  resendStatus_->set_text(text);
}

void VerifyPage::Configure(const std::string& userAuth) {
  ++*epoch_;
  userAuth_ = userAuth;
  submitting_ = false;
  sending_ = false;
  resendCooldown_.disconnect();
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
  host_.ResendVerifyCode(userAuth_, [this, epoch, issued](bool ok, std::string) {
    PostToMain([this, epoch, issued, ok] {
      if (*epoch != issued) return;
      sending_ = false;
      if (ok) {
        resendStatus_->set_text(T_("verification_code_sent",
                                   "Check your email/phone for a verification code."));
        StartResendCooldown();
      } else {
        // a failed resend must leave the button usable (no timer re-enables it)
        resendBtn_->set_sensitive(true);
        resendStatus_->add_css_class("ur-error-text");
        resendStatus_->set_text(T_("error_sending_verification_code",
                                   "There was an error sending the verification code."));
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
  if (sending_) return;
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
  host_.SendPasswordResetLink(userAuth, [this, epoch, issued, userAuth](bool ok, std::string) {
    PostToMain([this, epoch, issued, userAuth, ok] {
      if (*epoch != issued) return;
      sending_ = false;
      spinner_->set_visible(false);
      spinner_->stop();
      sendBtn_->set_sensitive(true);
      email_->set_sensitive(true);
      if (!ok) {
        errorLabel_->set_text(T_("something_went_wrong_please_try_again_later",
                                 "Something went wrong. Please try again later."));
        return;
      }
      sentTo_->set_text(userAuth);
      formBox_->set_visible(false);
      sentBox_->set_visible(true);
    });
  });
}

}  // namespace urnw
