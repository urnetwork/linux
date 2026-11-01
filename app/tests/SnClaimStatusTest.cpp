// The word an epoch's v1 SN25α claim status reads as on the Earnings page, in
// the history and the claim sheet (SnPayoutPresentation.hpp ClaimStatusWordFor;
// support inbox 8126). The SDK reports an epoch the vault finalized without a
// payout root as "not-finalized": it reads No payout published, never as
// unsettled, and an epoch the vault has not finalized is not claimable, so it
// reads Pending rather than Unclaimed.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>

#include "SnPayoutPresentation.hpp"

using urnw::snpayout::ClaimStatusWord;
using urnw::snpayout::ClaimStatusWordFor;

UR_TEST(SnClaimStatusClaimableReadsUnclaimed) {
  UR_EXPECT_TRUE(ClaimStatusWordFor("claimable") == ClaimStatusWord::Unclaimed);
}

UR_TEST(SnClaimStatusClaimedReadsClaimed) {
  UR_EXPECT_TRUE(ClaimStatusWordFor("claimed") == ClaimStatusWord::Claimed);
}

UR_TEST(SnClaimStatusExpiredReadsExpired) {
  UR_EXPECT_TRUE(ClaimStatusWordFor("expired") == ClaimStatusWord::Expired);
}

UR_TEST(SnClaimStatusOpenReadsPendingNotUnclaimed) {
  UR_EXPECT_TRUE(ClaimStatusWordFor("open") == ClaimStatusWord::Pending);
}

UR_TEST(SnClaimStatusNotFinalizedReadsNoPayoutPublished) {
  UR_EXPECT_TRUE(ClaimStatusWordFor("not-finalized") == ClaimStatusWord::NoPayoutPublished);
}

UR_TEST(SnClaimStatusUnknownReadsPendingNotAKnownWord) {
  UR_EXPECT_TRUE(ClaimStatusWordFor("pending-review") == ClaimStatusWord::Pending);
  UR_EXPECT_TRUE(ClaimStatusWordFor("") == ClaimStatusWord::Pending);
}
