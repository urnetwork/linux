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
  // Wallet sign-in authenticated but the wallet has no network yet: the host
  // kept the signed wallet_auth (see CreateNetworkWithPendingWallet) and the
  // UI routes into the create-network page instead of dead-ending.
  bool wallet_needs_network = false;
};

// Outcome of the authLogin account discovery (macOS LoginInitialViewModel
// routing; same shape as the Windows SdkHost): an existing password account
// goes to the password step, an unknown user auth goes to sign-up, an
// account under another sign-in method reports the allowed methods.
enum class LoginRoute {
  Login,          // the discovery itself yielded a session (jwt)
  Password,       // existing account: prompt for the password
  Create,         // no account: create a network
  Verify,         // account exists but is unverified: enter the code
  IncorrectAuth,  // the user auth belongs to another sign-in method
  Error,
};

struct LoginRouting {
  LoginRoute route = LoginRoute::Error;
  std::string userAuth;     // echoed user auth (password / create / verify)
  std::string authAllowed;  // comma-joined methods (IncorrectAuth)
  std::string error;
};

// Change notifications for the connect drawer (charts, block actions, dns,
// blocker, contracts, connection controls). Fired on SDK listener threads —
// and from StartTunnel/Logout for DeviceLifecycle — so the UI must marshal
// onto the GTK loop and re-read through the SdkHost accessors.
enum class DrawerEvent {
  DeviceLifecycle,  // DeviceLocal created or destroyed
  Throughput,       // new throughput points (charts)
  BlockActions,     // block action window changed
  BlockStats,       // allowed/blocked counts changed
  Overrides,        // block action overrides ("split rules") changed
  DnsSettings,      // dns resolver settings changed
  Blocker,          // block-ads-and-trackers toggle changed
  Contracts,        // egress/ingress contract details changed
  Location,         // connect location changed
  Profile,          // performance profile changed
  Locations,        // filtered provider-location list changed (the chooser)
  Peers,            // connected network peers changed (chooser + drawer label)
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
  // the LIVE effective provide mode (protocol values: 0 none, 1 network,
  // 2 friends-and-family, 3 public — a bit set, compare per-case)
  int64_t provideMode = 0;
  // the provider holds a Network-mode provide key: with provideEnabled this
  // means the device is discoverable/connectable as a same-network peer
  bool provideHasNetworkKey = false;
};

class SdkHost {
 public:
  using AuthStateHandler = std::function<void(bool loggedIn)>;
  // Fired when the sdk finds the stored auth is no longer valid on the server
  // (e.g. the client was removed): the sdk has already cleared its local auth
  // state. The handler runs on an sdk thread and must only marshal -- the ui
  // marshals onto the main loop and calls Logout().
  using AuthInvalidHandler = std::function<void()>;
  using JwtRefreshedHandler = std::function<void()>;
  using ConnectionStatusHandler = std::function<void(std::string status)>;
  using StatsHandler = std::function<void(const LiveStats& stats)>;
  using DrawerEventHandler = std::function<void(DrawerEvent event)>;

  bool Initialize(const std::string& storageDir, const std::string& logDir);
  bool IsLoggedIn();

  // Account discovery for the email-first login flow (Api::authLogin with just
  // the user_auth): says whether the auth belongs to a password account, to
  // another sign-in method, or to nobody (-> sign-up). The callback fires on
  // an SDK thread with the routing decision.
  void StartLogin(const std::string& userAuth, std::function<void(LoginRouting)> done);
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

  // Sign in with a Bittensor wallet through the same bridge, in one hop: the
  // bridge signs the challenge with an injected substrate wallet (or a wallet app
  // paired over WalletConnect when Config.hpp carries a project id) and returns
  // the ss58 address + sr25519 signature -> authLogin{wallet_auth} with
  // blockchain urnet::TAO. Same deep-link routing as Solana (HandleDeepLink).
  void SignInWithBittensor(std::function<void(AuthResult)> done);

  // Route a urnetwork:// deep link (wallet callback, later OAuth) into the host.
  void HandleDeepLink(const std::string& url);

  // ---- sign-up / verify / password reset (Phase 3) --------------------------
  // Create a full network (sign-up): verification_required routes to the
  // verify page; by_jwt goes straight through RegisterNetworkClient.
  void CreateNetwork(const std::string& networkName, const std::string& userAuth,
                     const std::string& password, const std::string& referralCode,
                     std::function<void(AuthResult)> done);
  // Create a network bound to the wallet_auth captured by a wallet sign-in
  // that had no network yet (name + terms, no password).
  void CreateNetworkWithPendingWallet(const std::string& networkName,
                                      const std::string& referralCode,
                                      std::function<void(AuthResult)> done);
  bool HasPendingWalletAuth();
  // Guest -> full account (Api::upgradeGuest). On success the guest device is
  // torn down and the network client re-registered under the upgraded jwt;
  // the UI restarts the tunnel.
  void UpgradeGuest(const std::string& networkName, const std::string& userAuth,
                    const std::string& password, std::function<void(AuthResult)> done);
  // Verify-code entry (Api::authVerify) and resend (Api::authVerifySend).
  void VerifyCode(const std::string& userAuth, const std::string& code,
                  std::function<void(AuthResult)> done);
  void ResendVerifyCode(const std::string& userAuth,
                        std::function<void(bool ok, std::string error)> done);
  void SendPasswordResetLink(const std::string& userAuth,
                             std::function<void(bool ok, std::string error)> done);
  // Network-name availability through the SDK's shared
  // NetworkNameValidationViewController (the caller debounces).
  void CheckNetworkName(const std::string& networkName,
                        std::function<void(bool ok, bool available)> done);
  void ValidateReferralCode(const std::string& referralCode,
                            std::function<void(bool ok, bool valid, bool capped)> done);

  // ---- balance plumbing ------------------------------------------------------
  // Offline claims from the stored jwt (Pro / GuestMode / network name).
  std::optional<urnet::ByJwt> ParseByJwt();
  // Refresh the jwt when the server's Pro disagrees with the jwt claim (mac
  // parity: Device::refreshToken). No-op without a device.
  void RefreshJwt();

  void Logout();

  bool StartTunnel();  // build DeviceLocal, open tun, wire IoLoop, start connect VC
  void ConnectBestAvailable();
  // Connect to a chosen provider location (country/region/city/device/peer). The
  // chooser passes an SDK-supplied ConnectLocation as-is, or one built from a peer
  // (client id + display name). No-op with the tunnel down (no connect VC).
  void Connect(const std::optional<urnet::ConnectLocation>& location);
  void Disconnect();
  bool Connected();

  // Provide/earn: control mode "never"|"always"|"network"|"auto"|"manual".
  // "network" is the private provider: the provider is always on, but provides
  // ONLY to same-network peers — never publicly.
  void SetProvideControlMode(const std::string& mode);
  std::string GetProvideControlMode();
  bool ProvideEnabled();
  // The free -> Pro upgrade side effect (mac MainView reacts to
  // SubscriptionBalanceViewModel.didDetectUpgradeToPro by setting
  // DeviceManager.provideControlMode = .Never): stop providing and persist the
  // mode — the two writes handleProvideControlModeUpdate does; DeviceLocal
  // does not persist the control mode itself.
  void ResetProvideToNever();

  void SetAuthStateHandler(AuthStateHandler h) { onAuth_ = std::move(h); }
  void SetAuthInvalidHandler(AuthInvalidHandler h) { onAuthInvalid_ = std::move(h); }
  void SetJwtRefreshedHandler(JwtRefreshedHandler h) { onJwtRefreshed_ = std::move(h); }
  void SetConnectionStatusHandler(ConnectionStatusHandler h) { onStatus_ = std::move(h); }
  // Live stats push (connection/throughput/provide). Fired on SDK listener
  // callbacks; the UI marshals onto the GTK loop and gates on window visibility.
  void SetStatsHandler(StatsHandler h) { onStats_ = std::move(h); }
  LiveStats CurrentStats();  // snapshot on demand (e.g. resync when window shows)
  // Connect drawer change feed. The handler may be invoked while the SdkHost
  // lock is held — it must only marshal, never call back into SdkHost.
  void SetDrawerEventHandler(DrawerEventHandler h) { onDrawerEvent_ = std::move(h); }

  // ---- connect drawer accessors (all locked; graceful with no device) ------
  // The DeviceLocal exists only while the tunnel runs. Reads fall back to the
  // persisted LocalState where one exists so the drawer shows the restored
  // preferences; writes go to the device when present (the SDK persists the
  // blocker/dns/overrides itself) and to LocalState otherwise so the next
  // device creation restores them.
  std::optional<urnet::ConnectLocation> SelectedLocation();
  std::optional<urnet::PerformanceProfile> GetPerformanceProfile();
  // Persists to LocalState and applies to the device: unlike the other device
  // settings, DeviceLocal does not persist the profile itself (macOS parity).
  void SetPerformanceProfile(const std::optional<urnet::PerformanceProfile>& profile);
  bool GetBlockerEnabled();
  void SetBlockerEnabled(bool enabled);
  std::optional<urnet::DnsResolverSettings> GetDnsResolverSettings();
  void SetDnsResolverSettings(const urnet::DnsResolverSettings& settings);
  std::optional<urnet::ThroughputPointList> ThroughputPoints();
  int64_t ThroughputWindowSeconds();
  std::optional<urnet::BlockActionList> BlockActions();
  std::optional<urnet::BlockStats> BlockStatsSnapshot();
  std::optional<urnet::BlockActionOverrideList> BlockActionOverrides();
  void AddBlockActionOverride(const urnet::BlockActionOverride& override_);
  // Replaces the hosts of the override with the given id (full-list rebuild).
  void SetBlockActionOverrideHosts(const std::string& overrideId, const urnet::StringList& hosts);
  void RemoveBlockActionOverride(const std::string& overrideId);
  std::string ClientId();
  // Per-peer, per-contract rows for this device's own (client) traffic, straight
  // from the single-feed SDK ContractDetailsViewController. One row per peer client
  // id; each row carries its send + receive contracts (newest first) un-aggregated
  // and the two summed bit rates. The VC owns the direction-resolved grouping,
  // closing/eject lifecycle, rows-update throttle AND the FINAL display ordering --
  // the at-top activity sort plus the scrolled-away freeze -- so the sheet renders
  // the rows as-is (shared with apple/android). Report the scroll position with
  // SetContractsAtTop (true at the very top); ContractsPendingCount is the "N new"
  // count of rows collected while scrolled away (0 at the top). nullopt/0 with the
  // tunnel down. (Client + provider are two instances of the same single-feed VC;
  // only the client feed is wired -- a provider sheet would open its own VC via
  // openProviderContractDetailsViewController.)
  std::optional<urnet::ContractPeerRowList> ContractRows();
  void SetContractsAtTop(bool atTop);
  int64_t ContractsPendingCount();

  // ---- location/provider chooser ---------------------------------------------
  // LocationsViewController buckets provider locations into sections and owns the
  // search; PeerViewController surfaces the connected, provide-enabled network
  // peers pinned atop the chooser. Both live only while the tunnel runs; reads
  // return nullopt/empty otherwise.
  std::optional<urnet::FilteredLocations> GetFilteredLocations();
  void FilterLocations(const std::string& query);
  std::string GetFilteredLocationState();
  std::optional<urnet::NetworkPeerList> ConnectedProvidePeers();
  // count of ALL connected peers (online, provide or not)
  int64_t ConnectedPeerCount();

  // Exposed so the (full-parity) UI/view models can drive the SDK directly.
  urnet::Api& api() { return *api_; }
  bool hasDevice() { return device_.has_value(); }
  urnet::DeviceLocal& device() { return *device_; }

 private:
  urnet::NetworkSpace BuildNetworkSpace();
  void RegisterNetworkClient(const std::string& byJwt, std::function<void(AuthResult)> done);
  // Shared routing for NetworkCreateResult (sign-up + wallet sign-up).
  void HandleNetworkCreateResult(std::optional<urnet::NetworkCreateResult> result,
                                 std::optional<std::string> err,
                                 std::function<void(AuthResult)> done);
  // Tear down the device/tunnel/view-controllers without touching the stored
  // auth (Logout clears auth too; the guest upgrade only swaps the device).
  void TeardownDeviceLocked();
  void SetupWalletCallbacks();
  // blockchain: "solana" (ed25519, base64 signature) | urnet::TAO (sr25519, hex)
  void AuthLoginWithWallet(const std::string& address, const std::string& signature,
                           const std::string& message, const std::string& blockchain);
  void SubscribeStats();  // subscribe the live-stats listeners (in StartTunnel)
  void SubscribeDrawer();  // subscribe the connect-drawer listeners (in StartTunnel)
  void EmitDrawerEvent(DrawerEvent event);
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
  // single-feed per-peer per-contract rows for this device's own (client) traffic;
  // the VC owns the display ordering + scrolled-away freeze + "N new" pending count
  // (a provider sheet would open a second, provider-feed VC -- none exists yet)
  std::optional<urnet::ContractDetailsViewController> clientContractDetailsVc_;
  std::optional<urnet::BlockActionViewController> blockActionVc_;  // block actions/stats feed
  // sign-up network-name availability (SDK shared view controller)
  std::optional<urnet::NetworkNameValidationViewController> networkNameVc_;
  std::optional<urnet::LocationsViewController> locationsVc_;  // provider chooser feed
  std::optional<urnet::PeerViewController> peerVc_;  // connected provide-enabled peers
  std::optional<urnet::IoLoop> ioLoop_;
  std::unique_ptr<Tunnel> tunnel_;
  std::vector<urnet::Sub> subs_;

  WalletConnect wallet_;
  std::function<void(AuthResult)> walletAuthDone_;
  // The signed wallet_auth of a wallet sign-in with no network, carried into
  // CreateNetworkWithPendingWallet (android/apple route the same way).
  std::optional<urnet::WalletAuthArgs> pendingWalletAuth_;

  AuthStateHandler onAuth_;
  AuthInvalidHandler onAuthInvalid_;
  JwtRefreshedHandler onJwtRefreshed_;
  ConnectionStatusHandler onStatus_;
  StatsHandler onStats_;
  DrawerEventHandler onDrawerEvent_;
};

}  // namespace urnw
