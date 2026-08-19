// SPDX-License-Identifier: MPL-2.0
#include "LogTailClient.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace urnw {
namespace {

// Deliberately SHORTER than ControlClient's 30 s. That bound exists for
// start_tunnel, which does device creation + tun + routes inline; log_tail
// answers out of a deque and returns immediately. This fetch rides the
// Developer page's 5 s poll on its serial bridge, so a stalled read must not
// wedge the whole poll (and every reliability rpc behind it) for half a minute.
constexpr time_t kReceiveTimeoutSeconds = 5;
constexpr time_t kSendTimeoutSeconds = 5;
constexpr size_t kMaxFrameBytes = 1 << 20;  // same 1 MiB protocol bound

void SetTimeout(int fd, int kind, time_t seconds) {
  timeval tv{};
  tv.tv_sec = seconds;
  ::setsockopt(fd, SOL_SOCKET, kind, &tv, sizeof(tv));
}

}  // namespace

LogTailClient::~LogTailClient() { Close(); }

void LogTailClient::SetLocalSdkVersion(std::string sdkVersion) {
  std::scoped_lock lock(mutex_);
  localSdkVersion_ = std::move(sdkVersion);
}

void LogTailClient::Close() {
  std::scoped_lock lock(mutex_);
  CloseLocked();
}

void LogTailClient::CloseLocked() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  helloOk_ = false;
  recvBuffer_.clear();
  // cursor_ is deliberately NOT reset: the socket is the transport, the cursor
  // is the conversation. A daemon restart is caught by the seq rewind instead.
}

void LogTailClient::Reset() {
  std::scoped_lock lock(mutex_);
  cursor_ = 0;
}

bool LogTailClient::ConnectLocked(std::string* error) {
  CloseLocked();
  // The SAME resolution ControlClient uses, so a dev run with
  // URNETWORK_CONTROL_SOCKET pointed at a temp path reaches the same daemon.
  const std::string path = ControlClient::SocketPath();
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
#ifdef SO_NOSIGPIPE
  {
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif
  SetTimeout(fd, SO_RCVTIMEO, kReceiveTimeoutSeconds);
  SetTimeout(fd, SO_SNDTIMEO, kSendTimeoutSeconds);

  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    // The errno is carried through verbatim. EACCES here is the "you are not in
    // the urnetwork group" case the installers leave behind — a different
    // problem from ENOENT ("the daemon is not running"), and the card renders
    // the daemon's own message rather than flattening both to a blank.
    if (error) *error = path + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  fd_ = fd;
  return true;
}

bool LogTailClient::SendAllLocked(const std::string& data) {
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

bool LogTailClient::ReadLineLocked(std::string& line) {
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
      return false;
    }
    recvBuffer_.append(buf, static_cast<size_t>(n));
  }
}

std::optional<nlohmann::json> LogTailClient::RoundTripLocked(const nlohmann::json& request,
                                                             int64_t id) {
  if (fd_ < 0) return std::nullopt;
  if (!SendAllLocked(ctl::EncodeFrame(request))) return std::nullopt;
  for (;;) {
    std::string line;
    if (!ReadLineLocked(line)) return std::nullopt;
    auto frame = ctl::DecodeFrame(line);
    if (!frame) return std::nullopt;
    // Skip anything that is not our reply: a future daemon may push unsolicited
    // events, and ignoring them keeps this forward-compatible.
    if (ctl::FrameId(*frame) == id) return frame;
  }
}

DaemonSessionState LogTailClient::HelloLocked(std::string* error, std::string* code) {
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
    const std::string message = ctl::ReplyError(*reply);
    if (error) *error = message.empty() ? "hello rejected" : message;
    const std::string replyCode = ctl::ReplyCode(*reply);
    if (code) *code = replyCode;
    CloseLocked();
    if (replyCode == ctl::kCodeClientProtocolTooOld) return DaemonSessionState::ClientTooOld;
    if (replyCode == ctl::kCodeSdkVersionMismatch) return DaemonSessionState::SdkMismatch;
    return DaemonSessionState::Error;
  }
  const auto helloReply = reply->get<ctl::HelloReply>();
  if (!ctl::ClientAcceptsDaemonProtocol(helloReply.protocol_version)) {
    if (error) {
      *error = "daemon protocol " + std::to_string(helloReply.protocol_version) +
               " below supported minimum " + std::to_string(ctl::kMinSupportedDaemonProtocol);
    }
    CloseLocked();
    return DaemonSessionState::DaemonTooOld;
  }
  // Enforced from THIS side too: a daemon predating the check accepted us above.
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

DaemonSessionState LogTailClient::EnsureSessionLocked(std::string* error, std::string* code) {
  if (fd_ >= 0 && helloOk_) return DaemonSessionState::Ok;
  if (!ConnectLocked(error)) return DaemonSessionState::Unreachable;
  return HelloLocked(error, code);
}

LogTailClient::FetchResult LogTailClient::Fetch(int maxLines) {
  std::scoped_lock lock(mutex_);
  FetchResult out;
  out.cursor = cursor_;

  for (int attempt = 0; attempt < 2; ++attempt) {
    out.error.clear();
    out.code.clear();
    out.state = EnsureSessionLocked(&out.error, &out.code);
    if (out.state != DaemonSessionState::Ok) return out;

    ctl::LogTailRequest request;
    request.cursor = cursor_;
    request.max_lines = maxLines;
    const int64_t id = nextId_++;
    // MakeRequest takes a ctl::Verb; log_tail has no enumerator yet (see
    // LogTailProtocol.hpp), so the payload carries the verb string directly.
    // The envelope shape is identical to MakeRequest's.
    nlohmann::json frame = nlohmann::json(request);
    frame["verb"] = ctl::kVerbLogTail;
    frame["id"] = id;

    const auto reply = RoundTripLocked(frame, id);
    if (!reply) {
      // Dead socket (the daemon restarted, or SIGTERM'd between polls):
      // reconnect once, then report Unreachable rather than an empty log.
      CloseLocked();
      out.state = DaemonSessionState::Unreachable;
      out.error = "lost connection to the daemon";
      continue;
    }
    if (!ctl::ReplyOk(*reply)) {
      // Includes an OLD daemon answering "unknown verb", and the gated policy's
      // log_access_denied. Both are stated, never rendered as "no lines".
      out.state = DaemonSessionState::Error;
      out.error = ctl::ReplyError(*reply);
      out.code = ctl::ReplyCode(*reply);
      return out;
    }

    const auto payload = reply->get<ctl::LogTailReply>();
    // The daemon's seq restarts at 1 with its process. If what came back begins
    // at or below the cursor we sent, this is a NEW daemon and the lines above
    // belong to a run that is over — the caller says so instead of silently
    // interleaving two processes' logs.
    // Two independent tells, because either can occur alone:
    //   * lines came back that begin AT OR BELOW the cursor we sent, or
    //   * the daemon's next_cursor went BACKWARDS past our cursor, which is the
    //     only tell available when the new process has not logged enough to
    //     reach our old seq yet (an empty tail after a restart). Guarding this
    //     on a non-empty reply, as it was, silently interleaved two processes'
    //     logs for as long as the new one stayed quiet.
    out.restarted = (!payload.lines.empty() && payload.lines.front().seq <= cursor_) ||
                    (cursor_ > 0 && payload.next_cursor < cursor_);
    cursor_ = payload.next_cursor;
    out.ok = true;
    out.lines = std::move(payload.lines);
    out.dropped = payload.dropped;
    out.cursor = cursor_;
    return out;
  }
  return out;
}

}  // namespace urnw
