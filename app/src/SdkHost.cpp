// SPDX-License-Identifier: MPL-2.0
#include "SdkHost.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "NetworkSpaceConfig.hpp"

// The release version, threaded in via the -Dapp_version meson option (the
// pipeline passes $VERSION); the fallback matches the option's default.
#ifndef UR_APP_VERSION
#define UR_APP_VERSION "0.0.0"
#endif

namespace urnw {
namespace {
// Device identity strings (hostname device description, "linux amd64/arm64"
// spec) and the network-space values are shared with urnetworkd through
// NetworkSpaceConfig.hpp — the two binaries must agree on them.
constexpr const char* kAppVersion = UR_APP_VERSION;

// The GUI's memory bound. The data plane's budget now lives in urnetworkd
// (TunnelHost); this only scales the GUI-side SDK (api + DeviceRemote).
constexpr int64_t kMemoryLimit = 64ll * 1024 * 1024;
// The challenge every wallet signs for wallet sign-in — the same static string on
// every client (apple/NEXTSTEPS2.md §4); no client sends a nonce.
constexpr const char* kWalletSignInMessage = "Welcome to URnetwork";
// AuthLogin{wallet_auth} blockchain ids. The server matches case-insensitively:
// "solana" -> ed25519, urnet::TAO ("TAO") -> sr25519 (bittensor).
constexpr const char* kSolanaBlockchain = "solana";
}  // namespace

bool SdkHost::Initialize(const std::string& storageDir, const std::string& logDir) {
  std::scoped_lock lock(mutex_);
  try {
    urnet::setLogDir(logDir);
    urnet::setMemoryLimit(kMemoryLimit);
    spaceManager_ = urnet::newNetworkSpaceManager(storageDir);
    networkSpace_ = BuildUrNetworkSpace(*spaceManager_);
    api_ = networkSpace_->getApi();
    asyncLocalState_ = networkSpace_->getAsyncLocalState();
    localState_ = asyncLocalState_->getLocalState();
    // sign-up name availability rides the SDK's shared view controller (the
    // apple CreateNetworkViewModel binds the same one)
    networkNameVc_ = urnet::newNetworkNameValidationViewController(*api_);
    // our SDK build, exact-match checked against the daemon's at hello: the
    // gob device rpc has no version negotiation of its own
    control_.SetLocalSdkVersion(urnet::version());
    SetupWalletCallbacks();
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] initialize failed: %s\n", e.what());
    return false;
  }
}

bool SdkHost::IsLoggedIn() {
  std::scoped_lock lock(mutex_);
  return localState_ && !localState_->getByClientJwt().empty();
}

// ---- auth (mirrors the Windows SdkHost) -----------------------------------

// Account discovery for the email-first login flow (Windows SdkHost::StartLogin,
// macOS LoginInitialViewModel + UrApiService.authLogin): authLogin{user_auth}
// answers with the sign-in methods the auth is registered under. password ->
// the password step; another method only (e.g. SSO) -> IncorrectAuth with the
// allowed list; nothing -> Create (sign-up).
void SdkHost::StartLogin(const std::string& userAuth, std::function<void(LoginRouting)> done) {
  urnet::AuthLoginArgs args;
  args.user_auth = userAuth;
  api_->authLogin(args, [this, userAuth, done](std::optional<urnet::AuthLoginResult> result,
                                               std::optional<std::string> err) {
    LoginRouting routing;  // route defaults to Error
    routing.userAuth = userAuth;
    if (err) { routing.error = *err; done(routing); return; }
    if (!result) { routing.error = "no result"; done(routing); return; }
    if (result->user_auth && !result->user_auth->empty()) {
      routing.userAuth = *result->user_auth;  // the normalized echo
    }
    if (result->error && !result->error->message.empty()) {
      routing.error = result->error->message;
      done(routing);
      return;
    }
    // a jwt straight from discovery (not the user-auth path, but handle it)
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, [done](AuthResult r) {
        LoginRouting routed;
        routed.route = r.ok ? LoginRoute::Login : LoginRoute::Error;
        routed.error = r.error;
        done(routed);
      });
      return;
    }
    if (result->auth_allowed && !result->auth_allowed->empty()) {
      const auto& allowed = *result->auth_allowed;
      if (std::find(allowed.begin(), allowed.end(), "password") != allowed.end()) {
        routing.route = LoginRoute::Password;
      } else {
        // the account exists under another sign-in method (e.g. a wallet)
        routing.route = LoginRoute::IncorrectAuth;
        for (const auto& method : allowed) {
          if (!routing.authAllowed.empty()) routing.authAllowed += ", ";
          routing.authAllowed += method;
        }
      }
      done(routing);
      return;
    }
    // unknown user auth: create a new network
    routing.route = LoginRoute::Create;
    done(routing);
  });
}

void SdkHost::LoginWithPassword(const std::string& userAuth, const std::string& password,
                                std::function<void(AuthResult)> done) {
  urnet::AuthLoginWithPasswordArgs args;
  args.user_auth = userAuth;
  args.password = password;
  // an unverified account gets a NUMERIC code (the verify page's OTP entry),
  // matching the apple LoginPasswordViewModel
  args.verify_otp_numeric = true;
  api_->authLoginWithPassword(args, [this, done](std::optional<urnet::AuthLoginWithPasswordResult> result,
                                                 std::optional<std::string> err) {
    if (err) { done({false, false, *err}); return; }
    if (!result) { done({false, false, "no result"}); return; }
    if (result->error && !result->error->message.empty()) { done({false, false, result->error->message}); return; }
    if (result->verification_required) { done({false, true, ""}); return; }
    if (result->network && result->network->by_jwt) {
      RegisterNetworkClient(*result->network->by_jwt, done);
      return;
    }
    done({false, false, "login returned no network"});
  });
}

void SdkHost::LoginWithCode(const std::string& authCode, std::function<void(AuthResult)> done) {
  urnet::AuthCodeLoginArgs args;
  args.auth_code = authCode;
  api_->authCodeLogin(args, [this, done](std::optional<urnet::AuthCodeLoginResult> result,
                                         std::optional<std::string> err) {
    if (err) { done({false, false, *err}); return; }
    if (!result) { done({false, false, "no result"}); return; }
    if (result->error && !result->error->message.empty()) { done({false, false, result->error->message}); return; }
    if (!result->by_jwt.empty()) { RegisterNetworkClient(result->by_jwt, done); return; }
    done({false, false, "code login returned no jwt"});
  });
}

void SdkHost::LoginAsGuest(std::function<void(AuthResult)> done) {
  urnet::NetworkCreateArgs args;
  args.terms = true;
  args.guest_mode = true;
  api_->networkCreate(args, [this, done](std::optional<urnet::NetworkCreateResult> result,
                                         std::optional<std::string> err) {
    if (err) { done({false, false, *err}); return; }
    if (!result) { done({false, false, "no result"}); return; }
    if (result->error && !result->error->message.empty()) { done({false, false, result->error->message}); return; }
    if (result->network && result->network->by_jwt) {
      RegisterNetworkClient(*result->network->by_jwt, done);
      return;
    }
    done({false, false, "guest create returned no network"});
  });
}

// ---- sign-up / verify / password reset (Phase 3) ----------------------------

// NetworkCreateResult routing shared by the password and wallet sign-ups:
// by_jwt -> RegisterNetworkClient; verification_required -> the verify page.
void SdkHost::HandleNetworkCreateResult(std::optional<urnet::NetworkCreateResult> result,
                                        std::optional<std::string> err,
                                        std::function<void(AuthResult)> done) {
  if (err) { done({false, false, *err}); return; }
  if (!result) { done({false, false, "no result"}); return; }
  if (result->error && !result->error->message.empty()) {
    done({false, false, result->error->message});
    return;
  }
  if (result->verification_required) { done({false, true, ""}); return; }
  if (result->network && result->network->by_jwt) {
    {
      std::scoped_lock lock(mutex_);
      pendingWalletAuth_.reset();  // consumed (only set on the wallet path)
    }
    RegisterNetworkClient(*result->network->by_jwt, done);
    return;
  }
  done({false, false, "network create returned no network"});
}

void SdkHost::CreateNetwork(const std::string& networkName, const std::string& userAuth,
                            const std::string& password, const std::string& referralCode,
                            std::function<void(AuthResult)> done) {
  urnet::NetworkCreateArgs args;
  args.user_name = std::string();  // mac parity: always empty
  args.user_auth = userAuth;
  args.password = password;
  args.network_name = networkName;
  args.terms = true;  // the page's continue button is gated on the terms switch
  args.verify_use_numeric = true;
  if (!referralCode.empty()) args.referral_code = referralCode;
  api_->networkCreate(args, [this, done](std::optional<urnet::NetworkCreateResult> result,
                                         std::optional<std::string> err) {
    HandleNetworkCreateResult(std::move(result), std::move(err), done);
  });
}

void SdkHost::CreateNetworkWithPendingWallet(const std::string& networkName,
                                             const std::string& referralCode,
                                             std::function<void(AuthResult)> done) {
  std::optional<urnet::WalletAuthArgs> walletAuth;
  {
    std::scoped_lock lock(mutex_);
    walletAuth = pendingWalletAuth_;
  }
  if (!walletAuth) {
    done({false, false, "no wallet sign-in pending"});
    return;
  }
  urnet::NetworkCreateArgs args;
  args.user_name = std::string();
  args.network_name = networkName;
  args.terms = true;
  args.verify_use_numeric = true;
  if (!referralCode.empty()) args.referral_code = referralCode;
  args.wallet_auth = walletAuth;  // the signed challenge from the wallet sign-in
  api_->networkCreate(args, [this, done](std::optional<urnet::NetworkCreateResult> result,
                                         std::optional<std::string> err) {
    HandleNetworkCreateResult(std::move(result), std::move(err), done);
  });
}

bool SdkHost::HasPendingWalletAuth() {
  std::scoped_lock lock(mutex_);
  return pendingWalletAuth_.has_value();
}

void SdkHost::UpgradeGuest(const std::string& networkName, const std::string& userAuth,
                           const std::string& password, std::function<void(AuthResult)> done) {
  urnet::UpgradeGuestArgs args;
  args.network_name = networkName;
  args.user_auth = userAuth;
  args.password = password;
  api_->upgradeGuest(args, [this, done](std::optional<urnet::UpgradeGuestResult> result,
                                        std::optional<std::string> err) {
    if (err) { done({false, false, *err}); return; }
    if (!result) { done({false, false, "no result"}); return; }
    if (result->error && !result->error->message.empty()) { done({false, false, result->error->message}); return; }
    if (result->verification_required) { done({false, true, ""}); return; }
    if (result->network && result->network->by_jwt) {
      // the upgraded network needs a fresh device under the new jwt:
      // RegisterNetworkClient tears the guest device down and re-registers;
      // the UI restarts the tunnel (the mac handleSuccessWithJwt rebuild)
      RegisterNetworkClient(*result->network->by_jwt, done);
      return;
    }
    done({false, false, "guest upgrade returned no network"});
  });
}

void SdkHost::VerifyCode(const std::string& userAuth, const std::string& code,
                         std::function<void(AuthResult)> done) {
  urnet::AuthVerifyArgs args;
  args.user_auth = userAuth;
  args.verify_code = code;
  api_->authVerify(args, [this, done](std::optional<urnet::AuthVerifyResult> result,
                                      std::optional<std::string> err) {
    if (err) { done({false, false, *err}); return; }
    if (!result) { done({false, false, "no result"}); return; }
    if (result->error && !result->error->message.empty()) { done({false, false, result->error->message}); return; }
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, done);
      return;
    }
    done({false, false, "verify returned no network"});
  });
}

void SdkHost::ResendVerifyCode(const std::string& userAuth,
                               std::function<void(bool ok, std::string error)> done) {
  urnet::AuthVerifySendArgs args;
  args.user_auth = userAuth;
  args.use_numeric = true;  // the verify page's OTP entry is numeric
  api_->authVerifySend(args, [done](std::optional<urnet::AuthVerifySendResult> result,
                                    std::optional<std::string> err) {
    if (err) { done(false, *err); return; }
    if (!result) { done(false, "no result"); return; }
    done(true, "");
  });
}

void SdkHost::SendPasswordResetLink(const std::string& userAuth,
                                    std::function<void(bool ok, std::string error)> done) {
  urnet::AuthPasswordResetArgs args;
  args.user_auth = userAuth;
  api_->authPasswordReset(args, [done](std::optional<urnet::AuthPasswordResetResult> result,
                                       std::optional<std::string> err) {
    if (err) { done(false, *err); return; }
    if (!result) { done(false, "no result"); return; }
    done(true, "");
  });
}

void SdkHost::CheckNetworkName(const std::string& networkName,
                               std::function<void(bool ok, bool available)> done) {
  std::scoped_lock lock(mutex_);
  if (!networkNameVc_) { done(false, false); return; }
  networkNameVc_->networkCheck(networkName,
                               [done](std::optional<urnet::NetworkCheckResult> result,
                                      std::optional<std::string> err) {
                                 if (err || !result) { done(false, false); return; }
                                 done(true, result->available);
                               });
}

void SdkHost::ValidateReferralCode(const std::string& referralCode,
                                   std::function<void(bool ok, bool valid, bool capped)> done) {
  urnet::ValidateReferralCodeArgs args;
  args.referral_code = referralCode;
  api_->validateReferralCode(args,
                             [done](std::optional<urnet::ValidateReferralCodeResult> result,
                                    std::optional<std::string> err) {
                               if (err || !result) { done(false, false, false); return; }
                               done(true, result->is_valid, result->is_capped);
                             });
}

// ---- balance plumbing --------------------------------------------------------

std::optional<urnet::ByJwt> SdkHost::ParseByJwt() {
  std::scoped_lock lock(mutex_);
  if (!localState_) return std::nullopt;
  try {
    return localState_->parseByJwt();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] parse by jwt failed: %s\n", e.what());
    return std::nullopt;
  }
}

void SdkHost::RefreshJwt() {
  std::scoped_lock lock(mutex_);
  if (!device_) return;  // refreshed on the next device creation anyway
  try {
    device_->refreshToken(0);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] refresh token failed: %s\n", e.what());
  }
}

// ---- Sign in with a wallet (Solana / Bittensor via ur.io/wallet-connect) ----

void SdkHost::SetupWalletCallbacks() {
  // Solana is a two-hop flow: connect first, then ask the wallet to sign the
  // challenge. (Bittensor never fires this — it signs in a single hop.)
  wallet_.on_public_key = [this](std::string, WalletConnect::Provider) {
    wallet_.SignMessage(kWalletSignInMessage);
  };
  // Either way the wallet address is on the WalletConnect by now: solana set it
  // on the connect callback, bittensor returns it alongside the signature.
  wallet_.on_signature = [this](std::string signature) {
    const bool bittensor = wallet_.provider() == WalletConnect::Provider::Bittensor;
    AuthLoginWithWallet(wallet_.publicKey(), signature, kWalletSignInMessage,
                        bittensor ? urnet::TAO : kSolanaBlockchain);
  };
  wallet_.on_error = [this](std::string err) {
    auto done = walletAuthDone_;
    walletAuthDone_ = nullptr;
    if (done) done({false, false, err});
  };
}

void SdkHost::SignInWithSolana(WalletConnect::Provider provider,
                               std::function<void(AuthResult)> done) {
  walletAuthDone_ = std::move(done);
  wallet_.Connect(provider);  // opens the browser; the rest continues on the deep-link callback
}

void SdkHost::SignInWithBittensor(std::function<void(AuthResult)> done) {
  walletAuthDone_ = std::move(done);
  // one hop: the bridge connects the substrate wallet and signs; the rest
  // continues on the urnetwork://bittensor-sign-message callback
  wallet_.SignInWithBittensor(kWalletSignInMessage);
}

void SdkHost::HandleDeepLink(const std::string& url) {
  wallet_.HandleDeepLink(url);  // returns false for non-wallet links (future: OAuth)
}

void SdkHost::AuthLoginWithWallet(const std::string& address, const std::string& signature,
                                  const std::string& message, const std::string& blockchain) {
  urnet::WalletAuthArgs w;
  w.wallet_address = address;  // base58 public key (solana) | ss58 address (TAO)
  w.wallet_signature = signature;
  w.wallet_message = message;
  w.blockchain = blockchain;
  urnet::AuthLoginArgs args;
  args.wallet_auth = w;
  api_->authLogin(args, [this, w](std::optional<urnet::AuthLoginResult> result,
                                  std::optional<std::string> err) {
    auto done = walletAuthDone_;
    walletAuthDone_ = nullptr;
    if (err) { if (done) done({false, false, *err}); return; }
    if (!result) { if (done) done({false, false, "no result"}); return; }
    if (result->error && !result->error->message.empty()) {
      if (done) done({false, false, result->error->message});
      return;
    }
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, done ? done : [](AuthResult) {});
      return;
    }
    // Wallet authenticated but isn't linked to a network yet. Keep the signed
    // wallet_auth we sent and route into the create-network page (android/
    // apple do the same): NetworkCreate{wallet_auth} works for solana AND TAO.
    {
      std::scoped_lock lock(mutex_);
      pendingWalletAuth_ = w;
    }
    if (done) {
      AuthResult r;
      r.wallet_needs_network = true;
      done(r);
    }
  });
}

void SdkHost::RegisterNetworkClient(const std::string& byJwt, std::function<void(AuthResult)> done) {
  {
    // a new network jwt invalidates a running device (guest upgrade, verify
    // after an upgrade): tear it down so the UI rebuilds under the new auth.
    // Fresh sign-ins have no device and skip this. The daemon's tunnel runs
    // under the old jwt, so stop it too; the UI restarts it under the new one.
    std::scoped_lock lock(mutex_);
    if (device_) {
      TeardownDeviceLocked();
      control_.StopTunnel();
      EmitDrawerEvent(DrawerEvent::DeviceLifecycle);
    }
  }
  api_->setByJwt(byJwt);
  asyncLocalState_->setByJwt(byJwt, [](bool) {});
  urnet::AuthNetworkClientArgs args;
  args.description = UrDeviceDescription();
  args.device_spec = UrDeviceSpec();
  api_->authNetworkClient(args, [this, done](std::optional<urnet::AuthNetworkClientResult> result,
                                             std::optional<std::string> err) {
    if (err) { done({false, false, *err}); return; }
    if (!result) { done({false, false, "no result"}); return; }
    if (result->error && !result->error->message.empty()) { done({false, false, result->error->message}); return; }
    if (!result->by_client_jwt) { done({false, false, "no client jwt"}); return; }
    asyncLocalState_->setByClientJwt(*result->by_client_jwt, [](bool) {});
    if (onAuth_) onAuth_(true);
    done({true, false, ""});
  });
}

// ---- tunnel ---------------------------------------------------------------
// The split point (linux/MIGRATION.md). Everything privileged that used to
// happen here — DeviceLocal construction with persisted key material, the tun
// open/route/DNS setup, the IoLoop — now lives in urnetworkd (daemon
// TunnelHost). This side: control-channel handshake, then a DeviceRemote
// against the daemon's loopback mTLS device RPC. All the listeners and view
// controllers below are on the shared Device interface and run against the
// remote unchanged.

TunnelStartResult SdkHost::StartTunnel() {
  std::scoped_lock lock(mutex_);
  lastTunnelError_.clear();
  if (device_) return TunnelStartResult::Started;
  const std::string clientJwt = localState_->getByClientJwt();
  if (clientJwt.empty()) {
    lastTunnelError_ = "not signed in";
    return TunnelStartResult::Failed;
  }
  const std::string instanceId = localState_->getInstanceId();

  // 1) daemon session: connect + hello. The protocol version is enforced in
  //    BOTH directions here (APPIMAGE.md §11b) — each failure mode is a
  //    distinct, renderable state, never a silent false.
  std::string error;
  switch (control_.EnsureSession(&error)) {
    case DaemonSessionState::Ok:
      break;
    case DaemonSessionState::Unreachable:
      lastTunnelError_ = error;
      return TunnelStartResult::DaemonUnreachable;
    case DaemonSessionState::DaemonTooOld:
      lastTunnelError_ = error;
      return TunnelStartResult::DaemonTooOld;
    case DaemonSessionState::ClientTooOld:
      lastTunnelError_ = error;
      return TunnelStartResult::AppTooOld;
    case DaemonSessionState::SdkMismatch:
      lastTunnelError_ = error;
      return TunnelStartResult::SdkMismatch;
    case DaemonSessionState::Error:
      lastTunnelError_ = error;
      return TunnelStartResult::Failed;
  }

  // 2) start_tunnel: the daemon builds the DeviceLocal (rpc enabled), opens
  //    the tun and wires the IoLoop. First authenticated client wins; a
  //    tunnel owned by another live client comes back as a plain error.
  int rpcPort = 0;
  if (!control_.StartTunnel(clientJwt, instanceId, kAppVersion, &rpcPort, &error)) {
    lastTunnelError_ = error;
    switch (control_.LastSessionState()) {
      case DaemonSessionState::Unreachable:
        return TunnelStartResult::DaemonUnreachable;
      case DaemonSessionState::DaemonTooOld:
        return TunnelStartResult::DaemonTooOld;
      case DaemonSessionState::ClientTooOld:
        return TunnelStartResult::AppTooOld;
      case DaemonSessionState::SdkMismatch:
        return TunnelStartResult::SdkMismatch;
      default:
        return TunnelStartResult::Failed;
    }
  }

  try {
    // 3) the remote face of the daemon's device. Uses the SDK's default
    //    loopback rpc address — the same one the daemon's DeviceLocal
    //    (enable_rpc=true) listens on; rpcPort from the reply is
    //    informational. Same instanceId on both sides: the rpc sync pairs on
    //    it and the daemon side rejects a mismatch.
    device_ = urnet::newDeviceRemoteWithDefaults(*networkSpace_, clientJwt, instanceId);

    // The jwt refresh (which runs immediately at device creation) tells us
    // when the stored client no longer exists on the server. Only marshal from
    // the callback: it runs on an sdk thread, and Logout() clears subs_ --
    // which would destroy the sub whose callback is running.
    subs_.push_back(device_->addAuthLogoutListener([this] {
      if (onAuthInvalid_) onAuthInvalid_();
    }));

    // A jwt refresh re-derives Pro from the (now-updated) token — mac's
    // JwtRefreshListener parity. Without this a mid-session Pro change (notably a
    // Pro->free lapse, which a Pro network's paused poll won't catch) isn't reflected
    // until the window is re-shown. Same marshaling rule as the logout listener.
    subs_.push_back(device_->addJwtRefreshListener([this](std::string) {
      if (onJwtRefreshed_) onJwtRefreshed_();
    }));

    // Restore the persisted performance profile (connection mode / fixed IP /
    // strong anonymization / post quantum encryption). Unlike the blocker,
    // dns settings, and overrides, the device does not restore the profile
    // from local state itself (the macOS DeviceManager does exactly this at
    // device creation). Applies over the device rpc.
    if (auto profile = localState_->getPerformanceProfile(); profile) {
      device_->setPerformanceProfile(profile);
    }

    // Restore the persisted provide control mode the same way: the device does
    // not restore it from local state itself, and starts at its default (the
    // macOS DeviceManager seeds exactly this at device creation). LocalState
    // defaults to "never" when nothing is stored — providing is opt-in.
    device_->setProvideControlMode(localState_->getProvideControlMode());

    // Connection choice is data-plane state, so keep this lightweight listener
    // alive for the tray even when every presentation controller is closed.
    subs_.push_back(device_->addConnectLocationChangeListener(
        [this](std::optional<urnet::ConnectLocation> location) {
          if (onStatus_) onStatus_(location ? "DESTINATION_SET" : "DISCONNECTED");
        }));
    if (presentationActive_) {
      SubscribeStats();
      SubscribeDrawer();
    }
    if (onStatus_) {
      onStatus_(device_->getConnectLocation() ? "DESTINATION_SET" : "DISCONNECTED");
    }
    EmitDrawerEvent(DrawerEvent::DeviceLifecycle);
    return TunnelStartResult::Started;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] start tunnel failed: %s\n", e.what());
    lastTunnelError_ = e.what();
    // StartTunnel is retryable. Tear down every partially-created resource and
    // listener so a failed attempt cannot leave a subscription or
    // manager-owned controller behind for the next attempt, and stop the
    // daemon-side tunnel we just started but cannot bind to.
    TeardownDeviceLocked();
    control_.StopTunnel();
    return TunnelStartResult::Failed;
  }
}

std::string SdkHost::LastTunnelError() {
  std::scoped_lock lock(mutex_);
  return lastTunnelError_;
}

// ---- live stats (macOS parity: listener-push, not polling) ----------------
// SubscribeStats runs under StartTunnel's lock; the callbacks (like the existing
// connection-status listener) read the SDK getters without the lock — the getters
// are thread-safe and Logout clears subs_ before resetting the objects.

void SdkHost::SubscribeStats() {
  if (!device_ || connectVc_) return;
  connectVc_ = device_->openConnectViewController();
  connectVc_->start();
  contractVc_ = device_->openContractViewController();  // live throughput feed
  auto pub = [this] { PublishStats(); };
  presentationSubs_.push_back(connectVc_->addConnectionStatusListener(pub));
  presentationSubs_.push_back(connectVc_->addGridListener(pub));  // provider window size
  presentationSubs_.push_back(connectVc_->addSelectedLocationListener(
      [this](std::optional<urnet::ConnectLocation>) { PublishStats(); }));
  presentationSubs_.push_back(contractVc_->addThroughputListener(pub));
  presentationSubs_.push_back(device_->addContractStatusChangeListener(
      [this](std::optional<urnet::ContractStatus>) { PublishStats(); }));
  presentationSubs_.push_back(device_->addProvideChangeListener([this](bool) { PublishStats(); }));
  presentationSubs_.push_back(
      device_->addProvidePausedChangeListener([this](bool) { PublishStats(); }));
  // The network-visible bit: DeviceRemote exposes the provide secret keys only
  // as a listener (the getter is DeviceLocal-only), so cache the derived flag
  // — the Windows GUI does exactly this.
  presentationSubs_.push_back(device_->addProvideSecretKeysListener(
      [this](std::optional<urnet::ProvideSecretKeyList> keys) {
        bool hasNetworkKey = false;
        if (keys) {
          for (const auto& key : *keys) {
            if (key.provide_mode == 1 /* network — bit set, per-case */) {
              hasNetworkKey = true;
              break;
            }
          }
        }
        provideHasNetworkKey_.store(hasNetworkKey);
        PublishStats();
      }));
  presentationSubs_.push_back(device_->addTunnelChangeListener([this](bool) { PublishStats(); }));
  PublishStats();  // initial snapshot
}

LiveStats SdkHost::ReadStats() {
  LiveStats s;
  if (connectVc_) {
    s.connectionStatus = connectVc_->getConnectionStatus();
    s.connected = connectVc_->getConnected();
    auto grid = connectVc_->getGrid();
    s.providerCount = grid.getWindowCurrentSize();
  } else if (device_) {
    s.connected = device_->getConnectLocation().has_value();
    s.connectionStatus = s.connected ? "DESTINATION_SET" : "DISCONNECTED";
  }
  if (contractVc_) {
    if (auto pts = contractVc_->getThroughputPoints(); pts && !pts->empty()) {
      for (auto it = pts->rbegin(); it != pts->rend(); ++it) {
        if (it->Remote) {
          s.downBitsPerSecond = it->Remote->IngressBitRate;
          s.upBitsPerSecond = it->Remote->EgressBitRate;
          break;
        }
      }
    }
  }
  if (device_) {
    if (auto cs = device_->getContractStatus(); cs) s.insufficientBalance = cs->InsufficientBalance;
    s.provideEnabled = device_->getProvideEnabled();
    s.providePaused = device_->getProvidePaused();
    s.provideMode = static_cast<int64_t>(device_->getProvideMode());
    // cached off addProvideSecretKeysListener (no remote getter; see
    // SubscribeStats)
    s.provideHasNetworkKey = provideHasNetworkKey_.load();
    if (auto np = device_->getNetworkPeers(); np && np->Connected) {
      s.provideClients = static_cast<int64_t>(np->Connected->size());
    }
  }
  return s;
}

void SdkHost::PublishStats() {
  if (onStats_) onStats_(ReadStats());
}

LiveStats SdkHost::CurrentStats() { return ReadStats(); }

// ---- connect drawer feed ---------------------------------------------------
// Same threading contract as the stats feed: SubscribeDrawer runs under
// StartTunnel's lock; the listener callbacks fire on SDK threads and only emit
// an event tag — the UI marshals onto the GTK loop and re-reads through the
// locked accessors below.

void SdkHost::EmitDrawerEvent(DrawerEvent event) {
  if (onDrawerEvent_) onDrawerEvent_(event);
}

void SdkHost::SubscribeDrawer() {
  if (!device_ || !contractVc_) return;
  blockActionVc_ = device_->openBlockActionViewController();  // block actions/stats feed
  presentationSubs_.push_back(contractVc_->addThroughputListener(
      [this] { EmitDrawerEvent(DrawerEvent::Throughput); }));
  presentationSubs_.push_back(blockActionVc_->addBlockActionsListener(
      [this] { EmitDrawerEvent(DrawerEvent::BlockActions); }));
  presentationSubs_.push_back(blockActionVc_->addBlockActionStatsListener(
      [this] { EmitDrawerEvent(DrawerEvent::BlockStats); }));
  presentationSubs_.push_back(device_->addBlockActionOverridesChangeListener(
      [this](std::optional<urnet::BlockActionOverrideList>) {
        EmitDrawerEvent(DrawerEvent::Overrides);
      }));
  presentationSubs_.push_back(device_->addDnsResolverSettingsChangeListener(
      [this](std::optional<urnet::DnsResolverSettings>) {
        EmitDrawerEvent(DrawerEvent::DnsSettings);
      }));
  presentationSubs_.push_back(device_->addBlockerEnabledChangeListener(
      [this](bool) { EmitDrawerEvent(DrawerEvent::Blocker); }));
  // contract details: a single-feed ContractDetailsViewController for this device's
  // own (client) traffic. The VC groups the egress + ingress contracts per peer
  // (direction-resolved), keeps each direction's contracts un-aggregated and
  // newest-first, runs the closing/eject lifecycle, owns the display ordering (the
  // at-top activity sort + the scrolled-away freeze + the "N new" pending count),
  // and rate-limits recomputes (RowsUpdateThrottle, ~1/s). It fires
  // ContractRowsChanged once per settled change; the sheet re-reads ContractRows()
  // + ContractsPendingCount() and animates the per-contract stacks itself. (A
  // provider sheet would open its own VC via openProviderContractDetailsViewController.)
  clientContractDetailsVc_ = device_->openClientContractDetailsViewController();
  presentationSubs_.push_back(clientContractDetailsVc_->addContractRowsListener(
      [this] { EmitDrawerEvent(DrawerEvent::Contracts); }));
  clientContractDetailsVc_->start();
  presentationSubs_.push_back(device_->addConnectLocationChangeListener(
      [this](std::optional<urnet::ConnectLocation>) { EmitDrawerEvent(DrawerEvent::Location); }));
  presentationSubs_.push_back(device_->addPerformanceProfileChangeListener(
      [this](std::optional<urnet::PerformanceProfile>) {
        EmitDrawerEvent(DrawerEvent::Profile);
      }));

  // provider chooser: the bucketed location feed + the connected, provide-enabled
  // peers pinned at its top. start() kicks the initial load (FilterLocations("")).
  locationsVc_ = device_->openLocationsViewController();
  presentationSubs_.push_back(locationsVc_->addFilteredLocationsListener(
      [this](std::optional<urnet::FilteredLocations>, std::string) {
        EmitDrawerEvent(DrawerEvent::Locations);
      }));
  locationsVc_->start();
  peerVc_ = device_->openPeerViewController();
  presentationSubs_.push_back(peerVc_->addPeersListener(
      [this](std::optional<urnet::NetworkPeerList>) { EmitDrawerEvent(DrawerEvent::Peers); }));
  peerVc_->start();

  // post quantum identity: the device's own identity key (hash) + the
  // providers with an identity-verified e2e session, via the SDK's shared
  // view controller (the apple PostQuantumIdentityStore binds the same one —
  // it re-emits the device's urnet_device_add_provider_identity_change_listener
  // feed). start() seeds the listener with the current state.
  pqiVc_ = device_->openPostQuantumIdentityViewController();
  presentationSubs_.push_back(pqiVc_->addPostQuantumIdentityListener(
      [this] { EmitDrawerEvent(DrawerEvent::ProviderIdentities); }));
  pqiVc_->start();

  // connected provider locations: a pure derivation over the window monitor, so
  // there is no view controller to open or start -- just the signal-only change
  // listener. It carries no payload by design; every consumer re-reads
  // ConnectedProviderLocations(). It fires on window turnover, which is frequent,
  // so the sheet dedupes by value before touching widgets.
  presentationSubs_.push_back(device_->addConnectedProviderLocationChangeListener(
      [this] { EmitDrawerEvent(DrawerEvent::ProviderLocations); }));
}

void SdkHost::ClosePresentationLocked() {
  presentationSubs_.clear();
  if (!device_) {
    connectVc_.reset();
    contractVc_.reset();
    clientContractDetailsVc_.reset();
    blockActionVc_.reset();
    locationsVc_.reset();
    peerVc_.reset();
    pqiVc_.reset();
    return;
  }
  if (pqiVc_) device_->closePostQuantumIdentityViewController(*pqiVc_);
  pqiVc_.reset();
  if (peerVc_) device_->closePeerViewController(*peerVc_);
  peerVc_.reset();
  if (locationsVc_) device_->closeLocationsViewController(*locationsVc_);
  locationsVc_.reset();
  if (clientContractDetailsVc_) {
    device_->closeContractDetailsViewController(*clientContractDetailsVc_);
  }
  clientContractDetailsVc_.reset();
  if (blockActionVc_) device_->closeBlockActionViewController(*blockActionVc_);
  blockActionVc_.reset();
  if (contractVc_) device_->closeContractViewController(*contractVc_);
  contractVc_.reset();
  if (connectVc_) device_->closeConnectViewController(*connectVc_);
  connectVc_.reset();
}

void SdkHost::SetPresentationActive(bool active) {
  std::scoped_lock lock(mutex_);
  if (presentationActive_ == active) return;
  presentationActive_ = active;
  if (!active) {
    ClosePresentationLocked();
    return;
  }
  if (!device_) return;
  SubscribeStats();
  SubscribeDrawer();
  EmitDrawerEvent(DrawerEvent::DeviceLifecycle);
}

// ---- connect drawer accessors ----------------------------------------------

std::optional<urnet::ConnectLocation> SdkHost::SelectedLocation() {
  std::scoped_lock lock(mutex_);
  if (device_) return device_->getConnectLocation();
  if (localState_) return localState_->getConnectLocation();
  return std::nullopt;
}

std::optional<urnet::PerformanceProfile> SdkHost::GetPerformanceProfile() {
  std::scoped_lock lock(mutex_);
  if (device_) return device_->getPerformanceProfile();
  if (localState_) return localState_->getPerformanceProfile();
  return std::nullopt;
}

void SdkHost::SetPerformanceProfile(const std::optional<urnet::PerformanceProfile>& profile) {
  std::scoped_lock lock(mutex_);
  // persist to local state (DeviceLocal does not persist the profile itself),
  // then apply live
  if (localState_) localState_->setPerformanceProfile(profile);
  if (device_) device_->setPerformanceProfile(profile);
}

bool SdkHost::GetBlockerEnabled() {
  std::scoped_lock lock(mutex_);
  if (device_) return device_->getBlockerEnabled();
  return localState_ && localState_->getBlockerEnabled();
}

void SdkHost::SetBlockerEnabled(bool enabled) {
  std::scoped_lock lock(mutex_);
  if (device_) {
    device_->setBlockerEnabled(enabled);  // the device persists to local state
    return;
  }
  // no device (tunnel down): persist the preference; restored at the next
  // device creation by the SDK
  if (localState_) localState_->setBlockerEnabled(enabled);
}

std::optional<urnet::DnsResolverSettings> SdkHost::GetDnsResolverSettings() {
  std::scoped_lock lock(mutex_);
  if (device_) return device_->getDnsResolverSettings();
  if (localState_) return localState_->getDnsResolverSettings();
  return std::nullopt;
}

void SdkHost::SetDnsResolverSettings(const urnet::DnsResolverSettings& settings) {
  std::scoped_lock lock(mutex_);
  if (device_) {
    device_->setDnsResolverSettings(settings);  // applies to the live mux + persists
    return;
  }
  if (localState_) localState_->setDnsResolverSettings(settings);
}

std::optional<urnet::ThroughputPointList> SdkHost::ThroughputPoints() {
  std::scoped_lock lock(mutex_);
  if (!contractVc_) return std::nullopt;
  return contractVc_->getThroughputPoints();
}

int64_t SdkHost::ThroughputWindowSeconds() {
  std::scoped_lock lock(mutex_);
  return contractVc_ ? contractVc_->getWindowDurationSeconds() : 60;
}

std::optional<urnet::BlockActionList> SdkHost::BlockActions() {
  std::scoped_lock lock(mutex_);
  if (!blockActionVc_) return std::nullopt;
  return blockActionVc_->getBlockActions();
}

std::optional<urnet::BlockStats> SdkHost::BlockStatsSnapshot() {
  std::scoped_lock lock(mutex_);
  if (!blockActionVc_) return std::nullopt;
  return blockActionVc_->getBlockStats();
}

std::optional<urnet::BlockActionOverrideList> SdkHost::BlockActionOverrides() {
  std::scoped_lock lock(mutex_);
  if (device_) return device_->getBlockActionOverrides();
  if (localState_) return localState_->getBlockActionOverrides();
  return std::nullopt;
}

void SdkHost::AddBlockActionOverride(const urnet::BlockActionOverride& override_) {
  std::scoped_lock lock(mutex_);
  if (device_) {
    device_->addBlockActionOverride(override_);  // the device persists
    return;
  }
  if (localState_) {
    urnet::BlockActionOverrideList overrides;
    if (auto current = localState_->getBlockActionOverrides()) overrides = std::move(*current);
    overrides.push_back(override_);
    localState_->setBlockActionOverrides(overrides);
  }
}

void SdkHost::SetBlockActionOverrideHosts(const std::string& overrideId,
                                          const urnet::StringList& hosts) {
  std::scoped_lock lock(mutex_);
  std::optional<urnet::BlockActionOverrideList> overrides;
  if (device_) {
    overrides = device_->getBlockActionOverrides();
  } else if (localState_) {
    overrides = localState_->getBlockActionOverrides();
  }
  if (!overrides) return;
  // set the hosts on the backing override, then rebuild the full list
  bool found = false;
  for (auto& override_ : *overrides) {
    if (override_.OverrideId && *override_.OverrideId == overrideId) {
      override_.Hosts = hosts;
      found = true;
      break;
    }
  }
  if (!found) return;
  if (device_) {
    device_->setBlockActionOverrides(overrides);
  } else if (localState_) {
    localState_->setBlockActionOverrides(overrides);
  }
}

void SdkHost::RemoveBlockActionOverride(const std::string& overrideId) {
  std::scoped_lock lock(mutex_);
  if (device_) {
    device_->removeBlockActionOverride(overrideId);
    return;
  }
  if (localState_) {
    auto overrides = localState_->getBlockActionOverrides();
    if (!overrides) return;
    overrides->erase(std::remove_if(overrides->begin(), overrides->end(),
                                    [&](const urnet::BlockActionOverride& o) {
                                      return o.OverrideId && *o.OverrideId == overrideId;
                                    }),
                     overrides->end());
    localState_->setBlockActionOverrides(overrides);
  }
}

std::string SdkHost::ClientId() {
  std::scoped_lock lock(mutex_);
  return device_ ? device_->getClientId() : std::string();
}

std::optional<urnet::ContractPeerRowList> SdkHost::ContractRows() {
  std::scoped_lock lock(mutex_);
  if (!clientContractDetailsVc_) return std::nullopt;
  return clientContractDetailsVc_->getContractRows();
}

void SdkHost::SetContractsAtTop(bool atTop) {
  std::scoped_lock lock(mutex_);
  // reports scroll to the VC, which owns the ordering: at the top it re-sorts
  // active rows above idle ones; scrolled away it freezes membership + order and
  // collects new rows into pendingCount()
  if (clientContractDetailsVc_) clientContractDetailsVc_->setAtTop(atTop);
}

int64_t SdkHost::ContractsPendingCount() {
  std::scoped_lock lock(mutex_);
  return clientContractDetailsVc_ ? clientContractDetailsVc_->pendingCount() : 0;
}

std::optional<urnet::FilteredLocations> SdkHost::GetFilteredLocations() {
  std::scoped_lock lock(mutex_);
  if (locationsVc_) return locationsVc_->getFilteredLocations();
  return std::nullopt;
}

void SdkHost::FilterLocations(const std::string& query) {
  std::scoped_lock lock(mutex_);
  if (locationsVc_) locationsVc_->filterLocations(query);
}

std::string SdkHost::GetFilteredLocationState() {
  std::scoped_lock lock(mutex_);
  if (locationsVc_) return locationsVc_->getFilteredLocationState();
  return std::string();
}

std::optional<urnet::NetworkPeerList> SdkHost::ConnectedProvidePeers() {
  std::scoped_lock lock(mutex_);
  if (peerVc_) return peerVc_->getPeers();
  return std::nullopt;
}

int64_t SdkHost::ConnectedPeerCount() {
  std::scoped_lock lock(mutex_);
  // ALL connected peers, whether or not they provide — the "You have {n}
  // other devices online" count (connecting still requires provide, which is
  // what ConnectedProvidePeers captures)
  if (peerVc_) return static_cast<int64_t>(peerVc_->getConnectedCount());
  return 0;
}

// ---- post quantum identity (PQI) --------------------------------------------

std::optional<urnet::ProviderIdentityList> SdkHost::ProviderIdentities() {
  std::scoped_lock lock(mutex_);
  if (!pqiVc_) return std::nullopt;
  return pqiVc_->getProviderIdentities();
}

// ---- connected provider locations --------------------------------------------

std::optional<urnet::ConnectedProviderLocationList> SdkHost::ConnectedProviderLocations() {
  std::scoped_lock lock(mutex_);
  if (!device_) return std::nullopt;  // tunnel down: the sheet shows the unavailable state
  return device_->getConnectedProviderLocations();
}

void SdkHost::RemoveConnectedProvider(const std::string& clientId) {
  std::scoped_lock lock(mutex_);
  if (!device_ || clientId.empty()) return;
  device_->removeConnectedProvider(clientId);
}

std::string SdkHost::PublicIdentityKeyHash() {
  std::scoped_lock lock(mutex_);
  return pqiVc_ ? pqiVc_->getPublicIdentityKeyHash() : std::string();
}

std::vector<uint8_t> SdkHost::PublicIdentityKey() {
  std::scoped_lock lock(mutex_);
  // the raw key comes off the device (the linux cgo VC exposes only the hash)
  return device_ ? device_->getPublicIdentityKey() : std::vector<uint8_t>();
}

void SdkHost::ConnectBestAvailable() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) {
    connectVc_->connectBestAvailable();
  } else if (device_) {
    auto controller = device_->openConnectViewController();
    controller.connectBestAvailable();
    device_->closeConnectViewController(controller);
  }
}

void SdkHost::Connect(const std::optional<urnet::ConnectLocation>& location) {
  std::scoped_lock lock(mutex_);
  if (connectVc_) {
    connectVc_->connect(location);
  } else if (device_) {
    auto controller = device_->openConnectViewController();
    controller.connect(location);
    device_->closeConnectViewController(controller);
  }
}

void SdkHost::Disconnect() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) {
    connectVc_->disconnect();
  } else if (device_) {
    auto controller = device_->openConnectViewController();
    controller.disconnect();
    device_->closeConnectViewController(controller);
  }
}

bool SdkHost::Connected() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) return connectVc_->getConnected();
  return device_ && device_->getConnectLocation().has_value();
}

void SdkHost::SetProvideControlMode(const std::string& mode) {
  std::scoped_lock lock(mutex_);
  if (device_) device_->setProvideControlMode(mode);
  // Persist alongside the device write, like ResetProvideToNever below (mac
  // handleProvideControlModeUpdate does both) — DeviceLocal.SetProvideControlMode
  // alone does not persist, and StartTunnel restores the persisted mode.
  if (localState_) localState_->setProvideControlMode(mode);
}

std::string SdkHost::GetProvideControlMode() {
  std::scoped_lock lock(mutex_);
  if (device_) return device_->getProvideControlMode();
  if (localState_) return localState_->getProvideControlMode();
  return "never";
}

bool SdkHost::ProvideEnabled() {
  std::scoped_lock lock(mutex_);
  return device_ && device_->getProvideEnabled();
}

// The free -> Pro upgrade side effect. mac handleProvideControlModeUpdate
// (DeviceManager.provideControlMode's didSet) applies the mode to the device
// AND persists it to local state — DeviceLocal.SetProvideControlMode alone
// does not persist. Mirror both writes; with the tunnel down only the
// persisted preference is written.
void SdkHost::ResetProvideToNever() {
  std::scoped_lock lock(mutex_);
  if (device_) device_->setProvideControlMode("never");
  if (localState_) localState_->setProvideControlMode("never");
}

// DeviceRemote teardown without touching the stored auth or the daemon:
// Logout adds the auth clear + stop_tunnel; the guest upgrade only swaps the
// device. Caller holds mutex_.
void SdkHost::TeardownDeviceLocked() {
  ClosePresentationLocked();
  subs_.clear();
  // close() actually stops the remote's rpc connection, sync loop and view
  // controllers; reset() alone only releases the handle (urnet_release),
  // leaking them on every logout. The daemon's DeviceLocal, tun and IoLoop
  // are NOT touched here — stopping the tunnel is an explicit stop_tunnel on
  // the control channel, decided by the caller.
  if (device_) { device_->close(); device_.reset(); }
  provideHasNetworkKey_.store(false);
}

void SdkHost::Logout() {
  std::scoped_lock lock(mutex_);
  TeardownDeviceLocked();
  // the session is over: bring the daemon's tunnel down too (best effort — an
  // unreachable daemon has nothing running for us anyway)
  control_.StopTunnel();
  pendingWalletAuth_.reset();
  if (asyncLocalState_) asyncLocalState_->logout([](bool) {});
  if (onAuth_) onAuth_(false);
  EmitDrawerEvent(DrawerEvent::DeviceLifecycle);  // drawer falls back to empty states
}

}  // namespace urnw
