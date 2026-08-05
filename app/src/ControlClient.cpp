// SPDX-License-Identifier: MPL-2.0
#include "ControlClient.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace urnw {
namespace {

// start_tunnel covers device creation + tun + route/DNS setup in the daemon;
// everything else answers immediately. One generous bound for all verbs.
constexpr time_t kReceiveTimeoutSeconds = 30;
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
}

bool ControlClient::ConnectLocked(std::string* error) {
  CloseLocked();
  const std::string path = SocketPath();
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

  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (error) *error = path + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  fd_ = fd;
  return true;
}

bool ControlClient::SendAllLocked(const std::string& data) {
  size_t sent = 0;
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
                                                             int64_t id) {
  if (fd_ < 0) return std::nullopt;
  if (!SendAllLocked(ctl::EncodeFrame(request))) return std::nullopt;
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
  return DaemonSessionState::Ok;
}

DaemonSessionState ControlClient::EnsureSessionLocked(std::string* error) {
  if (fd_ >= 0 && helloOk_) {
    lastState_ = DaemonSessionState::Ok;
    return lastState_;
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

std::optional<nlohmann::json> ControlClient::CallLocked(ctl::Verb verb, nlohmann::json payload,
                                                        std::string* error) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (EnsureSessionLocked(error) != DaemonSessionState::Ok) return std::nullopt;
    const int64_t id = nextId_++;
    if (auto reply = RoundTripLocked(ctl::MakeRequest(verb, id, payload), id)) {
      return reply;
    }
    // dead socket (daemon restarted?): reconnect once, then give up
    CloseLocked();
    lastState_ = DaemonSessionState::Unreachable;
    if (error) *error = "lost connection to the daemon";
  }
  return std::nullopt;
}

bool ControlClient::StartTunnel(const std::string& byJwt, const std::string& instanceId,
                                const std::string& appVersion, int* rpcPort,
                                std::string* error) {
  std::scoped_lock lock(mutex_);
  ctl::StartTunnelRequest req;
  req.by_jwt = byJwt;
  req.instance_id = instanceId;
  req.app_version = appVersion;
  const auto reply = CallLocked(ctl::Verb::StartTunnel, nlohmann::json(req), error);
  if (!reply) return false;
  if (!ctl::ReplyOk(*reply)) {
    if (error) *error = ctl::ReplyError(*reply);
    lastState_ = DaemonSessionState::Error;
    return false;
  }
  if (rpcPort) *rpcPort = reply->get<ctl::StartTunnelReply>().rpc_port;
  return true;
}

bool ControlClient::StopTunnel(std::string* error) {
  std::scoped_lock lock(mutex_);
  const auto reply = CallLocked(ctl::Verb::StopTunnel, nlohmann::json::object(), error);
  if (!reply) return false;
  if (!ctl::ReplyOk(*reply)) {
    if (error) *error = ctl::ReplyError(*reply);
    return false;
  }
  return true;
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
