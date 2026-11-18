// The account's cap of active sessions on the sign-in screens
// (SessionLimitNotice.hpp; session1118 C.md §1, §5 A). Every route that starts
// a session answers a refusal at the cap as its result, with the sdk's code
// (SessionErrorCodeLimitReached). Before this the screens showed the server's
// English, or worse a wrong line ("There was a problem authenticating your
// verification code." for a code that was good). Now each says, in the store's
// words, where to sign sessions out, as it says what to do about the client
// limit; nothing retries and nothing signs out.
//
// The decision is pure and runs here; the routes and screens need the SDK and
// GTK, so their sources are read with the comments blanked
// (WiringSource.hpp). AuthSessionRegression.cpp runs the routes themselves
// against a refusing fake.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "SessionLimitNotice.hpp"
#include "WiringSource.hpp"

namespace sl = urnw::session_limit;
namespace cl = urnw::client_limit;
namespace wiring = urnw::testing::wiring;

namespace {

// Each needle occurs after the one before it.
bool SessionLimitInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

// Only the sdk's code is the cap's refusal: the retention cap's is another on
// purpose (signing sessions out does not free it), and so is any other code.
UR_TEST(SessionLimitNotice_OnlyTheSdksCodeIsTheCap) {
  UR_EXPECT_TRUE(std::string(sl::kRefusalCode) == "session_limit_reached");
  UR_EXPECT_TRUE(sl::Refused(std::string("session_limit_reached")));
  UR_EXPECT_FALSE(sl::Refused(std::nullopt));
  for (const char* other : {"", "session_retention_limit_reached", "SESSION_LIMIT_REACHED",
                            "session_limit_reached ", "client_limit_exceeded", "invalid_password"}) {
    UR_EXPECT_TRUE_MSG(std::string("\"") + other + "\"", !sl::Refused(std::string(other)));
  }
  // the mirrored code is the sdk's, or the GUI does not build
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::ReadCode("SdkHost.cpp"),
      "static_assert(std::string_view(session_limit::kRefusalCode) ==\n"
      "                  std::string_view(urnet::SessionErrorCodeLimitReached),"));
}

// The cap's line first, then the client limit's for its prompt, else nothing:
// the screen keeps the server's words or its own line.
UR_TEST(SessionLimitNotice_ARefusedSignInSaysWhereToSignOut) {
  UR_EXPECT_TRUE(sl::SignInRefusalBody(true, cl::Prompt::None) == &sl::kSignInBody);
  // the sdk never sends both; the cap would still be what is said
  UR_EXPECT_TRUE(sl::SignInRefusalBody(true, cl::Prompt::Upgrade) == &sl::kSignInBody);
  UR_EXPECT_TRUE(sl::SignInRefusalBody(true, cl::Prompt::RemoveClient) == &sl::kSignInBody);
  UR_EXPECT_TRUE(sl::SignInRefusalBody(false, cl::Prompt::Upgrade) == &cl::kUpgradeBody);
  UR_EXPECT_TRUE(sl::SignInRefusalBody(false, cl::Prompt::RemoveClient) == &cl::kRemoveClientBody);
  UR_EXPECT_TRUE(sl::SignInRefusalBody(false, cl::Prompt::None) == nullptr);
}

UR_TEST(SessionLimitNotice_TheWordsAreTheCatalogs) {
  UR_EXPECT_TRUE(std::string(sl::kSignInBody.key) == "sessions_limit_reached_sign_in");
  std::ifstream in(std::string(UR_SRC_DIR) + "/../po/en.po", std::ios::binary);
  UR_EXPECT_TRUE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string entry = std::string("msgctxt \"") + sl::kSignInBody.key + "\"\nmsgid \"" +
                            sl::kSignInBody.english + "\"\n";
  UR_EXPECT_TRUE_MSG(entry, buffer.str().find(entry) != std::string::npos);
}

// Every route that starts a session reads the code from its result, sets the
// flag the screens read, and does so ahead of the path that hands on the
// server's words. Discovery's own sign-in hands the flag on.
UR_TEST(SessionLimitWiring_EverySignInRouteReadsTheCode) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  for (const char* signature :
       {"void SdkHost::StartLogin(", "void SdkHost::LoginWithPassword(",
        "void SdkHost::LoginWithCode(", "void SdkHost::LoginWithSeedphrase(",
        "void SdkHost::CreateInstantAccount(", "void SdkHost::HandleNetworkCreateResult(",
        "void SdkHost::VerifyCode(", "void SdkHost::AuthLoginWithSso(",
        "void SdkHost::AuthLoginWithWallet("}) {
    const std::string body = wiring::FunctionBody(host, signature);
    UR_EXPECT_TRUE_MSG(signature, SessionLimitInOrder(
                                      body, {"if (result->error && session_limit::Refused(result->error->code)) {",
                                             "sessionLimit = true;", "return;",
                                             "if (result->error && !result->error->message.empty())"}));
  }
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(host, "void SdkHost::StartLogin("),
                                  "routed.sessionLimit = r.sessionLimit;"));
  // the three network creations all answer through the one reading
  for (const char* signature :
       {"void SdkHost::CreateNetwork(", "void SdkHost::CreateNetworkWithPendingSso(",
        "void SdkHost::FinishCreateNetworkWithWallet("}) {
    UR_EXPECT_TRUE_MSG(signature, wiring::Contains(wiring::FunctionBody(host, signature),
                                                   "HandleNetworkCreateResult("));
  }
}

// The device client registered after a sign-in: the cap's refusal is read
// after the client limit's and ahead of the server's words, and it offers no
// device list and keeps nothing to register again (no removal frees it).
UR_TEST(SessionLimitWiring_TheDeviceClientsRefusalOffersNoList) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string registration =
      wiring::FunctionBody(host, "void SdkHost::RegisterNetworkClient(const std::string& byJwt,");
  UR_EXPECT_TRUE(SessionLimitInOrder(
      registration,
      {"refused.clientLimitPrompt = ToClientLimitPrompt(result->error->client_limit_prompt);",
       "if (onClientLimitRefusal_) onClientLimitRefusal_(refused.clientLimitPrompt);",
       "if (session_limit::Refused(result->error->code)) {", "refused.sessionLimit = true;",
       "done(refused);", "if (result->error && !result->error->message.empty()) {"}));
  const size_t cap = registration.find("if (session_limit::Refused(result->error->code)) {");
  const size_t capEnd = registration.find("return;", cap);
  UR_EXPECT_TRUE(cap != std::string::npos && capEnd != std::string::npos);
  if (cap != std::string::npos && capEnd != std::string::npos) {
    const std::string capBlock = registration.substr(cap, capEnd - cap);
    UR_EXPECT_FALSE(wiring::Contains(capBlock, "refusedSignIn_"));
    UR_EXPECT_FALSE(wiring::Contains(capBlock, "onClientLimitRefusal_"));
  }
}

// Every sign-in screen that shows a refusal shows the cap's line where it shows
// its errors: the window's shared words (password, code, recovery phrase,
// wallets and sso, discovery, the instant account and the device list's retry)
// and the create and verify pages.
UR_TEST(SessionLimitWiring_EverySignInScreenShowsIt) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  const std::string text = wiring::FunctionBody(
      window, "Glib::ustring SignInFailureText(bool sessionLimit, client_limit::Prompt clientLimitPrompt,");
  UR_EXPECT_TRUE(SessionLimitInOrder(
      text, {"session_limit::SignInRefusalBody(sessionLimit, clientLimitPrompt)",
             "return T_(body->key, body->english);", "error.empty()"}));
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window, "Glib::ustring SignInFailureText(const AuthResult& r,"),
      "return SignInFailureText(r.sessionLimit, r.clientLimitPrompt, r.error, screenLine);"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(window, "void MainWindow::OnGetStarted() {"),
                                  "routing.sessionLimit, routing.clientLimitPrompt, routing.error,"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(window, "void MainWindow::OnInstantSubmit() {"),
                                  "account.sessionLimit, client_limit::Prompt::None, account.error,"));
  for (const char* screen :
       {"void MainWindow::OnSignIn() {", "void MainWindow::OnSeedphraseSubmit() {",
        "void MainWindow::OnUseCode() {", "void MainWindow::OnWalletAuth(const AuthResult& result) {",
        "void MainWindow::OnRefusedSignInRetry(const AuthResult& result) {"}) {
    UR_EXPECT_TRUE_MSG(screen, wiring::Contains(wiring::FunctionBody(window, screen),
                                                "SignInFailureText("));
  }

  const std::string views = wiring::ReadCode("AuthViews.cpp");
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(views, "Glib::ustring SignInRefusalText(const AuthResult& r) {"),
      "session_limit::SignInRefusalBody(r.sessionLimit, r.clientLimitPrompt);"));
  UR_EXPECT_TRUE(SessionLimitInOrder(wiring::FunctionBody(views, "void CreateNetworkPage::OnContinue() {"),
                                     {"SignInRefusalText(r)", "error_creating_network"}));
  // a code that verified is not a bad code
  UR_EXPECT_TRUE(SessionLimitInOrder(views, {"host_.VerifyCode(userAuth_, code,",
                                             "SignInRefusalText(r)", "verify_input_invalid"}));
}
