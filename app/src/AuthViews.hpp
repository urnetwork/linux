// The sign-up / verify / password-reset stack pages (ports of the apple
// Authenticate/ views):
//
//   * CreateNetworkPage — CreateNetworkView/ViewModel: network name with
//     debounced availability through the SDK's shared
//     NetworkNameValidationViewController, password (12+ chars), the terms
//     switch, and an optional bonus referral code validated with
//     Api::validateReferralCode. Three modes share the form the way the mac
//     view does: password sign-up (email + password), wallet sign-up (the
//     wallet_auth captured from a Solana/Bittensor sign-in that had no
//     network — name + terms only), and guest -> full-account upgrade
//     (Api::upgradeGuest).
//   * VerifyPage — CreateNetworkVerifyView: the 6-digit code entry with
//     auto-submit, and resend with the 15s cooldown. Also the landing for the
//     password-login "needs verification" path that used to dead-end.
//   * ResetPasswordPage — ResetPasswordView: send the reset link, then a sent
//     confirmation.
//
// Pages navigate through std::function callbacks the MainWindow wires up; all
// SDK completions arrive on SDK threads and are marshalled with PostToMain.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <gtkmm.h>

#include "SdkHost.hpp"

namespace urnw {

class CreateNetworkPage : public Gtk::Box {
 public:
  enum class Mode {
    Password,      // full sign-up: email + network name + password
    Wallet,        // wallet sign-in with no network yet: name + terms only
    Sso,           // Google/Apple (ur.io/sso bridge) with no network yet: name + terms only
    UpgradeGuest,  // guest -> full account (email + name + password)
  };

  explicit CreateNetworkPage(SdkHost& host);

  // Reset the form for a fresh navigation. userAuth prefills the email field
  // (it stays editable — the login page may not have one yet).
  void Configure(Mode mode, const std::string& userAuth);
  // Focus the first EMPTY field for the configured mode (windows
  // EnterCreateStep parity): the guest upgrade starts at the email box,
  // everything else at the network name.
  void FocusFirstField();

  std::function<void()> on_success;                        // network ready -> start tunnel
  std::function<void(std::string userAuth)> on_verify;     // verification required
  std::function<void()> on_back;

 private:
  enum class NameState { NotChecked, Validating, Valid, Invalid };

  void BuildUi();
  void OnNetworkNameChanged();
  void RunNetworkCheck(const std::string& name);
  void SetNameSupporting(const char* text, const char* cssClass);
  void OnValidateReferral();
  void UpdateFormValid();
  void OnContinue();
  void SetCreating(bool creating);

  SdkHost& host_;
  Mode mode_ = Mode::Password;
  bool creating_ = false;
  NameState nameState_ = NameState::NotChecked;
  bool referralValid_ = false;
  bool referralCapped_ = false;
  bool validatingReferral_ = false;
  sigc::connection nameDebounce_;  // 250ms debounce before networkCheck (mac parity)
  uint64_t nameCheckGeneration_ = 0;  // drops stale availability answers
  // invalidates in-flight SDK completions across Configure()
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  Gtk::Label* title_ = nullptr;
  Gtk::Entry* email_ = nullptr;
  Gtk::Label* emailCaption_ = nullptr;
  Gtk::Entry* networkName_ = nullptr;
  Gtk::Label* nameSupporting_ = nullptr;
  Gtk::PasswordEntry* password_ = nullptr;
  Gtk::Label* passwordCaption_ = nullptr;
  Gtk::Switch* termsSwitch_ = nullptr;
  Gtk::Button* referralToggle_ = nullptr;
  Gtk::Revealer* referralRevealer_ = nullptr;
  Gtk::Entry* referralEntry_ = nullptr;
  Gtk::Button* referralApply_ = nullptr;
  Gtk::Label* referralSupporting_ = nullptr;
  Gtk::Box* referralAppliedRow_ = nullptr;
  Gtk::Button* continueBtn_ = nullptr;
  Gtk::Spinner* spinner_ = nullptr;
  Gtk::Label* errorLabel_ = nullptr;
};

class VerifyPage : public Gtk::Box {
 public:
  explicit VerifyPage(SdkHost& host);

  void Configure(const std::string& userAuth);
  // An informational line under the resend row (e.g. "a code was sent" when a
  // password sign-in routed here) — never the error voice.
  void ShowNotice(const std::string& text);

  std::function<void()> on_success;
  std::function<void()> on_back;

 private:
  void BuildUi();
  void Submit();
  void Resend();
  void SetSubmitting(bool submitting);
  void StartResendCooldown();

  SdkHost& host_;
  std::string userAuth_;
  bool submitting_ = false;
  bool sending_ = false;
  sigc::connection resendCooldown_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  Gtk::Label* title_ = nullptr;
  Gtk::Entry* code_ = nullptr;
  Gtk::Spinner* spinner_ = nullptr;
  Gtk::Label* errorLabel_ = nullptr;
  Gtk::Button* resendBtn_ = nullptr;
  Gtk::Label* resendStatus_ = nullptr;
};

class ResetPasswordPage : public Gtk::Box {
 public:
  explicit ResetPasswordPage(SdkHost& host);

  void Configure(const std::string& userAuth);

  std::function<void()> on_back;

 private:
  void BuildUi();
  void Send();

  SdkHost& host_;
  bool sending_ = false;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  Gtk::Box* formBox_ = nullptr;
  Gtk::Box* sentBox_ = nullptr;
  Gtk::Entry* email_ = nullptr;
  Gtk::Button* sendBtn_ = nullptr;
  Gtk::Spinner* spinner_ = nullptr;
  Gtk::Label* errorLabel_ = nullptr;
  Gtk::Label* sentTo_ = nullptr;
};

}  // namespace urnw
