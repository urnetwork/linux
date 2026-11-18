// The account's cap of active sessions, as the sign-in screens show it
// (session1118 C.md §1, §5 A). Every route that starts a session (a sign-in of
// any kind, a sign-in with a code, a network's creation, and the device client
// created right after a sign-in) answers a refusal at the cap as its result,
// with the sdk's code (SessionErrorCodeLimitReached), not as an error. This
// device is not signed in then, so the screen sends the user to a device that
// is, where Account > Sessions signs out the sessions no longer used. Nothing
// here tries again or signs anything out.
//
// SdkHost reads the code into AuthResult::sessionLimit (and LoginRouting's and
// InstantAccount's), and every sign-in screen shows SignInRefusalBody where it
// shows its errors, in place of the server's words, as it does for the client
// limit (ClientLimitNotice.hpp).
//
// Pure and dependency-free: tests/SessionLimitNoticeTest.cpp runs it.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

#include "ClientLimitNotice.hpp"

namespace urnw::session_limit {

// The sdk's SessionErrorCodeLimitReached; SdkHost.cpp asserts the two equal.
// The retention cap's code is another on purpose: signing sessions out does
// not free what it counts.
inline constexpr const char* kRefusalCode = "session_limit_reached";

// Whether a sign-in route's error is the cap's refusal.
inline bool Refused(const std::optional<std::string>& code) {
  return code.has_value() && *code == kRefusalCode;
}

// What a sign-in screen says for it. 'Account' and 'Sessions' are the apps'
// own names for those screens.
inline constexpr client_limit::Text kSignInBody{
    "sessions_limit_reached_sign_in",
    "Too many active sessions. On a device that's signed in to this account, go to Account > "
    "Sessions and sign out sessions you no longer use, then try again."};

// What a refused sign-in says in place of the server's words: the session
// cap's line, else the client limit's for its prompt
// (client_limit::SignInRefusalBody); null for any other refusal, which keeps
// the server's words or the screen's own line. The sdk never sends both: its
// refusal at the session cap carries no client limit prompt.
inline constexpr const client_limit::Text* SignInRefusalBody(bool sessionLimit,
                                                            client_limit::Prompt prompt) {
  return sessionLimit ? &kSignInBody : client_limit::SignInRefusalBody(prompt);
}

}  // namespace urnw::session_limit
