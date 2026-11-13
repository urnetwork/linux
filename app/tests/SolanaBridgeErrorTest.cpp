// The Solana wallet bridge's own failure codes (SolanaWalletPresentation.hpp
// BridgeErrorTextFor): a code this app knows reads in its own words, the
// delivery (WalletConnect.cpp) says it on the connect and the sign step, and
// the strings are translated in every language the app ships.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "SolanaWalletPresentation.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace solana = urnw::solana;

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

// The page's code (urnet::SolanaWalletBridgeError*) reads in this app's words,
// with the wallet's name where the string has a {}; any other code shows the
// page's own text.
UR_TEST(SolanaBridgeError_CodesReadInTheAppsWords) {
  struct Case {
    const char* code;
    const char* key;
    bool takesWalletName;
  };
  for (const Case& c : {
           Case{"extension_not_found", "bittensor_error_extension_not_found", true},
           Case{"no_account", "bittensor_error_no_account", true},
           Case{"session_not_found", "solana_wallet_error_session_not_found", false},
           Case{"user_rejected", "bittensor_error_user_rejected", false},
       }) {
    const solana::BridgeErrorText text = solana::BridgeErrorTextFor(c.code);
    UR_EXPECT_TRUE_MSG(c.code, text.key != nullptr && std::string(text.key) == c.key);
    UR_EXPECT_TRUE_MSG(c.code, text.takesWalletName == c.takesWalletName);
    UR_EXPECT_TRUE_MSG(c.code, text.english != nullptr &&
                                   (std::string(text.english).find("{}") != std::string::npos) ==
                                       c.takesWalletName);
  }
  // the page's other failures, a code this app does not know, a page before
  // the codes (-1), and no code
  for (const char* code : {"invalid_request", "wallet_error", "wallet_locked", "-1", ""}) {
    UR_EXPECT_TRUE_MSG(code, solana::BridgeErrorTextFor(code).key == nullptr);
  }
}

// Every string a code reads in is a key the linux catalog carries with its
// English as the msgid, translated in every language the app ships.
UR_TEST(SolanaBridgeError_StringsAreTranslated) {
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
  for (const char* code :
       {"extension_not_found", "no_account", "session_not_found", "user_rejected"}) {
    const solana::BridgeErrorText text = solana::BridgeErrorTextFor(code);
    if (text.key == nullptr) {
      UR_FAIL(std::string(code) + " has no string");
      continue;
    }
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
      if (language != "en") {
        UR_EXPECT_TRUE_MSG(std::string(code) + " is English in " + language,
                           translated != text.english);
      }
      UR_EXPECT_TRUE_MSG(std::string(code) + " keeps {} in " + language,
                         (translated.find("{}") != std::string::npos) == text.takesWalletName);
    }
  }
}

// The bridge's failure goes through the page's code to the user on both steps,
// with Phantom or Solflare as the wallet's name.
UR_TEST(SolanaBridgeError_TheCodeReachesTheUser) {
  const std::string wallet = ReadFile("WalletConnect.cpp");
  if (wallet.empty()) {
    UR_FAIL("could not read WalletConnect.cpp");
    return;
  }
  const std::string localized = FunctionBody(wallet, "std::string LocalizedBridgeError(");
  for (const char* want : {"solana::BridgeErrorTextFor(code)", "if (!text.key) return pageText;",
                           "g_dpgettext2(GETTEXT_PACKAGE, text.key, text.english)",
                           "p == WalletConnect::Provider::Solflare ? \"Solflare\" : \"Phantom\""}) {
    UR_EXPECT_TRUE_MSG(want, localized.find(want) != std::string::npos);
  }
  const std::string delivery =
      "on_error(LocalizedBridgeError(p, params[\"errorCode\"], pageText";
  for (const char* handler :
       {"void WalletConnect::HandleConnect(", "void WalletConnect::HandleSignMessage("}) {
    UR_EXPECT_TRUE_MSG(handler, FunctionBody(wallet, handler).find(delivery) != std::string::npos);
  }
}

// g_dpgettext2 answers a string with no translation (no catalog for the
// language, or none installed) with the msgid pointer it was given, so the
// msgid must outlive the answer: a translation kept in a variable must not
// come from a temporary std::string.
UR_TEST(SolanaBridgeError_NoTranslationOutlivesItsMsgid) {
  const std::string wallet = ReadFile("WalletConnect.cpp");
  if (wallet.empty()) {
    UR_FAIL("could not read WalletConnect.cpp");
    return;
  }
  const std::regex keptFromTemporary(
      R"(const\s+char\s*\*\s*\w+\s*=\s*g_dpgettext2\([^;]*std::string\()");
  UR_EXPECT_FALSE(std::regex_search(wallet, keptFromTemporary));
}
