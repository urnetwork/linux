// SPDX-License-Identifier: MPL-2.0
#include "BittensorWalletChooser.hpp"

#include <string>

#include <adwaita.h>
#include <urnetwork_sdk.hpp>

#include "BittensorWalletFlow.hpp"
#include "I18n.hpp"

namespace urnw {

GtkWidget* NewBittensorWalletChooser(GtkWindow* parent) {
  GtkWidget* dialog = adw_message_dialog_new(
      parent, T_("bittensor_choose_wallet", "Choose your Bittensor wallet"), nullptr);
  adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "cancel", T_("cancel", "Cancel"));
  std::string body;
  for (const auto& wallet : bittensor::kChooserWallets) {
    const std::string walletId(wallet.walletId);
    // wallet names are product names: never translated (the SDK names them)
    const std::string name = urnet::bittensorWalletDisplayName(walletId);
    adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), walletId.c_str(), name.c_str());
    if (!wallet.hintKey.empty()) {
      const std::string hintKey(wallet.hintKey);
      const std::string hintEnglish(wallet.hintEnglish);
      if (!body.empty()) body += "\n";
      body += name + ": " + g_dpgettext2(GETTEXT_PACKAGE, hintKey.c_str(), hintEnglish.c_str());
    }
  }
  if (!body.empty()) adw_message_dialog_set_body(ADW_MESSAGE_DIALOG(dialog), body.c_str());
  const std::string first(bittensor::kChooserWallets[0].walletId);
  adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog), first.c_str(),
                                             ADW_RESPONSE_SUGGESTED);
  adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), first.c_str());
  adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(dialog), "cancel");
  return dialog;
}

}  // namespace urnw
