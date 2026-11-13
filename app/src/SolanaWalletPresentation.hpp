// A failed Solana wallet step (Phantom or Solflare) in this app's words.
// Signing in with the wallet, adding it as a sign-in method and creating a
// network with it open the ur.io wallet bridge page, and WalletConnect.cpp
// hands every failure of that hop to the sdk (urnet::solanaWalletFailureFor,
// sdk solana_wallet_failure.go): the code the page comes back with, or a
// failure the app found itself (a browser that would not open the page, a
// return it cannot read). The sdk names the message every app shows for the
// failure, and the wallet name when the message takes one; this app only
// shows it, through FailureTextFor and FailureMessageFor (owner, 2026-11-13:
// the same specific message on every app). The page's own English is logged,
// never shown.
//
// The Solana payout wallet this header also decided for (the Earnings card,
// its connect sheet and the USDC waiting for it) was removed on 2026-11-13:
// USDC provider payouts ended on 2026-11-12.
//
// Header-only and free of GTK and the SDK so the unit tests need no vendored
// headers (tests/SolanaBridgeErrorTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace urnw::solana {

// One of the sdk's messages for a failure (urnet::SolanaWalletFailure's
// message_key, one of urnet::SolanaWalletFailureMessage*) as this app says it:
// the store key and its English (the gettext msgid), with one {} for the
// wallet's product name when `takesWalletName`.
struct FailureText {
  const char* key;
  const char* english;
  bool takesWalletName;
};

// The wallet could not finish the request: the sdk's message for any failure
// it has no more specific one for, and what a key this build does not know
// reads as.
inline constexpr FailureText kWalletFailedText = {
    "bittensor_error_wallet_failed",
    "Your {} wallet couldn't finish the request. Check the wallet, then try again.", true};

// Every message the sdk names, in the sdk's order.
inline constexpr FailureText kFailureTexts[] = {
    {"bittensor_error_user_rejected",
     "The request was declined in your wallet. Start again and approve it to continue.", false},
    {"no_wallets_found_alert_content",
     "No Solana wallets were found installed on this device. Please install a wallet and try "
     "again.",
     false},
    {"couldnt_open_wallet", "Couldn't open wallet. Please install it and try again.", false},
    {"bittensor_error_extension_not_found",
     "The {} extension was not found in this browser. Install it, then try again.", true},
    {"bittensor_error_no_account",
     "Your {} wallet has no account to sign with. Add or connect an account in the wallet, then "
     "try again.",
     true},
    {"solana_wallet_error_session_not_found",
     "The wallet connection wasn't found in this browser. Start again to reconnect your wallet.",
     false},
    kWalletFailedText,
};

// The text for `messageKey`; any other key, "" included, reads as the wallet
// failing.
constexpr FailureText FailureTextFor(std::string_view messageKey) noexcept {
  for (const FailureText& text : kFailureTexts) {
    if (messageKey == text.key) return text;
  }
  return kWalletFailedText;
}

// The wallet a message names when neither the sdk nor the app knows which
// wallet answered, as the sdk names it.
inline constexpr const char* kUnknownWalletName = "Solana";

// What the user reads for the sdk's answer: the text for its message key, and
// the wallet name the text's {} takes ("" when it takes none).
struct FailureMessage {
  FailureText text;
  std::string walletName;
};

// `messageKey` and `sdkWalletName` are the sdk's answer, both "" when the sdk
// gave none (it reads as the wallet failing). A text that takes a wallet name
// names the sdk's, else `chosenWalletName` (the product name of the wallet the
// user chose), else Solana.
inline FailureMessage FailureMessageFor(std::string_view messageKey,
                                        std::string_view sdkWalletName,
                                        std::string_view chosenWalletName) {
  const FailureText text = FailureTextFor(messageKey);
  if (!text.takesWalletName) return {text, std::string()};
  if (!sdkWalletName.empty()) return {text, std::string(sdkWalletName)};
  if (!chosenWalletName.empty()) return {text, std::string(chosenWalletName)};
  return {text, kUnknownWalletName};
}

}  // namespace urnw::solana
