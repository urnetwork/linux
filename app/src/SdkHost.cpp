// SPDX-License-Identifier: MPL-2.0
#include "SdkHost.hpp"

#include <cstdio>

namespace urnw {
namespace {
constexpr const char* kHostName = "ur.network";
constexpr const char* kEnvName = "main";
constexpr const char* kDeviceDescription = "linux-desktop";
constexpr const char* kAppVersion = "0.0.1";
constexpr int64_t kMemoryLimit = 64ll * 1024 * 1024;
// The challenge the wallet signs for Sign-in-with-Solana (macOS parity).
constexpr const char* kWalletSignInMessage = "Welcome to URnetwork";

std::string DeviceSpec() {
#if defined(__aarch64__)
  return "linux arm64";
#else
  return "linux amd64";
#endif
}
}  // namespace

bool SdkHost::Initialize(const std::string& storageDir, const std::string& logDir) {
  std::scoped_lock lock(mutex_);
  try {
    urnet::setLogDir(logDir);
    urnet::setMemoryLimit(kMemoryLimit);
    spaceManager_ = urnet::newNetworkSpaceManager(storageDir);
    networkSpace_ = BuildNetworkSpace();
    api_ = networkSpace_->getApi();
    asyncLocalState_ = networkSpace_->getAsyncLocalState();
    localState_ = asyncLocalState_->getLocalState();
    SetupWalletCallbacks();
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] initialize failed: %s\n", e.what());
    return false;
  }
}

urnet::NetworkSpace SdkHost::BuildNetworkSpace() {
  urnet::NetworkSpaceKey key;
  key.host_name = std::string(kHostName);
  key.env_name = std::string(kEnvName);
  urnet::NetworkSpaceValues values;
  values.bundled = true;
  values.net_expose_server_ips = true;
  values.net_expose_server_host_names = true;
  values.link_host_name = "ur.io";
  values.migration_host_name = "bringyour.com";
  values.wallet = "circle";
  values.sso_google = false;
  return spaceManager_->updateNetworkSpaceValues(key, values);
}

bool SdkHost::IsLoggedIn() {
  std::scoped_lock lock(mutex_);
  return localState_ && !localState_->getByClientJwt().empty();
}

// ---- auth (mirrors the Windows SdkHost) -----------------------------------

void SdkHost::LoginWithPassword(const std::string& userAuth, const std::string& password,
                                std::function<void(AuthResult)> done) {
  urnet::AuthLoginWithPasswordArgs args;
  args.user_auth = userAuth;
  args.password = password;
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

// ---- Sign in with Solana (Phantom/Solflare via ur.io/wallet-connect) --------

void SdkHost::SetupWalletCallbacks() {
  wallet_.on_public_key = [this](std::string publicKey, WalletConnect::Provider) {
    walletAddress_ = publicKey;
    wallet_.SignMessage(kWalletSignInMessage);  // now ask the wallet to sign the challenge
  };
  wallet_.on_signature = [this](std::string signatureB64) {
    AuthLoginWithWallet(walletAddress_, signatureB64, kWalletSignInMessage);
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

void SdkHost::HandleDeepLink(const std::string& url) {
  wallet_.HandleDeepLink(url);  // returns false for non-wallet links (future: OAuth)
}

void SdkHost::AuthLoginWithWallet(const std::string& address, const std::string& signatureB64,
                                  const std::string& message) {
  urnet::WalletAuthArgs w;
  w.wallet_address = address;
  w.wallet_signature = signatureB64;
  w.wallet_message = message;
  w.blockchain = "solana";
  urnet::AuthLoginArgs args;
  args.wallet_auth = w;
  api_->authLogin(args, [this](std::optional<urnet::AuthLoginResult> result,
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
    // Wallet authenticated but isn't linked to a network yet — the create-account-
    // with-wallet path (NetworkCreate{wallet_auth}) needs the account UI.
    if (done)
      done({false, false, "This wallet isn't linked to a network yet — create an account to continue."});
  });
}

void SdkHost::RegisterNetworkClient(const std::string& byJwt, std::function<void(AuthResult)> done) {
  api_->setByJwt(byJwt);
  asyncLocalState_->setByJwt(byJwt, [](bool) {});
  urnet::AuthNetworkClientArgs args;
  args.description = kDeviceDescription;
  args.device_spec = DeviceSpec();
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

bool SdkHost::StartTunnel() {
  std::scoped_lock lock(mutex_);
  if (device_) return true;
  const std::string clientJwt = localState_->getByClientJwt();
  if (clientJwt.empty()) return false;
  const std::string instanceId = localState_->getInstanceId();

  try {
    device_ = urnet::newDeviceLocalWithDefaults(*networkSpace_, clientJwt, kDeviceDescription,
                                                DeviceSpec(), kAppVersion, instanceId,
                                                /*enable_rpc=*/false);

    TunnelConfig cfg;
    cfg.local_addr = device_->tunnelLocalAddress();
    if (cfg.local_addr.empty()) cfg.local_addr = "169.254.2.1";
    if (auto dns = device_->tunnelDnsSetting(); dns && !dns->Server.empty()) cfg.dns_server = dns->Server;

    tunnel_ = Tunnel::Open(cfg);
    if (!tunnel_) { device_.reset(); return false; }

    // hand the tun fd to the SDK's fd loop (the Android/Linux data plane path)
    ioLoop_ = urnet::newIoLoop(*device_, tunnel_->fd(), [] {});
    device_->setTunnelStarted(true);

    connectVc_ = device_->openConnectViewController();
    connectVc_->start();
    subs_.push_back(connectVc_->addConnectionStatusListener([this] {
      if (onStatus_ && connectVc_) onStatus_(connectVc_->getConnectionStatus());
    }));
    SubscribeStats();  // live connection/throughput/provide feed (macOS parity)
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] start tunnel failed: %s\n", e.what());
    tunnel_.reset();
    if (device_) { device_->close(); device_.reset(); }
    return false;
  }
}

// ---- live stats (macOS parity: listener-push, not polling) ----------------
// SubscribeStats runs under StartTunnel's lock; the callbacks (like the existing
// connection-status listener) read the SDK getters without the lock — the getters
// are thread-safe and Logout clears subs_ before resetting the objects.

void SdkHost::SubscribeStats() {
  if (!device_ || !connectVc_) return;
  contractVc_ = device_->openContractViewController();  // live throughput feed
  auto pub = [this] { PublishStats(); };
  subs_.push_back(connectVc_->addConnectionStatusListener(pub));  // status -> full refresh
  subs_.push_back(connectVc_->addGridListener(pub));  // provider window size
  subs_.push_back(connectVc_->addSelectedLocationListener(
      [this](std::optional<urnet::ConnectLocation>) { PublishStats(); }));
  subs_.push_back(contractVc_->addThroughputListener(pub));  // bytes/bit rate up/down
  subs_.push_back(device_->addContractStatusChangeListener(
      [this](std::optional<urnet::ContractStatus>) { PublishStats(); }));
  subs_.push_back(device_->addProvideChangeListener([this](bool) { PublishStats(); }));
  subs_.push_back(device_->addProvidePausedChangeListener([this](bool) { PublishStats(); }));
  subs_.push_back(device_->addTunnelChangeListener([this](bool) { PublishStats(); }));
  PublishStats();  // initial snapshot
}

LiveStats SdkHost::ReadStats() {
  LiveStats s;
  if (connectVc_) {
    s.connectionStatus = connectVc_->getConnectionStatus();
    s.connected = connectVc_->getConnected();
    auto grid = connectVc_->getGrid();
    s.providerCount = grid.getWindowCurrentSize();
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

void SdkHost::ConnectBestAvailable() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) connectVc_->connectBestAvailable();
}

void SdkHost::Disconnect() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) connectVc_->disconnect();
}

bool SdkHost::Connected() {
  std::scoped_lock lock(mutex_);
  return connectVc_ && connectVc_->getConnected();
}

void SdkHost::SetProvideEnabled(bool enabled) {
  std::scoped_lock lock(mutex_);
  if (device_) device_->setProvideControlMode(enabled ? "always" : "never");
}

bool SdkHost::ProvideEnabled() {
  std::scoped_lock lock(mutex_);
  return device_ && device_->getProvideEnabled();
}

void SdkHost::Logout() {
  std::scoped_lock lock(mutex_);
  subs_.clear();
  // close() actually stops the view controller's monitor goroutines + grid and
  // the IoLoop; reset() alone only releases the handle (urnet_release), leaking
  // them on every logout
  if (connectVc_) connectVc_->close();
  connectVc_.reset();
  if (device_) device_->setTunnelStarted(false);
  if (ioLoop_) ioLoop_->close();
  ioLoop_.reset();
  tunnel_.reset();
  if (device_) { device_->close(); device_.reset(); }
  if (asyncLocalState_) asyncLocalState_->logout([](bool) {});
  if (onAuth_) onAuth_(false);
}

}  // namespace urnw
