// Where this urnetworkd is, for every command it tells a person to type: the
// recovery lines, --help, --diagnose, the self-test, the egress refusal and
// the DNS takeover's notes all name the binary by SelfPath().
//
// No package puts urnetworkd on $PATH. The deb, rpm and Arch packages install
// it at /usr/lib/urnetwork/urnetworkd, and the install tarball moves it to
// /usr/local/lib/urnetwork/urnetworkd on an immutable host, so a bare
// `urnetworkd --diagnose` is "command not found" on every one of them. The
// running binary's own path is right for every packaging format.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <unistd.h>

#include <climits>  // PATH_MAX
#include <cstddef>
#include <string>

namespace urnw {

// Where the packages install this binary (the unit's ExecStart): the answer
// when the running binary cannot say where it is.
inline constexpr const char* kInstalledDaemonPath = "/usr/lib/urnetwork/urnetworkd";

// /proc/self/exe FIRST, because what you are told to run must exist where it
// says. kInstalledDaemonPath is a guess (the tarball relocates the daemon) and
// argv[0] is whatever the caller typed; the kernel link is the one answer that
// is always the truth. Pure, for the unit suite: `exeLink` is what
// readlink(/proc/self/exe) gave ("" when it failed), `argv0` the process's
// argv[0], or nullptr where none is at hand.
inline std::string ResolveSelfPath(const std::string& exeLink, const char* argv0) {
  // A deleted-and-replaced binary reads back as "<path> (deleted)"; that is
  // not a runnable path, so fall through to the weaker answers.
  if (!exeLink.empty() && exeLink.find(" (deleted)") == std::string::npos) return exeLink;
  if (argv0 != nullptr && argv0[0] == '/') return argv0;
  return kInstalledDaemonPath;
}

// What /proc/self/exe links to, or "" when it cannot be read.
inline std::string ReadSelfExeLink() {
  char buf[PATH_MAX];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

// This binary's own path. The tunnel's messages have no argv and pass nullptr.
inline std::string SelfPath(const char* argv0 = nullptr) {
  return ResolveSelfPath(ReadSelfExeLink(), argv0);
}

}  // namespace urnw
