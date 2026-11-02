// What the app does with the SDK Bittensor wallet session's answers
// (sdk bittensor_wallet.go, BittensorWalletSession). The SDK decides whether a
// wallet result is a proof; this header decides what each refusal means for
// the flow on screen, and which string explains it. Header-only and free of
// GTK and the SDK (tests/BittensorWalletFlowTest.cpp).
//
// Wallets and transports on linux (the SDK's BittensorWalletTransportFor):
//   - Talisman: browser_bridge. The system browser opens
//     https://ur.io/bittensor-connect?...&wallet=talisman, the page drives the
//     Talisman extension, and the result comes back on the app's existing
//     urnetwork://bittensor-sign-message scheme handler (the .desktop file's
//     x-scheme-handler/urnetwork).
//   - TAO.com (and any other wallet): manual. TAO.com documents no
//     programmatic interface, so the manual sheet shows the challenge and takes
//     the address and a pasted signature.
//   - WalletConnect (Nova, Nightly and other WalletConnect v2 substrate
//     wallets): browser_bridge with wallet=walletconnect. The page pairs (a QR
//     to scan with the wallet app) with the build's WalletConnect project id
//     (Config.hpp, -Dwalletconnect_project_id; empty = the page's own) and
//     returns on the same scheme handler.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace urnw::bittensor {

// the redirect link the bridge returns on (already registered by the .desktop)
inline constexpr std::string_view kRedirectLink = "urnetwork://bittensor-sign-message";
inline constexpr std::string_view kPlatform = "linux";

inline constexpr std::string_view kWalletTalisman = "talisman";
inline constexpr std::string_view kWalletTaoCom = "taocom";
inline constexpr std::string_view kWalletWalletConnect = "walletconnect";

// A row of the wallet chooser, in display order. The label is the SDK's
// product name (never translated); `hint` is the translated subtitle, empty
// when the name says it all.
struct ChooserWallet {
  std::string_view walletId;
  std::string_view hintKey;
  std::string_view hintEnglish;
};

inline constexpr ChooserWallet kChooserWallets[] = {
    {kWalletTalisman, "", ""},
    {kWalletTaoCom, "", ""},
    {kWalletWalletConnect, "bittensor_walletconnect_hint",
     "Nova, Nightly and other WalletConnect wallets"},
};

// The chooser's response id for a wallet is the wallet id itself; anything
// else (cancel, a closed dialog) chooses nothing.
constexpr std::string_view ChosenWallet(std::string_view response) noexcept {
  for (const auto& wallet : kChooserWallets) {
    if (wallet.walletId == response) return wallet.walletId;
  }
  return {};
}

// Only the WalletConnect page is given the build's project id.
constexpr bool SendsWalletConnectProjectId(std::string_view walletId) noexcept {
  return walletId == kWalletWalletConnect;
}

// What to tell the user while the browser bridge is open. `takesWalletName`:
// the text has one {} for the wallet's product name.
struct ContinueText {
  std::string_view key;
  std::string_view english;
  bool takesWalletName;
};

constexpr ContinueText ContinueTextFor(std::string_view walletId) noexcept {
  if (walletId == kWalletWalletConnect) {
    return {"bittensor_walletconnect_continue",
            "Continue in your browser: scan the code with your wallet app, or open the wallet "
            "on this device.",
            false};
  }
  return {"bittensor_continue_in_browser",
          "Continue in your browser and approve the request in the {} extension.", true};
}

inline constexpr std::string_view kPurposeLogin = "login";
inline constexpr std::string_view kPurposeCreate = "create";
inline constexpr std::string_view kPurposeConnect = "connect";
// adding the wallet as a sign-in method (AddSignInFlow.hpp): a login or create
// session refuses its return (purpose_mismatch), so it never signs in
inline constexpr std::string_view kPurposeAdd = "add";
// the network payout consent (NetworkConsent.hpp): the session signs the
// message SnNetworkConsentChallenge issued, not a /auth/wallet-challenge one
inline constexpr std::string_view kPurposeConsent = "consent";

inline constexpr std::string_view kTransportBrowserBridge = "browser_bridge";
inline constexpr std::string_view kTransportManual = "manual";

// The error every flow is answered with when the user closes the manual
// sheet: the flow ends by their choice, so nothing is shown as a failure.
inline constexpr std::string_view kCancelled = "bittensor wallet request cancelled";

constexpr bool IsCancelled(std::string_view error) noexcept { return error == kCancelled; }

// What a session answer means for the flow waiting on it.
enum class Outcome {
  // a proof: continue the flow (sign in, create, connect)
  Accept,
  // not this flow's answer (another purpose, a late or replayed return): drop
  // it and keep waiting
  Ignore,
  // manual entry the user can correct: keep the sheet open and say why
  Retry,
  // the flow is over: answer it with the error
  Fail,
};

constexpr Outcome Classify(std::string_view errorCode, bool manual) noexcept {
  if (errorCode.empty()) return Outcome::Accept;
  // unsupported_wallet on a return: it names another wallet's page (a stale
  // tab from an earlier choice), so it is not this flow's answer either
  if (errorCode == "purpose_mismatch" || errorCode == "not_bittensor_return" ||
      errorCode == "not_awaiting_wallet" || errorCode == "unsupported_wallet") {
    return Outcome::Ignore;
  }
  if (manual && (errorCode == "invalid_signature" || errorCode == "address_mismatch" ||
                 errorCode == "invalid_ss58_address")) {
    return Outcome::Retry;
  }
  return Outcome::Fail;
}

// The store key (and its English, the gettext msgid) explaining a refusal.
// An empty key means another text explains it (FailureTextFor).
struct ErrorText {
  std::string_view key;
  std::string_view english;
};

constexpr ErrorText ErrorTextFor(std::string_view errorCode) noexcept {
  if (errorCode == "invalid_signature") {
    return {"bittensor_error_invalid_signature",
            "That is not a valid signature. Paste the full 0x signature from your wallet."};
  }
  if (errorCode == "challenge_expired") {
    return {"bittensor_error_challenge_expired",
            "This request expired. Start again to get a new message to sign."};
  }
  if (errorCode == "message_mismatch") {
    return {"bittensor_error_message_mismatch",
            "The wallet signed a different request. Start again."};
  }
  if (errorCode == "address_mismatch") {
    return {"earnings_wallet_mismatch", "The wallet that signed is not the address you entered."};
  }
  if (errorCode == "invalid_ss58_address") {
    return {"invalid_ss58_address", "That is not a valid Bittensor address."};
  }
  return {"", ""};
}

// What this app says for the bridge page's own code for a wallet_error (the
// sdk's BittensorWalletResult::BridgeErrorCode, one of
// urnet::BittensorWalletBridgeError*): the store key and its English (the
// msgid), with one {} for the wallet's product name when `takesWalletName`.
// An empty key for a code this app does not know (or none, from a page before
// the codes): the page's own text is shown then.
struct BridgeErrorText {
  std::string_view key;
  std::string_view english;
  bool takesWalletName;
};

constexpr BridgeErrorText BridgeErrorTextFor(std::string_view bridgeCode) noexcept {
  if (bridgeCode == "address_not_in_wallet") {
    return {"bittensor_error_address_not_in_wallet",
            "Your {} wallet doesn't have the address you entered. Add or connect that account in "
            "the wallet, or enter an address it has.",
            true};
  }
  if (bridgeCode == "address_mismatch") {
    return {"earnings_wallet_mismatch", "The wallet that signed is not the address you entered.",
            false};
  }
  if (bridgeCode == "extension_not_found") {
    return {"bittensor_error_extension_not_found",
            "The {} extension was not found in this browser. Install it, then try again.", true};
  }
  if (bridgeCode == "no_account") {
    return {"bittensor_error_no_account",
            "Your {} wallet has no account to sign with. Add or connect an account in the wallet, "
            "then try again.",
            true};
  }
  if (bridgeCode == "extension_not_authorized") {
    return {"bittensor_error_extension_not_authorized",
            "{} hasn't allowed ur.io to use your Bittensor account yet. Approve the connection "
            "request in the wallet, or allow ur.io under Connected Sites in its settings, then "
            "try again.",
            true};
  }
  if (bridgeCode == "user_rejected") {
    return {"bittensor_error_user_rejected",
            "The request was declined in your wallet. Start again and approve it to continue.",
            false};
  }
  if (bridgeCode == "walletconnect_expired") {
    return {"bittensor_error_walletconnect_expired",
            "The WalletConnect request expired before the wallet answered. Start again.", false};
  }
  if (bridgeCode == "walletconnect_unavailable") {
    return {"bittensor_error_walletconnect_unavailable",
            "WalletConnect is not available right now. Try again later, or enter your address "
            "manually.",
            false};
  }
  return {"", "", false};
}

// Which text a failed answer shows: its refusal's string (ErrorTextFor), the
// bridge page's code in this app's words (BridgeErrorTextFor), the page's own
// words (a wallet_error with a code this app does not know, or none), or the
// generic failure. Only a wallet_error's message is words for the user;
// another refusal's detail is not (the refused address, the transport).
enum class FailureText { Refusal, BridgeCode, PageText, Generic };

constexpr FailureText FailureTextFor(std::string_view errorCode, std::string_view bridgeCode,
                                     bool hasErrorMessage) noexcept {
  if (!ErrorTextFor(errorCode).key.empty()) return FailureText::Refusal;
  if (errorCode != "wallet_error") return FailureText::Generic;
  if (!BridgeErrorTextFor(bridgeCode).key.empty()) return FailureText::BridgeCode;
  return hasErrorMessage ? FailureText::PageText : FailureText::Generic;
}

// A consent session's expired challenge reaches its flow as the SDK's own
// "challenge_expired: ..." error, the form its consent calls fail in
// (CONSENT.md A1), so the consent section words it itself
// (sn_consent_expired). Every other refusal reads as for any flow.
constexpr bool ReportsCode(std::string_view purpose, std::string_view errorCode) noexcept {
  return purpose == kPurposeConsent && errorCode == "challenge_expired";
}

// A consent never shows a return's own words (CONSENT.md A3): any opener of the
// app's link can put text in a hand-back, so a page failure without a code this
// app words reads as the generic failure. Every other flow shows them as before.
constexpr bool ShowsPageText(std::string_view purpose) noexcept {
  return purpose != kPurposeConsent;
}

// the session state of a bridge transport waiting for its hand-back (the SDK's
// BittensorWalletStateAwaitingWallet)
inline constexpr std::string_view kStateAwaitingWallet = "awaiting_wallet";

// A bridge return a consent session did not take, and that left it waiting (a
// forged hand-back, or one for another message, CONSENT.md A3), is not the
// consent's answer: the consent keeps waiting for its own.
constexpr bool KeepsWaiting(std::string_view purpose, std::string_view sessionState,
                            bool proof) noexcept {
  return purpose == kPurposeConsent && !proof && sessionState == kStateAwaitingWallet;
}

// How long a flow waits for its answer. A bridge round trip reports nothing
// when the tab is closed, so it times out, but not before the challenge does (5
// minutes, plus the time to submit). Every bridge page takes a signature by
// hand ("Enter address manually": the user signs outside the browser, with
// btcli or a wallet's own tool, and pastes it), and WalletConnect pairing (scan,
// approve the session, approve the signature) takes a while too. A return after
// the watchdog fired is dropped for good (EarningsPage::BeginFlow), so a shorter
// wait throws away a proof the server would still take.
inline constexpr uint32_t kBridgeTimeoutMs = 330'000;
inline constexpr uint32_t kManualTimeoutMs = 330'000;

constexpr uint32_t TimeoutMsFor(std::string_view transport,
                                std::string_view walletId = {}) noexcept {
  if (transport == kTransportManual || walletId == kWalletWalletConnect) return kManualTimeoutMs;
  return kBridgeTimeoutMs;
}

// POST /sn/wallet refuses a well-formed signature that does not verify for the
// entered address with this SnError code (sdk SnErrorCodeSignatureMismatch): the
// wallet signed with another account (or other text), and the server cannot
// say which.
inline constexpr std::string_view kSnErrorSignatureMismatch = "signature_mismatch";

// The words for a refused coldkey connect when the page has its own, with one
// {} for the wallet's product name. Only a manual entry pastes a signature, so
// only then does the page say to sign again in that wallet; empty otherwise
// (the SDK's error text, as before). A browser-bridge wallet signed with the
// account it returned.
constexpr ErrorText ConnectErrorTextFor(std::string_view snErrorCode,
                                        std::string_view transport) noexcept {
  if (snErrorCode == kSnErrorSignatureMismatch && transport == kTransportManual) {
    return {"bittensor_error_signature_mismatch",
            "This signature isn't from the address you entered. In {}, sign the message with "
            "that address, then paste the signature again."};
  }
  return {"", ""};
}

}  // namespace urnw::bittensor
