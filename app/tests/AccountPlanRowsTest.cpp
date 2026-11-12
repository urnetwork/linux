// The Account page's Plan pane (AccountPlanRows.hpp): the plan value is Pro or
// Free only as the server answered (Pro also by a Pro claim before it does),
// and until then its state, never "Free", with Upgrade only for a plan the
// server said is Free and Try again after a failed read (support inbox 4074);
// the daily balance and the usage bar's Used, Pending and Available show their
// figure once a snapshot has landed and their state before it, never "0 B";
// the referral rule sits under the referral numbers; and the Balance Codes
// count shows for every answered fetch, zero included, and hides otherwise.
// The page needs gtk and the sdk, so the wiring cases read AccountPage.cpp
// (and the store and window that feed it) with the comments blanked.
// SPDX-License-Identifier: MPL-2.0
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "AccountPlanRows.hpp"
#include "TestHarness.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::account_plan::CodeCountText;
using urnw::account_plan::FigureFor;
using urnw::account_plan::FigureView;
using urnw::account_plan::OffersRetry;
using urnw::account_plan::OffersUpgrade;
using urnw::account_plan::PlanView;
using urnw::account_plan::PlanViewFor;

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadPlanRowsSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string text = buffer.str();
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

std::string PlanRowsBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Each needle occurs after the one before it.
bool PlanRowsInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

UR_TEST(AccountPlanRows_AFigureWaitsForItsSnapshot) {
  UR_EXPECT_TRUE(FigureFor(/*snapshotLoaded=*/true, /*canCallApi=*/true) == FigureView::Figure);
  // a snapshot already landed is the figure, even after the session went
  UR_EXPECT_TRUE(FigureFor(true, false) == FigureView::Figure);
  UR_EXPECT_TRUE(FigureFor(false, true) == FigureView::Loading);
  UR_EXPECT_TRUE(FigureFor(false, false) == FigureView::NoSession);
}

UR_TEST(AccountPlanRows_EveryAnsweredFetchHasACount) {
  UR_EXPECT_TRUE(CodeCountText(/*answered=*/true, 3) == std::optional<std::string>("3"));
  // zero is an answer
  UR_EXPECT_TRUE(CodeCountText(true, 0) == std::optional<std::string>("0"));
  // no session, loading, failed: no row, whatever the list held before
  UR_EXPECT_FALSE(CodeCountText(false, 0).has_value());
  UR_EXPECT_FALSE(CodeCountText(false, 5).has_value());
}

// Pane A's rows in Windows' order: the daily balance, the bar's three figures
// under the legend's keys, the referral pair and its rule, the codes count,
// then Redeem.
UR_TEST(AccountPlanRowsWiring_ThePaneBuildsTheRowsInOrder) {
  const std::string page = ReadPlanRowsSource("AccountPage.cpp");
  const std::string build = PlanRowsBody(page, "void AccountPage::BuildPlanPane() {");
  UR_EXPECT_TRUE(PlanRowsInOrder(
      build, {"dailyValue_ = row.value;", "T_(\"used_data_key\", \"Used\")), &usedValue_",
              "T_(\"pending_data_key\", \"Pending\")), &pendingValue_",
              "T_(\"available_data_key\", \"Available\")), &availableValue_",
              "referralBonus_ = row.value;", "referralDetail_ = row.line;",
              "T_(\"balance_codes_title\", \"Balance Codes\")", "codesCountRow_ = row.root;",
              "codesCountRow_->set_visible(false);", "T_(\"redeem_balance_code\""}));
}

// Every figure goes through the one rule, and the referral rule reads the
// terms the bonus is computed with, not a fixed amount.
UR_TEST(AccountPlanRowsWiring_TheBalanceRelayFillsEveryFigure) {
  const std::string page = ReadPlanRowsSource("AccountPage.cpp");
  const std::string figure = PlanRowsBody(page, "void ApplyBalanceFigure(");
  UR_EXPECT_TRUE(PlanRowsInOrder(
      figure, {"account_plan::FigureFor(snapshotLoaded, canCallApi)", "FigureView::Figure:",
               "FormatByteCountCompact(byteCount)", "FigureView::Loading:",
               "AccountFieldState::Loading", "FigureView::NoSession:",
               "AccountFieldState::NoSession", "SetAccessibleLabel(value, key + \", \""}));
  const std::string relay =
      PlanRowsBody(page, "void AccountPage::ApplyBalance(const AccountBalance& snapshot) {");
  UR_EXPECT_TRUE(PlanRowsInOrder(
      relay, {"ApplyBalanceFigure(*dailyValue_,", "balance_.startBalanceByteCount, balance_.loaded",
              "ApplyBalanceFigure(*usedValue_,", "balance_.usedByteCount",
              "ApplyBalanceFigure(*pendingValue_,", "balance_.pendingByteCount",
              "ApplyBalanceFigure(*availableValue_,", "balance_.availableByteCount",
              "referralDetail_->set_text(Format(T_(\"referral_panel_detail\"",
              "CurrentReferralTerms().bonusGibPerDay"}));
  // no figure row writes a byte count of its own
  UR_EXPECT_TRUE(relay.find("FormatByteCountCompact") == std::string::npos);
}

// The count row is set on every render, before the table's early return, so a
// sign-out (NoSession) and a failed fetch hide it.
UR_TEST(AccountPlanRowsWiring_TheCodesRenderSetsTheCountRowFirst) {
  const std::string page = ReadPlanRowsSource("AccountPage.cpp");
  const std::string render = PlanRowsBody(page, "void AccountPage::RenderBalanceCodes() {");
  UR_EXPECT_TRUE(PlanRowsInOrder(
      render, {"account_plan::CodeCountText(",
               "codesState_ == AccountFieldState::Loaded || codesState_ == AccountFieldState::Empty",
               "codes_.size()", "codesCountRow_->set_visible(count.has_value());",
               "if (!loaded) {", "return;"}));
}

// Support inbox 4074: in December 2025 the balance read failed under database
// load, and a paying network whose stored jwt still said Free read "Free" with
// Upgrade. Only the server says Free now.
UR_TEST(AccountPlanRows_AFailedReadIsNeverFree) {
  // the value before this change: Guest, else Pro, else Free
  const auto planValueBefore = [](bool isPro, bool guest) -> std::string {
    return guest ? "Guest" : isPro ? "Pro" : "Free";
  };
  // the network paid; its stored jwt still says Free; every read fails
  const PlanView failedPlanView =
      PlanViewFor("unknown", /*lastReadFailed=*/true, /*canCallApi=*/true);
  // control: the value before this change read Free, with Upgrade
  UR_EXPECT_TRUE(planValueBefore(/*isPro=*/false, /*guest=*/false) == "Free");
  UR_EXPECT_TRUE(failedPlanView == PlanView::Failed);
  UR_EXPECT_TRUE(OffersRetry(failedPlanView, /*guest=*/false));
  UR_EXPECT_FALSE(OffersUpgrade(failedPlanView, /*guest=*/false));

  // the first read on its way: Loading, nothing to retry, no Upgrade
  const PlanView loadingPlanView = PlanViewFor("unknown", false, true);
  UR_EXPECT_TRUE(loadingPlanView == PlanView::Loading);
  UR_EXPECT_FALSE(OffersRetry(loadingPlanView, false));
  UR_EXPECT_FALSE(OffersUpgrade(loadingPlanView, false));

  // no session to ask with
  UR_EXPECT_TRUE(PlanViewFor("unknown", false, false) == PlanView::NoSession);
  UR_EXPECT_TRUE(PlanViewFor("unknown", true, false) == PlanView::NoSession);
  // a value it does not know is unknown, not Free
  UR_EXPECT_TRUE(PlanViewFor("", false, true) == PlanView::Loading);
  UR_EXPECT_TRUE(PlanViewFor("supporter", true, true) == PlanView::Failed);
}

// Pro and Free as the server answered, Pro also by a claim before it does;
// Upgrade only for Free, and a guest gets neither Upgrade nor Try again.
UR_TEST(AccountPlanRows_ProAndFreeAreTheServers) {
  // the server's Pro, or a Pro claim before it answers, also through a failed read
  UR_EXPECT_TRUE(PlanViewFor("pro", false, true) == PlanView::Pro);
  UR_EXPECT_TRUE(PlanViewFor("pro", true, true) == PlanView::Pro);
  UR_EXPECT_FALSE(OffersUpgrade(PlanView::Pro, false));
  UR_EXPECT_FALSE(OffersRetry(PlanView::Pro, false));
  // the server's Free, which a later failed read keeps
  UR_EXPECT_TRUE(PlanViewFor("free", false, true) == PlanView::Free);
  UR_EXPECT_TRUE(PlanViewFor("free", true, true) == PlanView::Free);
  UR_EXPECT_TRUE(OffersUpgrade(PlanView::Free, false));
  UR_EXPECT_FALSE(OffersRetry(PlanView::Free, false));
  // a snapshot already relayed stands after the session went, like a figure
  UR_EXPECT_TRUE(PlanViewFor("free", false, false) == PlanView::Free);
  // a guest creates an account instead: no Upgrade, no Try again
  for (const PlanView planView :
       {PlanView::Pro, PlanView::Free, PlanView::Loading, PlanView::Failed, PlanView::NoSession}) {
    UR_EXPECT_FALSE(OffersUpgrade(planView, /*guest=*/true));
    UR_EXPECT_FALSE(OffersRetry(planView, /*guest=*/true));
  }
}

// The relay paints the plan value from the sdk's decision: its state while
// unknown, Upgrade only for a server Free, and Try again after a failed read;
// the old rule (Free and Upgrade whenever not Pro) is gone.
UR_TEST(AccountPlanRowsWiring_ThePlanValueIsTheSdksDecision) {
  const std::string page = ReadPlanRowsSource("AccountPage.cpp");
  const std::string relay =
      PlanRowsBody(page, "void AccountPage::ApplyBalance(const AccountBalance& snapshot) {");
  UR_EXPECT_TRUE(PlanRowsInOrder(
      relay,
      {"account_plan::PlanViewFor(balance_.shownPlan, balance_.planReadFailed, CanCallApi())",
       "if (balance_.guest) {", "T_(\"guest\", \"Guest\")", "case account_plan::PlanView::Pro:",
       "kProGold, T_(\"supporter\", \"Pro\")", "case account_plan::PlanView::Free:",
       "T_(\"free\", \"Free\")", "case account_plan::PlanView::Loading:",
       "AccountFieldState::Loading", "case account_plan::PlanView::Failed:",
       "AccountFieldState::Failed", "case account_plan::PlanView::NoSession:",
       "AccountFieldState::NoSession",
       "planRetryButton_->set_visible(account_plan::OffersRetry(planView, balance_.guest));",
       "account_plan::OffersUpgrade(planView, balance_.guest)"}));
  UR_EXPECT_TRUE(relay.find("!balance_.isPro") == std::string::npos);
  UR_EXPECT_TRUE(relay.find(": Glib::ustring(T_(\"free\", \"Free\"))") == std::string::npos);
  const std::string build = PlanRowsBody(page, "void AccountPage::BuildPlanPane() {");
  UR_EXPECT_TRUE(PlanRowsInOrder(
      build, {"planValueBox->append(*planValue_);", "T_(\"try_again\", \"Try again\")",
              "planRetryButton_->set_visible(false);", "if (on_retry_plan) on_retry_plan();",
              "planValueBox->append(*planRetryButton_);"}));
}

// Every failed read reaches the decision, and the window relays the plan and
// wires Try again to a read.
UR_TEST(AccountPlanRowsWiring_AFailedReadReachesThePlanValue) {
  const std::string store = ReadPlanRowsSource("SubscriptionBalance.cpp");
  UR_EXPECT_TRUE(PlanRowsInOrder(store, {"if (err || !result) {", "errorFetching_ = true;",
                                         "proTracker_.balanceFailed();", "} else {"}));
  UR_EXPECT_TRUE(PlanRowsInOrder(
      store, {"static_assert(std::string_view(urnet::ShownPlanPro) == account_plan::kShownPlanPro);",
              "static_assert(std::string_view(urnet::ShownPlanFree) == account_plan::kShownPlanFree);",
              "static_assert(std::string_view(urnet::ShownPlanUnknown) == "
              "account_plan::kShownPlanUnknown);"}));
  const std::string window = ReadPlanRowsSource("MainWindow.cpp");
  UR_EXPECT_TRUE(PlanRowsInOrder(window, {"snapshot.shownPlan = balance_.ShownPlan();",
                                          "snapshot.planReadFailed = balance_.PlanReadFailed();",
                                          "accountPage_->ApplyBalance(snapshot);"}));
  UR_EXPECT_TRUE(window.find("accountPage_->on_retry_plan = [this] { balance_.FetchNow(); };") !=
                 std::string::npos);
}
