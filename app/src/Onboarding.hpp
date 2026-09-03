// The post-sign-up onboarding flow (android IntroNavHost parity): four pages
// in a modal sheet — welcome + plan, your bandwidth, contribute bandwidth,
// refer friends — with the shared top bar (step bubbles, back, a muted Skip),
// the route line with the walking ur-people on page 1, and the connector mark
// that flies from the route into the header on the later pages.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "RedeemCodeSheet.hpp"
#include "SdkHost.hpp"
#include "SubscriptionBalance.hpp"
#include "UpgradeSheet.hpp"
#include "UsageBar.hpp"

namespace urnw {

inline constexpr int kOnboardingSteps = 4;
inline constexpr int64_t kFreeTrialDays = 15;  // the Stripe checkout's trial (server pro terms)

class RouteLine;
class StepBubbles;
class GoldPlanCard;
class ReferralPanel;
class ReferralProgressBox;

class OnboardingWindow : public Gtk::Window {
 public:
  OnboardingWindow(Gtk::Window& parent, SdkHost& host, SubscriptionBalanceStore& balance);
  ~OnboardingWindow() override;

  void Open();
  void OpenAt(int step);  // design review: open on a given page
  // the flow ended (skip, or Get connected): the owner clears the pending flag
  std::function<void()> on_finished;

 private:
  void BuildUi();
  void BuildTopBar(Gtk::Box& column);
  Gtk::Widget* WrapPage(Gtk::Widget& page);
  void BuildWelcome();
  void BuildBandwidth();
  void BuildProvide();
  void BuildReferral();
  void ShowStep(int step);
  void Finish();
  void RefreshBalance();
  void RefreshReferral();
  void SelectPlan(bool yearly);
  void FlyConnector(bool toHeader);
  void PlaceConnector(double x, double y, double size);

 protected:
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

 private:

  SdkHost& host_;
  SubscriptionBalanceStore& balance_;
  int step_ = 1;
  bool yearly_ = true;
  bool syncingProvide_ = false;

  Gtk::Overlay overlay_;
  Gtk::Stack stack_;
  Gtk::Button* back_ = nullptr;
  Gtk::Button* skip_ = nullptr;
  Gtk::Box* headerSlot_ = nullptr;
  StepBubbles* bubbles_ = nullptr;
  // the flying mark, drawn by the window's own snapshot over everything
  bool connectorVisible_ = false;
  double connectorX_ = 0, connectorY_ = 0, connectorSize_ = 0;
  guint flightTick_ = 0;
  double flightStart_ = 0;
  double fromX_ = 0, fromY_ = 0, fromSize_ = 0, toX_ = 0, toY_ = 0, toSize_ = 0;
  bool connectorInHeader_ = false;

  RouteLine* route_ = nullptr;
  GoldPlanCard* yearlyCard_ = nullptr;
  Gtk::Button* monthlyCard_ = nullptr;
  Gtk::Button* startButton_ = nullptr;
  Gtk::Label* monthlyDot_ = nullptr;
  std::unique_ptr<UpgradeSheet> checkout_;
  std::unique_ptr<RedeemCodeSheet> redeem_;

  UsageBar* usage_ = nullptr;
  Gtk::Label* dailyLine_ = nullptr;
  sigc::connection balancePoll_;

  std::vector<Gtk::CheckButton*> provideChecks_;

  Gtk::Box* perkYou_ = nullptr;
  Gtk::Box* perkFriend_ = nullptr;
  ReferralProgressBox* referralProgress_ = nullptr;
  ReferralPanel* referralPanel_ = nullptr;
};

}  // namespace urnw
