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

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <glib-unix.h>

namespace urnw {
namespace {

constexpr size_t kMaxFrameBytes = 1 << 20;  // request line beyond 1 MiB: protocol error
constexpr int kSendTimeoutMillis = 5000;

// --- peer credential resolution ---------------------------------------------

// SO_PEERCRED at accept time, before any frame is read. Returns false when the
// kernel cannot answer — which fails closed (the connection is dropped).
bool PeerCredentials(int fd, int64_t* uid, int64_t* gid) {
#if defined(__linux__)
  struct ucred cred {};
  socklen_t len = sizeof(cred);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
  *uid = cred.uid;
  *gid = cred.gid;
  return true;
#else
  // Non-Linux (dev builds only): getpeereid is the closest analogue.
  uid_t peerUid = 0;
  gid_t peerGid = 0;
  if (::getpeereid(fd, &peerUid, &peerGid) != 0) return false;
  *uid = peerUid;
  *gid = peerGid;
  return true;
#endif
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

}  // namespace

ControlServer::ControlServer(TunnelHost& tunnel, GeoClueWriter& geoWriter)
    : tunnel_(tunnel), geoWriter_(geoWriter) {}

ControlServer::~ControlServer() { Stop(); }

std::string ControlServer::SocketPath() {
  if (const char* env = std::getenv("URNETWORK_CONTROL_SOCKET"); env && *env) return env;
  return ctl::kControlSocketPath;
}

bool ControlServer::Start() {
  const std::string path = SocketPath();
  const bool defaultPath = (path == ctl::kControlSocketPath);
  const int64_t groupGid = ControlGroupGid();

  if (defaultPath) {
    // dir 0750 root:urnetwork (systemd's RuntimeDirectory normally made it;
    // this is the --foreground/dev fallback and a permission re-assert)
    ::mkdir(ctl::kControlSocketDir, 0750);
    ::chmod(ctl::kControlSocketDir, 0750);
    if (groupGid >= 0) {
      if (::chown(ctl::kControlSocketDir, 0, static_cast<gid_t>(groupGid)) != 0) {
        std::fprintf(stderr, "[control] chown %s: %s\n", ctl::kControlSocketDir,
                     std::strerror(errno));
      }
    } else {
      std::fprintf(stderr,
                   "[control] group '%s' does not exist: only root will be authorized\n",
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
  // sock 0660 root:urnetwork. The chmod is not a substitute for SO_PEERCRED —
  // it only narrows who can connect at all; the ucred check is the boundary.
  ::chmod(path.c_str(), 0660);
  if (groupGid >= 0) {
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
  std::fprintf(stderr, "[control] listening on %s\n", path.c_str());
  return true;
}

void ControlServer::Stop() {
  while (!connections_.empty()) {
    CloseConnection(connections_.begin()->second.get());
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

  // SO_PEERCRED FIRST — before any byte of any frame is read. Authorize on
  // uid/gid only; cache the ucred on the connection.
  int64_t uid = -1;
  int64_t gid = -1;
  if (!PeerCredentials(fd, &uid, &gid)) {
    std::fprintf(stderr, "[control] rejecting peer: no credentials\n");
    ::close(fd);
    return;
  }
  if (!ctl::AuthorizeControlPeer(uid, gid, PeerGroupIds(uid, gid), ControlGroupGid())) {
    std::fprintf(stderr, "[control] rejecting peer uid=%lld: not root or '%s'\n",
                 static_cast<long long>(uid), ctl::kControlGroupName);
    ::close(fd);
    return;
  }

  auto conn = std::make_unique<Connection>();
  conn->fd = fd;
  conn->uid = uid;
  conn->gid = gid;
  conn->watchId = g_unix_fd_add(fd, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
                                &ControlServer::OnConnectionReadable, this);
  connections_[fd] = std::move(conn);
}

gboolean ControlServer::OnConnectionReadable(gint fd, GIOCondition, gpointer data) {
  auto* self = static_cast<ControlServer*>(data);
  const auto it = self->connections_.find(fd);
  if (it == self->connections_.end()) return G_SOURCE_REMOVE;
  Connection* conn = it->second.get();
  if (!self->ReadAndDispatch(conn)) {
    // the source is being removed by returning FALSE; make CloseConnection
    // not double-remove it
    conn->watchId = 0;
    self->CloseConnection(conn);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

bool ControlServer::ReadAndDispatch(Connection* conn) {
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
  // dispatch every complete line
  size_t pos;
  while ((pos = conn->inBuf.find('\n')) != std::string::npos) {
    const std::string line = conn->inBuf.substr(0, pos);
    conn->inBuf.erase(0, pos + 1);
    const auto frame = ctl::DecodeFrame(line);
    if (!frame) return false;  // not JSON: drop the connection
    const nlohmann::json reply = Dispatch(conn, *frame);
    if (!SendFrame(conn, reply)) return false;
  }
  return true;
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
  if (conn->watchId != 0) {
    g_source_remove(conn->watchId);
    conn->watchId = 0;
  }
  if (tunnelOwner_ == conn) {
    // the owner went away: the tunnel keeps running (the GUI may only have
    // crashed or be restarting) and ownership becomes adoptable. `status` now
    // reports owner_connected=false, so a captured machine with no UI attached
    // is discoverable instead of invisible, and the orphan timeout (off by
    // default) starts running from here.
    tunnelOwner_ = nullptr;
    tunnel_.SetOwnerConnected(false);
    std::fprintf(stderr, "[control] tunnel owner disconnected (tunnel stays up)\n");
  }
  if (overrideWriter_ == conn) {
    // nobody is tracking the connected provider any more: never keep
    // reporting a city we are not exiting through
    overrideWriter_ = nullptr;
    if (geoWriter_.Clear()) {
      std::fprintf(stderr, "[control] cleared location override after client disconnect\n");
    }
  }
  const int fd = conn->fd;
  ::close(fd);
  connections_.erase(fd);  // frees conn
}

// ---- request dispatch ------------------------------------------------------

nlohmann::json ControlServer::Dispatch(Connection* conn, const nlohmann::json& request) {
  const ctl::Verb verb = ctl::RequestVerb(request);
  const int64_t id = ctl::FrameId(request);

  // HandleHello parses the frame with json::get<>, which THROWS on a malformed
  // or wrongly-typed field. It used to sit outside the try below, so a single
  // bad hello frame took the ROOT daemon down (measured: core dumped) and with
  // it every other client's tunnel. Nothing reachable before authorization may
  // be able to kill this process.
  if (verb == ctl::Verb::Hello) {
    try {
      return HandleHello(conn, id, request);
    } catch (const std::exception& e) {
      return ctl::MakeErrorReply(id, std::string("malformed hello: ") + e.what(),
                                 ctl::kCodeHelloRequired);
    } catch (...) {
      return ctl::MakeErrorReply(id, "malformed hello", ctl::kCodeHelloRequired);
    }
  }

  // hello is mandatory before anything else: this is what makes the version
  // negotiation enforced rather than declared
  if (!conn->helloOk) {
    return ctl::MakeErrorReply(id, "hello with a supported protocol_version is required",
                               ctl::kCodeHelloRequired);
  }

  // log_tail is ADDITIVE within protocol v1: it is not a ctl::Verb, so it is
  // recognised off the raw frame by name. It sits after the helloOk gate above,
  // so an unauthorized peer can never read the daemon's log.
  if (ctl::IsLogTailRequest(request)) {
    try {
      const auto req = request.at("args").get<ctl::LogTailRequest>();
      return ctl::MakeReply(id, true,
                            nlohmann::json(DaemonLog::Instance().Tail(req.cursor, req.max_lines)));
    } catch (const std::exception& e) {
      return ctl::MakeErrorReply(id, std::string("malformed log_tail: ") + e.what(),
                                 ctl::kCodeLogAccessDenied);
    }
  }

  try {
    switch (verb) {
      case ctl::Verb::Status:
        return ctl::MakeReply(id, true, nlohmann::json(tunnel_.Status()));

      case ctl::Verb::StartTunnel:
        return HandleStartTunnel(conn, id, request);

      case ctl::Verb::StopTunnel: {
        nlohmann::json denied;
        if (!CheckTunnelOwner(conn, id, &denied)) return denied;
        // An explicit stop is the one path that also lifts the kill-switch
        // policy (windows semantics: only an unexpected drop keeps it armed).
        tunnel_.Stop("user");
        if (tunnelOwner_ == conn) {
          tunnelOwner_ = nullptr;
          tunnel_.SetOwnerConnected(false);
        }
        return ctl::MakeReply(id, true, nlohmann::json(tunnel_.Status()));
      }

      case ctl::Verb::SetProvide: {
        nlohmann::json denied;
        if (!CheckTunnelOwner(conn, id, &denied)) return denied;
        const auto req = request.get<ctl::SetProvideRequest>();
        if (!tunnel_.SetProvideMode(req.mode)) {
          return ctl::MakeErrorReply(id, "set provide mode failed");
        }
        return ctl::MakeReply(id, true);
      }

      case ctl::Verb::SetKillSwitch: {
        nlohmann::json denied;
        if (!CheckTunnelOwner(conn, id, &denied)) return denied;
        const auto req = request.get<ctl::SetKillSwitchRequest>();
        std::string error;
        if (!tunnel_.SetKillSwitch(req.enabled, &error)) {
          // The reply still carries the status, whose kill_switch field now
          // reads "failed": a protection that is not in force must never be
          // reported as merely off.
          nlohmann::json reply = ctl::MakeReply(id, false, nlohmann::json(tunnel_.Status()));
          reply["error"] = error.empty() ? "the kill switch could not be installed" : error;
          reply["code"] = ctl::kCodeKillSwitchFailed;
          return reply;
        }
        return ctl::MakeReply(id, true, nlohmann::json(tunnel_.Status()));
      }

      case ctl::Verb::LocationOverrideAvailable: {
        ctl::LocationOverrideAvailableReply reply;
        reply.available = geoWriter_.Available();
        reply.reason = reply.available ? "" : "writer_unavailable";
        return ctl::MakeReply(id, true, nlohmann::json(reply));
      }

      case ctl::Verb::LocationOverrideWrite: {
        const auto req = request.get<ctl::LocationOverrideWriteRequest>();
        if (!(req.accuracy_m > 0)) {
          return ctl::MakeErrorReply(id, "accuracy_m must be > 0");
        }
        if (!geoWriter_.Write(req.lat, req.lon, req.accuracy_m)) {
          return ctl::MakeErrorReply(id, "could not write the system location file");
        }
        overrideWriter_ = conn;
        return ctl::MakeReply(id, true);
      }

      case ctl::Verb::LocationOverrideClear: {
        if (!geoWriter_.Clear()) {
          return ctl::MakeErrorReply(id, "could not remove the system location file");
        }
        overrideWriter_ = nullptr;
        return ctl::MakeReply(id, true);
      }

      case ctl::Verb::Hello:
      case ctl::Verb::Unknown:
        break;
    }
  } catch (const std::exception& e) {
    return ctl::MakeErrorReply(id, e.what());
  }
  return ctl::MakeErrorReply(id, "unknown verb");
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
  return ctl::MakeReply(id, true, nlohmann::json(payload));
}

bool ControlServer::CheckTunnelOwner(Connection* conn, int64_t id, nlohmann::json* denied) {
  // root may always act (recovery path); otherwise only the owning connection
  // may touch the tunnel while its owner is still connected
  if (conn->uid == 0) return true;
  if (tunnelOwner_ != nullptr && tunnelOwner_ != conn) {
    *denied = ctl::MakeErrorReply(id, "the tunnel is controlled by another client",
                                  ctl::kCodeTunnelOwnedByOtherClient);
    return false;
  }
  return true;
}

nlohmann::json ControlServer::HandleStartTunnel(Connection* conn, int64_t id,
                                                const nlohmann::json& request) {
  // first authenticated start_tunnel wins; a later one from a DIFFERENT live
  // client gets a clear error (MIGRATION.md). The same client restarting, or
  // adopting after the previous owner disconnected, is allowed.
  nlohmann::json denied;
  if (!CheckTunnelOwner(conn, id, &denied)) return denied;

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
                   static_cast<long long>(conn->uid), invalid->message.c_str(), invalid->code);
    }
    return ctl::MakeErrorReply(id, invalid->message, invalid->code);
  }

  // ADOPT before restarting. The client issues start_tunnel unconditionally at
  // launch, so without this a live tunnel from a previous GUI run was torn
  // down and rebuilt every single time the app started.
  if (tunnel_.CanAdopt(req)) {
    tunnelOwner_ = conn;
    tunnel_.SetOwnerConnected(true);
    const ctl::StatusReply status = tunnel_.Status();
    std::fprintf(stderr, "[control] adopted the running tunnel for a reconnecting client\n");
    ctl::StartTunnelReply payload;
    payload.rpc_port = status.rpc_port;
    payload.tunnel_state = status.tunnel_state;
    // The LIVE session's fact, not a re-derivation: CanAdopt only returns true
    // when the request's rpc triple is byte-identical to the one the running
    // listener was pinned with, so this is the same pinning the client asked
    // for. Reporting it is what lets the client dial after a reattach instead
    // of refusing its own working tunnel.
    payload.rpc_pinned = status.rpc_pinned;
    return ctl::MakeReply(id, true, nlohmann::json(payload));
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
  tunnelOwner_ = conn;
  tunnel_.SetOwnerConnected(true);
  ctl::StartTunnelReply payload;
  payload.rpc_port = status.rpc_port;
  payload.tunnel_state = status.tunnel_state;
  // false on the async path (the bring-up has not reached setRpcServer yet) —
  // the client reads StatusReply::rpc_pinned at the transition to Up instead.
  payload.rpc_pinned = status.rpc_pinned;
  return ctl::MakeReply(id, true, nlohmann::json(payload));
}

}  // namespace urnw
