// What the upgrade sheet does with the answer to its card payment report
// (UpgradeSheet::ReportCardPayment over the SDK's ReportStripePurchase, which
// calls back once, after its retries; server POST
// /subscription/verify-stripe-checkout).
//
//   - credited or already_credited: the server holds the credit, so the sheet
//     reads the balance now and its state follows the balance, as before.
//   - an answer that carries an error is a refusal, whatever its status (the
//     SDK's rule: the server's last word on the purchase, which every later
//     report of it answers the same). Today that is a yearly trial whose card
//     failed the CVC check (status invalid, error.code incorrect_cvc): the
//     trial did not start, so no Pro, and the card was detached, so it is
//     never charged. The balance never carries a refusal, so a confirmation
//     left running would spend its budget and then say the payment has not
//     arrived yet. The attempt is over instead: the sheet ends the
//     confirmation and goes back to the options with the refusal's line
//     (ServerRefusalText.hpp) and Try again, which starts a NEW payment sheet
//     or checkout session. Nothing falls through to the next checkout path.
//   - anything else (pending after the retries, invalid or wrong_network
//     without a reason, no answer): the confirmation poll decides, as before.
//
// A refusal is the sheet's to show only while the sheet is still on that
// attempt: the attempt is its latest (a new checkout or invoice starts a new
// one) and for the network signed in now (the sheet outlives a sign-out), and
// the sheet is waiting for the confirmation, timed out on it, or back on its
// options (closed while it waited, then opened again). A sheet on screen
// shows it at once; a hidden one keeps it for its next open. The hosted
// checkout (the AppImage has no WebKitGTK) reports from the moment the
// browser opens, and the SDK asks again while the answer is pending, waiting
// 1 s, 5 s and 30 s, then 5 minutes between reports, for about 21 minutes: its
// refusal can land long after the 2-minute confirmation gave up.
//
// Dependency-free so tests/CardPaymentReportTest.cpp runs it without GTK or
// the SDK; the statuses' spelling stays the SDK's (urnet::PurchaseReportStatus*,
// read by the sheet).
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw {

enum class CardReportOutcome {
  // the confirmation poll decides (pending, a refusal without a reason, no answer)
  Confirming,
  // the server holds the credit: read the balance now
  Credited,
  // the attempt is over: back to the options with the refusal and Try again
  Refused,
};

// `credited`: the status is credited or already_credited. `refused`: the
// answer carries an error. A credit wins: Pro is there whatever else came with
// it.
inline CardReportOutcome CardReportOutcomeFor(bool credited, bool refused) {
  if (credited) return CardReportOutcome::Credited;
  if (refused) return CardReportOutcome::Refused;
  return CardReportOutcome::Confirming;
}

// Where the sheet is when a refusal lands (its State, as far as a refusal
// cares).
enum class CardRefusalStage {
  Options,   // the plans and the button
  Starting,  // a checkout or an invoice starting (Launching)
  Paying,    // the web view's payment form (Checkout) or a coin's invoice (Invoice)
  Waiting,   // waiting for the confirmation
  TimedOut,  // the confirmation gave up
  Success,   // Pro
};

enum class CardRefusalAction {
  // not the sheet's: an older attempt, or the sheet went on to something else
  Ignore,
  // back to the options with the refusal, on screen now
  ShowNow,
  // back to the options; the refusal shows when the sheet next opens
  ShowAtOpen,
};

// `latestAttempt`: the refused attempt is the sheet's latest, for the network
// signed in now. `visible`: the sheet is on screen.
inline CardRefusalAction CardRefusalActionFor(bool latestAttempt, CardRefusalStage stage,
                                              bool visible) {
  if (!latestAttempt) return CardRefusalAction::Ignore;
  switch (stage) {
    case CardRefusalStage::Options:
    case CardRefusalStage::Waiting:
    case CardRefusalStage::TimedOut:
      return visible ? CardRefusalAction::ShowNow : CardRefusalAction::ShowAtOpen;
    case CardRefusalStage::Starting:
    case CardRefusalStage::Paying:
    case CardRefusalStage::Success:
      return CardRefusalAction::Ignore;
  }
  return CardRefusalAction::Ignore;
}

// Whether the refusal ends the confirmation poll: only the one this attempt
// started (waiting, or given up and still flagged). On the options the poll,
// if any runs, is not this attempt's.
inline bool CardRefusalEndsConfirmation(CardRefusalStage stage) {
  return stage == CardRefusalStage::Waiting || stage == CardRefusalStage::TimedOut;
}

}  // namespace urnw
