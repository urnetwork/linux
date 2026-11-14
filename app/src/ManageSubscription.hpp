// Whether the Manage Subscription row (the Stripe customer portal) shows, from
// the current subscription's store family (urnet::classifySubscriptionStore,
// sdk payment_catalog.go). The portal can only manage a Stripe subscription:
// an App Store or Play subscription is managed in that store, a one-time
// crypto year (family "crypto": USDC on Solana, Zcash, Monero) never renews
// and has nothing to manage (Account says "does not renew" instead), a gift
// code's year of Pro (family "gift") was paid for by someone else and never
// renews (Account says "A gift. It doesn't renew." instead), and a free
// network has nothing to manage — for those the row only produced the
// portal's raw error (UPGRADE.md D7).
//
// Dependency-free so tests/ManageSubscriptionTest.cpp runs it without the SDK.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <vector>

namespace urnw {

// storeFamily: urnet::classifySubscriptionStore(current_subscription.store),
// or "" without a current subscription
inline bool ShowsManageSubscription(const std::string& storeFamily) {
  return storeFamily == "stripe";  // urnet::SubscriptionStoreStripe
}

// The store family of a redeemed gift code's year of Pro
// (SubscriptionBalance.cpp asserts it is the SDK's).
inline constexpr const char* kStoreFamilyGift = "gift";  // urnet::SubscriptionStoreGift

// "A gift. It doesn't renew." (gift_does_not_renew) on the Account plan pane,
// where a crypto year says it does not renew: a gift year has nothing to
// manage, cancel or pay, so the line is all it gets.
inline bool ShowsGiftDoesNotRenew(const std::string& storeFamily) {
  return storeFamily == kStoreFamilyGift;
}

// The store family the plan pane goes by. A subscriber who pays (card, App
// Store, Play, a coin) and redeems a gift has two active stores, and the
// server names one current_subscription, which can be the gift: the plan then
// keeps the paying store's controls (Manage Subscription, the crypto line), so
// they can stop paying during the gift year, and the gift line does not take
// their place. So the gift is the plan's store only while the balance's
// subscriptions list shows no other active store; otherwise the first other
// one, in the server's order (by market, the order it picks the current one
// by). android GiftSubscription.kt planStore is the same rule.
//
// currentFamily: urnet::classifySubscriptionStore(current_subscription.store),
// or "" without a current subscription; activeFamilies: the same for each of
// the balance's subscriptions, in order.
inline std::string PlanStoreFamily(const std::string& currentFamily,
                                   const std::vector<std::string>& activeFamilies) {
  if (currentFamily != kStoreFamilyGift) return currentFamily;
  for (const std::string& family : activeFamilies) {
    if (!family.empty() && family != kStoreFamilyGift) return family;
  }
  return currentFamily;
}

}  // namespace urnw
