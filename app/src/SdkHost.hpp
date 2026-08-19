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
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <urnetwork_sdk.hpp>

#include "ControlClient.hpp"
#include "Health.hpp"
#include "RpcSession.hpp"
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

// THE ONE CONNECT READING — every fact the connect status row is allowed to
// depend on, sampled TOGETHER from the live SDK getters at one instant.
//
// It exists because the status row used to be written from three independently
// aged copies of the same underlying bit (see Health.hpp). Nothing here is a
// cached copy of anything else: ReadConnectReading() re-reads all of it every
// time, so no field of one reading can describe a different moment than
// another field of the same reading, and no reading can outlive its producer.
struct ConnectReading {
  // The connect controller's OWN status, latched to the last KNOWN value for
  // the life of the session (a freshly reopened controller reports
  // Disconnected until its first window-monitor event). Unknown = no status
  // has ever landed for this session.
  health::SdkStatus sdk = health::SdkStatus::Unknown;
  // The raw token behind `sdk`, for the Advanced status strip only. Never a
  // decision input — decisions read `sdk`.
  std::string rawStatus;
  // ConnectViewController::GetConnected(): a destination is SELECTED.
  bool destinationSelected = false;
  // A DeviceRemote is still bound over the CURRENT control session.
  bool tunnelBound = false;
  int64_t providerCount = 0;  // grid.getWindowCurrentSize()
  bool insufficientBalance = false;

  // Value equality, so a consumer can skip a rebuild when nothing moved. It
  // compares EVERY field on purpose: a partial comparison would be one more
  // place that can decide two different readings are the same one.
  bool operator==(const ConnectReading& o) const {
    return sdk == o.sdk && rawStatus == o.rawStatus &&
           destinationSelected == o.destinationSelected && tunnelBound == o.tunnelBound &&
           providerCount == o.providerCount && insufficientBalance == o.insufficientBalance;
  }
  bool operator!=(const ConnectReading& o) const { return !(*this == o); }

  health::Signals ToSignals(bool disconnectRequested) const {
    health::Signals s;
    s.sdk = sdk;
    s.destinationSelected = destinationSelected;
    s.tunnelBound = tunnelBound;
    s.providerCount = providerCount;
    s.insufficientBalance = insufficientBalance;
    s.disconnectRequested = disconnectRequested;
    return s;
  }
};

// ONE consistent reading of the SDK's smart-routing (reliability) state: the
// exit window, the destination->exit routing table, the knob set, the counters
// and the probe suite. Taken under a single SdkHost lock hold so the parts can
// never describe different sessions — separate reads can straddle a device
// teardown and then join a destination ip against an exit that belonged to a
// device which no longer exists.
//
// EVERY field distinguishes UNKNOWN from a real answer, because on this
// surface a fabricated zero is the failure mode:
//   * `settings`/`metrics` nullopt = "nothing was read". It is NOT "every knob
//     is off / every counter is zero". A default-constructed ReliabilitySettings
//     written back would disable the whole reliability stack (that bug shipped
//     once on Windows) — never round-trip a nullopt read as a struct.
//   * the three list fields nullopt = "never read, or the getter threw"; an
//     EMPTY list is a real answer ("this device has no exits"). A caller must
//     render the two differently — "unknown" and "none" are different facts.
//   * the two bools have no third state to carry: a read that threw reads as
//     false, which is why the getter failure is also logged (g_warning).
struct ReliabilitySnapshot {
  bool haveDevice = false;       // a DeviceRemote existed when the read ran
  bool remoteConnected = false;  // the daemon's device rpc is attached
  std::optional<urnet::ReliabilitySettings> settings;
  std::optional<urnet::ReliabilityMetrics> metrics;
  std::optional<urnet::ExitList> exits;
  std::optional<urnet::DestinationExitList> destinationExits;
  bool probeSuiteRunning = false;
  std::optional<urnet::ProbeResultList> probeResults;
};

// How much of the snapshot to pay for. Each field is one SYNCHRONOUS device
// rpc, so the scope is the difference between a 3-rpc poll and a 7-rpc one.
enum class ReliabilityRead {
  ExitsOnly,  // remoteConnected + the two exit tables (Home's Advanced inspector)
  Full,       // + settings/metrics/probe suite (the Developer destination)
};

// ---- the kill switch -------------------------------------------------------
// What the three toggles now drive. THREE legs, in this order
// (docs/parity/settings.md §113, windows SdkHost::SetKillSwitch):
//
//   1. LocalState  routeLocal = !on   — the persistent truth; survives a crash
//                                       and is what the next start_tunnel and
//                                       the next device creation replay.
//   2. DeviceRemote routeLocal = !on  — the SDK's SOFT leg, over the device
//                                       rpc: a branch inside sendPacket, so it
//                                       never sees IPv6, the deliberately
//                                       route-excluded LAN, another adapter's
//                                       resolver, or a dead daemon.
//   3. urnetworkd  set_kill_switch(on) — the ENFORCEMENT leg: the nftables
//                                       ruleset (`table inet urnetwork`). This
//                                       is the leg the toggles never had, and
//                                       without it the UI claimed a protection
//                                       that was not in force.
//
// Leg 3 is a blocking control-socket round trip (up to 30 s against a wedged
// daemon), so it runs on a worker thread; legs 1 and 2 stay on the caller's
// thread so a re-read right after the call already reflects them.
//
// The daemon reports what it ACTUALLY installed. Never assume the request
// took, and never render Failed as Off.
struct KillSwitchStatus {
  // The standing preference (== !routeLocal). This is what the toggle shows.
  bool requested = false;
  // What the daemon says is installed. Meaningful ONLY when installed_known.
  ctl::KillSwitchState installed = ctl::KillSwitchState::Off;
  // false => the daemon did not answer, so `installed` is a default and NOT a
  // fact. "Unknown" and "off" are different states and must render differently.
  bool installed_known = false;
  // A floor is really up (Armed or Connected).
  bool in_force = false;
  // The daemon's own tunnel state, so the UI can tell the daemon's DELIBERATE
  // "requested, nothing connected, so nothing is blocked yet" (TunnelHost::
  // SetKillSwitch refuses to cut a machine off that never connected) apart
  // from a real "the tunnel is up and the floor is missing".
  ctl::TunnelState tunnel_state = ctl::TunnelState::Stopped;
  // How the control channel stands — the UI maps this to remediation copy
  // (not installed / not running / not authorized / version skew).
  DaemonSessionState session = DaemonSessionState::Unreachable;
  // WHY the channel is Unreachable. Meaningful only while
  // session == Unreachable, and the reason the UI can stop saying "the service
  // is not running" at a socket that is right there and merely refuses this
  // user (EACCES — the installers create the `urnetwork` group empty, so this
  // is the state a FRESH INSTALL lands in). See DaemonUnreachableReason.
  DaemonUnreachableReason unreachable_reason = DaemonUnreachableReason::None;
  // The daemon's kill_switch_detail, or the transport error. "" when there is
  // nothing to explain. NOT localized: it is a daemon string, and the UI pairs
  // it with its own localized lead sentence.
  std::string detail;
  // A leg-3 write is still in flight. The toggle stays where the user put it
  // and the state line says so — it must never flap.
  bool pending = false;
};

// The ONE classification the three surfaces share, so they cannot disagree
// about what the same status means. Pure.
//
// THREE STATES THAT USED TO BE ONE. `installed_known == false` used to fall
// into NotInForce, whose copy asserts "your traffic is not being blocked" and
// whose remediation says the service is not running. Both are claims, and in
// the state that matters most they are the OPPOSITE of the truth: the nftables
// table is not process-bound, so a floor installed by an earlier session is
// still blocking this machine when the daemon is gone, and a daemon that is
// running but merely refuses THIS user (EACCES) is not a daemon that is
// stopped. "Cannot tell" is now its own state and says so.
enum class KillSwitchDisplay {
  Off,       // not requested, and nothing is installed: nothing to say
  Applying,  // a leg-3 write is in flight, in EITHER direction
  InForce,   // requested AND a floor is installed
  // NOT requested and a floor is installed anyway — a removal that failed, or
  // a floor re-armed from the crash marker under a switch the user turned off.
  // The user is CUT OFF while the control reads "off"; this is the state the
  // UI said nothing at all about.
  InForceUnrequested,
  ArmedAtNextStart,  // requested, no tunnel: the daemon deliberately holds off
  // requested, the daemon ANSWERED, and it installed nothing while the tunnel
  // is up. A real defect, and the only state entitled to claim "not blocked".
  NotInForce,
  // requested, and the daemon could not be asked at all. NOT "off": what is
  // installed is genuinely unknown, and the user may be cut off by our own
  // floor right now. Carries the recovery command, because the app cannot lift
  // a floor it cannot reach the daemon to lift.
  Unknown,
  Failed,  // requested, the daemon TRIED and could not — never render as Off
};

inline KillSwitchDisplay ClassifyKillSwitch(const KillSwitchStatus& s) {
  // A write in flight outranks everything, in either direction: the last
  // reading describes a state the daemon is in the middle of leaving. (This
  // deliberately also covers !requested — turning the switch OFF is a write
  // that can fail, and it used to render as silence.)
  if (s.pending) return KillSwitchDisplay::Applying;
  if (!s.requested) {
    // Silence is correct ONLY when the daemon has told us nothing is up.
    // A floor still standing under an off switch is not a nuance, it is the
    // user's network being blocked with no explanation on screen.
    if (s.installed_known && s.in_force) return KillSwitchDisplay::InForceUnrequested;
    return KillSwitchDisplay::Off;
  }
  if (s.installed_known && s.installed == ctl::KillSwitchState::Failed) {
    return KillSwitchDisplay::Failed;
  }
  if (s.in_force) return KillSwitchDisplay::InForce;
  // The daemon could not be asked. Unknown, never "not in force".
  if (!s.installed_known) return KillSwitchDisplay::Unknown;
  // The daemon answered "off" with the switch on. That is EXPECTED while
  // nothing is connected — switching it on with no tunnel must not cut the
  // machine off the network — and a defect once the tunnel is up.
  if (s.tunnel_state != ctl::TunnelState::Up) return KillSwitchDisplay::ArmedAtNextStart;
  return KillSwitchDisplay::NotInForce;
}

class SdkHost {
 public:
  ~SdkHost();

  using AuthStateHandler = std::function<void(bool loggedIn)>;
  // Fired when the sdk finds the stored auth is no longer valid on the server
  // (e.g. the client was removed): the sdk has already cleared its local auth
  // state. The handler runs on an sdk thread and must only marshal -- the ui
  // marshals onto the main loop and calls Logout().
  using AuthInvalidHandler = std::function<void()>;
  using JwtRefreshedHandler = std::function<void()>;
  // The ONE connection feed. It replaced a string push whose five call sites
  // could only ever emit "DESTINATION_SET" or "DISCONNECTED" — a vocabulary
  // with no word for "connected" — beside a separate, visibility-gated stats
  // push that carried the real status and was never read for the status row.
  using ConnectReadingHandler = std::function<void(ConnectReading reading)>;
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
  // The connection feed. Fired on every event that can change any part of the
  // reading, and NEVER gated on window visibility by its consumer: the copies
  // this replaced diverged precisely because one of them was gated and the
  // other was not.
  void SetConnectReadingHandler(ConnectReadingHandler h) { onReading_ = std::move(h); }
  // Snapshot on demand. Used on window re-show and by the page's 1 Hz poll, so
  // no part of the reading can persist on screen after its producer goes quiet.
  // Locked, exactly as the SdkHost::Connected() it replaced was — the callers
  // are UI-thread callers that already take this lock through the other feed
  // accessors (BlockActions, SelectedLocation, ContractRows).
  ConnectReading CurrentConnectReading();
  // The daemon reported the tunnel gone. Latches until the next start_tunnel;
  // a plain field on a copied reading is self-reverting, because the next SDK
  // push re-derives tunnelBound from getters that cannot see the daemon.
  void NoteDaemonTunnelGone();
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
  //
  // READ ONLY, and only the SOFT leg. Every UI writer must go through
  // SetKillSwitch below instead — a bare setRouteLocal leaves the daemon's
  // nftables floor untouched, which is precisely the bug this replaces.
  bool GetRouteLocal();

  // ---- kill switch (the three legs; see KillSwitchStatus above) ------------
  using KillSwitchDone = std::function<void(KillSwitchStatus)>;

  // The standing preference, no daemon I/O: !routeLocal, device-preferred and
  // falling back to LocalState (parity rule: with neither, claim the
  // PERMISSIVE default, never the strict one). Readable signed out, with no
  // tunnel and with no daemon — this is the toggle's position.
  bool CurrentKillSwitch();
  // The last snapshot published by a write or a read-back. No I/O: safe on the
  // GTK main loop and safe to call from a build path before any daemon
  // round trip has happened (installed_known is then false).
  KillSwitchStatus CurrentKillSwitchStatus();

  // Legs 1+2 SYNCHRONOUSLY (so an immediate CurrentKillSwitch() read-back
  // already reflects them), then leg 3 on a worker thread. `done` runs ON THE
  // GTK MAIN LOOP exactly once with the state READ BACK from the daemon after
  // the write — not with the value that was asked for. It may land after the
  // caller was destroyed, so `done` must carry its own epoch/liveness guard,
  // the same contract as RequestReliability.
  //
  // Requests are queued and served in order: a rapid double-toggle costs two
  // round trips and the LAST one wins. Nothing is ever dropped — a dropped
  // kill-switch write is a machine left in a state nobody asked for.
  void SetKillSwitch(bool on, KillSwitchDone done = {});
  // Read-back only: no write, same completion contract. Call it after a
  // tunnel state change (the floor moves between Armed and Connected on its
  // own) and when a surface comes back on screen.
  void RefreshKillSwitchStatus(KillSwitchDone done = {});
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

  // ---- reliability / exits (Home's Advanced inspector + the Developer page) --
  // The locked, BLOCKING read. Every field behind it is a synchronous device
  // rpc over the loopback mTLS channel to urnetworkd — three for ExitsOnly,
  // seven for Full — and the whole batch runs under mutex_, the same lock the
  // UI-thread accessors take. So:
  //
  //   NEVER call this on the GTK main loop.
  //
  // Call it only from a thread that already exists for SDK work (the Developer
  // page's serial FIFO bridge), or use RequestReliability below, which owns
  // the thread and the marshal for you. Each getter is guarded individually: a
  // throwing rpc costs its own field (which then reads as UNKNOWN), never the
  // whole snapshot. No device is not an error — the snapshot then carries
  // haveDevice=false, a DEFINITE "no session" the caller can render, rather
  // than silence.
  ReliabilitySnapshot ReadReliability(ReliabilityRead scope = ReliabilityRead::Full);

  // The GTK-safe form: runs ONE ReadReliability(scope) on a worker thread and
  // delivers the snapshot to `done` ON THE MAIN LOOP (PostToMain).
  //
  // SINGLE-FLIGHT for the whole host and across both scopes: while a read is
  // outstanding this returns false IMMEDIATELY and `done` is never invoked, so
  // a poll whose read is slower than its own interval cannot stack requests
  // behind mutex_ — the caller simply skips that tick and asks again on the
  // next one. Returns true when the read was started, and then `done` runs
  // exactly once unless the main loop is gone by the time it lands.
  //
  // Call from the main loop. `done` must carry its OWN liveness/epoch guard:
  // the completion can land after the calling page was destroyed, and this
  // host has no way to know that.
  bool RequestReliability(ReliabilityRead scope,
                          std::function<void(ReliabilitySnapshot)> done);

  // Exposed so the (full-parity) UI/view models can drive the SDK directly.
  urnet::Api& api() { return *api_; }
  // "There is a session I can drive", NOT "I am holding a handle".
  //
  // A urnet::DeviceRemote handle belongs to this process and nothing
  // invalidates it when the daemon-side DeviceLocal disappears (a service
  // restart or reinstall, another client's stop_tunnel, the IoLoop ending).
  // Reporting the handle alone is what let MainWindow::ToggleConnect skip the
  // start path entirely and drive a dead rpc: no start_tunnel was ever sent,
  // no error was ever produced, and the daemon journal stayed empty.
  //
  // So it is also gated on the control session being the SAME one the device
  // was bound over. That is an O(1) atomic read — no daemon round trip — so
  // this stays callable from the GTK main loop and from every page that folds
  // on it. False from here means "ask StartTunnel", and StartTunnel does the
  // authoritative check (a `status` round trip) before it rebuilds anything.
  bool hasDevice() {
    return device_.has_value() && deviceControlGeneration_ == control_.SessionGeneration();
  }
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
  // The body of StartTunnel(). Requires mutex_, so the recovery inside
  // ConnectBestAvailable can rebuild a session it has just discovered is dead
  // without re-entering a non-recursive lock. Every outcome logs, and the
  // failing ones leave a renderable sentence in lastTunnelError_.
  TunnelStartResult StartTunnelLocked();
  // ---- the two doors onto a tunnel that is ALREADY UP ----------------------
  // Door 1. Loads the remembered session (metadata from disk, the mTLS client
  // key and the pinned cert from the Secret Service) and, when it still
  // describes the tunnel the daemon reports, re-adopts it with attach_tunnel
  // instead of building a new one.
  //
  // nullopt means THE DOOR DID NOT OPEN — no record, an unreadable one, a
  // locked keyring, a record for a session that is not the live one, or a
  // daemon that refused the attach — and StartTunnelLocked must fall back to a
  // fresh start_tunnel, which is always available and is why none of those
  // failures can leave the user unable to connect. A value means the door was
  // taken and is the whole result of the start. Requires mutex_.
  std::optional<TunnelStartResult> TryAttachRememberedSessionLocked(
      const std::string& clientJwt, const ctl::StatusReply& status);
  // The DeviceRemote half, shared by BOTH doors: the 12025 reservation, the
  // pinned construction, the listeners and the bind watchdog. On failure it has
  // already torn down every partial resource and stopped the daemon-side
  // tunnel. `rememberOnSync` arms the session to be written to the store on the
  // first proof it works (false on the attach door, whose record is already
  // stored). Requires mutex_.
  TunnelStartResult BindRemoteDeviceLocked(const std::string& clientJwt,
                                           const RpcSessionRecord& session,
                                           bool rememberOnSync);
  // Commits the armed session once the pinned DeviceRemote reports
  // remote_connected. Best-effort by design — see the definition. Requires
  // mutex_.
  void RememberSyncedSessionLocked();
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
  // Re-reads EVERY field from the live SDK getters. Takes no lock (same
  // contract as ReadStats): it is called from SDK listener threads, from the
  // GTK loop, and from inside StartTunnelLocked with mutex_ already held.
  ConnectReading ReadConnectReading();
  void PublishConnectReading();  // ReadConnectReading() -> onReading_

  // ---- kill switch internals ------------------------------------------------
  // Requires mutex_. The soft legs, in the parity order: LocalState FIRST
  // (persistent truth), then the device.
  void ApplyRouteLocalLocked(bool routeLocal);
  // Requires mutex_. !routeLocal, device-preferred, LocalState fallback.
  bool KillSwitchRequestedLocked();
  struct KillSwitchRequest {
    bool apply = false;  // false = read-back only
    bool wanted = false;
    KillSwitchDone done;
  };
  void EnqueueKillSwitch(KillSwitchRequest request);
  void RunKillSwitchRequest(KillSwitchRequest request);  // ON THE WORKER
  void KillSwitchWorkerMain();
  void StopKillSwitchWorker();  // destructor: drain + join

  // ---- device rpc mTLS ------------------------------------------------------
  // The construction + setRpcServer pair lives INSIDE StartTunnel's existing
  // try on purpose (a throw from malformed material must land in the catch
  // that already tears down and stops the daemon-side tunnel), so there is no
  // separate Pin* helper. These three bound the case nothing throws for.
  void ArmRpcBindWatchdogLocked();     // requires mutex_
  void CancelRpcBindWatchdogLocked();  // requires mutex_; also bumps the epoch
  void OnRpcBindDeadline();            // MAIN LOOP; epoch-guarded

  // THE UNPINNED DIAL WINDOW, AND WHY THERE IS A SOCKET IN THIS CLASS.
  //
  // urnet::newDeviceRemoteWithDefaults is the ONLY DeviceRemote constructor
  // the shipped binding exposes (urnetwork_sdk.hpp:19882 — and the .so exports
  // exactly one such symbol, urnet_new_device_remote_with_defaults). It builds
  // its dialer with EMPTY clientPem/serverCertPem against the SDK's built-in
  // ctl::kDeviceRpcPort address (sdk/device_rpc.go:290 over
  // deviceRpcDefaultAddress, :117) and starts the dial goroutine before it
  // returns (:487). setRpcServer cannot run first — there is no object yet —
  // and when it does run it blocks on the state lock the constructor left held
  // for InitialLockTimeout (1 s, :134). So the plain-ws dial is not a race: it
  // ALWAYS happens, two or three times, on every DeviceRemote we build.
  //
  // We cannot make the first dial pinned. We CAN make sure it has nowhere to
  // land: bind 127.0.0.1:<kDeviceRpcPort> ourselves and never listen() on it,
  // which reserves the address against every other process and makes each
  // connect() to it fail immediately with ECONNREFUSED. Holding it FAILS the
  // start when it cannot be taken, because the alternative is handing the
  // occupant this device's rpc session — and, once synced, the account bearer
  // token, since the DeviceRemote proxies the Api's authenticated HTTP over
  // that same connection (sdk/device_rpc.go:437-438).
  //
  // NOT listen()ing is deliberate: a listening socket we never accept() from
  // parks the dial in the accept queue until RpcConnectTimeout (30 s), and
  // that dial holds the lock setRpcServer needs — a 30-second freeze of the
  // GTK main loop instead of an instant refusal.
  //
  // Requires mutex_. Idempotent; once taken the reservation is held for the
  // life of the process (each new DeviceRemote reopens the same window).
  bool HoldDeviceRpcDefaultPortLocked(std::string* error);
  void ReleaseDeviceRpcDefaultPort();

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
  // control channel to urnetworkd (tunnel lifecycle + location override)
  ControlClient control_;
  std::string lastTunnelError_;
  // The network-provide-key bit, cached off addProvideSecretKeysListener:
  // DeviceRemote has no getProvideSecretKeys getter (it is DeviceLocal-only),
  // so like the Windows GUI the listener feeds this atomic and ReadStats
  // reads it.
  std::atomic<bool> provideHasNetworkKey_{false};
  // RequestReliability's worker. Two guards, deliberately separate:
  //   * reliabilityBusy_ is the single-flight gate and is cleared BY THE WORKER
  //     as soon as the read returns — lock-free, and never under the mutex
  //     below (the joiner holds that one, so taking it on the worker would
  //     deadlock). Clearing it before the marshal also means a blocked or
  //     vanished main loop cannot wedge the next read.
  //   * reliabilityWorkerMutex_ guards ONLY the thread object: assigning over
  //     a still-joinable std::thread is std::terminate.
  std::atomic<bool> reliabilityBusy_{false};
  std::mutex reliabilityWorkerMutex_;
  std::thread reliabilityWorker_;

  // ---- kill switch ----------------------------------------------------------
  // The last published snapshot (guarded by mutex_). Seeded requested-only at
  // construction, so a surface built before any daemon round trip reads
  // installed_known=false — UNKNOWN, which is the honest answer, rather than a
  // fabricated "off".
  KillSwitchStatus killSwitchStatus_;
  // Leg 3 lives on ONE serial worker with a FIFO queue, deliberately not the
  // single-flight gate the reliability read uses: a skipped reliability read
  // costs a stale pane, a skipped kill-switch write costs a machine in a state
  // nobody asked for. The worker is started lazily and joined in ~SdkHost.
  std::mutex killSwitchMutex_;
  std::condition_variable killSwitchCv_;
  std::deque<KillSwitchRequest> killSwitchQueue_;
  std::thread killSwitchWorker_;
  bool killSwitchQuit_ = false;
  // Writes accepted but not yet answered. A read-back that was already in
  // flight when a write was issued must not publish pending=false and let the
  // line claim the PREVIOUS request's floor for a round trip — on this control
  // a momentary "in force" over a pending "turn it off" is exactly the lie
  // being removed.
  std::atomic<int> killSwitchWritesPending_{0};

  // ---- device rpc mTLS ------------------------------------------------------
  // Bumped on every DeviceRemote construction and on every teardown; the bind
  // watchdog carries the value it was armed with and does nothing when it no
  // longer matches, so a completed-then-restarted session cannot tear down its
  // successor.
  std::atomic<uint64_t> rpcSessionGeneration_{0};
  unsigned int rpcBindWatchId_ = 0;        // g_timeout source id; 0 = unarmed
  uint64_t rpcBindWatchGeneration_ = 0;    // the session the armed watchdog belongs to
  std::string rpcHostPort_;                // what THIS session dialed ("" when none)
  // A session that has been bound but has NOT yet proved it works. Written to
  // the store (disk metadata + Secret Service secrets) by
  // RememberSyncedSessionLocked on the first remote_connected edge, and dropped
  // on teardown — so nothing is ever remembered that did not demonstrably
  // pair, and a pairing that silently mismatched cannot be offered to the next
  // launch as something to attach to. Empty on the attach door: that record is
  // already stored.
  std::optional<RpcSessionRecord> unsavedSession_;
  // ControlClient::SessionGeneration() at the moment device_ was bound. A
  // different value now means the control connection was rebuilt — the daemon
  // restarted — so the DeviceLocal this device_ talks to is gone. Atomic
  // because hasDevice() reads it without mutex_.
  std::atomic<uint64_t> deviceControlGeneration_{0};
  // The reservation described by HoldDeviceRpcDefaultPortLocked: a bound,
  // NEVER-listening socket on 127.0.0.1:<ctl::kDeviceRpcPort>. -1 = not held.
  int deviceRpcDefaultPortFd_ = -1;
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
  ConnectReadingHandler onReading_;
  // The last KNOWN connect-controller status of the CURRENT session, held as
  // the enum's integer value. A controller reopened on window re-show reports
  // Disconnected until its first window-monitor event
  // (connect_view_controller.go:89), and letting that regress the reading
  // would flash "Connecting to providers" across a carrying tunnel every time
  // the window is shown. It is SESSION-scoped, not sticky: ReadConnectReading
  // clears it the moment the session stops being up, so it can never outlive
  // the thing it describes.
  std::atomic<int> lastKnownSdk_{static_cast<int>(health::SdkStatus::Unknown)};
  // The daemon stopped the tunnel underneath us (PollDaemonHealth saw it).
  // Forces tunnelBound false in EVERY subsequent reading until a new
  // start_tunnel clears it — see ReadConnectReading. Atomic because the poll
  // runs on the GTK loop and readings are published from SDK threads.
  std::atomic<bool> daemonTunnelGone_{false};
  StatsHandler onStats_;
  DrawerEventHandler onDrawerEvent_;
};

}  // namespace urnw
