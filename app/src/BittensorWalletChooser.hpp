// The Bittensor wallet chooser (an AdwMessageDialog): one response per
// bittensor::kChooserWallets row, its id the wallet id, its label the SDK's
// product name; the body lists each wallet's hint (WalletConnect: which wallets
// it works with). Map a response with bittensor::ChosenWallet. The caller
// connects "response" and presents it.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <gtk/gtk.h>

namespace urnw {

GtkWidget* NewBittensorWalletChooser(GtkWindow* parent);

}  // namespace urnw
