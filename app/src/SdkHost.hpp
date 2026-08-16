// SdkHost — the UNPRIVILEGED core of the Linux GUI (the daemon-split model,
// linux/MIGRATION.md; the shape Apple and Windows ship). It owns the SDK's
// api/auth surface (NetworkSpace, Api, LocalState) in-process, but the VPN
// itself lives in urnetworkd: StartTunnel talks to the daemon over the unix
// control socket (ControlClient — connect → hello with version enforcement →
// start_tunnel) and then binds a urnet::DeviceRemote to the daemon's
// DeviceLocal over the SDK's loopback mTLS device RPC. Everything downstream
// (view controllers, listeners, the drawer accessors) runs against the shared
// Device interface exactly as before. This process never needs root and
// degrades cleanly with no daemon present (TunnelStartResult below).
//
// Callbacks fire on SDK background threads; the UI marshals them onto the GTK
// main loop.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <urnetwork_sdk.hpp>

#include "ControlClient.hpp"
#include "SecretServiceRpcSessionStore.hpp"
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
  DeviceLifecycle,  // device (remote) created or destroyed
  Throughput,       // new throughput points (charts)
  BlockActions,     // block action window changed
  BlockStats,       // allowed/blocked counts changed
  Overrides,        // block action overrides ("split rules") changed
  DnsSettings,      // dns resolver settings changed
  Blocker,          // block-ads-and-trackers toggle changed
  RouteLocal,       // routeLocal changed (the kill switch, inverted)
  Contracts,        // egress/ingress contract details changed
  Location,         // connect location changed
  Profile,          // performance profile changed
  Locations,        // filtered provider-location list changed (the chooser)
  Peers,            // connected network peers changed (chooser + drawer label)
  ProviderIdentities,  // post-quantum identity set changed (PQI panel + list)
  ProviderLocations,   // connected provider set/locations changed (locations sheet)
  ProviderSelection,   // the globe's selected provider changed (locations sheet)
};

// Outcome of StartTunnel. Everything except Started is a degraded state the
// UI must render DISTINCTLY and actionably (MIGRATION.md: "daemon
// unreachable" and "daemon too old" are never a blank or a zero — the same
// treatment as the RPC-hosted stats' gray "discovery disabled").
enum class TunnelStartResult {
  Started,
  DaemonUnreachable,  // urnetworkd not installed / not running / socket unauthorized
  DaemonTooOld,       // daemon control protocol below our supported minimum
  AppTooOld,          // daemon rejected OUR protocol: this app needs the update
  SdkMismatch,        // GUI and daemon SDK builds differ (the gob device rpc
                      // has no version field, so a drifted pair is refused at
                      // hello): update both to the same version
  Failed,             // daemon reachable but start failed (see LastTunnelError)
};

// Snapshot of live connection / throughput / provide stats. Pushed to the UI on
// SDK listener callbacks (macOS parity: listener-push, not polling). Shared shape
// with the Windows SdkHost.
struct LiveStats {
  std::string connectionStatus;
  bool connected = false;
  int64_t providerCount = 0;      // grid window current size
  // the live provider grid the hero canvas rides (empty = the bare lattice)
  std::vector<urnet::ProviderGridPoint> gridPoints;
  int64_t gridWidth = 0;
  int64_t gridHeight = 0;
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

  // Sign in with a BIP-39 seedphrase (macOS LoginSeedphraseView / windows
  // parity): authLogin{seedphrase}, normalized (lowercase, single-spaced)
  // before it leaves the process. Nothing may log the phrase.
  void LoginWithSeedphrase(const std::string& seedphrase,
                           std::function<void(AuthResult)> done);

  // Instant (seedphrase-only) account: networkCreate with nothing but the
  // terms consent mints a network whose only credential is a seedphrase. The
  // phrase is shown BEFORE the device is registered — Confirm registers the
  // held jwt, Discard drops it — so a dismissed sheet cannot leave a
  // signed-in account nobody can ever recover.
  struct InstantAccount {
    bool ok = false;
    std::string error;
    std::string seedphrase;  // the caller zeroes its copy after display
  };
  void CreateInstantAccount(std::function<void(InstantAccount)> done);
  void ConfirmInstantAccount(std::function<void(AuthResult)> done);
  void DiscardInstantAccount();

  // ---- network server (iOS NetworkServerSheet / windows parity) ------------
  // Which network API this client talks to — on this fork, the difference
  // between the official ur.network and a self-hosted deployment.
  struct NetworkServer {
    std::string hostName;
    std::string apiUrl;      // live, derived or overridden
    std::string connectUrl;  // live platform (connect) url
    // the EXPLICIT overrides in force, or empty when the urls are derived
    std::string configuredApiUrl;
    std::string configuredConnectUrl;
    // What "the default network" means for THIS process: the compiled-in
    // host, or URNETWORK_NETWORK_HOST when set — never silently production.
    std::string defaultHostName;
    bool managerAvailable = false;
  };
  NetworkServer CurrentNetworkServer();
  // Point the client at `hostName`, with optional explicit api/connect url
  // overrides (empty = derive from the host). Changes which LocalState — and
  // so which stored jwt — is in force: tears the live device down and
  // re-derives Api/LocalState. Offered from the SIGNED-OUT screen only.
  // Fires the auth-state handler with the new space's stored auth.
  bool ApplyNetworkServer(const std::string& hostName, const std::string& apiUrl,
                          const std::string& connectUrl);
  // The active space serialized for the daemon's start_tunnel: the daemon
  // must build its DeviceLocal in the SAME space or the DeviceRemote would
  // sync against a device registered in a different network ("" = default).
  std::string NetworkSpaceJson();

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

  // Quit-path teardown: bring the device and the daemon tunnel down WITHOUT
  // touching stored auth. Quitting the app is not signing out — Logout()'s
  // localState wipe deletes the jwt, and for a guest network the jwt is the
  // only credential, so quit-as-logout permanently destroys the account and
  // any balance it purchased.
  void Shutdown();

  // Daemon session: connect → hello (protocol enforced both ways) →
  // start_tunnel → bind the DeviceRemote to the daemon's device RPC. The
  // tunnel itself (DeviceLocal, tun fd, IoLoop) lives in urnetworkd.
  TunnelStartResult StartTunnel();
  // Human-readable detail for the last non-Started result ("" when none).
  std::string LastTunnelError();
  void ConnectBestAvailable();
  // Connect to a chosen provider location (country/region/city/device/peer). The
  // chooser passes an SDK-supplied ConnectLocation as-is, or one built from a peer
  // (client id + display name). No-op with the tunnel down (no connect VC).
  void Connect(const std::optional<urnet::ConnectLocation>& location);
  void Disconnect();
  bool Connected();
  // Own presentation-only SDK view controllers only while the GTK window is
  // visible. The DeviceLocal, tunnel and packet loop remain alive in the tray.
  void SetPresentationActive(bool active);

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

  // ---- Advanced Mode (the windows D5 standing-state contract) --------------
  // A STANDING STATE, not an event: loaded from app_prefs at startup into an
  // atomic (surfaces may build ~25s later), authority readable any time,
  // persist-FIRST-publish-second on write, and a replay call for late-built
  // surfaces. Bind-then-replay everywhere — change-notification-only provably
  // loses the restored-from-disk value.
  bool CurrentAdvancedMode();
  void SetAdvancedMode(bool on);
  void SetAdvancedModeHandler(std::function<void(bool)> h);
  void RefreshAdvancedMode();  // replay the current value to the handler

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
  // The device (remote) exists only while a tunnel session runs. Reads fall
  // back to the persisted LocalState where one exists so the drawer shows the
  // restored preferences; writes go to the device when present (forwarded
  // over the device rpc; the daemon side persists the blocker/dns/overrides)
  // and to LocalState otherwise so the next device creation restores them.
  std::optional<urnet::ConnectLocation> SelectedLocation();
  std::optional<urnet::PerformanceProfile> GetPerformanceProfile();
  // Persists to LocalState and applies to the device: unlike the other device
  // settings, DeviceLocal does not persist the profile itself (macOS parity).
  void SetPerformanceProfile(const std::optional<urnet::PerformanceProfile>& profile);
  bool GetBlockerEnabled();
  void SetBlockerEnabled(bool enabled);
  // Kill switch = !routeLocal (apple SettingsForm parity): with routeLocal
  // off the device DROPS tun-captured packets whenever no provider connection
  // is up, instead of falling back to local egress. Persisted in the GUI's
  // LocalState (the daemon's DeviceLocal neither persists nor restores it)
  // and re-applied over the device rpc at StartTunnel.
  bool GetRouteLocal();
  void SetRouteLocal(bool routeLocal);
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

  // ---- post quantum identity (PQI) -----------------------------------------
  // The device's own public identity key (+ its canonical 52-char display
  // hash) and the providers with an established, identity-verified e2e
  // session, through the SDK's shared PostQuantumIdentityViewController (the
  // apple PostQuantumIdentityStore binds the same one). The VC lives only
  // while the tunnel runs; reads return empty/nullopt otherwise. Changes
  // arrive as DrawerEvent::ProviderIdentities.
  std::optional<urnet::ProviderIdentityList> ProviderIdentities();
  std::string PublicIdentityKeyHash();
  std::vector<uint8_t> PublicIdentityKey();

  // ---- connected provider locations ------------------------------------------
  // Where each provider in the current connect window is, in the SDK's shared
  // DISPLAY ORDER: west to east about the providers' centroid, then the ones
  // with no coordinates. That is the order the list renders and the order the
  // globe's wheel steps through. It is NOT sorted by connected duration, so the
  // location override finds its target by stamp (OldestPlottableIndex) rather
  // than by taking the first row. Read from the provider-locations view
  // controller, so the rows and the selection always come from one snapshot;
  // nullopt with the tunnel down. Changes arrive as DrawerEvent::ProviderLocations
  // -- the listener is signal-only, so re-read the getter on every notification.
  std::optional<urnet::ConnectedProviderLocationList> ConnectedProviderLocations();
  // Drops a provider from the connection by its EGRESS client id and excludes it
  // from re-discovery for the rest of this connection. No-op with no device.
  void RemoveConnectedProvider(const std::string& clientId);

  // ---- the globe's selection and scroll wheel --------------------------------
  // The SDK's shared ProviderLocationsViewController, which every URnetwork app
  // binds so they all traverse the globe identically. StepProviderSelection
  // moves along the plottable providers ordered west to east relative to their
  // centroid and CLAMPS at the ends: stepping past the extreme west or east
  // sticks there instead of cycling round the globe. Changes arrive as
  // DrawerEvent::ProviderSelection; "" means nothing is selected.
  std::string SelectedProviderClientId();
  void SetSelectedProviderClientId(const std::string& clientId);
  void StepProviderSelection(int steps);

  // Exposed so the (full-parity) UI/view models can drive the SDK directly.
  urnet::Api& api() { return *api_; }
  bool hasDevice() { return device_.has_value(); }
  urnet::DeviceRemote& device() { return *device_; }
  // The daemon control channel, shared with the location-override writer
  // (DaemonGeoClueWriter) — one socket, one hello, one version check.
  ControlClient& Control() { return control_; }

 private:
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
  void SubscribeStats();   // caller holds mutex_; opens presentation controllers
  void SubscribeDrawer();  // caller holds mutex_; opens presentation controllers
  void ClosePresentationLocked();
  void EmitDrawerEvent(DrawerEvent event);
  LiveStats ReadStats();  // read the current snapshot from the SDK getters
  void PublishStats();    // ReadStats() -> onStats_

  std::mutex mutex_;
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::Api> api_;
  std::optional<urnet::AsyncLocalState> asyncLocalState_;
  std::optional<urnet::LocalState> localState_;
  // The remote face of the daemon's DeviceLocal. Exists only while a tunnel
  // session was successfully started; every accessor below falls back to
  // LocalState without it, exactly as before the split.
  std::optional<urnet::DeviceRemote> device_;
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
  // post quantum identity feed: own identity key hash + verified provider identities
  std::optional<urnet::PostQuantumIdentityViewController> pqiVc_;
  // the provider globe's selection + scroll wheel, shared across every app
  std::optional<urnet::ProviderLocationsViewController> providerLocationsVc_;
  // GUI-only system keyring backend. Private RPC credentials never enter the
  // metadata file or the privileged daemon.
  SecretServiceRpcSessionStore rpcSecretStore_;
  // control channel to urnetworkd (tunnel lifecycle + location override)
  ControlClient control_;
  std::string storageDir_;
  std::string lastTunnelError_;
  // The network-provide-key bit, cached off addProvideSecretKeysListener:
  // DeviceRemote has no getProvideSecretKeys getter (it is DeviceLocal-only),
  // so like the Windows GUI the listener feeds this atomic and ReadStats
  // reads it.
  std::atomic<bool> provideHasNetworkKey_{false};
  bool presentationActive_ = false;
  std::vector<urnet::Sub> subs_;
  std::vector<urnet::Sub> presentationSubs_;

  WalletConnect wallet_;
  // Guarded by mutex_: set on the UI thread (SignInWithSolana/Bittensor),
  // consumed on wallet deep-link and SDK callback threads (on_error /
  // AuthLoginWithWallet) — always taken under the lock, invoked outside it.
  std::function<void(AuthResult)> walletAuthDone_;
  // The signed wallet_auth of a wallet sign-in with no network, carried into
  // CreateNetworkWithPendingWallet (android/apple route the same way).
  std::optional<urnet::WalletAuthArgs> pendingWalletAuth_;
  // The instant account's jwt, held between CreateInstantAccount and the
  // seedphrase sheet's confirm (guarded by mutex_; a secret — never log it).
  std::optional<std::string> pendingInstantJwt_;

  // Advanced Mode standing state (D5): the atomic is the authority between
  // the disk read at startup and any later toggle.
  std::atomic<bool> advancedMode_{false};
  bool advancedModeLoaded_ = false;
  std::function<void(bool)> onAdvancedMode_;

  AuthStateHandler onAuth_;
  AuthInvalidHandler onAuthInvalid_;
  JwtRefreshedHandler onJwtRefreshed_;
  ConnectionStatusHandler onStatus_;
  StatsHandler onStats_;
  DrawerEventHandler onDrawerEvent_;
};

}  // namespace urnw
