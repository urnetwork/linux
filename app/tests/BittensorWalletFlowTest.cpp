// The Bittensor wallet-connect adoption (UPGRADE.md 4.6): the SDK session
// judges every wallet answer; the app maps each refusal onto the flow (drop or
// fail) and onto a store string, and the bridge return goes through the session
// rather than an ad-hoc query parse.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "BittensorWalletFlow.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace bt = urnw::bittensor;

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

std::string FunctionBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

}  // namespace

// A proof continues the flow; an answer for another purpose, a late return
// after cancel or a replay is not this flow's and must not end it.
UR_TEST(BittensorWalletFlow_ProofsContinueAndForeignAnswersAreDropped) {
  UR_EXPECT_TRUE(bt::Classify("") == bt::Outcome::Accept);
  // unsupported_wallet: a return naming another wallet's page (a stale tab
  // from an earlier choice) is not this flow's either
  for (const char* code : {"purpose_mismatch", "not_bittensor_return", "not_awaiting_wallet",
                           "unsupported_wallet"}) {
    UR_EXPECT_TRUE_MSG(code, bt::Classify(code) == bt::Outcome::Ignore);
  }
}

// A refusal of a bridge return ends the attempt (the page cannot be asked again).
UR_TEST(BittensorWalletFlow_BridgeRefusalsFail) {
  for (const char* code : {"invalid_signature", "address_mismatch", "invalid_ss58_address",
                           "challenge_expired", "message_mismatch", "wallet_error"}) {
    UR_EXPECT_TRUE_MSG(code, bt::Classify(code) == bt::Outcome::Fail);
  }
}

// Every refusal with a string names a key the linux catalog carries, with the
// English as its msgid (g_dpgettext2 looks up context AND msgid: a drifted
// English silently shows untranslated text).
UR_TEST(BittensorWalletFlow_RefusalStringsAreInTheCatalog) {
  const std::string pot = ReadFile("../po/urnetwork.pot");
  if (pot.empty()) {
    UR_FAIL("could not read po/urnetwork.pot");
    return;
  }
  int checked = 0;
  for (const char* code : {"invalid_signature", "challenge_expired", "message_mismatch",
                           "address_mismatch", "invalid_ss58_address"}) {
    const bt::ErrorText text = bt::ErrorTextFor(code);
    UR_EXPECT_TRUE_MSG(code, !text.key.empty());
    const std::string entry = "msgctxt \"" + std::string(text.key) + "\"\nmsgid \"" +
                              std::string(text.english) + "\"";
    UR_EXPECT_TRUE_MSG(std::string(code) + " -> " + std::string(text.key),
                       pot.find(entry) != std::string::npos);
    ++checked;
  }
  UR_EXPECT_TRUE(checked == 5);
  // the wallet's own words, or the generic failure
  UR_EXPECT_TRUE(bt::ErrorTextFor("wallet_error").key.empty());
  UR_EXPECT_TRUE(bt::ErrorTextFor("").key.empty());
}

// A wallet_error carries the bridge page's code for the failure
// (BridgeErrorCode): a code this app knows reads in its own words, with the
// wallet's name where the text has a {}; any other shows the page's text.
UR_TEST(BittensorWalletFlow_BridgeCodesReadInTheAppsWords) {
  struct Case {
    const char* code;
    const char* key;
    bool takesWalletName;
  };
  for (const Case& c : {
           Case{"address_not_in_wallet", "bittensor_error_address_not_in_wallet", true},
           Case{"address_mismatch", "earnings_wallet_mismatch", false},
           Case{"extension_not_found", "bittensor_error_extension_not_found", true},
           Case{"no_account", "bittensor_error_no_account", true},
           // the follow-up from support inbox 8142: the page used to hand back
           // wallet_error for this, with the extension's own English
           Case{"extension_not_authorized", "bittensor_error_extension_not_authorized", true},
           Case{"user_rejected", "bittensor_error_user_rejected", false},
           Case{"walletconnect_expired", "bittensor_error_walletconnect_expired", false},
           Case{"walletconnect_unavailable", "bittensor_error_walletconnect_unavailable", false},
       }) {
    const bt::BridgeErrorText text = bt::BridgeErrorTextFor(c.code);
    UR_EXPECT_TRUE_MSG(c.code, text.key == c.key);
    UR_EXPECT_TRUE_MSG(c.code, text.takesWalletName == c.takesWalletName);
    UR_EXPECT_TRUE_MSG(c.code, (text.english.find("{}") != std::string_view::npos) ==
                                   c.takesWalletName);
  }
  // a code this app does not know, the page's other failures, and a page
  // before the codes (no code): the page's own text
  for (const char* code : {"wallet_locked", "wallet_error", "invalid_request", ""}) {
    UR_EXPECT_TRUE_MSG(code, bt::BridgeErrorTextFor(code).key.empty());
  }
  // only a wallet_error is the page's: the refusals with their own string keep it
  UR_EXPECT_TRUE(bt::ErrorTextFor("wallet_error").key.empty());
}

// Every bridge code text is a key the linux catalog carries with its English
// as the msgid, translated in every language the app ships.
UR_TEST(BittensorWalletFlow_BridgeCodeStringsAreTranslated) {
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
  int checked = 0;
  for (const char* code : {"address_not_in_wallet", "extension_not_found", "no_account",
                           "extension_not_authorized", "user_rejected", "walletconnect_expired",
                           "walletconnect_unavailable"}) {
    const bt::BridgeErrorText text = bt::BridgeErrorTextFor(code);
    const std::string entry = "msgctxt \"" + std::string(text.key) + "\"\nmsgid \"" +
                              std::string(text.english) + "\"\nmsgstr \"";
    UR_EXPECT_TRUE_MSG(std::string(code) + " in the pot", pot.find(entry) != std::string::npos);
    for (const auto& language : languages) {
      const std::string po = ReadFile("../po/" + language + ".po");
      const size_t at = po.find(entry);
      UR_EXPECT_TRUE_MSG(std::string(code) + " in " + language, at != std::string::npos);
      if (at == std::string::npos) continue;
      const size_t start = at + entry.size();
      const std::string translated = po.substr(start, po.find('"', start) - start);
      UR_EXPECT_TRUE_MSG(std::string(code) + " translated in " + language, !translated.empty());
      UR_EXPECT_TRUE_MSG(std::string(code) + " keeps {} in " + language,
                         (translated.find("{}") != std::string::npos) == text.takesWalletName);
    }
    ++checked;
  }
  UR_EXPECT_TRUE(checked == 7);
}

// A failed answer shows its refusal's string, else (a wallet_error only) the
// bridge page's code in this app's words, else the page's own words, else the
// generic failure. Another refusal's detail is never shown as words: it is
// the refused address or the session's transport.
UR_TEST(BittensorWalletFlow_FailuresShowTheirOwnText) {
  using bt::FailureText;
  UR_EXPECT_TRUE(bt::FailureTextFor("challenge_expired", "", false) == FailureText::Refusal);
  UR_EXPECT_TRUE(bt::FailureTextFor("address_mismatch", "", true) == FailureText::Refusal);
  UR_EXPECT_TRUE(bt::FailureTextFor("wallet_error", "user_rejected", true) ==
                 FailureText::BridgeCode);
  UR_EXPECT_TRUE(bt::FailureTextFor("wallet_error", "address_not_in_wallet", false) ==
                 FailureText::BridgeCode);
  UR_EXPECT_TRUE(bt::FailureTextFor("wallet_error", "wallet_locked", true) ==
                 FailureText::PageText);
  UR_EXPECT_TRUE(bt::FailureTextFor("wallet_error", "", true) == FailureText::PageText);
  UR_EXPECT_TRUE(bt::FailureTextFor("wallet_error", "", false) == FailureText::Generic);
  // a session refusal's detail (here the transport name) is not for the user
  UR_EXPECT_TRUE(bt::FailureTextFor("wrong_transport", "", true) == FailureText::Generic);
  UR_EXPECT_TRUE(bt::FailureTextFor("wrong_transport", "user_rejected", true) ==
                 FailureText::Generic);
  UR_EXPECT_TRUE(bt::FailureTextFor("no_challenge", "", false) == FailureText::Generic);
}

// The bridge's failure goes through the page's code to the user: the
// delivery picks its text by FailureTextFor and names the session's wallet.
UR_TEST(BittensorWalletFlow_TheBridgeCodeReachesTheUser) {
  const std::string wallet = ReadFile("WalletConnect.cpp");
  if (wallet.empty()) {
    UR_FAIL("could not read WalletConnect.cpp");
    return;
  }
  const std::string deliver = FunctionBody(wallet, "void WalletConnect::DeliverBittensorResult(");
  UR_EXPECT_TRUE_MSG("the text is chosen with the page's code",
                     deliver.find("bittensor::FailureTextFor(result.ErrorCode, result.BridgeErrorCode,") !=
                         std::string::npos);
  UR_EXPECT_TRUE_MSG("the page's code reads in this app's words",
                     deliver.find("bittensor::BridgeErrorTextFor(result.BridgeErrorCode)") !=
                         std::string::npos);
  UR_EXPECT_TRUE_MSG("with the session's wallet name",
                     deliver.find("urnet::bittensorWalletDisplayName(bittensorSession_->walletId())") !=
                         std::string::npos);
}

// The bridge returns on the scheme the .desktop file already registers; no
// new handler is added.
UR_TEST(BittensorWalletFlow_TheRedirectIsTheRegisteredScheme) {
  UR_EXPECT_TRUE(bt::kRedirectLink == "urnetwork://bittensor-sign-message");
  const std::string desktop = ReadFile("../packaging/com.bringyour.network.desktop");
  if (desktop.empty()) {
    UR_FAIL("could not read packaging/com.bringyour.network.desktop");
    return;
  }
  UR_EXPECT_TRUE(desktop.find("x-scheme-handler/urnetwork;") != std::string::npos);
}

// The bridge page's "Enter address manually" is the desktop's manual path: the
// user signs the challenge outside the browser (btcli, a wallet's own tool) and
// pastes the result, and the server takes it for the challenge's 5 minutes. A
// return after the watchdog fired is dropped for good (EarningsPage::BeginFlow),
// so the wait has to outlast the challenge, or a proof the server would still
// take is lost.
UR_TEST(BittensorWalletFlow_TheBridgeWaitsOutTheChallenge) {
  constexpr uint32_t kChallengeLifetimeMs = 300'000;  // the server's WalletAuthChallengeLifetime
  UR_EXPECT_TRUE(bt::kBridgeTimeoutMs > kChallengeLifetimeMs);
  // the coldkey connect waits on it, as the payout consent does (NetworkConsentTest)
  const std::string start =
      FunctionBody(ReadFile("EarningsPage.cpp"), "void EarningsPage::StartWalletSignature(");
  UR_EXPECT_TRUE(start.find("BeginFlow(connectFlow_, static_cast<int>(bittensor::kBridgeTimeoutMs),") !=
                 std::string::npos);
}

// The bridge is how a session signs on linux: every chooser wallet opens the
// page (the SDK's BittensorWalletTransportFor), and the app has no form of its
// own. A session of any other transport has no bridge url, so asking for one
// answers the flow with the session's refusal at once: nothing may end the hop
// quietly and leave the flow waiting out its watchdog with nothing on screen.
UR_TEST(BittensorWalletFlow_ASessionOffTheBridgeFailsAtOnce) {
  const std::string wallet = ReadFile("WalletConnect.cpp");
  const std::string open = FunctionBody(wallet, "void WalletConnect::SignWithBittensor(");
  UR_EXPECT_TRUE_MSG("the hop is read", !open.empty());
  UR_EXPECT_TRUE_MSG("the transport does not end the hop", open.find("transport()") == std::string::npos);
  const size_t url = open.find("url = bittensorSession_->bridgeUrl();");
  const size_t refusal = open.find("on_error(e.what())", url);
  const size_t opened = open.find("OpenUrl(url);", url);
  UR_EXPECT_TRUE_MSG("the session's refusal reaches the flow before anything opens",
                     url != std::string::npos && refusal != std::string::npos &&
                         opened != std::string::npos && refusal < opened);
  // and the host keeps no other way to take an answer
  const std::string start = FunctionBody(ReadFile("SdkHost.cpp"), "void SdkHost::StartBittensorSession(");
  UR_EXPECT_TRUE_MSG("the session starts", !start.empty());
  UR_EXPECT_TRUE_MSG("no other transport has a step", start.find("transport()") == std::string::npos);
  UR_EXPECT_TRUE(wallet.find("handleSignature") == std::string::npos);
}

// The root cause this adoption removes: the app parsed the bridge return
// itself and accepted any address + signature, without checking that the
// message was the challenge it issued, that the purpose was its own, or that
// the signature was a signature. The return must go through the session.
UR_TEST(BittensorWalletFlow_TheBridgeReturnIsJudgedByTheSession) {
  const std::string wallet = ReadFile("WalletConnect.cpp");
  if (wallet.empty()) {
    UR_FAIL("could not read WalletConnect.cpp");
    return;
  }
  const std::string ret = FunctionBody(wallet, "void WalletConnect::HandleBittensorReturn(");
  UR_EXPECT_TRUE_MSG("the bittensor return is handled", !ret.empty());
  UR_EXPECT_TRUE_MSG("the session judges the return",
                     ret.find("->handleBridgeReturn(url,") != std::string::npos);
  UR_EXPECT_TRUE_MSG("the return is not parsed by hand",
                     ret.find("ParseQuery") == std::string::npos &&
                         wallet.find("params[\"signature\"]") == std::string::npos);
  const std::string open = FunctionBody(wallet, "void WalletConnect::SignWithBittensor(");
  UR_EXPECT_TRUE_MSG("the bridge url is the session's", open.find("->bridgeUrl()") != std::string::npos);
  UR_EXPECT_TRUE_MSG("no WalletConnect project id is sent for Bittensor",
                     wallet.find("wc_project_id") == std::string::npos);
  const std::string deliver = FunctionBody(wallet, "void WalletConnect::DeliverBittensorResult(");
  const size_t classify = deliver.find("bittensor::Classify(");
  const size_t signature = deliver.find("on_signature(result.Proof->Signature)");
  UR_EXPECT_TRUE_MSG("only an accepted proof reaches on_signature",
                     classify != std::string::npos && signature != std::string::npos &&
                         classify < signature);
}

// Every Bittensor flow starts a session for its own purpose on linux.
UR_TEST(BittensorWalletFlow_TheHostRunsEveryFlowOnASession) {
  const std::string host = ReadFile("SdkHost.cpp");
  if (host.empty()) {
    UR_FAIL("could not read SdkHost.cpp");
    return;
  }
  const std::string start = FunctionBody(host, "void SdkHost::StartBittensorSession(");
  UR_EXPECT_TRUE_MSG("the session is made for linux on the registered redirect",
                     start.find("urnet::newBittensorWalletSession(") != std::string::npos &&
                         start.find("bittensor::kPlatform") != std::string::npos &&
                         start.find("bittensor::kRedirectLink") != std::string::npos);
  UR_EXPECT_TRUE_MSG("the challenge comes from the session's args",
                     start.find("session->challengeArgs(expectedAddress)") != std::string::npos &&
                         start.find("session->setChallenge(") != std::string::npos);
  for (const auto& [fn, purpose] :
       {std::pair<const char*, const char*>{"void SdkHost::SignInWithBittensor(", "kPurposeLogin"},
        {"void SdkHost::SignBittensorConnect(", "kPurposeConnect"},
        {"void SdkHost::CreateNetworkWithPendingWallet(", "kPurposeCreate"}}) {
    const std::string body = FunctionBody(host, fn);
    UR_EXPECT_TRUE_MSG(std::string(fn) + " starts a session for " + purpose,
                       body.find("StartBittensorSession(") != std::string::npos &&
                           body.find(purpose) != std::string::npos);
  }
}

// WalletConnect is the third wallet (Nova, Nightly and other WalletConnect v2
// substrate wallets), after Talisman and TAO.com; the chooser's response id is
// the wallet id and nothing else chooses a wallet.
UR_TEST(BittensorWalletFlow_TheChooserOffersThreeWallets) {
  const std::string_view expected[] = {"talisman", "taocom", "walletconnect"};
  size_t n = 0;
  for (const auto& wallet : bt::kChooserWallets) {
    UR_EXPECT_TRUE_MSG(std::string(wallet.walletId), n < 3 && wallet.walletId == expected[n]);
    ++n;
  }
  UR_EXPECT_TRUE(n == 3);
  UR_EXPECT_TRUE(bt::kWalletWalletConnect == "walletconnect");
  for (const auto& id : expected) UR_EXPECT_TRUE_MSG(std::string(id), bt::ChosenWallet(id) == id);
  UR_EXPECT_TRUE(bt::ChosenWallet("cancel").empty());
  UR_EXPECT_TRUE(bt::ChosenWallet("").empty());
  UR_EXPECT_TRUE(bt::ChosenWallet("subwallet-js").empty());
  // only the WalletConnect page is given the project id
  UR_EXPECT_TRUE(bt::SendsWalletConnectProjectId("walletconnect"));
  UR_EXPECT_FALSE(bt::SendsWalletConnectProjectId("talisman"));
  UR_EXPECT_FALSE(bt::SendsWalletConnectProjectId("taocom"));
}

// The chooser hints and the browser hand-off texts are store keys the linux
// catalog carries, English as msgid.
UR_TEST(BittensorWalletFlow_WalletConnectStringsAreInTheCatalog) {
  const std::string pot = ReadFile("../po/urnetwork.pot");
  if (pot.empty()) {
    UR_FAIL("could not read po/urnetwork.pot");
    return;
  }
  auto has = [&](std::string_view key, std::string_view english) {
    return pot.find("msgctxt \"" + std::string(key) + "\"\nmsgid \"" + std::string(english) + "\"") !=
           std::string::npos;
  };
  int hints = 0;
  for (const auto& wallet : bt::kChooserWallets) {
    if (wallet.hintKey.empty()) continue;
    UR_EXPECT_TRUE_MSG(std::string(wallet.hintKey), has(wallet.hintKey, wallet.hintEnglish));
    ++hints;
  }
  UR_EXPECT_TRUE(hints == 1);
  const bt::ContinueText wc = bt::ContinueTextFor("walletconnect");
  UR_EXPECT_TRUE(wc.key == "bittensor_walletconnect_continue" && !wc.takesWalletName);
  UR_EXPECT_TRUE_MSG(std::string(wc.key), has(wc.key, wc.english));
  const bt::ContinueText talisman = bt::ContinueTextFor("talisman");
  UR_EXPECT_TRUE(talisman.key == "bittensor_continue_in_browser" && talisman.takesWalletName);
  UR_EXPECT_TRUE_MSG(std::string(talisman.key), has(talisman.key, talisman.english));
}

// Both choosers (sign-in, Earnings) come from the shared three-row chooser,
// and the session is given the build's project id for WalletConnect only.
UR_TEST(BittensorWalletFlow_ChoosersAndSessionCarryWalletConnect) {
  const std::string window = ReadFile("MainWindow.cpp");
  const std::string earnings = ReadFile("EarningsPage.cpp");
  const std::string host = ReadFile("SdkHost.cpp");
  const std::string source = ReadFile("BittensorWalletChooser.cpp");
  if (window.empty() || earnings.empty() || host.empty() || source.empty()) {
    UR_FAIL("could not read the sources");
    return;
  }
  const std::string chooser = FunctionBody(source, "GtkWidget* NewBittensorWalletChooser(");
  UR_EXPECT_TRUE_MSG("the chooser lists every kChooserWallets row",
                     chooser.find("bittensor::kChooserWallets") != std::string::npos);
  for (const auto& [source, fn] :
       {std::pair<const std::string*, const char*>{&window, "void MainWindow::OnBittensor("},
        {&earnings, "void EarningsPage::ChooseBittensorWallet("}}) {
    const std::string body = FunctionBody(*source, fn);
    UR_EXPECT_TRUE_MSG(std::string(fn) + " uses the shared chooser",
                       body.find("NewBittensorWalletChooser(") != std::string::npos &&
                           body.find("bittensor::ChosenWallet(") != std::string::npos);
  }
  const std::string start = FunctionBody(host, "void SdkHost::StartBittensorSession(");
  const size_t guard = start.find("bittensor::SendsWalletConnectProjectId(walletId)");
  const size_t set = start.find("session->setWalletConnectProjectId(kWalletConnectProjectId)");
  UR_EXPECT_TRUE_MSG("the project id goes to the WalletConnect session only",
                     guard != std::string::npos && set != std::string::npos && guard < set);
}

// Sign-in and network create ask for result_errors, so a signature from another
// account than the address comes back in the server's words rather than as a
// 401, and each page shows them as sent. The bridge page's manual form checks a
// paste before it hands it back, so this is what is left when it could not.
// (The one refusal shown in other words is the client limit's, which says what
// to do instead: ClientLimitWiringTest.cpp.)
UR_TEST(BittensorWalletFlow_ARefusedProofReadsInTheServersWords) {
  const std::string host = ReadFile("SdkHost.cpp");
  for (const char* signature : {"void SdkHost::AuthLoginWithWallet(",
                                "void SdkHost::FinishCreateNetworkWithWallet("}) {
    const std::string body = FunctionBody(host, signature);
    UR_EXPECT_TRUE_MSG(signature, body.find("args.result_errors = true;") != std::string::npos);
  }
  const std::string window = ReadFile("MainWindow.cpp");
  UR_EXPECT_TRUE_MSG("sign-in", FunctionBody(window, "void MainWindow::OnWalletAuth(")
                                        .find("SetLoginError(SignInFailureText(") != std::string::npos);
  UR_EXPECT_TRUE_MSG("sign-in words",
                     window.find("return error.empty() ? Glib::ustring(screenLine) : "
                                 "Glib::ustring(error);") != std::string::npos);
  UR_EXPECT_TRUE_MSG("create",
                     ReadFile("AuthViews.cpp").find(": Glib::ustring(r.error));") != std::string::npos);
  UR_EXPECT_TRUE_MSG("add", ReadFile("AccountPage.cpp")
                                .find("providerError_ = result.error.empty()") != std::string::npos);
}
