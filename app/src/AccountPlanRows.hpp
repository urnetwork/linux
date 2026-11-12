// What the Account page's Plan pane (pane A) shows in its plan value and its
// figure rows, decided pure so tests/AccountPlanRowsTest.cpp pins it without
// GTK.
//
// The plan value is the sdk's decision (urnet::SubscriptionProTracker
// getShownPlan, getLastReadFailed): Pro or Free as the server answered, Pro
// also by the stored jwt's claim before it answers. Until the server answers
// the plan is unknown, and the value shows its state, never Free: a claim of
// Free stays Free after a purchase until the network jwt renews, and in
// December 2025 a balance read that failed under database load showed paying
// networks Free, with Upgrade (support inbox 4074). Only a plan the server said
// is Free offers Upgrade; a read that failed offers Try again.
//
// Under the usage bar, which shows the balance's shape and whose legend only
// colours its parts, rows carry the figures: the daily balance, then Used,
// Pending and Available. Before a balance snapshot has landed none of them has
// a figure, so each shows its state instead of an invented "0 B" (spec §0.4).
// Above Redeem, a Balance Codes row counts the redeemed codes for every
// answered fetch, zero included; with no answer the row is hidden rather than
// drawn over nothing.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace urnw::account_plan {

enum class PlanView {
  Pro,
  Free,
  Loading,    // unknown: the first balance read is on its way
  Failed,     // unknown: a read failed and none has loaded since
  NoSession,  // unknown, and no session to ask with
};

// The sdk's plan values (urnet::ShownPlanPro, ShownPlanFree, ShownPlanUnknown;
// SubscriptionBalance.cpp asserts they match).
inline constexpr std::string_view kShownPlanPro = "pro";
inline constexpr std::string_view kShownPlanFree = "free";
inline constexpr std::string_view kShownPlanUnknown = "unknown";

// `shownPlan` is the sdk's getShownPlan; any value but Pro and Free is
// unknown.
constexpr PlanView PlanViewFor(std::string_view shownPlan, bool lastReadFailed,
                               bool canCallApi) {
  if (shownPlan == kShownPlanPro) return PlanView::Pro;
  if (shownPlan == kShownPlanFree) return PlanView::Free;
  if (!canCallApi) return PlanView::NoSession;
  return lastReadFailed ? PlanView::Failed : PlanView::Loading;
}

// Upgrade below the plan: a signed-in account the server said is Free (a guest
// gets "Create an account" there instead). Never while the plan is unknown.
constexpr bool OffersUpgrade(PlanView plan, bool guest) {
  return !guest && plan == PlanView::Free;
}

// Try again beside the plan value: a read failed and the server has not
// answered since.
constexpr bool OffersRetry(PlanView plan, bool guest) {
  return !guest && plan == PlanView::Failed;
}

enum class FigureView {
  Figure,     // the snapshot's figure
  Loading,    // no snapshot yet, and the page can ask for one
  NoSession,  // no snapshot, and no session to ask with
};

constexpr FigureView FigureFor(bool snapshotLoaded, bool canCallApi) {
  if (snapshotLoaded) return FigureView::Figure;
  return canCallApi ? FigureView::Loading : FigureView::NoSession;
}

// The Balance Codes row's count, or nothing to hide the row. `answered`: the
// fetch returned a list, empty or not.
inline std::optional<std::string> CodeCountText(bool answered, size_t count) {
  if (!answered) return std::nullopt;
  return std::to_string(count);
}

}  // namespace urnw::account_plan
