// The Earnings page's v1 SN25α claim words (support inbox 8126): the history
// and the claim sheet read each claim status through ClaimStatusWordFor, an
// epoch the vault finalized without a payout root (the SDK's not-finalized)
// reads No payout published in both instead of a dash or nothing, an epoch the
// vault has not finalized reads Pending instead of Unclaimed, and the count
// under the sheet's total says claimable epochs, which is what it counts. The
// page needs GTK and the SDK, so this reads its source with the comments
// blanked, and po/en.po: each English fallback must be the catalog's msgid.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

std::string ReadClaimStatusFile(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadClaimStatusSource(const std::string& relative) {
  std::string text = ReadClaimStatusFile(relative);
  bool inString = false;
  for (size_t at = 0; at < text.size(); ++at) {
    const char c = text[at];
    if (inString) {
      if (c == '\\') {
        ++at;
      } else if (c == '"' || c == '\n') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '/' && at + 1 < text.size() && text[at + 1] == '/') {
      while (at < text.size() && text[at] != '\n') text[at++] = ' ';
    }
  }
  return text;
}

// Every English fallback passed to T_("key", ...) in `source`.
std::vector<std::string> ClaimStatusFallbacks(const std::string& source, const std::string& key) {
  std::vector<std::string> out;
  const std::string open = "T_(\"" + key + "\", \"";
  for (size_t at = source.find(open); at != std::string::npos; at = source.find(open, at + 1)) {
    const size_t from = at + open.size();
    out.push_back(source.substr(from, source.find('"', from) - from));
  }
  return out;
}

std::string ClaimStatusBetween(const std::string& text, const std::string& start,
                               const std::string& end) {
  const size_t from = text.find(start);
  if (from == std::string::npos) return std::string();
  const size_t to = text.find(end, from + start.size());
  return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

bool InCatalog(const std::string& catalog, const std::string& key, const std::string& english) {
  return catalog.find("msgctxt \"" + key + "\"\nmsgid \"" + english + "\"\n") != std::string::npos;
}

}  // namespace

UR_TEST(SnClaimStatusPageReadsEveryStatusThroughTheSharedWord) {
  const std::string page = ReadClaimStatusSource("EarningsPage.cpp");
  UR_EXPECT_TRUE(!page.empty());
  // neither the history nor the sheet decides a status on its own
  UR_EXPECT_TRUE(page.find("\"not-finalized\"") == std::string::npos);
  const std::string history = ClaimStatusBetween(page, "void EarningsPage::RebuildHistory()", "\n}\n");
  UR_EXPECT_TRUE(history.find("if (claim == nullptr) {") != std::string::npos);
  UR_EXPECT_TRUE(history.find("switch (snpayout::ClaimStatusWordFor(claim->status)) {") !=
                 std::string::npos);
  const std::string phase =
      ClaimStatusBetween(page, "ClaimAlphaSheet::Phase ClaimAlphaSheet::PhaseForStatus(", "\n}\n");
  UR_EXPECT_TRUE(phase.find("switch (snpayout::ClaimStatusWordFor(status)) {") != std::string::npos);
  UR_EXPECT_TRUE(phase.find("case snpayout::ClaimStatusWord::NoPayoutPublished:\n"
                            "      return Phase::NoPayout;") != std::string::npos);
}

UR_TEST(SnClaimStatusNoPayoutReadsNoPayoutPublishedInTheHistoryAndTheSheet) {
  const std::string page = ReadClaimStatusSource("EarningsPage.cpp");
  const std::string catalog = ReadClaimStatusFile("../po/en.po");
  UR_EXPECT_TRUE(!catalog.empty());
  const std::vector<std::string> fallbacks = ClaimStatusFallbacks(page, "sn_status_not_finalized");
  UR_EXPECT_EQ(size_t{2}, fallbacks.size());
  for (const std::string& english : fallbacks) UR_EXPECT_TRUE(english == "No payout published");
  UR_EXPECT_TRUE(InCatalog(catalog, "sn_status_not_finalized", "No payout published"));
  const std::string chips = ClaimStatusBetween(page, "case Phase::NoPayout:", "break;");
  UR_EXPECT_TRUE(chips.find("T_(\"sn_status_not_finalized\", \"No payout published\")") !=
                 std::string::npos);
  const std::string history =
      ClaimStatusBetween(page, "case snpayout::ClaimStatusWord::NoPayoutPublished:\n              status",
                         "break;");
  UR_EXPECT_TRUE(history.find("T_(\"sn_status_not_finalized\", \"No payout published\")") !=
                 std::string::npos);
}

UR_TEST(SnClaimStatusAnOpenEpochReadsPendingNotUnclaimed) {
  const std::string page = ReadClaimStatusSource("EarningsPage.cpp");
  const std::string catalog = ReadClaimStatusFile("../po/en.po");
  UR_EXPECT_TRUE(InCatalog(catalog, "pending_data_key", "Pending"));
  const std::string chip = ClaimStatusBetween(page, "case Phase::Open:", "break;");
  UR_EXPECT_TRUE(chip.find("T_(\"pending_data_key\", \"Pending\")") != std::string::npos);
  UR_EXPECT_TRUE(chip.find("unclaimed") == std::string::npos);
  const std::string history =
      ClaimStatusBetween(page, "case snpayout::ClaimStatusWord::Pending:\n              status", "break;");
  UR_EXPECT_TRUE(history.find("T_(\"pending_data_key\", \"Pending\")") != std::string::npos);
}

UR_TEST(SnClaimStatusTheSheetsCountSaysClaimableEpochs) {
  const std::string page = ReadClaimStatusSource("EarningsPage.cpp");
  const std::string catalog = ReadClaimStatusFile("../po/en.po");
  const std::vector<std::string> fallbacks = ClaimStatusFallbacks(page, "claim_across_epochs");
  UR_EXPECT_EQ(size_t{1}, fallbacks.size());
  for (const std::string& english : fallbacks) UR_EXPECT_TRUE(english == "Claimable epochs: {}");
  UR_EXPECT_TRUE(InCatalog(catalog, "claim_across_epochs", "Claimable epochs: {}"));
  // the count is the sheet's claimable epochs
  UR_EXPECT_TRUE(page.find("const size_t count = ClaimableEpochs().size();\n"
                           "  totalCaption_->set_text(\n"
                           "      Format(T_(\"claim_across_epochs\", \"Claimable epochs: {}\"), "
                           "static_cast<int64_t>(count)));") != std::string::npos);
}
