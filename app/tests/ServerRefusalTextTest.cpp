// What the upgrade sheet and Manage subscription say when the server refuses
// (ServerRefusalText.hpp): a code with a line of its own reads as that line
// alone (incorrect_cvc, the card payment report's refused trial card, among
// them), invalid_request and start_failed as the screen's own line, and any
// other code or none as the screen's own line with the server's words under
// it. The screens word their refusals with it (UpgradeSheet.cpp and
// AccountPage.cpp, read as text: they need GTK and the SDK).
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ServerRefusalText.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

// A file by its path from app/src (UR_SRC_DIR), or "" when it cannot be read.
std::string ReadFile(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// The body of the function whose definition starts with `signature`, braces
// balanced from the first '{' after it, or "".
std::string FunctionBody(const std::string& source, const std::string& signature) {
  const size_t at = source.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t open = source.find('{', at);
  if (open == std::string::npos) return std::string();
  int depth = 0;
  for (size_t i = open; i < source.size(); ++i) {
    if (source[i] == '{') ++depth;
    if (source[i] == '}' && --depth == 0) return source.substr(open, i - open + 1);
  }
  return std::string();
}

// The code without its whitespace, so a match does not depend on line breaks.
std::string Compact(const std::string& code) {
  std::string out;
  for (const char c : code) {
    if (std::isspace(static_cast<unsigned char>(c)) == 0) out += c;
  }
  return out;
}

// Where `code` starts in `body`, both compacted, or npos.
size_t Find(const std::string& body, const std::string& code) {
  return Compact(body).find(Compact(code));
}

// The line's store key, or "" without one.
std::string Key(const urnw::ServerRefusalText& text) {
  return text.line.key ? text.line.key : "";
}

// The line's English, or "" without one.
std::string English(const urnw::ServerRefusalText& text) {
  return text.line.english ? text.line.english : "";
}

// Both screens' own lines.
const std::vector<urnw::RefusalLine>& ScreenLines() {
  static const std::vector<urnw::RefusalLine> lines = {urnw::kUpgradeSheetRefusalLine,
                                                       urnw::kManageSubscriptionRefusalLine};
  return lines;
}

// A code with a line of its own, and the line's key.
struct CodeLine {
  const char* code;
  const char* key;
};

// Every code with a line of its own.
const std::vector<CodeLine>& CodeLines() {
  static const std::vector<CodeLine> lines = {
      CodeLine{"already_subscribed", "site_payment_error_already_subscribed"},
      CodeLine{"plan_unavailable", "site_payment_error_plan_unavailable"},
      CodeLine{"checkout_unavailable", "checkout_error_unavailable"},
      CodeLine{"incorrect_cvc", "checkout_error_incorrect_cvc"},
      CodeLine{"no_customer", "site_subscription_error_no_customer"},
      CodeLine{"store_unavailable", "site_subscription_error_store_unavailable"},
  };
  return lines;
}

}  // namespace

// A code with a line of its own reads as that line alone, on either screen,
// with the server's words left out.
UR_TEST(ServerRefusalText_CodesWithALineReadAlone) {
  const std::string words = "The server's English sentence.";
  for (const urnw::RefusalLine& screen : ScreenLines()) {
    for (const CodeLine& c : CodeLines()) {
      const urnw::ServerRefusalText text = urnw::ServerRefusalTextFor(c.code, words, screen);
      UR_EXPECT_TRUE_MSG(c.code, Key(text) == c.key);
      UR_EXPECT_TRUE_MSG(c.code, !English(text).empty() && English(text) != screen.english);
      UR_EXPECT_TRUE_MSG(c.code, text.detail.empty());
    }
  }
  // the English of each line, the msgid the catalogs key it on
  struct Source {
    const char* code;
    const char* english;
  };
  for (const Source& s : {
           Source{"already_subscribed",
                  "You already have Pro, so nothing was charged. Manage your subscription from "
                  "your account."},
           Source{"plan_unavailable", "This plan is not available right now. Try again later."},
           Source{"checkout_unavailable",
                  "Checkout isn't available right now. Please try again later."},
           Source{"incorrect_cvc",
                  "Your card's security code (CVC) didn't match. Check the card details and try "
                  "again."},
           Source{"no_customer", "This network has no Stripe billing details."},
           Source{"store_unavailable",
                  "Stripe could not be reached just now. Try again in a moment."},
       }) {
    const urnw::ServerRefusalText text =
        urnw::ServerRefusalTextFor(s.code, words, urnw::kUpgradeSheetRefusalLine);
    UR_EXPECT_TRUE_MSG(s.code, English(text) == s.english);
  }
}

// invalid_request and start_failed read as the screen's own line, with
// nothing under it.
UR_TEST(ServerRefusalText_RequestAndStartFailuresReadTheScreensLine) {
  for (const urnw::RefusalLine& screen : ScreenLines()) {
    for (const char* code : {"invalid_request", "start_failed"}) {
      const urnw::ServerRefusalText text =
          urnw::ServerRefusalTextFor(code, "Unknown plan.", screen);
      UR_EXPECT_TRUE_MSG(code, Key(text) == screen.key);
      UR_EXPECT_TRUE_MSG(code, English(text) == screen.english);
      UR_EXPECT_TRUE_MSG(code, text.detail.empty());
    }
  }
}

// Any other code, or none (a server from before the codes), reads as the
// screen's own line with the server's words under it. Codes compare exactly.
UR_TEST(ServerRefusalText_OtherRefusalsKeepTheServersWords) {
  const std::string words = "No stripe customer found";
  for (const urnw::RefusalLine& screen : ScreenLines()) {
    for (const char* code : {"", "item_unavailable", "rate_limited", "no_subscription",
                             "ALREADY_SUBSCRIBED", "Invalid_Request", " no_customer"}) {
      const urnw::ServerRefusalText text = urnw::ServerRefusalTextFor(code, words, screen);
      UR_EXPECT_TRUE_MSG(code, Key(text) == screen.key);
      UR_EXPECT_TRUE_MSG(code, English(text) == screen.english);
      UR_EXPECT_TRUE_MSG(code, text.detail == words);
    }
    // a refusal without words reads as the line alone
    const urnw::ServerRefusalText bare = urnw::ServerRefusalTextFor("", "", screen);
    UR_EXPECT_TRUE(Key(bare) == screen.key);
    UR_EXPECT_TRUE(bare.detail.empty());
  }
  UR_EXPECT_TRUE(std::string(urnw::kUpgradeSheetRefusalLine.key) ==
                 "something_went_wrong_please_try_again_later");
  UR_EXPECT_TRUE(std::string(urnw::kManageSubscriptionRefusalLine.key) == "something_went_wrong");
}

// The server's words go on the line under the translated line; a line that
// stands alone is shown as it is.
UR_TEST(ServerRefusalText_TheWordsGoUnderTheLine) {
  const urnw::ServerRefusalText alone = urnw::ServerRefusalTextFor(
      "plan_unavailable", "Plan not offered.", urnw::kUpgradeSheetRefusalLine);
  UR_EXPECT_TRUE(urnw::ServerRefusalDisplay("Translated line.", alone) == "Translated line.");
  const urnw::ServerRefusalText under = urnw::ServerRefusalTextFor(
      "", "No stripe customer found", urnw::kManageSubscriptionRefusalLine);
  UR_EXPECT_TRUE(urnw::ServerRefusalDisplay("Translated line.", under) ==
                 "Translated line.\nNo stripe customer found");
}

// Every line a refusal can read as is in the linux catalog with its English as
// the msgid, and the code lines are translated in every language the app
// ships.
UR_TEST(ServerRefusalText_StringsAreTranslated) {
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
  struct Case {
    urnw::RefusalLine line;
    bool translatedEverywhere;
  };
  std::vector<Case> cases;
  for (const CodeLine& c : CodeLines()) {
    cases.push_back(Case{urnw::ServerRefusalTextFor(c.code, "", urnw::kUpgradeSheetRefusalLine).line,
                         true});
  }
  for (const urnw::RefusalLine& screen : ScreenLines()) cases.push_back(Case{screen, false});
  for (const Case& c : cases) {
    const std::string key = c.line.key ? c.line.key : "";
    const std::string entry = "msgctxt \"" + key + "\"\nmsgid \"" +
                              std::string(c.line.english ? c.line.english : "") + "\"\nmsgstr \"";
    UR_EXPECT_TRUE_MSG(key + " in the pot", pot.find(entry) != std::string::npos);
    if (!c.translatedEverywhere) continue;
    for (const auto& language : languages) {
      const std::string po = ReadFile("../po/" + language + ".po");
      const size_t at = po.find(entry);
      UR_EXPECT_TRUE_MSG(key + " in " + language, at != std::string::npos);
      if (at == std::string::npos) continue;
      const size_t start = at + entry.size();
      const std::string translated = po.substr(start, po.find('"', start) - start);
      UR_EXPECT_TRUE_MSG(key + " translated in " + language, !translated.empty());
      if (language != "en") {
        UR_EXPECT_TRUE_MSG(key + " is English in " + language, translated != c.line.english);
      }
    }
  }
}

// The upgrade sheet words the hosted checkout's refusal with it, after the
// guest refusal has opened the add sign-in flow and after an embedded failure
// has fallen through to the hosted checkout; a request that got no answer
// still shows what it showed before.
UR_TEST(ServerRefusalText_TheUpgradeSheetWordsTheRefusal) {
  const std::string sheet = ReadFile("UpgradeSheet.cpp");
  if (sheet.empty()) {
    UR_FAIL("could not read UpgradeSheet.cpp");
    return;
  }
  const std::string body = FunctionBody(sheet, "void UpgradeSheet::RequestSession(");
  for (const char* want : {
           "if (result->error) {",
           "const ServerRefusalText refusal = ServerRefusalTextFor("
           "result->error->code.value_or(std::string()), result->error->message, "
           "kUpgradeSheetRefusalLine);",
           "fail(ServerRefusalDisplay(T_(refusal.line.key, refusal.line.english), refusal));",
           "if (err) { fail(*err); return; }",
       }) {
    UR_EXPECT_TRUE_MSG(want, Find(body, want) != std::string::npos);
  }
  UR_EXPECT_TRUE(Find(body, "fail(result->error->message)") == std::string::npos);
  const size_t worded = Find(body, "ServerRefusalTextFor(");
  const size_t guest = Find(body, "RefuseForGuest();");
  const size_t hosted = Find(body, "RequestSession(/*embedded=*/false);");
  const size_t noAnswer = Find(body, "if (err) { fail(*err); return; }");
  UR_EXPECT_TRUE(worded != std::string::npos && guest != std::string::npos &&
                 hosted != std::string::npos && noAnswer != std::string::npos);
  UR_EXPECT_TRUE(guest < worded);
  UR_EXPECT_TRUE(hosted < worded);
  UR_EXPECT_TRUE(noAnswer < worded);
}

// Manage subscription words the billing portal's refusal with it; a request
// that got no answer still shows the SDK's words, or the row's own line.
UR_TEST(ServerRefusalText_ManageSubscriptionWordsTheRefusal) {
  const std::string page = ReadFile("AccountPage.cpp");
  if (page.empty()) {
    UR_FAIL("could not read AccountPage.cpp");
    return;
  }
  const std::string body = FunctionBody(page, "void AccountPage::OpenCustomerPortal(");
  for (const char* want : {
           "if (result && result->error) {",
           "const ServerRefusalText refusal = ServerRefusalTextFor("
           "result->error->code.value_or(std::string()), result->error->message, "
           "kManageSubscriptionRefusalLine);",
           "Snack(Glib::ustring(ServerRefusalDisplay(T_(refusal.line.key, refusal.line.english), "
           "refusal)), true);",
           "FirstMessage(result && result->error ? result->error->message : std::string(), err);",
           ": Glib::ustring(message),",
       }) {
    UR_EXPECT_TRUE_MSG(want, Find(body, want) != std::string::npos);
  }
  // the refusal is worded before the no-answer line, and returns before it
  const size_t worded = Find(body, "ServerRefusalTextFor(");
  const size_t noAnswer = Find(body, ": Glib::ustring(message),");
  UR_EXPECT_TRUE(worded != std::string::npos && noAnswer != std::string::npos);
  UR_EXPECT_TRUE(worded < noAnswer);
  const size_t returned = Compact(body).find("return;", worded);
  UR_EXPECT_TRUE(returned != std::string::npos && returned < noAnswer);
}
