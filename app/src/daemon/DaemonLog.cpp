// SPDX-License-Identifier: MPL-2.0
#include "daemon/DaemonLog.hpp"

#include <glib/gstdio.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace urnw {
namespace {

// Per-poll read budget. A daemon that has been logging hard for a minute must
// not stall the control loop draining its whole backlog in one tick — the
// offset advances, so the rest arrives on the next poll.
constexpr gint64 kMaxBytesPerPoll = 256 * 1024;

// On the FIRST attach only, start this far back from the end rather than at
// byte 0: enough to carry the session banner and the recent [rel] stream into
// the ring, without replaying a log file that has been growing for days.
// A rotation (a NEW file appearing) always starts at 0 — that file IS new.
constexpr gint64 kInitialTailBytes = 64 * 1024;

// A single line beyond this is a runaway (a serialized blob, a stack dump with
// no newlines): truncate rather than let one entry dominate the ring and the
// reply frame.
constexpr std::size_t kMaxLineBytes = 8 * 1024;

int64_t NowUnixMillis() {
  return static_cast<int64_t>(g_get_real_time() / 1000);
}

// Trailing \n / \r removed: the newline is a framing artifact of the call site
// (every existing fprintf carries one), never part of the message.
void StripEol(std::string& text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
}

gboolean OnSdkPollTick(gpointer) {
  DaemonLog::Instance().PollSdkLog();
  return G_SOURCE_CONTINUE;
}

}  // namespace

DaemonLog& DaemonLog::Instance() {
  // Function-local static: thread-safe initialization, and it is never
  // destroyed, which is deliberate — a breadcrumb emitted during static
  // destruction must not touch a dead ring.
  static DaemonLog* instance = new DaemonLog();
  return *instance;
}

void DaemonLog::AppendLocked(const char* source, std::string text) {
  if (text.size() > kMaxLineBytes) {
    text.resize(kMaxLineBytes);
    text += " ...[truncated]";
  }
  ctl::LogLine line;
  line.seq = ++lastSeq_;
  line.unix_ms = NowUnixMillis();
  line.source = source != nullptr ? source : ctl::kLogSourceDaemon;
  line.text = std::move(text);
  ring_.push_back(std::move(line));
  // The drop is not announced here — Tail() derives it from the seq of the
  // oldest surviving entry, so a client that was never attached still learns
  // that it is looking at a shortened log.
  while (ring_.size() > ctl::kDaemonLogRingCapacity) ring_.pop_front();
}

void DaemonLog::WriteText(const char* source, const std::string& text, bool mirror) {
  if (mirror) {
    // Verbatim, with the newline the ring entry does not keep: journald's copy
    // must remain byte-identical to what it received before this feature.
    std::fprintf(stderr, "%s\n", text.c_str());
  }
  std::lock_guard<std::mutex> lock(mutex_);
  // Split embedded newlines: one ring entry per line, so a multi-line
  // breadcrumb still reads as lines in the client's tail.
  std::size_t start = 0;
  for (;;) {
    const std::size_t nl = text.find('\n', start);
    std::string piece =
        text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    StripEol(piece);
    if (!piece.empty() || nl == std::string::npos) AppendLocked(source, std::move(piece));
    if (nl == std::string::npos) break;
    start = nl + 1;
    if (start >= text.size()) break;
  }
}

void DaemonLog::Writev(const char* source, const char* fmt, va_list args) {
  va_list copy;
  va_copy(copy, args);
  char stackBuffer[512];
  const int needed = std::vsnprintf(stackBuffer, sizeof(stackBuffer), fmt, args);
  std::string formatted;
  if (needed < 0) {
    formatted = "(log format error)";
  } else if (static_cast<std::size_t>(needed) < sizeof(stackBuffer)) {
    formatted.assign(stackBuffer, static_cast<std::size_t>(needed));
  } else {
    std::vector<char> heap(static_cast<std::size_t>(needed) + 1);
    std::vsnprintf(heap.data(), heap.size(), fmt, copy);
    formatted.assign(heap.data(), static_cast<std::size_t>(needed));
  }
  va_end(copy);
  StripEol(formatted);
  WriteText(source, formatted, /*mirror=*/true);
}

void DaemonLog::Writef(const char* source, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  Writev(source, fmt, args);
  va_end(args);
}

void DaemonLogf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  DaemonLog::Instance().Writev(ctl::kLogSourceDaemon, fmt, args);
  va_end(args);
}

ctl::LogTailReply DaemonLog::Tail(int64_t cursor, int maxLines) {
  ctl::LogTailReply reply;
  if (maxLines <= 0) maxLines = ctl::kLogTailDefaultMaxLines;
  if (maxLines > ctl::kLogTailMaxLines) maxLines = ctl::kLogTailMaxLines;
  if (cursor < 0) cursor = 0;

  std::lock_guard<std::mutex> lock(mutex_);
  // A cursor above our highest seq belongs to a PREVIOUS daemon process (seq
  // restarts at 1 with the process). Serving nothing forever would make the
  // card look merely idle after a daemon restart, which is the failure mode
  // this whole surface exists to prevent — so rewind to the oldest we hold. The
  // client detects the rewind because the first line it gets back carries a seq
  // at or below the cursor it sent, and says the daemon restarted.
  if (cursor > lastSeq_) cursor = 0;

  const int64_t oldest = ring_.empty() ? lastSeq_ + 1 : ring_.front().seq;
  if (cursor + 1 < oldest) reply.dropped = oldest - (cursor + 1);

  for (const ctl::LogLine& line : ring_) {
    if (line.seq <= cursor) continue;
    reply.lines.push_back(line);
    if (static_cast<int>(reply.lines.size()) >= maxLines) break;
  }
  reply.next_cursor =
      reply.lines.empty() ? std::max<int64_t>(cursor, oldest - 1) : reply.lines.back().seq;
  return reply;
}

// ---- the SDK glog tail ------------------------------------------------------

void DaemonLog::SetSdkLogDir(std::string dir) { sdkLogDir_ = std::move(dir); }

void DaemonLog::StartSdkLogPolling(unsigned intervalMs) {
  if (sdkPollSourceId_ != 0) return;  // idempotent
  if (intervalMs == 0) intervalMs = 1000;
  PollSdkLog();  // attach immediately so the session banner is not a tick late
  sdkPollSourceId_ = g_timeout_add(intervalMs, &OnSdkPollTick, nullptr);
}

void DaemonLog::StopSdkLogPolling() {
  if (sdkPollSourceId_ == 0) return;
  g_source_remove(sdkPollSourceId_);
  sdkPollSourceId_ = 0;
}

void DaemonLog::PollSdkLog() {
  if (sdkLogDir_.empty()) return;

  // Newest regular file in the directory — the same "the SDK exposes only the
  // DIRECTORY (urnet::getLogDir), so the newest regular file in it stands in"
  // stand-in SettingsPage::SaveLogsToFile uses. A real file-path accessor in
  // the binding would remove the guess on both sides at once.
  GDir* dir = g_dir_open(sdkLogDir_.c_str(), 0, nullptr);
  if (dir == nullptr) return;
  std::string newest;
  gint64 newestMtime = -1;
  guint64 newestInode = 0;
  while (const char* name = g_dir_read_name(dir)) {
    const std::string candidate = sdkLogDir_ + "/" + name;
    if (!g_file_test(candidate.c_str(), G_FILE_TEST_IS_REGULAR)) continue;
    GStatBuf info{};
    if (g_stat(candidate.c_str(), &info) != 0) continue;
    if (static_cast<gint64>(info.st_mtime) > newestMtime) {
      newestMtime = static_cast<gint64>(info.st_mtime);
      newest = candidate;
      newestInode = static_cast<guint64>(info.st_ino);
    }
  }
  g_dir_close(dir);
  if (newest.empty()) return;

  GStatBuf info{};
  if (g_stat(newest.c_str(), &info) != 0) return;
  const gint64 size = static_cast<gint64>(info.st_size);

  if (newest != sdkLogPath_ || newestInode != sdkLogInode_) {
    // First attach, or the SDK rotated to a new file.
    const bool firstAttach = !sdkAttached_;
    sdkLogPath_ = newest;
    sdkLogInode_ = newestInode;
    sdkPending_.clear();
    if (firstAttach && size > kInitialTailBytes) {
      sdkLogOffset_ = size - kInitialTailBytes;
      sdkSkipPartial_ = true;  // we landed mid-line
    } else {
      sdkLogOffset_ = 0;
      sdkSkipPartial_ = false;
    }
    sdkAttached_ = true;
  } else if (size < sdkLogOffset_) {
    // Truncated in place (logrotate copytruncate): start over rather than read
    // garbage from a stale offset.
    sdkLogOffset_ = 0;
    sdkPending_.clear();
    sdkSkipPartial_ = false;
  }

  if (size <= sdkLogOffset_) return;  // nothing new
  ReadSdkFileChunk();
}

void DaemonLog::ReadSdkFileChunk() {
  FILE* file = std::fopen(sdkLogPath_.c_str(), "rb");
  if (file == nullptr) return;
  if (std::fseek(file, static_cast<long>(sdkLogOffset_), SEEK_SET) != 0) {
    std::fclose(file);
    return;
  }
  std::vector<char> buffer(16 * 1024);
  gint64 consumed = 0;
  while (consumed < kMaxBytesPerPoll) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<gint64>(static_cast<gint64>(buffer.size()), kMaxBytesPerPoll - consumed));
    const std::size_t got = std::fread(buffer.data(), 1, want, file);
    if (got == 0) break;
    consumed += static_cast<gint64>(got);
    sdkPending_.append(buffer.data(), got);

    std::size_t start = 0;
    for (;;) {
      const std::size_t nl = sdkPending_.find('\n', start);
      if (nl == std::string::npos) break;
      std::string line = sdkPending_.substr(start, nl - start);
      start = nl + 1;
      StripEol(line);
      if (sdkSkipPartial_) {
        // The first fragment after a mid-file seek is half a line; emitting it
        // would put a truncated record in front of the user and let them read
        // it as the whole thing.
        sdkSkipPartial_ = false;
        continue;
      }
      if (line.empty()) continue;
      // mirror=false: these lines are already in the SDK's own file, and
      // echoing them to stderr would duplicate the whole SDK log into journald.
      WriteText(ctl::kLogSourceSdk, line, /*mirror=*/false);
    }
    sdkPending_.erase(0, start);
    // A "line" this long has no newline coming; flush it rather than grow the
    // pending buffer without bound.
    if (sdkPending_.size() > kMaxLineBytes) {
      std::string line = sdkPending_;
      sdkPending_.clear();
      if (sdkSkipPartial_) {
        sdkSkipPartial_ = false;
      } else {
        WriteText(ctl::kLogSourceSdk, line, /*mirror=*/false);
      }
    }
  }
  sdkLogOffset_ += consumed;
  std::fclose(file);
}

}  // namespace urnw
