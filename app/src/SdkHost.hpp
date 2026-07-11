// SdkHost — the single-process core of the Linux app (the Android-like model).
// It owns the SDK objects (NetworkSpace, Api, LocalState, DeviceLocal) in-process
// and drives the tunnel directly through the SDK's fd-based IoLoop. No daemon,
// no RPC: this process IS the VPN. Consumes the shared cgo urnetwork_sdk.hpp
// wrapper (same as the WinUI Windows app) — this SDK-host logic is the layer
// shared across Windows and Linux; only the UI toolkit + tun layer differ.
//
// Callbacks fire on SDK background threads; the UI marshals them onto the GTK
// main loop.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <urnetwork_sdk.hpp>

#include "Tunnel.hpp"
#include "WalletConnect.hpp"

namespace urnw {

struct AuthResult {
  bool ok = false;
  bool verification_required = false;
  std::string error;
};

// Snapshot of live connection / throughput / provide stats. Pushed to the UI on
// SDK listener callbacks (macOS parity: listener-push, not polling). Shared shape
// with the Windows SdkHost.
struct LiveStats {
  std::string connectionStatus;
  bool connected = false;
  int64_t providerCount = 0;      // grid window current size
  int64_t downBitsPerSecond = 0;  // remote (tunneled) ingress bit rate
  int64_t upBitsPerSecond = 0;    // remote (tunneled) egress bit rate
  bool insufficientBalance = false;
  bool provideEnabled = false;
  bool providePaused = false;
  int64_t provideClients = 0;
};

class SdkHost {
 public:
  using AuthStateHandler = std::function<void(bool loggedIn)>;
  using ConnectionStatusHandler = std::function<void(std::string status)>;
  using StatsHandler = std::function<void(const LiveStats& stats)>;

  bool Initialize(const std::string& storageDir, const std::string& logDir);
  bool IsLoggedIn();

  void LoginWithPassword(const std::string& userAuth, const std::string& password,
                         std::function<void(AuthResult)> done);
  void LoginWithCode(const std::string& authCode, std::function<void(AuthResult)> done);
  // Guest mode: create a throwaway network (no email/password), same as iOS/macOS.
  // Upgradeable later via Api::upgradeGuest / upgradeGuestExisting.
  void LoginAsGuest(std::function<void(AuthResult)> done);

  // Sign in with a Solana wallet (Phantom/Solflare) via the ur.io/wallet-connect
  // browser bridge: connect -> sign a challenge -> authLogin{wallet_auth}. The
  // urnetwork:// callback must be routed back in via HandleDeepLink.
  void SignInWithSolana(WalletConnect::Provider provider, std::function<void(AuthResult)> done);

  // Route a urnetwork:// deep link (wallet callback, later OAuth) into the host.
  void HandleDeepLink(const std::string& url);

  void Logout();

  bool StartTunnel();  // build DeviceLocal, open tun, wire IoLoop, start connect VC
  void ConnectBestAvailable();
  void Disconnect();
  bool Connected();

  // Provide/earn: control mode "never"|"always"|"auto"|"manual".
  void SetProvideEnabled(bool enabled);
  bool ProvideEnabled();

  void SetAuthStateHandler(AuthStateHandler h) { onAuth_ = std::move(h); }
  void SetConnectionStatusHandler(ConnectionStatusHandler h) { onStatus_ = std::move(h); }
  // Live stats push (connection/throughput/provide). Fired on SDK listener
  // callbacks; the UI marshals onto the GTK loop and gates on window visibility.
  void SetStatsHandler(StatsHandler h) { onStats_ = std::move(h); }
  LiveStats CurrentStats();  // snapshot on demand (e.g. resync when window shows)

  // Exposed so the (full-parity) UI/view models can drive the SDK directly.
  urnet::Api& api() { return *api_; }
  bool hasDevice() { return device_.has_value(); }
  urnet::DeviceLocal& device() { return *device_; }

 private:
  urnet::NetworkSpace BuildNetworkSpace();
  void RegisterNetworkClient(const std::string& byJwt, std::function<void(AuthResult)> done);
  void SetupWalletCallbacks();
  void AuthLoginWithWallet(const std::string& address, const std::string& signatureB64,
                           const std::string& message);
  void SubscribeStats();  // subscribe the live-stats listeners (in StartTunnel)
  LiveStats ReadStats();  // read the current snapshot from the SDK getters
  void PublishStats();    // ReadStats() -> onStats_

  std::mutex mutex_;
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::Api> api_;
  std::optional<urnet::AsyncLocalState> asyncLocalState_;
  std::optional<urnet::LocalState> localState_;
  std::optional<urnet::DeviceLocal> device_;
  std::optional<urnet::ConnectViewController> connectVc_;
  std::optional<urnet::ContractViewController> contractVc_;  // live throughput feed
  std::optional<urnet::IoLoop> ioLoop_;
  std::unique_ptr<Tunnel> tunnel_;
  std::vector<urnet::Sub> subs_;

  WalletConnect wallet_;
  std::function<void(AuthResult)> walletAuthDone_;
  std::string walletAddress_;

  AuthStateHandler onAuth_;
  ConnectionStatusHandler onStatus_;
  StatsHandler onStats_;
};

}  // namespace urnw
