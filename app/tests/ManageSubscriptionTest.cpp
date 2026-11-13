// The Manage Subscription row's visibility (ManageSubscription.hpp,
// UPGRADE.md D7): the Stripe customer portal shows only for a Stripe
// subscription, never for a free network or a store, crypto or gift
// subscription; a crypto year reads "does not renew" instead, and a gift
// code's year of Pro "A gift. It doesn't renew.".
// The store and the page need GTK and the SDK, so their wiring is read as text.
//
// SPDX-License-Identifier: MPL-2.0
#include "ManageSubscription.hpp"
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::PlanStoreFamily;
using urnw::ShowsGiftDoesNotRenew;
using urnw::ShowsManageSubscription;

std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool Has(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

UR_TEST(manageSubscriptionShowsOnlyForStripe) {
  // the families urnet::classifySubscriptionStore returns
  UR_EXPECT_TRUE(ShowsManageSubscription("stripe"));
  UR_EXPECT_FALSE(ShowsManageSubscription(""));
  UR_EXPECT_FALSE(ShowsManageSubscription("apple"));
  UR_EXPECT_FALSE(ShowsManageSubscription("google"));
  UR_EXPECT_FALSE(ShowsManageSubscription("other"));
  // a one-time crypto year (solana, monero, zcash) has nothing to manage
  UR_EXPECT_FALSE(ShowsManageSubscription("crypto"));
  // nor does a gift code's year of Pro (urnet::SubscriptionStoreGift)
  UR_EXPECT_FALSE(ShowsManageSubscription("gift"));
}

// ...and Account says so instead: "does not renew" for the crypto family
// (CryptoPay.hpp StoreDoesNotRenew), never the portal.
UR_TEST(manageSubscriptionCryptoReadsDoesNotRenew) {
  const std::string page = ReadSource("AccountPage.cpp");
  UR_EXPECT_TRUE(Has(page, "crypto_pay::StoreDoesNotRenew(balance_.subscriptionStoreFamily)"));
  UR_EXPECT_TRUE(Has(page, "T_(\"crypto_does_not_renew\", \"Does not renew. Pay again when it ends.\")"));
}

// A gift code's year of Pro: someone else paid for it and it never renews, so
// the plan pane says so in its own line beside the crypto one, for the gift
// family only, and offers nothing to manage, cancel or pay.
UR_TEST(manageSubscriptionGiftReadsAGiftThatDoesNotRenew) {
  UR_EXPECT_TRUE(ShowsGiftDoesNotRenew("gift"));
  for (const char* family : {"stripe", "apple", "google", "crypto", "other", ""}) {
    UR_EXPECT_FALSE(ShowsGiftDoesNotRenew(family));
  }
  const std::string page = ReadSource("AccountPage.cpp");
  UR_EXPECT_TRUE(Has(page, "T_(\"gift_does_not_renew\", \"A gift. It doesn't renew.\")"));
  UR_EXPECT_TRUE(Has(page, "giftRow_->set_visible(false);"));
  UR_EXPECT_TRUE(Has(page, "ShowsGiftDoesNotRenew(balance_.subscriptionStoreFamily)"));
  // the SDK's family, pinned where the store classifies it
  UR_EXPECT_TRUE(Has(ReadSource("SubscriptionBalance.cpp"),
                     "static_assert(std::string_view(urnet::SubscriptionStoreGift) == "
                     "kStoreFamilyGift);"));
}

// A subscriber who pays and redeems a gift keeps the paying store's controls:
// the gift is the plan's store only while no other store is active, otherwise
// the first other one in the server's order (android planStore's rule).
UR_TEST(manageSubscriptionAGiftNeverHidesAPayingStore) {
  using Families = std::vector<std::string>;
  // no gift: the current subscription's family, whatever else is active
  UR_EXPECT_TRUE(PlanStoreFamily("", Families{}).empty());
  UR_EXPECT_TRUE(PlanStoreFamily("stripe", Families{"gift", "stripe"}) == "stripe");
  UR_EXPECT_TRUE(PlanStoreFamily("apple", Families{"apple", "gift"}) == "apple");
  // a gift and nothing else active: the gift
  UR_EXPECT_TRUE(PlanStoreFamily("gift", Families{}) == "gift");
  UR_EXPECT_TRUE(PlanStoreFamily("gift", Families{"gift"}) == "gift");
  UR_EXPECT_TRUE(PlanStoreFamily("gift", Families{"", "gift"}) == "gift");
  // a gift beside a paying store (gift sorts before google, monero, solana,
  // stripe and zcash): the paying store, first in the server's order
  UR_EXPECT_TRUE(PlanStoreFamily("gift", Families{"gift", "stripe"}) == "stripe");
  UR_EXPECT_TRUE(PlanStoreFamily("gift", Families{"gift", "google"}) == "google");
  UR_EXPECT_TRUE(PlanStoreFamily("gift", Families{"gift", "crypto", "stripe"}) == "crypto");
  UR_EXPECT_TRUE(PlanStoreFamily("gift", Families{"gift", "other"}) == "other");
  // ...so the card subscriber keeps Manage Subscription, and no gift line
  const std::string card = PlanStoreFamily("gift", Families{"gift", "stripe"});
  UR_EXPECT_TRUE(ShowsManageSubscription(card));
  UR_EXPECT_FALSE(ShowsGiftDoesNotRenew(card));
  // the store classifies every active subscription and asks the rule
  const std::string store = ReadSource("SubscriptionBalance.cpp");
  UR_EXPECT_TRUE(Has(store, "for (const auto& subscription : *result->subscriptions) {"));
  UR_EXPECT_TRUE(
      Has(store, "activeFamilies.push_back(urnet::classifySubscriptionStore(subscription.store));"));
  UR_EXPECT_TRUE(Has(store, "subscriptionStoreFamily_ = PlanStoreFamily("));
}

UR_TEST(manageSubscriptionRowFollowsTheSubscriptionStore) {
  const std::string store = ReadSource("SubscriptionBalance.cpp");
  UR_EXPECT_TRUE(Has(store, "urnet::classifySubscriptionStore(result->current_subscription->store)"));
  const std::string window = ReadSource("MainWindow.cpp");
  UR_EXPECT_TRUE(Has(window, "snapshot.subscriptionStoreFamily = balance_.SubscriptionStoreFamily();"));
  const std::string page = ReadSource("AccountPage.cpp");
  UR_EXPECT_TRUE(Has(page, "portalRow_->set_visible(ShowsManageSubscription(balance_.subscriptionStoreFamily))"));
}

}  // namespace
