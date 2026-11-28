// The words of the account's emailed-code flows, by the code of the server's
// refusal: the email change (sdk api_email_change.go, the Account page's email
// change sheet) and the payout wallet disconnect (sdk
// api_sn_wallet_disconnect.go, the Earnings page's disconnect sheet). The codes
// are the sdk's AuthEmailChangeErrorCode* and SnWalletDisconnectErrorCode*,
// spelled as the server sends them. A code without a line here (a newer
// server's) reads as the server's message, and a request that got no answer as
// the sheet's generic line.
//
// Dependency-free so tests/AccountCodeTextTest.cpp runs it without GTK or the
// SDK.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string_view>

namespace urnw::account_code {

// A line a sheet shows: a store key and its English (the gettext msgid).
struct Text {
  const char* key;
  const char* english;
};

// The lines the account's code flows share.
inline constexpr Text kTooManyTries{"account_too_many_tries",
                                    "Too many tries. Please try again later."};
inline constexpr Text kCodeSendFailed{"account_code_send_failed",
                                      "The code could not be sent. Please try again."};
inline constexpr Text kCodeInvalid{"account_code_invalid",
                                   "That code is wrong. Check it and try again."};

// The email change without the network credential: the sdk refuses it before
// sending it (ErrNetworkCredentialRequired), and the server answers owner_only
// for any other credential. Signing in again brings one.
inline constexpr Text kEmailChangeSignInRequired{"email_change_sign_in_required",
                                                 "Sign in again to change your email."};

// An email change refusal (its AuthEmailChangeError code, or the local check's
// urnet::validateEmailChangeAddress answer) in this app's words; nullopt for a
// code without a line here.
inline std::optional<Text> EmailChangeRefusalText(std::string_view code) {
  if (code == "owner_only") return kEmailChangeSignInRequired;
  if (code == "no_email") {
    return Text{"email_change_no_email",
                "Your account has no email sign-in. Add one as a sign-in method instead."};
  }
  if (code == "email_unverified") {
    return Text{"email_change_email_unverified",
                "Verify your current email before you change it."};
  }
  if (code == "invalid_email") {
    return Text{"email_change_invalid_email", "Enter a valid email address."};
  }
  if (code == "same_email") return Text{"email_change_same_email", "That's already your email."};
  if (code == "email_in_use") {
    return Text{"email_change_email_in_use", "Another account already uses that email."};
  }
  if (code == "rate_limited") return kTooManyTries;
  if (code == "send_failed") return kCodeSendFailed;
  if (code == "no_change") {
    return Text{"email_change_expired",
                "This email change ended before it was confirmed. Start again."};
  }
  if (code == "code_invalid") return kCodeInvalid;
  if (code == "too_many_tries") {
    return Text{"email_change_too_many_tries", "Too many wrong codes. Start again."};
  }
  return std::nullopt;
}

// A payout wallet disconnect refusal (its SnWalletDisconnectError code) in
// this app's words; nullopt for a code without a line here: owner_only and a
// newer server's code read as the server's message, and no_wallet is no
// refusal to word (the card reads the wallet again).
inline std::optional<Text> WalletDisconnectRefusalText(std::string_view code) {
  if (code == "consent_in_force") {
    return Text{"wallet_disconnect_consent_in_force",
                "Your signed payout consent sends your earnings to this wallet, and only another "
                "signed consent can replace it. Use Change wallet instead."};
  }
  if (code == "email_required") {
    return Text{"wallet_disconnect_email_required",
                "Add an email sign-in to your account first. The confirmation code is sent to "
                "it."};
  }
  if (code == "rate_limited") return kTooManyTries;
  if (code == "send_failed") return kCodeSendFailed;
  if (code == "code_invalid") return kCodeInvalid;
  if (code == "code_expired") {
    return Text{"account_code_expired",
                "That code expired or was already used. Ask for a new one."};
  }
  return std::nullopt;
}

}  // namespace urnw::account_code
