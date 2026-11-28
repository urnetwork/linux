// The words of the account's emailed-code flows (AccountCodeText.hpp): every
// email change and payout wallet disconnect refusal code reads as its own store
// key, the local address check's answer reads as the server's invalid_email
// does, and a code without a line here reads as the server's message
// (nullopt).
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <optional>
#include <string>
#include <utility>

#include "AccountCodeText.hpp"

namespace ac = urnw::account_code;

namespace {

// The key a code reads as, "" for none.
std::string EmailChangeKey(const char* code) {
  const std::optional<ac::Text> text = ac::EmailChangeRefusalText(code);
  return text ? std::string(text->key) : std::string();
}

std::string WalletDisconnectKey(const char* code) {
  const std::optional<ac::Text> text = ac::WalletDisconnectRefusalText(code);
  return text ? std::string(text->key) : std::string();
}

}  // namespace

// sdk AuthEmailChangeErrorCode*, each in the words the brief names
UR_TEST(AccountCodeText_EveryEmailChangeRefusalHasItsLine) {
  const std::pair<const char*, const char*> cases[] = {
      {"owner_only", "email_change_sign_in_required"},
      {"no_email", "email_change_no_email"},
      {"email_unverified", "email_change_email_unverified"},
      {"invalid_email", "email_change_invalid_email"},
      {"same_email", "email_change_same_email"},
      {"email_in_use", "email_change_email_in_use"},
      {"rate_limited", "account_too_many_tries"},
      {"send_failed", "account_code_send_failed"},
      {"no_change", "email_change_expired"},
      {"code_invalid", "account_code_invalid"},
      {"too_many_tries", "email_change_too_many_tries"},
  };
  for (const auto& [code, key] : cases) {
    UR_EXPECT_TRUE_MSG(std::string(code) + " reads as " + EmailChangeKey(code),
                       EmailChangeKey(code) == key);
  }
}

// a newer server's code, and none, read as the server's message
UR_TEST(AccountCodeText_AnUnknownEmailChangeCodeHasNoLine) {
  for (const char* code : {"", "something_new", "Invalid_Email", "code_expired"}) {
    UR_EXPECT_TRUE_MSG(code, !ac::EmailChangeRefusalText(code).has_value());
  }
}

// the sdk's refusal before sending (no network credential) and the server's
// owner_only are the same line
UR_TEST(AccountCodeText_TheSignInRequiredLineIsOwnerOnlys) {
  const std::optional<ac::Text> ownerOnly = ac::EmailChangeRefusalText("owner_only");
  UR_EXPECT_TRUE(ownerOnly.has_value());
  if (!ownerOnly) return;
  UR_EXPECT_TRUE(std::string(ownerOnly->key) == ac::kEmailChangeSignInRequired.key);
  UR_EXPECT_TRUE(std::string(ownerOnly->english) == "Sign in again to change your email.");
}

// sdk SnWalletDisconnectErrorCode*, each in the words the brief names
UR_TEST(AccountCodeText_EveryWalletDisconnectRefusalHasItsLine) {
  const std::pair<const char*, const char*> cases[] = {
      {"consent_in_force", "wallet_disconnect_consent_in_force"},
      {"email_required", "wallet_disconnect_email_required"},
      {"rate_limited", "account_too_many_tries"},
      {"send_failed", "account_code_send_failed"},
      {"code_invalid", "account_code_invalid"},
      {"code_expired", "account_code_expired"},
  };
  for (const auto& [code, key] : cases) {
    UR_EXPECT_TRUE_MSG(std::string(code) + " reads as " + WalletDisconnectKey(code),
                       WalletDisconnectKey(code) == key);
  }
}

// owner_only and a newer server's code read as the server's message, and
// no_wallet is no refusal to word: the card reads the wallet again
UR_TEST(AccountCodeText_TheServersWordsForTheOtherDisconnectCodes) {
  for (const char* code : {"owner_only", "no_wallet", "", "something_new", "no_change"}) {
    UR_EXPECT_TRUE_MSG(code, !ac::WalletDisconnectRefusalText(code).has_value());
  }
}

// both flows share the lines for a wrong code, too many tries and a failed send
UR_TEST(AccountCodeText_BothFlowsShareTheCodeLines) {
  for (const char* code : {"rate_limited", "send_failed", "code_invalid"}) {
    UR_EXPECT_TRUE_MSG(code, EmailChangeKey(code) == WalletDisconnectKey(code));
  }
}
