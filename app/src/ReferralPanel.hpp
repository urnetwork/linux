// The referral pieces the onboarding "Refer friends" step and the Account
// "Refer and earn" page share (android ReferralGoldPanel + the referral
// progress card): ONE implementation, so the two surfaces cannot drift in
// wording, numbers or the crowned state. Every figure comes from the balance
// store's ReferralTerms (the server's cap and bonus); nothing here hardcodes
// the bonus.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

#include <gtkmm.h>

#include "ReferralRoyalty.hpp"  // ReferralTerms

namespace urnw {

// The onboarding CSS vocabulary (ur-onb-*) these pieces are styled with.
// Idempotent; every constructor below calls it.
void EnsureOnboardingCss();

// The referral progress card: "Refer friends" and "n/max" over a gold bar with
// its legend. `max` is the code's cap from the referral terms; the bar reaches
// the end when the code is used up.
class ReferralProgressBox : public Gtk::Box {
 public:
  ReferralProgressBox();
  void Update(int64_t totalReferrals, const ReferralTerms& terms);

 private:
  Gtk::Label* count_ = nullptr;
  Gtk::DrawingArea* bar_ = nullptr;
  double fraction_ = 0;
};

// The referral king-frog panel (android ReferralGoldPanel): a gold-washed
// surface with a pulsing aura, the frog, the copy, the code pill and share.
// Crowned (heading + congrats) as soon as the network has one referral.
class ReferralPanel : public Gtk::Box {
 public:
  ReferralPanel();
  ~ReferralPanel() override;
  void Update(const std::string& referralCode, int64_t totalReferrals,
              const ReferralTerms& terms);

 protected:
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

 private:
  void Draw(const Cairo::RefPtr<Cairo::Context>& cr, double w, double h);

  Gtk::Label heading_;
  Gtk::Label detail_;
  Gtk::Box* codePill_ = nullptr;
  Gtk::Label code_;
  Gtk::Button* copy_ = nullptr;
  Gtk::Button* share_ = nullptr;
  Gtk::Label status_;
  std::string referralCode_;
  bool crowned_ = false;
  double breath_ = 0.5;
  double start_ = 0;
  guint tick_ = 0;
};

}  // namespace urnw
