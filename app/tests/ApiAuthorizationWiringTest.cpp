// The GUI api's credential across a device teardown (SdkHost.cpp). Binding a
// DeviceRemote replaces the api's user jwt with the device's client jwt, and
// closing it (the SDK's Api.closeDeviceOwner) clears that and leaves none, so
// after a teardown that leaves the person signed in, the referral code, the preferences and the subscription
// balance went out unauthorized: a tester whose daemon restarted saw
// "Something went wrong." on Referrals and on product updates, and Data usage
// stuck on "Loading...". SdkHost needs the SDK, so this reads its source.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>

#include "WiringSource.hpp"

using namespace urnw::testing::wiring;

namespace {

// `text` with every run of whitespace collapsed to one space.
std::string Squeezed(const std::string& text) {
  std::string out;
  for (const char c : text) {
    const bool space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
    if (!space) {
      out += c;
    } else if (!out.empty() && out.back() != ' ') {
      out += ' ';
    }
  }
  return out;
}

}  // namespace

UR_TEST(ApiAuthorization_TheStoredUserJwtGoesBack) {
  const std::string host = ReadCode("SdkHost.cpp");
  const std::string restore = FunctionBody(host, "void SdkHost::RestoreApiAuthorizationLocked()");
  UR_EXPECT_TRUE(!restore.empty());
  UR_EXPECT_TRUE(Before(restore, "localState_->getByJwt()", "api_->setByJwt(byJwt)"));
  UR_EXPECT_TRUE_MSG("an empty stored jwt is never installed",
                     Contains(restore, "!byJwt.empty()"));
}

// Every teardown that leaves the person signed in puts it back, right after.
UR_TEST(ApiAuthorization_EveryStaleOrFailedDeviceTeardownRestoresIt) {
  const std::string host = ReadCode("SdkHost.cpp");
  for (const char* signature : {"TunnelStartResult SdkHost::StartTunnelLocked(",
                                "TunnelStartResult SdkHost::BindRemoteDeviceLocked(",
                                "void SdkHost::ConnectBestAvailableNow()",
                                "void SdkHost::ReconcileProviderLocked(",
                                "void SdkHost::OnRpcBindDeadline()"}) {
    const std::string body = Squeezed(FunctionBody(host, signature));
    UR_EXPECT_TRUE_MSG(std::string(signature) + " tears the device down",
                       Contains(body, "TeardownDeviceLocked();"));
    UR_EXPECT_TRUE_MSG(
        std::string(signature) + " restores the api's credential right after it",
        Contains(body, "TeardownDeviceLocked(); RestoreApiAuthorizationLocked();"));
  }
}

// Sign-out and quit do not: the api must leave them without a credential, and
// a network change or a fresh sign-in installs its own.
UR_TEST(ApiAuthorization_SignOutQuitAndNetworkChangeDoNotRestoreIt) {
  const std::string host = ReadCode("SdkHost.cpp");
  for (const char* signature : {"void SdkHost::LogoutNow()", "void SdkHost::Shutdown()",
                                "bool SdkHost::ApplyNetworkServer(",
                                "void SdkHost::RegisterNetworkClient("}) {
    UR_EXPECT_TRUE_MSG(
        std::string(signature) + " does not restore the credential",
        !Contains(FunctionBody(host, signature), "RestoreApiAuthorizationLocked();"));
  }
  UR_EXPECT_EQ(std::size_t{5}, CountOf(host, "RestoreApiAuthorizationLocked();"));
}
