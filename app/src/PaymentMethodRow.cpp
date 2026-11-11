// SPDX-License-Identifier: MPL-2.0
#include "PaymentMethodRow.hpp"

#include <exception>
#include <utility>

#include <urnetwork_sdk.hpp>

#include "BrandIcons.hpp"  // CoinMark
#include "I18n.hpp"
#include "PaneKit.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// a mark on the More coins tab, and beside a coin in its list
constexpr int kTabMarkPx = 16;
constexpr int kCoinMarkPx = 20;

// The tab a method sits in (urnet::cryptoPaymentTabId): the asset itself for
// USDC on Solana, Zcash and Monero, more_coins for TAO, Bitcoin, ETH and SOL,
// "" for Card. The remembered method selects this tab.
std::string PaymentTabIdFor(const std::string& method) {
  if (!crypto_pay::IsCoin(method)) return std::string();
  try {
    return urnet::cryptoPaymentTabId(method);
  } catch (const std::exception&) {
    return std::string();
  }
}

// A tab the other way, as the SDK lays it out (urnet::CryptoPaymentTab): what
// urnet::cryptoPaymentTabCoin reads. The coins' network lines are no part of
// the SDK's tab and stay behind.
urnet::CryptoPaymentTab TabToSdk(const crypto_pay::PaymentTabView& tab) {
  urnet::CryptoPaymentTab sdkTab;
  sdkTab.id = tab.id;
  sdkTab.label = tab.label;
  sdkTab.label_key = tab.labelKey;
  sdkTab.icon_keys = tab.iconKeys;
  urnet::CryptoPaymentCoinList sdkCoins;
  for (const crypto_pay::PaymentCoinView& coin : tab.coins) {
    urnet::CryptoPaymentCoin sdkCoin;
    sdkCoin.asset = coin.asset;
    sdkCoin.label = coin.label;
    sdkCoin.label_key = coin.labelKey;
    sdkCoin.icon_key = coin.iconKey;
    sdkCoin.available = coin.available;
    sdkCoins.push_back(std::move(sdkCoin));
  }
  sdkTab.coins = std::move(sdkCoins);
  sdkTab.available = tab.available;
  return sdkTab;
}

// The coin a tap on `tab` chooses (urnet::cryptoPaymentTabCoin): `lastChosen`,
// the coin chosen last in that tab (crypto_pay::LastChosenCoins), while it can
// be paid with now; else `remembered`, the method remembered on this device,
// while it is one of the tab's coins that can; else the tab's first that can.
// "" when none can (the tab cannot be chosen), for a coin this app does not
// take, and on an SDK error.
std::string TabCoinFor(const crypto_pay::PaymentTabView& tab, const std::string& lastChosen,
                       const std::string& remembered) {
  try {
    const std::string coin = urnet::cryptoPaymentTabCoin(TabToSdk(tab), lastChosen, remembered);
    return crypto_pay::IsCoin(coin) ? coin : std::string();
  } catch (const std::exception&) {
    return std::string();
  }
}

// A choice's name, and "Temporarily unavailable" under it while the choice
// cannot be made now.
Gtk::Box* MakeChoiceText(Gtk::Label*& label, const Glib::ustring& text, bool available) {
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  box->set_valign(Gtk::Align::CENTER);
  label = Gtk::make_managed<Gtk::Label>(text);
  label->set_xalign(0);
  box->append(*label);
  if (!available) {
    auto* note =
        Gtk::make_managed<Gtk::Label>(T_("pay_method_unavailable", "Temporarily unavailable"));
    note->add_css_class("dim-label");
    note->add_css_class("caption");
    note->set_xalign(0);
    box->append(*note);
  }
  return box;
}

// A choice that cannot be made now (or while the row holds picks): its label
// greys, a screen reader hears it disabled, and it takes neither the pointer
// nor the focus. Never insensitive: libadwaita's disabled filter would dim its
// mark with its label.
void SetChoosable(Gtk::Widget& choice, Gtk::Label& label, bool choosable) {
  choice.set_can_target(choosable);
  choice.set_focusable(choosable);
  if (choosable) {
    label.remove_css_class("dim-label");
  } else {
    label.add_css_class("dim-label");
  }
  gtk_accessible_update_state(GTK_ACCESSIBLE(choice.gobj()), GTK_ACCESSIBLE_STATE_DISABLED,
                              choosable ? FALSE : TRUE, -1);
}

}  // namespace

Glib::ustring LabelText(const std::string& key, const std::string& english) {
  if (key.empty()) return Glib::ustring(english);
  return Glib::ustring(T_(key.c_str(), english.c_str()));
}

Glib::ustring PaymentMethodName(const std::string& method) {
  if (method == crypto_pay::kMethodCard) return T_("pay_method_card", "Card");
  if (!crypto_pay::IsCoin(method)) return Glib::ustring(method);
  try {
    return LabelText(urnet::cryptoAssetLabelKey(method), urnet::cryptoAssetLabel(method));
  } catch (const std::exception&) {
    return Glib::ustring(method);
  }
}

std::string PaymentMethodEventStore(const std::string& method) {
  if (!crypto_pay::IsCoin(method)) return urnet::EventStoreStripe;
  try {
    return urnet::cryptoAssetMarket(method);
  } catch (const std::exception&) {
    return std::string();
  }
}

std::string PaymentMethodEventProduct(const std::string& method) {
  if (!crypto_pay::IsCoin(method)) return std::string();
  try {
    return urnet::cryptoPaymentEventProduct(method);
  } catch (const std::exception&) {
    return std::string();
  }
}

PaymentMethodRow::PaymentMethodRow()
    : Gtk::Box(Gtk::Orientation::VERTICAL, 6), selected_(crypto_pay::kMethodCard) {
  EnsureDrawerCss();
  // a group to assistive tech, named "Pay with"; each segment is named by its
  // method. The role is construct-only, so the box comes from the C
  // constructor (PlanPicker's monthly card does the same).
  toggles_ = Gtk::manage(Glib::wrap(GTK_BOX(g_object_new(
      GTK_TYPE_BOX, "orientation", GTK_ORIENTATION_HORIZONTAL, "accessible-role",
      GTK_ACCESSIBLE_ROLE_GROUP, nullptr))));
  toggles_->add_css_class("linked");
  kit::SetAccessibleLabel(*toggles_, T_("pay_method_label", "Pay with"));
  append(*toggles_);

  // More coins' list, under the toggles while that tab is chosen
  coins_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  coins_->set_margin_start(4);
  coins_->set_visible(false);
  append(*coins_);

  unavailable_ = Gtk::make_managed<Gtk::Label>(
      T_("pay_method_unavailable", "Temporarily unavailable"));
  unavailable_->add_css_class("dim-label");
  unavailable_->add_css_class("caption");
  unavailable_->set_xalign(0);
  unavailable_->set_wrap(true);
  unavailable_->set_visible(false);
  append(*unavailable_);

  Rebuild();
}

bool PaymentMethodRow::SetTabs(const std::vector<crypto_pay::PaymentTabView>& tabs) {
  if (tabs == tabs_) return false;
  tabs_ = tabs;
  selected_ = crypto_pay::MethodSelection(tabs_, selected_);
  Rebuild();
  return true;
}

void PaymentMethodRow::Select(const std::string& method) {
  remembered_ = method;
  selected_ = crypto_pay::MethodSelection(tabs_, method);
  lastChosen_.Choose(tabs_, selected_);
  Sync();
}

void PaymentMethodRow::SetUnavailable(bool unavailable) { unavailable_->set_visible(unavailable); }

void PaymentMethodRow::SetEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  ApplyChoosable();
}

void PaymentMethodRow::Pick(const std::string& method) {
  selected_ = crypto_pay::MethodSelection(tabs_, method);
  lastChosen_.Choose(tabs_, selected_);
  Sync();
  if (on_select) on_select(selected_);
}

void PaymentMethodRow::Rebuild() {
  syncing_ = true;
  tabChoices_.clear();
  coinChoices_.clear();
  RemoveAllChildren(*toggles_);  // managed: removing destroys them
  RemoveAllChildren(*coins_);
  Gtk::ToggleButton* group = nullptr;
  const auto addTab = [this, &group](const std::string& id, Gtk::Widget& content, Gtk::Label* label,
                                     bool available) {
    auto* toggle = Gtk::make_managed<Gtk::ToggleButton>();
    toggle->set_child(content);
    toggle->set_hexpand(true);
    if (group) {
      toggle->set_group(*group);
    } else {
      group = toggle;
    }
    toggle->signal_toggled().connect([this, toggle, id] {
      // one report per pick, not per segment toggling off; none for Sync
      if (syncing_ || !toggle->get_active()) return;
      if (id.empty()) {
        Pick(crypto_pay::kMethodCard);
        return;
      }
      for (const crypto_pay::PaymentTabView& tab : tabs_) {
        if (tab.id != id) continue;
        // the SDK's coin for the tap: a tab of one coin is that coin; More
        // coins the coin chosen there last, else the remembered one, else its
        // first that can be paid with now
        const std::string coin = TabCoinFor(tab, lastChosen_.In(tab.id), remembered_);
        if (coin.empty()) {
          Sync();  // nothing to choose there
          return;
        }
        Pick(coin);
        return;
      }
    });
    toggles_->append(*toggle);
    tabChoices_.push_back(TabChoice{id, toggle, label, available});
  };

  // Card first, then the SDK's tabs in its order; nothing at all without
  // them, which hides the row
  if (crypto_pay::MethodRowShown(tabs_)) {
    Gtk::Label* cardLabel = nullptr;
    Gtk::Box* card = MakeChoiceText(cardLabel, PaymentMethodName(crypto_pay::kMethodCard), true);
    card->set_halign(Gtk::Align::CENTER);
    addTab(std::string(), *card, cardLabel, true);
  }
  for (const crypto_pay::PaymentTabView& tab : tabs_) {
    const Glib::ustring tabName = LabelText(tab.labelKey, tab.label);
    Gtk::Label* label = nullptr;
    Gtk::Box* text = MakeChoiceText(label, tabName, tab.available);
    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    content->set_halign(Gtk::Align::CENTER);
    const bool opensCoins = crypto_pay::TabOpensCoins(tab);
    if (opensCoins) {
      // its coins' marks ahead of its name, each its own with space between
      // them, never merged into one badge (Solana's brand rules)
      auto* marks = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
      marks->set_valign(Gtk::Align::CENTER);
      for (const std::string& iconKey : tab.iconKeys) {
        auto* mark = Gtk::make_managed<CoinMark>(iconKey, kTabMarkPx);
        mark->set_visible(mark->Drawn());
        marks->append(*mark);
      }
      content->append(*marks);
    }
    content->append(*text);
    addTab(tab.id, *content, label, tab.available);
    if (!opensCoins) continue;

    // the marks stand for coins the name does not list: a screen reader hears
    // them
    std::vector<std::string> names;
    for (const crypto_pay::PaymentCoinView& coin : tab.coins) {
      names.push_back(LabelText(coin.labelKey, coin.label).raw());
    }
    kit::SetAccessibleLabel(*tabChoices_.back().button,
                            Format(T_("pay_method_more_coins_a11y", "More coins: {}"),
                                   crypto_pay::JoinNames(names)));

    // the list under the row while the tab is chosen, titled with its name:
    // each coin with its mark, its name over its network, and while it cannot
    // be paid with now its name greyed with "Temporarily unavailable" under it,
    // its mark as it is
    auto* title = Gtk::make_managed<Gtk::Label>(tabName);
    title->add_css_class("dim-label");
    title->add_css_class("caption");
    title->set_xalign(0);
    coins_->append(*title);
    Gtk::CheckButton* coinGroup = nullptr;
    for (const crypto_pay::PaymentCoinView& coin : tab.coins) {
      auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
      auto* mark = Gtk::make_managed<CoinMark>(coin.iconKey, kCoinMarkPx);
      mark->set_visible(mark->Drawn());
      line->append(*mark);
      const Glib::ustring name = LabelText(coin.labelKey, coin.label);
      Gtk::Label* nameLabel = nullptr;
      Gtk::Box* texts = MakeChoiceText(nameLabel, name, coin.available);
      Glib::ustring description;
      const crypto_pay::CoinLine& network = coin.networkLine;
      if (!network.key.empty()) {
        description = LabelText(network.key, network.english);
        auto* networkLabel = Gtk::make_managed<Gtk::Label>(description);
        networkLabel->add_css_class("dim-label");
        networkLabel->add_css_class("caption");
        networkLabel->set_xalign(0);
        texts->insert_child_after(*networkLabel, *nameLabel);
      }
      if (!coin.available) {
        description += Glib::ustring(description.empty() ? "" : ". ") +
                       T_("pay_method_unavailable", "Temporarily unavailable");
      }
      line->append(*texts);
      auto* radio = Gtk::make_managed<Gtk::CheckButton>();
      radio->set_child(*line);
      if (coinGroup) {
        radio->set_group(*coinGroup);
      } else {
        coinGroup = radio;
      }
      kit::SetAccessibleLabel(*radio, name);
      if (!description.empty()) {
        gtk_accessible_update_property(GTK_ACCESSIBLE(radio->gobj()),
                                       GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, description.c_str(), -1);
      }
      const std::string asset = coin.asset;
      radio->signal_toggled().connect([this, radio, asset] {
        if (syncing_ || !radio->get_active()) return;
        Pick(asset);
      });
      coins_->append(*radio);
      coinChoices_.push_back(CoinChoice{coin.asset, radio, nameLabel, coin.available});
    }
  }
  syncing_ = false;
  ApplyChoosable();
  Sync();
  // no coin listed: no row at all (§6.1)
  set_visible(crypto_pay::MethodRowShown(tabs_));
}

void PaymentMethodRow::ApplyChoosable() {
  for (const TabChoice& choice : tabChoices_) {
    SetChoosable(*choice.button, *choice.label, enabled_ && choice.available);
  }
  for (const CoinChoice& choice : coinChoices_) {
    SetChoosable(*choice.button, *choice.label, enabled_ && choice.available);
  }
}

void PaymentMethodRow::Sync() {
  syncing_ = true;
  // the selection's segment: Card's, or the tab its coin sits in (the SDK's
  // tab id), and More coins' list while that is the one; the group turns the
  // others off
  const std::string tabId = PaymentTabIdFor(selected_);
  bool coinsShown = false;
  for (const TabChoice& choice : tabChoices_) {
    const bool active = choice.id.empty() ? tabId.empty() : choice.id == tabId;
    if (active) choice.button->set_active(true);
  }
  for (const crypto_pay::PaymentTabView& tab : tabs_) {
    if (!tabId.empty() && tab.id == tabId && crypto_pay::TabOpensCoins(tab)) coinsShown = true;
  }
  for (const CoinChoice& choice : coinChoices_) choice.button->set_active(choice.asset == selected_);
  syncing_ = false;
  coins_->set_visible(coinsShown);
}

}  // namespace urnw
