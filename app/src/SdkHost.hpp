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

#include "ClientEvents.hpp"
#include "ControlClient.hpp"
#include "ExtenderReset.hpp"
#include "LogUpload.hpp"
#include "Health.hpp"
#include "ProvideLifecycle.hpp"
#include "RowConnectCoalescer.hpp"
#include "RpcSession.hpp"
#include "SignOut.hpp"
#include "VerifySendNotice.hpp"
#include "AddSignInFlow.hpp"
#include "AuthLogout.hpp"
#include "BittensorWalletFlow.hpp"
#include "WalletBridgeRoute.hpp"
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
  // Google/Apple (the provider's web flow) authenticated but the identity has
  // no network yet: the host kept the identity token (see
  // CreateNetworkWithPendingSso) and the UI routes into the create-network
  // page the same way.
  bool sso_needs_network = false;
  // The account exists under other sign-in methods (comma-joined), so this
  // attempt cannot sign it in; the UI names them (login_error_auth_allowed).
  std::string authAllowed = {};
  bool sso = false;  // the outcome of an sso attempt (its generic error copy)
  // With verification_required: whether the server sent the code (its
  // send_error); the verify page must not say a code was sent otherwise.
  VerifySendNotice sendNotice = {};
  // A wallet refusal's code (signature_mismatch: a pasted signature from
  // another account than the entered address) and the Bittensor wallet that
  // signed ("" for none); the page words it (WalletProofRefusalText).
  std::string errorCode = {};
  std::string bittensorWalletId = {};
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
  TransportSettings,          // client transport policy changed (transport bar footer + editor)
  ProviderTransportSettings,  // provider transport policy changed
  Blocker,          // block-ads-and-trackers toggle changed
  RouteLocal,       // routeLocal changed (the kill switch, inverted)
  Contracts,        // egress/ingress contract details changed
  Location,         // connect location changed
  Profile,          // performance profile changed
  Locations,        // filtered provider-location list changed (the chooser)
  Peers,            // connected network peers changed (chooser + connect page)
  ProviderIdentities,  // post-quantum identity set changed (identities list + badges)
  ProviderLocations,   // connected provider set/locations changed (locations sheet)
  ProviderSelection,   // the globe's selected provider changed (locations sheet)
  ExtenderStatus,      // extender directory / gossip status changed (connect page panel)
  // this device's own extender role changed state or setting (the connect
  // page's extender row, the earnings page's read-only row and statistics)
  ExtenderProvideStatus,
  // this device's provider status: a poll landed or failed, a stop dropped
  // one in flight, or the controller opened or closed (the earnings page's
  // reason line, demand histogram and ranking numbers)
  ProviderStatus,
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
  // provideClients could not be read: the provider-only device's count, from
  // a daemon that predates it or a read that failed. provideClients is then 0
  // and no count is shown. Never true with a DeviceRemote, whose peers are
  // read directly.
  bool provideClientsUnknown = false;
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
  // The degrade hold's verdict on this session (health::DegradeHold), so the
  // page, the strip and the tray read one verdict.
  health::ProofLoss proofLoss = health::ProofLoss::None;
  // A controller status is in hand: the presentation is open, or this session
  // latched one before it closed. False for a session started while the
  // window was hidden, where the tray keeps the session's own claim.
  bool statusObserved = false;
  // The SDK's diagnosis of the forming window (WindowStatus.StallReason),
  // read only while it can matter: a session up, the controller not saying
  // CONNECTED, the presentation open. "" otherwise.
  std::string stallReason;

  // Value equality, so a consumer can skip a rebuild when nothing moved. It
  // compares EVERY field on purpose: a partial comparison would be one more
  // place that can decide two different readings are the same one.
  bool operator==(const ConnectReading& o) const {
    return sdk == o.sdk && rawStatus == o.rawStatus &&
           destinationSelected == o.destinationSelected && tunnelBound == o.tunnelBound &&
           providerCount == o.providerCount && insufficientBalance == o.insufficientBalance &&
           proofLoss == o.proofLoss && statusObserved == o.statusObserved &&
           stallReason == o.stallReason;
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
    s.proofLoss = proofLoss;
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
  // (e.g. the client was removed, or this session was signed out from another
  // device): the sdk has already cleared its local auth state. The report
  // carries the cause its listener read and the sign-in it was heard in
  // (AuthLogout.hpp). The handler runs on an sdk thread and must only marshal
  // -- the ui marshals onto the main loop and, when SignsOut(report), calls
  // Logout().
  using AuthInvalidHandler = std::function<void(auth_logout::Report report)>;
  using JwtRefreshedHandler = std::function<void()>;
  // The ONE connection feed. It replaced a string push whose five call sites
  // could only ever emit "DESTINATION_SET" or "DISCONNECTED" — a vocabulary
  // with no word for "connected" — beside a separate, visibility-gated stats
  // push that carried the real status and was never read for the status row.
  using ConnectReadingHandler = std::function<void(ConnectReading reading)>;
  using StatsHandler = std::function<void(const LiveStats& stats)>;
  using DrawerEventHandler = std::function<void(DrawerEvent event)>;

  bool Initialize(const std::string& storageDir, const std::string& logDir);
  // Why the last Initialize failed, as the SDK said it; empty after a success.
  // The startup shows it (StartupFailure.hpp): a failed start has no window.
  const std::string& InitializeError() const { return initializeError_; }
  bool IsLoggedIn();

  // Account discovery for the email-first login flow (Api::authLogin with just
  // the user_auth): says whether the auth belongs to a password account, to
  // another sign-in method, or to nobody (-> sign-up). The callback fires on
  // an SDK thread with the routing decision.
  void StartLogin(const std::string& userAuth, std::function<void(LoginRouting)> done);
  void LoginWithPassword(const std::string& userAuth, const std::string& password,
                         std::function<void(AuthResult)> done);
  void LoginWithCode(const std::string& authCode, std::function<void(AuthResult)> done);

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
  // Instant accounts can be referred too: a validated referral code rides
  // along on the create (empty = none).
  void CreateInstantAccount(const std::string& referralCode,
                            std::function<void(InstantAccount)> done);
  void ConfirmInstantAccount(std::function<void(AuthResult)> done);
  void DiscardInstantAccount();

  // ---- network server (iOS NetworkServerSheet / windows parity) ------------
  // Which network API this client talks to — on this fork, the difference
  // between the official bringyour.com and a self-hosted deployment.
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
  //
  // Writes the host's values OVER what the space already stores under that
  // key, so re-applying the same server (or returning to one used before)
  // keeps the VLESS server and the private extender saved in it. The space is
  // made active, so the next launch binds it, except the space of the launch's
  // URNETWORK_NETWORK_HOST override, which its host is keyed to with the
  // override's env and which stays bound for this process only.
  bool ApplyNetworkServer(const std::string& hostName, const std::string& apiUrl,
                          const std::string& connectUrl);
  // The active space serialized for the daemon's start_tunnel: the daemon
  // must build its DeviceLocal in the SAME space or the DeviceRemote would
  // sync against a device registered in a different network ("" = default).
  std::string NetworkSpaceJson();

  // ---- VLESS (sdk vless_settings_ui.go) --------------------------------------
  // The VLESS server of the ACTIVE network space: the space the api/auth calls
  // dial and the one start_tunnel hands urnetworkd. Settings > VLESS and the
  // login screen's network sheet (before sign-in) both edit it here. The
  // daemon imports the space at its next tunnel start, so a save reaches the
  // VPN the next time it connects.
  //
  // Never empty while a space exists: with nothing stored the SDK answers the
  // new-form defaults (443, tcp, reality, vision, chrome, off). nullopt only
  // with no space, or when the read threw.
  std::optional<urnet::VlessSettings> GetVlessSettings();
  // "" when saved -- applied in place, so the space and everything derived
  // from it stay valid. A vless_error_* id when ENABLED settings do not
  // validate; nothing was saved then. Settings that are off are saved as typed,
  // and off with no address and no id clears them. nullopt with no space to
  // save to, or when the call threw.
  std::optional<std::string> SetVlessSettings(const urnet::VlessSettings& settings);
  // The SDK's free functions, wrapped so an SDK exception never reaches a view.
  // They touch no host state. ParseVlessLink: nullopt when the call failed (the
  // caller says something went wrong); otherwise Settings (enabled) or an
  // Error id. VlessSettingsLink: "" when the settings do not validate.
  // ValidateVlessSettings: "" or the error id of the first problem (whether the
  // settings are enabled does not matter), the C ABI's URNET_ERROR_ID_INTERNAL
  // when the call failed.
  static std::optional<urnet::VlessLinkResult> ParseVlessLink(const std::string& link);
  static std::string VlessSettingsLink(const urnet::VlessSettings& settings);
  static std::string ValidateVlessSettings(const urnet::VlessSettings& settings);

  // ---- bootstrap DNS-over-HTTPS servers (sdk control_doh_ui.go) ---------------
  // `https://<ip literal>/<path>` servers in the active network space, tried
  // ahead of the built-in DoH servers for the lookups of the space's own names,
  // for networks that block the built-in ones. Account > Extenders and the
  // login screen's network sheet (before sign-in: a fresh install behind such a
  // network cannot sign in without them) both edit them here. Like VLESS they
  // need no tunnel; the daemon imports the space at its next tunnel start.
  //
  // The servers, v4 then v6, normalized; empty is the built-in servers alone.
  // nullopt only with no space, or when the read threw.
  std::optional<std::vector<std::string>> GetControlDohUrls();
  // "" when saved -- applied in place, so the space and everything derived
  // from it stay valid; an empty list clears them. A control_doh_error_* id
  // when a line does not validate or there are too many; nothing was saved
  // then. nullopt with no space to save to, or when the call threw.
  std::optional<std::string> SetControlDohUrls(const std::vector<std::string>& urls);
  // The SDK's preset for a country (extender::kControlDohChinaCountryCode),
  // v4 first; empty when there is none or the call threw. No host state.
  static std::vector<std::string> RegionalControlDohUrls(const std::string& countryCode);

  // Sign in with a Solana wallet (Phantom/Solflare) via the ur.io/wallet-connect
  // browser bridge: connect -> sign a challenge -> authLogin{wallet_auth}. The
  // urnetwork:// callback must be routed back in via HandleDeepLink.
  void SignInWithSolana(WalletConnect::Provider provider, std::function<void(AuthResult)> done);

  // Sign in with a Bittensor wallet (bittensor::kWalletTalisman or
  // kWalletTaoCom, BittensorWalletFlow.hpp) through an SDK
  // BittensorWalletSession: Talisman signs on the ur.io bridge (one hop, the
  // extension in the system browser), TAO.com on the manual sheet
  // (SetBittensorManualHandler). The proof -> authLogin{wallet_auth} with
  // blockchain urnet::TAO. Same deep-link routing as Solana (HandleDeepLink).
  void SignInWithBittensor(const std::string& walletId, std::function<void(AuthResult)> done);

  // The manual transport (TAO.com): the host asks the window to show the
  // challenge and collect the address and signature. Runs on the GTK main loop.
  struct BittensorManualRequest {
    std::string walletId;
    std::string purpose;
    std::string message;
    // the address the challenge is bound to ("" = any)
    std::string expectedAddress;
    // the wallet flow it belongs to (CancelBittensorManual)
    uint64_t flow = 0;
  };
  void SetBittensorManualHandler(std::function<void(BittensorManualRequest)> handler);
  // The sheet's Continue. A correctable refusal comes back for the sheet to
  // show (bittensor::Classify == Retry); an accepted proof continues the flow,
  // and any other refusal ends it.
  urnet::BittensorWalletResult SubmitBittensorManual(const std::string& address,
                                                     const std::string& signature);
  // The sheet for `flow` was closed: that flow, if it is still the newest, is
  // answered bittensor::kCancelled (a newer flow is left alone).
  void CancelBittensorManual(uint64_t flow);

  // Sign in with Google or Apple through the provider's own web flow
  // (SsoBridge.hpp): the host mints a state + nonce for the attempt, the api's
  // callback returns the provider's identity token on
  // urnetwork://oauth/<provider>, and only a return echoing that state with a
  // token minted for that nonce reaches
  // authLogin{auth_jwt_type, auth_jwt}. A new identity (no network) reports
  // sso_needs_network and the create page finishes with
  // CreateNetworkWithPendingSso.
  void SignInWithSso(const std::string& provider, std::function<void(AuthResult)> done);

  // The same bridge as a plain SIGNER (no sign-in): fetch a wallet challenge for
  // `walletAddress` (empty = whichever wallet the bridge picks), open the bridge
  // with purpose "connect", and hand back the ss58 address, the sr25519
  // signature and the exact message that was signed. The Earnings page uses it
  // to attach a Bittensor coldkey to the UR protocol (POST /sn/wallet verifies
  // the signature server-side). Same deep-link routing as sign-in.
  struct WalletSignature {
    bool ok = false;
    std::string address;
    std::string signature;
    std::string message;
    std::string error;
  };
  void SignBittensorConnect(const std::string& walletId, const std::string& walletAddress,
                            std::function<void(WalletSignature)> done);

  // The same bridge as a plain CONNECT for a Solana wallet (Phantom / Solflare):
  // no challenge and no signature. The bridge connects the wallet and the
  // urnetwork://<provider>-connect return hands back its base58 public key,
  // which `done` receives as-is (the server validates it on POST /account/wallet,
  // which takes no signature -- android's MWA connect and apple's
  // connectPhantomWallet did the same). The Earnings page links it as the USDC
  // payout wallet. The bridge is this request's from the moment it starts: a
  // connect still waiting and a Bittensor signature request still waiting
  // (SignBittensorConnect) are answered "superseded by a wallet connect
  // request", and a challenge still being fetched for an older flow will not
  // open the bridge over it (walletFlows_). Any later wallet flow answers this
  // one "superseded by ..." in turn. `done` runs where the answer arrives: on the
  // GTK main loop for a deep link, on the caller's own thread when the browser
  // cannot be opened; callers marshal with PostToMain either way.
  struct SolanaConnectResult {
    bool ok = false;
    std::string address;  // base58 public key
    std::string error;
  };
  void ConnectSolanaWallet(WalletConnect::Provider provider,
                           std::function<void(SolanaConnectResult)> done);

  // ---- add a sign-in method to the signed-in network (AddSignInFlow.hpp) ----
  // Account > Login methods. Each runs the login's own flow (the provider's web
  // flow, the wallet bridge, the Bittensor session with purpose "add") but ends
  // in POST /auth/add-auth on the current session: never authLogin, so the
  // session's jwt is never replaced and the app never signs in as the added
  // identity. A browser return is answered to the flow that opened it: an add's
  // return never reaches a sign-in and a sign-in's never reaches the add.
  // `done` runs on whatever thread the answer arrives on; callers marshal with
  // PostToMain. A newer add, or any other wallet or sso flow, answers a waiting
  // add "superseded by ..." (bridge::IsSuperseded).
  struct AddSignInResult {
    bool ok = false;
    std::string error;
    // the server's code for the refusal ("" for none): signature_mismatch for
    // a pasted wallet signature from another account
    std::string code = {};
  };
  void AddSignInWithSso(const std::string& provider, std::function<void(AddSignInResult)> done);
  void AddSignInWithSolana(WalletConnect::Provider provider,
                           std::function<void(AddSignInResult)> done);
  void AddSignInWithBittensor(const std::string& walletId,
                              std::function<void(AddSignInResult)> done);
  // The sheet closed: a waiting add is dropped unanswered, and a late return
  // for it is ignored.
  void CancelAddSignIn();
  // POST /auth/add-auth with `args` on the current session.
  void AddAuthMethod(const addsignin::Args& args, std::function<void(AddSignInResult)> done);

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
  // Create a network bound to the identity token captured by an sso sign-in
  // that had no network yet (name + terms, no password).
  void CreateNetworkWithPendingSso(const std::string& networkName,
                                   const std::string& referralCode,
                                   std::function<void(AuthResult)> done);
  bool HasPendingSsoAuth();
  // Verify-code entry (Api::authVerify) and resend (Api::authVerifySend).
  void VerifyCode(const std::string& userAuth, const std::string& code,
                  std::function<void(AuthResult)> done);
  void ResendVerifyCode(const std::string& userAuth,
                        std::function<void(VerifySendNotice notice)> done);
  // The reset link send's outcome, decided like a verification code send.
  void SendPasswordResetLink(const std::string& userAuth,
                             std::function<void(VerifySendNotice notice)> done);
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

  // Sign out of URnetwork (owner decisions 2026-10-05: the tunnel and the
  // provider stop as on Quit, the app keeps running, signed out; and each
  // network starts fresh). A sign-in method being added is cancelled, so its
  // late return cannot add it to the next account. Signed out at once
  // (signedOut_), so a reconcile or a connect that runs after this starts
  // nothing for the account; the local credentials are logged out and the
  // api's credential cleared; the DeviceRemote goes, as Shutdown takes it
  // down; then the sign-out is recorded as owed (SignOut.hpp) and delivered:
  // Quit's stop_tunnel, which also lifts the kill-switch floor, then logout,
  // which deletes the daemon's device identity and what its sdk stored for
  // the account, unless the daemon runs another user's session. A delivery
  // that does not complete (no daemon, a refusal) leaves it owed in a marker
  // that outlives the app: the provider reconcile, which the health poll
  // runs, delivers it first, and nothing starts until it has been delivered.
  // The sign-out completes in the app either way.
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
  // `reason` names the gesture in the journal (a static string, never shown).
  TunnelStartResult StartTunnel(const char* reason);
  // Human-readable detail for the last non-Started result ("" when none).
  std::string LastTunnelError();
  // The device rpc's host:port this session dialed, "" with none.
  std::string RpcHostPort();
  // Both connect calls ask the connect gate first (SetConnectGate) and do
  // nothing when it blocks.
  void ConnectBestAvailable();
  // Connect to a chosen provider location (country/region/city/device/peer). The
  // chooser passes an SDK-supplied ConnectLocation as-is, or one built from a peer
  // (client id + display name). No-op with the tunnel down (no connect VC).
  void Connect(const std::optional<urnet::ConnectLocation>& location);
  // The start-connect gate (InsufficientBalanceNotice.hpp DecideStartConnect),
  // installed by MainWindow. Returns true when the connect must not happen now;
  // it shows the upgrade path itself, or reads a stale balance first and then
  // calls retry, which repeats the same connect. Called on the caller's thread
  // (the GTK main loop) before mutex_ is taken, so it may touch window state.
  using ConnectGate = std::function<bool(std::function<void()> retry)>;
  void SetConnectGate(ConnectGate gate) { connectGate_ = std::move(gate); }
  // A location row's click, from the chooser and the Network page (nullopt is
  // the best-available row). It runs the start path the window installs
  // (SetRowConnect), which starts a tunnel when there is none, as the Connect
  // button does: Connect alone drives only a session that is already up, so
  // after a Disconnect, or a stop by the daemon, a row click started nothing.
  //
  // Coalesced, as on Windows (RowConnectCoalescer.hpp): the click runs once it
  // has been the last one for 1.2 s, and a click on the location the session
  // is already driving is no connect and drops a newer click still settling.
  // Main loop only.
  void ConnectFromRow(const std::optional<urnet::ConnectLocation>& location);
  // Drops a row click still settling. The immediate gestures supersede it: the
  // Connect button and the tray (MainWindow), Disconnect and Logout.
  void CancelRowConnect(const char* why);
  // The start path a row click runs: MainWindow::StartTunnelUi with the row's
  // location. Without one a row click only connects.
  using RowConnect = std::function<void(const std::optional<urnet::ConnectLocation>& location)>;
  void SetRowConnect(RowConnect run) { rowConnect_ = std::move(run); }
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

  // Keep providing while disconnected (ProvideLifecycle.hpp). With no tunnel
  // session the daemon's provider-only device is the provider; this keeps it in
  // step with the stored provide mode — started, re-moded, or stopped
  // (provide::DisconnectedProviderStep) — and drops a device bound to a
  // session the daemon no longer runs, the rule StartTunnelLocked applies. A
  // live tunnel session is left alone: its device provides. Runs after
  // Disconnect, on a provide mode or provider policy change, after a network
  // space value is saved, and from the window's health poll while
  // disconnected; `userInitiated` restarts the retry pacing, and
  // `settingsChanged` sends start_provider even to a running provider, which
  // the daemon replaces when the request differs (a saved DoH server list
  // changes the network space it was built from). Asks the daemon nothing for a
  // mode that does not provide once nothing is known to run. Main loop.
  void ReconcileProvider(const char* reason, bool userInitiated = false,
                         bool settingsChanged = false);
  // The health poll's reconcile, decided on the status reply its worker has
  // just read rather than on a read of its own, so a steady tick waits on
  // nothing. A device still bound is read afresh (ReconcileProviderLocked).
  void ReconcileProvider(const char* reason, const ctl::StatusReply& polled);
  // A provider device runs: a tunnel session's (a DeviceRemote is bound) or,
  // with none, the daemon's provider-only device as its status last said. The
  // Earnings page derives a local idle reason only then
  // (providerstatus::SessionIdleReasonFor).
  bool ProviderRuns();

  // The network country (P052). urnetworkd reads the country of the mobile
  // network this machine is on from ModemManager and publishes it in `status`
  // (network_country_code). The sdk's value is per process, so this applies it
  // here too: this process's own sign-in and api dials draw their extender
  // names from the same spoof list as the daemon's devices. A redacted status
  // (another user's session) or a daemon that predates the field gives "".
  // While the daemon cannot be asked, the last value stays. Main loop: the
  // window's health poll calls it with the status its worker read.
  void FollowDaemonNetworkCountry(const ctl::StatusReply& status);

  // "Send feedback with logs" while disconnected (support inbox 2090). The
  // logs that matter are urnetworkd's, and the DeviceRemote reaches them only
  // while a tunnel session runs. This asks the daemon to upload its own logs
  // for `feedbackId` (ControlProtocol.hpp upload_logs), connected or not, with
  // this session's client credentials as start_provider carries them. The
  // daemon answers once it admitted the upload, which then runs on its own
  // thread: Accepted (its outcome is followed in status, FollowDaemonLogUpload),
  // Busy (one is in flight already), or NotTaken when it cannot be asked,
  // predates the verb or refused, and the caller then falls back to the
  // DeviceRemote as before (logupload::GuiStepAfterDaemon). Blocking, bounded by
  // the control client's receive timeout, and never holding mutex_ across the
  // call. Main loop. This process's own newest glog files ride with the
  // request, by descriptor, into the daemon's zip (PassedLogFiles.hpp).
  logupload::DaemonAnswer UploadDaemonLogs(const std::string& feedbackId);
  // The outcome of the upload UploadDaemonLogs left pending, once the daemon's
  // status names it finished (logupload::CompletionFor): logged, and the wait
  // ends. Main loop (MainWindow::ApplyDaemonHealth, with the poll's status).
  void FollowDaemonLogUpload(const ctl::StatusReply& status);
  // A Reset extenders urnetworkd refused because a bring-up owned its session
  // (OwedExtenderReset), sent again, once, on the reset's worker when the
  // status shows that bring-up settled. It asks for no dialog, so the daemon
  // refuses it rather than prompt (beside another user's live session, or
  // where authorizing it would need a dialog), and it is then dropped. Main
  // loop (MainWindow::ApplyDaemonHealth, with the poll's status).
  void FollowDaemonExtenderReset(const ctl::StatusReply& status);

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
  // Whether a report the auth-invalid handler was given still signs the app
  // out: it was heard while signed in, and nothing has signed out or in since.
  // Each listener that hears a rejection reports it (the Api's, and the bound
  // DeviceRemote's), and only the first of them signs out. Main loop.
  bool SignsOut(const auth_logout::Report& report) const { return authLogouts_.SignsOut(report); }
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
  // The location the user chose. The connect view controller's selection,
  // which it keeps across a Disconnect, and with the presentation closed the
  // device's connect location, are Windows' reads; the connect location
  // persisted in the LocalState, and then the persisted default location (the
  // last choice, which ConnectViewController.Connect saves and a Disconnect
  // leaves), are Linux's, for a closed presentation and for no device. What
  // the provider row shows and a Connect press connects to.
  std::optional<urnet::ConnectLocation> SelectedLocation();
  // The location the device is connected to (with no device, the persisted
  // connect location): what the DNS recommendation reads its country from.
  std::optional<urnet::ConnectLocation> ConnectedLocation();
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
  // Transport policy (client: the carrier this device uses to reach providers;
  // provider: the carrier it uses while providing for others). Reads come from
  // the device when present (the daemon's truth over the rpc, the pending /
  // last-known policy offline), else the GUI's persisted mirror; nullopt =
  // never edited (the editor drafts from the SDK default). Writes apply over
  // the device rpc AND mirror into the GUI's LocalState -- the daemon's
  // DeviceLocal persists its own copy, but the two processes do not share
  // local state, so StartTunnel seeds the device from the mirror (an edit made
  // with the tunnel down survives a relaunch and lands on the next start; the
  // apple TransportSettingsStore/DeviceManager.initDevice pattern). Changes
  // arrive as DrawerEvent::TransportSettings / ProviderTransportSettings.
  std::optional<urnet::TransportSettings> GetTransportSettings();
  void SetTransportSettings(const urnet::TransportSettings& settings);
  std::optional<urnet::TransportSettings> GetProviderTransportSettings();
  void SetProviderTransportSettings(const urnet::TransportSettings& settings);
  // Runtime Auto capability from the daemon process that owns the memory
  // budget. Nil while no device/last-known status exists.
  std::optional<urnet::TransportStatus> GetTransportStatus();
  std::optional<urnet::TransportStatus> GetProviderTransportStatus();
  std::optional<urnet::ThroughputPointList> ThroughputPoints();
  int64_t ThroughputWindowSeconds();
  // The window's remote traffic partitioned by transport, ready to render (the
  // SDK's TransportDistribution: shares in the stable order with cumulative
  // boundaries, whole percents, used/enabled flags). Read on the same
  // Throughput tick as the points; nullopt with the tunnel down.
  std::optional<urnet::TransportDistribution> ClientTransportDistribution();
  std::optional<urnet::TransportDistribution> ProviderTransportDistribution();
  // The provider and the extender series of the same controller, read on the
  // same Throughput tick as ThroughputPoints (EXTENDER.md O3, O5): the provider
  // points carry the provider's Local and Block routes, the extender points
  // the traffic this device's extender role relayed, in the Remote route only.
  // nullopt with no session, except that both series (and the provider
  // distribution above) of the daemon's provider-only device stand in while no
  // DeviceRemote is bound (provider_stats; see DaemonProviderStats).
  std::optional<urnet::ThroughputPointList> ProviderThroughputPoints();
  std::optional<urnet::ThroughputPointList> ExtenderThroughputPoints();
  // The device reports provider packet stats: the half of the provider
  // statistics gate (O8) the provide control mode does not decide. With no
  // session, the provider-only device's answer as the daemon last gave it, and
  // false without one. The extender role's running state is not read here: it
  // is the Enabled of the pushed ProviderExtenderProvideStatus.
  bool HasProviderStats();
  // The same fact asked of the DEVICE, one device rpc, for the forced re-reads
  // right after a contract view controller opens (a device arriving, the
  // window coming back): a new controller's provider stats stay nil until its
  // first sample, while the device answers at once. Provider presence is fixed
  // per device, so the two agree once the controller has sampled. With no
  // device, the provider-only device's answer as HasProviderStats gives it.
  bool DeviceHasProviderStats();
  std::optional<urnet::BlockActionList> BlockActions();
  std::optional<urnet::BlockStats> BlockStatsSnapshot();
  std::optional<urnet::BlockActionOverrideList> BlockActionOverrides();
  void AddBlockActionOverride(const urnet::BlockActionOverride& override_);
  // The connect inspector's host rules (QuickAction.hpp), written like any
  // override and returning the new override's id for the press's Undo, or ""
  // when nothing was written (no hosts, or no device and no local state).
  // AddHostBlockRule: BlockOverride{block}, true blocks the hosts and false
  // countermands a block. AddHostRouteRule: RouteOverride{local} with Pin
  // off, true bypasses the tunnel and false keeps the hosts in it (a pin is
  // exit placement inside the tunnel, never tunnel membership).
  std::string AddHostBlockRule(const urnet::StringList& hosts, bool block);
  std::string AddHostRouteRule(const urnet::StringList& hosts, bool local);
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
  // The providers with an established, identity-verified e2e session, through
  // the SDK's shared PostQuantumIdentityViewController (the apple
  // PostQuantumIdentityStore binds the same one). The VC lives only while the
  // tunnel runs; reads return nullopt otherwise. Changes arrive as
  // DrawerEvent::ProviderIdentities.
  std::optional<urnet::ProviderIdentityList> ProviderIdentities();

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

  // ---- extenders (EXTENDER.md K4 to K8, N2 to N8) ---------------------------
  // The extender directory + gossip status, read off the DEVICE (K5: it lives
  // on DeviceLocal and reaches DeviceRemote over the rpc with the last value
  // cached, exactly as the provider family transport status), so the connect
  // page's panel reads the DAEMON's directory rather than this process's.
  // nullopt with no device, which the panel draws as the disconnected network
  // (Windows' rule). Changes arrive as DrawerEvent::ExtenderStatus, coalesced
  // by the SDK to one callback per second.
  std::optional<urnet::ExtenderStatus> GetExtenderStatus();

  // The status of this device's OWN extender role (EXTENDER.md N2, N3), read
  // off the DEVICE like GetExtenderStatus, because the role runs in the
  // daemon's DeviceLocal. DeviceRemote reads it through the rpc with the last
  // value cached, and answers the unsupported status against a device process
  // that lacks the method. With no device, the daemon's provider-only device's
  // as provider_stats last read it, only while the daemon takes the switch's
  // write (provide::ExtenderSwitchSourceFor); nullopt otherwise, which the
  // connect page's row renders as hidden. Changes arrive as
  // DrawerEvent::ExtenderProvideStatus: the SDK coalesces them to one callback
  // per epoch (a second) after any change of the setting, the provide state or
  // the role, and fires none on registration, so the pages re-read the status
  // on DeviceLifecycle; the provider_stats poll raises the same event.
  std::optional<urnet::ExtenderProvideStatus> GetExtenderProvideStatus();
  // The extender role of the device that provides, for the earnings page's
  // read-only row and the running state behind its extender statistics:
  // GetExtenderProvideStatus with a session, and while no DeviceRemote is
  // bound the daemon's provider-only device's, as provider_stats last read it
  // (a change there raises the same DrawerEvent::ExtenderProvideStatus).
  // nullopt with neither, and from a daemon that predates the field. The
  // connect page's row keeps GetExtenderProvideStatus: its switch must stay
  // hidden over a daemon that cannot take its write (N1).
  std::optional<urnet::ExtenderProvideStatus> ProviderExtenderProvideStatus();
  // The provider extender setting of the daemon's space, through the device:
  // the queued or last-known value while the daemon is out of contact, so the
  // toggle never snaps back during a daemon restart (N2). With no device, the
  // provider-only device's as provider_stats last read it, while the daemon
  // takes the switch's write; otherwise the setting's default, on (N4) --
  // nothing draws it then, the row is hidden.
  bool GetProvideExtender();
  // Writes the setting through the device, which persists and applies it at
  // once; queued and replayed at the next sync while the daemon is unreachable
  // (N2, N4). With no device, to the daemon (set_provide_extender), which
  // persists it in the same space a tunnel session's device reads, and the
  // setting is read back at once, written or refused. The GUI's own LocalState
  // is not the daemon's space and is never written. Otherwise the write is
  // dropped: the row is hidden then, and the setter is never called while the
  // row is hidden (N1).
  void SetProvideExtender(bool on);

  // The SDK's shared ExtenderViewController (K7: "encoding, decoding and
  // applying live in the sdk, one implementation for every app"). Opened with
  // the rest of the presentation, so every call below returns nullopt with the
  // window hidden or the tunnel down and the account section renders its
  // no-device state rather than an empty form.
  //
  // The controller works on the device's space, and a DeviceRemote's space is
  // this process's own (networkSpace_), not urnetworkd's. A settings save
  // changes this process's space and restarts its extender client and node in
  // place. urnetworkd takes the saved values at its next start_tunnel or
  // start_provider import, which carries them in network_space_json, not
  // directly: until then the tunnel and the provider-only device keep the
  // values they were started with. A share is built from this process's
  // directory, and an import adds its addresses there; those addresses never
  // reach the daemon, while an import's settings travel like a save.
  std::optional<urnet::ExtenderSettings> GetExtenderSettings();
  std::optional<urnet::ExtenderSettings> SetExtenderSettings(const std::string& dnsName,
                                                             const std::string& gossipUrl,
                                                             const std::vector<std::string>& hosts);
  std::optional<urnet::ExtenderShareResult> BuildExtenderShare(bool includeSettings);
  std::optional<urnet::ExtenderShareDecodeResult> DecodeExtenderShare(const std::string& text);
  std::optional<urnet::ExtenderImportResult> ImportExtenderShare(const std::string& text,
                                                                 bool useSettings);

  // The LEGACY private extender (F1: NetExtender stays), an advanced override
  // on the network space values written through updateNetworkSpaceValues --
  // the same path ApplyNetworkServer uses -- over what the space stores, so no
  // other value moves. Empty ip AND secret clears it.
  //
  // This writes the GUI's own space, the one its api/auth calls dial. urnetworkd
  // builds its devices from the space the GUI sends with start_tunnel and
  // start_provider, so a tunnel takes a private extender set here at its next
  // connect, and a running provider-only device at once (the save sends
  // start_provider again). The settings the view controller above saves are
  // in this same space and reach the daemon the same way, at its next import,
  // except that their save sends no start_provider.
  std::optional<urnet::NetExtender> GetPrivateExtender();
  bool SetPrivateExtender(const std::string& ip, const std::string& secret);

  // Account > Extenders' Reset extenders (connect EXTENDER.md E7): back to a
  // fresh install's extender state. Resets this process's own network space
  // (NetworkSpace::resetExtenders: what the space learned and what the user
  // added go, the dns name, gossip url and root keys return to their defaults,
  // the cleared values are persisted with the reset's id, and the space's
  // extender client and node relearn), then hands the space's key and the id
  // to urnetworkd (reset_extenders), which applies the reset to the space it
  // holds under that key, the one its tunnel session's device and its
  // provider-only device run in. Not the device rpc: the verb covers the
  // provider-only device and no session too. A daemon that is unreachable or
  // does not take it applies the reset at its next import of this space
  // (start_tunnel, start_provider), whose values carry the id, except that one
  // refusing it because a bring-up owns its session gets it again once that
  // bring-up settled (FollowDaemonExtenderReset). The bootstrap DoH servers, the
  // gossip mode and the provider extender setting stay.
  //
  // Runs on a worker, never on the main loop: the sdk joins the space's old
  // extender client, and the daemon may put a polkit dialog in front of the
  // verb. `done` runs on the main loop. False, with `done` never called,
  // without a network space or while a reset is in flight.
  struct ExtenderResetOutcome {
    bool reset = false;        // this process's space was reset
    bool daemonReset = false;  // urnetworkd applied it to the space it holds
  };
  bool ResetExtenders(std::function<void(ExtenderResetOutcome)> done);

  // ---- this device's provider status (support part P008) -------------------
  // The SDK's ProviderStatusViewController: GET /network/provider-status about
  // once a minute while it polls, publishing how often the network offered
  // this device to clients per minute over the last hour, the numbers it is
  // ranked by and the first reason holding it back. Opened with the rest of
  // the presentation (SubscribeDrawer), but only while the provide control
  // mode is not never, and closed with it or when the mode becomes never.
  // Changes arrive as DrawerEvent::ProviderStatus after every poll, success or
  // failure, after a stop that dropped a poll in flight, and when the
  // controller opens or closes.
  struct ProviderStatusSnapshot {
    bool open = false;    // a controller exists
    bool loaded = false;  // a poll has succeeded
    std::string lastFetchError;  // the last failed poll's error, "" once one succeeds
    // this device's status; nullopt before a poll, or when the device is not
    // one of the network's provider clients
    std::optional<urnet::ProviderStatus> status;
  };
  // The controller's state, read together under the lock. With no DeviceRemote
  // it is the controller the daemon runs on its provider-only device, as
  // provider_stats last read it.
  ProviderStatusSnapshot ProviderStatusNow();
  // Polling follows the Earnings destination on screen: true starts the
  // controller (one poll at once, then about once a minute), false stops it
  // and keeps its last snapshot. Remembered across the controller's close and
  // reopen. It also paces provider_stats, which reads the provider-only
  // device's statistics from the daemon at once and then about once a second
  // while polling.
  void SetProviderStatusPolling(bool polling);
  // provider_stats on the same pace while the connect destination is on
  // screen, for its Extender switch over the provider-only device. It never
  // keeps the provider status controller polling the API.
  void SetProviderExtenderPolling(bool polling);

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

  // One daemon `status` read on a worker, for the window's health poll, and
  // `done` on the GTK main loop with the reply (nullopt when the daemon did
  // not answer). The poll used to read on the main loop every 5 s, so a
  // daemon that accepts the socket but no longer answers held the window for
  // the control client's 30 s receive timeout on every tick (Windows D4).
  // Single-flight: false, and no `done`, while a read is in flight, so a slow
  // daemon costs ticks rather than a queue. `done` may land after its caller
  // has moved on, so it carries its own staleness guard.
  bool RequestDaemonStatus(std::function<void(std::optional<ctl::StatusReply>)> done);

  // Exposed so the (full-parity) UI/view models can drive the SDK directly.
  urnet::Api& api() { return *api_; }
  // The app-wide client event queue (ClientEvents.hpp): every product event
  // the onboarding optimization loop reads goes through this one facade.
  // Valid after Initialize().
  ClientEventQueue& events() { return *events_; }
  // The sign-up pages' "Periodic product updates" switch, read at submit:
  // the next network create carries product_updates=false when it is off
  // (absent = opted in), and signup.optout_changed records the opt-out.
  void SetProductUpdatesOptOut(bool optOut);
  // urnetwork://onboarding/<step> deep links (the campaign emails' buttons):
  // routed to the window, which owns the destinations. Fired on the GTK loop.
  void SetOnboardingLinkHandler(std::function<void(const std::string& url)> handler) {
    onOnboardingLink_ = std::move(handler);
  }
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
  // Requires mutex_. Run right after every `api_ =`: the Api just taken from
  // the space reports this app's client info (NetworkSpaceConfig.hpp
  // ReportClientInfo), and its own sign-out reaches the auth-invalid handler.
  void AdoptSpaceApiLocked();
  // A listener's sign-out, with the cause it read: to the auth-invalid
  // handler, which marshals (an sdk thread).
  void ReportAuthLogout(std::string cause);
  void RegisterNetworkClient(const std::string& byJwt, std::function<void(AuthResult)> done);
  // Shared routing for NetworkCreateResult (sign-up + wallet sign-up).
  // `bittensorWalletId` is the Bittensor wallet that signed ("" for none).
  void HandleNetworkCreateResult(std::optional<urnet::NetworkCreateResult> result,
                                 std::optional<std::string> err,
                                 std::function<void(AuthResult)> done,
                                 const std::string& bittensorWalletId = std::string());
  // The body of StartTunnel(). Requires mutex_, so the recovery inside
  // ConnectBestAvailable can rebuild a session it has just discovered is dead
  // without re-entering a non-recursive lock. Every outcome logs, and the
  // failing ones leave a renderable sentence in lastTunnelError_.
  TunnelStartResult StartTunnelLocked(const char* reason);
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
  // the device persists the override, else local state does; false when
  // there is neither
  bool AddBlockActionOverrideLocked(const urnet::BlockActionOverride& override_);
  void SetupWalletCallbacks();
  // Answers a ConnectSolanaWallet that is still waiting with `reason` (another
  // wallet flow is taking the bridge). Takes mutex_: never call it holding it.
  void CancelPendingSolanaConnect(const std::string& reason);
  // Whether `flow` is still the newest wallet flow (walletFlows_); logs the drop
  // when it is not. Takes mutex_.
  bool WalletFlowIsCurrent(uint64_t flow);
  void RequestWalletChallenge(
      const std::string& blockchain, const std::string& walletAddress,
      std::function<void(std::optional<std::string> message, std::string error)> done);
  void FailWalletOperation(const std::string& error);
  // One Bittensor proof: a new SDK session for `walletId` and `purpose`, its
  // challenge (bound to `expectedAddress` when set), then -- on the main loop,
  // while `flow` is still the newest -- `prepare(message)` (false aborts; it
  // answers the flow itself) and the wallet transport: the bridge tab, or the
  // manual sheet. Failures before that answer the waiting flow
  // (FailWalletOperation), or `fail` when given -- which also hears a
  // superseded flow (the create-network flow, whose slot is set by `prepare`).
  void StartBittensorSession(const std::string& walletId, const std::string& purpose,
                             const std::string& expectedAddress, uint64_t flow,
                             std::function<bool(const std::string& message)> prepare,
                             std::function<void(const std::string& error)> fail = nullptr);
  void FinishCreateNetworkWithWallet(const std::string& signature);
  // blockchain: "solana" (ed25519, base64 signature) | urnet::TAO (sr25519, hex)
  void AuthLoginWithWallet(const std::string& address, const std::string& signature,
                           const std::string& message, const std::string& blockchain);
  void SubscribeStats();   // caller holds mutex_; opens presentation controllers
  void SubscribeDrawer();  // caller holds mutex_; opens presentation controllers
  void ClosePresentationLocked();
  // The provider status controller's two halves (P008). The open is a no-op
  // with no device, with one already open or with the mode never, and starts
  // the controller when polling; the close drops its listener first, then
  // hands it back with the typed close. Both require mutex_.
  void OpenProviderStatusLocked(const std::string& provideControlMode);
  void CloseProviderStatusLocked();
  // The bound device's extender role, nullopt when the read throws. Requires
  // mutex_ and a device.
  std::optional<urnet::ExtenderProvideStatus> DeviceExtenderProvideStatusLocked();
  // What the connect page's Extender switch reads and writes now
  // (provide::ExtenderSwitchSourceFor). Requires mutex_.
  provide::ExtenderSwitchSource ExtenderSwitchSourceLocked() const;
  void EmitDrawerEvent(DrawerEvent event);
  LiveStats ReadStats();  // read the current snapshot from the SDK getters
  void PublishStats();    // ReadStats() -> onStats_
  // Re-reads EVERY field from the live SDK getters. Takes no lock (same
  // contract as ReadStats): it is called from SDK listener threads, from the
  // GTK loop, and from inside StartTunnelLocked with mutex_ already held.
  // The reading's facts, then the degrade hold folded over them.
  ConnectReading ReadConnectReading();
  ConnectReading ReadConnectFacts();
  // The degrade hold (health::DegradeHold) over every reading, under its own
  // lock because readings are taken on any thread, and the one-shot timeout
  // that reads again when a running hold ends, since nothing else may.
  std::mutex degradeMutex_;
  health::DegradeHold degradeHold_;
  int64_t degradeReevalAtMillis_ = 0;  // the hold end the timeout is armed for
  unsigned int degradeReevalId_ = 0;    // g_timeout source id; 0 = unarmed
  void NoteNewConnectAttempt();
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
  std::string initializeError_;  // InitializeError(); the main thread's alone
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::Api> api_;
  std::unique_ptr<ClientEventQueue> events_;
  std::function<void(const std::string& url)> onOnboardingLink_;
  bool productUpdatesOptOut_ = false;  // the next create's product_updates
  // the sign-up pages' opt-out onto a create's args (absent = opted in)
  void ApplySignupPreferences(urnet::NetworkCreateArgs& args) const;
  // POST /network/auth-client with the device's time zone (IANA) and locale
  // (BCP 47) on the args: the campaign engine schedules its emails in the
  // user's local time. Sent as extra json fields until the C ABI's
  // AuthNetworkClientArgs carries them.
  void AuthNetworkClientWithLocale(const urnet::AuthNetworkClientArgs& args,
                                   urnet::AuthNetworkClientCallback callback);
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
  // extender settings, share and import (the SDK owns the payload format)
  std::optional<urnet::ExtenderViewController> extenderVc_;
  // this device's provider status (P008), open with the presentation while the
  // provide control mode is not never. Its listener is held apart from
  // presentationSubs_ so a mode change to never can drop it before the close.
  std::optional<urnet::ProviderStatusViewController> providerStatusVc_;
  std::optional<urnet::Sub> providerStatusSub_;
  bool providerStatusPolling_ = false;  // the Earnings destination is on screen
  bool providerExtenderPolling_ = false;  // the connect destination is on screen
  // control channel to urnetworkd (tunnel lifecycle + location override)
  ControlClient control_;
  std::string lastTunnelError_;
  // The network-provide-key bit, cached off addProvideSecretKeysListener:
  // DeviceRemote has no getProvideSecretKeys getter (it is DeviceLocal-only),
  // so like the Windows GUI the listener feeds this atomic and ReadStats
  // reads it.
  std::atomic<bool> provideHasNetworkKey_{false};
  // ---- the provider-only device (ReconcileProvider) -------------------------
  // Requires mutex_. settingsChanged: the provider policy was just edited,
  // which only a new device picks up. polled: a status the caller has just
  // read, used in place of a read here while no device is bound.
  void ReconcileProviderLocked(const char* reason, bool userInitiated, bool settingsChanged,
                               const ctl::StatusReply* polled = nullptr);
  // After a saved network space value (DoH servers, VLESS, the private
  // extender, the server): the reconcile with settingsChanged, posted to the
  // main loop. Takes no lock, so callers may hold mutex_.
  void ReconcileProviderAfterSpaceChange(const char* reason);
  // Caches what the daemon's status says about the provider-only device and
  // republishes the stats when it changed. Requires mutex_.
  void NoteDaemonProviderLocked(const ctl::StatusReply& status);
  provide::ProviderStepBackoff providerBackoff_;  // guarded by mutex_
  // A status has been read since launch, so "nothing runs" is known rather
  // than assumed (guarded by mutex_).
  bool providerStateKnown_ = false;
  // Set by Shutdown: a reconcile already queued (a posted Disconnect follow-up,
  // a poll in the same dispatch) must not start a provider the quit just
  // stopped (guarded by mutex_).
  bool providerReconcileClosed_ = false;

  // ---- the sign-out the daemon is owed (SignOut.hpp) -------------------------
  // Set by Logout and cleared when a sign-in stores its credential or a space
  // with one is chosen: until the asynchronous local logout lands, the stored
  // jwt still names the account that left, so IsLoggedIn, the reconcile and the
  // start read this first. Atomic: the sign-in's commit lands on the sdk's
  // thread.
  std::atomic<bool> signedOut_{false};
  // The sign-in the sdk's sign-out reports are heard in and checked against
  // (AuthLogout.hpp), moved with signedOut_ and at launch. Declared before
  // every subscription whose listener reads it.
  auth_logout::Tracker authLogouts_;
  // The daemon as a delivery sees it: the session ensured, the status read for
  // this uid, and stop_tunnel. Requires mutex_.
  signout::Daemon SignOutDaemonLocked();
  // Deliver an owed sign-out ahead of a reconcile or a start, paced by
  // signOutBackoff_ unless a person asked (userInitiated). Requires mutex_.
  void SettleSignOutLocked(const char* reason, bool userInitiated);
  // The marker file (SignOutOwedPath in SdkHost.cpp).
  static signout::Marker SignOutMarker();
  // Loaded by Initialize (guarded by mutex_).
  signout::Obligation signOut_{SignOutMarker()};
  provide::ProviderStepBackoff signOutBackoff_;  // guarded by mutex_
  // What ReadStats shows with no DeviceRemote: the provider-only device's
  // running bit, live tier, network-key bit and client count as `status` last
  // said, the count -1 while not said. Atomic for the same reason
  // provideHasNetworkKey_ is.
  std::atomic<bool> daemonProviderRunning_{false};
  std::atomic<int64_t> daemonProviderMode_{0};
  std::atomic<bool> daemonProviderNetworkKey_{false};
  std::atomic<int64_t> daemonProviderClientCount_{-1};
  // The network country last applied to this process (FollowDaemonNetworkCountry).
  // Main loop only.
  std::string followedNetworkCountry_;
  // The daemon's id of the log upload this process waits on (UploadDaemonLogs),
  // 0 for none. Main loop only, like the calls that read and write it.
  int64_t pendingLogUploadId_ = 0;
  // The Reset extenders owed to urnetworkd (FollowDaemonExtenderReset). Main
  // loop only: a press's answer is noted from the main loop, where the health
  // poll takes it. Never persisted, so a GUI that quits drops it.
  OwedExtenderReset owedExtenderReset_;
  // ---- the provider-only device's statistics (provider_stats) ---------------
  // What the daemon's view controllers on the provider-only device last said,
  // in the SDK's types. The provider statistics accessors, ProviderStatusNow,
  // ExtenderThroughputPoints and ProviderExtenderProvideStatus read it while no
  // DeviceRemote is bound, so the Earnings page's plots, its "no traffic yet"
  // line, the provider status and the extender row describe the device that is
  // providing, and the connect page's Extender switch reads it while the daemon
  // takes its write. Fetched at once and then about once a second while the
  // Earnings or the connect destination is on screen and the daemon's status
  // says that device runs.
  struct DaemonProviderStats {
    bool hasProviderStats = false;
    std::optional<urnet::ThroughputPointList> providerPoints;
    std::optional<urnet::TransportDistribution> providerDistribution;
    ProviderStatusSnapshot status;
    // the status part as it arrived, so an unchanged poll emits nothing
    std::string statusJson;
    // the device's extender role and the series of what it relayed; nullopt
    // from a daemon that predates them
    std::optional<urnet::ExtenderProvideStatus> extenderProvideStatus;
    std::optional<urnet::ThroughputPointList> extenderPoints;
    // the role's part as it arrived, for the same reason as statusJson
    std::string extenderProvideStatusJson;
    // the provider extender setting beside the role, the connect page's
    // switch, and that the daemon takes its write; false from a daemon that
    // predates them, which keeps that switch hidden
    bool provideExtender = false;
    bool provideExtenderWritable = false;
  };
  // Requires mutex_. One tick of the poll (provide::DaemonProviderStatsStep).
  void PollDaemonProviderStatsLocked();
  // Requires mutex_. Arms the poll's timer while a destination wants it, reading
  // at once when `readNow`, and disarms it once none does.
  void ScheduleProviderStatsPollLocked(bool readNow);
  // Requires mutex_. Forgets the snapshot and tells the pages when there was one.
  void DropDaemonProviderStatsLocked();
  std::optional<DaemonProviderStats> daemonProviderStats_;  // guarded by mutex_
  unsigned int providerStatsPollId_ = 0;  // g_timeout source id; 0 = unarmed
  // ControlClient::SessionGeneration() of the daemon connection that answered
  // provider_stats with ctl::kErrorUnknownVerb: it predates the verb, and is
  // not asked again until the connection is rebuilt. 0 = none (guarded by
  // mutex_).
  uint64_t providerStatsUnsupportedGeneration_ = 0;
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
  // RequestDaemonStatus' worker, guarded the same two ways.
  std::atomic<bool> daemonStatusBusy_{false};
  std::mutex daemonStatusWorkerMutex_;
  std::thread daemonStatusWorker_;
  // ResetExtenders' worker, guarded the same two ways: extenderResetBusy_ is
  // its single-flight gate, cleared by the worker before it marshals `done`,
  // and extenderResetWorkerMutex_ guards only the thread object, which ~SdkHost
  // joins.
  std::atomic<bool> extenderResetBusy_{false};
  std::mutex extenderResetWorkerMutex_;
  std::thread extenderResetWorker_;

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
  // Identity from a wallet sign-in with no network. Its first signature was
  // consumed by AuthLogin; CreateNetworkWithPendingWallet always fetches and
  // signs a new address-bound challenge before submitting it.
  std::optional<urnet::WalletAuthArgs> pendingWalletAuth_;
  std::string pendingWalletNetworkName_;
  std::string pendingWalletReferralCode_;
  std::function<void(AuthResult)> walletCreateDone_;
  std::function<void(WalletSignature)> walletSignDone_;  // SignBittensorConnect
  // The Bittensor wallet the last sign-in used (guarded by mutex_): the
  // create-network signature asks the same wallet.
  std::string bittensorWalletId_;
  // Set once on the UI thread before any flow (MainWindow).
  std::function<void(BittensorManualRequest)> onBittensorManual_;
  // ConnectSolanaWallet (guarded by mutex_, like walletSignDone_): consumed by the
  // connect return (on_public_key) or a bridge error (on_error), or answered
  // "superseded by ..." by the next wallet flow.
  std::function<void(SolanaConnectResult)> walletConnectDone_;
  // Guarded by mutex_: every wallet flow start takes the next number, and a step
  // that runs later (a challenge arriving after its fetch) opens the bridge only
  // while its flow is still the newest (WalletBridgeRoute.hpp FlowCounter).
  bridge::FlowCounter walletFlows_;
  // The sso attempt in flight (guarded by mutex_): the provider it was opened
  // for and the state + nonce minted for it; cleared by the first return that
  // echoes the state. walletAuthDone_ carries its completion, so the shared
  // error path (FailWalletOperation / on_error) covers it too.
  std::string ssoProvider_;
  std::string ssoState_;
  std::string ssoNonce_;
  // Who opened the sso attempt (guarded by mutex_): a sign-in (walletAuthDone_)
  // or an add (ssoAddDone_). A verified return goes only to its owner
  // (addsignin::RouteSso).
  addsignin::Owner ssoOwner_ = addsignin::Owner::None;
  std::function<void(AddSignInResult)> ssoAddDone_;
  // A wallet being added (guarded by mutex_): its signature goes to add-auth
  // (bridge::SignatureRoute::AnswerAdd), never to authLogin.
  std::function<void(AddSignInResult)> walletAddDone_;
  addsignin::WalletChain walletAddChain_ = addsignin::WalletChain::Solana;
  // Answers a waiting add (wallet and sso) with `reason`. Takes mutex_.
  void CancelPendingAddSignIn(const std::string& reason);
  // Identity from an sso sign-in with no network: the token and its type,
  // consumed by CreateNetworkWithPendingSso.
  bool pendingSsoAuth_ = false;
  std::string pendingSsoType_;
  std::string pendingSsoJwt_;
  void AuthLoginWithSso(const std::string& provider, const std::string& jwt);
  // The instant account's jwt, held between CreateInstantAccount and the
  // seedphrase sheet's confirm (guarded by mutex_; a secret — never log it).
  std::optional<std::string> pendingInstantJwt_;

  // Advanced Mode standing state (D5): the atomic is the authority between
  // the disk read at startup and any later toggle.
  std::atomic<bool> advancedMode_{false};
  bool advancedModeLoaded_ = false;
  std::function<void(bool)> onAdvancedMode_;

  AuthStateHandler onAuth_;
  ConnectGate connectGate_;
  RowConnect rowConnect_;
  // The settling row click and its timer (ConnectFromRow); main loop only.
  RowConnectCoalescer<std::optional<urnet::ConnectLocation>> rowConnects_;
  unsigned int rowConnectTimerId_ = 0;  // g_timeout source id; 0 = unarmed
  void ArmRowConnectTimer(int64_t delayMillis);
  void OnRowConnectDue();
  void RunRowConnect(const std::optional<urnet::ConnectLocation>& location);
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
  // The Api's own sign-out (AdoptSpaceApiLocked), subscribed on the current
  // api_ and replaced with it. Declared last so it closes before the handler
  // it calls and before the Api it listens to.
  std::optional<urnet::Sub> apiLogoutSub_;
};

}  // namespace urnw
