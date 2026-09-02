// Usage bar (port of the apple Shared/Views/UsageBar.swift, with the android
// port's legend): the stacked used / pending / available balance bar, the
// series legend, the daily-data-balance row, and the referral row
// ("Total referrals: N" / "+N*30 GiB/Month"). Non-zero segments are widened to
// a 1.5% floor so a sliver of pending data still reads (mac minNonZeroValue),
// and the bar's 12px corner radius matches the card system.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include <gtkmm.h>

namespace urnw {

class UsageBar : public Gtk::Box {
 public:
  UsageBar();

  void SetData(int64_t usedByteCount, int64_t pendingByteCount, int64_t availableByteCount,
               int64_t dailyBalanceByteCount, int64_t totalReferrals);
  // the referral row; off where referrals have their own page
  void SetShowReferrals(bool show);
  // the program's cap and bonus (server terms), for the referral bonus line
  void SetReferralTerms(int64_t maxReferrals, int64_t bonusGibPerDay);

 private:
  void DrawBar(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height);

  Gtk::DrawingArea bar_;
  Gtk::Label* dailyBalanceValue_ = nullptr;
  Gtk::Label* referralCount_ = nullptr;
  Gtk::Label* referralBonus_ = nullptr;
  Gtk::Separator* referralSeparator_ = nullptr;
  Gtk::Box* referralRow_ = nullptr;
  int64_t maxReferrals_ = 20;
  int64_t bonusGibPerDay_ = 3;
  int64_t totalReferrals_ = 0;

  int64_t used_ = 0;
  int64_t pending_ = 0;
  int64_t available_ = 0;
};

}  // namespace urnw
