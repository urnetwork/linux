// Build-time app configuration. Values here are placeholders that a build
// machine (or a developer) fills in — nothing in this file is a secret, and
// every value degrades gracefully when left empty.
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw {

// WalletConnect Cloud project id, passed to the ur.io/wallet-connect bridge as
// `wc_project_id` (see WalletConnect::SignInWithBittensor). One project id is
// shared by all URnetwork clients — the same value that goes in
// android/app/local.properties (WALLETCONNECT_PROJECT_ID) and
// apple/app/URnetwork-Info.plist (URWalletConnectProjectId). See
// apple/NEXTSTEPS2.md §1.
//
// FILL THIS IN with the project id from the WalletConnect / Reown Cloud
// dashboard, or set it at build time without editing this file:
//     meson setup build -Dwalletconnect_project_id=<project id>
//
// EMPTY IS VALID: the bridge then uses injected wallets only (a Bittensor
// Wallet / SubWallet / Talisman / polkadot-js browser extension) — which is the
// common desktop case. The project id only buys pairing with a wallet app over
// a QR code. It is never sent when empty, and nothing crashes.
#ifndef UR_WALLETCONNECT_PROJECT_ID
#define UR_WALLETCONNECT_PROJECT_ID ""
#endif
inline constexpr const char* kWalletConnectProjectId = UR_WALLETCONNECT_PROJECT_ID;

}  // namespace urnw
