// SPDX-License-Identifier: MPL-2.0
#include "daemon/ControlServer.hpp"

#include "daemon/DaemonLog.hpp"

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <gio/gio.h>
#include <glib-unix.h>

// GUnixFDList is forward-declared in gio-2.0's giotypes.h and
// g_dbus_connection_call_with_unix_fd_list is declared in gio-2.0's
// gdbusconnection.h, so the CALL always compiles. Building an fd list needs
// gio/gunixfdlist.h, which on glib < 2.78 (Ubuntu 22.04 ships 2.72 — measured,
// and 22.04 is exactly where the daemon wants to be built for its glibc floor)
// lives under the separate gio-unix-2.0 pkg-config module that this target does
// not declare.
//
// So the pidfd subject is compiled in only where the header is reachable, and
// the classic {pid, start-time, uid} subject — which every polkit accepts and
// which is the form actually observed working — is the universal path. Adding
// gio-unix-2.0 to the urnetworkd dependencies in app/meson.build turns the
// hardening on everywhere; nothing else has to change.
#if defined(__has_include)
#if __has_include(<gio/gunixfdlist.h>)
#include <gio/gunixfdlist.h>
#define UR_HAVE_GIO_UNIX_FDLIST 1
#endif
#endif

// SO_PEERPIDFD (kernel >= 6.5) is the strongest identity a unix socket can
// produce: the kernel pins the peer's `struct pid` at connect(2), exactly as it
// does the ucred, so — unlike pidfd_open(pid_from_SO_PEERCRED), which reopens
// the very reuse window it is meant to close — there is NO window at all, and
// it is namespace-independent. Defined here for build hosts whose headers
// predate it; the getsockopt simply fails with ENOPROTOOPT at runtime on an
// older kernel and we fall back to {pid, start-time, uid}.
#if defined(__linux__) && !defined(SO_PEERPIDFD)
#define SO_PEERPIDFD 77
#endif

namespace urnw {
namespace {

constexpr size_t kMaxFrameBytes = 1 << 20;  // request line beyond 1 MiB: protocol error
constexpr int kSendTimeoutMillis = 5000;

// ---- DoS caps -------------------------------------------------------------
// Under polkit the socket is 0666, so mode 0660 is no longer implicitly
// admitting only a handful of peers. These caps are what replaces that, and
// they are load-bearing rather than optional hardening: without them any local
// uid can exhaust the fd table of the ROOT daemon that holds the tun.
constexpr size_t kMaxConnections = 128;
constexpr int kMaxConnectionsPerUid = 16;
// A connection that has not completed `hello` in this long is dropped: the
// pre-authorization surface must not be holdable for free.
constexpr unsigned kHelloDeadlineMillis = 10000;
// Per-connection frame cap, over a rolling one-second window. The GUI's
// busiest moment is a 4 Hz status poll alongside a 0.2 Hz log tail, so 64 is
// ~13x headroom over a sustained 32/s and still bounds a flooder.
constexpr int kMaxFramesPerWindow = 64;

// polkit's own bounds. Non-interactive checks are answered by polkitd without
// human involvement, so 5 s is generous. An interactive one may sit in front of
// a person, so the D-Bus call itself is unbounded and OUR guard is the bound —
// 120 s, comfortably inside the client's own start_tunnel receive timeout, so a
// refused start surfaces as a refusal rather than as a client-side timeout.
constexpr int kPolkitCallTimeoutMillis = 5000;
constexpr unsigned kPolkitInteractiveGuardMillis = 120000;
// The same guard for a NON-interactive check, which also covers the window
// where the check is queued because the system bus has not been acquired yet.
// Comfortably above the 5 s D-Bus call timeout, which normally fires first.
constexpr unsigned kPolkitQueuedGuardMillis = 20000;
// polkit gained pidfd subjects in 124 (NEWS: "PIDFDs are used if available to
// track processes"). Below that the subject parser may reject the unknown key,
// so the classic {pid, start-time, uid} triple is sent instead.
constexpr int kPolkitPidfdMinBackend = 124;

int64_t NowMillis() { return static_cast<int64_t>(g_get_monotonic_time() / 1000); }

// --- peer credential resolution ---------------------------------------------

// SO_PEERCRED at accept time, before any frame is read. Returns false when the
// kernel cannot answer — which fails closed (the connection is dropped).
//
// A peer inside a user+PID namespace (every Flatpak sandbox) CANNOT LIE here:
// the kernel translates the ucred into the RECEIVER's namespaces, so a client
// that believes it is uid 0 pid 1 is reported to this daemon as its real host
// uid and its real host pid. That is also why the pid below is usable as a
// polkit subject at all.
bool PeerCredentials(int fd, int64_t* uid, int64_t* gid, int64_t* pid) {
#if defined(__linux__)
  struct ucred cred {};
  socklen_t len = sizeof(cred);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
  *uid = cred.uid;
  *gid = cred.gid;
  *pid = cred.pid;
  return true;
#else
  // Non-Linux (dev builds only): getpeereid is the closest analogue.
  uid_t peerUid = 0;
  gid_t peerGid = 0;
  if (::getpeereid(fd, &peerUid, &peerGid) != 0) return false;
  *uid = peerUid;
  *gid = peerGid;
  *pid = -1;
  return true;
#endif
}

// SO_PEERPIDFD, or -1 where the kernel or the socket does not offer it.
int PeerPidFd(int fd) {
#if defined(__linux__)
  int pidfd = -1;
  socklen_t len = sizeof(pidfd);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &len) != 0) return -1;
  return pidfd;
#else
  (void)fd;
  return -1;
#endif
}

// /proc/<pid>/stat field 22 (starttime). Read through a dirfd opened right
// after accept() so it cannot be re-pointed at a recycled pid mid-read.
//
// Field 2 is the comm, which may itself contain spaces AND parentheses, so the
// only correct parse is "everything after the LAST ')'" — a naive whitespace
// split is a real bug against a process named "evil ) 1 2 3".
bool ReadProcStartTime(int64_t pid, uint64_t* startTime) {
  if (pid <= 0) return false;
  char dirPath[64];
  std::snprintf(dirPath, sizeof(dirPath), "/proc/%lld", static_cast<long long>(pid));
  const int dirFd = ::open(dirPath, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dirFd < 0) return false;
  const int statFd = ::openat(dirFd, "stat", O_RDONLY | O_CLOEXEC);
  ::close(dirFd);
  if (statFd < 0) return false;
  char buf[4096];
  ssize_t n = 0;
  for (;;) {
    n = ::read(statFd, buf, sizeof(buf) - 1);
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  ::close(statFd);
  if (n <= 0) return false;
  buf[n] = '\0';
  const char* close = std::strrchr(buf, ')');
  if (close == nullptr) return false;
  const char* p = close + 1;
  // fields from 3 (state) onward; starttime is field 22, i.e. the 20th here
  for (int field = 3; field <= 21; ++field) {
    while (*p == ' ') ++p;
    while (*p != '\0' && *p != ' ') ++p;
  }
  while (*p == ' ') ++p;
  if (*p < '0' || *p > '9') return false;
  *startTime = std::strtoull(p, nullptr, 10);
  return true;
}

// The `urnetwork` group's gid, or -1 when the group does not exist (in which
// case only root passes — AuthorizeControlPeer fails closed).
int64_t ControlGroupGid() {
  if (struct group* g = ::getgrnam(ctl::kControlGroupName)) {
    return static_cast<int64_t>(g->gr_gid);
  }
  return -1;
}

// Every gid the peer uid belongs to (primary + supplementary), via getpwuid +
// getgrouplist. An unresolvable uid yields just the primary gid.
//
// NEVER the gids the peer reports about itself: measured from inside a Flatpak
// sandbox, the peer's own supplementary gids read as 65534 (unmapped) while the
// host still sees the real 10/954/959/1000. Only this side's resolution is
// meaningful. These are NSS calls, which can block on LDAP/SSSD, so under
// polkit they are not made at all — that path needs only the uid.
std::vector<int64_t> PeerGroupIds(int64_t uid, int64_t gid) {
  std::vector<int64_t> out{gid};
  struct passwd* pw = ::getpwuid(static_cast<uid_t>(uid));
  if (pw == nullptr || pw->pw_name == nullptr) return out;
  int count = 32;
#if defined(__APPLE__)
  std::vector<int> groups(static_cast<size_t>(count));
  if (::getgrouplist(pw->pw_name, static_cast<int>(gid), groups.data(), &count) < 0) {
    groups.resize(static_cast<size_t>(count));
    ::getgrouplist(pw->pw_name, static_cast<int>(gid), groups.data(), &count);
  }
#else
  std::vector<gid_t> groups(static_cast<size_t>(count));
  if (::getgrouplist(pw->pw_name, static_cast<gid_t>(gid), groups.data(), &count) < 0) {
    groups.resize(static_cast<size_t>(count));
    ::getgrouplist(pw->pw_name, static_cast<gid_t>(gid), groups.data(), &count);
  }
#endif
  for (int i = 0; i < count && i < static_cast<int>(groups.size()); ++i) {
    out.push_back(static_cast<int64_t>(groups[static_cast<size_t>(i)]));
  }
  return out;
}

void SetCloexecNonblock(int fd) {
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Logging a refusal is ITSELF an attack surface once the socket is
// world-connectable: a peer reconnecting in a loop would otherwise evict every
// useful line from the 2000-entry log ring — the ring the GUI shows a user
// whose machine is blocked. So the pure-abuse breadcrumbs (never the
// authorization outcomes, which are the audit record) are emitted at most once
// per 5 s each, and say how many they stand for.
struct LogThrottle {
  int64_t lastMs = 0;
  int suppressed = 0;
  // -1 => suppress this line; otherwise the count of lines suppressed since
  // the last emitted one.
  int Admit() {
    const int64_t now = NowMillis();
    if (lastMs != 0 && now - lastMs < 5000) {
      suppressed += 1;
      return -1;
    }
    lastMs = now;
    const int n = suppressed;
    suppressed = 0;
    return n;
  }
};

// A g_timeout/user-data pairing that cannot dangle: the id is looked up, the
// pointer never dereferenced blind.
struct ConnTimerCtx {
  ControlServer* server = nullptr;
  uint64_t connId = 0;
};
void DestroyConnTimerCtx(gpointer data) { delete static_cast<ConnTimerCtx*>(data); }

}  // namespace

// ============================================================================
// PolkitAuthorizer — org.freedesktop.PolicyKit1.Authority over raw GDBus
// ============================================================================
//
// The daemon already links gio (app/meson.build: urnetworkd deps are
// [gio, json, threads, sdk_lib], "raw GDBus C API"), so this costs NO new
// dependency. The GUI gains nothing and needs no new Flatpak permission — it
// never speaks to polkit. The agent dialog is drawn by the session's existing
// authentication agent, outside the sandbox.
//
//   bus     G_BUS_TYPE_SYSTEM
//   dest    org.freedesktop.PolicyKit1
//   path    /org/freedesktop/PolicyKit1/Authority
//   iface   org.freedesktop.PolicyKit1.Authority
//   method  CheckAuthorization
//           in  (sa{sv}) subject, s action_id, a{ss} details, u flags,
//               s cancellation_id
//           out (bba{ss}) -> (is_authorized, is_challenge, details)
//           flags: 0 = non-interactive, 1 = AllowUserInteraction
//
// TWO RULES THAT ARE SECURITY, NOT STYLE:
//
//  1. `details` is ALWAYS EMPTY. polkitd substitutes $(key) from it into the
//     text of the dialog the user is shown, and it accepts details only from
//     trusted callers — which we are, being uid 0. Passing anything derived
//     from a client would therefore be dialog-text injection against the user.
//
//  2. The subject is built ONLY from SO_PEERCRED plus the kernel's own pidfd.
//     No field of any request may reach it. Being uid 0 is also what lets us
//     ask about a subject that is not ourselves ("Only trusted callers ... for
//     subjects belonging to other identities") — which means a bug that let a
//     request influence the subject would be a silent, total local privilege
//     escalation, not a misfeature.
class PolkitAuthorizer {
 public:
  enum class Decision {
    Allowed,          // (is_authorized=true)
    Denied,           // (false, is_challenge=false) — a hard no
    ChallengeNeeded,  // (false, true) — auth required, none could be obtained
    Dismissed,        // the user closed the agent dialog
    TimedOut,         // nobody answered inside our guard
    Failed,           // bus/polkitd unreachable, or an unusable reply
  };
  using Callback = std::function<void(Decision, std::string)>;

  explicit PolkitAuthorizer(std::string policyPath) : policyPath_(std::move(policyPath)) {
    box_ = std::make_shared<PolkitAuthorizer*>(this);
  }

  ~PolkitAuthorizer() {
    // Any in-flight GDBus callback that outlives us finds a null box and
    // returns without touching freed memory.
    *box_ = nullptr;
    CancelAll();
    if (busCancellable_ != nullptr) {
      g_cancellable_cancel(busCancellable_);
      g_object_unref(busCancellable_);
      busCancellable_ = nullptr;
    }
    if (bus_ != nullptr) {
      g_object_unref(bus_);
      bus_ = nullptr;
    }
  }

  PolkitAuthorizer(const PolkitAuthorizer&) = delete;
  PolkitAuthorizer& operator=(const PolkitAuthorizer&) = delete;

  // THE fallback discriminator: an install fact, latched here, never a runtime
  // probe. Keying on "polkitd answered" would make `kill polkitd` a policy
  // downgrade attack.
  bool Expected() const { return expected_; }

  // Non-blocking. Acquires the system bus and reads the Authority's
  // BackendVersion asynchronously; checks that arrive first are queued and
  // issued when the bus lands. Daemon startup never waits on this.
  void Start() {
    expected_ = (::access(policyPath_.c_str(), F_OK) == 0);
    if (!expected_) return;
    busState_ = BusState::Pending;
    busCancellable_ = g_cancellable_new();
    g_bus_get(G_BUS_TYPE_SYSTEM, busCancellable_, &PolkitAuthorizer::OnBusReady,
              new CallCtx{box_, 0});
  }

  // Returns a non-zero check id while the check is still in flight, or 0 when
  // `cb` already ran (a synchronous failure). `cb` runs at most once, on this
  // same GMainContext, and never after Cancel().
  uint64_t CheckAsync(const PeerIdentity& peer, const char* actionId, bool allowInteraction,
                      Callback cb) {
    auto check = std::make_unique<Check>();
    const uint64_t id = nextCheckId_++;
    check->id = id;
    check->cb = std::move(cb);
    check->actionId = actionId;
    check->interactive = allowInteraction;
    check->uid = peer.uid;
    check->pid = peer.pid;
    check->startTime = peer.start_time;
    check->startTimeKnown = peer.start_time_known;
    // Our OWN dup, so the check is independent of the connection's lifetime.
    if (peer.pidfd >= 0) check->pidfd = ::fcntl(peer.pidfd, F_DUPFD_CLOEXEC, 0);
    check->cancellable = g_cancellable_new();
    // Unique and non-empty, always: it is what CancelCheckAuthorization needs
    // in order to abandon an interactive check (e.g. the client disconnected).
    check->cancellationId = "urnetworkd-" + std::to_string(::getpid()) + "-" +
                            std::to_string(static_cast<unsigned long long>(id));

    // Neither a pidfd nor a start-time means polkitd has nothing to pin the
    // process with, and a bare pid is exactly the CVE-2013-4288/CVE-2019-6133
    // reuse race. Refuse rather than ask a question whose answer cannot be
    // trusted.
    if (check->pidfd < 0 && !check->startTimeKnown) {
      Callback cb2 = std::move(check->cb);
      ReleaseCheck(check.get());
      if (cb2) cb2(Decision::Failed, "the peer process could not be identified");
      return 0;
    }

    // THE GUARD IS INSTALLED HERE, not in Issue(), so it also bounds a check
    // that is still QUEUED because the system bus has not been acquired yet.
    // Without that, a bus acquisition that never completes would leave the
    // connection's read pump stopped forever with no reply and no timeout.
    check->guardId = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        allowInteraction ? kPolkitInteractiveGuardMillis : kPolkitQueuedGuardMillis,
        &PolkitAuthorizer::OnGuardTimeout, new CallCtx{box_, id},
        &PolkitAuthorizer::DestroyCallCtx);

    Check* raw = check.get();
    checks_[id] = std::move(check);
    if (busState_ == BusState::Ready) {
      Issue(raw);
    } else if (busState_ != BusState::Pending) {
      Complete(id, Decision::Failed, "the system authorization service is not reachable");
    }
    return checks_.count(id) != 0 ? id : 0;
  }

  // Guarantees `cb` will NOT run.
  void Cancel(uint64_t checkId) {
    const auto it = checks_.find(checkId);
    if (it == checks_.end()) return;
    std::unique_ptr<Check> check = std::move(it->second);
    checks_.erase(it);
    if (check->issued && check->interactive) SendCancelCheck(check->cancellationId);
    ReleaseCheck(check.get());
  }

  void CancelAll() {
    while (!checks_.empty()) Cancel(checks_.begin()->first);
  }

 private:
  enum class BusState { Idle, Pending, Ready, Failed };

  struct Check {
    uint64_t id = 0;
    Callback cb;
    GCancellable* cancellable = nullptr;
    guint guardId = 0;
    std::string cancellationId;
    std::string actionId;
    bool interactive = false;
    bool issued = false;
    bool usedPidfd = false;
    bool retriedWithoutPidfd = false;
    // Subject material, copied so the check outlives its Connection.
    int64_t uid = -1;
    int64_t pid = -1;
    uint64_t startTime = 0;
    bool startTimeKnown = false;
    int pidfd = -1;  // OWNED dup
  };

  // What every async callback carries: a weak-ish box that reads null once the
  // authorizer is gone, plus the check id (never a Check*).
  struct CallCtx {
    std::shared_ptr<PolkitAuthorizer*> box;
    uint64_t checkId = 0;
  };

  static void OnBusReady(GObject*, GAsyncResult* res, gpointer data) {
    std::unique_ptr<CallCtx> ctx(static_cast<CallCtx*>(data));
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_finish(res, &error);
    PolkitAuthorizer* self = ctx->box ? *ctx->box : nullptr;
    if (self == nullptr) {
      if (bus != nullptr) g_object_unref(bus);
      g_clear_error(&error);
      return;
    }
    if (bus == nullptr) {
      DaemonLogf("[control] polkit: no system bus (%s); privileged verbs will be refused\n",
                 error != nullptr ? error->message : "unknown error");
      g_clear_error(&error);
      self->busState_ = BusState::Failed;
      self->FlushQueued();
      return;
    }
    g_clear_error(&error);
    self->bus_ = bus;
    self->busState_ = BusState::Ready;
    self->ProbeBackendVersion();
    self->FlushQueued();
  }

  void ProbeBackendVersion() {
    g_dbus_connection_call(
        bus_, "org.freedesktop.PolicyKit1", "/org/freedesktop/PolicyKit1/Authority",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.PolicyKit1.Authority", "BackendVersion"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, kPolkitCallTimeoutMillis,
        busCancellable_, &PolkitAuthorizer::OnBackendVersion, new CallCtx{box_, 0});
  }

  static void OnBackendVersion(GObject* source, GAsyncResult* res, gpointer data) {
    std::unique_ptr<CallCtx> ctx(static_cast<CallCtx*>(data));
    GError* error = nullptr;
    GVariant* reply =
        g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    PolkitAuthorizer* self = ctx->box ? *ctx->box : nullptr;
    if (self == nullptr) {
      if (reply != nullptr) g_variant_unref(reply);
      g_clear_error(&error);
      return;
    }
    if (reply == nullptr) {
      // Non-fatal by design: the classic {pid, start-time, uid} subject is the
      // universally supported form, so an unknown backend version simply means
      // "do not send the pidfd key".
      DaemonLogf("[control] polkit: BackendVersion unavailable (%s); using the pid+start-time "
                 "subject\n",
                 error != nullptr ? error->message : "unknown error");
      g_clear_error(&error);
      return;
    }
    g_clear_error(&error);
    GVariant* boxed = nullptr;
    g_variant_get(reply, "(v)", &boxed);
    if (boxed != nullptr && g_variant_is_of_type(boxed, G_VARIANT_TYPE_STRING)) {
      const char* text = g_variant_get_string(boxed, nullptr);
      const int major = text != nullptr ? std::atoi(text) : 0;
#if defined(UR_HAVE_GIO_UNIX_FDLIST)
      self->usePidfd_ = major >= kPolkitPidfdMinBackend;
#else
      (void)major;
      self->usePidfd_ = false;
#endif
      DaemonLogf("[control] polkit backend %s (%s subject)\n", text != nullptr ? text : "?",
                 self->usePidfd_ ? "pidfd" : "pid+start-time");
    }
    if (boxed != nullptr) g_variant_unref(boxed);
    g_variant_unref(reply);
  }

  void FlushQueued() {
    std::vector<uint64_t> ids;
    ids.reserve(checks_.size());
    for (const auto& entry : checks_) {
      if (!entry.second->issued) ids.push_back(entry.first);
    }
    for (const uint64_t id : ids) {
      const auto it = checks_.find(id);
      if (it == checks_.end()) continue;
      if (busState_ == BusState::Ready) {
        Issue(it->second.get());
      } else {
        Complete(id, Decision::Failed, "the system authorization service is not reachable");
      }
    }
  }

  void Issue(Check* check) {
    check->issued = true;
    const bool withPidfd = usePidfd_ && check->pidfd >= 0 && !check->retriedWithoutPidfd;

    GVariantBuilder subjectBuilder;
    g_variant_builder_init(&subjectBuilder, G_VARIANT_TYPE("a{sv}"));
    // pid and start-time stay in the dict even alongside the pidfd: they are
    // what an older polkitd keys on, and they are what our own audit line and
    // polkitd's log report.
    g_variant_builder_add(&subjectBuilder, "{sv}", "pid",
                          g_variant_new_uint32(static_cast<guint32>(check->pid)));
    if (check->startTimeKnown) {
      g_variant_builder_add(&subjectBuilder, "{sv}", "start-time",
                            g_variant_new_uint64(check->startTime));
    }
    // MANDATORY beside a pidfd (libpolkit-gobject carries the validation
    // string "'pidfd' specified withtout 'uid'"), and it is also the value
    // polkitd cross-checks against the process — the slowfork defence.
    g_variant_builder_add(&subjectBuilder, "{sv}", "uid",
                          g_variant_new_int32(static_cast<gint32>(check->uid)));

    GUnixFDList* fdList = nullptr;
#if defined(UR_HAVE_GIO_UNIX_FDLIST)
    if (withPidfd) {
      fdList = g_unix_fd_list_new();
      GError* fdError = nullptr;
      // append DUPs the fd, so our own pidfd stays ours to close
      const gint handle = g_unix_fd_list_append(fdList, check->pidfd, &fdError);
      if (handle < 0) {
        g_clear_error(&fdError);
        g_object_unref(fdList);
        fdList = nullptr;
      } else {
        // A D-Bus handle ('h') indexes into the GUnixFDList attached to the
        // message; polkitd reads it back with g_variant_get_handle.
        g_variant_builder_add(&subjectBuilder, "{sv}", "pidfd", g_variant_new_handle(handle));
      }
      g_clear_error(&fdError);
    }
#else
    (void)withPidfd;
#endif
    check->usedPidfd = (fdList != nullptr);

    GVariant* subject =
        g_variant_new("(s@a{sv})", "unix-process", g_variant_builder_end(&subjectBuilder));
    // ALWAYS EMPTY — see rule 1 in the class comment.
    GVariant* details = g_variant_new_array(G_VARIANT_TYPE("{ss}"), nullptr, 0);
    const guint32 flags = check->interactive ? 1u : 0u;
    GVariant* params = g_variant_new("(@(sa{sv})s@a{ss}us)", subject, check->actionId.c_str(),
                                     details, flags, check->cancellationId.c_str());

    g_dbus_connection_call_with_unix_fd_list(
        bus_, "org.freedesktop.PolicyKit1", "/org/freedesktop/PolicyKit1/Authority",
        "org.freedesktop.PolicyKit1.Authority", "CheckAuthorization", params,
        G_VARIANT_TYPE("((bba{ss}))"), G_DBUS_CALL_FLAGS_NONE,
        check->interactive ? G_MAXINT : kPolkitCallTimeoutMillis, fdList, check->cancellable,
        &PolkitAuthorizer::OnCheckDone, new CallCtx{box_, check->id});
    if (fdList != nullptr) g_object_unref(fdList);
  }

  static void DestroyCallCtx(gpointer data) { delete static_cast<CallCtx*>(data); }

  static gboolean OnGuardTimeout(gpointer data) {
    auto* ctx = static_cast<CallCtx*>(data);
    PolkitAuthorizer* self = ctx->box ? *ctx->box : nullptr;
    if (self == nullptr) return G_SOURCE_REMOVE;
    const auto it = self->checks_.find(ctx->checkId);
    if (it == self->checks_.end()) return G_SOURCE_REMOVE;
    // clear first: we are INSIDE this source, and ReleaseCheck must not try to
    // remove a source that is already finishing
    it->second->guardId = 0;
    const bool interactive = it->second->interactive;
    self->SendCancelCheck(it->second->cancellationId);
    // Only an INTERACTIVE check that ran out of time is "nobody answered the
    // prompt". A non-interactive one hitting this bound means polkitd or the
    // bus never answered US, which is a service failure and must not be
    // reported to the user as an unanswered dialog they never saw.
    if (interactive) {
      self->Complete(ctx->checkId, Decision::TimedOut,
                     "the permission request was not answered");
    } else {
      self->Complete(ctx->checkId, Decision::Failed,
                     "the authorization service did not answer");
    }
    return G_SOURCE_REMOVE;
  }

  void SendCancelCheck(const std::string& cancellationId) {
    if (bus_ == nullptr || cancellationId.empty()) return;
    g_dbus_connection_call(bus_, "org.freedesktop.PolicyKit1",
                           "/org/freedesktop/PolicyKit1/Authority",
                           "org.freedesktop.PolicyKit1.Authority", "CancelCheckAuthorization",
                           g_variant_new("(s)", cancellationId.c_str()), nullptr,
                           G_DBUS_CALL_FLAGS_NONE, kPolkitCallTimeoutMillis, nullptr, nullptr,
                           nullptr);
  }

  static void OnCheckDone(GObject* source, GAsyncResult* res, gpointer data) {
    std::unique_ptr<CallCtx> ctx(static_cast<CallCtx*>(data));
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_with_unix_fd_list_finish(
        G_DBUS_CONNECTION(source), nullptr, res, &error);
    PolkitAuthorizer* self = ctx->box ? *ctx->box : nullptr;
    if (self == nullptr) {
      if (reply != nullptr) g_variant_unref(reply);
      g_clear_error(&error);
      return;
    }
    const auto it = self->checks_.find(ctx->checkId);
    if (it == self->checks_.end()) {
      // cancelled, timed out, or the owning connection went away
      if (reply != nullptr) g_variant_unref(reply);
      g_clear_error(&error);
      return;
    }
    Check* check = it->second.get();

    if (reply == nullptr) {
      const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      const std::string message = error != nullptr && error->message != nullptr
                                      ? error->message
                                      : "the authorization check failed";
      // THE ONE INFERENCE IN THIS FILE, made self-correcting rather than
      // asserted: the pidfd wire encoding (a D-Bus handle indexing an attached
      // GUnixFDList) is read off libpolkit-gobject's and polkitd's imports, not
      // off a captured message. If polkitd rejects the subject we disable the
      // pidfd key for the rest of this process and retry ONCE with the classic
      // {pid, start-time, uid} triple, which is the tested form. A parse
      // failure is therefore loud in the log and harmless in behaviour.
      const bool subjectRejected =
          check->usedPidfd && !check->retriedWithoutPidfd && !cancelled &&
          (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS) ||
           message.find("pidfd") != std::string::npos ||
           message.find("subject") != std::string::npos);
      g_clear_error(&error);
      if (subjectRejected) {
        DaemonLogf("[control] polkit rejected the pidfd subject (%s); falling back to "
                   "pid+start-time for the rest of this run\n",
                   message.c_str());
        self->usePidfd_ = false;
        check->retriedWithoutPidfd = true;
        self->Issue(check);
        return;
      }
      if (cancelled) return;  // Cancel() already removed the check
      self->Complete(ctx->checkId, Decision::Failed, message);
      return;
    }
    g_clear_error(&error);

    GVariant* result = nullptr;
    g_variant_get(reply, "(@(bba{ss}))", &result);
    gboolean authorized = FALSE;
    gboolean challenge = FALSE;
    GVariant* resultDetails = nullptr;
    if (result != nullptr) {
      g_variant_get(result, "(bb@a{ss})", &authorized, &challenge, &resultDetails);
    }
    bool dismissed = false;
    std::string polkitResult;
    if (resultDetails != nullptr) {
      const gchar* value = nullptr;
      if (g_variant_lookup(resultDetails, "polkit.dismissed", "&s", &value) && value != nullptr &&
          *value != '\0') {
        dismissed = true;
      }
      value = nullptr;
      if (g_variant_lookup(resultDetails, "polkit.result", "&s", &value) && value != nullptr) {
        polkitResult = value;
      }
      g_variant_unref(resultDetails);
    }
    if (result != nullptr) g_variant_unref(result);
    g_variant_unref(reply);

    if (authorized) {
      self->Complete(ctx->checkId, Decision::Allowed, polkitResult);
      return;
    }
    if (dismissed) {
      self->Complete(ctx->checkId, Decision::Dismissed, "the permission request was closed");
      return;
    }
    if (challenge) {
      self->Complete(ctx->checkId, Decision::ChallengeNeeded,
                     polkitResult.empty() ? std::string("authentication is required")
                                          : polkitResult);
      return;
    }
    self->Complete(ctx->checkId, Decision::Denied,
                   polkitResult.empty() ? std::string("not authorized") : polkitResult);
  }

  void Complete(uint64_t checkId, Decision decision, std::string detail) {
    const auto it = checks_.find(checkId);
    if (it == checks_.end()) return;
    std::unique_ptr<Check> check = std::move(it->second);
    checks_.erase(it);
    Callback cb = std::move(check->cb);
    ReleaseCheck(check.get());
    // Invoked LAST, with the check already out of the map, so a callback that
    // starts another check (or destroys the connection) is safe. Wrapped
    // because this is the boundary where our C++ re-enters from a GDBus C
    // callback: an exception crossing it is std::terminate in the root daemon.
    if (!cb) return;
    try {
      cb(decision, std::move(detail));
    } catch (const std::exception& e) {
      DaemonLogf("[control] polkit callback threw: %s\n", e.what());
    } catch (...) {
      DaemonLogf("[control] polkit callback threw\n");
    }
  }

  static void ReleaseCheck(Check* check) {
    if (check->guardId != 0) {
      g_source_remove(check->guardId);  // its GDestroyNotify frees the CallCtx
      check->guardId = 0;
    }
    if (check->cancellable != nullptr) {
      g_cancellable_cancel(check->cancellable);
      g_object_unref(check->cancellable);
      check->cancellable = nullptr;
    }
    if (check->pidfd >= 0) {
      ::close(check->pidfd);
      check->pidfd = -1;
    }
  }

  std::string policyPath_;
  bool expected_ = false;
  BusState busState_ = BusState::Idle;
  GDBusConnection* bus_ = nullptr;
  GCancellable* busCancellable_ = nullptr;
  bool usePidfd_ = false;
  std::shared_ptr<PolkitAuthorizer*> box_;
  uint64_t nextCheckId_ = 1;
  std::map<uint64_t, std::unique_ptr<Check>> checks_;
};

namespace {

// THE audit line. Every authorization outcome that is not a plain allow names
// the uid, the pid, the action and the reason — the brief's requirement, and
// the only record on the machine of who was refused what.
void LogAuthOutcome(const char* verdict, int64_t uid, int64_t pid, const char* actionId,
                    const char* code, const std::string& reason) {
  DaemonLogf("[control] auth %s uid=%lld pid=%lld action=%s code=%s: %s\n", verdict,
             static_cast<long long>(uid), static_cast<long long>(pid),
             actionId != nullptr ? actionId : "(none)", code != nullptr ? code : "-",
             reason.empty() ? "(no detail)" : reason.c_str());
}

}  // namespace

// ============================================================================
// ControlServer
// ============================================================================

ControlServer::ControlServer(TunnelHost& tunnel, GeoClueWriter& geoWriter)
    : tunnel_(tunnel), geoWriter_(geoWriter) {}

ControlServer::~ControlServer() { Stop(); }

std::string ControlServer::SocketPath() {
  if (const char* env = std::getenv("URNETWORK_CONTROL_SOCKET"); env && *env) return env;
  return ctl::kControlSocketPath;
}

std::string ControlServer::PolicyPath() {
  if (const char* env = std::getenv("URNETWORK_POLKIT_POLICY"); env && *env) return env;
  // BOTH PREFIXES, and this is not defensive padding — it is the difference
  // between polkit working and not working on every ostree/bootc host.
  //
  // On an immutable machine (Bazzite, Silverblue, Kinoite, SteamOS, MicroOS)
  // /usr is a read-only image mount, so the installer maps every payload path
  // under /usr/local — the action file included. A daemon that stats only
  // /usr/share therefore finds nothing, latches `group` mode, and hands the
  // user back the "log out and back in" requirement that polkit exists to
  // remove — on precisely the class of machine this was written for. polkit
  // itself reads both directories since 124 (multi-directory action lookup),
  // so the file IS live there; only our probe was looking in one place.
  //
  // Order matters: /usr/local wins, because that is where an immutable host's
  // install actually is, and a stale /usr copy from an older package would
  // otherwise shadow it.
  for (const char* candidate : {ctl::kPolkitPolicyPathLocal, ctl::kPolkitPolicyPath}) {
    if (::access(candidate, F_OK) == 0) return candidate;
  }
  return ctl::kPolkitPolicyPath;  // absent: the group fallback, named honestly
}

// Is polkit ITSELF installed on this machine? An INSTALL FACT, deliberately the
// same shape as the action-file test and mirroring detect_polkit() in
// packaging/tarball/install.sh — never "did polkitd answer", which would make
// `kill polkitd` a policy downgrade any unprivileged process could trigger.
//
// This exists because our action file's presence alone is not a safe
// discriminator for every channel. The tarball installer withholds the file on
// a machine with no polkit, but a .deb cannot: dpkg ships a package's files
// unconditionally, and `polkitd | policykit-1` is a Recommends (it must be, so
// the daemon still installs on a headless box) which apt can be told to skip.
// The result was a host that latched "polkit is the authority", had nothing to
// ask, and FAILED SHUT — every verb denied, with no way back from the UI.
// Requiring both facts turns that into the group fallback it should have been.
bool ControlServer::PolkitRuntimePresent() {
  static const char* const kProbes[] = {
      "/usr/bin/pkcheck",
      "/usr/bin/pkaction",
      "/usr/lib/polkit-1/polkitd",
      "/usr/libexec/polkit-1/polkitd",
      "/usr/lib64/polkit-1/polkitd",
      "/usr/lib/x86_64-linux-gnu/polkit-1/polkitd",
      "/usr/lib/aarch64-linux-gnu/polkit-1/polkitd",
      "/usr/local/lib/polkit-1/polkitd",
      "/usr/local/libexec/polkit-1/polkitd",
      // polkit has shipped its own action file since 0.105.
      "/usr/share/polkit-1/actions/org.freedesktop.policykit.policy",
      "/usr/local/share/polkit-1/actions/org.freedesktop.policykit.policy",
  };
  for (const char* probe : kProbes) {
    if (::access(probe, F_OK) == 0) return true;
  }
  return false;
}

bool ControlServer::PolkitPolicyPresent() {
  return ::access(PolicyPath().c_str(), F_OK) == 0 && PolkitRuntimePresent();
}

const char* ControlServer::AuthModeName() const {
  return authMode_ == AuthMode::Polkit ? ctl::kAuthModePolkit : ctl::kAuthModeGroup;
}

bool ControlServer::Start() {
  const std::string path = SocketPath();
  const bool defaultPath = (path == ctl::kControlSocketPath);
  const int64_t groupGid = ControlGroupGid();

  // Latch the authority BEFORE the socket exists, because the socket's mode is
  // a consequence of it.
  polkit_ = std::make_unique<PolkitAuthorizer>(PolicyPath());
  polkit_->Start();
  authMode_ = polkit_->Expected() ? AuthMode::Polkit : AuthMode::Group;
  const bool polkitMode = (authMode_ == AuthMode::Polkit);

  if (polkitMode) {
    DaemonLogf("[control] authorization: polkit (%s). No group membership and no re-login are "
               "required — the person at this machine's screen is authorized in their current "
               "session.\n",
               PolicyPath().c_str());
  } else {
    DaemonLogf("[control] authorization: the '%s' group (no polkit action file at %s). "
               "Members of that group, plus root, may control the tunnel; group membership "
               "only applies to NEW login sessions.\n",
               ctl::kControlGroupName, PolicyPath().c_str());
  }

  if (defaultPath) {
    // polkit: dir 0755 root:root — the socket is world-connectable and the
    // refusal is a protocol frame, not an EACCES nobody can read.
    // group:   dir 0750 root:urnetwork, exactly as before.
    const mode_t dirMode = polkitMode ? 0755 : 0750;
    ::mkdir(ctl::kControlSocketDir, dirMode);
    ::chmod(ctl::kControlSocketDir, dirMode);
    if (polkitMode) {
      if (::chown(ctl::kControlSocketDir, 0, 0) != 0) {
        std::fprintf(stderr, "[control] chown %s: %s\n", ctl::kControlSocketDir,
                     std::strerror(errno));
      }
    } else if (groupGid >= 0) {
      if (::chown(ctl::kControlSocketDir, 0, static_cast<gid_t>(groupGid)) != 0) {
        std::fprintf(stderr, "[control] chown %s: %s\n", ctl::kControlSocketDir,
                     std::strerror(errno));
      }
    } else {
      std::fprintf(stderr,
                   "[control] group '%s' does not exist and there is no polkit policy: only "
                   "root will be authorized\n",
                   ctl::kControlGroupName);
    }
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    std::fprintf(stderr, "[control] socket path too long: %s\n", path.c_str());
    return false;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listenFd_ < 0) {
    std::perror("[control] socket");
    return false;
  }
  SetCloexecNonblock(listenFd_);
  ::unlink(path.c_str());  // stale socket from an unclean shutdown
  if (::bind(listenFd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::fprintf(stderr, "[control] bind %s: %s\n", path.c_str(), std::strerror(errno));
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  boundPath_ = path;
  // polkit: 0666 root:root. DAC is deliberately no longer the boundary —
  // SO_PEERCRED plus polkit is, and a world-connectable socket is what lets a
  // refusal arrive as a frame with a code and a sentence instead of a bare
  // EACCES at connect(2) that carries neither a reason nor a remedy.
  // group:   0660 root:urnetwork, unchanged, EACCES and all.
  ::chmod(path.c_str(), polkitMode ? 0666 : 0660);
  if (polkitMode) {
    if (::chown(path.c_str(), 0, 0) != 0 && defaultPath) {
      std::fprintf(stderr, "[control] chown %s: %s\n", path.c_str(), std::strerror(errno));
    }
  } else if (groupGid >= 0) {
    if (::chown(path.c_str(), 0, static_cast<gid_t>(groupGid)) != 0 && defaultPath) {
      std::fprintf(stderr, "[control] chown %s: %s\n", path.c_str(), std::strerror(errno));
    }
  }
  if (::listen(listenFd_, 8) != 0) {
    std::perror("[control] listen");
    Stop();
    return false;
  }
  listenWatchId_ = g_unix_fd_add(listenFd_, G_IO_IN, &ControlServer::OnAcceptReady, this);
  std::fprintf(stderr, "[control] listening on %s (auth %s, mode %s)\n", path.c_str(),
               AuthModeName(), polkitMode ? "0666" : "0660");
  return true;
}

void ControlServer::Stop() {
  while (!connections_.empty()) {
    CloseConnection(connections_.begin()->second.get());
  }
  if (polkit_ != nullptr) {
    polkit_->CancelAll();
    polkit_.reset();
  }
  if (listenWatchId_ != 0) {
    g_source_remove(listenWatchId_);
    listenWatchId_ = 0;
  }
  if (listenFd_ >= 0) {
    ::close(listenFd_);
    listenFd_ = -1;
  }
  if (!boundPath_.empty()) {
    ::unlink(boundPath_.c_str());
    boundPath_.clear();
  }
}

gboolean ControlServer::OnAcceptReady(gint, GIOCondition, gpointer data) {
  static_cast<ControlServer*>(data)->AcceptOne();
  return G_SOURCE_CONTINUE;
}

void ControlServer::AcceptOne() {
  const int fd = ::accept(listenFd_, nullptr, nullptr);
  if (fd < 0) return;
  SetCloexecNonblock(fd);

  // SO_PEERCRED FIRST — before any byte of any frame is read. Its job is now
  // IDENTITY, not policy, but the ordering is unchanged and a kernel that
  // cannot answer still fails closed.
  PeerIdentity peer;
  if (!PeerCredentials(fd, &peer.uid, &peer.gid, &peer.pid)) {
    std::fprintf(stderr, "[control] rejecting peer: no credentials\n");
    ::close(fd);
    return;
  }
  peer.pidfd = PeerPidFd(fd);
  peer.start_time_known = ReadProcStartTime(peer.pid, &peer.start_time);

  const bool polkitMode = (authMode_ == AuthMode::Polkit);
  if (!polkitMode) {
    // FALLBACK PATH, byte-for-byte today's behaviour: resolve the peer's group
    // list and drop the connection outright when it is not root and not a
    // member. The socket is 0660 root:urnetwork here, so in practice a
    // non-member never reaches this at all — which is exactly why the GUI's
    // DaemonUnreachableReason::PermissionDenied still means what it always
    // meant against a group-gated daemon.
    peer.gids = PeerGroupIds(peer.uid, peer.gid);
    const bool ok =
        ctl::AuthorizeControlPeer(peer.uid, peer.gid, peer.gids, ControlGroupGid());
    if (!ok) {
      static LogThrottle groupRefusalLog;
      if (const int suppressed = groupRefusalLog.Admit(); suppressed >= 0) {
        DaemonLogf("[control] auth refused uid=%lld pid=%lld action=(connect) code=%s: not root "
                   "and not a member of the '%s' group (%d similar suppressed)\n",
                   static_cast<long long>(peer.uid), static_cast<long long>(peer.pid),
                   ctl::kCodeAuthUnavailable, ctl::kControlGroupName, suppressed);
      }
      if (peer.pidfd >= 0) ::close(peer.pidfd);
      ::close(fd);
      return;
    }
  }

  // DoS caps. Under polkit the socket is world-connectable, so these are what
  // replaces the admission control mode 0660 used to provide implicitly.
  if (connections_.size() >= kMaxConnections) {
    static LogThrottle globalCapLog;
    if (const int suppressed = globalCapLog.Admit(); suppressed >= 0) {
      DaemonLogf("[control] refusing uid=%lld: %zu connections already open (global cap; %d "
                 "similar suppressed)\n",
                 static_cast<long long>(peer.uid), connections_.size(), suppressed);
    }
    if (peer.pidfd >= 0) ::close(peer.pidfd);
    ::close(fd);
    return;
  }
  if (connectionsPerUid_[peer.uid] >= kMaxConnectionsPerUid) {
    static LogThrottle perUidCapLog;
    if (const int suppressed = perUidCapLog.Admit(); suppressed >= 0) {
      DaemonLogf("[control] refusing uid=%lld: per-uid connection cap (%d) reached (%d similar "
                 "suppressed)\n",
                 static_cast<long long>(peer.uid), kMaxConnectionsPerUid, suppressed);
    }
    if (peer.pidfd >= 0) ::close(peer.pidfd);
    ::close(fd);
    return;
  }

  auto conn = std::make_unique<Connection>();
  const uint64_t connId = nextConnId_++;
  conn->fd = fd;
  conn->id = connId;
  conn->peer = std::move(peer);
  conn->groupAuthorized = !polkitMode;  // established above on that path
  conn->watchId = g_unix_fd_add(fd, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
                                &ControlServer::OnConnectionReadable, this);
  conn->helloDeadlineId =
      g_timeout_add_full(G_PRIORITY_DEFAULT, kHelloDeadlineMillis, &ControlServer::OnHelloDeadline,
                         new ConnTimerCtx{this, connId}, &DestroyConnTimerCtx);
  connectionsPerUid_[conn->peer.uid] += 1;
  connIdByFd_[fd] = connId;
  connections_[connId] = std::move(conn);
}

gboolean ControlServer::OnHelloDeadline(gpointer data) {
  auto* ctx = static_cast<ConnTimerCtx*>(data);
  ControlServer* self = ctx->server;
  Connection* conn = self->FindConnection(ctx->connId);
  if (conn == nullptr) return G_SOURCE_REMOVE;
  conn->helloDeadlineId = 0;  // we are inside this source
  if (conn->helloOk) return G_SOURCE_REMOVE;
  static LogThrottle helloDeadlineLog;
  if (const int suppressed = helloDeadlineLog.Admit(); suppressed >= 0) {
    DaemonLogf("[control] dropping uid=%lld pid=%lld: no hello within %ums (%d similar "
               "suppressed)\n",
               static_cast<long long>(conn->peer.uid), static_cast<long long>(conn->peer.pid),
               kHelloDeadlineMillis, suppressed);
  }
  self->CloseConnection(conn);
  return G_SOURCE_REMOVE;
}

ControlServer::Connection* ControlServer::FindConnection(uint64_t connId) {
  const auto it = connections_.find(connId);
  return it == connections_.end() ? nullptr : it->second.get();
}

gboolean ControlServer::OnConnectionReadable(gint fd, GIOCondition, gpointer data) {
  auto* self = static_cast<ControlServer*>(data);
  const auto fdIt = self->connIdByFd_.find(fd);
  if (fdIt == self->connIdByFd_.end()) return G_SOURCE_REMOVE;
  const uint64_t connId = fdIt->second;
  Connection* conn = self->FindConnection(connId);
  if (conn == nullptr) return G_SOURCE_REMOVE;
  if (!self->ReadIntoBuffer(conn)) {
    // the source is being removed by returning FALSE; make CloseConnection
    // not double-remove it
    conn->watchId = 0;
    self->CloseConnection(conn);
    return G_SOURCE_REMOVE;
  }
  if (!self->PumpConnection(connId)) {
    conn = self->FindConnection(connId);
    if (conn != nullptr) {
      conn->watchId = 0;
      self->CloseConnection(conn);
    }
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

bool ControlServer::ReadIntoBuffer(Connection* conn) {
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(conn->fd, buf, sizeof(buf), 0);
    if (n > 0) {
      conn->inBuf.append(buf, static_cast<size_t>(n));
      if (conn->inBuf.size() > kMaxFrameBytes) return false;
      continue;
    }
    if (n == 0) return false;  // peer closed
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    return false;
  }
  return true;
}

bool ControlServer::AllowFrame(Connection* conn) {
  const int64_t now = NowMillis();
  if (conn->rateWindowMs == 0 || now - conn->rateWindowMs >= 1000) {
    conn->rateWindowMs = now;
    conn->rateFrames = 0;
  }
  conn->rateFrames += 1;
  return conn->rateFrames <= kMaxFramesPerWindow;
}

// The reply rendezvous. Dispatch may answer inline (the common case) or later
// (an interactive polkit check). This is how one call site handles both without
// the lambda ever touching a stack object that has gone away.
namespace {
struct PendingReply {
  bool syncPhase = true;
  bool haveSync = false;
  nlohmann::json reply;
};
}  // namespace

bool ControlServer::PumpConnection(uint64_t connId) {
  for (;;) {
    Connection* conn = FindConnection(connId);
    if (conn == nullptr) return false;
    // A check is in flight: STOP consuming lines. Without this a client can
    // queue hundreds of interactive checks and carpet the user's screen with
    // password dialogs.
    if (conn->authPending) return true;
    const size_t pos = conn->inBuf.find('\n');
    if (pos == std::string::npos) return true;
    const std::string line = conn->inBuf.substr(0, pos);
    conn->inBuf.erase(0, pos + 1);

    if (!AllowFrame(conn)) {
      static LogThrottle rateLog;
      if (const int suppressed = rateLog.Admit(); suppressed >= 0) {
        DaemonLogf("[control] dropping uid=%lld pid=%lld: more than %d frames in one second "
                   "(%d similar suppressed)\n",
                   static_cast<long long>(conn->peer.uid),
                   static_cast<long long>(conn->peer.pid), kMaxFramesPerWindow, suppressed);
      }
      return false;
    }

    const auto frame = ctl::DecodeFrame(line);
    if (!frame) return false;  // not JSON: drop the connection

    auto state = std::make_shared<PendingReply>();
    conn->authPending = true;  // provisional; cleared below if we answer inline
    // The whole dispatch is inside the try, not just the verb bodies: this runs
    // on a glib callback, so an exception escaping here is std::terminate in
    // the ROOT daemon — the exact failure the hardened hello was added for, and
    // the reachable population is now every local uid rather than one group.
    try {
      Dispatch(connId, *frame, [this, connId, state](nlohmann::json reply) {
        if (state->syncPhase) {
          state->haveSync = true;
          state->reply = std::move(reply);
          return;
        }
        DeliverDeferredReply(connId, std::move(reply));
      });
    } catch (const std::exception& e) {
      state->haveSync = true;
      state->reply = ctl::MakeErrorReply(ctl::FrameId(*frame), e.what());
    } catch (...) {
      state->haveSync = true;
      state->reply = ctl::MakeErrorReply(ctl::FrameId(*frame), "internal error");
    }
    state->syncPhase = false;

    conn = FindConnection(connId);
    if (conn == nullptr) return false;
    if (!state->haveSync) return true;  // deferred; authPending stays set
    conn->authPending = false;
    if (!SendFrame(conn, state->reply)) return false;
  }
}

void ControlServer::DeliverDeferredReply(uint64_t connId, nlohmann::json reply) {
  Connection* conn = FindConnection(connId);
  // The peer disconnected while its check was pending — the NORMAL case when a
  // user quits the app with the password dialog up. Drop the reply.
  if (conn == nullptr) return;
  conn->authPending = false;
  bool alive = true;
  try {
    if (!SendFrame(conn, reply)) alive = false;
    if (alive && !PumpConnection(connId)) alive = false;
  } catch (const std::exception& e) {
    DaemonLogf("[control] deferred reply failed: %s\n", e.what());
    alive = false;
  } catch (...) {
    alive = false;
  }
  if (!alive) {
    Connection* again = FindConnection(connId);
    if (again != nullptr) CloseConnection(again);
  }
}

bool ControlServer::SendFrame(Connection* conn, const nlohmann::json& frame) {
  const std::string data = ctl::EncodeFrame(frame);
  size_t sent = 0;
  while (sent < data.size()) {
#if defined(MSG_NOSIGNAL)
    const ssize_t n = ::send(conn->fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(conn->fd, data.data() + sent, data.size() - sent, 0);
#endif
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // small bounded wait; a control client that cannot drain a one-line
      // reply in 5s is broken and gets dropped
      pollfd p{conn->fd, POLLOUT, 0};
      if (::poll(&p, 1, kSendTimeoutMillis) > 0) continue;
    }
    return false;
  }
  return true;
}

void ControlServer::CloseConnection(Connection* conn) {
  const uint64_t connId = conn->id;
  const int fd = conn->fd;
  const int64_t uid = conn->peer.uid;

  if (conn->watchId != 0) {
    g_source_remove(conn->watchId);
    conn->watchId = 0;
  }
  if (conn->helloDeadlineId != 0) {
    g_source_remove(conn->helloDeadlineId);
    conn->helloDeadlineId = 0;
  }
  // EVERY check this connection owns dies with it. Without this a callback
  // fires against a freed Connection minutes later and takes the ROOT daemon
  // down, and with it every client's tunnel.
  if (polkit_ != nullptr) {
    for (const uint64_t checkId : conn->checks) polkit_->Cancel(checkId);
  }
  conn->checks.clear();

  if (tunnelOwner_ == conn) {
    // the owner went away: the tunnel keeps running (the GUI may only have
    // crashed or be restarting) and ownership becomes adoptable. `status` now
    // reports owner_connected=false, so a captured machine with no UI attached
    // is discoverable instead of invisible, and the orphan timeout (off by
    // default) starts running from here.
    //
    // tunnelOwnerUid_ is deliberately NOT cleared: the tunnel is still THAT
    // user's, so another uid still needs kActionTakeOverTunnel to touch it.
    // Gating on the connection alone is what used to let any group member
    // silently displace an absent owner.
    tunnelOwner_ = nullptr;
    tunnel_.SetOwnerConnected(false);
    std::fprintf(stderr, "[control] tunnel owner disconnected (tunnel stays up, uid %lld keeps "
                         "ownership)\n",
                 static_cast<long long>(tunnelOwnerUid_));
  }
  if (overrideWriter_ == conn) {
    // nobody is tracking the connected provider any more: never keep
    // reporting a city we are not exiting through
    overrideWriter_ = nullptr;
    if (geoWriter_.Clear()) {
      std::fprintf(stderr, "[control] cleared location override after client disconnect\n");
    }
  }
  if (conn->peer.pidfd >= 0) {
    ::close(conn->peer.pidfd);
    conn->peer.pidfd = -1;
  }
  const auto uidIt = connectionsPerUid_.find(uid);
  if (uidIt != connectionsPerUid_.end() && --uidIt->second <= 0) {
    connectionsPerUid_.erase(uidIt);
  }
  ::close(fd);
  connIdByFd_.erase(fd);
  connections_.erase(connId);  // frees conn
}

// ---- authorization ---------------------------------------------------------

bool ControlServer::TunnelOwnedByOtherUid(const Connection* conn) const {
  if (conn->peer.uid == 0) return false;                 // root recovery path
  if (tunnelOwnerUid_ < 0) return false;                 // nobody owns it
  if (tunnelOwnerUid_ == conn->peer.uid) return false;   // their own tunnel
  // Ownership only bites while there is something to own. A tunnel that has
  // stopped on its own leaves tunnelOwnerUid_ set, and this is what makes that
  // harmless without polling for the transition.
  const ctl::TunnelState state = tunnel_.Status().tunnel_state;
  return state == ctl::TunnelState::Up || state == ctl::TunnelState::Starting ||
         state == ctl::TunnelState::Stopping;
}

bool ControlServer::StatusMustBeRedactedFor(const Connection* conn) const {
  if (authMode_ != AuthMode::Polkit) return false;  // 0660: today's population
  return TunnelOwnedByOtherUid(conn);
}

// THE LOG OUTLIVES THE TUNNEL, and that is the whole difference between this
// and TunnelOwnedByOtherUid. That predicate deliberately stops biting once the
// tunnel is down — correct for "may you CONTROL it", because there is nothing
// left to control. It is wrong for "may you READ the log": the ring still holds
// the finished session's client ids, provider ids, routes and DNS detail. Under
// the 0660 socket that never mattered; with a world-connectable socket it meant
// any local user could wait for someone else's tunnel to stop and then read it.
bool ControlServer::LogBelongsToOtherUid(const Connection* conn) const {
  if (conn->peer.uid == 0) return false;   // root recovery path
  if (logMixedUids_) return true;          // two users' sessions in one ring
  if (logOwnerUid_ < 0) return false;      // nothing has run yet
  return logOwnerUid_ != conn->peer.uid;
}

void ControlServer::ClaimTunnelOwnership(Connection* conn) {
  tunnelOwner_ = conn;
  tunnelOwnerUid_ = conn->peer.uid;
  // Once a SECOND uid has run a tunnel, the ring contains both and no
  // per-user answer is honest any more, so it narrows to root. Latched
  // rather than recomputed: the point is that the old bytes are still there.
  if (logOwnerUid_ >= 0 && logOwnerUid_ != conn->peer.uid) logMixedUids_ = true;
  logOwnerUid_ = conn->peer.uid;
  tunnel_.SetOwnerConnected(true);
}

void ControlServer::RequireAuth(uint64_t connId, const char* actionId, bool interactive,
                                int64_t replyId,
                                std::function<void(bool, nlohmann::json)> next) {
  Connection* conn = FindConnection(connId);
  if (conn == nullptr) {
    next(false, nlohmann::json());
    return;
  }
  const int64_t uid = conn->peer.uid;
  const int64_t pid = conn->peer.pid;

  // 1. uid 0. Root recovery must work with the system bus down — this is the
  //    path `urnetworkd --revert` and a root shell depend on, and it is the
  //    same fast path AuthorizeControlPeer always had.
  if (uid == 0) {
    next(true, nlohmann::json());
    return;
  }

  // 2. Fallback mode: the group check, and nothing else. Identical to the
  //    behaviour this daemon has always had on a machine with no polkit.
  if (authMode_ != AuthMode::Polkit) {
    if (conn->groupAuthorized) {
      next(true, nlohmann::json());
      return;
    }
    LogAuthOutcome("refused", uid, pid, actionId, ctl::kCodeAuthUnavailable,
                   "not a member of the '" + std::string(ctl::kControlGroupName) +
                       "' group, and this system has no polkit policy");
    next(false,
         ctl::MakeErrorReply(replyId,
                             "this account is not allowed to control URnetwork on this device; "
                             "it has no polkit authorization service, so membership of the '" +
                                 std::string(ctl::kControlGroupName) +
                                 "' group is required instead",
                             ctl::kCodeAuthUnavailable));
    return;
  }

  // 3. A grant already made on THIS connection for THIS action. Per-connection
  //    only — never per-uid, never global — and it dies with the connection.
  //    Keeps a 0.2 Hz log tail off the bus and keeps a multi-step Connect from
  //    prompting twice. take-over-tunnel is deliberately never cached: it is
  //    the cross-user protection.
  if (actionId != nullptr && std::strcmp(actionId, ctl::kActionTakeOverTunnel) != 0 &&
      conn->grants.count(actionId) != 0) {
    next(true, nlohmann::json());
    return;
  }

  if (polkit_ == nullptr) {
    LogAuthOutcome("refused", uid, pid, actionId, ctl::kCodeAuthCheckFailed,
                   "the authorization client is not running");
    next(false, ctl::MakeErrorReply(
                    replyId,
                    "the system authorization service could not be reached, so nothing was "
                    "changed",
                    ctl::kCodeAuthCheckFailed));
    return;
  }

  // At most one check per connection can be in flight (authPending stops the
  // read pump), so clearing here bounds this vector at 1 rather than letting it
  // grow with every request on a long-lived connection.
  conn->checks.clear();

  const std::string action = actionId != nullptr ? actionId : "";
  auto nextShared = std::make_shared<std::function<void(bool, nlohmann::json)>>(std::move(next));
  const uint64_t checkId = polkit_->CheckAsync(
      conn->peer, actionId, interactive,
      [this, connId, replyId, action, nextShared](PolkitAuthorizer::Decision decision,
                                                  std::string detail) {
        Connection* c = FindConnection(connId);
        const int64_t cuid = c != nullptr ? c->peer.uid : -1;
        const int64_t cpid = c != nullptr ? c->peer.pid : -1;
        const char* actionCStr = action.c_str();
        switch (decision) {
          case PolkitAuthorizer::Decision::Allowed:
            if (c != nullptr && action != ctl::kActionTakeOverTunnel) c->grants.insert(action);
            LogAuthOutcome("allowed", cuid, cpid, actionCStr, nullptr, detail);
            (*nextShared)(true, nlohmann::json());
            return;
          case PolkitAuthorizer::Decision::Denied:
            LogAuthOutcome("refused", cuid, cpid, actionCStr, ctl::kCodeAuthDenied, detail);
            (*nextShared)(false,
                          ctl::MakeErrorReply(replyId,
                                              "this device's policy does not allow this account "
                                              "to control URnetwork",
                                              ctl::kCodeAuthDenied));
            return;
          case PolkitAuthorizer::Decision::ChallengeNeeded:
            LogAuthOutcome("refused", cuid, cpid, actionCStr, ctl::kCodeAuthRequired, detail);
            (*nextShared)(
                false,
                ctl::MakeErrorReply(replyId,
                                    "administrator permission is required to do this from this "
                                    "session, and no one could be asked for it",
                                    ctl::kCodeAuthRequired));
            return;
          case PolkitAuthorizer::Decision::Dismissed:
            LogAuthOutcome("refused", cuid, cpid, actionCStr, ctl::kCodeAuthDismissed, detail);
            // The client renders NOTHING for this: a declined elevation is the
            // user changing their mind, not an error.
            (*nextShared)(false, ctl::MakeErrorReply(replyId,
                                                     "the permission request was closed",
                                                     ctl::kCodeAuthDismissed));
            return;
          case PolkitAuthorizer::Decision::TimedOut:
            LogAuthOutcome("refused", cuid, cpid, actionCStr, ctl::kCodeAuthTimeout, detail);
            (*nextShared)(false, ctl::MakeErrorReply(
                                     replyId,
                                     "the permission request was not answered, so nothing was "
                                     "changed",
                                     ctl::kCodeAuthTimeout));
            return;
          case PolkitAuthorizer::Decision::Failed:
            break;
        }
        // FAIL CLOSED. polkit was expected on this machine (our action file is
        // installed) and the check could not be completed, so this is a DENY —
        // never a silent downgrade to the group check, or `systemctl stop
        // polkit` becomes a way to pick the weaker policy.
        LogAuthOutcome("refused", cuid, cpid, actionCStr, ctl::kCodeAuthCheckFailed, detail);
        (*nextShared)(false, ctl::MakeErrorReply(
                                 replyId,
                                 "the system authorization service could not be reached, so "
                                 "nothing was changed",
                                 ctl::kCodeAuthCheckFailed));
      });

  if (checkId != 0) {
    Connection* again = FindConnection(connId);
    if (again != nullptr) again->checks.push_back(checkId);
  }
}

// ---- request dispatch ------------------------------------------------------

void ControlServer::Dispatch(uint64_t connId, const nlohmann::json& request, ReplyFn reply) {
  Connection* conn = FindConnection(connId);
  if (conn == nullptr) return;
  const ctl::Verb verb = ctl::RequestVerb(request);
  const int64_t id = ctl::FrameId(request);

  // HandleHello parses the frame with json::get<>, which THROWS on a malformed
  // or wrongly-typed field. It used to sit outside the try below, so a single
  // bad hello frame took the ROOT daemon down (measured: core dumped) and with
  // it every other client's tunnel. Nothing reachable before authorization may
  // be able to kill this process — and under polkit that surface is reachable
  // by every local uid, so this is now load-bearing for a much larger
  // population than the `urnetwork` group ever was.
  if (verb == ctl::Verb::Hello) {
    try {
      reply(HandleHello(conn, id, request));
    } catch (const std::exception& e) {
      reply(ctl::MakeErrorReply(id, std::string("malformed hello: ") + e.what(),
                                ctl::kCodeHelloRequired));
    } catch (...) {
      reply(ctl::MakeErrorReply(id, "malformed hello", ctl::kCodeHelloRequired));
    }
    return;
  }

  // hello is mandatory before anything else: this is what makes the version
  // negotiation enforced rather than declared. It stays PRE-authorization on
  // purpose — a peer that cannot authorize must still be told WHY (protocol or
  // SDK skew) rather than dropped with no explanation.
  if (!conn->helloOk) {
    reply(ctl::MakeErrorReply(id, "hello with a supported protocol_version is required",
                              ctl::kCodeHelloRequired));
    return;
  }

  const bool isLogTail = ctl::IsLogTailRequest(request);

  // `status` is NEVER gated: it is polled at ~4 Hz through an async bring-up,
  // and a bus round trip (let alone a prompt) there would be intolerable. It is
  // REDACTED for a foreign uid instead — the socket is world-connectable now,
  // and the full status names the provider, the client id, the loopback rpc
  // port and the byte counters.
  if (!isLogTail && verb == ctl::Verb::Status) {
    ctl::StatusReply status = tunnel_.Status();
    if (StatusMustBeRedactedFor(conn)) {
      status = ctl::RedactStatusForForeignUid(status);
    }
    reply(ctl::MakeReply(id, true, nlohmann::json(status)));
    return;
  }

  // A capability query that changes nothing and reveals nothing about a session.
  if (!isLogTail && verb == ctl::Verb::LocationOverrideAvailable) {
    ctl::LocationOverrideAvailableReply payload;
    payload.available = geoWriter_.Available();
    payload.reason = payload.available ? "" : "writer_unavailable";
    reply(ctl::MakeReply(id, true, nlohmann::json(payload)));
    return;
  }

  const bool crossUid = TunnelOwnedByOtherUid(conn);
  const char* actionId = ctl::ActionIdForVerb(verb, isLogTail, crossUid);
  if (actionId == nullptr) {
    reply(ctl::MakeErrorReply(id, "unknown verb"));
    return;
  }
  const bool interactive = ctl::VerbWantsInteraction(verb, isLogTail);

  // `request` is captured BY VALUE: the caller's frame dies when PumpConnection
  // moves on, and an interactive check may not be answered for minutes.
  const nlohmann::json requestCopy = request;
  // THE VALUE THAT CHOSE THE ACTION travels with the callback. Re-deriving
  // ownership after the check is what created a TOCTOU: an interactive polkit
  // check can sit on screen for minutes, and if no foreign tunnel existed when
  // actionId was picked, the everyday control-tunnel action ran under
  // allow_active=yes with NO prompt at all. Should another user's tunnel come
  // up in that window, the callback would then see cross=true and take it over
  // on the strength of an admin check THAT NEVER HAPPENED. Carrying the
  // decision forward means the take-over branch can only be reached by a
  // request that was actually authorized as a take-over.
  const bool authorizedCrossUid = crossUid;
  RequireAuth(connId, actionId, interactive, id,
              [this, connId, id, verb, isLogTail, requestCopy, reply,
               authorizedCrossUid](bool ok, nlohmann::json denied) {
                if (!ok) {
                  reply(std::move(denied));
                  return;
                }
                DispatchAuthorized(connId, id, verb, isLogTail, requestCopy, reply,
                                   authorizedCrossUid);
              });
}

void ControlServer::DispatchAuthorized(uint64_t connId, int64_t id, ctl::Verb verb,
                                       bool isLogTail, const nlohmann::json& request,
                                       const ReplyFn& reply, bool authorizedCrossUid) {
  Connection* conn = FindConnection(connId);
  if (conn == nullptr) return;  // peer vanished mid-check; nothing to answer

  try {
    // log_tail is ADDITIVE within protocol v1: it is not a ctl::Verb, so it is
    // recognised off the raw frame by name. It now needs TWO gates, not one:
    // kActionReadLog (applied above) AND owner-or-root. The daemon log carries
    // client ids, provider ids, route and DNS detail; the socket mode used to
    // be the only thing keeping non-members out of it, and the socket is now
    // world-connectable.
    if (isLogTail) {
      if (conn->peer.uid != 0 && LogBelongsToOtherUid(conn)) {
        LogAuthOutcome("refused", conn->peer.uid, conn->peer.pid, ctl::kActionReadLog,
                       ctl::kCodeAuthNotTunnelOwner,
                       "the session log belongs to uid " + std::to_string(tunnelOwnerUid_));
        reply(ctl::MakeErrorReply(
            id, "another user on this device owns this URnetwork session, so its log is not "
                "readable from this account",
            ctl::kCodeAuthNotTunnelOwner));
        return;
      }
      try {
        const auto req = request.at("args").get<ctl::LogTailRequest>();
        reply(ctl::MakeReply(
            id, true, nlohmann::json(DaemonLog::Instance().Tail(req.cursor, req.max_lines))));
      } catch (const std::exception& e) {
        // Its own code, unchanged: the log card's state machine renders
        // "refused" in words, and a malformed request must not arrive there as
        // a blank.
        reply(ctl::MakeErrorReply(id, std::string("malformed log_tail: ") + e.what(),
                                  ctl::kCodeLogAccessDenied));
      }
      return;
    }

    switch (verb) {
      case ctl::Verb::StartTunnel:
        reply(HandleStartTunnel(conn, id, request, authorizedCrossUid));
        return;

      case ctl::Verb::AttachTunnel:
        reply(HandleAttachTunnel(conn, id, request, authorizedCrossUid));
        return;

      case ctl::Verb::StopTunnel: {
        nlohmann::json denied;
        bool crossUid = false;
        if (!CheckTunnelOwner(conn, id, &denied, &crossUid, authorizedCrossUid)) {
          reply(std::move(denied));
          return;
        }
        // An explicit stop is the one path that also lifts the kill-switch
        // policy (windows semantics: only an unexpected drop keeps it armed).
        tunnel_.Stop("user");
        if (tunnelOwner_ == conn) {
          tunnelOwner_ = nullptr;
          tunnel_.SetOwnerConnected(false);
        }
        // The session is over, so nobody owns it any more. Leaving the uid set
        // would make the next user's first Connect look like a take-over.
        tunnelOwnerUid_ = -1;
        reply(ctl::MakeReply(id, true, nlohmann::json(tunnel_.Status())));
        return;
      }

      case ctl::Verb::SetProvide: {
        nlohmann::json denied;
        bool crossUid = false;
        if (!CheckTunnelOwner(conn, id, &denied, &crossUid, authorizedCrossUid)) {
          reply(std::move(denied));
          return;
        }
        const auto req = request.get<ctl::SetProvideRequest>();
        if (!tunnel_.SetProvideMode(req.mode)) {
          reply(ctl::MakeErrorReply(id, "set provide mode failed"));
          return;
        }
        reply(ctl::MakeReply(id, true));
        return;
      }

      case ctl::Verb::SetKillSwitch: {
        nlohmann::json denied;
        bool crossUid = false;
        if (!CheckTunnelOwner(conn, id, &denied, &crossUid, authorizedCrossUid)) {
          reply(std::move(denied));
          return;
        }
        const auto req = request.get<ctl::SetKillSwitchRequest>();
        std::string error;
        if (!tunnel_.SetKillSwitch(req.enabled, &error)) {
          // The reply still carries the status, whose kill_switch field now
          // reads "failed": a protection that is not in force must never be
          // reported as merely off.
          nlohmann::json failed = ctl::MakeReply(id, false, nlohmann::json(tunnel_.Status()));
          failed["error"] = error.empty() ? "the kill switch could not be installed" : error;
          failed["code"] = ctl::kCodeKillSwitchFailed;
          reply(std::move(failed));
          return;
        }
        reply(ctl::MakeReply(id, true, nlohmann::json(tunnel_.Status())));
        return;
      }

      case ctl::Verb::LocationOverrideWrite: {
        const auto req = request.get<ctl::LocationOverrideWriteRequest>();
        if (!(req.accuracy_m > 0)) {
          reply(ctl::MakeErrorReply(id, "accuracy_m must be > 0"));
          return;
        }
        if (!geoWriter_.Write(req.lat, req.lon, req.accuracy_m)) {
          reply(ctl::MakeErrorReply(id, "could not write the system location file"));
          return;
        }
        overrideWriter_ = conn;
        reply(ctl::MakeReply(id, true));
        return;
      }

      case ctl::Verb::LocationOverrideClear: {
        if (!geoWriter_.Clear()) {
          reply(ctl::MakeErrorReply(id, "could not remove the system location file"));
          return;
        }
        overrideWriter_ = nullptr;
        reply(ctl::MakeReply(id, true));
        return;
      }

      case ctl::Verb::Hello:
      case ctl::Verb::Status:
      case ctl::Verb::LocationOverrideAvailable:
      case ctl::Verb::Unknown:
        break;
    }
  } catch (const std::exception& e) {
    reply(ctl::MakeErrorReply(id, e.what()));
    return;
  }
  reply(ctl::MakeErrorReply(id, "unknown verb"));
}

nlohmann::json ControlServer::HandleHello(Connection* conn, int64_t id,
                                          const nlohmann::json& request) {
  const auto hello = request.get<ctl::HelloRequest>();
  // every hello reply — accepting or rejecting — names our protocol +
  // versions, so the client can always render the right state
  ctl::HelloReply payload;
  payload.protocol_version = ctl::kControlProtocolVersion;
  payload.daemon_version = daemonVersion_;
  payload.sdk_version = sdkVersion_;
  // Told even on a REJECTED hello, deliberately: a client that cannot talk to
  // this daemon still needs to know which remediation applies, and "add
  // yourself to the urnetwork group, then log out and back in" is correct only
  // under `group`.
  payload.auth_mode = AuthModeName();

  if (!ctl::DaemonAcceptsClientProtocol(hello.protocol_version)) {
    // rejection direction 1a: the app is older than this daemon supports
    nlohmann::json reply = ctl::MakeReply(id, false, nlohmann::json(payload));
    reply["error"] = "client protocol " + std::to_string(hello.protocol_version) +
                     " below supported minimum " +
                     std::to_string(ctl::kMinSupportedClientProtocol);
    reply["code"] = ctl::kCodeClientProtocolTooOld;
    return reply;
  }
  if (!ctl::SdkVersionsAgree(sdkVersion_, hello.sdk_version)) {
    // rejection direction 1b: the two binaries carry different SDK builds.
    // The gob device RPC would not fail loudly on drift (renamed fields
    // decode as zero), so it must never be reached by a mismatched pair.
    nlohmann::json reply = ctl::MakeReply(id, false, nlohmann::json(payload));
    reply["error"] =
        "sdk build mismatch: daemon " + (sdkVersion_.empty() ? "(unset)" : sdkVersion_) +
        " vs app " + (hello.sdk_version.empty() ? "(unreported)" : hello.sdk_version);
    reply["code"] = ctl::kCodeSdkVersionMismatch;
    return reply;
  }
  conn->helloOk = true;
  if (conn->helloDeadlineId != 0) {
    g_source_remove(conn->helloDeadlineId);
    conn->helloDeadlineId = 0;
  }
  return ctl::MakeReply(id, true, nlohmann::json(payload));
}

bool ControlServer::CheckTunnelOwner(Connection* conn, int64_t id, nlohmann::json* denied,
                                     bool* crossUid, bool authorizedTakeOver) {
  if (crossUid != nullptr) *crossUid = false;
  // root may always act (recovery path)
  if (conn->peer.uid == 0) return true;

  const bool cross = TunnelOwnedByOtherUid(conn);
  if (crossUid != nullptr) *crossUid = cross;
  if (cross && authMode_ == AuthMode::Polkit && authorizedTakeOver) {
    // Reachable ONLY when the request was authorized AS a take-over —
    // kActionTakeOverTunnel, auth_admin_keep in all three slots, so an
    // administrator password was supplied even at the console. Displacing the
    // other user's session is exactly what was authorized, so the
    // per-connection check below must not then refuse it.
    return true;
  }
  if (cross && !authorizedTakeOver) {
    // Ownership changed while this request's authorization was in flight. The
    // check that completed was NOT the take-over check, so it cannot stand in
    // for one. Refuse and make the caller ask again — the retry re-derives
    // crossUid, selects kActionTakeOverTunnel, and prompts properly.
    *denied = ctl::MakeErrorReply(
        id, "another user started a tunnel while this request was being authorized; try again",
        ctl::kCodeTunnelOwnedByOtherClient);
    return false;
  }
  // Otherwise: unchanged. Only the owning connection may touch the tunnel
  // while its owner is still connected.
  if (tunnelOwner_ != nullptr && tunnelOwner_ != conn) {
    *denied = ctl::MakeErrorReply(id, "the tunnel is controlled by another client",
                                  ctl::kCodeTunnelOwnedByOtherClient);
    return false;
  }
  return true;
}

nlohmann::json ControlServer::HandleStartTunnel(Connection* conn, int64_t id,
                                                const nlohmann::json& request,
                                                bool authorizedCrossUid) {
  // first authorized start_tunnel wins; a later one from a DIFFERENT live
  // client gets a clear error (MIGRATION.md). The same client restarting, or
  // adopting after the previous owner disconnected, is allowed.
  nlohmann::json denied;
  bool crossUid = false;
  if (!CheckTunnelOwner(conn, id, &denied, &crossUid, authorizedCrossUid)) return denied;

  const auto req = request.get<ctl::StartTunnelRequest>();
  // instance_id is the device pairing key (the DeviceLocal is constructed
  // with it and the GUI's DeviceRemote syncs on it) — reject rather than
  // fall back to a daemon-generated id, which would pair-mismatch silently.
  //
  // The same validator is now also the mTLS gate, and it runs BEFORE anything
  // is constructed: a missing triple (kCodeRpcPinRequired) or a malformed one
  // (kCodeRpcPinInvalid) never reaches DeviceLocal::setRpcServer, so a
  // control-socket peer cannot make root bind the device RPC off loopback.
  // The reply now carries the code — it used to be dropped on the floor, which
  // left the client with prose and nothing to branch on.
  if (const auto invalid = ctl::ValidateStartTunnelRequest(req)) {
    if (invalid->code != nullptr) {
      std::fprintf(stderr, "[control] rejecting start_tunnel from uid=%lld: %s (%s)\n",
                   static_cast<long long>(conn->peer.uid), invalid->message.c_str(),
                   invalid->code);
    }
    return ctl::MakeErrorReply(id, invalid->message, invalid->code);
  }

  // ADOPT before restarting. The client issues start_tunnel unconditionally at
  // launch, so without this a live tunnel from a previous GUI run was torn
  // down and rebuilt every single time the app started.
  if (tunnel_.CanAdopt(req)) {
    ClaimTunnelOwnership(conn);
    const ctl::StatusReply status = tunnel_.Status();
    std::fprintf(stderr, "[control] adopted the running tunnel for a reconnecting client "
                         "(uid %lld)\n",
                 static_cast<long long>(conn->peer.uid));
    ctl::StartTunnelReply payload;
    payload.rpc_port = status.rpc_port;
    payload.tunnel_state = status.tunnel_state;
    // The LIVE session's fact, not a re-derivation: CanAdopt only returns true
    // when the request's rpc triple is byte-identical to the one the running
    // listener was pinned with, so this is the same pinning the client asked
    // for. Reporting it is what lets the client dial after a reattach instead
    // of refusing its own working tunnel.
    payload.rpc_pinned = status.rpc_pinned;
    // Same echo attach_tunnel would have produced, from the same source. This
    // IS our adoption path, so it answers with the live identity too — a
    // client must never have to guess which of the two doors it came through.
    payload.instance_id = status.instance_id;
    payload.rpc_session_id = status.rpc_session_id;
    return ctl::MakeReply(id, true, nlohmann::json(payload));
  }

  // A LIVE GENERATION MAY NOT BE RE-POINTED AT DIFFERENT MATERIAL.
  //
  // We are past CanAdopt, so this request does NOT describe the running
  // session — and yet it claims that session's NAME. Falling through would
  // tear a working tunnel down and rebuild it under the same
  // (instance_id, rpc_session_id) with a different key pair, and any other
  // holder of the older pair — a second GUI, a stale keyring record — would
  // afterwards attach BY NAME and be handed a port it cannot dial. That is the
  // silent "connects and never populates" failure, manufactured by us.
  //
  // So the working tunnel stays up and the caller is told what to do instead:
  // mint a new generation (name AND material, together) and start again, which
  // terminates because the new name cannot collide with the live one. This is
  // upstream's kCodeTunnelAlreadyRunning, emitted on a narrower case than
  // upstream's — upstream refuses EVERY start while a tunnel runs, having no
  // adoption path — and it is the daemon's half of "the session id changes
  // whenever the RPC material changes": the client mints, the daemon refuses to
  // let one live name mean two different credentials.
  {
    const ctl::StatusReply live = tunnel_.Status();
    if (live.tunnel_state == ctl::TunnelState::Up &&
        live.instance_id == req.instance_id &&
        live.rpc_session_id == req.rpc_session_id) {
      // The id itself is deliberately NOT logged: it is the name an attach is
      // granted on, and the journal is a wider audience than this session.
      std::fprintf(stderr,
                   "[control] refusing start_tunnel from uid=%lld: it re-uses the running "
                   "session's identity with different rpc material; the running tunnel is "
                   "kept\n",
                   static_cast<long long>(conn->peer.uid));
      return ctl::MakeErrorReply(
          id,
          "a tunnel is already running under this rpc session id with different pinning "
          "material; attach to it with its live identity, or start a new session with a "
          "freshly generated rpc session id and key material",
          ctl::kCodeTunnelAlreadyRunning);
    }
  }

  const ctl::StatusReply status = tunnel_.Start(req);
  if (status.error_code == ctl::kCodeStartInProgress) {
    // Distinct on purpose: a client that timed out and reconnected must NOT
    // cause a teardown-and-rebuild of the bring-up already running.
    nlohmann::json reply = ctl::MakeReply(id, false, nlohmann::json(status));
    reply["error"] = status.error;
    reply["code"] = ctl::kCodeStartInProgress;
    return reply;
  }
  // async: `starting` is a success — the client polls `status` from here.
  const bool accepted = status.tunnel_state == ctl::TunnelState::Up ||
                        (req.async && status.tunnel_state == ctl::TunnelState::Starting);
  if (!accepted) {
    nlohmann::json reply = ctl::MakeReply(id, false, nlohmann::json(status));
    reply["error"] = status.error.empty() ? "tunnel start failed" : status.error;
    // The actionable code the tun/route/DNS layers produced ("the tun kernel
    // module is not loaded", "CAP_NET_ADMIN missing", "route 8.0.0.0/7
    // failed: …", "egress_unprotected"), never a generic failure.
    if (!status.error_code.empty()) reply["code"] = status.error_code;
    return reply;
  }
  ClaimTunnelOwnership(conn);
  ctl::StartTunnelReply payload;
  payload.rpc_port = status.rpc_port;
  payload.tunnel_state = status.tunnel_state;
  // false on the async path (the bring-up has not reached setRpcServer yet) —
  // the client reads StatusReply::rpc_pinned at the transition to Up instead.
  payload.rpc_pinned = status.rpc_pinned;
  // Echoed from the LIVE session, never from the request: on the adoption path
  // above and here alike, what the client needs back is the identity the
  // daemon is actually paired with. BOTH ARE EMPTY ON THE ASYNC PATH — the
  // identity is latched at the up edge and this reply is written while the
  // bring-up is still running — exactly like rpc_pinned beside them, and the
  // client reads them from `status` at the transition to Up.
  payload.instance_id = status.instance_id;
  payload.rpc_session_id = status.rpc_session_id;
  return ctl::MakeReply(id, true, nlohmann::json(payload));
}

// attach_tunnel — re-adopt the running session by NAMING it.
//
// Upstream's verb, and now ours on the same terms: a GUI relaunch loads
// (instance_id, rpc_session_id) out of its record — metadata on disk, the
// client key and pinned server cert in the Secret Service — and re-attaches
// without re-sending by_jwt or any key material. The shape is upstream's: owner gate, validate,
// compare against the live session, claim ownership, reply with the same
// StartTunnelReply a successful start produces.
//
// TWO THINGS ARE OURS, and both are deliberate.
//
// 1. THE OWNER GATE IS OUR SIGNATURE, NOT UPSTREAM'S. Upstream calls
//    CheckTunnelOwner(conn, id, &denied); ours also takes the crossUid out-
//    param and, critically, authorizedTakeOver IN. That last flag is the
//    crossUid value that SELECTED the polkit action back in Dispatch, threaded
//    through DispatchAuthorized and handed here UNCHANGED. It is never
//    re-derived, because re-deriving it is precisely the TOCTOU the polkit fix
//    closed: an interactive check can sit on screen for minutes, and if no
//    foreign tunnel existed when the action was chosen, what completed was the
//    everyday control-tunnel check — often with no prompt at all. Should
//    another user's tunnel appear in that window, a re-derived crossUid would
//    let this take it over on the strength of an admin check that never
//    happened. attach_tunnel is exactly the verb where that would matter most,
//    since taking over is its whole job.
//
// 2. THE REFUSALS ARE SPLIT WHERE UPSTREAM FOLDS THEM. Upstream answers "no
//    tunnel is up" and "that is not the session running" with one comparison
//    and one sentence. Both still carry kCodeRpcSessionMismatch here — the
//    client's move is the same for either, build a session with start_tunnel —
//    but they get different messages, because "nothing is running" and "what is
//    running is not the one you named" send a maintainer reading a bug report
//    in completely different directions.
//
//    THIS HANDLER USED TO BE UNCONDITIONALLY DEAD, and the write-up that stood
//    here blamed the fork's design for it. That was wrong on the facts. It
//    tested `status.rpc_session_id.empty()` and answered a fork-local
//    "this daemon does not persist rpc sessions" code, and that test could
//    never be false because NOTHING IN THE DAEMON EVER SET THE FIELD — not
//    because a regenerating credential model is incompatible with attaching.
//    The missing piece was daemon state, and it is now here: start_tunnel
//    requires rpc_session_id, TunnelHost latches it with instance_id at the up
//    edge, and Status() publishes both. The comparison below therefore has two
//    real halves to test, the fork-local code is DELETED rather than left to
//    document an unreachable path, and the three wrong ways out stay closed —
//    no fabricated session id, no attach accepted without a match, and no
//    "your credentials are stale" for a daemon that simply had none to compare.
//
//    A STALE RECORD FAILS SAFELY AND LEGIBLY, which is the property that
//    matters most on this path: the identity must match in BOTH halves before
//    ownership moves, the refusal names no part of the live identity back to
//    the caller (a mismatch must not become an oracle for guessing one), and a
//    refused client keeps a running tunnel and a working fallback. Nobody is
//    wedged and nobody is silently attached to somebody else's session.
nlohmann::json ControlServer::HandleAttachTunnel(Connection* conn, int64_t id,
                                                 const nlohmann::json& request,
                                                 bool authorizedCrossUid) {
  nlohmann::json denied;
  bool crossUid = false;
  if (!CheckTunnelOwner(conn, id, &denied, &crossUid, authorizedCrossUid)) return denied;

  const auto req = request.get<ctl::AttachTunnelRequest>();
  if (const auto invalid = ctl::ValidateAttachTunnelRequest(req)) {
    return ctl::MakeErrorReply(id, *invalid);
  }

  const ctl::StatusReply status = tunnel_.Status();

  // Its own branch, not folded into the identity comparison below: an idle
  // machine and a stale record are the same code (build a session with
  // start_tunnel) but not the same sentence, and the second one is the only
  // one that means the caller's stored session is worthless.
  //
  // It is also the liveness half of the gate. A tunnel that is `starting`
  // names nothing yet — TunnelHost clears the identity at the head of every
  // bring-up and latches it in the same locked block that publishes Up — so an
  // attach can never land on a half-built session.
  if (status.tunnel_state != ctl::TunnelState::Up) {
    return ctl::MakeErrorReply(
        id, "there is no running tunnel to attach to", ctl::kCodeRpcSessionMismatch);
  }

  // Upstream's identity check, and now it has both halves to test. BOTH
  // matter: instance_id is the device pairing key the DeviceLocal was born
  // with, and rpc_session_id is the credential generation — matching one
  // without the other would hand back a live rpc_port for a session the client
  // cannot actually drive.
  //
  // The message names NEITHER value and does not say which half failed. A
  // refusal that reported "the instance id matched but the session did not"
  // would turn this verb into an oracle for guessing a live session name, and
  // the caller has nothing to do with the answer either way: its record does
  // not describe the running tunnel, so it starts a new session.
  if (status.instance_id != req.instance_id ||
      status.rpc_session_id != req.rpc_session_id) {
    std::fprintf(stderr,
                 "[control] attach_tunnel from uid=%lld refused: the named session is not the "
                 "one running\n",
                 static_cast<long long>(conn->peer.uid));
    return ctl::MakeErrorReply(id, "running tunnel identity or RPC session does not match",
                               ctl::kCodeRpcSessionMismatch);
  }

  ClaimTunnelOwnership(conn);
  std::fprintf(stderr, "[control] attached the running tunnel to uid=%lld%s\n",
               static_cast<long long>(conn->peer.uid),
               crossUid ? " (authorized take-over from another user)" : "");
  ctl::StartTunnelReply payload;
  payload.rpc_port = status.rpc_port;
  payload.tunnel_state = status.tunnel_state;
  payload.rpc_pinned = status.rpc_pinned;
  payload.instance_id = status.instance_id;
  payload.rpc_session_id = status.rpc_session_id;
  return ctl::MakeReply(id, true, nlohmann::json(payload));
}

}  // namespace urnw
