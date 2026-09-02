// File-descriptor ownership at the native-to-Go tunnel-loop boundary.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <fcntl.h>

namespace urnw {

// Returns the descriptor that the Go IoLoop owns and eventually closes.
// The caller retains its independent descriptor, so asynchronous IoLoop
// retirement cannot close a different resource that reused the caller's fd.
inline int DuplicateIoLoopFd(int fd) { return ::fcntl(fd, F_DUPFD_CLOEXEC, 0); }

}  // namespace urnw
