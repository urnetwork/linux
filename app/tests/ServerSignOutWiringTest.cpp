// Sign-out cleared only local state and told the daemon: the device's network
// client stayed active and its session valid, because nothing called the sdk's
// sign-out. A sign-in whose client the server created but this app did not
// publish (signed out while it registered, or its client token could not be
// stored) left that client behind too. Each counted toward the network's cap
// of active top-level clients until the 30-day idle reap, which the
// end-of-week client limits enforce. SdkHost needs the SDK, so this reads its
// source.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>

#include "WiringSource.hpp"

using urnw::testing::wiring::Before;
using urnw::testing::wiring::Contains;
using urnw::testing::wiring::CountOf;
using urnw::testing::wiring::FunctionBody;
using urnw::testing::wiring::ReadCode;

// The server half runs on the worker before the local credentials go, while
// api_ still holds the network credential it needs, and its wait is bounded.
UR_TEST(ServerSignOut_RunsBeforeTheLocalCredentialsClear) {
  const std::string host = ReadCode("SdkHost.cpp");
  const std::string logout = FunctionBody(host, "void SdkHost::LogoutNow()");
  UR_EXPECT_TRUE_MSG("SdkHost::LogoutNow is defined", !logout.empty());
  UR_EXPECT_TRUE(Before(logout, "ServerSignOutLocked();", "if (localState_) localState_->logout();"));
  UR_EXPECT_TRUE(Before(logout, "ServerSignOutLocked();", "if (api_) api_->setByJwt(\"\");"));
  const std::string server = FunctionBody(host, "void SdkHost::ServerSignOutLocked()");
  UR_EXPECT_TRUE_MSG("SdkHost::ServerSignOutLocked is defined", !server.empty());
  // the clients the main loop recorded go first, with the same credential
  UR_EXPECT_TRUE(Before(server, "abandoned.swap(abandonedClientIds_);", "api_->signOut("));
  UR_EXPECT_TRUE(Contains(server, "answer.wait_for(kServerSignOutWait)"));
}

// A client the server created for a sign-in that does not publish is removed:
// on the worker at once, or by the next sign-out tail when the main loop,
// which takes no mutex_, heard it.
UR_TEST(ServerSignOut_AnAbandonedSignInRemovesItsClient) {
  const std::string host = ReadCode("SdkHost.cpp");
  const std::string finish = FunctionBody(host, "void SdkHost::FinishNetworkClientRegistration(");
  UR_EXPECT_EQ(3u, CountOf(finish, "RemoveAbandonedClientLocked(clientId);"));
  // the sdk call stays outside authStateMutex_, which takes none
  UR_EXPECT_TRUE(Before(finish, "std::scoped_lock authLock(authStateMutex_);",
                        "if (!published) RemoveAbandonedClientLocked(clientId);"));
  const std::string reg = FunctionBody(host, "void SdkHost::RegisterNetworkClient(");
  UR_EXPECT_TRUE(Before(reg, "if (result) NoteAbandonedClient(result->client_id);", "NoteApiError(err);"));
  UR_EXPECT_TRUE(Contains(reg, "FinishNetworkClientRegistration(*result->by_client_jwt, result->client_id, done,"));
}
