// SPDX-License-Identifier: MPL-2.0
#include "SdkHost.hpp"

#include <urnetwork_sdk.h>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <glib/gstdio.h>

#include "AppPrefs.hpp"
#include "Config.hpp"
#include "LocationSelection.hpp"
#include "NetworkSpaceConfig.hpp"
// The Secret Service backend for the remembered rpc session. GUI-ONLY: this is
// the one translation unit that links libsecret, and urnetworkd (which builds
// in a container that has no libsecret at all) never sees this header.
#include "SecretServiceRpcSessionStore.hpp"
#include "SsoBridge.hpp"
#include "Ui.hpp"  // PostToMain — the only UI dependency here, and only to marshal
#include "WalletBridgeRoute.hpp"

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
// AuthLogin{wallet_auth} blockchain ids. The server matches case-insensitively:
// "solana" -> ed25519, urnet::TAO ("TAO") -> sr25519 (bittensor).
constexpr const char* kSolanaBlockchain = "solana";

// One rpc, guarded: a throwing getter costs its OWN field (which then reads as
// UNKNOWN), not the whole snapshot. The failure is logged because the two bool
// fields have no unknown state to carry — false is all they can say.
template <typename T, typename Fn>
T ReadGuarded(const char* what, Fn&& fn, T fallback) {
  try {
    return fn();
  } catch (const std::exception& e) {
    g_warning("sdkhost: %s threw: %s", what, e.what());
  } catch (...) {
    g_warning("sdkhost: %s threw", what);
  }
  return fallback;
}

// ---- the remembered device-rpc session -------------------------------------
// UPSTREAM'S MODEL, ADOPTED. What is kept, and where:
//
//   $XDG_STATE_HOME/urnetwork/rpc/rpc_session.json  — NON-SECRET METADATA ONLY.
//     version, state, instance_id, rpc_session_id, host_port. 0600, in the
//     STATE dir rather than the config dir AppPrefs.hpp uses (this is a session
//     credential, not a preference, and must not land in a synced or backed-up
//     config tree), and in its OWN subdirectory rather than
//     $XDG_STATE_HOME/urnetwork itself: main.cpp hands that exact directory to
//     the SDK as its log dir and the SDK prunes it, so it is enumerated and
//     deleted from by another component and swept up wholesale by any "send us
//     your logs" flow.
//
//   the desktop Secret Service                      — THE TWO SECRETS.
//     client_pem (this GUI's mTLS private key) and server_cert_pem (the cert
//     it pins). RpcSessionStore.hpp's contract, and the reason this file no
//     longer writes a private key to disk at all.
//
// THE FILE THAT USED TO BE HERE. Our fork wrote all six fields — including
// BOTH private keys — as plaintext JSON at the same path, so that a relaunch
// could re-present the whole pinning triple and be adopted by
// TunnelHost::CanAdopt. That format has no `version` key, so RpcSessionStore
// reads it as "corrupt", which lands on the Unreadable disposition and gets it
// DELETED on the next launch. That deletion is not incidental: it is how two
// private keys finally leave the disk of every machine that ever ran the old
// build.
//
// BLOCKING D-BUS ON THE MAIN LOOP, deliberately and boundedly. Every call
// below is a synchronous libsecret round trip made from the GTK main loop with
// mutex_ held. That is upstream's shape, and it is the same trade StartTunnel
// already makes for a synchronous start_tunnel (up to 180 s). What is NOT
// acceptable, and what the callers below are built to guarantee, is a keyring
// failure of any kind stopping the user connecting: every one of them falls
// back to a fresh start_tunnel and reports the reason in the journal.

RpcSessionSecretStore& SessionSecretStore() {
  static SecretServiceRpcSessionStore store;
  return store;
}

std::string RpcSessionPath() {
  std::string dir = std::string(g_get_user_state_dir()) + "/urnetwork/rpc";
  g_mkdir_with_parents(dir.c_str(), 0700);
  return dir + "/rpc_session.json";
}

// Where the session file used to live, before it was moved out of the SDK's
// log dir. Only ever deleted, never read: adopting one costs nothing to skip,
// and leaving private keys behind in a directory that gets collected does.
std::string LegacyRpcSessionPath() {
  return std::string(g_get_user_state_dir()) + "/urnetwork/rpc_session.json";
}

// Best-effort removal of the metadata file itself, for the one disposition
// RemoveRpcSessionRecord cannot serve: a file it cannot PARSE (the old
// plaintext blob, a truncated write, a file that is not ours) never reaches
// its unlink, so it would otherwise sit on disk forever. There is no keyring
// item to orphan in that case — an unparseable file references nothing.
void UnlinkRpcSessionFile(const char* why) {
  const std::string path = RpcSessionPath();
  if (g_unlink(path.c_str()) == 0) {
    g_message("sdkhost: discarded the stored rpc session (%s)", why);
  } else if (errno != ENOENT) {
    g_warning("sdkhost: could not discard the stored rpc session (%s): %s", why,
              g_strerror(errno));
  }
}

// Drop the remembered session, both halves. Called when the session it names is
// over (Shutdown, Logout, a bind that never synced) and when the stored record
// is one that can never load again.
void ForgetRpcSession() {
  std::string diagnostic;
  if (!RemoveRpcSessionRecord(RpcSessionPath(), SessionSecretStore(), &diagnostic)) {
    // Two very different failures land here and only one of them may be
    // resolved by deleting the file. If the metadata could not be PARSED there
    // is no keyring item to strand, so the file must go. If the keyring itself
    // refused, the reference is the only way a later run can still clean the
    // item up, so it stays.
    const auto fault = rpcsession::FaultFromDiagnostic(diagnostic);
    if (fault == rpcsession::StoredSessionFault::Unreadable) {
      UnlinkRpcSessionFile(rpcsession::Explain(fault));
    } else {
      g_warning("sdkhost: could not forget the stored rpc session (%s); leaving the "
                "reference so a later run can still clean it up",
                diagnostic.empty() ? "no detail" : diagnostic.c_str());
    }
  }
  // The pre-Secret-Service file, unconditionally and every time: it holds two
  // private keys and nothing reads it.
  if (g_unlink(LegacyRpcSessionPath().c_str()) != 0 && errno != ENOENT) {
    g_warning("sdkhost: could not remove the legacy rpc session file: %s", g_strerror(errno));
  }
}

// The remembered session, or nullopt with the reason logged and the stored
// record disposed of per rpcsession::ShouldForget. NEVER throws and never
// blocks the caller from connecting: nullopt simply means "start a fresh
// session", which is always available.
std::optional<RpcSessionRecord> LoadRpcSession() {
  std::string diagnostic;
  auto record = LoadRpcSessionRecord(RpcSessionPath(), SessionSecretStore(), &diagnostic);
  if (record) {
    // Shape, separately from readability: the store proves the fields are
    // present, rpcsession::IsUsableRecord proves they are dialable (two
    // pairable uuids, two PEMs, and a port in our own draw range that is not
    // the 12025 this process holds).
    if (!rpcsession::IsUsableRecord(*record)) {
      g_warning("sdkhost: the stored rpc session is not in a usable shape; discarding it and "
                "starting a fresh session");
      ForgetRpcSession();
      return std::nullopt;
    }
    g_message("sdkhost: loaded the stored rpc session (%s)", diagnostic.c_str());
    return record;
  }

  const auto fault = rpcsession::FaultFromDiagnostic(diagnostic);
  if (fault == rpcsession::StoredSessionFault::Absent) return std::nullopt;
  if (rpcsession::ShouldForget(fault)) {
    g_message("sdkhost: %s; discarding it and starting a fresh session",
              rpcsession::Explain(fault));
    ForgetRpcSession();
  } else {
    // The credential may well still be good — a locked keyring at login is the
    // ordinary case — so it is KEPT and simply not used this time. Retaining it
    // costs one failed lookup next launch; discarding it would throw away a
    // working credential and orphan its keyring item.
    g_message("sdkhost: %s; keeping it and starting a fresh session this time",
              rpcsession::Explain(fault));
  }
  return std::nullopt;
}

// Persist a session that has DEMONSTRABLY SYNCED. See the call site
// (SdkHost::RememberSyncedSessionLocked): nothing is written until the pinned
// DeviceRemote reports remote_connected, so a record on disk always describes a
// pairing that really worked — which is the whole value of remembering one.
// A failure here is logged and otherwise ignored: the tunnel is up and
// carrying, and all that is lost is the ability to reattach to it next launch.
void SaveRpcSession(const RpcSessionRecord& record) {
  if (!rpcsession::IsUsableRecord(record)) {
    // Refuse to write what LoadRpcSession would refuse to read: a record that
    // cannot come back is worse than none, because we would have spent a
    // keyring write on it for nothing.
    g_warning("sdkhost: not persisting an unusable rpc session record (the next launch will "
              "start a fresh session)");
    return;
  }
  std::string diagnostic;
  if (!SaveRpcSessionRecord(RpcSessionPath(), record, SessionSecretStore(), &diagnostic)) {
    g_warning("sdkhost: could not remember this rpc session (%s); the tunnel is unaffected, "
              "the next launch will start a fresh session instead of reattaching",
              diagnostic.empty() ? "no detail" : diagnostic.c_str());
    return;
  }
  g_message("sdkhost: remembered this rpc session for reattachment (%s)", diagnostic.c_str());
}

// The name of ONE credential generation, minted by this GUI because this GUI
// owns half the material (client_pem and server_cert_pem never reach the
// daemon). g_uuid_string_random draws from the CSPRNG, which is the property
// StartTunnelRequest::rpc_session_id asks of the mint and that no wire check
// can verify; the canonical dashed form then satisfies both the daemon's
// ctl::LooksLikeRpcSessionId and our own tighter rpcsession::IsPairableId.
std::string MintRpcSessionId() {
  gchar* raw = g_uuid_string_random();
  std::string id = raw ? raw : "";
  g_free(raw);
  return id;
}

// A fresh loopback listener per session, drawn from [kRpcPortMin, kRpcPortMax]
// — a closed range that deliberately EXCLUDES ctl::kDeviceRpcPort (12025), so
// the daemon echoing our port back can never be a coincidence against a peer
// that ignored the pinning triple.
std::string RandomLoopbackRpcHostPort() {
  static std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> pick(ctl::kRpcPortMin, ctl::kRpcPortMax);
  return "127.0.0.1:" + std::to_string(pick(rng));
}

// How long a freshly pinned DeviceRemote gets to report remote_connected
// before the session is declared broken. This is the ONLY detector for a
// well-formed but MISMATCHED pair: nothing throws on either side, both ends
// bind and dial, the TLS handshake fails at connect time, and the symptom is
// every screen empty forever. Tune against a real bring-up.
constexpr int kRpcBindDeadlineSeconds = 8;

// The verify page's notice for a server that was asked to send a code.
VerifySendNotice SendErrorNotice(const std::optional<urnet::AuthVerifySendError>& sendError) {
  if (!sendError) return DecideVerifySendNotice(false, "", "", 0);
  return DecideVerifySendNotice(false, sendError->code, sendError->message,
                                sendError->retry_after_seconds.value_or(0));
}

// A verification_required answer, carrying whether the code was sent.
AuthResult VerificationRequired(const std::optional<urnet::AuthVerifySendError>& sendError) {
  AuthResult r{false, true, ""};
  r.sendNotice = SendErrorNotice(sendError);
  return r;
}

// Present while a sign-out is owed to the daemon (SignOut.hpp): beside the
// app's preferences (AppPrefs.hpp), where no local logout reaches.
std::string SignOutOwedPath() {
  const std::string dir = std::string(g_get_user_config_dir()) + "/urnetwork";
  g_mkdir_with_parents(dir.c_str(), 0700);
  return dir + "/sign_out_owed";
}

// This process's own glog files for the daemon's log upload (PassedLogFiles.hpp):
// the newest the sdk's upload would take from this process (its inventory,
// within the upload's cap), at most logupload::kMaxGuiLogFiles, each opened
// here with this process's rights. glog is flushed first, so the lines written
// up to the feedback are on disk. A file that cannot be opened is left out.
logupload::PassedLogFiles OpenGuiLogFiles() {
  logupload::PassedLogFiles files;
  try {
    urnet::flushGlog();
    const std::optional<urnet::LogFileInfoList> inventory = urnet::uploadLogsInventory();
    if (!inventory) return files;
    for (const urnet::LogFileInfo& info : *inventory) {
      if (files.Size() >= logupload::kMaxGuiLogFiles) break;
      if (!logupload::LooksLikeGlogFileName(info.Name)) continue;
      // non-blocking for the open only, so that nothing but a regular file can
      // hold this thread, and blocking again for the reads
      const int fd = ::open(info.Path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
      if (fd < 0) continue;
      struct stat fileStat{};
      if (::fstat(fd, &fileStat) != 0 || !S_ISREG(fileStat.st_mode)) {
        ::close(fd);
        continue;
      }
      const int flags = ::fcntl(fd, F_GETFL);
      if (flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
      files.Add(info.Name, fd);
    }
  } catch (const std::exception& e) {
    g_warning("support: listing this app's log files failed: %s", e.what());
  }
  return files;
}

static_assert(std::string_view(auth_logout::kSessionRevoked) ==
                  std::string_view(urnet::AuthLogoutCauseSessionRevoked),
              "the sign-in page's notice reads the sdk's session-revoked cause");

// Device::getAuthLogoutCause through a handle the caller does not own: a device
// listener runs on an sdk thread, where device_ is not its to read, and the
// DeviceRemote it was added on is not shared. A handle released meanwhile
// answers "" (the sdk never reuses one).
std::string DeviceAuthLogoutCause(uint64_t device) {
  char* cause = urnet_device_get_auth_logout_cause(device);
  if (cause == nullptr) return std::string();
  std::string out(cause);
  urnet_free_string(cause);
  return out;
}

}  // namespace

SdkHost::~SdkHost() {
  // Leg 3 of the kill switch runs here and holds `this`. Stop it before any
  // member dies; it never takes mutex_ while the joiner holds it, so the join
  // cannot deadlock.
  StopKillSwitchWorker();
  // The extender reset's worker holds `this` too (control_ and the marshal),
  // and never takes extenderResetWorkerMutex_. Unbounded like the join below,
  // for the same reason: a daemon that holds the verb holds quit.
  {
    std::scoped_lock resetLock(extenderResetWorkerMutex_);
    if (extenderResetWorker_.joinable()) extenderResetWorker_.join();
  }
  // The worker holds `this` and calls back into ReadReliability, so it must be
  // finished before any member dies. It never takes reliabilityWorkerMutex_,
  // so joining under that lock cannot deadlock. The join is NOT bounded — a
  // read blocked on mutex_ (held across a slow StartTunnel) or on a hung
  // daemon rpc can hold quit for seconds. That is the same trade the Developer
  // page's bridge makes, and it is the right one against a use-after-free.
  std::scoped_lock lock(reliabilityWorkerMutex_);
  if (reliabilityWorker_.joinable()) reliabilityWorker_.join();
  // The health poll's status read holds `this` too; unbounded for the same
  // reason, and it never takes daemonStatusWorkerMutex_.
  {
    std::scoped_lock statusLock(daemonStatusWorkerMutex_);
    if (daemonStatusWorker_.joinable()) daemonStatusWorker_.join();
  }
  // The provider_stats poll's timeout holds `this` as well.
  if (providerStatsPollId_ != 0) {
    g_source_remove(providerStatsPollId_);
    providerStatsPollId_ = 0;
  }
  // ...and so does a settling row click's, and the degrade hold's.
  if (rowConnectTimerId_ != 0) {
    g_source_remove(rowConnectTimerId_);
    rowConnectTimerId_ = 0;
  }
  {
    std::scoped_lock degradeLock(degradeMutex_);
    if (degradeReevalId_ != 0) g_source_remove(degradeReevalId_);
    degradeReevalId_ = 0;
  }
  // Last, and unconditionally: the reservation is the only member that is a
  // kernel resource rather than an SDK handle, and leaking it would keep the
  // address held by a zombie fd for the rest of the process.
  ReleaseDeviceRpcDefaultPort();
}

// See the contract on the declaration (SdkHost.hpp) for WHY this exists: the
// binding gives no already-pinned DeviceRemote constructor, so the unpinned
// first dial cannot be prevented — only aimed at a dead address.
bool SdkHost::HoldDeviceRpcDefaultPortLocked(std::string* error) {
  if (deviceRpcDefaultPortFd_ >= 0) return true;
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    if (error) *error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  // NO SO_REUSEADDR and NO SO_REUSEPORT, on purpose: the exclusive reservation
  // IS the mitigation, and either option would let a second process share the
  // address with us.
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(ctl::kDeviceRpcPort));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    const int err = errno;
    ::close(fd);
    if (error) {
      *error = std::string("bind 127.0.0.1:") + std::to_string(ctl::kDeviceRpcPort) + ": " +
               std::strerror(err);
    }
    return false;
  }
  // Deliberately no listen(): bound-but-not-listening holds the address AND
  // makes every connect() to it fail instantly with ECONNREFUSED, which is
  // exactly what the SDK's plaintext dialer must be given.
  deviceRpcDefaultPortFd_ = fd;
  return true;
}

void SdkHost::ReleaseDeviceRpcDefaultPort() {
  if (deviceRpcDefaultPortFd_ < 0) return;
  ::close(deviceRpcDefaultPortFd_);
  deviceRpcDefaultPortFd_ = -1;
}

// Every Api the GUI takes from its space comes through here, right after the
// assignment (Initialize, ApplyNetworkServer, SetPrivateExtender): a freshly
// derived Api reports the unknown client until it is told what it is, and
// the account's Sessions list would then show this machine's uses as
// "Unknown device".
void SdkHost::AdoptSpaceApiLocked() {
  // requires mutex_
  ReportClientInfo(*api_, kAppVersion);
  // ...and the Api's own sign-out is followed. The sdk clears the account
  // credential the server rejects (a confirmed 401), or the one this app
  // signed out from the account's Sessions list, and says so on the Api it
  // was rejected on. That is the sign-out the device's listener reports for
  // the client credential (BindRemoteDeviceLocked), so it goes to the same
  // handler, which marshals and runs Logout(). The device listener alone
  // missed it: this Api carries its own credential, and without a tunnel
  // there is no device to report anything. Logout's own setByJwt("") does not
  // fire it. A new subscription goes with every new Api, so a rejection on a
  // replaced one cannot sign the next account out.
  //
  // The listener reads why (Api.getAuthLogoutCause, set before the listeners
  // run) on the sdk thread it runs on, before anything is posted, through a
  // handle of its own on this Api: api_ is mutex_'s.
  apiLogoutSub_.reset();
  auto api = std::make_shared<urnet::Api>(networkSpace_->getApi());
  apiLogoutSub_.emplace(api_->addAuthLogoutListener([this, api] {
    // an sdk thread: read, then marshal only (MainWindow's handler posts Logout)
    ReportAuthLogout(api->getAuthLogoutCause());
  }));
}

void SdkHost::ReportAuthLogout(std::string cause) {
  // the sign-in it was heard in goes with it: a second listener's report of
  // the same rejection, or one that lands after a sign-out, signs nothing out
  if (onAuthInvalid_) onAuthInvalid_(authLogouts_.Hear(std::move(cause)));
}

bool SdkHost::Initialize(const std::string& storageDir, const std::string& logDir) {
  std::scoped_lock lock(mutex_);
  initializeError_.clear();
  // A sign-out an earlier run could not deliver: the first reconcile (the
  // health poll's) or a Connect delivers it before anything starts.
  signOut_.Load();
  if (signOut_.Owed()) {
    g_message("sdkhost: a sign-out is still owed to the daemon from an earlier run; it is "
              "delivered before anything starts");
  }
  try {
    urnet::setLogDir(logDir);
    urnet::setMemoryLimit(kMemoryLimit);
    spaceManager_ = urnet::newNetworkSpaceManager(storageDir);
    // moves a space stored under the retired ur.network key first, then
    // builds the bundled space, then binds the space the user last chose in
    // the network sheet -- the manager persisted it as active -- so a custom
    // server survives a relaunch, its jwt with it (NetworkSpaceBootstrap.hpp).
    // URNETWORK_NETWORK_HOST binds a test network instead, for this process.
    // Nothing below may take a NetworkSpace from the manager before this.
    const LaunchOverride launchOverride = LaunchOverrideFromEnvironment();
    if (launchOverride.Active()) {
      g_warning("sdkhost: NETWORK OVERRIDE host=%s env=%s. This client is NOT talking to "
                "production.",
                launchOverride.host.c_str(), launchOverride.env.c_str());
    }
    networkSpace_ = LaunchUrNetworkSpace(*spaceManager_, launchOverride);
    if (const std::string hostName = networkSpace_->getHostName();
        !launchOverride.Active() && hostName != kUrHostName) {
      g_message("sdkhost: restored the network space this client was last pointed at: '%s'",
                hostName.c_str());
    }
    api_ = networkSpace_->getApi();
    AdoptSpaceApiLocked();
    asyncLocalState_ = networkSpace_->getAsyncLocalState();
    localState_ = asyncLocalState_->getLocalState();
    // a launch signed in (IsLoggedIn) is a sign-in the server can reject
    if (!localState_->getByClientJwt().empty()) authLogouts_.SignedIn();
    // the SDK's client event queue over this network space: it persists,
    // batches and sends the product events (ClientEvents.hpp)
    events_ = std::make_unique<ClientEventQueue>(networkSpace_->handle(), UR_APP_VERSION,
                                                 ClientEventLocale());
    // RESTORE THE API'S AUTHORIZATION FROM THE PERSISTED SESSION.
    //
    // api_->setByJwt is called in exactly one other place — RegisterNetworkClient,
    // the fresh-sign-in path — so the token otherwise lives only in the Api of
    // the process that did the login. A relaunch rebuilds the Api with no token
    // while the app still LOOKS signed in (the client jwt is on disk, so
    // IsLoggedIn() is true and every page runs its loads), and the SDK only
    // re-authorizes the Api as a side effect of creating a DeviceRemote — which
    // needs urnetworkd to be up. With no daemon, or before the tunnel is
    // started, every authenticated read 401s and each page renders that as its
    // own failure state. The Windows host already carries this restore
    // (urnetwork-windows/app/src/App/SdkHost.cpp:353-357).
    //
    // getByJwt() is the USER jwt the Api authorizes with; getByClientJwt() is
    // the device credential the tunnel session needs — they are not the same.
    if (const std::string byJwt = localState_->getByJwt(); !byJwt.empty()) {
      api_->setByJwt(byJwt);
    }
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
    initializeError_ = e.what();
    return false;
  }
}

bool SdkHost::IsLoggedIn() {
  std::scoped_lock lock(mutex_);
  // A sign-out is the answer at once: the stored jwt outlives it until the
  // asynchronous local logout lands (Logout).
  return !signedOut_.load() && localState_ && !localState_->getByClientJwt().empty();
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
    if (result->verification_required) {
      done(VerificationRequired(result->verification_required->send_error));
      return;
    }
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

namespace {
// lowercase, trimmed, single-spaced — the normalization every client applies
// before sending a seedphrase, so a phrase pasted with newlines or double
// spaces authenticates (windows SdkHost / macOS LoginSeedphraseViewModel).
std::string NormalizeSeedphrase(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  bool pendingSpace = false;
  for (unsigned char c : raw) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}
}  // namespace

void SdkHost::LoginWithSeedphrase(const std::string& seedphrase,
                                  std::function<void(AuthResult)> done) {
  urnet::AuthLoginArgs args;
  args.seedphrase = NormalizeSeedphrase(seedphrase);
  // NOTE: nothing on any path below may echo the args — an error log that
  // included the request would put the credential in a file on disk.
  api_->authLogin(args, [this, done](std::optional<urnet::AuthLoginResult> result,
                                     std::optional<std::string> err) {
    if (err) { done({false, false, *err}); return; }
    if (!result) { done({false, false, "no result"}); return; }
    if (result->error && !result->error->message.empty()) {
      // a wrong phrase is a form error, not a session error
      done({false, false, result->error->message});
      return;
    }
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, done);
      return;
    }
    done({false, false, "seedphrase login returned no network"});
  });
}

void SdkHost::CreateInstantAccount(const std::string& referralCode,
                                   std::function<void(InstantAccount)> done) {
  // NO user_auth, password, auth_jwt or wallet_auth: that combination is what
  // makes the server mint a seedphrase-secured network and return the phrase.
  urnet::NetworkCreateArgs args;
  ApplySignupPreferences(args);
  args.terms = true;  // the form's button is gated on the terms consent
  if (!referralCode.empty()) args.referral_code = referralCode;
  api_->networkCreate(args, [this, done](std::optional<urnet::NetworkCreateResult> result,
                                         std::optional<std::string> err) {
    InstantAccount out;
    if (err || !result) {
      out.error = err ? *err : "no result";
      if (done) done(out);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      out.error = result->error->message;
      if (done) done(out);
      return;
    }
    if (result->verification_required) {
      // an instant account carries no user auth: nothing could take a code
      out.error = "the server asked to verify an account with no user auth";
      if (done) done(out);
      return;
    }
    if (!result->seedphrase || result->seedphrase->empty()) {
      // Refuse to register: a network whose only credential never reached the
      // user is an account nobody can ever get back into.
      out.error = "instant account returned no seedphrase";
      if (done) done(out);
      return;
    }
    if (!result->network || !result->network->by_jwt || result->network->by_jwt->empty()) {
      out.error = "instant account returned no network";
      if (done) done(out);
      return;
    }
    {
      std::scoped_lock lock(mutex_);
      pendingInstantJwt_ = *result->network->by_jwt;
    }
    out.ok = true;
    out.seedphrase = *result->seedphrase;
    if (done) done(out);
  });
}

void SdkHost::ConfirmInstantAccount(std::function<void(AuthResult)> done) {
  std::string jwt;
  {
    std::scoped_lock lock(mutex_);
    if (!pendingInstantJwt_) {
      if (done) done({false, false, "no instant account is pending"});
      return;
    }
    jwt = *pendingInstantJwt_;
    pendingInstantJwt_.reset();
  }
  RegisterNetworkClient(jwt, done);
}

void SdkHost::DiscardInstantAccount() {
  std::scoped_lock lock(mutex_);
  pendingInstantJwt_.reset();
}

// ---- Advanced Mode (the windows D5 standing-state contract) -----------------

bool SdkHost::CurrentAdvancedMode() {
  if (!advancedModeLoaded_) {
    advancedMode_.store(prefs::Get<bool>("advanced_mode", false), std::memory_order_release);
    advancedModeLoaded_ = true;
  }
  return advancedMode_.load(std::memory_order_acquire);
}

void SdkHost::SetAdvancedMode(bool on) {
  // persist FIRST, publish second: a crash between the two must lose the
  // publish, never the preference
  prefs::Set("advanced_mode", on);
  advancedMode_.store(on, std::memory_order_release);
  advancedModeLoaded_ = true;
  if (onAdvancedMode_) onAdvancedMode_(on);
}

void SdkHost::SetAdvancedModeHandler(std::function<void(bool)> h) {
  onAdvancedMode_ = std::move(h);
}

void SdkHost::RefreshAdvancedMode() {
  if (onAdvancedMode_) onAdvancedMode_(CurrentAdvancedMode());
}

// ---- network server (iOS NetworkServerSheet / windows parity) ---------------

SdkHost::NetworkServer SdkHost::CurrentNetworkServer() {
  std::scoped_lock lock(mutex_);
  NetworkServer out;
  out.managerAvailable = spaceManager_.has_value();
  // the same resolution the launch uses, so "Use default network" means the
  // network this process was started against — never silently production
  const LaunchOverride launchOverride = LaunchOverrideFromEnvironment();
  out.defaultHostName = launchOverride.Active() ? launchOverride.host : std::string(kUrHostName);
  if (!networkSpace_) return out;
  try {
    out.hostName = networkSpace_->getHostName();
    out.apiUrl = networkSpace_->getApiUrl();
    out.connectUrl = networkSpace_->getPlatformUrl();
    out.configuredApiUrl = networkSpace_->getConfiguredApiUrl();
    out.configuredConnectUrl = networkSpace_->getConfiguredPlatformUrl();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] read network space failed: %s\n", e.what());
  }
  return out;
}

bool SdkHost::ApplyNetworkServer(const std::string& hostName, const std::string& apiUrl,
                                 const std::string& connectUrl) {
  if (hostName.empty()) return false;
  bool ok = false;
  bool loggedIn = false;
  {
    std::scoped_lock lock(mutex_);
    if (!spaceManager_) return false;

    // A different space is a different LocalState and so a different stored
    // jwt: a running session belongs to the OLD server and cannot survive it.
    TeardownDeviceLocked();
    control_.StopTunnel();  // best effort — signed-out screens have none
    pendingWalletAuth_.reset();
    pendingInstantJwt_.reset();

    try {
      // the override's own host is keyed under the override's env, as the
      // launch built it (NetworkSpaceBootstrap.hpp)
      const LaunchOverride launchOverride = LaunchOverrideFromEnvironment();
      const std::string envName = EnvNameFor(launchOverride, hostName);
      const bool official = hostName == kUrHostName && envName == kUrEnvName;
      const bool explicitUrls = !apiUrl.empty() || !connectUrl.empty();

      urnet::NetworkSpaceKey key;
      key.host_name = hostName;
      key.env_name = envName;

      // The same host values BuildUrNetworkSpace writes, with the
      // host-dependent parts varied (iOS DeviceManager.applyNetworkSpace
      // parity), written OVER what the space already stores under this key --
      // nothing for a server this client never used. updateNetworkSpaceValues
      // replaces the whole set, and a set built from nothing dropped what the
      // user had saved in the space (its VLESS server, its private extender)
      // whenever the sheet re-applied the server in force. Only what this
      // sheet decides changes: the host's values and the url overrides.
      // `bundled` is true only for the official host with no overrides: a
      // bundled space carries pinned endpoints a custom deployment does not
      // have.
      urnet::NetworkSpaceValues values = UrNetworkSpaceValuesOver(
          StoredNetworkSpaceValues(*spaceManager_, key), official, hostName);
      values.bundled = official && !explicitUrls;
      values.api_url = apiUrl;
      values.platform_url = connectUrl;

      networkSpace_ = spaceManager_->updateNetworkSpaceValues(key, values);
      // the user's choice persists, but the override's space stays bound for
      // this process only
      if (!IsLaunchOverrideSpace(launchOverride, hostName, envName)) {
        spaceManager_->setActiveNetworkSpace(*networkSpace_);
      }

      // everything derived from the space re-derives: the Api talks to the
      // new host, the LocalState holds the new host's jwt
      api_ = networkSpace_->getApi();
      AdoptSpaceApiLocked();
      asyncLocalState_ = networkSpace_->getAsyncLocalState();
      localState_ = asyncLocalState_->getLocalState();
      // ...INCLUDING the Api's authorization. Same defect as Initialize(): a
      // freshly derived Api carries no token, so switching to a space this
      // device is ALREADY signed in to would leave every authenticated read
      // 401ing while the app still looked signed in.
      if (const std::string byJwt = localState_->getByJwt(); !byJwt.empty()) {
        api_->setByJwt(byJwt);
      }
      networkNameVc_ = urnet::newNetworkNameValidationViewController(*api_);
      loggedIn = !localState_->getByClientJwt().empty();
      // the new space's stored credential is the answer, not a sign-out made
      // in another space
      signedOut_.store(!loggedIn);
      // ...and a report from the replaced space signs nothing out
      if (loggedIn) {
        authLogouts_.SignedIn();
      } else {
        authLogouts_.SignedOut();
      }
      ok = true;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[sdk] switch network space to '%s' failed: %s\n",
                   hostName.c_str(), e.what());
      ok = false;
    }
  }
  if (!ok) return false;
  // The new space's stored auth decides what the window shows. Almost always
  // LoggedOut — a fresh server has no jwt — and saying so is the point: the
  // old session is genuinely gone.
  if (onAuth_) onAuth_(loggedIn);
  EmitDrawerEvent(DrawerEvent::DeviceLifecycle);
  // stop_tunnel retired the old server's provider-only device with the rest; a
  // space this device is already signed in to provides again on its own.
  ReconcileProviderAfterSpaceChange("network server changed");
  return true;
}

std::string SdkHost::NetworkSpaceJson() {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return "";
  try {
    return networkSpace_->toJson();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] network space toJson failed: %s\n", e.what());
    return "";
  }
}

// ---- VLESS (sdk vless_settings_ui.go) ----------------------------------------
// Nothing here logs the settings: the user id is the server's credential.

std::optional<urnet::VlessSettings> SdkHost::GetVlessSettings() {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return std::nullopt;
  try {
    return networkSpace_->getVlessSettings();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] getVlessSettings failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::optional<std::string> SdkHost::SetVlessSettings(const urnet::VlessSettings& settings) {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return std::nullopt;
  try {
    // The SDK persists through the space's manager and applies the change IN
    // PLACE (sdk updateInPlaceValues): the manager keeps this same space, so
    // networkSpace_ and the Api, LocalState and device derived from it all
    // stay valid -- nothing to re-derive, unlike ApplyNetworkServer.
    std::optional<std::string> answer = networkSpace_->setVlessSettings(settings);
    // "" is a save; a provider-only device takes the new space now.
    if (answer && answer->empty()) ReconcileProviderAfterSpaceChange("VLESS settings saved");
    return answer;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] setVlessSettings failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::optional<urnet::VlessLinkResult> SdkHost::ParseVlessLink(const std::string& link) {
  try {
    return urnet::parseVlessLink(link);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] parseVlessLink failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::string SdkHost::VlessSettingsLink(const urnet::VlessSettings& settings) {
  try {
    return urnet::vlessSettingsLink(settings);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] vlessSettingsLink failed: %s\n", e.what());
    return "";
  }
}

std::string SdkHost::ValidateVlessSettings(const urnet::VlessSettings& settings) {
  try {
    return urnet::validateVlessSettings(settings);
  } catch (const std::exception& e) {
    // a check that could not run is not a pass, and no refusal of the
    // settings either: the C ABI's own answer for a call that could not run,
    // from its header, the id's one source in this app
    std::fprintf(stderr, "[sdk] validateVlessSettings failed: %s\n", e.what());
    return URNET_ERROR_ID_INTERNAL;
  }
}

// ---- bootstrap DNS-over-HTTPS servers (sdk control_doh_ui.go) ---------------
// Nothing here logs the servers: which resolver a user can reach says where
// they are.

std::optional<std::vector<std::string>> SdkHost::GetControlDohUrls() {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return std::nullopt;
  try {
    return networkSpace_->getControlDohUrls().value_or(urnet::StringList());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] getControlDohUrls failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::optional<std::string> SdkHost::SetControlDohUrls(const std::vector<std::string>& urls) {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return std::nullopt;
  try {
    // Through the space's own setter, not the values write SetPrivateExtender
    // makes: the setter validates every line (an https url on an ip literal),
    // drops repeats, normalizes and answers the error id, and it applies in
    // place (sdk updateInPlaceValues) -- the strategy's DoH cache is swapped
    // and networkSpace_, with everything derived from it, stays valid.
    std::optional<std::string> answer = networkSpace_->setControlDohUrls(urnet::StringList(urls));
    // "" is a save. The tunnel takes the servers at the next connect (the
    // section's note); a provider-only device takes them now.
    if (answer && answer->empty()) ReconcileProviderAfterSpaceChange("DoH servers saved");
    return answer;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] setControlDohUrls failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::vector<std::string> SdkHost::RegionalControlDohUrls(const std::string& countryCode) {
  try {
    return urnet::regionalControlDohUrls(countryCode).value_or(urnet::StringList());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] regionalControlDohUrls failed: %s\n", e.what());
    return {};
  }
}

// ---- sign-up / verify / password reset (Phase 3) ----------------------------

// NetworkCreateResult routing shared by the password and wallet sign-ups:
// by_jwt -> RegisterNetworkClient; verification_required -> the verify page.
void SdkHost::HandleNetworkCreateResult(std::optional<urnet::NetworkCreateResult> result,
                                        std::optional<std::string> err,
                                        std::function<void(AuthResult)> done,
                                        const std::string& bittensorWalletId) {
  if (err) { done({false, false, *err}); return; }
  if (!result) { done({false, false, "no result"}); return; }
  if (result->error && !result->error->message.empty()) {
    AuthResult r{false, false, result->error->message};
    r.errorCode = result->error->code.value_or(std::string());
    r.bittensorWalletId = bittensorWalletId;
    done(r);
    return;
  }
  if (result->verification_required) {
    done(VerificationRequired(result->verification_required->send_error));
    return;
  }
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
  ApplySignupPreferences(args);
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
  CancelPendingSolanaConnect("superseded by wallet network creation");
  std::optional<urnet::WalletAuthArgs> walletAuth;
  uint64_t flow = 0;
  {
    std::scoped_lock lock(mutex_);
    flow = walletFlows_.Begin();  // this flow signs through the bridge too
    walletAuth = pendingWalletAuth_;
  }
  if (!walletAuth) {
    done({false, false, "no wallet sign-in pending"});
    return;
  }

  if (walletAuth->blockchain.value_or(std::string()) == urnet::TAO) {
    // the second signature, over a challenge bound to the signed-in address,
    // from the same wallet the sign-in used
    std::string walletId;
    {
      std::scoped_lock lock(mutex_);
      walletId = bittensorWalletId_;
    }
    auto shared = std::make_shared<std::function<void(AuthResult)>>(std::move(done));
    StartBittensorSession(
        walletId, std::string(bittensor::kPurposeCreate),
        walletAuth->wallet_address.value_or(std::string()), flow,
        [this, networkName, referralCode, shared](const std::string& message) {
          if (!*shared) return false;
          std::scoped_lock lock(mutex_);
          if (!pendingWalletAuth_) {
            (*shared)({false, false, "no wallet sign-in pending"});
            return false;
          }
          pendingWalletAuth_->wallet_message = message;
          pendingWalletAuth_->wallet_signature = std::string();
          pendingWalletNetworkName_ = networkName;
          pendingWalletReferralCode_ = referralCode;
          walletCreateDone_ = std::move(*shared);
          *shared = nullptr;
          return true;
        },
        [shared](const std::string& error) {
          // before the flow's slot is set: answer it here, once
          if (!*shared) return;
          auto done = std::move(*shared);
          *shared = nullptr;
          done({false, false, error});
        });
    return;
  }

  RequestWalletChallenge(walletAuth->blockchain.value_or(std::string()),
                         walletAuth->wallet_address.value_or(std::string()),
                         [this, flow, networkName, referralCode, done = std::move(done)](
                             std::optional<std::string> message, std::string error) mutable {
    if (!message) {
      done({false, false, error.empty() ? "could not fetch wallet challenge" : error});
      return;
    }

    PostToMain([this, flow, networkName, referralCode, message = *message,
                done = std::move(done)]() mutable {
      WalletConnect::Provider provider;
      {
        std::scoped_lock lock(mutex_);
        if (!walletFlows_.IsCurrent(flow)) {
          // another wallet flow took the bridge while the challenge was fetched
          done({false, false, "superseded by another wallet flow"});
          return;
        }
        if (!pendingWalletAuth_) {
          done({false, false, "no wallet sign-in pending"});
          return;
        }
        pendingWalletAuth_->wallet_message = message;
        pendingWalletAuth_->wallet_signature = std::string();
        pendingWalletNetworkName_ = networkName;
        pendingWalletReferralCode_ = referralCode;
        walletCreateDone_ = std::move(done);
        provider = wallet_.provider();
      }

      (void)provider;
      wallet_.SignMessage(message);  // solana (bittensor takes the session path above)
    });
  });
}

bool SdkHost::HasPendingWalletAuth() {
  std::scoped_lock lock(mutex_);
  return pendingWalletAuth_.has_value();
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
                               std::function<void(VerifySendNotice notice)> done) {
  urnet::AuthVerifySendArgs args;
  args.user_auth = userAuth;
  args.use_numeric = true;  // the verify page's OTP entry is numeric
  // a rate limit or failed send comes back as result->error (with the retry
  // time) instead of an HTTP 429 / 502
  args.result_errors = true;
  api_->authVerifySend(args, [done](std::optional<urnet::AuthVerifySendResult> result,
                                    std::optional<std::string> err) {
    if (err || !result) {
      done(DecideVerifySendNotice(true, "", "", 0));
      return;
    }
    done(SendErrorNotice(result->error));
  });
}

void SdkHost::SendPasswordResetLink(const std::string& userAuth,
                                    std::function<void(VerifySendNotice notice)> done) {
  urnet::AuthPasswordResetArgs args;
  args.user_auth = userAuth;
  // a rate limit or failed send comes back as result->error (with the retry
  // time) instead of an HTTP 429 / 502; it used to be read as sent
  args.result_errors = true;
  api_->authPasswordReset(args, [done](std::optional<urnet::AuthPasswordResetResult> result,
                                       std::optional<std::string> err) {
    if (err || !result) {
      done(DecideVerifySendNotice(true, "", "", 0));
      return;
    }
    done(SendErrorNotice(result->error));
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
  // Every return goes to the flow waiting for it (WalletBridgeRoute.hpp), and
  // one nobody waits for is dropped. The bridge page keeps "Return to
  // URnetwork" on screen after its automatic redirect and the key pair lives
  // until the next Connect, so a second delivery of a connect return decrypts
  // again: it must never become a wallet sign-in in a signed-in app. A wallet
  // sign-in is waiting exactly when walletAuthDone_ is set.
  //
  // Solana signs in in two hops: connect first, then ask the wallet to sign the
  // challenge. (Bittensor never fires this — it signs in a single hop.)
  wallet_.on_public_key = [this](std::string publicKey, WalletConnect::Provider provider) {
    std::function<void(SolanaConnectResult)> connectDone;
    bridge::PublicKeyRoute route = bridge::PublicKeyRoute::Drop;
    uint64_t flow = 0;
    {
      std::scoped_lock lock(mutex_);
      flow = walletFlows_.Latest();
      route = bridge::RoutePublicKey(provider == WalletConnect::Provider::Bittensor,
                                     static_cast<bool>(walletConnectDone_),
                                     static_cast<bool>(walletAuthDone_),
                                     static_cast<bool>(walletAddDone_));
      if (route == bridge::PublicKeyRoute::AnswerConnect) {
        connectDone = std::move(walletConnectDone_);
        walletConnectDone_ = nullptr;
      }
    }
    switch (route) {
      case bridge::PublicKeyRoute::Drop:
        std::fprintf(stderr,
                     "[wallet] a connect return arrived with no flow in flight, ignoring it\n");
        return;
      case bridge::PublicKeyRoute::AnswerConnect: {
        // a plain connect (ConnectSolanaWallet) wants the key itself: no
        // challenge, no signature
        SolanaConnectResult out;
        out.ok = true;
        out.address = std::move(publicKey);
        connectDone(std::move(out));
        return;
      }
      case bridge::PublicKeyRoute::SignIn:
      case bridge::PublicKeyRoute::AddSignIn:
        // both sign a fresh challenge; the signature route decides where it goes
        break;
    }
    RequestWalletChallenge(kSolanaBlockchain, publicKey,
                           [this, flow](std::optional<std::string> message, std::string error) {
      if (!message) {
        // a newer wallet flow owns the slots now: it is not failed for this one
        if (!WalletFlowIsCurrent(flow)) return;
        FailWalletOperation(error.empty() ? "could not fetch wallet challenge" : error);
        return;
      }
      PostToMain([this, flow, message = *message] {
        if (!WalletFlowIsCurrent(flow)) return;  // a newer flow owns the bridge now
        wallet_.SignMessage(message);
      });
    });
  };
  // Either way the wallet address is on the WalletConnect by now: solana set it
  // on the connect callback, bittensor returns it alongside the signature.
  wallet_.on_signature = [this](std::string signature) {
    // A plain signing request (SignBittensorConnect) never authenticates: the
    // signature goes back to the caller with the address and the message. A
    // signature nobody waits for (a superseded or abandoned tab) is dropped: it
    // must not reach AuthLoginWithWallet, which would move the session.
    // A wallet being added (AddSignInWithSolana / AddSignInWithBittensor) goes
    // to add-auth on this session, ahead of any sign-in.
    std::function<void(WalletSignature)> signDone;
    std::function<void(AddSignInResult)> addDone;
    addsignin::WalletChain addChain = addsignin::WalletChain::Solana;
    bridge::SignatureRoute route = bridge::SignatureRoute::Drop;
    {
      std::scoped_lock lock(mutex_);
      route = bridge::RouteSignature(static_cast<bool>(walletSignDone_),
                                     static_cast<bool>(walletCreateDone_),
                                     static_cast<bool>(walletAuthDone_),
                                     static_cast<bool>(walletAddDone_));
      if (route == bridge::SignatureRoute::AnswerRequest) {
        signDone = std::move(walletSignDone_);
        walletSignDone_ = nullptr;
      } else if (route == bridge::SignatureRoute::AnswerAdd) {
        addDone = std::move(walletAddDone_);
        walletAddDone_ = nullptr;
        addChain = walletAddChain_;
      }
    }
    switch (route) {
      case bridge::SignatureRoute::Drop:
        std::fprintf(stderr,
                     "[wallet] a wallet signature arrived with no flow in flight, ignoring it\n");
        return;
      case bridge::SignatureRoute::AnswerRequest: {
        WalletSignature out;
        out.ok = true;
        out.address = wallet_.publicKey();
        out.signature = std::move(signature);
        out.message = wallet_.message();
        signDone(std::move(out));
        return;
      }
      case bridge::SignatureRoute::AnswerAdd:
        AddAuthMethod(addsignin::WalletArgs(addChain, wallet_.publicKey(), signature,
                                            wallet_.message()),
                      std::move(addDone));
        return;
      case bridge::SignatureRoute::FinishCreate:
        FinishCreateNetworkWithWallet(signature);
        return;
      case bridge::SignatureRoute::SignIn:
        break;
    }
    const bool bittensor = wallet_.provider() == WalletConnect::Provider::Bittensor;
    AuthLoginWithWallet(wallet_.publicKey(), signature, wallet_.message(),
                        bittensor ? urnet::TAO : kSolanaBlockchain);
  };
  // The sign-in return (the api's oauth callback): only the attempt in flight is accepted (echoed
  // state, token minted for the nonce), then the identity token signs in.
  wallet_.on_sso = [this](std::string provider, std::string jwt, std::string state,
                          std::string error) {
    sso::Return r;
    r.provider = provider;
    r.authJwt = jwt;
    r.state = state;
    r.error = error;
    sso::Verdict verdict;
    std::string expectedProvider;
    addsignin::Owner owner = addsignin::Owner::None;
    std::function<void(AddSignInResult)> addDone;
    {
      std::scoped_lock lock(mutex_);
      verdict = sso::CheckReturn(r, ssoProvider_, ssoState_, ssoNonce_);
      expectedProvider = ssoProvider_;
      // a return that echoes the state ends the attempt either way; a stray
      // one (no attempt, another state) leaves a live attempt untouched
      if (r.state == ssoState_ && !ssoState_.empty()) {
        owner = ssoOwner_;
        if (owner == addsignin::Owner::AddSignIn) {
          addDone = std::move(ssoAddDone_);
          ssoAddDone_ = nullptr;
        }
        ssoOwner_ = addsignin::Owner::None;
        ssoProvider_.clear();
        ssoState_.clear();
        ssoNonce_.clear();
      }
    }
    if (!verdict.ok) {
      std::fprintf(stderr, "[sso] rejected return for %s: %s\n", provider.c_str(),
                   verdict.error.c_str());
      // a stray return must not fail the attempt in flight
      if (verdict.error == "unexpected sign-in return" && !expectedProvider.empty() &&
          r.state != state) {
        return;
      }
      if (owner == addsignin::Owner::AddSignIn) {
        if (addDone) addDone({false, verdict.error});
        return;
      }
      FailWalletOperation(verdict.error);
      return;
    }
    switch (addsignin::RouteSso(owner)) {
      case addsignin::SsoRoute::AddSignIn: {
        // the identity token is added to this network; it never signs in
        const addsignin::Method method = provider == sso::kProviderApple
                                             ? addsignin::Method::Apple
                                             : addsignin::Method::Google;
        AddAuthMethod(addsignin::ProviderArgs(method, jwt), std::move(addDone));
        return;
      }
      case addsignin::SsoRoute::SignIn:
        AuthLoginWithSso(provider, jwt);
        return;
      case addsignin::SsoRoute::Drop:
        std::fprintf(stderr, "[sso] a return with no owner, ignoring it\n");
        return;
    }
  };
  wallet_.on_error = [this](std::string err) {
    // walletAuthDone_ is set on the UI thread and consumed on wallet/SDK
    // callback threads: take it under the lock, invoke it outside
    std::function<void(SolanaConnectResult)> connectDone;
    std::function<void(WalletSignature)> signDone;
    std::function<void(AddSignInResult)> addDone;
    std::function<void(AuthResult)> done;
    {
      std::scoped_lock lock(mutex_);
      // a plain connect is answered first, and alone: every other wallet flow
      // cancels a waiting connect when it starts, so a connect still waiting
      // is the flow that opened the bridge last
      connectDone = std::move(walletConnectDone_);
      walletConnectDone_ = nullptr;
      if (!connectDone) {
        signDone = std::move(walletSignDone_);
        walletSignDone_ = nullptr;
        if (!signDone) {
          addDone = std::move(walletAddDone_);
          walletAddDone_ = nullptr;
        }
        if (walletCreateDone_) {
          done = std::move(walletCreateDone_);
          pendingWalletNetworkName_.clear();
          pendingWalletReferralCode_.clear();
        } else {
          done = std::move(walletAuthDone_);
        }
        walletAuthDone_ = nullptr;
        walletCreateDone_ = nullptr;
      }
    }
    if (connectDone) {
      SolanaConnectResult out;
      out.error = err;
      connectDone(std::move(out));
      return;
    }
    if (signDone) {
      WalletSignature out;
      out.error = err;
      signDone(std::move(out));
      return;
    }
    if (addDone) {
      addDone({false, err});
      return;
    }
    if (done) done({false, false, err});
  };
}

void SdkHost::CancelPendingSolanaConnect(const std::string& reason) {
  std::function<void(SolanaConnectResult)> connectDone;
  {
    std::scoped_lock lock(mutex_);
    connectDone = std::move(walletConnectDone_);
    walletConnectDone_ = nullptr;
  }
  if (!connectDone) return;
  SolanaConnectResult out;
  out.error = reason;
  connectDone(std::move(out));
}

bool SdkHost::WalletFlowIsCurrent(uint64_t flow) {
  {
    std::scoped_lock lock(mutex_);
    if (walletFlows_.IsCurrent(flow)) return true;
  }
  std::fprintf(stderr, "[wallet] a challenge arrived for a superseded wallet flow; the bridge "
                       "stays with the newer one\n");
  return false;
}

void SdkHost::ConnectSolanaWallet(WalletConnect::Provider provider,
                                  std::function<void(SolanaConnectResult)> done) {
  if (provider == WalletConnect::Provider::Bittensor) {
    // Bittensor has no connect hop on the bridge (it signs in one)
    SolanaConnectResult out;
    out.error = "not a solana wallet provider";
    if (done) done(std::move(out));
    return;
  }
  CancelPendingSolanaConnect("superseded by a wallet connect request");
  CancelPendingAddSignIn("superseded by a wallet connect request");
  // The bridge is this request's now. A Bittensor connect still waiting for its
  // signature is answered (the page settles it quietly), and its challenge, if
  // it is still being fetched, will not open the bridge over this one: the flow
  // number moves on.
  std::function<void(WalletSignature)> signDone;
  {
    std::scoped_lock lock(mutex_);
    walletFlows_.Begin();
    signDone = std::move(walletSignDone_);
    walletSignDone_ = nullptr;
  }
  if (signDone) {
    WalletSignature out;
    out.error = "superseded by a wallet connect request";
    signDone(std::move(out));
  }
  {
    std::scoped_lock lock(mutex_);
    walletConnectDone_ = std::move(done);
  }
  // opens the browser; the key comes back on the urnetwork://<provider>-connect
  // callback (on_public_key) and a failure on on_error -- a browser that cannot
  // be opened is answered before this returns. The payout sheet also takes a
  // typed address, so a missing extension points at it.
  wallet_.Connect(provider, /*offersManualEntry=*/true);
}

void SdkHost::SignInWithSolana(WalletConnect::Provider provider,
                               std::function<void(AuthResult)> done) {
  CancelPendingSolanaConnect("superseded by a wallet sign-in");
  CancelPendingAddSignIn("superseded by a wallet sign-in");
  {
    std::scoped_lock lock(mutex_);
    walletFlows_.Begin();
    pendingWalletAuth_.reset();
    walletAuthDone_ = std::move(done);
  }
  wallet_.Connect(provider);  // opens the browser; the rest continues on the deep-link callback
}

void SdkHost::SignInWithBittensor(const std::string& walletId,
                                  std::function<void(AuthResult)> done) {
  CancelPendingSolanaConnect("superseded by a wallet sign-in");
  CancelPendingAddSignIn("superseded by a wallet sign-in");
  uint64_t flow = 0;
  {
    std::scoped_lock lock(mutex_);
    flow = walletFlows_.Begin();
    pendingWalletAuth_.reset();
    walletAuthDone_ = std::move(done);
    bittensorWalletId_ = walletId;
  }
  // one hop: the wallet signs the challenge and the rest continues on the
  // urnetwork://bittensor-sign-message callback (Talisman) or the manual
  // sheet's Continue (TAO.com)
  StartBittensorSession(walletId, std::string(bittensor::kPurposeLogin), std::string(), flow,
                        nullptr);
}

void SdkHost::SetBittensorManualHandler(std::function<void(BittensorManualRequest)> handler) {
  std::scoped_lock lock(mutex_);
  onBittensorManual_ = std::move(handler);
}

urnet::BittensorWalletResult SdkHost::SubmitBittensorManual(const std::string& address,
                                                            const std::string& signature) {
  return wallet_.SubmitBittensorManual(address, signature);
}

void SdkHost::CancelBittensorManual(uint64_t flow) {
  {
    std::scoped_lock lock(mutex_);
    if (!walletFlows_.IsCurrent(flow)) return;
  }
  if (auto session = wallet_.bittensorSession()) {
    // a proof already accepted stays accepted; anything later is refused
    if (session->state() == "signed") return;
    session->cancel();
  }
  FailWalletOperation(std::string(bittensor::kCancelled));
}

void SdkHost::StartBittensorSession(const std::string& walletId, const std::string& purpose,
                                    const std::string& expectedAddress, uint64_t flow,
                                    std::function<bool(const std::string& message)> prepare,
                                    std::function<void(const std::string& error)> fail) {
  if (!fail) fail = [this](const std::string& error) { FailWalletOperation(error); };
  if (!api_) {
    fail("no api");
    return;
  }
  std::shared_ptr<urnet::BittensorWalletSession> session;
  std::optional<urnet::AuthWalletChallengeArgs> args;
  try {
    session = std::make_shared<urnet::BittensorWalletSession>(urnet::newBittensorWalletSession(
        walletId, std::string(bittensor::kPlatform), purpose,
        std::string(bittensor::kRedirectLink)));
    // the WalletConnect page pairs with this build's project id (empty: the
    // page's own); the other wallets' pages never see it
    if (bittensor::SendsWalletConnectProjectId(walletId)) {
      session->setWalletConnectProjectId(kWalletConnectProjectId);
    }
    // blockchain TAO, the purpose, and the typed address when there is one
    args = session->challengeArgs(expectedAddress);
  } catch (const std::exception& e) {
    fail(e.what());
    return;
  }
  if (!args) {
    fail("could not fetch wallet challenge");
    return;
  }
  api_->authWalletChallenge(*args, [this, flow, session, expectedAddress, prepare = std::move(prepare),
                                    fail](
                                       std::optional<urnet::AuthWalletChallengeResult> result,
                                       std::optional<std::string> err) mutable {
    std::string error;
    if (err) {
      error = *err;
    } else if (!result) {
      error = "wallet challenge returned no result";
    } else if (result->error && !result->error->message.empty()) {
      error = result->error->message;
    } else {
      try {
        // checks the message is a well formed challenge; starts its expiry
        session->setChallenge(result, g_get_real_time() / 1000);
      } catch (const std::exception& e) {
        error = e.what();
      }
    }
    if (!error.empty()) {
      // a newer wallet flow owns the slots now: it is not failed for this one
      // (a flow with its own `prepare` still owns its answer, though)
      if (!WalletFlowIsCurrent(flow)) {
        if (prepare) fail("superseded by another wallet flow");
        return;
      }
      fail(error);
      return;
    }
    PostToMain([this, flow, session, expectedAddress, prepare = std::move(prepare), fail] {
      // A Solana connect that started while this challenge was fetched owns the
      // bridge now: opening a Bittensor tab would also reset its keypair, so its
      // Phantom or Solflare return could no longer be read.
      if (!WalletFlowIsCurrent(flow)) {
        if (prepare) fail("superseded by another wallet flow");
        return;
      }
      const std::string message = session->message();
      if (prepare && !prepare(message)) return;
      wallet_.SignWithBittensor(session);
      if (session->transport() != bittensor::kTransportManual) return;
      std::function<void(BittensorManualRequest)> handler;
      {
        std::scoped_lock lock(mutex_);
        handler = onBittensorManual_;
      }
      if (!handler) {
        // the flow's slot is set by now (prepare): the shared path answers it
        FailWalletOperation("no manual wallet entry available");
        return;
      }
      BittensorManualRequest request;
      request.walletId = session->walletId();
      request.purpose = session->purpose();
      request.message = message;
      request.expectedAddress = expectedAddress;
      request.flow = flow;
      handler(std::move(request));
    });
  });
}

void SdkHost::SignInWithSso(const std::string& provider, std::function<void(AuthResult)> done) {
  CancelPendingSolanaConnect("superseded by a wallet sign-in");
  CancelPendingAddSignIn("superseded by a wallet sign-in");
  // a fresh state + nonce per attempt, never reused: the return is accepted
  // exactly once and only for this attempt
  std::string state;
  std::string nonce;
  if (char* s = g_uuid_string_random()) { state = s; g_free(s); }
  if (char* n = g_uuid_string_random()) { nonce = n; g_free(n); }
  // Google and Apple run their own web flow: the state carries the platform
  // claim the api's callback reads to redirect back to this app
  // (urnetwork://oauth/<provider>). No other provider signs in this way.
  const bool apple = provider == sso::kProviderApple;
  const bool google = provider == sso::kProviderGoogle;
  if (!apple && !google) {
    AuthResult r;
    r.error = "unknown sign-in provider";
    if (done) done(r);
    return;
  }
  state = sso::OAuthState(state);
  std::string apiUrl;
  {
    std::scoped_lock lock(mutex_);
    walletFlows_.Begin();
    pendingWalletAuth_.reset();
    pendingSsoAuth_ = false;
    pendingSsoType_.clear();
    pendingSsoJwt_.clear();
    ssoProvider_ = provider;
    ssoState_ = state;
    ssoNonce_ = nonce;
    ssoOwner_ = addsignin::Owner::SignIn;
    walletAuthDone_ = std::move(done);
    if (networkSpace_) apiUrl = networkSpace_->getApiUrl();
  }
  // opens the browser; the rest continues on the deep-link callback
  if (apple) {
    wallet_.SignInWithApple(apiUrl, state, nonce);
  } else if (google) {
    wallet_.SignInWithGoogle(apiUrl, state, nonce);
  }
}

// ---- add a sign-in method (AddSignInFlow.hpp) -------------------------------
// The login's own flows, ending in add-auth on this session instead of
// authLogin: the jwt is never replaced and the app never becomes the added
// identity.

void SdkHost::CancelPendingAddSignIn(const std::string& reason) {
  std::function<void(AddSignInResult)> walletDone;
  std::function<void(AddSignInResult)> ssoDone;
  {
    std::scoped_lock lock(mutex_);
    walletDone = std::move(walletAddDone_);
    walletAddDone_ = nullptr;
    ssoDone = std::move(ssoAddDone_);
    ssoAddDone_ = nullptr;
    if (ssoOwner_ == addsignin::Owner::AddSignIn) {
      // its return, if it still comes, matches no attempt and is dropped
      ssoOwner_ = addsignin::Owner::None;
      ssoProvider_.clear();
      ssoState_.clear();
      ssoNonce_.clear();
    }
  }
  if (walletDone) walletDone({false, reason});
  if (ssoDone) ssoDone({false, reason});
}

void SdkHost::CancelAddSignIn() {
  bool cancelSession = false;
  {
    std::scoped_lock lock(mutex_);
    cancelSession = static_cast<bool>(walletAddDone_);
    walletAddDone_ = nullptr;
    ssoAddDone_ = nullptr;
    if (ssoOwner_ == addsignin::Owner::AddSignIn) {
      ssoOwner_ = addsignin::Owner::None;
      ssoProvider_.clear();
      ssoState_.clear();
      ssoNonce_.clear();
    }
    // a challenge still being fetched for the add opens no tab
    if (cancelSession) walletFlows_.Begin();
  }
  if (!cancelSession) return;
  if (auto session = wallet_.bittensorSession()) {
    if (session->purpose() == bittensor::kPurposeAdd && session->state() != "signed") {
      session->cancel();
    }
  }
}

void SdkHost::AddSignInWithSso(const std::string& provider,
                               std::function<void(AddSignInResult)> done) {
  CancelPendingSolanaConnect("superseded by another sign-in method");
  CancelPendingAddSignIn("superseded by another sign-in method");
  const bool apple = provider == sso::kProviderApple;
  const bool google = provider == sso::kProviderGoogle;
  if (!apple && !google) {
    if (done) done({false, "unknown sign-in provider"});
    return;
  }
  // a fresh state + nonce per attempt, as for a sign-in
  std::string state;
  std::string nonce;
  if (char* s = g_uuid_string_random()) { state = s; g_free(s); }
  if (char* n = g_uuid_string_random()) { nonce = n; g_free(n); }
  state = sso::OAuthState(state);
  std::string apiUrl;
  {
    std::scoped_lock lock(mutex_);
    walletFlows_.Begin();
    ssoProvider_ = provider;
    ssoState_ = state;
    ssoNonce_ = nonce;
    // the verified return goes to add-auth and never signs in (on_sso)
    ssoOwner_ = addsignin::Owner::AddSignIn;
    ssoAddDone_ = std::move(done);
    if (networkSpace_) apiUrl = networkSpace_->getApiUrl();
  }
  if (apple) {
    wallet_.SignInWithApple(apiUrl, state, nonce);
  } else {
    wallet_.SignInWithGoogle(apiUrl, state, nonce);
  }
}

void SdkHost::AddSignInWithSolana(WalletConnect::Provider provider,
                                  std::function<void(AddSignInResult)> done) {
  if (provider == WalletConnect::Provider::Bittensor) {
    if (done) done({false, "not a solana wallet provider"});
    return;
  }
  CancelPendingSolanaConnect("superseded by another sign-in method");
  CancelPendingAddSignIn("superseded by another sign-in method");
  std::function<void(WalletSignature)> signDone;
  {
    std::scoped_lock lock(mutex_);
    walletFlows_.Begin();
    // a waiting signature request would take this wallet's signature first
    signDone = std::move(walletSignDone_);
    walletSignDone_ = nullptr;
    walletAddChain_ = addsignin::WalletChain::Solana;
    walletAddDone_ = std::move(done);
  }
  if (signDone) {
    WalletSignature out;
    out.error = "superseded by another sign-in method";
    signDone(std::move(out));
  }
  // connect, then the challenge is signed (on_public_key, AddSignIn) and the
  // signature goes to add-auth (on_signature, AnswerAdd)
  wallet_.Connect(provider);
}

void SdkHost::AddSignInWithBittensor(const std::string& walletId,
                                     std::function<void(AddSignInResult)> done) {
  CancelPendingSolanaConnect("superseded by another sign-in method");
  CancelPendingAddSignIn("superseded by another sign-in method");
  std::function<void(WalletSignature)> signDone;
  uint64_t flow = 0;
  {
    std::scoped_lock lock(mutex_);
    flow = walletFlows_.Begin();
    signDone = std::move(walletSignDone_);
    walletSignDone_ = nullptr;
    walletAddChain_ = addsignin::WalletChain::Bittensor;
    walletAddDone_ = std::move(done);
  }
  if (signDone) {
    WalletSignature out;
    out.error = "superseded by another sign-in method";
    signDone(std::move(out));
  }
  // purpose "add": a login or create session refuses this return, and the
  // proof (on_signature, AnswerAdd) goes to add-auth
  StartBittensorSession(walletId, std::string(bittensor::kPurposeAdd), std::string(), flow,
                        nullptr);
}

void SdkHost::AddAuthMethod(const addsignin::Args& in, std::function<void(AddSignInResult)> done) {
  if (!done) done = [](AddSignInResult) {};
  if (!addsignin::SuppliesMethod(in)) {
    done({false, "no auth method supplied"});
    return;
  }
  if (!api_) {
    done({false, "no api"});
    return;
  }
  urnet::AddAuthArgs args{};
  if (!in.userAuth.empty()) args.user_auth = in.userAuth;
  if (!in.password.empty()) args.password = in.password;
  if (!in.authJwt.empty()) args.auth_jwt = in.authJwt;
  if (!in.authJwtType.empty()) args.auth_jwt_type = in.authJwtType;
  if (!in.walletAddress.empty()) {
    urnet::WalletAuthArgs wallet{};
    wallet.wallet_address = in.walletAddress;  // base58 (solana) | ss58 (TAO)
    wallet.wallet_signature = in.walletSignature;
    wallet.wallet_message = in.walletMessage;
    wallet.blockchain = in.blockchain;
    args.wallet_auth = wallet;
  }
  // add-auth answers no jwt: the session's own stays installed
  api_->addAuth(std::optional<urnet::AddAuthArgs>(args),
                [done](std::optional<urnet::AddAuthResult> result,
                       std::optional<std::string> err) {
                  if (err) {
                    done({false, *err});
                    return;
                  }
                  if (!result) {
                    done({false, std::string()});
                    return;
                  }
                  if (result->error) {
                    done({false, result->error->message,
                          result->error->code.value_or(std::string())});
                    return;
                  }
                  done({true, std::string()});
                });
}

void SdkHost::AuthLoginWithSso(const std::string& provider, const std::string& jwt) {
  urnet::AuthLoginArgs args;
  args.auth_jwt_type = provider;
  args.auth_jwt = jwt;
  api_->authLogin(args, [this, provider, jwt](std::optional<urnet::AuthLoginResult> result,
                                              std::optional<std::string> err) {
    std::function<void(AuthResult)> done;
    {
      std::scoped_lock lock(mutex_);
      done = std::move(walletAuthDone_);
      walletAuthDone_ = nullptr;
    }
    auto fail = [&done](const std::string& message) {
      AuthResult r;
      r.error = message;
      r.sso = true;
      if (done) done(r);
    };
    if (err) { fail(*err); return; }
    if (!result) { fail("no result"); return; }
    if (result->error && !result->error->message.empty()) { fail(result->error->message); return; }
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, done ? done : [](AuthResult) {});
      return;
    }
    if (result->auth_allowed && !result->auth_allowed->empty()) {
      // the account exists under other sign-in methods
      AuthResult r;
      r.sso = true;
      for (const auto& method : *result->auth_allowed) {
        if (!r.authAllowed.empty()) r.authAllowed += ", ";
        r.authAllowed += method;
      }
      if (done) done(r);
      return;
    }
    // Authenticated identity with no network yet: keep the token and route
    // into the create-network page (the web's ssoCreateNetworkView).
    {
      std::scoped_lock lock(mutex_);
      pendingSsoAuth_ = true;
      pendingSsoType_ = provider;
      pendingSsoJwt_ = jwt;
    }
    if (done) {
      AuthResult r;
      r.sso = true;
      r.sso_needs_network = true;
      done(r);
    }
  });
}

void SdkHost::CreateNetworkWithPendingSso(const std::string& networkName,
                                          const std::string& referralCode,
                                          std::function<void(AuthResult)> done) {
  std::string type;
  std::string jwt;
  {
    std::scoped_lock lock(mutex_);
    if (!pendingSsoAuth_) {
      done({false, false, "no sign-in pending"});
      return;
    }
    type = pendingSsoType_;
    jwt = pendingSsoJwt_;
  }
  urnet::NetworkCreateArgs args;
  ApplySignupPreferences(args);
  args.user_name = std::string();  // mac parity: always empty
  args.auth_jwt_type = type;
  args.auth_jwt = jwt;
  args.network_name = networkName;
  args.terms = true;  // the page's continue button is gated on the terms switch
  if (!referralCode.empty()) args.referral_code = referralCode;
  api_->networkCreate(args, [this, done](std::optional<urnet::NetworkCreateResult> result,
                                         std::optional<std::string> err) {
    HandleNetworkCreateResult(std::move(result), std::move(err), [this, done](AuthResult r) {
      if (r.ok) {
        std::scoped_lock lock(mutex_);
        pendingSsoAuth_ = false;
        pendingSsoType_.clear();
        pendingSsoJwt_.clear();
      }
      done(r);
    });
  });
}

bool SdkHost::HasPendingSsoAuth() {
  std::scoped_lock lock(mutex_);
  return pendingSsoAuth_;
}

void SdkHost::RequestWalletChallenge(
    const std::string& blockchain, const std::string& walletAddress,
    std::function<void(std::optional<std::string> message, std::string error)> done) {
  urnet::AuthWalletChallengeArgs args;
  args.blockchain = blockchain;
  if (!walletAddress.empty()) args.wallet_address = walletAddress;
  api_->authWalletChallenge(args, [done = std::move(done)](
                                      std::optional<urnet::AuthWalletChallengeResult> result,
                                      std::optional<std::string> err) mutable {
    if (err) {
      done(std::nullopt, *err);
      return;
    }
    if (!result) {
      done(std::nullopt, "wallet challenge returned no result");
      return;
    }
    if (result->error && !result->error->message.empty()) {
      done(std::nullopt, result->error->message);
      return;
    }
    if (!result->message_template || result->message_template->empty()) {
      done(std::nullopt, "wallet challenge returned no message");
      return;
    }
    done(*result->message_template, std::string());
  });
}

void SdkHost::FailWalletOperation(const std::string& error) {
  std::function<void(WalletSignature)> signDone;
  std::function<void(AddSignInResult)> addDone;
  std::function<void(AuthResult)> done;
  {
    std::scoped_lock lock(mutex_);
    signDone = std::move(walletSignDone_);
    walletSignDone_ = nullptr;
    if (!signDone) {
      addDone = std::move(walletAddDone_);
      walletAddDone_ = nullptr;
    }
    if (walletCreateDone_) {
      done = std::move(walletCreateDone_);
      pendingWalletNetworkName_.clear();
      pendingWalletReferralCode_.clear();
    } else {
      done = std::move(walletAuthDone_);
    }
    walletAuthDone_ = nullptr;
    walletCreateDone_ = nullptr;
  }
  if (signDone) {
    WalletSignature out;
    out.error = error;
    signDone(std::move(out));
    return;
  }
  if (addDone) {
    addDone({false, error});
    return;
  }
  if (done) done({false, false, error});
}

void SdkHost::SignBittensorConnect(const std::string& walletId,
                                   const std::string& walletAddress,
                                   std::function<void(WalletSignature)> done) {
  CancelPendingSolanaConnect("superseded by a wallet signature request");
  CancelPendingAddSignIn("superseded by a wallet signature request");
  uint64_t flow = 0;
  {
    std::scoped_lock lock(mutex_);
    flow = walletFlows_.Begin();
    walletSignDone_ = std::move(done);
  }
  // the challenge is bound to the typed address when there is one, so a wallet
  // that signs for a different account is refused by the session (address
  // mismatch) before anything is sent; purpose "connect"
  StartBittensorSession(walletId, std::string(bittensor::kPurposeConnect), walletAddress, flow,
                        nullptr);
}

void SdkHost::FinishCreateNetworkWithWallet(const std::string& signature) {
  std::function<void(AuthResult)> done;
  std::optional<urnet::WalletAuthArgs> walletAuth;
  std::string networkName;
  std::string referralCode;
  {
    std::scoped_lock lock(mutex_);
    done = std::move(walletCreateDone_);
    walletCreateDone_ = nullptr;
    if (!done || !pendingWalletAuth_) return;
    if (wallet_.publicKey() != pendingWalletAuth_->wallet_address.value_or(std::string())) {
      pendingWalletNetworkName_.clear();
      pendingWalletReferralCode_.clear();
      walletAuth.reset();
    } else {
      pendingWalletAuth_->wallet_signature = signature;
      walletAuth = pendingWalletAuth_;
      networkName = std::move(pendingWalletNetworkName_);
      referralCode = std::move(pendingWalletReferralCode_);
      pendingWalletNetworkName_.clear();
      pendingWalletReferralCode_.clear();
    }
  }
  if (!walletAuth) {
    done({false, false, "wallet account changed; use the same account to create the network"});
    return;
  }

  urnet::NetworkCreateArgs args;
  ApplySignupPreferences(args);
  args.user_name = std::string();
  args.network_name = networkName;
  args.terms = true;
  args.verify_use_numeric = true;
  if (!referralCode.empty()) args.referral_code = referralCode;
  // a signature from another account than the address comes back as
  // result.error.code (a 401 error otherwise)
  args.result_errors = true;
  std::string bittensorWalletId;
  if (walletAuth->blockchain.value_or(std::string()) == urnet::TAO) {
    std::scoped_lock lock(mutex_);
    bittensorWalletId = bittensorWalletId_;
  }
  args.wallet_auth = walletAuth;
  api_->networkCreate(args, [this, bittensorWalletId, done = std::move(done)](
                                std::optional<urnet::NetworkCreateResult> result,
                                std::optional<std::string> err) mutable {
    HandleNetworkCreateResult(std::move(result), std::move(err), std::move(done),
                              bittensorWalletId);
  });
}

void SdkHost::SetProductUpdatesOptOut(bool optOut) {
  productUpdatesOptOut_ = optOut;
  if (optOut && events_) events_->SignupOptoutChanged(false);
}

void SdkHost::ApplySignupPreferences(urnet::NetworkCreateArgs& args) const {
  if (productUpdatesOptOut_) args.product_updates = false;
}

void SdkHost::AuthNetworkClientWithLocale(const urnet::AuthNetworkClientArgs& args,
                                          urnet::AuthNetworkClientCallback callback) {
  nlohmann::json json = args;
  json["time_zone"] = LocalTimeZoneId();
  json["locale"] = ClientEventLocale();
  const std::string body = json.dump();
  auto* fn = new urnet::AuthNetworkClientCallback(std::move(callback));
  urnet_api_auth_network_client(api_->handle(), body.c_str(),
                                &urnet::detail::oneshot_auth_network_client, fn);
}

void SdkHost::HandleDeepLink(const std::string& url) {
  if (url.rfind("urnetwork://onboarding/", 0) == 0) {
    if (onOnboardingLink_) onOnboardingLink_(url);
    return;
  }
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
  // a signature from another account than the address comes back as
  // result.error.code (a 401 error otherwise)
  args.result_errors = true;
  std::string bittensorWalletId;
  if (blockchain == urnet::TAO) {
    std::scoped_lock lock(mutex_);
    bittensorWalletId = bittensorWalletId_;
  }
  api_->authLogin(args, [this, w, bittensorWalletId](std::optional<urnet::AuthLoginResult> result,
                                                     std::optional<std::string> err) {
    // SDK callback thread: consume walletAuthDone_ under the lock (it is set
    // on the UI thread; the wallet on_error path races this same slot)
    std::function<void(AuthResult)> done;
    {
      std::scoped_lock lock(mutex_);
      done = std::move(walletAuthDone_);
      walletAuthDone_ = nullptr;
    }
    if (err) { if (done) done({false, false, *err}); return; }
    if (!result) { if (done) done({false, false, "no result"}); return; }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      r.errorCode = result->error->code.value_or(std::string());
      r.bittensorWalletId = bittensorWalletId;
      if (done) done(r);
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
  // The jwt persists asynchronously — and a failed persist means a signed-out
  // NEXT LAUNCH even though this session would appear to work, so a failure
  // is surfaced as an auth error instead of being ignored: the user retries
  // the sign-in rather than silently losing the session (for a guest network
  // the jwt is the only credential there is).
  asyncLocalState_->setByJwt(byJwt, [this, done](bool ok) {
    if (!ok) {
      std::fprintf(stderr, "[sdk] persist by_jwt failed (localState commit)\n");
      done({false, false, "could not save session"});
      return;
    }
    urnet::AuthNetworkClientArgs args;
    args.description = UrDeviceDescription();
    args.device_spec = UrDeviceSpec();
    AuthNetworkClientWithLocale(args, [this, done](std::optional<urnet::AuthNetworkClientResult> result,
                                                   std::optional<std::string> err) {
      if (err) { done({false, false, *err}); return; }
      if (!result) { done({false, false, "no result"}); return; }
      if (result->error && !result->error->message.empty()) { done({false, false, result->error->message}); return; }
      if (!result->by_client_jwt) { done({false, false, "no client jwt"}); return; }
      // same contract as the by_jwt persist above: an unsaved client jwt is a
      // broken next launch (no tunnel credential), never a silent success
      asyncLocalState_->setByClientJwt(*result->by_client_jwt, [this, done](bool ok) {
        if (!ok) {
          std::fprintf(stderr, "[sdk] persist by_client_jwt failed (localState commit)\n");
          done({false, false, "could not save session"});
          return;
        }
        // the stored credential is this sign-in's from here
        signedOut_.store(false);
        authLogouts_.SignedIn();
        if (onAuth_) onAuth_(true);
        done({true, false, ""});
      });
    });
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

TunnelStartResult SdkHost::StartTunnel(const char* reason) {
  std::scoped_lock lock(mutex_);
  return StartTunnelLocked(reason);
}

TunnelStartResult SdkHost::StartTunnelLocked(const char* reason) {
  lastTunnelError_.clear();
  // A new start makes any previous "the daemon stopped it" verdict obsolete.
  // This is the ONLY thing that clears the latch.
  daemonTunnelGone_.store(false);
  // EVERY outcome of this function is logged. It used to be silent on all of
  // them, so a Connect that failed here left NOTHING to read: not in the app,
  // not in the journal, not in the daemon (which is never reached on most of
  // these paths). "Pressing Connect does nothing" was unanswerable as a result.
  g_message("connect: start_tunnel requested (%s)", reason);
  // An owed sign-out first, at once: a Connect is a person asking (SignOut.hpp).
  SettleSignOutLocked("connect", /*userInitiated=*/true);
  // Signed out reads as no jwt: the stored one outlives a sign-out until its
  // asynchronous local logout lands.
  const std::string clientJwt = signedOut_.load() ? std::string() : localState_->getByClientJwt();
  if (clientJwt.empty()) {
    lastTunnelError_ = "not signed in";
    g_warning("connect: refused — no client jwt (not signed in)");
    return TunnelStartResult::Failed;
  }
  const std::string instanceId = localState_->getInstanceId();
  // FROM UPSTREAM (556dca7). The daemon's ValidateStartTunnelRequest already
  // refuses an empty instance_id, so this is not the enforcing check — it is
  // the one that fails FAST and legibly. Without it an empty id costs a daemon
  // connect, a hello and a version negotiation before coming back as a generic
  // "instance_id is required" attributed to the daemon, which reads like a
  // protocol fault rather than what it is: this process has no local device
  // identity yet. instance_id is the DEVICE PAIRING KEY, so an empty one can
  // never succeed and there is nothing to gain by putting it on the wire.
  if (instanceId.empty()) {
    lastTunnelError_ = "local device instance is missing";
    g_warning("connect: refused — the local state has no instance id");
    return TunnelStartResult::Failed;
  }

  // 1) daemon session: connect + hello. The protocol version is enforced in
  //    BOTH directions here (APPIMAGE.md §11b) — each failure mode is a
  //    distinct, renderable state, never a silent false.
  std::string error;
  switch (control_.EnsureSession(&error)) {
    case DaemonSessionState::Ok:
      break;
    case DaemonSessionState::Unreachable:
      lastTunnelError_ = error;
      g_warning("connect: daemon unreachable: %s", error.c_str());
      return TunnelStartResult::DaemonUnreachable;
    case DaemonSessionState::DaemonTooOld:
      lastTunnelError_ = error;
      g_warning("connect: daemon too old: %s", error.c_str());
      return TunnelStartResult::DaemonTooOld;
    case DaemonSessionState::ClientTooOld:
      lastTunnelError_ = error;
      g_warning("connect: app too old for this daemon: %s", error.c_str());
      return TunnelStartResult::AppTooOld;
    case DaemonSessionState::SdkMismatch:
      lastTunnelError_ = error;
      g_warning("connect: app/daemon SDK build mismatch: %s", error.c_str());
      return TunnelStartResult::SdkMismatch;
    case DaemonSessionState::Error:
      lastTunnelError_ = error;
      g_warning("connect: control session error: %s", error.c_str());
      return TunnelStartResult::Failed;
  }
  // A sign-out the daemon has not done yet: what it runs may still be the
  // account's that left. Nothing is attached or started until it has been;
  // the delivery above tried, and the health poll keeps trying.
  if (signOut_.Owed()) {
    lastTunnelError_ =
        "the URnetwork system service has not yet stopped what the previous sign-in ran";
    g_warning("connect: refused — a sign-out is still owed to the daemon");
    return TunnelStartResult::Failed;
  }

  // 2) the device-RPC mTLS material.
  //
  //    REATTACH when the daemon still has a tunnel up AND we remember the
  //    exact triple it was started with: TunnelHost::CanAdopt compares the
  //    three rpc fields byte for byte, so generating fresh material on every
  //    launch would tear down a working tunnel and rebuild it every single
  //    time the app starts — the precise regression CanAdopt exists to
  //    prevent. Otherwise generate.
  //
  //    The GUI is the generator (not the daemon) because the VERIFIER must
  //    choose its own pin: if the daemon generated, this side would pin
  //    whatever value arrived on the reply, and pinning to a value the peer
  //    chose is not pinning. The private half that crosses the socket is
  //    server_pem, travelling UP to a peer that has already authenticated us
  //    via SO_PEERCRED — and the same request already carries by_jwt, which is
  //    strictly more valuable than a per-session loopback server key.
  //    ONE status read serves both decisions below (is our bound device still
  //    real, and can this start be adopted instead of rebuilt) — it used to be
  //    two round trips on the same lock.
  std::string statusError;
  const std::optional<ctl::StatusReply> status = control_.Status(&statusError);
  if (!status) {
    // Not fatal here: the start below reports the transport failure with a
    // mapped, renderable state. But it must not pass unremarked, because every
    // decision that follows is now being made on no information.
    g_warning("connect: the daemon did not answer `status` (%s); continuing with a fresh start",
              statusError.empty() ? "no detail" : statusError.c_str());
  }
  const bool daemonTunnelUp = status && status->tunnel_state == ctl::TunnelState::Up;

  // 2a) IS THE DEVICE WE ALREADY HOLD STILL REAL?
  //
  //     A urnet::DeviceRemote handle is NOT proof of a live tunnel. The handle
  //     belongs to this process and NOTHING invalidates it when the
  //     daemon-side half disappears — a service restart or reinstall, another
  //     client's stop_tunnel, the IoLoop ending. This function used to
  //     `return Started` on `device_` alone, without one byte of daemon
  //     traffic, which stranded the GUI permanently: the caller then drove
  //     ConnectBestAvailable into a dead rpc, no start_tunnel was ever sent,
  //     and no error was ever produced. "Press Connect, nothing happens", with
  //     an empty daemon journal to match.
  if (device_) {
    const bool sameSession = deviceControlGeneration_ == control_.SessionGeneration();
    if (daemonTunnelUp && status->rpc_pinned && sameSession) {
      g_message("connect: the daemon still holds our tunnel (rpc pinned, same control "
                "session); reusing the bound device");
      return TunnelStartResult::Started;
    }
    g_warning("connect: the bound device is STALE (daemon tunnel_state=%s, rpc_pinned=%s, "
              "same control session=%s); dropping it and starting a new session",
              status ? ctl::ToString(status->tunnel_state) : "unknown",
              status && status->rpc_pinned ? "yes" : "no", sameSession ? "yes" : "no");
    // Drop it before anything else can use it, and tell the UI, so the pages
    // that fold on hasDevice() stop rendering a session that does not exist.
    TeardownDeviceLocked();
    EmitDrawerEvent(DrawerEvent::DeviceLifecycle);
    PublishConnectReading();
  }

  // ---- 2b) THE TWO DOORS ----------------------------------------------------
  // A relaunch that finds a tunnel already up can either NAME the running
  // session or DESCRIBE a new one, and the division between them is structural
  // rather than a matter of which is tried first (RpcSession.hpp spells the
  // whole argument out):
  //
  //   attach_tunnel — THE EXPLICIT DOOR, below. The stored record names the
  //     live session by (instance_id, rpc_session_id); the client key and the
  //     pinned cert come back out of the Secret Service; nothing crosses the
  //     socket but the two identifiers.
  //
  //   start_tunnel  — THE FALLBACK, and the answer to EVERY failure of the
  //     first: no record, a locked keyring, a stale entry, a session that has
  //     since stopped, a tunnel belonging to another uid, a daemon that does
  //     not persist sessions at all. It always exists, so no failure above can
  //     leave the user unable to connect. The daemon may still absorb it
  //     through TunnelHost::CanAdopt — that guard is the daemon's own
  //     idempotency contract for any client re-sending an identical request,
  //     and it is no longer something THIS side aims at: two thirds of the
  //     triple CanAdopt compares are the daemon's half of the material, which
  //     upstream's record deliberately does not keep.
  if (daemonTunnelUp) {
    if (auto attached = TryAttachRememberedSessionLocked(clientJwt, *status)) return *attached;
  }

  // ---- 2c) a FRESH session --------------------------------------------------
  // One act mints all of it: the mTLS material, the loopback port, and the
  // rpc_session_id that NAMES the three together. They are stored together too,
  // or not at all.
  RpcSessionRecord session;
  // "confirmed" because nothing is written until the pairing has demonstrably
  // worked — see RememberSyncedSessionLocked. ("pending" survives only for
  // records migrated out of the pre-Secret-Service format, whose state this
  // process never chose.)
  session.state = "confirmed";
  session.instance_id = instanceId;
  session.rpc_session_id = MintRpcSessionId();
  session.host_port = RandomLoopbackRpcHostPort();
  std::string rpcServerPem;      // the daemon's half: sent, never stored
  std::string rpcClientCertPem;  // the daemon's half: sent, never stored
  if (!rpcsession::IsPairableId(session.rpc_session_id)) {
    // Cannot happen (g_uuid_string_random is infallible), and is refused here
    // anyway: the daemon's ValidateStartTunnelRequest now REQUIRES a session id
    // alongside the pinning triple, so a blank one would come back as a generic
    // rpc_pin_required and read like a key-material fault instead of what it is.
    lastTunnelError_ = "a device rpc session name could not be generated";
    g_warning("connect: refused — %s", lastTunnelError_.c_str());
    return TunnelStartResult::Failed;
  }
  try {
    urnet::DeviceRpcKeyMaterial km = urnet::generateDeviceRpcKeyMaterial();
    // THE HANDLE-0 TRAP: urnet_generate_device_rpc_key_material can return
    // handle 0 with NO error, and the binding maps a NULL char* to an empty
    // string rather than throwing — so the four getters would hand back four
    // empty PEMs silently and the session would end up unpinned. The
    // explicit handle check plus the per-string shape gate below is the only
    // thing standing between that and a root rpc listener anyone can drive.
    if (!km) {
      lastTunnelError_ = "the device rpc key material could not be generated";
      g_warning("connect: refused — %s (the SDK returned a null key-material handle "
                "with no error)",
                lastTunnelError_.c_str());
      return TunnelStartResult::Failed;
    }
    rpcServerPem = km.getServerPem();
    rpcClientCertPem = km.getClientCertPem();
    session.client_pem = km.getClientPem();
    session.server_cert_pem = km.getServerCertPem();
  } catch (const std::exception& e) {
    lastTunnelError_ = std::string("the device rpc key material could not be generated: ") +
                       e.what();
    g_warning("connect: refused — %s", lastTunnelError_.c_str());
    return TunnelStartResult::Failed;
  }
  if (!ctl::LooksLikePem(rpcServerPem) || !ctl::LooksLikePem(rpcClientCertPem) ||
      !ctl::LooksLikePem(session.client_pem) || !ctl::LooksLikePem(session.server_cert_pem)) {
    lastTunnelError_ = "the device rpc key material is not usable";
    // The four lengths are the whole diagnosis (a handle-0 generate yields
    // four zeroes; a truncated one yields a short odd man out) and they leak
    // nothing — never log the PEMs themselves, two of them are private keys.
    g_warning("connect: refused — %s (server=%zu client_cert=%zu client=%zu "
              "server_cert=%zu bytes)",
              lastTunnelError_.c_str(), rpcServerPem.size(), rpcClientCertPem.size(),
              session.client_pem.size(), session.server_cert_pem.size());
    return TunnelStartResult::Failed;
  }

  // 3) start_tunnel: the daemon builds the DeviceLocal (rpc enabled, pinned to
  //    the material above), opens the tun and wires the IoLoop. First
  //    authenticated client wins; a tunnel owned by another live client comes
  //    back as a plain error. The active network space rides along (windows
  //    StartTunnel's network_space_json): the daemon must build its DeviceLocal
  //    in the SAME space, or a custom-server session would sync against a
  //    device registered on production. (mutex_ is held: read the space
  //    directly.)
  std::string spaceJson;
  try {
    if (networkSpace_) spaceJson = networkSpace_->toJson();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] network space toJson failed: %s\n", e.what());
  }
  ControlClient::StartTunnelOptions options;
  options.by_jwt = clientJwt;
  // Local state's CURRENT id. The reattach path no longer comes through here —
  // it goes through attach_tunnel above, which names the id the session was
  // STARTED with — so there is nothing left to prefer over this one. What the
  // daemon is ACTUALLY paired with still comes back on the reply and overrides
  // it below, because the daemon may adopt a session started under an earlier
  // id (RpcSession.hpp's instance-id trap).
  options.instance_id = instanceId;
  options.app_version = kAppVersion;
  options.network_space_json = spaceJson;
  // The kill switch the user has standing. The daemon reports back what it
  // ACTUALLY installed; nothing here assumes the request took.
  options.kill_switch = KillSwitchRequestedLocked();
  options.rpc_server_pem = rpcServerPem;
  options.rpc_client_cert_pem = rpcClientCertPem;
  options.rpc_listen_hostport = session.host_port;
  options.rpc_session_id = session.rpc_session_id;

  // StartTunnelEx is fail-closed by construction: it validates the triple
  // before a frame is sent, and on a synchronous start that comes back Up it
  // requires rpc_pinned AND an echoed port equal to the one we chose —
  // otherwise it fails the outcome and stops the daemon-side tunnel, because a
  // running tunnel whose ROOT rpc listener is unauthenticated is worse than no
  // tunnel. So there is no plaintext fallback to write here.
  g_message("connect: sending start_tunnel (fresh session, rpc %s, kill switch %s)",
            session.host_port.c_str(), options.kill_switch ? "on" : "off");
  const ControlClient::StartTunnelOutcome outcome = control_.StartTunnelEx(options);
  if (!outcome.ok) {
    lastTunnelError_ = outcome.error;
    // Whether the refusal was local (the fail-closed validator, before a byte
    // was sent) or the daemon's own answer, it is named here — this return
    // used to carry the reason no further than a notice the user may not have
    // been looking at.
    g_warning("connect: start_tunnel failed (code=%s): %s",
              outcome.code.empty() ? "none" : outcome.code.c_str(),
              outcome.error.empty() ? "no detail" : outcome.error.c_str());
    // The material never bound to anything, and any PREVIOUSLY remembered
    // session is equally not what is running now.
    ForgetRpcSession();
    switch (outcome.session) {
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

  // WHAT THE DAEMON IS ACTUALLY PAIRED WITH, which is not always what we asked
  // for: on its adoption path the live identity is the FIRST start's. Both the
  // DeviceRemote we build and the record we remember must use the daemon's
  // answer, or the rpc sync pairs against an id the DeviceLocal never had and
  // the remote connects and never populates. Empty means a daemon predating the
  // echo; fall back to what we sent, which then simply cannot be reattached to.
  if (!outcome.instance_id.empty() && outcome.instance_id != session.instance_id) {
    g_message("connect: the service adopted a session started under a different instance id; "
              "pairing with the service's");
    session.instance_id = outcome.instance_id;
  }
  if (!outcome.rpc_session_id.empty()) session.rpc_session_id = outcome.rpc_session_id;

  // Remember it only once it SYNCS (RememberSyncedSessionLocked). A daemon that
  // does not echo an rpc_session_id has no session to name, so there is nothing
  // that could be attached to later and nothing worth writing to the keyring.
  const bool attachableLater = !outcome.rpc_session_id.empty();
  if (!attachableLater) {
    g_message("connect: the service did not name this rpc session; it will be rebuilt rather "
              "than reattached on the next launch");
    // A previously remembered session is not this one. Drop it rather than
    // leave a record that can only fail to match.
    ForgetRpcSession();
  }
  return BindRemoteDeviceLocked(clientJwt, session, attachableLater);
}

// ---- door 1: attach_tunnel -------------------------------------------------
// nullopt means THE DOOR DID NOT OPEN and the caller must fall back to a fresh
// start_tunnel — which is the answer to every failure here, so none of them can
// leave the user unable to connect. A value means the door was taken and this
// is the whole result of StartTunnel.
//
// The failure inventory this is built around, each landing on a fallback:
//   * no record at all, or one this build cannot read      -> LoadRpcSession
//   * a locked or absent keyring                           -> LoadRpcSession
//   * a stale entry, or one for a session that has stopped -> rpcsession::CanAttach
//   * an entry for a tunnel owned by ANOTHER uid           -> the daemon, which
//     charges kActionTakeOverTunnel for it and refuses with auth_not_tunnel_owner
//     when that is not granted
//   * a daemon that does not persist rpc sessions          -> the daemon, with
//     kCodeRpcSessionNotPersisted
std::optional<TunnelStartResult> SdkHost::TryAttachRememberedSessionLocked(
    const std::string& clientJwt, const ctl::StatusReply& status) {
  auto remembered = LoadRpcSession();
  if (!remembered) return std::nullopt;  // LoadRpcSession has already said why

  // IS IT THE TUNNEL THAT IS RUNNING? Asked HERE, before a frame is sent, and
  // asked again by the daemon. A record that names a session other than the
  // live one is exactly the stale/foreign case: it must fall back to a fresh
  // start, never attach to whatever happens to be up.
  if (!rpcsession::CanAttach(*remembered, status)) {
    g_message("connect: the remembered rpc session is not the one the service is running "
              "(remembered port %s, live rpc port %d); starting a fresh session",
              remembered->host_port.c_str(), status.rpc_port);
    // NOT forgotten. The record may still be perfectly good and simply describe
    // a session that has ended; the fresh start below overwrites it, and if
    // that start fails the user keeps whatever they had.
    return std::nullopt;
  }

  g_message("connect: a tunnel is already up and this app remembers its rpc session; "
            "attaching to it instead of rebuilding it");
  const ControlClient::StartTunnelOutcome outcome = control_.AttachTunnel(
      remembered->instance_id, remembered->rpc_session_id,
      ctl::RpcPortFromHostPort(remembered->host_port));
  if (!outcome.ok) {
    g_warning("connect: attach_tunnel was refused (code=%s): %s; starting a fresh session",
              outcome.code.empty() ? "none" : outcome.code.c_str(),
              outcome.error.empty() ? "no detail" : outcome.error.c_str());
    // WHICH REFUSALS KILL THE RECORD. A mismatch means the daemon does not have
    // the session this record names, so it can never match again and keeping it
    // only costs a failed attach every launch. Everything else — a daemon that
    // does not persist sessions, a take-over that was not authorized, a
    // transport failure, a polkit prompt the user dismissed — says nothing
    // about whether the credential is good, and discarding it there would
    // destroy a working credential over a temporary answer.
    if (outcome.code == ctl::kCodeRpcSessionMismatch) ForgetRpcSession();
    // Deliberately NOT surfaced as lastTunnelError_ and NOT returned: this is
    // not a failure the user has to see or act on. The fresh start below is the
    // answer, and it reports its own outcome.
    return std::nullopt;
  }

  // Attached. From here the failure mode changes: we now OWN the daemon's
  // tunnel and a local bind failure means nobody is driving it, so
  // BindRemoteDeviceLocked's teardown (which stops the daemon-side tunnel) is
  // the right ending — and this returns that result rather than falling back,
  // because a bind failure is LOCAL (the 12025 reservation, setRpcServer) and a
  // fresh start would meet it again.
  //
  // rememberOnSync is false: this record is already in the Secret Service, and
  // re-writing it would cost a keyring round trip to store what is already
  // there.
  return BindRemoteDeviceLocked(clientJwt, *remembered, /*rememberOnSync=*/false);
}

// ---- the DeviceRemote half, shared by BOTH doors ---------------------------
// Everything from here on is identical whether the session was just created or
// just attached to, which is the point of factoring it: one pinned
// construction, one set of listeners, one watchdog, one teardown-and-stop
// failure path. Requires mutex_.
TunnelStartResult SdkHost::BindRemoteDeviceLocked(const std::string& clientJwt,
                                                  const RpcSessionRecord& session,
                                                  bool rememberOnSync) {
  try {
    // 4) the remote face of the daemon's device, PINNED to the other half of
    //    the material the daemon is listening with. Same instanceId on both
    //    sides: the rpc sync pairs on it and the daemon side rejects a
    //    mismatch.
    //
    //    FIRST, THE WINDOW THIS SIDE CANNOT CLOSE. newDeviceRemoteWithDefaults
    //    is the only DeviceRemote constructor the binding has, and it dials
    //    127.0.0.1:12025 in PLAIN ws — no TLS, no pin — from a goroutine it
    //    starts before it returns; setRpcServer below cannot run any earlier
    //    and is itself blocked behind the constructor's 1 s initial lock. An
    //    occupant of that address is handed this device's rpc sync and then
    //    proxies the Api's authenticated HTTP, i.e. the account bearer token.
    //    The binding has no already-pinned path (see the declaration of
    //    HoldDeviceRpcDefaultPortLocked for the symbol evidence), so the next
    //    best guarantee is that the address is DEAD while we use it: we hold
    //    it ourselves, or we do not build the DeviceRemote at all.
    //
    //    The throw is the point — it lands in the catch below, which is the
    //    one path that already tears down every partial resource AND stops the
    //    daemon-side tunnel we just started.
    if (std::string holdError; !HoldDeviceRpcDefaultPortLocked(&holdError)) {
      // Deliberately does not name a cause it did not measure: EADDRINUSE
      // (something is squatting the address) and any other errno are the same
      // decision here, because both leave the unpinned first dial able to
      // reach a peer we have not authenticated. holdError carries the errno.
      throw std::runtime_error(
          std::string("the device rpc cannot be started safely: this app could not "
                      "reserve 127.0.0.1:") +
          std::to_string(ctl::kDeviceRpcPort) +
          ", the address its SDK dials unencrypted and unauthenticated before it can "
          "present its certificate (" +
          holdError + ")");
    }
    device_ = urnet::newDeviceRemoteWithDefaults(*networkSpace_, clientJwt,
                                                 session.instance_id);
    // setRpcServer BEFORE any listener registration or getter (windows
    // SdkHost.cpp:2027-2029), and exactly once per DeviceRemote instance — the
    // binding gives no re-entrancy contract for a second call and no way to
    // clear a listener short of destroying the object. A throw here lands in
    // the catch below, which tears down and stops the daemon-side tunnel.
    const uint64_t rpcGeneration = ++rpcSessionGeneration_;
    device_->setRpcServer(session.client_pem, session.server_cert_pem, session.host_port);
    rpcHostPort_ = session.host_port;
    // WHICH daemon connection this device belongs to. The daemon's DeviceLocal
    // — the only thing on the other end of the rpc we just pinned — dies with
    // the daemon process, so a control session that has been rebuilt since
    // this moment means the handle below is a handle to nothing. Recorded
    // here, checked by hasDevice() and by the revalidation at the top of this
    // function, so nobody has to take a device handle on faith.
    deviceControlGeneration_ = control_.SessionGeneration();

    // The watchdog's cancel edge. Preferred over polling: the first
    // remote_connected=true is proof the pinned pair agreed, and it usually
    // lands well inside the deadline. Marshalled rather than taken inline —
    // this callback can fire on an SDK thread while StartTunnel still holds
    // mutex_, and re-entering a non-recursive lock is a deadlock.
    //
    // IT IS ALSO WHERE THE SESSION IS REMEMBERED. remote_connected turning true
    // is the ONLY proof the pinned pair actually agreed, so nothing is written
    // to disk or to the keyring before it: a record on disk therefore always
    // describes a pairing that really worked, and a session that never synced
    // is never offered to a later launch as something to attach to. The
    // `rpcBindWatchId_ == 0` guard below makes this the FIRST connected edge
    // only, so a session is stored exactly once however often the remote
    // reconnects.
    subs_.push_back(device_->addRemoteChangeListener([this, rpcGeneration](bool connected) {
      if (!connected) return;
      PostToMain([this, rpcGeneration] {
        std::scoped_lock lock(mutex_);
        if (rpcGeneration != rpcSessionGeneration_.load()) return;  // a newer session owns it
        if (rpcBindWatchId_ == 0) return;
        g_source_remove(rpcBindWatchId_);
        rpcBindWatchId_ = 0;
        RememberSyncedSessionLocked();
      });
    }));

    // The jwt refresh (which runs immediately at device creation) tells us
    // when the stored client no longer exists on the server. Only marshal from
    // the callback: it runs on an sdk thread, and Logout() clears subs_ --
    // which would destroy the sub whose callback is running. Its cause
    // (Device.getAuthLogoutCause, set before the listeners run) is read there
    // first, through the handle the listener was added on: device_ is mutex_'s.
    // The device signs out because the Api it is bound to did, so the Api's
    // listener has reported the same rejection already; ReportAuthLogout's
    // sign-in makes that one sign-out.
    const uint64_t deviceHandle = device_->handle();
    subs_.push_back(device_->addAuthLogoutListener([this, deviceHandle] {
      ReportAuthLogout(DeviceAuthLogoutCause(deviceHandle));
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

    // Restore the persisted routeLocal (the kill switch, inverted) the same
    // way: DeviceLocal starts at its default (true) and does not read local
    // state (the macOS DeviceManager applies exactly this at device
    // creation). LocalState defaults to true — kill switch off.
    device_->setRouteLocal(localState_->getRouteLocal());

    // Restore the persisted transport policies (client + provider) the same
    // way. The daemon's DeviceLocal persists and restores its own copy, but
    // the GUI and the daemon do not share local state, so an edit made while
    // the tunnel was down lives only in the GUI mirror until it is applied
    // here (apple DeviceManager.initDevice parity). nullopt = never edited:
    // the daemon's persisted or default policy stands.
    if (auto settings = localState_->getTransportSettings(); settings) {
      device_->setTransportSettings(settings);
    }
    if (auto settings = localState_->getProviderTransportSettings(); settings) {
      device_->setProviderTransportSettings(settings);
    }

    // Connection choice is data-plane state, so keep this lightweight listener
    // alive for the tray even when every presentation controller is closed.
    //
    // IT PUBLISHES THE WHOLE READING, not the one bit it carries. The old
    // handler pushed "DESTINATION_SET"/"DISCONNECTED" — a two-word vocabulary
    // that the page then had to read as "in flight", which is why a carrying
    // tunnel rendered "Connecting to providers" for the whole session: the
    // destination stays selected while the tunnel carries, and no later push
    // ever contradicted it. Re-reading everything means the page can never
    // hold one field from this instant beside another from a previous one.
    subs_.push_back(device_->addConnectLocationChangeListener(
        [this](std::optional<urnet::ConnectLocation>) { PublishConnectReading(); }));
    if (presentationActive_) {
      SubscribeStats();
      SubscribeDrawer();
    }
    PublishConnectReading();
    // ARMED, NOT WRITTEN. The record is handed to the remote-change listener
    // above and committed only when the pairing demonstrably syncs — see the
    // comment there. A locked or absent keyring at that moment costs the
    // ability to reattach next launch and nothing else; it can never fail this
    // start, which is already up by then.
    unsavedSession_.reset();
    if (rememberOnSync) unsavedSession_ = session;
    // The mismatched-but-well-formed case (§5 case D) throws on neither side:
    // both ends bind and dial, the handshake fails at connect time, and the
    // only evidence is getRemoteConnected() never turning true. Bound it.
    ArmRpcBindWatchdogLocked();
    EmitDrawerEvent(DrawerEvent::DeviceLifecycle);
    // The daemon has just decided what floor this session carries (Connected
    // with the block-all, or Connected without it). Read it back rather than
    // assume the request took — this is the same honesty rule the toggles now
    // follow, applied to the start path.
    EnqueueKillSwitch(KillSwitchRequest{});
    return TunnelStartResult::Started;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] start tunnel failed: %s\n", e.what());
    lastTunnelError_ = e.what();
    // The material is bound to nothing now; a reattach with it could only
    // mismatch, so forget it rather than remember a pairing that never was.
    // ForgetRpcSession covers BOTH doors: on the fresh door it drops material
    // that was never stored anyway plus any older record, and on the attach
    // door it drops the record we just proved we cannot drive.
    unsavedSession_.reset();
    ForgetRpcSession();
    rpcHostPort_.clear();
    // StartTunnel is retryable. Tear down every partially-created resource and
    // listener so a failed attempt cannot leave a subscription or
    // manager-owned controller behind for the next attempt, and stop the
    // daemon-side tunnel we just started but cannot bind to.
    TeardownDeviceLocked();
    control_.StopTunnel();
    return TunnelStartResult::Failed;
  }
}

// Commit the session that has just proved itself. Called from the FIRST
// remote_connected edge of a freshly started session and from nowhere else:
// the attach door does not arm it (its record is already stored), and a session
// that never syncs never reaches here, so the store can only ever hold a
// pairing that demonstrably worked.
//
// Everything about it is best-effort. This runs after the tunnel is up and
// carrying, so a locked keyring, a cancelled unlock prompt or no Secret Service
// at all costs exactly one thing — the next launch rebuilds the tunnel instead
// of attaching to it — and can never fail the session the user is already
// using. Requires mutex_.
void SdkHost::RememberSyncedSessionLocked() {
  if (!unsavedSession_) return;
  const RpcSessionRecord record = *unsavedSession_;
  // Consumed either way: a save that failed must not be retried on the next
  // reconnect edge, where it would re-raise the same keyring prompt against a
  // user who has already declined it once.
  unsavedSession_.reset();
  SaveRpcSession(record);
}

std::string SdkHost::LastTunnelError() {
  std::scoped_lock lock(mutex_);
  return lastTunnelError_;
}

std::string SdkHost::RpcHostPort() {
  std::scoped_lock lock(mutex_);
  return rpcHostPort_;
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
  // BOTH FEEDS, FROM ONE EVENT. The connect controller's status listener is
  // the only place CONNECTING -> CONNECTED is ever announced, and it used to
  // reach the stats feed alone — which the window drops while it is hidden,
  // and which the status row never read for its verdict. It now also
  // republishes the connect reading, ungated, so "the SDK has provider
  // sessions" reaches the status row the moment it becomes true.
  auto pub = [this] {
    PublishStats();
    PublishConnectReading();
  };
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
  presentationSubs_.push_back(device_->addTunnelChangeListener([this](bool) {
    PublishStats();
    PublishConnectReading();
  }));
  PublishStats();           // initial snapshot
  PublishConnectReading();  // ... and the reading it must never disagree with
}

LiveStats SdkHost::ReadStats() {
  LiveStats s;
  if (connectVc_) {
    s.connectionStatus = connectVc_->getConnectionStatus();
    s.connected = connectVc_->getConnected();
    // handle 0 = no grid: make NO getter calls on it (each was a recovered
    // Go nil-receiver panic on Windows — ~570 log lines per idle session)
    if (auto grid = connectVc_->getGrid()) {
      s.providerCount = grid.getWindowCurrentSize();
      s.gridWidth = grid.getWidth();
      s.gridHeight = grid.getHeight();
      if (auto pts = grid.getProviderGridPointList()) s.gridPoints = *pts;
    }
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
  } else {
    // No DeviceRemote, so no tunnel session: what provides now, if anything,
    // is the daemon's provider-only device, as its status last said
    // (ReconcileProvider). The provide dot and the discoverable line read
    // these; without them they said "not providing" over a device that is.
    s.provideMode = daemonProviderMode_.load();
    s.provideEnabled = daemonProviderRunning_.load() && s.provideMode != 0;
    s.provideHasNetworkKey = daemonProviderNetworkKey_.load();
    // The provide line's count, from the same status: a count it did not
    // give is unknown, never 0.
    const int64_t clientCount = daemonProviderClientCount_.load();
    s.provideClients = clientCount < 0 ? 0 : clientCount;
    s.provideClientsUnknown = s.provideEnabled && clientCount < 0;
  }
  return s;
}

void SdkHost::PublishStats() {
  if (onStats_) onStats_(ReadStats());
}

// ---- the one connect reading -------------------------------------------------
// EVERY field, every time, from the live getters. The defect this replaces was
// three copies of "are we connected" with three writers and three freshnesses:
// SdkHost::Connected() (destination selected AND the tunnel still ours),
// LiveStats::connected (destination selected, and only applied while the window
// was visible), and a pushed status string whose entire vocabulary was
// {DESTINATION_SET, DISCONNECTED}. See Health.hpp.
void SdkHost::NoteDaemonTunnelGone() {
  daemonTunnelGone_.store(true);
  PublishConnectReading();
}

ConnectReading SdkHost::ReadConnectReading() {
  ConnectReading r = ReadConnectFacts();
  const bool sessionUp = health::SessionUp(r.ToSignals(/*disconnectRequested=*/false));
  const int64_t nowMillis = g_get_monotonic_time() / 1000;
  int64_t armInMillis = -1;
  {
    std::scoped_lock lock(degradeMutex_);
    r.proofLoss =
        degradeHold_.Update(sessionUp, r.sdk == health::SdkStatus::Connected, nowMillis);
    const int64_t reevalAt = degradeHold_.ReevalAtMillis();
    if (reevalAt != 0 && reevalAt != degradeReevalAtMillis_) {
      degradeReevalAtMillis_ = reevalAt;
      if (degradeReevalId_ != 0) g_source_remove(degradeReevalId_);
      degradeReevalId_ = 0;
      armInMillis = reevalAt - nowMillis;
    }
  }
  if (armInMillis >= 0) {
    // one millisecond past the end, so the reading taken then is past it
    const guint id = g_timeout_add(
        static_cast<guint>(armInMillis + 1),
        [](gpointer data) -> gboolean {
          auto* self = static_cast<SdkHost*>(data);
          {
            std::scoped_lock lock(self->degradeMutex_);
            self->degradeReevalId_ = 0;
          }
          if (self->onReading_) self->onReading_(self->CurrentConnectReading());
          return G_SOURCE_REMOVE;
        },
        this);
    std::scoped_lock lock(degradeMutex_);
    degradeReevalId_ = id;
  }
  return r;
}

void SdkHost::NoteNewConnectAttempt() {
  std::scoped_lock lock(degradeMutex_);
  degradeHold_.NoteNewAttempt();
}

ConnectReading SdkHost::ReadConnectFacts() {
  ConnectReading r;
  const bool haveDevice = device_.has_value();
  r.tunnelBound = haveDevice && deviceControlGeneration_ == control_.SessionGeneration();
  // THE DAEMON'S VERDICT IS STICKY, AND IT HAS TO BE. When the daemon tears a
  // session down protectively (proven-unprotected egress, an amplification
  // storm, a DNS override it could not restore) the GUI learns it only from
  // PollDaemonHealth. Nothing in the three getters above can see it: device_
  // is still held and the control generation still matches, so tunnelBound
  // reads TRUE and the very next SDK push — which arrives ~10/s during a ramp —
  // would overwrite the verdict and put the hero back to green over a tunnel
  // that is gone. Patching tunnelBound onto a COPY of the reading, as the
  // caller used to, is self-reverting by construction.
  //
  // Cleared only by a NEW start_tunnel (StartTunnelLocked), because that is the
  // one event that makes the old verdict obsolete.
  if (daemonTunnelGone_.load()) r.tunnelBound = false;
  if (connectVc_) {
    r.rawStatus = connectVc_->getConnectionStatus();
    r.destinationSelected = connectVc_->getConnected();
    if (auto grid = connectVc_->getGrid()) r.providerCount = grid.getWindowCurrentSize();
  } else if (haveDevice) {
    // No presentation controller (the window is hidden): the destination is
    // still the honest half, and there is simply no status to report. Left
    // Unknown rather than fabricated — Unknown is not Disconnected.
    r.destinationSelected = device_->getConnectLocation().has_value();
  }
  if (haveDevice) {
    if (auto cs = device_->getContractStatus(); cs) r.insufficientBalance = cs->InsufficientBalance;
  }

  // The status latch, scoped to the session and to nothing else.
  const bool sessionUp = r.destinationSelected && r.tunnelBound;
  if (!sessionUp) {
    // THE LATCH DIES WITH THE SESSION IT DESCRIBES. Nothing here can outlive
    // its producer: the next reading over a new session starts from Unknown.
    lastKnownSdk_.store(static_cast<int>(health::SdkStatus::Unknown));
    r.sdk = health::SdkStatus::Unknown;
    // ... and so does the token the Advanced strip shows. Reporting the
    // controller's last word ("CONNECTING") beside a torn-down session is the
    // same lie in miniature.
    r.rawStatus = "DISCONNECTED";
    r.statusObserved = connectVc_.has_value();
    return r;
  }
  const health::SdkStatus parsed = health::ParseSdkStatus(r.rawStatus);
  if (parsed != health::SdkStatus::Unknown && parsed != health::SdkStatus::Disconnected) {
    lastKnownSdk_.store(static_cast<int>(parsed));
    r.sdk = parsed;
  } else {
    // Nothing usable came back this time (a controller that has just been
    // reopened, or none at all): keep the last thing this session actually
    // said.
    r.sdk = static_cast<health::SdkStatus>(lastKnownSdk_.load());
  }
  // With the presentation closed since before the session's first status
  // there is no evidence at all, and the tray keeps the session's claim.
  r.statusObserved = connectVc_.has_value() || r.sdk != health::SdkStatus::Unknown;
  // Why the window is not there yet, for the line under the status. A device
  // rpc, so only while the attempt is still building or has failed, and with
  // the presentation open, as the grid's size is.
  if (connectVc_ && r.sdk != health::SdkStatus::Connected) {
    if (auto windowStatus = device_->getWindowStatus()) r.stallReason = windowStatus->StallReason;
  }
  return r;
}

void SdkHost::PublishConnectReading() {
  if (onReading_) onReading_(ReadConnectReading());
}

// THE ONLY DEFINITION OF "ARE WE CONNECTED" LEFT IN THIS PROCESS.
//
// SdkHost::Connected() used to live here and answer a DIFFERENT question from
// LiveStats::connected, which answered a different question again from the
// status string pushed beside them — three predicates over one underlying bit,
// and the status row had to guess which pair of them to trust. There is one
// reading now and one decision table over it (health::Render); this is simply
// how a caller gets a fresh copy.
ConnectReading SdkHost::CurrentConnectReading() {
  std::scoped_lock lock(mutex_);
  return ReadConnectReading();
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
  // transport policies (dns-settings pattern): fired by the daemon's device on
  // change, forwarded over the rpc, re-fired with the daemon's truth on every
  // sync, and locally by the device remote for an edit queued while offline
  presentationSubs_.push_back(device_->addTransportSettingsChangeListener(
      [this](std::optional<urnet::TransportSettings>) {
        EmitDrawerEvent(DrawerEvent::TransportSettings);
      }));
  presentationSubs_.push_back(device_->addProviderTransportSettingsChangeListener(
      [this](std::optional<urnet::TransportSettings>) {
        EmitDrawerEvent(DrawerEvent::ProviderTransportSettings);
      }));
  presentationSubs_.push_back(device_->addBlockerEnabledChangeListener(
      [this](bool) { EmitDrawerEvent(DrawerEvent::Blocker); }));
  presentationSubs_.push_back(device_->addRouteLocalChangeListener(
      [this](bool) { EmitDrawerEvent(DrawerEvent::RouteLocal); }));
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
  // this device's provider status (P008): the earnings page's reason line,
  // demand histogram and ranking numbers. Opened with the peers, but only
  // while providing is not never, and polling only while the Earnings
  // destination is on screen (SetProviderStatusPolling).
  OpenProviderStatusLocked(device_->getProvideControlMode());

  // post quantum identity: the device's own identity key (hash) + the
  // providers with an identity-verified e2e session, via the SDK's shared
  // view controller (the apple PostQuantumIdentityStore binds the same one —
  // it re-emits the device's urnet_device_add_provider_identity_change_listener
  // feed). start() seeds the listener with the current state.
  pqiVc_ = device_->openPostQuantumIdentityViewController();
  presentationSubs_.push_back(pqiVc_->addPostQuantumIdentityListener(
      [this] { EmitDrawerEvent(DrawerEvent::ProviderIdentities); }));
  pqiVc_->start();

  // The provider-locations view controller: the SDK's, so the display order
  // (west to east about the providers' centroid), the selection and the wheel's
  // clamped ends are identical in every app.
  //
  // OPENED BEFORE the connected-provider listener below, and that order is
  // load-bearing: the controller subscribes to the same device listener when it
  // is opened, callbacks fire in subscription order, and ConnectedProviderLocations()
  // reads the controller's ordered window. Registering first would read a window
  // one notify behind.
  providerLocationsVc_ = device_->openProviderLocationsViewController();
  presentationSubs_.push_back(providerLocationsVc_->addSelectedProviderLocationChangeListener(
      [this] { EmitDrawerEvent(DrawerEvent::ProviderSelection); }));
  providerLocationsVc_->start();

  // connected provider locations: the change listener is signal-only and
  // carries no payload by design; every consumer re-reads
  // ConnectedProviderLocations(). It fires on window turnover, which is frequent,
  // so the sheet dedupes by value before touching widgets.
  presentationSubs_.push_back(device_->addConnectedProviderLocationChangeListener(
      [this] { EmitDrawerEvent(DrawerEvent::ProviderLocations); }));

  // extenders (EXTENDER.md K4/K5): the directory + gossip status, read off the
  // DEVICE so the connect page's panel shows the DAEMON's directory -- the one
  // whose dials the rings describe -- rather than this process's. The SDK
  // coalesces to one callback per second, so no throttle is needed here; the
  // panel dedupes by value anyway.
  presentationSubs_.push_back(device_->addExtenderStatusChangeListener(
      [this](std::optional<urnet::ExtenderStatus>) {
        EmitDrawerEvent(DrawerEvent::ExtenderStatus);
      }));
  // ...and this device's OWN extender role (N2, N7): the connect page's
  // extender row, and the earnings page's read-only row and the running state
  // behind its extender statistics (O4). The SDK coalesces it to one callback
  // per epoch (a second) after any change of the setting, the provide state or
  // the role, and fires none on registration, so the pages re-read the status
  // on DeviceLifecycle.
  presentationSubs_.push_back(device_->addExtenderProvideStatusChangeListener(
      [this](std::optional<urnet::ExtenderProvideStatus>) {
        EmitDrawerEvent(DrawerEvent::ExtenderProvideStatus);
      }));
  // ...and the shared view controller behind the account section's settings,
  // share and import. It is opened with the rest of the presentation and
  // closed with it, so every accessor is nullopt with the window hidden or the
  // tunnel down and the section renders its no-device state.
  extenderVc_ = device_->openExtenderViewController();
  extenderVc_->start();
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
    providerStatusSub_.reset();
    providerStatusVc_.reset();
    pqiVc_.reset();
    providerLocationsVc_.reset();
    extenderVc_.reset();
    return;
  }
  // The extender controller closes ITSELF (the SDK exposes no
  // device.closeExtenderViewController, unlike the older controllers), so stop
  // it first and then hand the handle back.
  if (extenderVc_) {
    extenderVc_->stop();
    extenderVc_->close();
  }
  extenderVc_.reset();
  if (providerLocationsVc_) {
    device_->closeProviderLocationsViewController(*providerLocationsVc_);
  }
  providerLocationsVc_.reset();
  if (pqiVc_) device_->closePostQuantumIdentityViewController(*pqiVc_);
  pqiVc_.reset();
  CloseProviderStatusLocked();
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

// ---- this device's provider status (support part P008) ----------------------
// The controller polls the API through the GUI's own space (DeviceRemote's
// GetApi) and finds this device by the remote's client id, the daemon's
// provider client. Its listener only emits an event tag, like the peers': the
// earnings page marshals onto the GTK loop and re-reads ProviderStatusNow.
// While disconnected there is no DeviceRemote, and the daemon runs the same
// controller on its provider-only device (provider_stats, below).

void SdkHost::OpenProviderStatusLocked(const std::string& provideControlMode) {
  if (!device_ || providerStatusVc_ || provideControlMode == "never") return;
  try {
    providerStatusVc_ = device_->openProviderStatusViewController();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] open provider status view controller failed: %s\n", e.what());
    providerStatusVc_.reset();
    return;
  }
  providerStatusSub_.emplace(providerStatusVc_->addProviderStatusListener(
      [this] { EmitDrawerEvent(DrawerEvent::ProviderStatus); }));
  if (providerStatusPolling_) providerStatusVc_->start();
}

void SdkHost::CloseProviderStatusLocked() {
  // the listener goes first, so nothing fires for a controller being closed;
  // the typed close is the only one that releases the controller (the
  // generic close cannot take a C++ handle)
  providerStatusSub_.reset();
  if (providerStatusVc_ && device_) {
    device_->closeProviderStatusViewController(*providerStatusVc_);
  }
  providerStatusVc_.reset();
}

SdkHost::ProviderStatusSnapshot SdkHost::ProviderStatusNow() {
  std::scoped_lock lock(mutex_);
  ProviderStatusSnapshot snapshot;
  if (!providerStatusVc_) {
    // No session: the controller the daemon runs on its provider-only device.
    if (!device_ && daemonProviderStats_) return daemonProviderStats_->status;
    return snapshot;
  }
  snapshot.open = true;
  try {
    snapshot.loaded = providerStatusVc_->getIsLoaded();
    snapshot.lastFetchError = providerStatusVc_->getLastFetchError();
    snapshot.status = providerStatusVc_->getProviderStatus();
  } catch (const std::exception& e) {
    // a malformed document must never take the page down: it reads as a
    // failed poll
    std::fprintf(stderr, "[sdk] read provider status failed: %s\n", e.what());
    snapshot.loaded = false;
    snapshot.lastFetchError = e.what();
    snapshot.status.reset();
  }
  return snapshot;
}

// ---- the provider-only device's statistics (provider_stats) -----------------
// While no DeviceRemote is bound the provider is the daemon's provider-only
// device, and its view controllers run in the daemon. This reads them about
// once a second while the Earnings destination is on screen, so the page's
// plots, its "no traffic yet" line and the provider status describe the device
// that is providing rather than saying nothing (support inbox 1521, P008).

namespace {

constexpr guint kProviderStatsPollMillis = 1000;

// One SDK payload of a provider_stats reply, in the SDK's own JSON. A part
// that does not parse is left empty: it costs its own chart, never the page.
// Logged at debug level only, because the poll would repeat it every second.
template <typename T>
std::optional<T> ProviderStatsPart(const std::string& json, const char* what) {
  if (json.empty()) return std::nullopt;
  try {
    return nlohmann::json::parse(json).get<T>();
  } catch (const std::exception& e) {
    g_debug("provide: the daemon's provider %s does not read: %s", what, e.what());
    return std::nullopt;
  }
}

}  // namespace

void SdkHost::PollDaemonProviderStatsLocked() {
  const bool unsupported = providerStatsUnsupportedGeneration_ != 0 &&
                           providerStatsUnsupportedGeneration_ == control_.SessionGeneration();
  if (provide::DaemonProviderStatsStep(device_.has_value(), daemonProviderRunning_.load(),
                                       unsupported) == provide::ProviderStatsStep::Drop) {
    DropDaemonProviderStatsLocked();
    return;
  }
  ctl::ProviderStatsRequest request;
  // asked only while the Earnings destination shows the provider status, never
  // for the connect page's switch alone
  request.poll_status = providerStatusPolling_;
  std::string error;
  std::optional<ctl::ProviderStatsReply> reply;
  try {
    reply = control_.ProviderStats(request, &error);
  } catch (const std::exception& e) {
    error = e.what();
  }
  if (!reply) {
    if (error == ctl::kErrorUnknownVerb) {
      // A daemon from before the verb: the page shows what it showed then, no
      // provider statistics while disconnected.
      g_message("provide: the system service predates provider_stats; no provider statistics "
                "while disconnected");
      providerStatsUnsupportedGeneration_ = control_.SessionGeneration();
      DropDaemonProviderStatsLocked();
    }
    // Unreachable or refused: the last snapshot stands until the status says
    // the device is gone.
    return;
  }
  if (!reply->running) {
    DropDaemonProviderStatsLocked();
    return;
  }
  DaemonProviderStats stats;
  stats.hasProviderStats = reply->has_provider_stats;
  stats.providerPoints = ProviderStatsPart<urnet::ThroughputPointList>(
      reply->provider_throughput_points_json, "series");
  stats.providerDistribution = ProviderStatsPart<urnet::TransportDistribution>(
      reply->provider_transport_distribution_json, "transport distribution");
  stats.status.open = reply->status_open;
  stats.status.loaded = reply->status_loaded;
  stats.status.lastFetchError = reply->status_last_fetch_error;
  stats.status.status =
      ProviderStatsPart<urnet::ProviderStatus>(reply->provider_status_json, "status");
  stats.statusJson = reply->provider_status_json;
  // the extender role and its series; a daemon that predates them sends
  // neither, and the extender row and plot stay hidden
  stats.extenderProvideStatus = ProviderStatsPart<urnet::ExtenderProvideStatus>(
      reply->extender_provide_status_json, "extender status");
  stats.extenderPoints = ProviderStatsPart<urnet::ThroughputPointList>(
      reply->extender_throughput_points_json, "extender series");
  stats.extenderProvideStatusJson = reply->extender_provide_status_json;
  // the setting beside the role and the daemon's word that it takes the
  // switch's write; a daemon that predates them sends neither
  stats.provideExtender = reply->provide_extender;
  stats.provideExtenderWritable = reply->provide_extender_writable;
  const bool statusChanged = !daemonProviderStats_ ||
                             daemonProviderStats_->status.open != stats.status.open ||
                             daemonProviderStats_->status.loaded != stats.status.loaded ||
                             daemonProviderStats_->status.lastFetchError !=
                                 stats.status.lastFetchError ||
                             daemonProviderStats_->statusJson != stats.statusJson;
  const bool extenderChanged =
      !daemonProviderStats_ ||
      daemonProviderStats_->extenderProvideStatusJson != stats.extenderProvideStatusJson ||
      daemonProviderStats_->provideExtender != stats.provideExtender ||
      daemonProviderStats_->provideExtenderWritable != stats.provideExtenderWritable;
  daemonProviderStats_ = std::move(stats);
  // the events the DeviceRemote and its controllers raise for the same facts
  EmitDrawerEvent(DrawerEvent::Throughput);
  if (statusChanged) EmitDrawerEvent(DrawerEvent::ProviderStatus);
  if (extenderChanged) EmitDrawerEvent(DrawerEvent::ExtenderProvideStatus);
}

void SdkHost::DropDaemonProviderStatsLocked() {
  if (!daemonProviderStats_) return;
  daemonProviderStats_.reset();
  EmitDrawerEvent(DrawerEvent::Throughput);
  EmitDrawerEvent(DrawerEvent::ProviderStatus);
  EmitDrawerEvent(DrawerEvent::ExtenderProvideStatus);
}

void SdkHost::ScheduleProviderStatsPollLocked(bool readNow) {
  // The provider-only device's statistics: read at once, then about once a
  // second, while a destination wants them. Stopping keeps the last snapshot,
  // as the provider status controller keeps its own.
  if (providerStatusPolling_ || providerExtenderPolling_) {
    if (providerStatsPollId_ == 0) {
      providerStatsPollId_ = g_timeout_add(
          kProviderStatsPollMillis,
          [](gpointer data) -> gboolean {
            auto* self = static_cast<SdkHost*>(data);
            std::scoped_lock pollLock(self->mutex_);
            self->PollDaemonProviderStatsLocked();
            return G_SOURCE_CONTINUE;
          },
          this);
    }
    if (readNow) PollDaemonProviderStatsLocked();
  } else if (providerStatsPollId_ != 0) {
    g_source_remove(providerStatsPollId_);
    providerStatsPollId_ = 0;
  }
}

void SdkHost::SetProviderExtenderPolling(bool polling) {
  std::scoped_lock lock(mutex_);
  if (providerExtenderPolling_ == polling) return;
  providerExtenderPolling_ = polling;
  ScheduleProviderStatsPollLocked(/*readNow=*/polling);
}

void SdkHost::SetProviderStatusPolling(bool polling) {
  std::scoped_lock lock(mutex_);
  if (providerStatusPolling_ == polling) return;
  providerStatusPolling_ = polling;
  ScheduleProviderStatsPollLocked(/*readNow=*/polling);
  if (!providerStatusVc_) return;
  if (polling) {
    providerStatusVc_->start();
  } else {
    providerStatusVc_->stop();
  }
}

// ---- connect drawer accessors ----------------------------------------------

std::optional<urnet::ConnectLocation> SdkHost::SelectedLocation() {
  std::scoped_lock lock(mutex_);
  // the controller keeps its selection when the connect location goes nil
  if (connectVc_) return connectVc_->getSelectedLocation();
  if (device_) {
    if (auto location = device_->getConnectLocation()) return location;
  }
  if (localState_) {
    if (auto location = localState_->getConnectLocation()) return location;
    return localState_->getDefaultLocation();
  }
  return std::nullopt;
}

std::optional<urnet::ConnectLocation> SdkHost::ConnectedLocation() {
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

bool SdkHost::GetRouteLocal() {
  std::scoped_lock lock(mutex_);
  if (device_) return device_->getRouteLocal();
  return !localState_ || localState_->getRouteLocal();  // default true (kill switch off)
}

// ---- kill switch -----------------------------------------------------------
// Three legs (see KillSwitchStatus in the header). Legs 1 and 2 are the SOFT
// ones the toggles used to drive alone; leg 3 is the nftables ruleset in
// urnetworkd, which is what actually blocks anything.

void SdkHost::ApplyRouteLocalLocked(bool routeLocal) {
  // LocalState FIRST — it is the persistent truth (unlike the blocker, the
  // daemon's DeviceLocal neither persists nor restores routeLocal), it is what
  // StartTunnel replays at the next device creation, and it is what survives a
  // crash between the two writes. macOS DeviceManager.setRouteLocalInternal
  // orders it the same way.
  if (localState_) localState_->setRouteLocal(routeLocal);
  if (device_) {
    // A throwing device rpc must not cost the persisted preference, which is
    // already written, nor the enforcement leg, which has not run yet.
    ReadGuarded<bool>(
        "device setRouteLocal",
        [&] {
          device_->setRouteLocal(routeLocal);
          return true;
        },
        false);
  }
}

bool SdkHost::KillSwitchRequestedLocked() {
  // Parity rule (docs/parity/settings.md §113): prefer the device, then
  // LocalState, and with neither claim the PERMISSIVE default — never the
  // strict one. A host that cannot read its own preference must not tell the
  // user their traffic is being blocked.
  if (device_) {
    return !ReadGuarded<bool>(
        "device getRouteLocal", [&] { return device_->getRouteLocal(); },
        localState_ ? localState_->getRouteLocal() : true);
  }
  if (localState_) return !localState_->getRouteLocal();
  return false;
}

bool SdkHost::CurrentKillSwitch() {
  std::scoped_lock lock(mutex_);
  return KillSwitchRequestedLocked();
}

KillSwitchStatus SdkHost::CurrentKillSwitchStatus() {
  std::scoped_lock lock(mutex_);
  // The preference is always live; the installed half is whatever the last
  // round trip reported (installed_known=false until one has happened, which
  // is UNKNOWN and deliberately not "off").
  killSwitchStatus_.requested = KillSwitchRequestedLocked();
  return killSwitchStatus_;
}

void SdkHost::SetKillSwitch(bool on, KillSwitchDone done) {
  {
    std::scoped_lock lock(mutex_);
    ApplyRouteLocalLocked(!on);  // legs 1 + 2, synchronously
    killSwitchStatus_.requested = on;
    killSwitchStatus_.pending = true;
    // The previous reading described the previous request. Do NOT carry it
    // forward: "the switch is on and the floor from the last answer was
    // armed" is a claim about a state that no longer exists.
    killSwitchStatus_.installed_known = false;
    killSwitchStatus_.in_force = false;
    killSwitchStatus_.installed = ctl::KillSwitchState::Off;
    killSwitchStatus_.detail.clear();
  }
  // The two other surfaces echo through the feed they already ride; the
  // caller's own `done` carries the authoritative read-back.
  EmitDrawerEvent(DrawerEvent::RouteLocal);
  KillSwitchRequest request;
  request.apply = true;
  request.wanted = on;
  request.done = std::move(done);
  killSwitchWritesPending_.fetch_add(1);
  EnqueueKillSwitch(std::move(request));
}

void SdkHost::RefreshKillSwitchStatus(KillSwitchDone done) {
  KillSwitchRequest request;  // apply=false: read-back only
  request.done = std::move(done);
  EnqueueKillSwitch(std::move(request));
}

void SdkHost::EnqueueKillSwitch(KillSwitchRequest request) {
  std::unique_lock<std::mutex> lock(killSwitchMutex_);
  if (killSwitchQuit_) return;  // shutting down: nothing may block quit
  if (!killSwitchWorker_.joinable()) {
    killSwitchWorker_ = std::thread([this] { KillSwitchWorkerMain(); });
  }
  killSwitchQueue_.push_back(std::move(request));
  lock.unlock();
  killSwitchCv_.notify_one();
}

void SdkHost::KillSwitchWorkerMain() {
  for (;;) {
    KillSwitchRequest request;
    {
      std::unique_lock<std::mutex> lock(killSwitchMutex_);
      killSwitchCv_.wait(lock, [this] { return killSwitchQuit_ || !killSwitchQueue_.empty(); });
      // Quit wins even with work outstanding: the process is going away, and a
      // completion that lands after the main loop is gone helps nobody.
      if (killSwitchQuit_) return;
      request = std::move(killSwitchQueue_.front());
      killSwitchQueue_.pop_front();
    }
    // An escaping exception on a worker thread is std::terminate.
    try {
      RunKillSwitchRequest(std::move(request));
    } catch (const std::exception& e) {
      g_warning("sdkhost: kill switch request threw: %s", e.what());
    } catch (...) {
      g_warning("sdkhost: kill switch request threw");
    }
  }
}

void SdkHost::StopKillSwitchWorker() {
  {
    std::scoped_lock lock(killSwitchMutex_);
    killSwitchQuit_ = true;
    killSwitchQueue_.clear();
  }
  killSwitchCv_.notify_all();
  if (killSwitchWorker_.joinable()) killSwitchWorker_.join();
}

// THE ENFORCEMENT LEG, on the worker. Two round trips on purpose: the write,
// then an INDEPENDENT status read. The write's own reply carries a status too,
// but re-reading is what makes "what is really in force" a fact about the
// daemon rather than an echo of what we just asked for — and it is the only
// way to catch a write that succeeded and was then undone (the reaper lifting
// the floor, or a `nft flush ruleset` from elsewhere on the machine).
void SdkHost::RunKillSwitchRequest(KillSwitchRequest request) {
  std::string writeError;
  bool wrote = true;
  if (request.apply) {
    ctl::StatusReply echoed;
    wrote = control_.SetKillSwitch(request.wanted, &echoed, &writeError);
  }
  std::string readError;
  const std::optional<ctl::StatusReply> fresh = control_.Status(&readError);
  const DaemonSessionState session = control_.LastSessionState();

  KillSwitchStatus out;
  out.session = session;
  // WHY the channel is down, not just that it is. Without this the copy for
  // every Unreachable state collapses into "the service is not running", which
  // on a fresh install (empty `urnetwork` group -> connect(2) EACCES) is both
  // false and unactionable.
  out.unreachable_reason = control_.LastUnreachableReason();
  // Still pending while ANY write is outstanding — including one issued after
  // this read-back was queued, whose answer has not landed yet.
  if (request.apply) killSwitchWritesPending_.fetch_sub(1);
  out.pending = killSwitchWritesPending_.load() > 0;
  if (fresh) {
    out.installed_known = true;
    out.installed = fresh->kill_switch;
    out.tunnel_state = fresh->tunnel_state;
    out.detail = fresh->kill_switch_detail;
    out.in_force = out.installed == ctl::KillSwitchState::Armed ||
                   out.installed == ctl::KillSwitchState::Connected;
  } else {
    // UNKNOWN, not off. The switch may well be armed from a previous session —
    // the nftables table is not process-bound and survives a dead daemon — so
    // reporting "off" here would be a fabrication in the dangerous direction.
    out.installed_known = false;
    out.detail = readError;
  }
  if (!wrote && !writeError.empty()) {
    // The write's own error wins the explanation: it is why the state is what
    // it is, and the status read may have succeeded and say nothing at all.
    out.detail = writeError;
  }
  {
    std::scoped_lock lock(mutex_);
    out.requested = KillSwitchRequestedLocked();
    killSwitchStatus_ = out;
  }
  if (request.apply && !wrote) {
    g_warning("sdkhost: the kill switch enforcement leg failed: %s",
              writeError.empty() ? "(no detail)" : writeError.c_str());
  }
  // Same contract as every other SDK listener: emit the tag on this thread and
  // let the window marshal. The two surfaces that ride the drawer feed re-read
  // CurrentKillSwitchStatus() from it.
  EmitDrawerEvent(DrawerEvent::RouteLocal);
  if (request.done) {
    PostToMain([done = std::move(request.done), out]() mutable { done(out); });
  }
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

std::optional<urnet::TransportDistribution> SdkHost::ClientTransportDistribution() {
  std::scoped_lock lock(mutex_);
  if (!contractVc_) return std::nullopt;
  return contractVc_->getTransportDistribution();
}

std::optional<urnet::TransportDistribution> SdkHost::ProviderTransportDistribution() {
  std::scoped_lock lock(mutex_);
  if (contractVc_) return contractVc_->getProviderTransportDistribution();
  // no session: the provider-only device's, as the daemon last read it
  if (!device_ && daemonProviderStats_) return daemonProviderStats_->providerDistribution;
  return std::nullopt;
}

std::optional<urnet::ThroughputPointList> SdkHost::ProviderThroughputPoints() {
  std::scoped_lock lock(mutex_);
  if (contractVc_) return contractVc_->getProviderThroughputPoints();
  if (!device_ && daemonProviderStats_) return daemonProviderStats_->providerPoints;
  return std::nullopt;
}

std::optional<urnet::ThroughputPointList> SdkHost::ExtenderThroughputPoints() {
  std::scoped_lock lock(mutex_);
  if (contractVc_) return contractVc_->getExtenderThroughputPoints();
  // no session: what the provider-only device's extender role relayed
  if (!device_ && daemonProviderStats_) return daemonProviderStats_->extenderPoints;
  return std::nullopt;
}

bool SdkHost::HasProviderStats() {
  std::scoped_lock lock(mutex_);
  if (contractVc_) return contractVc_->getProviderPacketStats().has_value();
  return !device_ && daemonProviderStats_ && daemonProviderStats_->hasProviderStats;
}

bool SdkHost::DeviceHasProviderStats() {
  std::scoped_lock lock(mutex_);
  if (!device_) return daemonProviderStats_ && daemonProviderStats_->hasProviderStats;
  try {
    return device_->getProviderPacketStats().has_value();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] getProviderPacketStats failed: %s\n", e.what());
    return false;
  }
}

std::optional<urnet::TransportSettings> SdkHost::GetTransportSettings() {
  std::scoped_lock lock(mutex_);
  if (device_) {
    if (auto settings = device_->getTransportSettings(); settings) return settings;
  }
  if (localState_) return localState_->getTransportSettings();
  return std::nullopt;
}

void SdkHost::SetTransportSettings(const urnet::TransportSettings& settings) {
  std::scoped_lock lock(mutex_);
  // apply live over the device rpc (the daemon persists it in its own local
  // state) AND mirror into the GUI local state: with the tunnel down the
  // mirror is the only copy, and StartTunnel seeds the device from it
  if (device_) device_->setTransportSettings(settings);
  if (localState_) {
    try {
      localState_->setTransportSettings(settings);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[sdk] persist transport settings failed: %s\n", e.what());
    }
  }
}

std::optional<urnet::TransportSettings> SdkHost::GetProviderTransportSettings() {
  std::scoped_lock lock(mutex_);
  if (device_) {
    if (auto settings = device_->getProviderTransportSettings(); settings) return settings;
  }
  if (localState_) return localState_->getProviderTransportSettings();
  return std::nullopt;
}

void SdkHost::SetProviderTransportSettings(const urnet::TransportSettings& settings) {
  std::scoped_lock lock(mutex_);
  if (device_) device_->setProviderTransportSettings(settings);
  if (localState_) {
    try {
      localState_->setProviderTransportSettings(settings);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[sdk] persist provider transport settings failed: %s\n", e.what());
    }
  }
  // A provider-only device runs on the policy it was built with; a new one
  // (the daemon rebuilds for a changed request) picks the edit up.
  ReconcileProviderLocked("provider transport policy changed", /*userInitiated=*/true,
                          /*settingsChanged=*/true);
}

std::optional<urnet::TransportStatus> SdkHost::GetTransportStatus() {
  std::scoped_lock lock(mutex_);
  return device_ ? device_->getTransportStatus() : std::nullopt;
}

std::optional<urnet::TransportStatus> SdkHost::GetProviderTransportStatus() {
  std::scoped_lock lock(mutex_);
  return device_ ? device_->getProviderTransportStatus() : std::nullopt;
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
  AddBlockActionOverrideLocked(override_);
}

bool SdkHost::AddBlockActionOverrideLocked(const urnet::BlockActionOverride& override_) {
  if (device_) {
    device_->addBlockActionOverride(override_);  // the device persists
    return true;
  }
  if (localState_) {
    urnet::BlockActionOverrideList overrides;
    if (auto current = localState_->getBlockActionOverrides()) overrides = std::move(*current);
    overrides.push_back(override_);
    localState_->setBlockActionOverrides(overrides);
    return true;
  }
  return false;
}

std::string SdkHost::AddHostBlockRule(const urnet::StringList& hosts, bool block) {
  if (hosts.empty()) return {};
  urnet::BlockActionOverride override_;
  override_.OverrideId = urnet::newId();
  override_.Hosts = hosts;
  urnet::BlockOverride blockOverride;
  blockOverride.Block = block;
  override_.BlockOverride = blockOverride;
  std::scoped_lock lock(mutex_);
  return AddBlockActionOverrideLocked(override_) ? *override_.OverrideId : std::string();
}

std::string SdkHost::AddHostRouteRule(const urnet::StringList& hosts, bool local) {
  if (hosts.empty()) return {};
  urnet::BlockActionOverride override_;
  override_.OverrideId = urnet::newId();
  override_.Hosts = hosts;
  urnet::RouteOverride route;
  route.Local = local;
  route.Pin = false;
  override_.RouteOverride = route;
  std::scoped_lock lock(mutex_);
  return AddBlockActionOverrideLocked(override_) ? *override_.OverrideId : std::string();
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
  if (!device_ || !providerLocationsVc_) {
    return std::nullopt;  // tunnel down: the sheet shows the unavailable state
  }
  // The view controller's window, not the device's: same providers, in the
  // shared display order, and read from the controller so the rows and the
  // selection always come from one snapshot.
  return providerLocationsVc_->getProviderLocations();
}

void SdkHost::RemoveConnectedProvider(const std::string& clientId) {
  std::scoped_lock lock(mutex_);
  if (!device_ || clientId.empty()) return;
  if (providerLocationsVc_) {
    // through the view controller: it hands the selection to the nearest
    // remaining provider when the removed one is selected, as every other app does
    providerLocationsVc_->removeProvider(clientId);
    return;
  }
  device_->removeConnectedProvider(clientId);
}

std::string SdkHost::SelectedProviderClientId() {
  std::scoped_lock lock(mutex_);
  if (!providerLocationsVc_) return std::string();
  return providerLocationsVc_->getSelectedClientId();
}

void SdkHost::SetSelectedProviderClientId(const std::string& clientId) {
  std::scoped_lock lock(mutex_);
  if (!providerLocationsVc_) return;
  providerLocationsVc_->setSelectedClientId(clientId);
}

void SdkHost::StepProviderSelection(int steps) {
  std::scoped_lock lock(mutex_);
  if (!providerLocationsVc_ || steps == 0) return;
  providerLocationsVc_->stepSelection(steps);
}

// ---- extenders (EXTENDER.md K4 to K8, N2 to N8) -----------------------------

std::optional<urnet::ExtenderStatus> SdkHost::GetExtenderStatus() {
  std::scoped_lock lock(mutex_);
  if (!device_) return std::nullopt;  // no session: the panel reads disconnected
  try {
    return device_->getExtenderStatus();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] getExtenderStatus failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::optional<urnet::ExtenderProvideStatus> SdkHost::DeviceExtenderProvideStatusLocked() {
  try {
    return device_->getExtenderProvideStatus();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] getExtenderProvideStatus failed: %s\n", e.what());
    return std::nullopt;
  }
}

provide::ExtenderSwitchSource SdkHost::ExtenderSwitchSourceLocked() const {
  return provide::ExtenderSwitchSourceFor(
      device_.has_value(), daemonProviderStats_.has_value(),
      daemonProviderStats_ && daemonProviderStats_->provideExtenderWritable);
}

std::optional<urnet::ExtenderProvideStatus> SdkHost::GetExtenderProvideStatus() {
  std::scoped_lock lock(mutex_);
  switch (ExtenderSwitchSourceLocked()) {
    case provide::ExtenderSwitchSource::Device:
      return DeviceExtenderProvideStatusLocked();
    case provide::ExtenderSwitchSource::Daemon:
      // no session: the provider-only device's role, whose switch the daemon
      // writes
      return daemonProviderStats_->extenderProvideStatus;
    case provide::ExtenderSwitchSource::None:
      break;
  }
  return std::nullopt;  // nothing takes the switch's write: the row hides
}

std::optional<urnet::ExtenderProvideStatus> SdkHost::ProviderExtenderProvideStatus() {
  std::scoped_lock lock(mutex_);
  if (device_) return DeviceExtenderProvideStatusLocked();
  // no session: the provider-only device's role, as the daemon last read it
  if (daemonProviderStats_) return daemonProviderStats_->extenderProvideStatus;
  return std::nullopt;
}

bool SdkHost::GetProvideExtender() {
  std::scoped_lock lock(mutex_);
  switch (ExtenderSwitchSourceLocked()) {
    case provide::ExtenderSwitchSource::Device:
      return device_->getProvideExtender();
    case provide::ExtenderSwitchSource::Daemon:
      return daemonProviderStats_->provideExtender;
    case provide::ExtenderSwitchSource::None:
      break;
  }
  // the setting's default (N4); the row is hidden then
  return true;
}

void SdkHost::SetProvideExtender(bool on) {
  std::scoped_lock lock(mutex_);
  switch (ExtenderSwitchSourceLocked()) {
    case provide::ExtenderSwitchSource::Device:
      device_->setProvideExtender(on);
      return;
    case provide::ExtenderSwitchSource::Daemon: {
      std::string error;
      if (!control_.SetProvideExtender(on, &error)) {
        g_warning("extender: the system service did not write the provide extender setting: %s",
                  error.empty() ? "no detail" : error.c_str());
      }
      // Read back at once, written or refused, and redrawn after the switch's
      // guess: the row shows the setting as it stands.
      PollDaemonProviderStatsLocked();
      EmitDrawerEvent(DrawerEvent::ExtenderProvideStatus);
      return;
    }
    case provide::ExtenderSwitchSource::None:
      break;
  }
  g_warning("extender: dropping a provide extender write with no device");
}

std::optional<urnet::ExtenderSettings> SdkHost::GetExtenderSettings() {
  std::scoped_lock lock(mutex_);
  if (!extenderVc_) return std::nullopt;
  try {
    return extenderVc_->getSettings();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] extender getSettings failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::optional<urnet::ExtenderSettings> SdkHost::SetExtenderSettings(
    const std::string& dnsName, const std::string& gossipUrl,
    const std::vector<std::string>& hosts) {
  std::scoped_lock lock(mutex_);
  if (!extenderVc_) return std::nullopt;
  try {
    // An EMPTY host list is a real edit (the user cleared every manual
    // bootstrap address), so it is sent as an empty list rather than as "no
    // opinion" -- passing nullopt would leave the previous list in place and
    // the form would silently refuse to clear.
    return extenderVc_->setSettings(dnsName, gossipUrl, urnet::StringList(hosts));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] extender setSettings failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::optional<urnet::ExtenderShareResult> SdkHost::BuildExtenderShare(bool includeSettings) {
  std::scoped_lock lock(mutex_);
  if (!extenderVc_) return std::nullopt;
  try {
    return extenderVc_->buildShare(includeSettings);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] extender buildShare failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::optional<urnet::ExtenderShareDecodeResult> SdkHost::DecodeExtenderShare(
    const std::string& text) {
  std::scoped_lock lock(mutex_);
  if (!extenderVc_) return std::nullopt;
  try {
    return extenderVc_->decodeShare(text);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] extender decodeShare failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::optional<urnet::ExtenderImportResult> SdkHost::ImportExtenderShare(const std::string& text,
                                                                       bool useSettings) {
  std::scoped_lock lock(mutex_);
  if (!extenderVc_) return std::nullopt;
  try {
    return extenderVc_->importShare(text, useSettings);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] extender importShare failed: %s\n", e.what());
    return std::nullopt;
  }
}

std::optional<urnet::NetExtender> SdkHost::GetPrivateExtender() {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return std::nullopt;
  try {
    return networkSpace_->getNetExtender();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] getNetExtender failed: %s\n", e.what());
    return std::nullopt;
  }
}

bool SdkHost::SetPrivateExtender(const std::string& ip, const std::string& secret) {
  std::scoped_lock lock(mutex_);
  if (!spaceManager_ || !networkSpace_) return false;
  try {
    // updateNetworkSpaceValues takes the WHOLE value set, not a patch, so the
    // write starts from what the space STORES (its toJson) and changes the
    // private extender alone. Not from the getters: they answer EFFECTIVE
    // values, so writing them back pinned every derived default (the extender
    // dns name, the gossip url, the bundled root keys) as an explicit
    // override, and the values no getter reads -- alt_url, sn_chain, the VLESS
    // server -- were silently dropped.
    const auto stored = ParseStoredNetworkSpace(networkSpace_->toJson());
    if (!stored) {
      // without a readable key the write would land in a different space
      std::fprintf(stderr, "[sdk] set private extender refused: the space json does not read\n");
      return false;
    }
    urnet::NetworkSpaceValues values = stored->values;

    // both fields empty = "no private extender": the advanced override is off
    // and discovery resumes
    values.net_extender.reset();
    if (!ip.empty() || !secret.empty()) {
      urnet::NetExtender netExtender;
      netExtender.ip = ip;
      netExtender.secret = secret;
      values.net_extender = netExtender;
    }
    // Saving what is already stored is not a write: the manager REBUILDS a
    // space it is handed unchanged values for, closing the one everything here
    // is derived from (a changed extender is applied in place instead).
    const auto& before = stored->values.net_extender;
    const auto& after = values.net_extender;
    if (before.has_value() == after.has_value() &&
        (!after || (before->ip == after->ip && before->secret == after->secret))) {
      return true;
    }

    networkSpace_ = spaceManager_->updateNetworkSpaceValues(stored->key, values);
    // the space stays the active one, unless it is the override's, which is
    // bound for this process only
    if (!IsLaunchOverrideSpace(LaunchOverrideFromEnvironment(),
                               stored->key.host_name.value_or(std::string()),
                               stored->key.env_name.value_or(std::string()))) {
      spaceManager_->setActiveNetworkSpace(*networkSpace_);
    }
    // ...and re-derive what hangs off the space, exactly as ApplyNetworkServer
    // does: the handle is new, and a freshly derived Api carries no token.
    api_ = networkSpace_->getApi();
    AdoptSpaceApiLocked();
    asyncLocalState_ = networkSpace_->getAsyncLocalState();
    localState_ = asyncLocalState_->getLocalState();
    if (const std::string byJwt = localState_->getByJwt(); !byJwt.empty()) {
      api_->setByJwt(byJwt);
    }
    // a provider-only device takes the new space now
    ReconcileProviderAfterSpaceChange("private extender saved");
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[sdk] set private extender failed: %s\n", e.what());
    return false;
  }
}

bool SdkHost::ResetExtenders(std::function<void(ExtenderResetOutcome)> done) {
  if (!done) return false;
  // The space by a handle of its own, the manager's object for the key that
  // networkSpace_ holds, so neither the sdk's reset nor the daemon's answer
  // runs under mutex_.
  std::optional<urnet::NetworkSpace> space;
  ctl::ResetExtendersRequest request;
  {
    std::scoped_lock lock(mutex_);
    if (!spaceManager_ || !networkSpace_) return false;
    try {
      const std::optional<urnet::NetworkSpaceKey> key = networkSpace_->getKey();
      if (!key || !key->host_name || !key->env_name) return false;
      urnet::NetworkSpace held = spaceManager_->getNetworkSpace(*key);
      if (!held) return false;
      request.host_name = *key->host_name;
      request.env_name = *key->env_name;
      space = std::move(held);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[sdk] extender reset: the network space did not read: %s\n", e.what());
      return false;
    }
  }
  bool expected = false;
  if (!extenderResetBusy_.compare_exchange_strong(expected, true)) return false;

  std::scoped_lock lock(extenderResetWorkerMutex_);
  // The previous worker cleared extenderResetBusy_ before its marshal, so its
  // thread object can still be joinable, and assigning over a joinable
  // std::thread is std::terminate. That join waits for no more than the
  // marshal's enqueue.
  if (extenderResetWorker_.joinable()) extenderResetWorker_.join();
  extenderResetWorker_ = std::thread([this, space = std::move(*space), request,
                                      done = std::move(done)]() mutable {
    ExtenderResetOutcome outcome;
    // The daemon's answer, which decides whether the reset is owed to it.
    bool sent = false;
    bool taken = false;
    std::string code;
    // An escaping exception on a worker thread is std::terminate.
    try {
      request.extender_reset_id = space.resetExtenders();
      outcome.reset = !request.extender_reset_id.empty();
      if (outcome.reset) {
        bool daemonReset = false;
        std::string error;
        sent = true;
        taken = control_.ResetExtenders(request, &daemonReset, &error, &code);
        if (taken) {
          outcome.daemonReset = daemonReset;
          g_message("extender: reset %s; urnetworkd %s", request.extender_reset_id.c_str(),
                    daemonReset ? "reset the space it holds"
                                : "holds no space to reset, or reset it already");
        } else if (code == ctl::kCodeStartInProgress) {
          g_message("extender: reset %s; urnetworkd is bringing a tunnel up, so it gets the "
                    "reset again once that settles",
                    request.extender_reset_id.c_str());
        } else if (error == ctl::kErrorUnknownVerb) {
          g_message("extender: reset %s; the system service predates reset_extenders",
                    request.extender_reset_id.c_str());
        } else {
          // unreachable or a refused authorization: the daemon's next import of
          // this space carries the reset
          g_message("extender: reset %s; urnetworkd did not take it now (code=%s): %s; its "
                    "next tunnel or provider start carries it",
                    request.extender_reset_id.c_str(), code.empty() ? "none" : code.c_str(),
                    error.empty() ? "no detail" : error.c_str());
        }
      } else {
        g_warning("extender: the network space did not reset");
      }
    } catch (const std::exception& e) {
      g_warning("extender: the reset threw: %s", e.what());
    } catch (...) {
      g_warning("extender: the reset threw");
    }
    // Cleared here, before the marshal, so a main loop that never runs the
    // completion cannot wedge the next reset.
    extenderResetBusy_.store(false);
    PostToMain([this, sent, taken, code, request, done = std::move(done), outcome]() mutable {
      // on the main loop, where the health poll takes what is owed
      if (sent) owedExtenderReset_.NoteAnswer(request, taken, code);
      done(outcome);
    });
  });
  return true;
}

// ---- reliability / exits ---------------------------------------------------
// Everything here reads the DeviceRemote's smart-routing getters, which are
// forwarded over the loopback mTLS device rpc to the DeviceLocal in
// urnetworkd. They are SYNCHRONOUS and they are several, which is the whole
// reason this pair exists rather than a handful of one-line accessors: the
// batch must be one lock hold (so the tables agree about which session they
// describe) and it must not be on the GTK loop (so a slow daemon is a stale
// pane, not a frozen app).

ReliabilitySnapshot SdkHost::ReadReliability(ReliabilityRead scope) {
  // ONE hold for the whole batch, deliberately. Separate holds would let the
  // exit table and the destination table come from either side of a teardown,
  // and the inspector's join (destination ip -> client id -> exit) would then
  // produce a PLAUSIBLE WRONG answer — worse than "I don't know".
  //
  // The cost is that a slow daemon holds mutex_ for the batch, and mutex_ is
  // what the UI-thread accessors take. That cost is why ExitsOnly exists and
  // why neither caller may run this on the main loop.
  std::scoped_lock lock(mutex_);
  ReliabilitySnapshot snap;
  if (!device_) return snap;  // no session: haveDevice false, everything UNKNOWN
  snap.haveDevice = true;
  snap.remoteConnected = ReadGuarded<bool>(
      "getRemoteConnected", [&] { return device_->getRemoteConnected(); }, false);
  snap.exits = ReadGuarded<std::optional<urnet::ExitList>>(
      "getExits", [&] { return device_->getExits(); }, std::nullopt);
  snap.destinationExits = ReadGuarded<std::optional<urnet::DestinationExitList>>(
      "getDestinationExits", [&] { return device_->getDestinationExits(); }, std::nullopt);
  if (scope == ReliabilityRead::ExitsOnly) return snap;
  snap.settings = ReadGuarded<std::optional<urnet::ReliabilitySettings>>(
      "getReliabilitySettings", [&] { return device_->getReliabilitySettings(); },
      std::nullopt);
  snap.metrics = ReadGuarded<std::optional<urnet::ReliabilityMetrics>>(
      "getReliabilityMetrics", [&] { return device_->getReliabilityMetrics(); }, std::nullopt);
  snap.probeSuiteRunning = ReadGuarded<bool>(
      "probeSuiteRunning", [&] { return device_->probeSuiteRunning(); }, false);
  snap.probeResults = ReadGuarded<std::optional<urnet::ProbeResultList>>(
      "getProbeResults", [&] { return device_->getProbeResults(); }, std::nullopt);
  return snap;
}

bool SdkHost::RequestDaemonStatus(std::function<void(std::optional<ctl::StatusReply>)> done) {
  if (!done) return false;
  bool expected = false;
  if (!daemonStatusBusy_.compare_exchange_strong(expected, true)) return false;
  std::scoped_lock lock(daemonStatusWorkerMutex_);
  // the previous worker cleared the gate before its marshal, so it may still
  // be joinable; its join returns as soon as that enqueue is done
  if (daemonStatusWorker_.joinable()) daemonStatusWorker_.join();
  daemonStatusWorker_ = std::thread([this, done = std::move(done)]() mutable {
    std::optional<ctl::StatusReply> status;
    try {
      status = control_.Status();
    } catch (const std::exception& e) {
      g_warning("sdkhost: daemon status read threw: %s", e.what());
    } catch (...) {
      g_warning("sdkhost: daemon status read threw");
    }
    // cleared here, before the marshal, so a main loop that never runs the
    // completion cannot wedge the next read
    daemonStatusBusy_.store(false);
    PostToMain([done = std::move(done), status = std::move(status)]() mutable {
      done(std::move(status));
    });
  });
  return true;
}

bool SdkHost::RequestReliability(ReliabilityRead scope,
                                 std::function<void(ReliabilitySnapshot)> done) {
  if (!done) return false;
  bool expected = false;
  // Single-flight. A refresh that is slower than its own tick must SKIP, never
  // queue: queued reads stack behind mutex_ and the pane then lags by however
  // many ticks the daemon was slow for.
  if (!reliabilityBusy_.compare_exchange_strong(expected, true)) return false;

  std::scoped_lock lock(reliabilityWorkerMutex_);
  // The previous worker cleared reliabilityBusy_ before its final marshal, so
  // its thread object can still be joinable here — and assigning over a
  // joinable std::thread is std::terminate. The join returns as soon as that
  // worker's PostToMain enqueue is done (g_idle_add, not a wait), so this does
  // not put a daemon round trip on the main loop.
  if (reliabilityWorker_.joinable()) reliabilityWorker_.join();
  reliabilityWorker_ = std::thread([this, scope, done = std::move(done)]() mutable {
    // ReadReliability guards every rpc, so nothing should escape — but an
    // escaping exception on a worker thread is std::terminate, and a snapshot
    // that says UNKNOWN everywhere is the honest fallback.
    ReliabilitySnapshot snap;
    try {
      snap = ReadReliability(scope);
    } catch (const std::exception& e) {
      g_warning("sdkhost: reliability read threw: %s", e.what());
    } catch (...) {
      g_warning("sdkhost: reliability read threw");
    }
    // Cleared HERE, on the worker, BEFORE the marshal: the gate must not
    // depend on the main loop ever running the completion. A main loop that is
    // blocked, or gone at quit, would otherwise wedge every later read for the
    // process lifetime and the pane would look merely stale rather than broken.
    reliabilityBusy_.store(false);
    PostToMain([done = std::move(done), snap = std::move(snap)]() mutable {
      done(std::move(snap));
    });
  });
  return true;
}

void SdkHost::ConnectBestAvailable() {
  // a location pick or a press while out of balance starts nothing
  if (connectGate_ && connectGate_([this] { ConnectBestAvailable(); })) return;
  // a deliberate connect: proof the last destination earned does not carry over
  NoteNewConnectAttempt();
  std::scoped_lock lock(mutex_);
  // THE CALLER GOT HERE BELIEVING THERE IS A SESSION. Verify that with the
  // daemon before driving anything: if the service restarted (or another
  // client stopped the tunnel), the DeviceLocal on the other end of our pinned
  // rpc is gone, and both branches below would write into an address with
  // nothing behind it — no traffic, no error, no journal entry, which is
  // precisely the "I press Connect and nothing happens" this path caused.
  //
  // A `status` round trip, not just the cached session generation: nothing has
  // necessarily touched the control socket since the daemon died, so the
  // generation can still look current. The verb answers off a published
  // snapshot, and the read is what discovers the closed socket.
  if (device_) {
    std::string statusError;
    const std::optional<ctl::StatusReply> status = control_.Status(&statusError);
    const bool live = status && status->tunnel_state == ctl::TunnelState::Up &&
                      status->rpc_pinned &&
                      deviceControlGeneration_ == control_.SessionGeneration();
    if (!live) {
      g_warning("connect: the bound device is stale "
                "(tunnel_state=%s, rpc_pinned=%s, same control session=%s%s%s); dropping it "
                "and starting a new session for this press",
                status ? ctl::ToString(status->tunnel_state) : "unknown",
                status && status->rpc_pinned ? "yes" : "no",
                deviceControlGeneration_ == control_.SessionGeneration() ? "yes" : "no",
                statusError.empty() ? "" : "; ", statusError.c_str());
      TeardownDeviceLocked();
      EmitDrawerEvent(DrawerEvent::DeviceLifecycle);
      PublishConnectReading();
      // AND THEN DO WHAT THE PRESS ASKED FOR. The caller skipped the start
      // path because hasDevice() was still true when it looked (nothing had
      // touched the control socket since the daemon died, so the cached
      // session generation still looked current) — we are the first code to
      // learn otherwise, and returning here would spend the user's press on
      // discovering it. One press, one connection attempt.
      const TunnelStartResult restarted = StartTunnelLocked("connect after a stale device");
      if (restarted != TunnelStartResult::Started) {
        // StartTunnelLocked has already named the reason in lastTunnelError_
        // and in the journal; only fill in when it somehow did not.
        if (lastTunnelError_.empty()) {
          lastTunnelError_ =
              "The URnetwork system service is no longer running the tunnel this app was "
              "attached to, and a new session could not be started.";
        }
        g_warning("connect: could not rebuild the session after dropping the stale device: "
                  "%s", lastTunnelError_.c_str());
        return;
      }
      g_message("connect: rebuilt the session after a stale device; continuing");
    }
  }
  if (connectVc_) {
    g_message("connect: connectBestAvailable via the view controller");
    connectVc_->connectBestAvailable();
  } else if (device_) {
    g_message("connect: connectBestAvailable via the device");
    auto controller = device_->openConnectViewController();
    controller.connectBestAvailable();
    device_->closeConnectViewController(controller);
  } else {
    // THE SILENT NO-OP. With no view controller and no device there is nothing
    // to ask, and this returned without a trace — the caller had already
    // decided the tunnel was up, so the UI showed no error either.
    if (lastTunnelError_.empty()) {
      lastTunnelError_ =
          "There is no connection to work with yet. Press Connect to start one.";
    }
    g_warning("connect: nothing to connect with (no view controller, no device) "
              "— the tunnel is not up");
  }
}

void SdkHost::Connect(const std::optional<urnet::ConnectLocation>& location) {
  if (connectGate_ && connectGate_([this, location] { Connect(location); })) return;
  NoteNewConnectAttempt();
  std::scoped_lock lock(mutex_);
  if (connectVc_) {
    connectVc_->connect(location);
  } else if (device_) {
    auto controller = device_->openConnectViewController();
    controller.connect(location);
    device_->closeConnectViewController(controller);
  }
}

void SdkHost::ConnectFromRow(const std::optional<urnet::ConnectLocation>& location) {
  // Driving: a session is up, its window has not settled on failure, and the
  // SDK's selection is this row. A selection left over from before a
  // Disconnect is no session, and a failed one is driven nowhere, so either
  // row connects.
  const bool driving = health::DrivesSelection(CurrentConnectReading().ToSignals(false)) &&
                       IsTargetSelected(SelectedLocation(), location);
  if (rowConnectTimerId_ != 0) {
    g_source_remove(rowConnectTimerId_);
    rowConnectTimerId_ = 0;
  }
  if (!rowConnects_.Offer(location, driving, g_get_monotonic_time() / 1000)) {
    g_message("connect: row click on the location already connected; nothing to do");
    return;
  }
  ArmRowConnectTimer(rowConnects_.kSettleMillis);
}

void SdkHost::CancelRowConnect(const char* why) {
  if (rowConnects_.Cancel()) g_message("connect: a row click still settling is dropped (%s)", why);
  if (rowConnectTimerId_ != 0) {
    g_source_remove(rowConnectTimerId_);
    rowConnectTimerId_ = 0;
  }
}

void SdkHost::ArmRowConnectTimer(int64_t delayMillis) {
  rowConnectTimerId_ = g_timeout_add(
      static_cast<guint>(std::max<int64_t>(1, delayMillis)),
      [](gpointer data) -> gboolean {
        auto* self = static_cast<SdkHost*>(data);
        self->rowConnectTimerId_ = 0;
        self->OnRowConnectDue();
        return G_SOURCE_REMOVE;
      },
      this);
}

void SdkHost::OnRowConnectDue() {
  const int64_t nowMillis = g_get_monotonic_time() / 1000;
  std::optional<std::optional<urnet::ConnectLocation>> due = rowConnects_.TakeDue(nowMillis);
  if (!due) {
    // a timer that fires a little early waits out the rest
    if (rowConnects_.Pending()) ArmRowConnectTimer(rowConnects_.DueAtMillis() - nowMillis);
    return;
  }
  g_message("connect: a row click settled; connecting");
  RunRowConnect(*due);
}

void SdkHost::RunRowConnect(const std::optional<urnet::ConnectLocation>& location) {
  if (rowConnect_) {
    rowConnect_(location);
  } else if (IsBestAvailableSelected(location)) {
    ConnectBestAvailable();
  } else {
    Connect(location);
  }
}

void SdkHost::Disconnect() {
  CancelRowConnect("disconnect");
  NoteNewConnectAttempt();
  std::scoped_lock lock(mutex_);
  // Bring the daemon's tunnel down. Ending the provider session does not
  // touch the tun device or the 31 capture routes — those are the daemon's,
  // and they are removed only by an explicit stop_tunnel. Without this the
  // user presses Disconnect and every packet keeps being routed into a tunnel
  // with nothing on the other end: the machine loses its internet and the UI
  // says "Disconnected". Best effort, exactly as Logout/Shutdown do it.
  //
  // First, as Windows does it: the routes, DNS and the filter come back before
  // the SDK is asked anything, so a slow device rpc cannot hold the machine's
  // network. The disconnect below then finds the daemon's device gone, and the
  // DeviceRemote applies it locally (selection and connect state).
  control_.StopTunnel();
  if (connectVc_) {
    connectVc_->disconnect();
  } else if (device_) {
    auto controller = device_->openConnectViewController();
    controller.disconnect();
    device_->closeConnectViewController(controller);
  }
  // Say so NOW rather than waiting for the connect-location listener: on a
  // teardown the SDK can simply stop publishing, and a reading nobody
  // refreshes is exactly how the row used to latch on its last word.
  PublishConnectReading();
  // And keep providing. stop_tunnel ended the daemon's DeviceLocal along with
  // the tunnel, which is how a Linux "Always" provider used to stop earning
  // the moment it disconnected. When the provide mode still provides while
  // disconnected, the provider-only device takes over — no tun, no routes, no
  // DNS (ProvideLifecycle.hpp). Posted, so the window settles on Disconnected
  // before the daemon builds it.
  PostToMain([this] { ReconcileProvider("disconnect", /*userInitiated=*/true); });
}

void SdkHost::SetProvideControlMode(const std::string& mode) {
  std::scoped_lock lock(mutex_);
  if (device_) device_->setProvideControlMode(mode);
  // Persist alongside the device write (mac handleProvideControlModeUpdate
  // does both) — DeviceLocal.SetProvideControlMode
  // alone does not persist, and StartTunnel restores the persisted mode.
  if (localState_) localState_->setProvideControlMode(mode);
  // The provider status controller lives only while providing is not never
  // (P008): never closes it, and a mode turned on while the presentation is
  // open (its peers controller is) opens it.
  if (mode == "never") {
    if (providerStatusVc_) {
      CloseProviderStatusLocked();
      EmitDrawerEvent(DrawerEvent::ProviderStatus);
    }
  } else if (peerVc_ && !providerStatusVc_) {
    OpenProviderStatusLocked(mode);
    EmitDrawerEvent(DrawerEvent::ProviderStatus);
  }
  // Disconnected, the provider-only device follows the new mode: started for a
  // mode that provides while disconnected, stopped for one that does not.
  ReconcileProviderLocked("provide mode changed", /*userInitiated=*/true,
                          /*settingsChanged=*/false);
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

void SdkHost::ReconcileProvider(const char* reason, bool userInitiated, bool settingsChanged) {
  std::scoped_lock lock(mutex_);
  ReconcileProviderLocked(reason, userInitiated, settingsChanged);
}

void SdkHost::ReconcileProvider(const char* reason, const ctl::StatusReply& polled) {
  std::scoped_lock lock(mutex_);
  ReconcileProviderLocked(reason, /*userInitiated=*/false, /*settingsChanged=*/false, &polled);
}

// A provider-only device runs on the network space it was built from. Once a
// saved value changes that space, start_provider goes out again and the daemon
// replaces the device (ctl::SameProviderDevice): otherwise a user in China who
// saves DoH servers that work there keeps a provider on the old ones, which may
// never reach the API, until it happens to restart. Posted, so the save's own
// outcome renders first and the daemon's device build never runs inside the
// caller's save.
void SdkHost::ReconcileProviderAfterSpaceChange(const char* reason) {
  PostToMain([this, reason] {
    ReconcileProvider(reason, /*userInitiated=*/true, /*settingsChanged=*/true);
  });
}

bool SdkHost::ProviderRuns() { return hasDevice() || daemonProviderRunning_.load(); }

void SdkHost::ReconcileProviderLocked(const char* reason, bool userInitiated,
                                      bool settingsChanged, const ctl::StatusReply* polled) {
  if (!localState_ || providerReconcileClosed_) return;
  // An owed sign-out first (SignOut.hpp): nothing below starts while it is owed.
  SettleSignOutLocked(reason, userInitiated);
  const std::string clientJwt = localState_->getByClientJwt();
  const std::string instanceId = localState_->getInstanceId();
  // Signed out, nothing provides: a provider the daemon still runs for this
  // user (a sign-out it missed) is stopped like any mode that does not
  // provide. signedOut_ first: the stored jwt outlives a sign-out until its
  // asynchronous local logout lands.
  const bool signedIn = !signedOut_.load() && !clientJwt.empty() && !instanceId.empty();
  const std::string mode = signedIn ? localState_->getProvideControlMode() : std::string("never");
  const provide::ControlMode controlMode = provide::ControlModeFrom(mode);
  // A mode that does not provide asks the daemon nothing once nothing is known
  // to run. The first call after launch still asks once: a provider an earlier
  // run left behind under another mode must be stopped.
  if (!provide::ProviderRuns(controlMode, /*connected=*/false) && providerStateKnown_ &&
      !daemonProviderRunning_.load()) {
    return;
  }
  if (userInitiated) providerBackoff_.NoteSuccess();

  // The caller's reply stands in for a read only with no device bound: one
  // read before a start that bound a device would drop that device below.
  std::string statusError;
  const std::optional<ctl::StatusReply> status =
      polled && !device_ ? std::optional<ctl::StatusReply>(*polled) : control_.Status(&statusError);
  if (!status) return;  // unreachable: Connect reports that, and the next poll asks again
  providerStateKnown_ = true;

  if (device_) {
    // A live tunnel session's device is the provider. One in flux is decided
    // on the next call.
    const bool live = status->tunnel_state == ctl::TunnelState::Up && status->rpc_pinned &&
                      deviceControlGeneration_ == control_.SessionGeneration();
    if (live || status->tunnel_state == ctl::TunnelState::Starting ||
        status->tunnel_state == ctl::TunnelState::Stopping) {
      return;
    }
    // Bound to a session the daemon no longer runs (a Disconnect's stop_tunnel,
    // a protective teardown, a service restart): the same staleness rule
    // StartTunnelLocked applies at 2a, so the pages stop folding on it.
    g_message("provide: the bound device is stale (daemon tunnel_state=%s); dropping it",
              ctl::ToString(status->tunnel_state));
    TeardownDeviceLocked();
    EmitDrawerEvent(DrawerEvent::DeviceLifecycle);
    PublishConnectReading();
  }
  NoteDaemonProviderLocked(*status);

  const provide::DisconnectedStep step =
      provide::DisconnectedProviderStep(mode, ctl::ProviderFactsFrom(*status), settingsChanged);
  if (step == provide::DisconnectedStep::None) return;
  const int64_t nowMillis = g_get_monotonic_time() / 1000;
  if (!providerBackoff_.Allows(nowMillis)) return;

  if (step == provide::DisconnectedStep::Start) {
    // A sign-out the daemon has not done yet holds every start: the device
    // would run beside, or after, what the account that left still runs.
    if (signOut_.Owed()) {
      g_warning("provide: not starting the provider (%s): a sign-out is still owed to the "
                "daemon",
                reason);
      return;
    }
    ctl::StartProviderRequest request;
    request.by_jwt = clientJwt;
    request.instance_id = instanceId;
    request.app_version = kAppVersion;
    try {
      if (networkSpace_) request.network_space_json = networkSpace_->toJson();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[sdk] network space toJson failed: %s\n", e.what());
    }
    request.provide_mode = mode;
    // The mirror BindRemoteDeviceLocked seeds a tunnel session's device from.
    if (auto settings = localState_->getProviderTransportSettings(); settings) {
      request.provider_transport_settings_json = nlohmann::json(*settings).dump();
    }
    ctl::StatusReply after;
    std::string error;
    std::string code;
    if (control_.StartProvider(request, &after, &error, &code)) {
      providerBackoff_.NoteSuccess();
      NoteDaemonProviderLocked(after);
      g_message("provide: providing without a tunnel (%s, mode %s)", reason,
                provide::ToString(controlMode));
      return;
    }
    providerBackoff_.NoteFailure(nowMillis);
    g_warning("provide: the provider could not run without a tunnel (%s, code=%s): %s; "
              "trying again in %llds",
              reason, code.empty() ? "none" : code.c_str(),
              error.empty() ? "no detail" : error.c_str(),
              static_cast<long long>(providerBackoff_.DelayMillis() / 1000));
    return;
  }

  // Apply and Stop are both set_provide: the daemon applies a mode that
  // provides while disconnected to the running device and retires it for one
  // that does not.
  std::string error;
  if (control_.SetProvide(mode, &error)) {
    providerBackoff_.NoteSuccess();
    if (const auto after = control_.Status()) NoteDaemonProviderLocked(*after);
    g_message("provide: %s the provider without a tunnel (%s, mode %s)",
              step == provide::DisconnectedStep::Stop ? "stopped" : "re-moded", reason,
              provide::ToString(controlMode));
    return;
  }
  providerBackoff_.NoteFailure(nowMillis);
  g_warning("provide: set_provide failed (%s): %s; trying again in %llds", reason,
            error.empty() ? "no detail" : error.c_str(),
            static_cast<long long>(providerBackoff_.DelayMillis() / 1000));
}

void SdkHost::FollowDaemonNetworkCountry(const ctl::StatusReply& status) {
  // Another user's session: the daemon names nothing of it, this included.
  const std::string countryCode = status.redacted ? std::string() : status.network_country_code;
  if (countryCode == followedNetworkCountry_) return;
  followedNetworkCountry_ = countryCode;
  urnet::setNetworkCountryCode(countryCode);
  g_message("sdkhost: network country = \"%s\" (urnetworkd)", countryCode.c_str());
}

logupload::DaemonAnswer SdkHost::UploadDaemonLogs(const std::string& feedbackId) {
  // This process's own log files ride with the request, by descriptor: the
  // daemon may not read this user's home, and opens no path a client names.
  // Closed when this returns; the daemon holds its own once the frame is sent.
  const logupload::PassedLogFiles guiLogFiles = OpenGuiLogFiles();
  // The request start_provider sends, so a daemon with no device builds the
  // same one (client jwt, instance id, app version, the active network space).
  ctl::UploadLogsRequest request;
  request.feedback_id = feedbackId;
  {
    std::scoped_lock lock(mutex_);
    if (!localState_) return logupload::DaemonAnswer::NotTaken;
    request.by_jwt = localState_->getByClientJwt();
    request.instance_id = localState_->getInstanceId();
    request.app_version = kAppVersion;
    try {
      if (networkSpace_) request.network_space_json = networkSpace_->toJson();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[sdk] network space toJson failed: %s\n", e.what());
    }
  }
  std::string carrier;
  std::string error;
  std::string code;
  int64_t uploadId = 0;
  if (control_.UploadLogs(request, guiLogFiles, &carrier, &error, &code, &uploadId)) {
    // its outcome comes through status (FollowDaemonLogUpload)
    pendingLogUploadId_ = uploadId;
    g_message("support: urnetworkd took the log upload (%s device), with %zu of this app's log "
              "files",
              carrier.c_str(), guiLogFiles.Size());
    return logupload::DaemonAnswer::Accepted;
  }
  if (code == ctl::kCodeLogUploadBusy) {
    g_message("support: urnetworkd is uploading its logs for an earlier feedback already");
    return logupload::DaemonAnswer::Busy;
  }
  if (error == ctl::kErrorUnknownVerb) {
    g_message("support: the system service predates upload_logs; the logs go up only through "
              "a connected tunnel");
  } else {
    g_warning("support: urnetworkd did not upload its logs (code=%s): %s",
              code.empty() ? "none" : code.c_str(), error.empty() ? "no detail" : error.c_str());
  }
  return logupload::DaemonAnswer::NotTaken;
}

void SdkHost::FollowDaemonLogUpload(const ctl::StatusReply& status) {
  // Another user's session: the daemon names nothing of it, this included.
  if (status.redacted) return;
  const std::optional<logupload::FlightState> outcome = logupload::CompletionFor(
      pendingLogUploadId_, status.log_upload_id,
      logupload::FlightStateFromString(status.log_upload_state));
  if (!outcome) return;
  pendingLogUploadId_ = 0;
  // Logged, never shown: the feedback itself was accepted.
  if (*outcome == logupload::FlightState::Uploaded) {
    g_message("support: urnetworkd uploaded its logs (%s device)",
              status.log_upload_carrier.c_str());
  } else {
    g_warning("support: urnetworkd's log upload ended %s (%s device)",
              status.log_upload_state.c_str(), status.log_upload_carrier.c_str());
  }
}

void SdkHost::FollowDaemonExtenderReset(const ctl::StatusReply& status) {
  if (!owedExtenderReset_.Owed()) return;
  // A reset in flight answers for itself, and its answer replaces what is owed;
  // until it is done this waits for the next poll.
  bool expected = false;
  if (!extenderResetBusy_.compare_exchange_strong(expected, true)) return;
  std::optional<ctl::ResetExtendersRequest> request = owedExtenderReset_.TakeIfSettled(status);
  if (!request) {
    extenderResetBusy_.store(false);
    return;
  }
  std::scoped_lock lock(extenderResetWorkerMutex_);
  // As in ResetExtenders: the previous worker may still be joinable.
  if (extenderResetWorker_.joinable()) extenderResetWorker_.join();
  extenderResetWorker_ = std::thread([this, request = std::move(*request)] {
    // Sent once and as taken, which asks for no dialog: a refusal (another
    // user's live session, or a check that would need a dialog) drops it, the
    // answer never makes it owed again, and the daemon's next import applies
    // the reset.
    try {
      bool daemonReset = false;
      std::string error;
      std::string code;
      if (control_.ResetExtenders(request, &daemonReset, &error, &code)) {
        g_message("extender: reset %s sent again after the bring-up; urnetworkd %s",
                  request.extender_reset_id.c_str(),
                  daemonReset ? "reset the space it holds"
                              : "holds no space to reset, or reset it already");
      } else {
        g_message("extender: reset %s sent again after the bring-up was refused without a "
                  "dialog (code=%s): %s; dropped, and the next tunnel or provider start "
                  "applies it",
                  request.extender_reset_id.c_str(), code.empty() ? "none" : code.c_str(),
                  error.empty() ? "no detail" : error.c_str());
      }
    } catch (const std::exception& e) {
      g_warning("extender: sending the reset again threw: %s", e.what());
    } catch (...) {
      g_warning("extender: sending the reset again threw");
    }
    extenderResetBusy_.store(false);
  });
}

void SdkHost::NoteDaemonProviderLocked(const ctl::StatusReply& status) {
  // A redacted status names nothing of ours.
  const bool running = status.provider_running && !status.redacted;
  const int64_t tier = running ? status.provider_mode : 0;
  const bool networkKey = running && status.provider_network_key;
  const int64_t clientCount = running ? status.provider_client_count : -1;
  bool changed = daemonProviderRunning_.exchange(running) != running;
  changed = daemonProviderMode_.exchange(tier) != tier || changed;
  changed = daemonProviderNetworkKey_.exchange(networkKey) != networkKey || changed;
  changed = daemonProviderClientCount_.exchange(clientCount) != clientCount || changed;
  if (changed) PublishStats();
}

// DeviceRemote teardown without touching the stored auth or the daemon:
// Logout adds the auth clear + stop_tunnel; the guest upgrade only swaps the
// device. Caller holds mutex_.
// ---- the device-rpc bind watchdog ------------------------------------------
// Everything else about the mTLS pairing fails LOUDLY: a malformed PEM throws
// out of setRpcServer on whichever side sees it, a missing pin is refused by
// ControlClient before a DeviceRemote is built, and a hostport the daemon did
// not honour shows up as a mismatched echoed port. The one case with no
// synchronous signal at all is a well-formed but MISMATCHED pair — two
// different generate calls. Both ends bind, both dial, the handshake fails at
// connect time, nothing throws, and every screen simply stays empty forever.
// This is the detector for that, and only that.

void SdkHost::ArmRpcBindWatchdogLocked() {
  if (rpcBindWatchId_ != 0) {
    g_source_remove(rpcBindWatchId_);
    rpcBindWatchId_ = 0;
  }
  rpcBindWatchGeneration_ = rpcSessionGeneration_.load();
  rpcBindWatchId_ = g_timeout_add_seconds(
      kRpcBindDeadlineSeconds,
      [](gpointer data) -> gboolean {
        static_cast<SdkHost*>(data)->OnRpcBindDeadline();
        return G_SOURCE_REMOVE;
      },
      this);
}

void SdkHost::CancelRpcBindWatchdogLocked() {
  if (rpcBindWatchId_ != 0) {
    g_source_remove(rpcBindWatchId_);
    rpcBindWatchId_ = 0;
  }
  // Anything already queued against this session — the timeout that has
  // already fired and is waiting on mutex_, the remote-change marshal — is
  // stale from here on and must not act on the NEXT session.
  ++rpcSessionGeneration_;
}

void SdkHost::OnRpcBindDeadline() {
  std::string syncError;
  {
    std::scoped_lock lock(mutex_);
    // A newer session owns rpcBindWatchId_ now; leave its watchdog alone.
    if (rpcBindWatchGeneration_ != rpcSessionGeneration_.load()) return;
    rpcBindWatchId_ = 0;  // this source is removing itself
    if (!device_) return;
    if (ReadGuarded<bool>(
            "device getRemoteConnected", [&] { return device_->getRemoteConnected(); },
            false)) {
      return;  // it came up; nothing to do
    }
    syncError = ReadGuarded<std::string>(
        "device getSyncError", [&] { return device_->getSyncError(); }, std::string());
  }

  // Never render as "empty": name the failure, tear the half-session down, and
  // stop the daemon-side tunnel rather than leave one running that this app
  // cannot drive.
  std::string message =
      "the local connection to the URnetwork system service never came up, so this "
      "session was stopped";
  if (!syncError.empty()) message += ": " + syncError;
  g_warning("sdkhost: %s", message.c_str());
  {
    std::scoped_lock lock(mutex_);
    lastTunnelError_ = message;
    TeardownDeviceLocked();
  }
  // Blocking, on the main loop, bounded by the control client's receive
  // timeout — the same trade StartTunnel's failure path already makes.
  control_.StopTunnel();
  // The material is bound to nothing now; remembering it could only produce a
  // reattach that mismatches again. TeardownDeviceLocked above has already
  // dropped the unwritten record, so this is only about a record from an
  // EARLIER session that is equally not what is running.
  ForgetRpcSession();
  PublishConnectReading();
  EmitDrawerEvent(DrawerEvent::DeviceLifecycle);
}

void SdkHost::TeardownDeviceLocked() {
  CancelRpcBindWatchdogLocked();
  rpcHostPort_.clear();
  // The session this armed record describes is over before it was ever
  // committed. Dropping it here is what stops a torn-down pairing from being
  // written by a late remote_connected edge — CancelRpcBindWatchdogLocked bumps
  // the generation, so such an edge returns early, but the record must not
  // outlive the device either way.
  unsavedSession_.reset();
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

void SdkHost::Shutdown() {
  std::scoped_lock lock(mutex_);
  // quit brings the daemon's tunnel down like Logout does, but leaves the
  // stored auth untouched: next launch signs straight back in. see the
  // header comment — quit-as-logout destroyed guest accounts. Before the
  // device's teardown, as Disconnect does: its view controller closes are rpcs
  // to the daemon's device, and the machine's network does not wait on them.
  control_.StopTunnel();
  TeardownDeviceLocked();
  // The daemon's DeviceLocal (and its pinned listener with it) is gone, so the
  // remembered session can no longer be attached to by anything.
  ForgetRpcSession();
  // stop_tunnel retires the provider-only device as well: quitting stops
  // providing, exactly as it stops the tunnel, and nothing restarts it.
  providerReconcileClosed_ = true;
  daemonProviderRunning_.store(false);
  daemonProviderMode_.store(0);
  daemonProviderNetworkKey_.store(false);
  daemonProviderClientCount_.store(-1);
  DropDaemonProviderStatsLocked();
}

// See the contract in the header.
void SdkHost::Logout() {
  // A sign-in method being added belongs to the account that is leaving: its
  // return, if it still comes, must not add it to the next account signed in.
  // Answered "superseded by ..." (bridge::IsSuperseded), so its sheet settles
  // quietly, and outside mutex_, which it takes.
  CancelPendingAddSignIn("superseded by signing out");
  // and so is a row click still settling
  CancelRowConnect("sign-out");
  std::scoped_lock lock(mutex_);
  // Signed out from here: a posted reconcile, the health poll or a Connect that
  // runs after this starts nothing for the account that is leaving.
  signedOut_.store(true);
  // ...and a sign-out the sdk reports for it from here signs nothing out again
  // (AuthLogout.hpp)
  authLogouts_.SignedOut();
  pendingWalletAuth_.reset();
  // so are sign-in flows left unfinished before this session (an sso identity
  // with no network, an instant account never confirmed): they belong to no
  // account, and the next sign-in starts clean (Windows' Logout drops them too)
  pendingSsoAuth_ = false;
  pendingSsoType_.clear();
  pendingSsoJwt_.clear();
  pendingInstantJwt_.reset();
  // The local credentials first, because nothing can hold them up: the app is
  // signed out on disk even if it is ended while the daemon is asked below.
  if (asyncLocalState_) asyncLocalState_->logout([](bool) {});
  // and the credential the api attaches to its calls, which the next sign-in's
  // own calls would otherwise carry until it installs its own
  if (api_) api_->setByJwt("");
  if (events_) events_->NewSession();  // the next sign-in is a new session
  TeardownDeviceLocked();
  // The daemon as Quit stops it (Shutdown), owed until it has (SignOut.hpp). A
  // person asked, so at once rather than at the backoff's pace. It used to be a
  // best-effort stop_tunnel: "an unreachable daemon has nothing running for
  // us" holds when the daemon is not running, not when its socket, its hello
  // or polkit fails while it still runs the account's tunnel or provider.
  signOutBackoff_.NoteSuccess();
  const signout::Delivery delivery = signOut_.Begin(SignOutDaemonLocked());
  if (delivery == signout::Delivery::Delivered) {
    g_message("sdkhost: sign-out: the daemon runs nothing of the account that signed out");
  } else {
    signOutBackoff_.NoteFailure(g_get_monotonic_time() / 1000);
    g_warning("sdkhost: sign-out: %s; the sign-out stays owed to the daemon, which is told "
              "as soon as it can be, and nothing starts until it has been",
              signout::ToString(delivery));
  }
  ForgetRpcSession();
  // stop_tunnel retired the provider-only device too; nothing provides for a
  // signed-out app.
  daemonProviderRunning_.store(false);
  daemonProviderMode_.store(0);
  daemonProviderNetworkKey_.store(false);
  daemonProviderClientCount_.store(-1);
  DropDaemonProviderStatsLocked();
  providerStateKnown_ = false;
  providerBackoff_.NoteSuccess();
  if (onAuth_) onAuth_(false);
  EmitDrawerEvent(DrawerEvent::DeviceLifecycle);  // drawer falls back to empty states
}

signout::Daemon SdkHost::SignOutDaemonLocked() {
  // requires mutex_, across the calls the delivery makes
  signout::Daemon daemon;
  daemon.reach = [this] {
    std::string error;
    const bool reached = control_.EnsureSession(&error) == DaemonSessionState::Ok;
    if (!reached) {
      g_warning("sdkhost: sign-out: the daemon session is not usable: %s",
                error.empty() ? "no detail" : error.c_str());
    }
    return reached;
  };
  daemon.otherUsersSession = [this]() -> std::optional<bool> {
    std::string error;
    const std::optional<ctl::StatusReply> status = control_.Status(&error);
    if (!status) {
      g_warning("sdkhost: sign-out: the daemon did not answer `status`: %s",
                error.empty() ? "no detail" : error.c_str());
      return std::nullopt;
    }
    return status->redacted;
  };
  daemon.send = [this](signout::Request request) {
    switch (request) {
      case signout::Request::StopTunnel: {
        std::string error;
        if (control_.StopTunnel(&error)) return true;
        g_warning("sdkhost: sign-out: stop_tunnel failed: %s",
                  error.empty() ? "no detail" : error.c_str());
        return false;
      }
      case signout::Request::Logout: {
        // The account's space, which the daemon's devices ran in. The space
        // outlives the sign-out, so a delivery after a relaunch names it too.
        std::string spaceJson;
        try {
          if (networkSpace_) spaceJson = networkSpace_->toJson();
        } catch (const std::exception& e) {
          g_warning("sdkhost: sign-out: the network space could not be read: %s", e.what());
          return false;
        }
        std::string error;
        std::string code;
        switch (control_.Logout(spaceJson, &error, &code)) {
          case ControlClient::LogoutOutcome::Done:
            return true;
          case ControlClient::LogoutOutcome::Unsupported:
            g_warning("sdkhost: sign-out: urnetworkd predates logout, so it keeps the device "
                      "identity it made; update it to clear it");
            return true;
          case ControlClient::LogoutOutcome::Failed:
            // Another user's session is live (a daemon in group mode does not
            // redact the status): what the daemon keeps is theirs now, and
            // there is nothing of this user's to clear, as when the status
            // says so first.
            if (code == ctl::kCodeAuthNotTunnelOwner) {
              g_message("sdkhost: sign-out: another user's session is live; the daemon cleared "
                        "nothing of it");
              return true;
            }
            break;
        }
        g_warning("sdkhost: sign-out: logout failed: %s%s%s",
                  error.empty() ? "no detail" : error.c_str(), code.empty() ? "" : " ",
                  code.c_str());
        return false;
      }
    }
    return false;
  };
  return daemon;
}

void SdkHost::SettleSignOutLocked(const char* reason, bool userInitiated) {
  if (!signOut_.Owed()) return;
  const int64_t nowMillis = g_get_monotonic_time() / 1000;
  if (userInitiated) signOutBackoff_.NoteSuccess();
  if (!signOutBackoff_.Allows(nowMillis)) return;
  const signout::Delivery delivery = signOut_.Settle(SignOutDaemonLocked());
  if (delivery == signout::Delivery::Delivered) {
    signOutBackoff_.NoteSuccess();
    g_message("sdkhost: the owed sign-out is delivered (%s)", reason);
    return;
  }
  signOutBackoff_.NoteFailure(nowMillis);
  g_warning("sdkhost: the sign-out is still owed (%s): %s; nothing starts until it is "
            "delivered, next try in %llds",
            reason, signout::ToString(delivery),
            static_cast<long long>(signOutBackoff_.DelayMillis() / 1000));
}

signout::Marker SdkHost::SignOutMarker() {
  signout::Marker marker;
  marker.read = [] { return g_file_test(SignOutOwedPath().c_str(), G_FILE_TEST_EXISTS) != 0; };
  marker.write = [](bool owed) {
    const std::string path = SignOutOwedPath();
    if (!owed) {
      // One left behind is delivered again at the next launch, which stops
      // what this user runs then; said, as that is the one way it can surprise.
      if (g_remove(path.c_str()) != 0 && errno != ENOENT) {
        g_warning("sdkhost: sign-out: the owed marker could not be removed: %s",
                  g_strerror(errno));
      }
      return;
    }
    std::ofstream file(path, std::ios::trunc);
    file << "a sign-out the URnetwork system service has not done yet\n";
    if (!file) g_warning("sdkhost: sign-out: the owed marker could not be written");
  };
  return marker;
}

}  // namespace urnw
