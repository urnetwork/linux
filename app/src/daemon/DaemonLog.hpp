// DaemonLog — urnetworkd's in-process log ring, and the only thing on this
// machine that can hand the unprivileged GUI the daemon's own diagnostics.
//
// TWO PRODUCERS, because they fail independently and a reader needs to tell
// them apart:
//
//   "daemon"  urnetworkd's own breadcrumbs — the 24 std::fprintf(stderr, ...)
//             sites across daemon/main.cpp, daemon/ControlServer.cpp and
//             daemon/TunnelHost.cpp ("[tunnel] start failed: %s",
//             "[tunnel] up (client=%s rpc=127.0.0.1:%d)", ...). DaemonLogf() is
//             a drop-in for those calls: it appends to the ring AND still
//             fprintf's to stderr verbatim, so journald's copy is not weakened
//             by this feature — it gains a second reader, it does not lose one.
//
//   "sdk"     the glog file the DeviceLocal writes under urnet::setLogDir()
//             (daemon/main.cpp:141 -> /var/log/urnetwork). This is where the
//             SESSION BANNER and the whole `[rel] event=<name> key=value`
//             grammar actually land — the stream the Windows README calls the
//             read half of the Advanced-Mode workflow. Tailed by polling the
//             newest regular file in the directory.
//
// WHY A RING AND NOT A FILE HANDED OVER. The log directory is 0700 root-owned
// by the unit and must stay that way; loosening it would widen who can read the
// daemon's forensics on every box to fix a UI gap on one. The ring is served
// through the control socket, which is ALREADY the authorization boundary for
// start_tunnel / stop_tunnel / set_provide / location_override_write.
//
// WRAPAROUND IS REPORTED, NOT SWALLOWED. Tail() computes how many lines were
// dropped before the first line it returns, and the client renders that as a
// stated gap. A log that silently shortens is worse than no log.
//
// THREADING. One mutex over the ring. Writef/DaemonLogf are callable from any
// thread (TunnelHost's IoLoop callbacks are not on the main loop). PollSdkLog
// and the g_timeout it installs run on the daemon's GMainLoop thread, the same
// thread ControlServer dispatches on.
//
// NO GTK, NO SDK: glib + libc only, so this links into urnetworkd without
// touching the "the daemon package installs without pulling GTK" rule.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdarg>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include <glib.h>

#include "LogTailProtocol.hpp"

namespace urnw {

class DaemonLog {
 public:
  // Process-wide. A singleton deliberately: the 24 breadcrumb sites are spread
  // across three translation units and threading a reference to all of them
  // would be a larger edit to files this change does not own, for no gain — the
  // ring is process state either way.
  static DaemonLog& Instance();

  DaemonLog(const DaemonLog&) = delete;
  DaemonLog& operator=(const DaemonLog&) = delete;

  // Append + mirror to stderr. `fmt` may carry a trailing newline (every
  // existing call site has one): stderr gets it exactly as written, the ring
  // entry does not. Embedded newlines split into separate ring entries so one
  // multi-line breadcrumb stays readable as lines.
  void Writef(const char* source, const char* fmt, ...)
      __attribute__((format(printf, 3, 4)));
  void Writev(const char* source, const char* fmt, va_list args);
  // Same, already-formatted. Does NOT mirror to stderr when `mirror` is false —
  // used by the SDK tail, whose lines are already in the SDK's own file and
  // would otherwise be duplicated into journald.
  void WriteText(const char* source, const std::string& text, bool mirror);

  // Serve everything with seq > cursor, oldest first, at most `maxLines`
  // (clamped to ctl::kLogTailMaxLines; a non-positive value means the default).
  // A cursor ABOVE our highest seq is treated as a cursor from a previous
  // daemon run — seq restarts with the process — and is reset to 0 rather than
  // answered with permanent silence.
  ctl::LogTailReply Tail(int64_t cursor, int maxLines);

  // The SDK's log directory (whatever was passed to urnet::setLogDir).
  void SetSdkLogDir(std::string dir);
  // One tail pass over the newest regular file in that directory. Cheap when
  // nothing changed (one g_dir scan + one stat). Safe to call before
  // SetSdkLogDir — it is then a no-op.
  void PollSdkLog();
  // Install a g_timeout driving PollSdkLog. Idempotent; the daemon's main loop
  // must exist. StopSdkLogPolling removes it (call before g_main_loop_unref).
  void StartSdkLogPolling(unsigned intervalMs = 1000);
  void StopSdkLogPolling();

 private:
  DaemonLog() = default;

  void AppendLocked(const char* source, std::string text);
  // Attach/advance over the newest file; called with mutex_ NOT held (it takes
  // it per appended line).
  void ReadSdkFileChunk();

  std::mutex mutex_;
  std::deque<ctl::LogLine> ring_;
  int64_t lastSeq_ = 0;

  // SDK tail state (main-loop thread only, except sdkLogDir_ which is written
  // once at startup before the loop runs).
  std::string sdkLogDir_;
  std::string sdkLogPath_;
  guint64 sdkLogInode_ = 0;
  gint64 sdkLogOffset_ = 0;
  std::string sdkPending_;      // trailing partial line, carried between polls
  bool sdkAttached_ = false;    // we have ever bound to a file
  bool sdkSkipPartial_ = false; // first read started mid-file: drop line 1
  guint sdkPollSourceId_ = 0;
};

// printf-compatible shorthand for the "daemon" source, so replacing an existing
// breadcrumb is a one-token edit:
//   std::fprintf(stderr, "[tunnel] up (client=%s)\n", id);
//   urnw::DaemonLogf(      "[tunnel] up (client=%s)\n", id);
void DaemonLogf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace urnw
