// A failed Solana wallet step (SolanaWalletPresentation.hpp): the sdk reads the
// failure and names its message (urnet::solanaWalletFailureFor), and this app
// shows each of the sdk's seven messages in its own string, with the wallet's
// name where the message takes one. Every failure of a Phantom or Solflare
// step (WalletConnect.cpp) reaches the user as that message and never as the
// bridge page's English, on the sign-in, the added sign-in method and the new
// network alike, and the strings are translated in every language the app
// ships.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "SolanaWalletPresentation.hpp"
#include "WiringSource.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace solana = urnw::solana;
namespace wiring = urnw::testing::wiring;

namespace {

std::string ReadFile(const std::string& relative) {
  const std::string candidates[] = {
      std::string(UR_SRC_DIR) + "/" + relative,
      "app/src/" + relative,
      "../app/src/" + relative,
      "linux/app/src/" + relative,
  };
  for (const auto& path : candidates) {
    std::ifstream in(path, std::ios::binary);
    if (!in) continue;
    std::ostringstream out;
    out << in.rdbuf();
    if (!out.str().empty()) return out.str();
  }
  return std::string();
}

}  // namespace

// Each of the sdk's seven messages (urnet::SolanaWalletFailureMessage*) reads in
// its own store string, which names the wallet exactly where its English has a
// {}. The table holds the seven, each once, and nothing else.
UR_TEST(SolanaWalletFailure_EveryMessageReadsInItsOwnWords) {
  struct Case {
    const char* key;
    bool takesWalletName;
  };
  const Case cases[] = {
      Case{"bittensor_error_user_rejected", false},
      Case{"no_wallets_found_alert_content", false},
      Case{"couldnt_open_wallet", false},
      Case{"bittensor_error_extension_not_found", true},
      Case{"bittensor_error_no_account", true},
      Case{"solana_wallet_error_session_not_found", false},
      Case{"bittensor_error_wallet_failed", true},
  };
  std::set<std::string> keys;
  for (const solana::FailureText& text : solana::kFailureTexts) keys.insert(text.key);
  UR_EXPECT_EQ(std::size(cases), std::size(solana::kFailureTexts));
  UR_EXPECT_EQ(std::size(cases), keys.size());
  for (const Case& c : cases) {
    const solana::FailureText text = solana::FailureTextFor(c.key);
    UR_EXPECT_TRUE_MSG(c.key, std::string(text.key) == c.key);
    UR_EXPECT_TRUE_MSG(c.key, text.takesWalletName == c.takesWalletName);
    UR_EXPECT_TRUE_MSG(c.key, (std::string(text.english).find("{}") != std::string::npos) ==
                                  c.takesWalletName);
  }
}

// A key this build does not know (a newer sdk's) reads as the wallet failing,
// as the sdk reads a code it does not know: a failure is always said in a
// message of the table, never left blank and never in other words.
UR_TEST(SolanaWalletFailure_AnyOtherKeyReadsAsTheWalletFailing) {
  // a failure's name and a bridge code (neither is a message key), another
  // flow's keys, a newer sdk's key, and none
  for (const char* key : {"user_rejected", "wallet_error", "-1", "wallet_sign_in_failed",
                          "bittensor_error_walletconnect_expired", "zzz_newer_message", ""}) {
    const solana::FailureText text = solana::FailureTextFor(key);
    UR_EXPECT_TRUE_MSG(key, std::string(text.key) == "bittensor_error_wallet_failed");
    UR_EXPECT_TRUE_MSG(key, text.takesWalletName);
  }
}

// The wallet a message names: the sdk's (it names Phantom or Solflare from the
// id the app passes), else the wallet the user chose, else Solana. A message
// that takes no name gets none, and no answer from the sdk reads as the wallet
// failing.
UR_TEST(SolanaWalletFailure_TheMessageNamesTheWallet) {
  struct Case {
    const char* key;
    const char* sdkWalletName;
    const char* chosenWalletName;
    const char* wantKey;
    const char* wantWalletName;
  };
  for (const Case& c : {
           Case{"bittensor_error_no_account", "Solflare", "Phantom", "bittensor_error_no_account",
                "Solflare"},
           Case{"bittensor_error_extension_not_found", "Phantom", "Phantom",
                "bittensor_error_extension_not_found", "Phantom"},
           Case{"bittensor_error_wallet_failed", "", "Phantom", "bittensor_error_wallet_failed",
                "Phantom"},
           Case{"bittensor_error_wallet_failed", "", "", "bittensor_error_wallet_failed", "Solana"},
           Case{"bittensor_error_user_rejected", "Phantom", "Phantom",
                "bittensor_error_user_rejected", ""},
           Case{"solana_wallet_error_session_not_found", "", "Solflare",
                "solana_wallet_error_session_not_found", ""},
           Case{"no_wallets_found_alert_content", "", "", "no_wallets_found_alert_content", ""},
           Case{"couldnt_open_wallet", "", "Phantom", "couldnt_open_wallet", ""},
           // a newer sdk's key, with a name or without one
           Case{"zzz_newer_message", "Backpack", "Phantom", "bittensor_error_wallet_failed",
                "Backpack"},
           Case{"zzz_newer_message", "", "Solflare", "bittensor_error_wallet_failed", "Solflare"},
           // no answer from the sdk
           Case{"", "", "Phantom", "bittensor_error_wallet_failed", "Phantom"},
           Case{"", "", "", "bittensor_error_wallet_failed", "Solana"},
       }) {
    const solana::FailureMessage message =
        solana::FailureMessageFor(c.key, c.sdkWalletName, c.chosenWalletName);
    const std::string what =
        std::string(c.key) + " / " + c.sdkWalletName + " / " + c.chosenWalletName;
    UR_EXPECT_TRUE_MSG(what, std::string(message.text.key) == c.wantKey);
    UR_EXPECT_TRUE_MSG(what, message.walletName == c.wantWalletName);
  }
}

// Every message is a key the linux catalog carries with its English as the
// msgid, translated in every language the app ships, with the wallet's {} kept.
UR_TEST(SolanaWalletFailure_StringsAreTranslated) {
  const std::string pot = ReadFile("../po/urnetwork.pot");
  const std::string linguas = ReadFile("../po/LINGUAS");
  if (pot.empty() || linguas.empty()) {
    UR_FAIL("could not read po/urnetwork.pot or po/LINGUAS");
    return;
  }
  std::vector<std::string> languages;
  std::istringstream lines(linguas);
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line[0] != '#') languages.push_back(line);
  }
  UR_EXPECT_TRUE(languages.size() >= 28);
  for (const solana::FailureText& text : solana::kFailureTexts) {
    const std::string key(text.key);
    const std::string entry =
        "msgctxt \"" + key + "\"\nmsgid \"" + std::string(text.english) + "\"\nmsgstr \"";
    UR_EXPECT_TRUE_MSG(key + " in the pot", pot.find(entry) != std::string::npos);
    for (const auto& language : languages) {
      const std::string po = ReadFile("../po/" + language + ".po");
      const size_t at = po.find(entry);
      UR_EXPECT_TRUE_MSG(key + " in " + language, at != std::string::npos);
      if (at == std::string::npos) continue;
      const size_t start = at + entry.size();
      const std::string translated = po.substr(start, po.find('"', start) - start);
      UR_EXPECT_TRUE_MSG(key + " translated in " + language, !translated.empty());
      if (language != "en") {
        UR_EXPECT_TRUE_MSG(key + " is English in " + language, translated != text.english);
      }
      UR_EXPECT_TRUE_MSG(key + " keeps {} in " + language,
                         (translated.find("{}") != std::string::npos) == text.takesWalletName);
    }
  }
}

// Every failure of a Phantom or Solflare step goes through FailSolana, which
// asks the sdk: the page's code on the connect and the sign step, a browser
// that would not open the page, and whatever the app finds wrong with a
// return. No step answers on_error itself, and neither hop opens the page
// where a failure answers the launcher's words (OpenUrl).
UR_TEST(SolanaWalletFailure_EverySolanaStepFailsThroughTheSdk) {
  const std::string wallet = wiring::ReadCode("WalletConnect.cpp");
  if (wallet.empty()) {
    UR_FAIL("could not read WalletConnect.cpp");
    return;
  }
  for (const char* step : {"void WalletConnect::Connect(Provider p)",
                           "void WalletConnect::SignMessage(const std::string& message)",
                           "void WalletConnect::OpenSolanaBridge(Provider p,",
                           "void WalletConnect::HandleConnect(Provider p,",
                           "void WalletConnect::HandleSignMessage(Provider p,"}) {
    const std::string body = wiring::FunctionBody(wallet, step);
    UR_EXPECT_TRUE_MSG(step, !body.empty());
    UR_EXPECT_TRUE_MSG(step, wiring::Contains(body, "FailSolana("));
    UR_EXPECT_TRUE_MSG(step, !wiring::Contains(body, "on_error"));
    UR_EXPECT_TRUE_MSG(step, !wiring::Contains(body, "OpenUrl("));
  }
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(wallet, "void WalletConnect::Connect("),
                                  "OpenSolanaBridge(p, url);"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(wallet, "void WalletConnect::SignMessage("),
                                  "OpenSolanaBridge(currentProvider_, url);"));
  // the wallet never saw the request: the sdk's wallet_error
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(wallet, "void WalletConnect::OpenSolanaBridge("),
      "FailSolana(p, urnet::SolanaWalletFailureSourceApp, urnet::SolanaWalletFailureWallet,"));
  // the page's code, -1 included, read by the sdk as a bridge code on both steps
  for (const char* handler :
       {"void WalletConnect::HandleConnect(", "void WalletConnect::HandleSignMessage("}) {
    UR_EXPECT_TRUE_MSG(handler,
                       wiring::Contains(wiring::FunctionBody(wallet, handler),
                                        "FailSolana(p, urnet::SolanaWalletFailureSourceBridge, "
                                        "params[\"errorCode\"],"));
  }
  // the sdk reads the failure for the wallet the user chose, and the user reads
  // this app's string for the sdk's message
  const std::string fail = wiring::FunctionBody(wallet, "void WalletConnect::FailSolana(");
  for (const char* want : {"urnet::solanaWalletFailureFor(source, code, walletId)",
                           "solana::FailureMessageFor(",
                           "g_dpgettext2(GETTEXT_PACKAGE, message.text.key, message.text.english)",
                           "Format(localized, message.walletName)"}) {
    UR_EXPECT_TRUE_MSG(want, wiring::Contains(fail, want));
  }
}

// The bridge page's English, and whatever the app found, reach the log and
// never the user: FailSolana logs the detail before it answers, and the answer
// is built from the table alone. The fallback that showed the page's text for
// a code the app did not know stays gone.
UR_TEST(SolanaWalletFailure_ThePagesWordsAreOnlyLogged) {
  const std::string wallet = wiring::ReadCode("WalletConnect.cpp");
  if (wallet.empty()) {
    UR_FAIL("could not read WalletConnect.cpp");
    return;
  }
  for (const char* handler :
       {"void WalletConnect::HandleConnect(", "void WalletConnect::HandleSignMessage("}) {
    UR_EXPECT_TRUE_MSG(handler, wiring::Contains(wiring::FunctionBody(wallet, handler),
                                                 "params[\"errorMessage\"]"));
  }
  const std::string fail = wiring::FunctionBody(wallet, "void WalletConnect::FailSolana(");
  const size_t answer = fail.find("if (!on_error) return;");
  UR_EXPECT_TRUE(answer != std::string::npos);
  if (answer == std::string::npos) return;
  UR_EXPECT_TRUE(wiring::Contains(fail.substr(0, answer), "std::fprintf(stderr,"));
  UR_EXPECT_TRUE(wiring::Contains(fail.substr(0, answer), "detail.c_str()"));
  UR_EXPECT_FALSE(wiring::Contains(fail.substr(answer), "detail"));
  UR_EXPECT_FALSE(wiring::Contains(fail.substr(answer), "code"));
  UR_EXPECT_FALSE(wiring::Contains(wallet, "LocalizedBridgeError"));
}

// The message FailSolana answers is what each flow shows: the host hands the
// wallet's error to the waiting flow as it came, and the sign-in screen, the
// add sign-in sheet and the new network page show a flow's error as it came.
UR_TEST(SolanaWalletFailure_EveryFlowShowsTheMessage) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string onError =
      wiring::FunctionBody(host, "wallet_.on_error = [this](std::string err)");
  UR_EXPECT_TRUE(!onError.empty());
  for (const char* want :
       {"out.error = err;", "addDone({false, err});", "done({false, false, err});"}) {
    UR_EXPECT_TRUE_MSG(want, wiring::Contains(onError, want));
  }
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window, "void MainWindow::OnWalletAuth(const AuthResult& result)"),
      "SetLoginError(SignInFailureText("));
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window, "Glib::ustring SignInFailureText(client_limit::Prompt"),
      "return error.empty() ? Glib::ustring(screenLine) : Glib::ustring(error);"));
  const std::string account = wiring::ReadCode("AccountPage.cpp");
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(account, "std::function<void(SdkHost::AddSignInResult)> Answer("),
      ": result.error;"));
  const std::string views = wiring::ReadCode("AuthViews.cpp");
  UR_EXPECT_TRUE(
      wiring::Contains(wiring::FunctionBody(views, "void CreateNetworkPage::OnContinue()"),
                       ": Glib::ustring(r.error));"));
}

// g_dpgettext2 answers a string with no translation (no catalog for the
// language, or none installed) with the msgid pointer it was given, so the
// msgid must outlive the answer: a translation kept in a variable must not
// come from a temporary std::string.
UR_TEST(SolanaWalletFailure_NoTranslationOutlivesItsMsgid) {
  const std::string wallet = ReadFile("WalletConnect.cpp");
  if (wallet.empty()) {
    UR_FAIL("could not read WalletConnect.cpp");
    return;
  }
  const std::regex keptFromTemporary(
      R"(const\s+char\s*\*\s*\w+\s*=\s*g_dpgettext2\([^;]*std::string\()");
  UR_EXPECT_FALSE(std::regex_search(wallet, keptFromTemporary));
}
