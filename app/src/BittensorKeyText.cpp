// SPDX-License-Identifier: MPL-2.0
#include "BittensorKeyText.hpp"

#include <urnetwork_sdk.hpp>

#include "I18n.hpp"
#include "PaneKit.hpp"

namespace urnw {

Glib::ustring BittensorKeyFailureText(const std::string& code, const std::string& message) {
  if (code.empty()) return {};
  if (code == urnet::SnErrorCodeChallengeExpired) {
    return T_("sn_consent_expired", "The request expired. Try again.");
  }
  if (code == urnet::SnErrorCodeSignatureMismatch) {
    return T_("sn_consent_failed", "The signature didn't match this wallet. Try again.");
  }
  if (code == urnet::SnErrorCodeNetworkOwnerOnly ||
      code == urnet::BittensorLocalKeyFailureNotAllowed) {
    return T_("sn_consent_owner_only", "Only the network owner can choose the payout wallet.");
  }
  if (code == urnet::BittensorLocalKeyFailureGenerate) {
    return T_("bittensor_key_failure_generate", "Couldn't make a key on this device. Try again.");
  }
  if (code == urnet::BittensorLocalKeyFailureStore) {
    return T_("bittensor_key_failure_store",
              "Couldn't save the key on this device, so nothing was signed. Try again, or save "
              "the recovery phrase first.");
  }
  if (code == urnet::BittensorLocalKeyFailureStoreUnavailable) {
    return T_("bittensor_key_failure_keychain",
              "Couldn't read this device's keychain. Unlock your device and try again.");
  }
  if (code == urnet::BittensorLocalKeyFailureDelete) {
    return T_("bittensor_key_failure_delete",
              "Couldn't delete the key from this device. It's still here.");
  }
  if (code == urnet::BittensorLocalKeyFailureReplacementNotConfirmed) {
    // the card warns first, so this reads only if a warning was skipped
    return T_("bittensor_key_replace_message",
              "This key replaces your payout wallet from the next epoch. Earlier earnings stay "
              "on the old wallet.");
  }
  // a consent failure: the server's or the sdk's own text, else the generic one
  if (message.empty()) {
    return T_("bittensor_key_failure_consent", "Couldn't set up payouts. Try again.");
  }
  return Glib::ustring(message);
}

Glib::ustring BittensorFundsDisclaimerText(const std::string& disclaimer) {
  if (disclaimer == urnet::BittensorFundsDisclaimerFull) {
    return T_("bittensor_funds_disclaimer",
              "This key is stored locally in the app secure storage. It is never shared with "
              "URnetwork. URnetwork does not touch and is not responsible for any user funds.");
  }
  if (disclaimer == urnet::BittensorFundsDisclaimerFundsOnly) {
    return T_("bittensor_funds_disclaimer_funds_only",
              "URnetwork does not touch and is not responsible for any user funds.");
  }
  return {};
}

Gtk::Box* MakeBittensorFundsDisclaimer(bool boxed) {
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  box->add_css_class(boxed ? "ur-key-disclaimer-boxed" : "ur-key-disclaimer");
  auto* shield = Gtk::make_managed<Gtk::Image>();
  shield->set_from_icon_name("security-high-symbolic");
  shield->set_valign(Gtk::Align::START);
  shield->add_css_class("ur-key-muted-icon");
  kit::MarkDecorative(*shield);
  box->append(*shield);
  auto* text = Gtk::make_managed<Gtk::Label>();
  text->add_css_class("ur-key-disclaimer-text");
  text->set_xalign(0);
  text->set_wrap(true);
  text->set_hexpand(true);
  box->append(*text);
  box->set_visible(false);
  return box;
}

void SetBittensorFundsDisclaimer(Gtk::Box& disclaimer, const std::string& id) {
  const Glib::ustring text = BittensorFundsDisclaimerText(id);
  if (auto* label = dynamic_cast<Gtk::Label*>(disclaimer.get_last_child())) label->set_text(text);
  disclaimer.set_visible(!text.empty());
}

}  // namespace urnw
