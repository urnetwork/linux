// SPDX-License-Identifier: MPL-2.0
#include "SdkHost.hpp"

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
#include <utility>

#include <glib/gstdio.h>

#include "AppPrefs.hpp"
#include "NetworkSpaceConfig.hpp"
// The Secret Service backend for the remembered rpc session. GUI-ONLY: this is
// the one translation unit that links libsecret, and urnetworkd (which builds
// in a container that has no libsecret at all) never sees this header.
#include "SecretServiceRpcSessionStore.hpp"
#include "Ui.hpp"  // PostToMain — the only UI dependency here, and only to marshal

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
}  // namespace

SdkHost::~SdkHost() {
  // Leg 3 of the kill switch runs here and holds `this`. Stop it before any
  // member dies; it never takes mutex_ while the joiner holds it, so the join
  // cannot deadlock.
  StopKillSwitchWorker();
  // The worker holds `this` and calls back into ReadReliability, so it must be
  // finished before any member dies. It never takes reliabilityWorkerMutex_,
  // so joining under that lock cannot deadlock. The join is NOT bounded — a
  // read blocked on mutex_ (held across a slow StartTunnel) or on a hung
  // daemon rpc can hold quit for seconds. That is the same trade the Developer
  // page's bridge makes, and it is the right one against a use-after-free.
  std::scoped_lock lock(reliabilityWorkerMutex_);
  if (reliabilityWorker_.joinable()) reliabilityWorker_.join();
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

void SdkHost::CreateInstantAccount(std::function<void(InstantAccount)> done) {
  // NO user_auth, password, auth_jwt or wallet_auth: that combination is what
  // makes the server mint a seedphrase-secured network and return the phrase.
  urnet::NetworkCreateArgs args;
  args.terms = true;  // the form's button is gated on the terms consent
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
  // the same resolution the space build uses, so "Use default network" means
  // the network this process was started against — never silently production
  if (const char* env = std::getenv("URNETWORK_NETWORK_HOST"); env && *env) {
    out.defaultHostName = env;
  } else {
    out.defaultHostName = kUrHostName;
  }
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
      const bool official = (hostName == std::string(kUrHostName));
      const bool explicitUrls = !apiUrl.empty() || !connectUrl.empty();

      urnet::NetworkSpaceKey key;
      key.host_name = hostName;
      key.env_name = std::string(kUrEnvName);

      // The same value set BuildUrNetworkSpace writes, with the
      // host-dependent parts varied (iOS DeviceManager.applyNetworkSpace
      // parity). `bundled` is true only for the official host with no
      // overrides: a bundled space carries pinned endpoints a custom
      // deployment does not have.
      urnet::NetworkSpaceValues values;
      values.bundled = official && !explicitUrls;
      values.net_expose_server_ips = true;
      values.net_expose_server_host_names = true;
      values.link_host_name = official ? std::string("ur.io") : hostName;
      values.migration_host_name = official ? std::string("bringyour.com") : std::string();
      values.wallet = "circle";
      values.sso_google = false;
      values.api_url = apiUrl;
      values.platform_url = connectUrl;

      networkSpace_ = spaceManager_->updateNetworkSpaceValues(key, values);
      spaceManager_->setActiveNetworkSpace(*networkSpace_);

      // everything derived from the space re-derives: the Api talks to the
      // new host, the LocalState holds the new host's jwt
      api_ = networkSpace_->getApi();
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
    // walletAuthDone_ is set on the UI thread and consumed on wallet/SDK
    // callback threads: take it under the lock, invoke it outside
    std::function<void(AuthResult)> done;
    {
      std::scoped_lock lock(mutex_);
      done = std::move(walletAuthDone_);
      walletAuthDone_ = nullptr;
    }
    if (done) done({false, false, err});
  };
}

void SdkHost::SignInWithSolana(WalletConnect::Provider provider,
                               std::function<void(AuthResult)> done) {
  {
    std::scoped_lock lock(mutex_);
    walletAuthDone_ = std::move(done);
  }
  wallet_.Connect(provider);  // opens the browser; the rest continues on the deep-link callback
}

void SdkHost::SignInWithBittensor(std::function<void(AuthResult)> done) {
  {
    std::scoped_lock lock(mutex_);
    walletAuthDone_ = std::move(done);
  }
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
    api_->authNetworkClient(args, [this, done](std::optional<urnet::AuthNetworkClientResult> result,
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

TunnelStartResult SdkHost::StartTunnel() {
  std::scoped_lock lock(mutex_);
  return StartTunnelLocked();
}

TunnelStartResult SdkHost::StartTunnelLocked() {
  lastTunnelError_.clear();
  // A new start makes any previous "the daemon stopped it" verdict obsolete.
  // This is the ONLY thing that clears the latch.
  daemonTunnelGone_.store(false);
  // EVERY outcome of this function is logged. It used to be silent on all of
  // them, so a Connect that failed here left NOTHING to read: not in the app,
  // not in the journal, not in the daemon (which is never reached on most of
  // these paths). "Pressing Connect does nothing" was unanswerable as a result.
  g_message("connect: start_tunnel requested");
  const std::string clientJwt = localState_->getByClientJwt();
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

    // Restore the persisted routeLocal (the kill switch, inverted) the same
    // way: DeviceLocal starts at its default (true) and does not read local
    // state (the macOS DeviceManager applies exactly this at device
    // creation). LocalState defaults to true — kill switch off.
    device_->setRouteLocal(localState_->getRouteLocal());

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
    return r;
  }
  const health::SdkStatus parsed = health::ParseSdkStatus(r.rawStatus);
  if (parsed != health::SdkStatus::Unknown && parsed != health::SdkStatus::Disconnected) {
    lastKnownSdk_.store(static_cast<int>(parsed));
    r.sdk = parsed;
    return r;
  }
  // Nothing usable came back this time (a controller that has just been
  // reopened, or none at all): keep the last thing this session actually said.
  r.sdk = static_cast<health::SdkStatus>(lastKnownSdk_.load());
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
    providerLocationsVc_.reset();
    return;
  }
  if (providerLocationsVc_) {
    device_->closeProviderLocationsViewController(*providerLocationsVc_);
  }
  providerLocationsVc_.reset();
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
      const TunnelStartResult restarted = StartTunnelLocked();
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
  // AND BRING THE DAEMON'S TUNNEL DOWN. Ending the provider session does not
  // touch the tun device or the 31 capture routes — those are the daemon's,
  // and they are removed only by an explicit stop_tunnel. Without this the
  // user presses Disconnect and every packet keeps being routed into a tunnel
  // with nothing on the other end: the machine loses its internet and the UI
  // says "Disconnected". Best effort, exactly as Logout/Shutdown do it.
  control_.StopTunnel();
  // Say so NOW rather than waiting for the connect-location listener: on a
  // teardown the SDK can simply stop publishing, and a reading nobody
  // refreshes is exactly how the row used to latch on its last word.
  PublishConnectReading();
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
  TeardownDeviceLocked();
  // quit brings the daemon's tunnel down like Logout does, but leaves the
  // stored auth untouched: next launch signs straight back in. see the
  // header comment — quit-as-logout destroyed guest accounts.
  control_.StopTunnel();
  // The daemon's DeviceLocal (and its pinned listener with it) is gone, so the
  // remembered session can no longer be attached to by anything.
  ForgetRpcSession();
}

void SdkHost::Logout() {
  std::scoped_lock lock(mutex_);
  TeardownDeviceLocked();
  // the session is over: bring the daemon's tunnel down too (best effort — an
  // unreachable daemon has nothing running for us anyway)
  control_.StopTunnel();
  ForgetRpcSession();
  pendingWalletAuth_.reset();
  if (asyncLocalState_) asyncLocalState_->logout([](bool) {});
  if (onAuth_) onAuth_(false);
  EmitDrawerEvent(DrawerEvent::DeviceLifecycle);  // drawer falls back to empty states
}

}  // namespace urnw
