// The redeem sheet's answer for a balance-code redeem (BalanceCodeRedeem.hpp)
// and its wiring to the SDK classifier (UPGRADE.md D2). The server refuses an
// unknown code and one this network already redeemed with the SAME payload,
// and a lost response may have committed, so the sheet must classify through
// urnet::classifyBalanceCodeRedeem with the network's redeemed-code list
// before it says "invalid" -- a retry after a lost-but-credited response must
// read "already redeemed". An empty answer is unknown, never success. A gift
// code's year of Pro confirms as "A year of Pro is yours." or the day a queued
// year starts, also when the list is all a lost answer leaves, and reads "A
// year of Pro" in both redeemed-code lists, never "+0 B"; the list is searched
// for the code as the server reads a typed one. The sheet needs GTK and the
// SDK, so its wiring is read as text.
//
// SPDX-License-Identifier: MPL-2.0
#include "BalanceCodeRedeem.hpp"
#include "TestHarness.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::BalanceCodeRedeemNeedsCodeList;
using urnw::BalanceCodeRedeemNotice;
using urnw::BalanceCodeRedeemNoticeAfterList;
using urnw::BalanceCodeRedeemNoticeFor;
using urnw::IsProGiftCode;
using urnw::NormalizeBalanceCode;
using urnw::RedeemedCodeKind;
using urnw::RefusalSaysAlreadyRedeemed;
using urnw::ShowsQueuedGiftLine;

// a row of the network's redeemed list, as the SDK's RedeemedBalanceCode has it
struct Listed {
  std::string secret;
  std::string kind;
};

std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

size_t Count(const std::string& haystack, const std::string& needle) {
  size_t n = 0;
  for (size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

UR_TEST(balanceCodeOutcomesMapToTheirNotices) {
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeFor("redeemed") == BalanceCodeRedeemNotice::Redeemed);
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeFor("already_redeemed") ==
                 BalanceCodeRedeemNotice::AlreadyRedeemed);
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeFor("invalid") == BalanceCodeRedeemNotice::Invalid);
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeFor("unknown") == BalanceCodeRedeemNotice::Unknown);
  // an outcome this build does not know is never success or "invalid"
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeFor("") == BalanceCodeRedeemNotice::Unknown);
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeFor("something_new") == BalanceCodeRedeemNotice::Unknown);
}

UR_TEST(balanceCodeOnlyACreditedRedeemSkipsTheCodeList) {
  UR_EXPECT_FALSE(BalanceCodeRedeemNeedsCodeList("redeemed"));
  // a refusal and a lost response both consult the redeemed-code list
  UR_EXPECT_TRUE(BalanceCodeRedeemNeedsCodeList("invalid"));
  UR_EXPECT_TRUE(BalanceCodeRedeemNeedsCodeList("unknown"));
}

UR_TEST(redeemSheetClassifiesWithTheSdkAndTheRedeemedCodeList) {
  const std::string sheet = ReadSource("RedeemCodeSheet.cpp");
  // first without the list, then again with it
  UR_EXPECT_EQ(2, Count(sheet, "urnet::classifyBalanceCodeRedeem("));
  UR_EXPECT_TRUE(Count(sheet, "BalanceCodeRedeemNeedsCodeList(") > 0);
  UR_EXPECT_TRUE(Count(sheet, "getNetworkRedeemedBalanceCodes(") >= 2);  // history + classify
  UR_EXPECT_TRUE(Count(sheet, "\"balance_code_already_redeemed_message\"") > 0);
  // the SDK's format gate, not a hardcoded 26
  UR_EXPECT_TRUE(Count(sheet, "urnet::isBalanceCodeFormatValid(") >= 2);
  UR_EXPECT_EQ(0, Count(sheet, "kBalanceCodeLength"));
  // the sheet never decides success itself from a result without a transfer balance
  UR_EXPECT_EQ(0, Count(sheet, "if (result->error) {"));
}

// A redeem is committed when the server answers: a data code's data is on the
// balance, and a gift code's year of Pro is granted before the answer. A
// credited redeem must confirm what the code added and read the balance once,
// which shows either, and never start the Pro confirmation poll (there is no
// payment to wait for, and it spins the plan ring for 2 minutes) or say a
// subscription is processing.
UR_TEST(redeemSheetConfirmsTheDataAddedAndReadsTheBalanceOnce) {
  const std::string sheet = ReadSource("RedeemCodeSheet.cpp");
  const size_t start = sheet.find("case BalanceCodeRedeemNotice::Redeemed:");
  UR_EXPECT_TRUE(start != std::string::npos);
  if (start == std::string::npos) return;
  const size_t end = sheet.find("case BalanceCodeRedeemNotice::", start + 1);
  const std::string redeemed = sheet.substr(start, end - start);
  UR_EXPECT_EQ(0, Count(redeemed, "StartConfirmationPolling("));
  UR_EXPECT_EQ(1, Count(redeemed, "balance_.FetchNow();"));
  UR_EXPECT_TRUE(Count(redeemed, "transfer_balance->balance_byte_count") > 0);
  UR_EXPECT_EQ(0, Count(sheet, "\"processing_subscription_balance\""));
}

// The SDK's BalanceCodeKind* values: only a gift code's kind is its year of Pro;
// a data code, a kind this build does not know and a code from a server that
// predates gift codes (no kind) show their data.
UR_TEST(balanceCodeOnlyAGiftCodesKindIsAYearOfPro) {
  UR_EXPECT_TRUE(IsProGiftCode("pro_gift"));
  UR_EXPECT_FALSE(IsProGiftCode("data"));
  UR_EXPECT_FALSE(IsProGiftCode(""));
  UR_EXPECT_FALSE(IsProGiftCode("something_new"));
}

// A gift code grants a year of Pro and a 0-byte transfer balance: the sheet
// confirms it, before it would print the bytes, as "A year of Pro is yours." or,
// for a year queued after a paid Pro period, the day it starts in this
// machine's locale; a gift found in the redeemed list shares the confirmation
// (with the plain line). The sheet's history and the Account page's codes pane
// show a gift as "A year of Pro", never "+0 B".
UR_TEST(redeemSheetAndCodesPaneShowAGiftsYearOfPro) {
  const std::string sheet = ReadSource("RedeemCodeSheet.cpp");
  const size_t start = sheet.find("case BalanceCodeRedeemNotice::Redeemed:");
  UR_EXPECT_TRUE(start != std::string::npos);
  if (start == std::string::npos) return;
  const std::string redeemed =
      sheet.substr(start, sheet.find("case BalanceCodeRedeemNotice::", start + 1) - start);
  const size_t gift = redeemed.find(
      "if (notice == BalanceCodeRedeemNotice::GiftRedeemed || (result && result->pro)) {");
  const size_t line = redeemed.find(
      "GiftRedeemedText(notice == BalanceCodeRedeemNotice::Redeemed ? result : std::nullopt)");
  const size_t bytes = redeemed.find("transfer_balance->balance_byte_count");
  UR_EXPECT_TRUE(gift != std::string::npos && line != std::string::npos && gift < line &&
                 line < bytes);
  UR_EXPECT_EQ(1, Count(sheet, "case BalanceCodeRedeemNotice::GiftRedeemed:\n"
                               "    case BalanceCodeRedeemNotice::Redeemed:"));
  const size_t text = sheet.find("std::string GiftRedeemedText(");
  UR_EXPECT_TRUE(text != std::string::npos);
  if (text == std::string::npos) return;
  const std::string confirm = sheet.substr(text, sheet.find("\n}\n", text) - text);
  for (const char* want : {"const bool queued = result && result->pro && result->pro->queued;",
                           "LocalDateText(*result->pro->start_time)",
                           "ShowsQueuedGiftLine(queued, !date.empty())",
                           "T_(\"gift_pro_redeemed_queued\",",
                           "T_(\"gift_pro_redeemed\", \"A year of Pro is yours.\")"}) {
    UR_EXPECT_TRUE_MSG(want, Count(confirm, want) == 1);
  }
  // the locale's date in this machine's zone, as the crypto renewal notice's
  UR_EXPECT_EQ(1, Count(sheet, "return LocalDate(when.to_unix());"));
  UR_EXPECT_TRUE(Count(sheet, "IsProGiftCode(code.kind)") == 1);  // the history
  UR_EXPECT_TRUE(
      Count(sheet, "static_assert(std::string_view(urnet::BalanceCodeKindProGift) == "
                   "kBalanceCodeKindProGift);") == 1);
  const std::string page = ReadSource("AccountPage.cpp");
  const size_t render = page.find("void AccountPage::RenderBalanceCodes() {");
  UR_EXPECT_TRUE(render != std::string::npos);
  if (render == std::string::npos) return;
  const std::string pane = page.substr(render, page.find("\n}\n", render) - render);
  UR_EXPECT_TRUE(Count(pane, "if (IsProGiftCode(code.kind)) {") == 1);
  UR_EXPECT_TRUE(Count(pane, "T_(\"balance_code_pro_year\", \"A year of Pro\")") == 1);
}

// The network has the code (redeemed now, or already): the window refreshes
// the Account page's codes pane, which otherwise kept the list it loaded with
// the page (AccountPage.hpp LoadBalanceCodes).
UR_TEST(redeemSheetRefreshesTheAccountCodesPane) {
  const std::string sheet = ReadSource("RedeemCodeSheet.cpp");
  UR_EXPECT_EQ(2, Count(sheet, "RefreshCodes();\n      if (on_redeemed) on_redeemed();"));
  const std::string window = ReadSource("MainWindow.cpp");
  UR_EXPECT_TRUE(
      Count(window, "redeemSheet_->on_redeemed = [this] {\n"
                    "        if (accountPage_) accountPage_->LoadBalanceCodes();") == 1);
}

// A redeem that did not credit, once the list is read: the code is looked for
// as the server reads a typed one, a gift found there confirms as the gift, and
// a refusal saying the code is redeemed reads "already redeemed". The entry has
// room for a code in groups of four, as a card prints it.
UR_TEST(redeemSheetLooksForALostAnswerInTheList) {
  const std::string sheet = ReadSource("RedeemCodeSheet.cpp");
  for (const char* want :
       {"ShowRedeemOutcome(BalanceCodeRedeemNoticeFor(outcome), result);",
        "codes ? RedeemedCodeKind(code, *codes) : std::nullopt;",
        "result && result->error && RefusalSaysAlreadyRedeemed(result->error->message);",
        "BalanceCodeRedeemNoticeAfterList(urnet::classifyBalanceCodeRedeem(result, codes, code),",
        "codeEntry_->set_max_length(static_cast<int>(2 * urnet::BalanceCodeLength));"}) {
    UR_EXPECT_TRUE_MSG(want, Count(sheet, want) == 1);
  }
}

// A typed code as the server reads it: no whitespace, dashes or dots, upper
// case, and 0, 1, 8 as O, I, B (server normalizeBalanceCodeSecret and its
// lookalikes; the SDK's twin).
UR_TEST(balanceCodeIsReadAsTheServerReadsATypedCode) {
  const std::string code = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  UR_EXPECT_TRUE(NormalizeBalanceCode(code) == code);
  UR_EXPECT_TRUE(NormalizeBalanceCode("abcd efgh ijkl mnop qrst uvwx yz") == code);
  UR_EXPECT_TRUE(NormalizeBalanceCode(" ABCD-EFGH-IJKL-MNOP-QRST-UVWX-YZ\n") == code);
  UR_EXPECT_TRUE(
      NormalizeBalanceCode("ABCD.EFGH\tIJKL–MNOP—QRST UVWX　YZ") == code);
  UR_EXPECT_TRUE(
      NormalizeBalanceCode("ABCD－EFGH‐IJKL‑MNOP‒QRST―UVWX⸺YZ") == code);
  UR_EXPECT_TRUE(NormalizeBalanceCode("0O1I8B") == "OOIIBB");
  UR_EXPECT_TRUE(NormalizeBalanceCode("2345679") == "2345679");
  UR_EXPECT_TRUE(NormalizeBalanceCode("ıſ") == "IS");
  // a minus sign is no dash, a zero-width space no space
  UR_EXPECT_TRUE(NormalizeBalanceCode("AB−CD") == "AB−CD");
  UR_EXPECT_TRUE(NormalizeBalanceCode("AB​CD") == "AB​CD");
  // malformed UTF-8 is malformed, never a letter: an overlong A, a surrogate
  UR_EXPECT_TRUE(NormalizeBalanceCode("\xC1\x81") == "\xEF\xBF\xBD\xEF\xBF\xBD");
  UR_EXPECT_TRUE(NormalizeBalanceCode("A\xED\xA0\x80" "B") ==
                 "A\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" "B");
  UR_EXPECT_TRUE(NormalizeBalanceCode("").empty() && NormalizeBalanceCode(" - . ").empty());
}

UR_TEST(balanceCodeIsFoundInTheRedeemedList) {
  const std::string data = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const std::string gift = "GIFTCODEQRSTUVWXYZ234567AB";
  const std::vector<Listed> list{{data, "data"}, {gift, "pro_gift"}};
  UR_EXPECT_TRUE(RedeemedCodeKind(data, list) == std::optional<std::string>("data"));
  // typed as the card prints it, and with 1, 0 and 8 for I, O and B
  UR_EXPECT_TRUE(RedeemedCodeKind("gift-code-qrst-uvwx-yz23-4567-ab", list) ==
                 std::optional<std::string>("pro_gift"));
  UR_EXPECT_TRUE(RedeemedCodeKind("G1FTC0DEQRSTUVWXYZ234567A8", list) ==
                 std::optional<std::string>("pro_gift"));
  UR_EXPECT_FALSE(RedeemedCodeKind("ZZZZEFGHIJKLMNOPQRSTUVWXYZ", list).has_value());
  UR_EXPECT_FALSE(RedeemedCodeKind(data, std::vector<Listed>{}).has_value());
  UR_EXPECT_FALSE(RedeemedCodeKind(" ", std::vector<Listed>{{"", "pro_gift"}}).has_value());
}

// the server's words for a gift code that is redeemed, by any network
UR_TEST(balanceCodeRefusalSaysAlreadyRedeemed) {
  UR_EXPECT_TRUE(RefusalSaysAlreadyRedeemed("Balance code already redeemed."));
  UR_EXPECT_TRUE(RefusalSaysAlreadyRedeemed("ALREADY REDEEMED"));
  UR_EXPECT_FALSE(RefusalSaysAlreadyRedeemed("Unknown balance code."));
  UR_EXPECT_FALSE(RefusalSaysAlreadyRedeemed("Balance code expired."));
  UR_EXPECT_FALSE(RefusalSaysAlreadyRedeemed("Balance code was refunded."));
  UR_EXPECT_FALSE(RefusalSaysAlreadyRedeemed(""));
}

// What a redeem that did not credit says once the list is read: a gift this
// network holds confirms as the gift; a data code it holds, and a code the
// server calls redeemed that another network holds, read "already redeemed";
// anything else is as before.
UR_TEST(balanceCodeAnswerAfterTheList) {
  using Notice = BalanceCodeRedeemNotice;
  const std::optional<std::string> none;
  const std::optional<std::string> gift("pro_gift");
  const std::optional<std::string> data("data");
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeAfterList("already_redeemed", gift, true) ==
                 Notice::GiftRedeemed);
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeAfterList("already_redeemed", gift, false) ==
                 Notice::GiftRedeemed);
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeAfterList("already_redeemed", data, false) ==
                 Notice::AlreadyRedeemed);
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeAfterList("invalid", none, true) ==
                 Notice::AlreadyRedeemed);
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeAfterList("invalid", none, false) == Notice::Invalid);
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeAfterList("unknown", none, false) == Notice::Unknown);
  UR_EXPECT_TRUE(BalanceCodeRedeemNoticeAfterList("already_redeemed", none, false) ==
                 Notice::AlreadyRedeemed);
}

// the queued line needs a queued year and a start date that reads
UR_TEST(balanceCodeGiftLine) {
  UR_EXPECT_TRUE(ShowsQueuedGiftLine(true, true));
  UR_EXPECT_FALSE(ShowsQueuedGiftLine(true, false));
  UR_EXPECT_FALSE(ShowsQueuedGiftLine(false, true));
}

}  // namespace
