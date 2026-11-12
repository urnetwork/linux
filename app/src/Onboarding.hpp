// The post-sign-up onboarding flow (android IntroNavHost parity): five pages
// in a modal sheet — welcome + plan, your bandwidth, contribute bandwidth,
// refer friends, the welcome offer — with the shared top bar (step bubbles,
// back, a muted Skip), the route line with the walking ur-people on page 1,
// and the connector mark that flies from the route into the header on the
// later pages.
//
// The welcome offer (mmm/onboarding/PLAN.md): the plan page issues the
// network's offer (POST /onboarding/offer/issue, surface intro_step) and
// prints it on the yearly card with the deadline and the trial timeline; the
// final page restates the same offer with one primary CTA and "Continue with
// the free plan", and everyone reaches it — Skip lands there once
// (OnboardingRouting.hpp). The offer.in_app holdout sees the four-page flow
// with the regular picker and no issue call. Every page transition, the
// offer surfaces and the decline are client events (ClientEvents.hpp).
//
// Both purchase pages carry the payment-method row (mmm/ZCASH-CONTRACT.md
// §6.3: the onboarding skips the upgrade sheet's options, so the row joins
// its own plan picker). It is one choice shown twice: picking a coin on
// either page greys monthly out, drops the card's trial lines, turns the CTA
// into "Pay with {method}" over the coin's fine print, and the CTA opens the
// coin's invoice (UpgradeSheet::OpenCryptoCheckout) instead of the card
// checkout. With an active offer a coin buys the welcome offer.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "CryptoPay.hpp"
#include "OfferCard.hpp"
#include "OnboardingRouting.hpp"
#include "PaymentMethodRow.hpp"
#include "PlanPicker.hpp"
#include "RedeemCodeSheet.hpp"
#include "SdkHost.hpp"
#include "SubscriptionBalance.hpp"
#include "UpgradeSheet.hpp"
#include "UsageBar.hpp"

namespace urnw {

// the most pages the flow has (the bubbles size for it; the holdout shows four)
inline constexpr int kOnboardingSteps = kOnboardingStepOffer;

class RouteLine;
class StepBubbles;
class ReferralPanel;

class OnboardingWindow : public Gtk::Window {
 public:
  OnboardingWindow(Gtk::Window& parent, SdkHost& host, SubscriptionBalanceStore& balance);
  ~OnboardingWindow() override;

  void Open();
  void OpenAt(int step);  // design review: open on a given page
  // The urnetwork://onboarding/offer destination: the offer page on its own
  // (no earlier pages; the link is the way out). Only while an offer is active.
  void OpenOffer();
  // the flow ended (skip, or Get connected): the owner clears the pending flag
  std::function<void()> on_finished;
  // the flow's checkout is for a legacy guest network, which buys nothing:
  // the balance said so at the tap, or the server refused the checkout
  // (guest_sign_in_required). The flow hid, and the owner opens the
  // conversion.
  std::function<void()> on_guest_sign_in_required;

 private:
  void BuildUi();
  void BuildTopBar(Gtk::Box& column);
  Gtk::Widget* WrapPage(Gtk::Widget& page);
  void BuildWelcome();
  void BuildBandwidth();
  void BuildProvide();
  void BuildReferral();
  void BuildOffer();
  void ShowStep(int step);
  void Skip();
  void Finish();
  void RefreshBalance();
  void RefreshReferral();
  // the plan cards and the offer texts from the balance store's tier/offer
  void ApplyPrices();
  // POST /onboarding/offer/issue from the plan page (once per open)
  void IssueOffer();
  void StartCheckout(bool yearly, const char* surface);
  int64_t StepElapsedMs() const;
  void SelectPlan(bool yearly);
  // ---- the payment method (both purchase pages) ----
  // The offer as the pages show it (the holdout's picker shows none).
  bool ShowsOffer() const;
  // The rows' segments from the balance, for the plan a coin buys here;
  // a list change re-selects (`remembered`: the device's last method).
  void ApplyMethods(bool remembered);
  // What the method changes on both pages.
  void ApplyMethod();
  void SelectMethod(const std::string& method);
  Glib::ustring StartLabel() const;
  Glib::ustring OfferCtaLabel() const;
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
  bool offerEnabled_ = true;   // not the offer.in_app holdout
  bool offerIssued_ = false;   // the issue call went out this open
  bool introOfferShown_ = false;  // offer.screen.shown(intro_step) emitted
  bool standalone_ = false;    // OpenOffer: the offer page alone
  double stepShownAt_ = 0;     // ms, for the step events' elapsed_ms

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
  PlanPicker* plans_ = nullptr;
  Gtk::Button* startButton_ = nullptr;
  std::unique_ptr<UpgradeSheet> checkout_;
  std::unique_ptr<RedeemCodeSheet> redeem_;

  UsageBar* usage_ = nullptr;
  Gtk::Label* dailyLine_ = nullptr;
  sigc::connection balancePoll_;

  std::vector<Gtk::CheckButton*> provideChecks_;

  Gtk::Box* perkYou_ = nullptr;
  Gtk::Box* perkFriend_ = nullptr;
  ReferralPanel* referralPanel_ = nullptr;
  Gtk::Button* referralDone_ = nullptr;

  // the offer on the plan page (under the picker) and the final page
  OfferCard* welcomeOffer_ = nullptr;
  Gtk::Label* offerEyebrow_ = nullptr;
  Gtk::Label* offerHeadline_ = nullptr;
  Gtk::Label* offerPrice_ = nullptr;
  Gtk::Label* offerThen_ = nullptr;
  Gtk::Label* offerTrial_ = nullptr;
  OfferCard* offerCard_ = nullptr;
  Gtk::Button* offerCta_ = nullptr;

  // the payment method: one choice, a row on each purchase page
  std::string method_ = crypto_pay::kMethodCard;
  bool methodPicked_ = false;  // picked in this open
  PaymentMethodRow* welcomeMethods_ = nullptr;
  PaymentMethodRow* offerMethods_ = nullptr;
  Gtk::Label* welcomeFinePrint_ = nullptr;
  Gtk::Label* offerFinePrint_ = nullptr;
};

}  // namespace urnw
