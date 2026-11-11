// The payment-method row: Card · USDC on Solana · Zcash · Monero · More coins
// as linked toggle buttons (mmm/ZCASH-CONTRACT.md §6.1, §6.3 Linux: no
// AdwToggleGroup, which the AppImage's libadwaita 1.5 lacks), labelled "Pay
// with" for a screen reader. It sits directly above the buy button, after the
// plan cards, on the upgrade sheet's options and on the onboarding's welcome
// and offer pages.
//
// The segments are Card, then the SDK's coin selector
// (SubscriptionBalanceStore::PaymentTabs over urnet::cryptoPaymentTabs, in its
// order and with its labels): a tab of one coin is that coin, and More coins
// (mmm/BTCPAY.md §17.6) shows the TAO, Bitcoin, ETH and SOL marks, each its
// own with space between them, and opens a list of those coins under the row,
// each with its network. With no tab the row hides itself and the surface looks
// exactly as before. A coin or tab that cannot be paid with now stays in its
// place and cannot be chosen: its label greys with "Temporarily unavailable"
// under it, and its mark stays at full contrast (no insensitive state, whose
// libadwaita filter would dim the mark with the label). "Temporarily
// unavailable" also shows under the row while the selected coin's last start
// failed with asset_unavailable; the coin stays selectable for a retry.
//
// The row only shows and reports the choice: the host follows it (the button
// and its fine print, monthly greyed for a coin) and remembers it on the
// device (CryptoInvoiceHost::SetPaymentMethod).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "CryptoPay.hpp"

namespace urnw {

// A method's name for "Pay with {}" and "Paying with {} is temporarily
// unavailable": Card's, or the SDK's label of a coin (urnet::cryptoAssetLabelKey
// and urnet::cryptoAssetLabel: "USDC on Solana", "TAO", "Bitcoin"), looked up
// by its key with the SDK's English as the msgid. The id itself for a method
// nothing labels.
Glib::ustring PaymentMethodName(const std::string& method);

// The store a method's offer and purchase events name (§6.1): a coin's market
// (urnet::cryptoAssetMarket: solana for USDC and SOL, zcash, monero,
// bittensor, bitcoin, ethereum), stripe for Card.
std::string PaymentMethodEventStore(const std::string& method);

class PaymentMethodRow : public Gtk::Box {
 public:
  PaymentMethodRow();

  // The SDK's coin selector to offer beside Card. True when it changed, so the
  // host chooses the selection again (Select); until then a selection the
  // tabs no longer offer falls back to Card. on_select does not fire for
  // either.
  bool SetTabs(const std::vector<crypto_pay::PaymentTabView>& tabs);
  // Selects `method` while it is offered, Card otherwise. Does not fire
  // on_select.
  void Select(const std::string& method);
  const std::string& Selected() const { return selected_; }
  // "Temporarily unavailable" under the row.
  void SetUnavailable(bool unavailable);
  // Takes picks (true), or holds them while a start is in flight (false):
  // the labels grey, the marks never do.
  void SetEnabled(bool enabled);

  // Fired when the user picks a method.
  std::function<void(const std::string& method)> on_select;

 private:
  // Card's toggle ("" id) or a tab's (its id), with its label.
  struct TabChoice {
    std::string id;
    Gtk::ToggleButton* button = nullptr;
    Gtk::Label* label = nullptr;
    bool available = true;
  };
  // A coin of the More coins list, with its name.
  struct CoinChoice {
    std::string asset;
    Gtk::CheckButton* button = nullptr;
    Gtk::Label* label = nullptr;
    bool available = true;
  };

  void Rebuild();
  // the toggles and the More coins list show selected_
  void Sync();
  // each choice takes picks while the row is enabled and the choice available
  void ApplyChoosable();
  // a pick of the user's: the selection, the coin More coins selects again,
  // and on_select
  void Pick(const std::string& method);

  Gtk::Box* toggles_ = nullptr;
  Gtk::Box* coins_ = nullptr;  // the More coins list, titled with its name
  Gtk::Label* unavailable_ = nullptr;
  std::vector<TabChoice> tabChoices_;
  std::vector<CoinChoice> coinChoices_;
  std::vector<crypto_pay::PaymentTabView> tabs_;
  std::string selected_;
  std::string wanted_;  // the coin More coins selects when it is chosen again
  bool enabled_ = true;
  bool syncing_ = false;  // set_active from Sync, not a pick
};

}  // namespace urnw
