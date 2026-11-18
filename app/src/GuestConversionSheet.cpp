// SPDX-License-Identifier: MPL-2.0
#include "GuestConversionSheet.hpp"

#include <optional>
#include <string>
#include <utility>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "SessionLimitNotice.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

constexpr int kSheetWidth = 380;

std::string FirstMessage(const std::string& serverMessage, const std::optional<std::string>& err) {
  if (!serverMessage.empty()) return serverMessage;
  if (err && !err->empty()) return *err;
  return T_("something_went_wrong", "Something went wrong.");
}

// The SDK side of the conversion. Every answer is posted to the main loop; an
// answer that arrives after the sheet is gone is dropped by `alive`.
class SdkGuestConversionSession : public GuestConversionSession {
 public:
  SdkGuestConversionSession(SdkHost& host, SubscriptionBalanceStore& balance)
      : host_(host), balance_(balance) {}
  ~SdkGuestConversionSession() override { *alive_ = false; }

  void AddSignIn(const std::string& userAuth, const std::string& password,
                 std::function<void(std::string error)> done) override {
    urnet::AddAuthArgs args{};
    args.user_auth = userAuth;
    args.password = password;
    host_.api().addAuth(std::optional<urnet::AddAuthArgs>(args),
                        [alive = alive_, done = std::move(done)](
                            std::optional<urnet::AddAuthResult> result,
                            std::optional<std::string> err) {
                          std::string error;
                          if (err || !result || result->error) {
                            error = FirstMessage(result && result->error ? result->error->message
                                                                         : std::string(),
                                                 err);
                          }
                          PostToMain([alive, done, error] {
                            if (*alive) done(error);
                          });
                        });
  }

  void RefreshJwt() override { host_.RefreshJwt(); }

  void RefreshBalance() override { balance_.FetchNow(); }

  void SendCode(const std::string& userAuth,
                std::function<void(VerifySendNotice notice)> done) override {
    host_.ResendVerifyCode(userAuth, [alive = alive_, done = std::move(done)](
                                         VerifySendNotice notice) {
      PostToMain([alive, done, notice] {
        if (*alive) done(notice);
      });
    });
  }

  // authVerify without SdkHost::VerifyCode's sign-in: the returned jwt is for
  // this same network, and the session already holds one.
  void VerifyCode(const std::string& userAuth, const std::string& code,
                  std::function<void(std::string error)> done) override {
    urnet::AuthVerifyArgs args{};
    args.user_auth = userAuth;
    args.verify_code = code;
    host_.api().authVerify(std::optional<urnet::AuthVerifyArgs>(args),
                           [alive = alive_, done = std::move(done)](
                               std::optional<urnet::AuthVerifyResult> result,
                               std::optional<std::string> err) {
                             std::string error;
                             if (result && result->error &&
                                 session_limit::Refused(result->error->code)) {
                               // the account's session cap refused the
                               // session the verification mints
                               error = T_(session_limit::kSignedInBody.key,
                                          session_limit::kSignedInBody.english);
                             } else if (err || !result || result->error) {
                               error = FirstMessage(
                                   result && result->error ? result->error->message
                                                           : std::string(),
                                   err);
                             }
                             PostToMain([alive, done, error] {
                               if (*alive) done(error);
                             });
                           });
  }

 private:
  SdkHost& host_;
  SubscriptionBalanceStore& balance_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

Gtk::Label* MakeLabel(const std::string& text, const char* cssClass) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class(cssClass);
  label->set_xalign(0);
  label->set_wrap(true);
  return label;
}

}  // namespace

GuestConversionSheet::GuestConversionSheet(Gtk::Window& parent, SdkHost& host,
                                           SubscriptionBalanceStore& balance)
    : session_(std::make_unique<SdkGuestConversionSession>(host, balance)),
      conversion_(std::make_unique<GuestConversion>(*session_)) {
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("create_an_account", "Create an account"));
  set_default_size(kSheetWidth, -1);
  set_resizable(false);
  set_hide_on_close(true);
  add_css_class("ur-sheet");
  AddEscapeToClose(*this);

  // ---- page 1: the sign-in to add ----
  auto* signIn = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  signIn->set_margin(24);
  signIn->set_size_request(kSheetWidth, -1);
  signIn->append(*MakeLabel(T_("create_an_account", "Create an account"), "ur-step-heading"));
  signIn->append(*MakeLabel(
      T_("guest_convert_explanation",
         "Add an email and a password to sign in to this network. Its plan and balance stay "
         "with it."),
      "dim-label"));
  signIn->append(*MakeLabel(T_("your_email", "Your email"), "ur-input-label"));
  auth_ = Gtk::make_managed<Gtk::Entry>();
  auth_->add_css_class("ur-input");
  auth_->signal_changed().connect([this] { Render(); });
  signIn->append(*auth_);
  signIn->append(*MakeLabel(T_("password_label", "Password"), "ur-input-label"));
  password_ = Gtk::make_managed<Gtk::PasswordEntry>();
  password_->add_css_class("ur-input");
  password_->set_show_peek_icon(true);
  password_->signal_changed().connect([this] { Render(); });
  signIn->append(*password_);
  signIn->append(*MakeLabel(T_("password_must_be_at_least_12_characters_long",
                               "Password must be at least 12 characters long"),
                            "ur-caption"));
  signInError_ = MakeLabel({}, "ur-error-text");
  signIn->append(*signInError_);
  auto* signInActions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  signInActions->set_halign(Gtk::Align::END);
  auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
  cancel->signal_clicked().connect([this] { set_visible(false); });
  signInActions->append(*cancel);
  add_ = Gtk::make_managed<Gtk::Button>(T_("add", "Add"));
  add_->add_css_class("suggested-action");
  add_->signal_clicked().connect([this] {
    conversion_->SubmitSignIn(auth_->get_text().raw(), password_->get_text().raw());
  });
  signInActions->append(*add_);
  signIn->append(*signInActions);
  stack_.add(*signIn, "sign-in");

  // ---- page 2: verify the added sign-in ----
  auto* verifyPage = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  verifyPage->set_margin(24);
  verifyPage->set_size_request(kSheetWidth, -1);
  verifyPage->append(*MakeLabel(T_("login_verify_header", "You've got mail"), "ur-step-heading"));
  verifyPage->append(*MakeLabel(
      T_("verify_explanation",
         "Tell us who you really are. Enter the code we sent you to verify your identity."),
      "dim-label"));
  code_ = Gtk::make_managed<Gtk::Entry>();
  code_->add_css_class("ur-otp");
  code_->set_placeholder_text(T_("verify_input_label", "Verification code"));
  code_->signal_changed().connect([this] { Render(); });
  code_->signal_activate().connect([this] { conversion_->SubmitCode(code_->get_text().raw()); });
  verifyPage->append(*code_);
  codeError_ = MakeLabel({}, "ur-error-text");
  verifyPage->append(*codeError_);
  notice_ = MakeLabel({}, "ur-caption");
  verifyPage->append(*notice_);
  auto* verifyActions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  verifyActions->set_halign(Gtk::Align::END);
  resend_ = Gtk::make_managed<Gtk::Button>(T_("resend_verify_code", "Resend code"));
  resend_->add_css_class("flat");
  resend_->signal_clicked().connect([this] { conversion_->Resend(); });
  verifyActions->append(*resend_);
  verify_ = Gtk::make_managed<Gtk::Button>(T_("verify", "Verify"));
  verify_->add_css_class("suggested-action");
  verify_->signal_clicked().connect([this] { conversion_->SubmitCode(code_->get_text().raw()); });
  verifyActions->append(*verify_);
  verifyPage->append(*verifyActions);
  stack_.add(*verifyPage, "verify");

  set_child(stack_);
  conversion_->on_changed = [this] { Render(); };
}

GuestConversionSheet::~GuestConversionSheet() {
  cooldownTick_.disconnect();
  // the conversion goes first: it drops its answers before the session goes
  conversion_.reset();
  session_.reset();
}

void GuestConversionSheet::Open() {
  cooldownTick_.disconnect();
  conversion_->Reset();  // first: clearing the fields re-renders
  auth_->set_text("");
  password_->set_text("");
  code_->set_text("");
  notice_->set_text("");
  Render();
  present();
}

void ShowVerifySendNotice(Gtk::Label& label, const std::optional<VerifySendNotice>& notice) {
  label.remove_css_class("ur-error-text");
  if (!notice) {
    label.set_text("");
    return;
  }
  switch (notice->kind) {
    case VerifySendNoticeKind::Sent:
      label.set_text(T_("verification_code_sent",
                        "Check your email/phone for a verification code."));
      return;
    case VerifySendNoticeKind::RateLimited:
      label.set_text(Format(TN_("verify_code_rate_limited",
                                "Too many attempts. You can request a new code in {} minute.",
                                "Too many attempts. You can request a new code in {} minutes.",
                                static_cast<unsigned long>(notice->minutes)),
                            notice->minutes));
      break;
    case VerifySendNoticeKind::SendFailed:
      label.set_text(T_("error_sending_verification_code",
                        "There was an error sending the verification code."));
      break;
    case VerifySendNoticeKind::ServerMessage:
      label.set_text(notice->message);
      break;
  }
  label.add_css_class("ur-error-text");
}

void TickCooldown(sigc::connection& tick, const GuestConversion& conversion,
                  std::function<void()> render) {
  if (!conversion.CoolingDown()) {
    tick.disconnect();
    return;
  }
  if (tick.connected()) return;
  tick = Glib::signal_timeout().connect_seconds(
      [&conversion, render = std::move(render)]() -> bool {
        // the last render (once the cooldown has passed) re-enables Resend
        const bool cooling = conversion.CoolingDown();
        render();
        return cooling;
      },
      1);
}

void GuestConversionSheet::Render() {
  const GuestConversionStep step = conversion_->Step();
  const bool busy = conversion_->Busy();
  switch (step) {
    case GuestConversionStep::EnterSignIn:
    case GuestConversionStep::AddingSignIn:
      stack_.set_visible_child("sign-in");
      signInError_->set_text(conversion_->Error());
      signInError_->set_visible(!conversion_->Error().empty());
      auth_->set_sensitive(!busy);
      password_->set_sensitive(!busy);
      add_->set_sensitive(!busy && GuestConversion::CanSubmitSignIn(auth_->get_text().raw(),
                                                                     password_->get_text().raw()));
      return;
    case GuestConversionStep::EnterCode:
    case GuestConversionStep::Verifying:
      stack_.set_visible_child("verify");
      codeError_->set_text(conversion_->Error());
      codeError_->set_visible(!conversion_->Error().empty());
      ShowVerifySendNotice(*notice_, conversion_->ShownNotice());
      TickCooldown(cooldownTick_, *conversion_, [this] { Render(); });
      resend_->set_sensitive(conversion_->CanResend());
      verify_->set_sensitive(!busy && !GuestConversion::Trim(code_->get_text().raw()).empty());
      return;
    case GuestConversionStep::Done:
      // on_done before the hide: the window's hide handler reads it
      if (on_done) on_done();
      set_visible(false);
      return;
  }
}

}  // namespace urnw
