// The `log_tail` control verb — the wire half of the network-log surface
// (docs/parity/support-developer.md; the Windows README's Advanced-Mode
// workflow: "flip the knob on the developer screen, then read the session
// banner in the log").
//
// WHY THIS EXISTS AS ITS OWN HEADER. Everything below belongs in
// ControlProtocol.hpp beside the other payloads, and folding it in is the
// intended end state (see the wiring note in the task return). It lives here
// because ControlProtocol.hpp was outside this change's ownership: this file is
// purely ADDITIVE and includes that header, so both binaries and the protocol
// unit tests can take it with a single #include and nothing existing moves.
//
// WHY THE LOG NEEDS A VERB AT ALL. The DeviceLocal that emits every
// `[rel] event=<name> key=value` line and the session banner lives in
// urnetworkd, which sets its SDK log dir to /var/log/urnetwork —
// LogsDirectoryMode=0700, root-owned (app/packaging/urnetworkd.service:53) —
// and its own breadcrumbs go to stderr -> journald with no SyslogIdentifier.
// The unprivileged GUI cannot open(2) either one. Windows has the same process
// split but both halves sit under one ProgramData root an admin can read. So on
// Linux the ONLY way the app can show its own reliability stream is to ask the
// daemon for it over the socket that is already authenticated.
//
// WHY REQUEST/REPLY AND NOT A PUSHED EVENT. ControlClient is blocking
// request/reply with one internal mutex (ControlClient.hpp:15-16) and the
// Developer destination already polls at 5000 ms on a serial FIFO bridge
// thread. A cursor-paged tail rides that cadence with no new lifecycle
// machinery and no pushed-events milestone. `dropped` is what makes the poll
// honest across a ring wrap: a gap is STATED, never silently swallowed.
//
// ADDITIVE WITHIN PROTOCOL v1, on the same argument network_space_json already
// used (ControlProtocol.hpp:232-244): both halves ship from one pipeline, and
// the hello sdk_version EXACT match already refuses a mismatched pair before
// any other verb is reachable. A daemon that predates the verb answers
// "unknown verb" with ok:false, which the client renders as a stated failure
// rather than a blank.
//
// Same constraints as ControlProtocol.hpp: shared by both binaries and by the
// tests, so no GTK, no glib, no SDK — plain C++17 + nlohmann_json.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ControlProtocol.hpp"

namespace urnw::ctl {

// The verb string on the wire. Deliberately a STRING constant rather than an
// enumerator: adding an arm to ctl::Verb means editing ControlProtocol.hpp, and
// IsLogTailRequest() below lets the daemon dispatch this verb with a two-line
// insert until that fold-in happens. Once Verb::LogTail exists, this constant
// is what ToString(Verb::LogTail) must return.
inline constexpr const char* kVerbLogTail = "log_tail";

// Rejection code for the GATED policy (owner decision option B — see the
// return value). Defined here from the first version deliberately: the card's
// state machine must be able to render "refused" in words from day one, because
// a refusal bolted on later becomes a blank.
inline constexpr const char* kCodeLogAccessDenied = "log_access_denied";

// How many lines one round trip may carry. The default is the report's 200 (at
// a 5 s poll that keeps up with any plausible line rate); the hard cap bounds
// the reply frame well under ControlClient's 1 MiB kMaxFrameBytes even with
// pathological line lengths.
inline constexpr int kLogTailDefaultMaxLines = 200;
inline constexpr int kLogTailMaxLines = 1000;

// The daemon-side ring. 2000 entries is the report's figure: enough to hold a
// whole bring-up plus the session banner plus several minutes of [rel] traffic,
// small enough that the daemon's resident set does not move.
inline constexpr std::size_t kDaemonLogRingCapacity = 2000;

// The client-side accumulation cap. Matches the ring so a client that has been
// attached the whole time holds exactly what the daemon holds.
inline constexpr std::size_t kLogTailClientCap = 2000;

// LogLine.source. Two producers, distinguished because they fail independently:
// "daemon" is urnetworkd's own breadcrumbs (also still mirrored to stderr ->
// journald, verbatim, so the systemd copy is not weakened by this feature) and
// "sdk" is the glog file the DeviceLocal writes — which is where the session
// banner and the `[rel] event=` grammar actually land.
inline constexpr const char* kLogSourceDaemon = "daemon";
inline constexpr const char* kLogSourceSdk = "sdk";

struct LogLine {
  // Monotonic within ONE daemon process, starting at 1. It is the paging
  // cursor. It RESTARTS with the process — the client detects that by the
  // served seq going backwards and says so rather than interleaving two runs.
  int64_t seq = 0;
  int64_t unix_ms = 0;  // wall clock at append; 0 only if the clock failed
  std::string source;   // kLogSourceDaemon | kLogSourceSdk
  std::string text;     // one line, no trailing newline, never re-wrapped
};
inline void to_json(nlohmann::json& j, const LogLine& v) {
  j["seq"] = v.seq;
  j["unix_ms"] = v.unix_ms;
  j["source"] = v.source;
  j["text"] = v.text;
}
inline void from_json(const nlohmann::json& j, LogLine& v) {
  detail::Get(j, "seq", v.seq);
  detail::Get(j, "unix_ms", v.unix_ms);
  detail::Get(j, "source", v.source);
  detail::Get(j, "text", v.text);
}

struct LogTailRequest {
  // Serve everything with seq > cursor. 0 = "from the oldest you still hold".
  int64_t cursor = 0;
  int max_lines = kLogTailDefaultMaxLines;
};
inline void to_json(nlohmann::json& j, const LogTailRequest& v) {
  j["cursor"] = v.cursor;
  j["max_lines"] = v.max_lines;
}
inline void from_json(const nlohmann::json& j, LogTailRequest& v) {
  detail::Get(j, "cursor", v.cursor);
  detail::Get(j, "max_lines", v.max_lines);
}

struct LogTailReply {
  std::vector<LogLine> lines;  // oldest first
  // Pass this back as the next request's cursor. When `lines` is empty it is
  // the cursor the caller should keep using — never 0 unless nothing exists.
  int64_t next_cursor = 0;
  // Lines the ring dropped BEFORE the first line in this reply. Non-zero only
  // on the round trip that crosses the gap, so a client that wants a persistent
  // "N lines were dropped" marker must ACCUMULATE it. Stating the gap is the
  // point: a silently shortened log is exactly the failure this surface exists
  // to prevent.
  int64_t dropped = 0;
};
inline void to_json(nlohmann::json& j, const LogTailReply& v) {
  j["lines"] = v.lines;
  j["next_cursor"] = v.next_cursor;
  j["dropped"] = v.dropped;
}
inline void from_json(const nlohmann::json& j, LogTailReply& v) {
  if (auto it = j.find("lines"); it != j.end() && it->is_array()) {
    v.lines = it->get<std::vector<LogLine>>();
  }
  detail::Get(j, "next_cursor", v.next_cursor);
  detail::Get(j, "dropped", v.dropped);
}

// True when this decoded request frame is a log_tail. Lets the daemon dispatch
// the verb WITHOUT an arm in ctl::Verb (RequestVerb would answer Verb::Unknown
// for it): one `if` before the existing switch. Delete this the moment
// Verb::LogTail exists.
inline bool IsLogTailRequest(const nlohmann::json& j) {
  const auto it = j.find("verb");
  return it != j.end() && it->is_string() && it->get<std::string>() == kVerbLogTail;
}

}  // namespace urnw::ctl
