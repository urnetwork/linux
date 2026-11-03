// The words the one-tap Bittensor key's screens share (apple BittensorKeyCard.swift,
// BittensorKeyText and BittensorFundsDisclaimerText): what a failure the flow
// reports reads as, and the owner's funds disclaimer. The SDK says which: a
// BittensorLocalKeyFailure* or consent refusal code, and a
// BittensorFundsDisclaimer* id, which is the store key itself. Nothing here
// decides where either shows.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#include <gtkmm.h>

namespace urnw {

// The words for a failure the flow reports (card.failure_code /
// screen.failure_code, with its failure_message), "" for none.
Glib::ustring BittensorKeyFailureText(const std::string& code, const std::string& message);

// The owner's disclaimer for the SDK's id: the whole text, its last sentence,
// or "" for none (urnet::BittensorFundsDisclaimerNone).
Glib::ustring BittensorFundsDisclaimerText(const std::string& disclaimer);

// The disclaimer as the easy flows draw it: a shield and the text, in its own
// box at the foot of a screen of its own (`boxed`), plain on a card. Hidden
// while the id says none; SetBittensorFundsDisclaimer updates it.
Gtk::Box* MakeBittensorFundsDisclaimer(bool boxed);
void SetBittensorFundsDisclaimer(Gtk::Box& disclaimer, const std::string& id);

}  // namespace urnw
