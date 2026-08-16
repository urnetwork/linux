// SPDX-License-Identifier: MPL-2.0
#include "ControlClient.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace urnw {
namespace {

// Every verb except start_tunnel answers off a published snapshot and returns
// in microseconds, so a short bound is right for them and a long one only
// hides a wedged daemon.
constexpr time_t kReceiveTimeoutSeconds = 30;
// A SYNCHRONOUS start_tunnel covers device creation (a network round trip),
// the tun, ~35 subprocesses for routes/rules/DNS and the nftables swap. 30 s
// was not a generous bound for that, it was a restart-loop trigger. Clients
// that ask for async never wait this long — they get `starting` and poll.
constexpr time_t kStartTunnelReceiveTimeoutSeconds = 180;
constexpr time_t kSendTimeoutSeconds = 10;
constexpr size_t kMaxFrameBytes = 1 << 20;  // a reply line beyond 1 MiB is a protocol error

void SetTimeout(int fd, int kind, time_t seconds) {
  timeval tv{};
  tv.tv_sec = seconds;
  ::setsockopt(fd, SOL_SOCKET, kind, &tv, sizeof(tv));
}

}  // namespace

ControlClient::~ControlClient() { Close(); }

std::string ControlClient::SocketPath() {
  if (const char* env = std::getenv("URNETWORK_CONTROL_SOCKET"); env && *env) return env;
  return ctl::kControlSocketPath;
}

void ControlClient::Close() {
  std::scoped_lock lock(mutex_);
  CloseLocked();
}

void ControlClient::CloseLocked() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  helloOk_ = false;
  recvBuffer_.clear();
  receiveTimeoutSeconds_ = 0;
}

bool ControlClient::ConnectLocked(std::string* error) {
  CloseLocked();
  const std::string path = SocketPath();
  lastSocketPath_ = path;
  lastUnreachable_ = DaemonUnreachableReason::Other;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    if (error) *error = "control socket path too long: " + path;
    return false;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    if (error) *error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE  // macOS: no MSG_NOSIGNAL, suppress SIGPIPE per socket
  {
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif
  SetTimeout(fd, SO_RCVTIMEO, kReceiveTimeoutSeconds);
  SetTimeout(fd, SO_SNDTIMEO, kSendTimeoutSeconds);
  receiveTimeoutSeconds_ = kReceiveTimeoutSeconds;

  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    const int e = errno;
    // The three Unreachable causes are three different problems. EACCES in
    // particular is "the daemon IS running and you are not in the `urnetwork`
    // group" — the state a fresh install lands in, because the installers
    // create the group empty.
    switch (e) {
      case EACCES:
      case EPERM:
        lastUnreachable_ = DaemonUnreachableReason::PermissionDenied;
        break;
      case ENOENT:
      case ECONNREFUSED:
        lastUnreachable_ = DaemonUnreachableReason::SocketMissing;
        break;
      default:
        lastUnreachable_ = DaemonUnreachableReason::Other;
        break;
    }
    if (error) *error = path + ": " + std::strerror(e);
    ::close(fd);
    return false;
  }
  lastUnreachable_ = DaemonUnreachableReason::None;
  fd_ = fd;
  return true;
}

void ControlClient::SetReceiveTimeoutLocked(long seconds) {
  if (fd_ < 0 || seconds <= 0 || seconds == receiveTimeoutSeconds_) return;
  SetTimeout(fd_, SO_RCVTIMEO, static_cast<time_t>(seconds));
  receiveTimeoutSeconds_ = seconds;
}

bool ControlClient::SendAllLocked(const std::string& data, size_t* sentOut) {
  size_t sent = 0;
  if (sentOut) *sentOut = 0;
  while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
    const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
#endif
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    sent += static_cast<size_t>(n);
    // Reported as we go, not at the end, so the count is meaningful on the
    // failure path too — "0 of 2875 bytes" and "2100 of 2875" are different
    // diagnoses of a dead socket.
    if (sentOut) *sentOut = sent;
  }
  return true;
}

// A cached fd is a memory of a connection, not a connection. urnetworkd
// restarting (a `systemctl restart`, or the installer's reinstall) closes
// every control connection, and a client that only notices at write time
// spends its one non-idempotent attempt on a socket with nothing behind it.
// That is exactly how a Connect after a daemon restart reached the daemon
// zero times and still reported "the service is not running".
bool ControlClient::SessionAliveLocked() {
  if (fd_ < 0) return false;
  pollfd pfd{};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  const int n = ::poll(&pfd, 1, 0);
  if (n < 0) return errno == EINTR;  // could not tell: assume alive, the write decides
  if (n == 0) return true;           // nothing pending: alive
  if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) return false;
  if (pfd.revents & POLLIN) {
    // Readable is ambiguous: an orderly close reads as EOF, while a future
    // daemon pushing an unsolicited event reads as data. PEEK so a real frame
    // stays queued for RoundTripLocked.
    char probe = 0;
    const ssize_t r = ::recv(fd_, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 0) return false;  // EOF: the daemon closed on us
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
  }
  return true;
}

bool ControlClient::ReadLineLocked(std::string& line) {
  for (;;) {
    if (const size_t pos = recvBuffer_.find('\n'); pos != std::string::npos) {
      line = recvBuffer_.substr(0, pos);
      recvBuffer_.erase(0, pos + 1);
      return true;
    }
    if (recvBuffer_.size() > kMaxFrameBytes) return false;
    char buf[4096];
    const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;  // EOF, timeout, or error: the caller closes + may retry
    }
    recvBuffer_.append(buf, static_cast<size_t>(n));
  }
}

std::optional<nlohmann::json> ControlClient::RoundTripLocked(const nlohmann::json& request,
                                                             int64_t id, bool* frameDelivered) {
  if (frameDelivered) *frameDelivered = false;
  if (fd_ < 0) return std::nullopt;
  size_t sent = 0;
  const std::string frame = ctl::EncodeFrame(request);
  const bool ok = SendAllLocked(frame, &sent);
  // A frame is one LINE (EncodeFrame ends in '\n'), so a short write cannot
  // have produced anything the daemon will decode, let alone act on. Only a
  // complete write puts the request in the daemon's hands.
  if (frameDelivered) *frameDelivered = ok;
  if (!ok) {
    std::fprintf(stderr, "[control] send failed after %zu of %zu bytes: %s\n", sent,
                 frame.size(), std::strerror(errno));
    return std::nullopt;
  }
  // Read until the reply matching our id; skip anything else (a future daemon
  // may push unsolicited events — additive, so ignore them here).
  for (;;) {
    std::string line;
    if (!ReadLineLocked(line)) return std::nullopt;
    auto frame = ctl::DecodeFrame(line);
    if (!frame) return std::nullopt;  // garbage on a trusted socket: bail out
    if (ctl::FrameId(*frame) == id) return frame;
  }
}

void ControlClient::SetLocalSdkVersion(std::string sdkVersion) {
  std::scoped_lock lock(mutex_);
  localSdkVersion_ = std::move(sdkVersion);
}

DaemonSessionState ControlClient::HelloLocked(std::string* error) {
  const int64_t id = nextId_++;
  ctl::HelloRequest hello;  // carries kControlProtocolVersion
  hello.sdk_version = localSdkVersion_;
  const auto reply =
      RoundTripLocked(ctl::MakeRequest(ctl::Verb::Hello, id, nlohmann::json(hello)), id);
  if (!reply) {
    if (error) *error = "daemon closed the connection during hello";
    CloseLocked();
    return DaemonSessionState::Unreachable;
  }
  if (!ctl::ReplyOk(*reply)) {
    // Rejection direction 1: the daemon dropped support for our protocol, or
    // its SDK build differs from ours (the device RPC has no version field of
    // its own, so this is where the mismatch gets caught).
    const std::string message = ctl::ReplyError(*reply);
    if (error) *error = message.empty() ? "hello rejected" : message;
    const std::string code = ctl::ReplyCode(*reply);
    // keep the daemon's identity for the mismatch UI even on rejection
    daemonVersion_ = reply->get<ctl::HelloReply>().daemon_version;
    CloseLocked();
    if (code == ctl::kCodeClientProtocolTooOld) return DaemonSessionState::ClientTooOld;
    if (code == ctl::kCodeSdkVersionMismatch) return DaemonSessionState::SdkMismatch;
    return DaemonSessionState::Error;
  }
  const auto helloReply = reply->get<ctl::HelloReply>();
  daemonVersion_ = helloReply.daemon_version;
  // Rejection direction 2: the daemon is older than we support.
  if (!ctl::ClientAcceptsDaemonProtocol(helloReply.protocol_version)) {
    if (error) {
      *error = "daemon protocol " + std::to_string(helloReply.protocol_version) +
               " below supported minimum " + std::to_string(ctl::kMinSupportedDaemonProtocol);
    }
    CloseLocked();
    return DaemonSessionState::DaemonTooOld;
  }
  // Exact SDK build agreement, enforced from THIS side too (a daemon
  // predating the check would have accepted us above).
  if (!ctl::SdkVersionsAgree(localSdkVersion_, helloReply.sdk_version)) {
    if (error) {
      *error = "sdk build mismatch: app " +
               (localSdkVersion_.empty() ? std::string("(unset)") : localSdkVersion_) +
               " vs daemon " +
               (helloReply.sdk_version.empty() ? std::string("(unreported)")
                                               : helloReply.sdk_version);
    }
    CloseLocked();
    return DaemonSessionState::SdkMismatch;
  }
  helloOk_ = true;
  // A NEW connection is now live. Everything a caller cached against the
  // previous one — most importantly a DeviceRemote bound to a DeviceLocal
  // that died with the old daemon process — is stale from here.
  ++sessionGeneration_;
  return DaemonSessionState::Ok;
}

DaemonSessionState ControlClient::EnsureSessionLocked(std::string* error) {
  if (fd_ >= 0 && helloOk_) {
    // NOT just `fd_ >= 0 && helloOk_`. That short-circuit believed a dead
    // socket forever: after urnetworkd restarted, the next verb was written
    // into a closed connection, and for the one verb that is not re-sent
    // (start_tunnel) the Connect was lost outright — the daemon received
    // nothing at all and the UI blamed a service that was running fine.
    if (SessionAliveLocked()) {
      lastState_ = DaemonSessionState::Ok;
      return lastState_;
    }
    std::fprintf(stderr,
                 "[control] the daemon closed our control session (service restarted?); "
                 "reconnecting before the next request\n");
    CloseLocked();
  }
  if (!ConnectLocked(error)) {
    lastState_ = DaemonSessionState::Unreachable;
    return lastState_;
  }
  lastState_ = HelloLocked(error);
  return lastState_;
}

DaemonSessionState ControlClient::EnsureSession(std::string* error) {
  std::scoped_lock lock(mutex_);
  return EnsureSessionLocked(error);
}

DaemonSessionState ControlClient::LastSessionState() {
  std::scoped_lock lock(mutex_);
  return lastState_;
}

std::string ControlClient::DaemonVersion() {
  std::scoped_lock lock(mutex_);
  return daemonVersion_;
}

DaemonUnreachableReason ControlClient::LastUnreachableReason() {
  std::scoped_lock lock(mutex_);
  return lastUnreachable_;
}

std::string ControlClient::LastSocketPath() {
  std::scoped_lock lock(mutex_);
  return lastSocketPath_.empty() ? SocketPath() : lastSocketPath_;
}

std::optional<nlohmann::json> ControlClient::CallLocked(ctl::Verb verb, nlohmann::json payload,
                                                        std::string* error, bool allowRetry,
                                                        long receiveTimeoutSeconds) {
  constexpr int kMaxAttempts = 2;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (EnsureSessionLocked(error) != DaemonSessionState::Ok) return std::nullopt;
    SetReceiveTimeoutLocked(receiveTimeoutSeconds);
    const int64_t id = nextId_++;
    bool frameDelivered = false;
    if (auto reply =
            RoundTripLocked(ctl::MakeRequest(verb, id, payload), id, &frameDelivered)) {
      return reply;
    }
    // Dead socket. Whether we may send it again turns on ONE question: did any
    // byte of this request reach the daemon?
    CloseLocked();
    lastState_ = DaemonSessionState::Unreachable;
    if (error) *error = "lost connection to the daemon";
    if (attempt + 1 >= kMaxAttempts) {
      std::fprintf(stderr, "[control] %s failed twice; giving up\n", ctl::ToString(verb));
      break;
    }
    if (!allowRetry && frameDelivered) {
      // It reached the daemon and went unanswered: the daemon may be acting on
      // it right now, and a duplicate start_tunnel is what used to tear a
      // half-built tunnel down and restart it. Abandon it, loudly.
      std::fprintf(stderr,
                   "[control] %s reached the daemon but it did not answer; NOT re-sending "
                   "(a duplicate would restart the bring-up)\n",
                   ctl::ToString(verb));
      break;
    }
    // The frame never landed whole, so the daemon cannot have decoded it, let
    // alone acted on it — this is not "a request that may be running", it is a
    // request that never happened. Reconnecting and sending it once is safe
    // even for start_tunnel, and NOT doing so is what turned the first Connect
    // after a daemon restart into a silent no-op with an empty daemon journal.
    std::fprintf(stderr, "[control] %s never reached the daemon; reconnecting and sending "
                         "it once\n",
                 ctl::ToString(verb));
  }
  return std::nullopt;
}

ControlClient::StartTunnelOutcome ControlClient::StartTunnelEx(
    const StartTunnelOptions& options) {
  std::scoped_lock lock(mutex_);
  StartTunnelOutcome outcome;
  ctl::StartTunnelRequest req;
  req.by_jwt = options.by_jwt;
  req.instance_id = options.instance_id;
  req.app_version = options.app_version;
  req.network_space_json = options.network_space_json;
  req.async = options.async;
  req.kill_switch = options.kill_switch;
  req.rpc_server_pem = options.rpc_server_pem;
  req.rpc_client_cert_pem = options.rpc_client_cert_pem;
  req.rpc_listen_hostport = options.rpc_listen_hostport;

  // ---- gate 1: refuse before the frame is built -----------------------------
  // The SAME pure validator the daemon runs, so the two halves can never
  // disagree about what a valid request is, and so a caller that forgot to
  // generate key material (or generated a handle-0 one, whose four PEM getters
  // return empty strings with NO exception) finds out here instead of getting
  // a root device RPC with no client pinning on it.
  if (const auto invalid = ctl::ValidateStartTunnelRequest(req)) {
    outcome.error = invalid->message;
    outcome.code = invalid->code == nullptr ? std::string() : std::string(invalid->code);
    // Not a transport problem, but the caller branches on LastSessionState()
    // as well as on the outcome, and leaving it at Unreachable would make the
    // UI offer "install or start the service" for what is a local refusal.
    lastState_ = DaemonSessionState::Error;
    outcome.session = lastState_;
    return outcome;
  }
  const int expectedRpcPort = ctl::RpcPortFromHostPort(options.rpc_listen_hostport);

  // NO retry (a re-sent start_tunnel used to restart the bring-up), and an
  // async request needs only the ordinary bound because the daemon answers as
  // soon as it has accepted the request.
  const auto reply = CallLocked(
      ctl::Verb::StartTunnel, nlohmann::json(req), &outcome.error, /*allowRetry=*/false,
      options.async ? kReceiveTimeoutSeconds : kStartTunnelReceiveTimeoutSeconds);
  outcome.session = lastState_;
  if (!reply) return outcome;
  if (!ctl::ReplyOk(*reply)) {
    outcome.error = ctl::ReplyError(*reply);
    outcome.code = ctl::ReplyCode(*reply);
    // A start already in progress is not a session error — the daemon is
    // healthy and busy. Collapsing it into Error would make the UI offer
    // "install or start the service".
    if (outcome.code != ctl::kCodeStartInProgress) lastState_ = DaemonSessionState::Error;
    outcome.session = lastState_;
    return outcome;
  }
  const auto payload = reply->get<ctl::StartTunnelReply>();
  outcome.rpc_port = payload.rpc_port;
  outcome.tunnel_state = payload.tunnel_state;
  outcome.rpc_pinned = payload.rpc_pinned;

  // ---- gate 2: a synchronous start must come back PINNED --------------------
  // A daemon that predates this contract drops the three fields on parse,
  // never calls setRpcServer, and answers rpc_port=12025 with rpc_pinned
  // absent — which parses false. Refuse: do not hand the caller an outcome it
  // could build a plaintext DeviceRemote from. The port cross-check catches
  // the same peer a second way, because the GUI's draw range deliberately
  // excludes 12025.
  //
  // Only the synchronous path can be decided here. On async the reply is
  // `starting` with rpc_port=0 and pinning has not happened yet, so the caller
  // MUST make the identical check against StatusReply::rpc_pinned when the
  // tunnel reaches Up, before it constructs a DeviceRemote.
  if (payload.tunnel_state == ctl::TunnelState::Up &&
      (!payload.rpc_pinned || payload.rpc_port != expectedRpcPort)) {
    // Rendered VERBATIM by the UI (MainWindow shows LastTunnelError() when it
    // is non-empty), so these are complete sentences with a next step, not
    // codes — and never a blank.
    outcome.error =
        payload.rpc_pinned
            ? "The URnetwork system service secured the local control connection on an "
              "unexpected port. Update the service, then try again."
            : "The URnetwork system service did not secure the local control connection. "
              "Update the service, then try again.";
    outcome.code = ctl::kCodeRpcPinRequired;
    outcome.ok = false;
    // Leaving a running tunnel whose ROOT rpc listener is unauthenticated is
    // worse than no tunnel: any local process that can open a TCP socket to it
    // could drive the daemon's DeviceLocal. Tear it down on the way out.
    std::string stopError;
    if (!StopTunnelLocked(&stopError)) {
      std::fprintf(stderr,
                   "[control] could not stop the unpinned tunnel after refusing it: %s\n",
                   stopError.c_str());
    }
    lastState_ = DaemonSessionState::Error;
    outcome.session = lastState_;
    return outcome;
  }
  outcome.ok = true;
  return outcome;
}

bool ControlClient::SetKillSwitch(bool enabled, ctl::StatusReply* out, std::string* error) {
  std::scoped_lock lock(mutex_);
  ctl::SetKillSwitchRequest req;
  req.enabled = enabled;
  const auto reply = CallLocked(ctl::Verb::SetKillSwitch, nlohmann::json(req), error);
  if (!reply) return false;
  // The status rides on BOTH the success and the failure reply: a kill switch
  // that could not be installed reports kill_switch=failed, which the UI must
  // render as its own state and never as "off".
  if (out) *out = reply->get<ctl::StatusReply>();
  if (!ctl::ReplyOk(*reply)) {
    if (error) *error = ctl::ReplyError(*reply);
    return false;
  }
  return true;
}

bool ControlClient::StopTunnelLocked(std::string* error) {
  const auto reply = CallLocked(ctl::Verb::StopTunnel, nlohmann::json::object(), error);
  if (!reply) return false;
  if (!ctl::ReplyOk(*reply)) {
    if (error) *error = ctl::ReplyError(*reply);
    return false;
  }
  return true;
}

bool ControlClient::StopTunnel(std::string* error) {
  std::scoped_lock lock(mutex_);
  return StopTunnelLocked(error);
}

bool ControlClient::SetProvide(const std::string& mode, std::string* error) {
  std::scoped_lock lock(mutex_);
  ctl::SetProvideRequest req;
  req.mode = mode;
  const auto reply = CallLocked(ctl::Verb::SetProvide, nlohmann::json(req), error);
  if (!reply) return false;
  if (!ctl::ReplyOk(*reply)) {
    if (error) *error = ctl::ReplyError(*reply);
    return false;
  }
  return true;
}

std::optional<ctl::StatusReply> ControlClient::Status(std::string* error) {
  std::scoped_lock lock(mutex_);
  const auto reply = CallLocked(ctl::Verb::Status, nlohmann::json::object(), error);
  if (!reply || !ctl::ReplyOk(*reply)) {
    if (reply && error) *error = ctl::ReplyError(*reply);
    return std::nullopt;
  }
  return reply->get<ctl::StatusReply>();
}

bool ControlClient::LocationOverrideAvailable(bool* available, std::string* reason) {
  std::scoped_lock lock(mutex_);
  std::string error;
  const auto reply =
      CallLocked(ctl::Verb::LocationOverrideAvailable, nlohmann::json::object(), &error);
  if (!reply || !ctl::ReplyOk(*reply)) {
    if (available) *available = false;
    if (reason) *reason = reply ? ctl::ReplyError(*reply) : error;
    return false;
  }
  const auto parsed = reply->get<ctl::LocationOverrideAvailableReply>();
  if (available) *available = parsed.available;
  if (reason) *reason = parsed.reason;
  return true;
}

bool ControlClient::LocationOverrideWrite(double lat, double lon, double accuracyM) {
  std::scoped_lock lock(mutex_);
  ctl::LocationOverrideWriteRequest req;
  req.lat = lat;
  req.lon = lon;
  req.accuracy_m = accuracyM;
  std::string error;
  const auto reply = CallLocked(ctl::Verb::LocationOverrideWrite, nlohmann::json(req), &error);
  return reply && ctl::ReplyOk(*reply);
}

bool ControlClient::LocationOverrideClear() {
  std::scoped_lock lock(mutex_);
  std::string error;
  const auto reply =
      CallLocked(ctl::Verb::LocationOverrideClear, nlohmann::json::object(), &error);
  return reply && ctl::ReplyOk(*reply);
}

}  // namespace urnw
