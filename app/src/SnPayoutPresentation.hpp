// The SN payout line on the Earnings page, decided in pure steps so every
// display rule is deterministic and testable; the page only draws what this
// decided.
//
// Provider payouts moved from weekly USDC to the UR subnet, and nothing on the
// page said how or when a provider is paid ("used it a month, no crypto"). Per
// the sn whitepaper (§5.2, §8.3), earnings settle every epoch and are paid in
// SN25α to the provider's Bittensor coldkey when the provider claims them; the
// app never claims by itself. The line under the points says so, with the
// current epoch's times from the epoch schedule the SDK reads with the claims
// (the coordinator's claim-open offset and claim TTL), so the copy never fixes
// a duration. Claim shows while something is claimable; with no coldkey the
// line asks for one.
//
// Header-only and free of GTK and the SDK so the unit tests need no vendored
// headers (tests/SnPayoutPresentationTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>

namespace urnw::snpayout {

// The current epoch's settlement times (the SDK's SnEpochSchedule): when it
// ends, when its share can be claimed and when an unclaimed share expires,
// and the policy's epoch length in blocks (the one-tap key's reminder reads it).
struct EpochSchedule {
  int64_t epoch = 0;
  int64_t endMillis = 0;
  int64_t claimOpenMillis = 0;
  int64_t expiryMillis = 0;
  int64_t epochBlocks = 0;
};

enum class LineKind {
  Hidden,      // whether a coldkey is set is not known yet
  SetColdkey,  // set_coldkey_to_get_paid, with set_coldkey opening the coldkey flow
  Schedule,    // sn_payout_schedule, then sn_payout_schedule_times when known
};

// What the line under the points shows.
struct PayoutLineView {
  LineKind kind = LineKind::Hidden;
  bool showTimes = false;  // sn_payout_schedule_times, with the three times below
  std::string epochEnd;
  std::string claimOpen;
  std::string expiry;
  bool showClaim = false;  // Claim, which opens the claim sheet

  bool operator==(const PayoutLineView& other) const {
    return kind == other.kind && showTimes == other.showTimes && epochEnd == other.epochEnd &&
           claimOpen == other.claimOpen && expiry == other.expiry &&
           showClaim == other.showClaim;
  }
};

// The line for the page. Hidden while it is not known whether a coldkey is set
// (the wallet is loading, or its read failed); SetColdkey with none, since alpha
// goes only to a coldkey; otherwise the schedule, with the times when the
// schedule is known and its epoch has not already ended (an ended epoch waits
// for the next read), and Claim while something is claimable.
inline PayoutLineView PayoutLineFor(bool walletKnown, bool hasColdkey, int64_t totalClaimableRao,
                                    const std::optional<EpochSchedule>& schedule,
                                    int64_t nowMillis,
                                    const std::function<std::string(int64_t)>& formatTime) {
  PayoutLineView view;
  if (!walletKnown) return view;
  if (!hasColdkey) {
    view.kind = LineKind::SetColdkey;
    return view;
  }
  view.kind = LineKind::Schedule;
  if (schedule && nowMillis < schedule->endMillis) {
    view.showTimes = true;
    view.epochEnd = formatTime(schedule->endMillis);
    view.claimOpen = formatTime(schedule->claimOpenMillis);
    view.expiry = formatTime(schedule->expiryMillis);
  }
  view.showClaim = totalClaimableRao > 0;
  return view;
}

// "2026-10-13 02:00": a schedule time in the reader's local time, given the
// zone's offset from UTC at that instant (the page asks GLib per instant, so a
// daylight change before the expiry is honored). Civil-from-days, so no locale.
inline std::string FormatScheduleTime(int64_t millis, int32_t utcOffsetMinutes) {
  constexpr int64_t kDayMillis = 86'400'000;
  const int64_t local = millis + static_cast<int64_t>(utcOffsetMinutes) * 60'000;
  int64_t days = local / kDayMillis;
  int64_t rest = local % kDayMillis;
  if (rest < 0) {
    rest += kDayMillis;
    --days;
  }
  // civil-from-days (Howard Hinnant)
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  const int64_t minutes = rest / 60'000;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld %02lld:%02lld",
                static_cast<long long>(year), static_cast<long long>(month),
                static_cast<long long>(day), static_cast<long long>(minutes / 60),
                static_cast<long long>(minutes % 60));
  return buf;
}

// The word an epoch's v1 claim status (the SDK's SnEpochClaim.Status) reads as
// in the Earnings history and the claim sheet. "not-finalized" is, despite its
// name, an epoch the vault finalized with nothing for this coldkey to claim (no
// payout root was published for it, which is final, or its payout list holds no
// share for this coldkey), so it reads No payout published, never as unsettled.
// "open" (the vault has not finalized it) and a status this build does not know
// read Pending, which promises nothing; neither is claimable.
enum class ClaimStatusWord {
  Unclaimed,          // unclaimed: claimable now
  Claimed,            // claim_confirmed
  Expired,            // claim_expired
  Pending,            // pending_data_key
  NoPayoutPublished,  // sn_status_not_finalized
};

inline ClaimStatusWord ClaimStatusWordFor(const std::string& status) {
  if (status == "claimable") return ClaimStatusWord::Unclaimed;
  if (status == "claimed") return ClaimStatusWord::Claimed;
  if (status == "expired") return ClaimStatusWord::Expired;
  if (status == "not-finalized") return ClaimStatusWord::NoPayoutPublished;
  return ClaimStatusWord::Pending;
}

}  // namespace urnw::snpayout
