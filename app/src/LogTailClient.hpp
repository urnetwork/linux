// LogTailClient — the GUI half of the `log_tail` verb.
//
// WHY THIS IS NOT A METHOD ON ControlClient. It should be, and folding it in is
// a ~10-line change (the wiring note in this task's return spells it out
// verbatim): ControlClient::CallLocked already does connect + hello + one
// reconnect retry, so `LogTail()` would be the same three-line shape as
// Status(). ControlClient.{hpp,cpp} were outside this change's ownership, so
// the transport is duplicated here instead, behind an interface narrow enough
// that the swap touches exactly one call site (DeveloperPage::ReadSnapshot).
//
// WHAT THE DUPLICATION COSTS, stated plainly rather than hidden: a SECOND
// connection to /run/urnetwork/control.sock, opened lazily on the first fetch
// and only while the Developer destination is polling (Advanced Mode + selected
// + presenting). It is authorized by the same SO_PEERCRED check as the first
// one and performs its own `hello`, so both the protocol-version and the exact
// SDK-version enforcement apply to it unchanged. It does NOT disturb tunnel
// ownership: ControlServer only assigns tunnelOwner_ inside HandleStartTunnel
// (daemon/ControlServer.cpp:454) and only clears it for the owning connection
// (285-295), and log_tail touches neither. It does hold one extra fd on the
// daemon side while the page is open.
//
// THE CURSOR LIVES HERE, not in the page. The paging cursor is a property of
// the conversation, and keeping it inside the one object that owns the socket
// means the UI thread and the page's bridge thread never share it. Reset()
// forgets it (a new device session starts a new reading); Close() drops the
// socket but KEEPS the cursor, so a reconnect resumes rather than replays.
//
// Thread-safe (one internal mutex), like ControlClient: the Developer page's
// serial FIFO bridge thread is the only caller today.
//
// No GTK, no glib, no SDK — plain sockets + the shared protocol headers, so
// this compiles anywhere the protocol tests do. The caller supplies the SDK
// version string (SetLocalSdkVersion), exactly as it does for ControlClient.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ControlClient.hpp"  // DaemonSessionState — the four distinct failures
#include "ControlProtocol.hpp"
#include "LogTailProtocol.hpp"

namespace urnw {

class LogTailClient {
 public:
  LogTailClient() = default;
  ~LogTailClient();

  LogTailClient(const LogTailClient&) = delete;
  LogTailClient& operator=(const LogTailClient&) = delete;

  // Our urnet::version(), carried in hello and EXACT-match enforced by the
  // daemon (ctl::SdkVersionsAgree). Unset fails closed as a mismatch — which is
  // the right direction: an unversioned pair must never be told the log is
  // simply empty.
  void SetLocalSdkVersion(std::string sdkVersion);

  // One round trip's worth of answer. `state` is ALWAYS meaningful; `ok` says
  // whether `lines` describe a real reading. Every failure is nameable — none
  // of them may reach the UI as an empty box.
  struct FetchResult {
    DaemonSessionState state = DaemonSessionState::Unreachable;
    bool ok = false;                 // the daemon answered ok:true
    std::vector<ctl::LogLine> lines; // NEW lines only, oldest first
    int64_t cursor = 0;              // where we are now (this object's cursor)
    int64_t dropped = 0;             // ring gap crossed by THIS reply; caller accumulates
    bool restarted = false;          // the daemon's seq rewound: a new process
    std::string error;               // the daemon's message, or the transport's
    std::string code;                // machine-readable rejection code, "" when none
  };

  // Blocking: connect (if needed) + hello (if needed) + one log_tail, with one
  // reconnect retry on a dead socket. NEVER call this on the GTK main loop.
  FetchResult Fetch(int maxLines = ctl::kLogTailDefaultMaxLines);

  // Forget the cursor — the next Fetch reads from the oldest line the daemon
  // still holds. For a new device session, not for a reconnect.
  void Reset();
  void Close();

 private:
  bool ConnectLocked(std::string* error);
  DaemonSessionState HelloLocked(std::string* error, std::string* code);
  DaemonSessionState EnsureSessionLocked(std::string* error, std::string* code);
  void CloseLocked();
  std::optional<nlohmann::json> RoundTripLocked(const nlohmann::json& request, int64_t id);
  bool SendAllLocked(const std::string& data);
  bool ReadLineLocked(std::string& line);

  std::mutex mutex_;
  int fd_ = -1;
  bool helloOk_ = false;
  int64_t nextId_ = 1;
  int64_t cursor_ = 0;
  std::string recvBuffer_;
  std::string localSdkVersion_;
};

}  // namespace urnw
