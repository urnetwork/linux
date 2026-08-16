// urnetworkd — the privileged URnetwork daemon (linux/MIGRATION.md workstream
// A). Runs as root under systemd, owns /dev/net/tun + routes + DNS via
// TunnelHost (DeviceLocal with the loopback mTLS device RPC enabled) and the
// privileged GeoClue write, and serves the unix control socket the
// unprivileged GUI drives (ControlServer).
//
// Deliberately NO GUI dependency of any kind — plain glib (GMainLoop), gio's
// glib, nlohmann_json and the SDK. The packaging depends on this: the daemon
// package must install without pulling GTK.
//
// systemd integration: Type=notify readiness is spoken directly over
// $NOTIFY_SOCKET (one datagram, "READY=1"), so there is no libsystemd
// dependency either. New-style daemons never self-daemonize; --foreground is
// accepted for interactive runs and simply skips the readiness notification.
//
// SPDX-License-Identifier: MPL-2.0
#include <fcntl.h>
#include <netinet/in.h>  // IPPROTO_TCP/IPPROTO_UDP for the self-test probes
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>  // PATH_MAX
#include <csignal>
#include <cstddef>  // offsetof
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <glib-unix.h>
#include <glib.h>

#include <urnetwork_sdk.hpp>

#include "LocationOverride.hpp"
#include "Tunnel.hpp"
#include "daemon/ControlServer.hpp"
#include "daemon/DaemonLog.hpp"
#include "daemon/TunnelHost.hpp"

// The release version, threaded in via the -Dapp_version meson option (the
// pipeline passes $VERSION). This is what the hello reply's daemon_version
// reports — the field the GUI's "daemon out of date" rendering names — so it
// must be the REAL release version, never a hardcoded constant.
#ifndef UR_APP_VERSION
#define UR_APP_VERSION "0.0.0"
#endif

namespace {

// Same bound the app used in-process: the data plane's memory target scales
// from it (SetMemoryLimit -> connect defaults).
constexpr int64_t kMemoryLimit = 64ll * 1024 * 1024;

// State (device identity + SDK storage) and log locations. systemd's
// StateDirectory=/LogsDirectory= set the env vars; the fallbacks match the
// unit, and URNETWORK_* allow an unprivileged dev run.
std::string StateDir() {
  if (const char* env = std::getenv("URNETWORK_STATE_DIR"); env && *env) return env;
  if (const char* env = std::getenv("STATE_DIRECTORY"); env && *env) return env;
  return "/var/lib/urnetwork";
}

std::string LogDir() {
  if (const char* env = std::getenv("URNETWORK_LOG_DIR"); env && *env) return env;
  if (const char* env = std::getenv("LOGS_DIRECTORY"); env && *env) return env;
  return "/var/log/urnetwork";
}

// Type=notify readiness without libsystemd: one datagram on $NOTIFY_SOCKET.
// '@'-prefixed (abstract) addresses per sd_notify(3). No-op when unset.
void NotifySystemdReady() {
  const char* path = std::getenv("NOTIFY_SOCKET");
  if (path == nullptr || path[0] == '\0') return;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const size_t len = std::strlen(path);
  if (len >= sizeof(addr.sun_path)) return;
  std::memcpy(addr.sun_path, path, len);
  socklen_t addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
  if (path[0] == '@') addr.sun_path[0] = '\0';  // abstract namespace
#if defined(SOCK_CLOEXEC)
  const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else  // macOS dev build: no SOCK_CLOEXEC; the fd lives three lines anyway
  const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
#endif
  if (fd < 0) return;
  static const char kReady[] = "READY=1";
  ::sendto(fd, kReady, sizeof(kReady) - 1, 0, reinterpret_cast<const sockaddr*>(&addr),
           addrLen);
  ::close(fd);
}

// Where this binary is installed (the unit's ExecStart). Printed in the
// recovery lines, so what the user is told to run is a path that exists on
// their machine; a dev run from a build tree prints its own argv[0] instead.
constexpr const char* kInstalledPath = "/usr/lib/urnetwork/urnetworkd";

// /proc/self/exe FIRST, because "what you are told to run must exist where it
// says". The tarball installer relocates the daemon on an immutable host
// (/usr/local/lib/urnetwork/urnetworkd on Bazzite, ExecStart rewritten to
// match), so kInstalledPath is a guess and argv[0] is whatever the caller
// typed. The kernel link is the only one of the three that is always the truth.
std::string SelfPath(const char* argv0) {
  char buf[PATH_MAX];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    // A deleted-and-replaced binary reads back as "<path> (deleted)"; that is
    // not a runnable path, so fall through to the weaker answers.
    if (std::strstr(buf, " (deleted)") == nullptr) return buf;
  }
  if (argv0 != nullptr && argv0[0] == '/') return argv0;
  return kInstalledPath;
}

// THE WAY BACK ONTO THE NETWORK, and the only thing on this machine that
// prints it. The kill switch is an nftables table, and nftables is not tied to
// process lifetime: a daemon that is SIGKILLed while armed leaves a machine
// that cannot reach anything — including any page explaining how to fix it. So
// the fix has to be reachable OFFLINE, from a surface a blocked user still has:
// `urnetworkd --help`, `urnetworkd --diagnose`, the journal (the floor logs it
// the moment it goes in), the log tail the GUI shows, and the comment block in
// the shipped unit (`systemctl cat urnetworkd`).
//
// The ORDER matters and is why this is prose and not one line: while the daemon
// is alive its reaper re-installs the table within seconds of anything deleting
// it (that is the tamper protection), so the daemon has to be stopped first.
void PrintRecovery(const char* argv0) {
  const std::string self = SelfPath(argv0);
  std::printf(
      "\n"
      "If this machine is cut off (the kill switch is armed, or urnetworkd died while a\n"
      "tunnel was up), this is the way back — no network access required:\n"
      "\n"
      "    sudo systemctl stop urnetworkd\n"
      "    sudo %s --revert\n"
      "\n"
      "  --revert removes the firewall table, the policy rules, the capture routes, any\n"
      "  /etc/resolv.conf takeover and the armed marker, so the next start comes up open.\n"
      "  With no systemd and no working daemon binary, the firewall half alone is:\n"
      "\n"
      "    %s\n"
      "\n"
      "  Stop the daemon FIRST: while it is running it re-installs its own ruleset within\n"
      "  seconds of anything removing it. With the daemon running, disconnecting in the\n"
      "  app is the normal way to lift the kill switch, and it always works — the app talks\n"
      "  to the daemon over a unix socket, which no firewall rule here can block.\n",
      self.c_str(), urnw::NetFilter::RecoveryCommand());
}

// ============================================================================
// --selftest-egress — DOES THE cgroup-BPF SOCKET MARKER WORK ON THIS KERNEL?
// ============================================================================
//
// The change this daemon now depends on for R4 self-exclusion is a
// four-instruction BPF_PROG_TYPE_CGROUP_SOCK program attached at
// BPF_CGROUP_INET_SOCK_CREATE (Tunnel.cpp, EgressSocketMarker). Until this mode
// existed, that program had never been loaded, attached or executed on any
// kernel: the only way to find out was to bring a tunnel up, and the last time
// this daemon's own packets went into its own tun it moved 3.38 Tb in forty
// minutes on the owner's machine. "It should work" is exactly the kind of claim
// that shipped the storm, so here is the way to turn it into a measurement.
//
// WHAT IT DOES
//   1. resolves this process's own cgroup v2 directory
//   2. mkdir()s ONE temporary cgroup underneath it, named urnw-selftest-<pid>
//   3. fork()s, moves the child into that cgroup (write "0" to cgroup.procs),
//      and re-exec()s /proc/self/exe with the internal child flag
//   4. the child runs the REAL EgressSocketMarker::Attach() — the same load,
//      the same attach, the same SO_MARK read-back the tunnel path uses — and
//      then reads SO_MARK off fresh UDP/TCP, v4/v6 sockets of its own
//   5. the child exits (which detaches the program), the parent reaps it and
//      removes the temporary cgroup on EVERY path: success, failure, exception,
//      SIGINT/SIGTERM/SIGHUP, and a 60 s watchdog alarm
//
// WHAT IT DOES NOT DO, and this is checkable by reading it: no tun device, no
// `ip route`/`ip rule`, no nftables of any kind, no DNS change, no packet on
// any wire (a socket is created and read with getsockopt, never connected and
// never sent on), no SDK, no control socket, no change to the daemon's own
// cgroup or to any cgroup this program did not create. The only kernel state it
// creates is one empty cgroup directory that it removes again.
//
// WHY A CHILD IN A TEMPORARY CGROUP RATHER THAN THIS PROCESS IN ITS OWN. Two
// reasons, both about blast radius: a program attached to the caller's real
// cgroup would mark the sockets of every process in it (under `sudo` from a
// terminal that is the login session's scope, which is the user's shell and
// everything they launch from it), and a process that has been moved between
// cgroups cannot be moved back to where systemd thinks it is. A child in a
// disposable leaf cgroup can only ever affect itself, and it exits.
//
// The verdict is printed in plain words and is also the exit status, so it can
// be used from a script: 0 = the mechanism works, 1 = it does not (and the
// output says which of load/attach/mark failed), 2 = the test could not be run
// at all (no answer either way — not root, no cgroup v2, mkdir refused).

constexpr const char kSelftestChildFlag[] = "--selftest-egress-child";
// Part of the contract, not decoration: the child refuses to attach anything to
// a cgroup whose leaf name does not start with this, so the internal flag
// cannot be turned into "attach a BPF program to an arbitrary cgroup".
constexpr const char kSelftestPrefix[] = "urnw-selftest-";
constexpr const char kCgroupRoot[] = "/sys/fs/cgroup/";

enum SelftestCode : int {
  kSelftestOk = 0,
  kSelftestMechanismFailed = 1,
  kSelftestNoAnswer = 2,        // the test could not run; nothing was proven
  kSelftestChildBadArgs = 9,    // the child was handed a path it will not touch
  kSelftestJoinFailed = 10,     // the child never made it into the test cgroup
  kSelftestLoadFailed = 11,     // bpf(BPF_PROG_LOAD) refused the program
  kSelftestAttachFailed = 12,   // bpf(BPF_PROG_ATTACH) refused the cgroup
  kSelftestMarkFailed = 13,     // loaded and attached, but the mark never lands
  kSelftestUnknownFailure = 19, // Attach() failed in a way not classified below
  kSelftestExecFailed = 20,     // execv(/proc/self/exe) failed in the child
};

// Open a socket of the given kind and report the SO_MARK the kernel gave it.
// NOTHING is sent: the socket is created, asked one question, and closed. This
// is the same read-back EgressSocketMarker uses; it is repeated here across the
// socket kinds the SDK actually opens, because a marker that covered UDP but
// not TCP would be a fresh way to leak.
bool ReadNewSocketMark(int domain, int type, int protocol, uint32_t* mark,
                       std::string* error) {
  const int fd = ::socket(domain, type | SOCK_CLOEXEC, protocol);
  if (fd < 0) {
    if (error != nullptr) *error = std::string("socket(): ") + std::strerror(errno);
    return false;
  }
  int value = 0;
  socklen_t len = sizeof(value);
  const int rc = ::getsockopt(fd, SOL_SOCKET, SO_MARK, &value, &len);
  const int saved = errno;
  ::close(fd);
  if (rc != 0) {
    if (error != nullptr) *error = std::string("getsockopt(SO_MARK): ") + std::strerror(saved);
    return false;
  }
  if (mark != nullptr) *mark = static_cast<uint32_t>(value);
  return true;
}

// "label ......." padded to a fixed width, so every step's verdict lands in the
// same column no matter how the labels are edited later. A report whose columns
// wander is a report people skim instead of read.
std::string Dotted(const std::string& label) {
  constexpr size_t kWidth = 35;
  std::string out = label + " ";
  while (out.size() < kWidth) out += '.';
  return out;
}

int CgroupLevel(const std::string& path) {
  if (path.empty()) return 0;
  int level = 1;
  for (const char c : path) {
    if (c == '/') ++level;
  }
  return level;
}

// WHICH STEP FAILED — the thing the owner actually needs from a failure.
// EgressSocketMarker::Attach() reports one string for three syscalls, so this
// maps its wording back onto the step. The raw text is ALWAYS printed as well,
// so if these prefixes ever drift the user still sees the true error and only
// the label is wrong.
//
// TODO(egress): give EgressSocketMarker::Attach() an out-parameter naming the
// failed step (load/attach/prove) so this string matching can be deleted. That
// is a Tunnel.hpp change and this file does not own it.
int ClassifySelftestFailure(const std::string& detail) {
  const auto has = [&](const char* needle) {
    return detail.find(needle) != std::string::npos;
  };
  if (has("BPF_PROG_LOAD")) return kSelftestLoadFailed;
  if (has("BPF_PROG_ATTACH") || has("could not open")) return kSelftestAttachFailed;
  if (has("came back with mark") || has("could not be proven")) return kSelftestMarkFailed;
  if (has("no usable cgroup v2")) return kSelftestJoinFailed;
  return kSelftestUnknownFailure;
}

// ---- the child half: runs INSIDE the temporary cgroup ----------------------

int RunSelftestEgressChild(const std::string& cgroupPath) {
  // This process was re-exec()d after being moved into the cgroup named on the
  // command line. Refuse anything that is not the kind of cgroup our own parent
  // half creates: an internal flag that attaches BPF programs to arbitrary
  // cgroups would be a worse thing to own than the bug it diagnoses.
  const std::string::size_type slash = cgroupPath.rfind('/');
  const std::string leaf =
      slash == std::string::npos ? cgroupPath : cgroupPath.substr(slash + 1);
  if (cgroupPath.empty() || cgroupPath.front() == '/' ||
      cgroupPath.find("..") != std::string::npos ||
      leaf.rfind(kSelftestPrefix, 0) != 0) {
    std::fprintf(stderr,
                 "urnetworkd %s: refusing '%s' — this flag is internal to "
                 "--selftest-egress and only ever operates on a %s* cgroup it created\n",
                 kSelftestChildFlag, cgroupPath.c_str(), kSelftestPrefix);
    return kSelftestChildBadArgs;
  }
  if (!urnw::CgroupV2PathExists(cgroupPath)) {
    std::fprintf(stderr, "urnetworkd %s: %s%s does not exist\n", kSelftestChildFlag,
                 kCgroupRoot, cgroupPath.c_str());
    return kSelftestChildBadArgs;
  }

  // STEP 3 — did the move actually take? Without this check a failed move would
  // surface below as "the program did not mark my socket", i.e. as a false
  // accusation against the kernel. /proc/self/cgroup is the kernel's own answer
  // to "which cgroup am I in".
  const urnw::CgroupRef mine = urnw::SelfCgroupV2();
  if (!mine.valid || mine.path != cgroupPath) {
    std::printf("  step 3  %s FAILED\n"
                "          expected 0::/%s, /proc/self/cgroup says 0::/%s\n",
                Dotted("child joined the test cgroup").c_str(), cgroupPath.c_str(),
                mine.valid ? mine.path.c_str() : "<none>");
    return kSelftestJoinFailed;
  }
  std::printf("  step 3  %s ok    pid %d is in 0::/%s\n",
              Dotted("child joined the test cgroup").c_str(), static_cast<int>(::getpid()),
              cgroupPath.c_str());

  urnw::CgroupRef ref;
  ref.valid = true;
  ref.path = cgroupPath;
  ref.level = CgroupLevel(cgroupPath);

  // STEPS 4-6 — THE REAL THING. Not a copy of the program, not a re-derivation
  // of the instructions: the same EgressSocketMarker::Attach() the tunnel path
  // calls, which loads, attaches and then proves itself with a fresh socket.
  // If this file held its own copy of those four instructions, a passing
  // self-test would only prove that the copy works.
  urnw::EgressSocketMarker marker;
  std::string error;
  if (!marker.Attach(ref, urnw::kEgressMark, &error)) {
    const int code = ClassifySelftestFailure(error);
    const char* step = code == kSelftestLoadFailed     ? "4  bpf(BPF_PROG_LOAD)"
                       : code == kSelftestAttachFailed ? "5  bpf(BPF_PROG_ATTACH)"
                       : code == kSelftestMarkFailed   ? "6  SO_MARK read-back"
                                                       : "?  EgressSocketMarker::Attach";
    std::printf("  step %s ... FAILED\n", step);
    std::printf("          %s\n", error.c_str());
    return code;
  }
  std::printf("  step 4  %s ok    the verifier accepted the 4-instruction program\n",
              Dotted("bpf(BPF_PROG_LOAD)").c_str());
  std::printf("  step 5  %s ok    BPF_CGROUP_INET_SOCK_CREATE, BPF_F_ALLOW_MULTI\n",
              Dotted("bpf(BPF_PROG_ATTACH)").c_str());
  std::printf("  step 6  %s ok    %s\n", Dotted("SO_MARK on a fresh socket").c_str(),
              marker.detail().c_str());

  // STEP 7 — the coverage question. The SDK opens TCP (platform API, DoH,
  // TURN) and UDP (QUIC/WebRTC/STUN), over v4 and v6. sock_create runs for all
  // of them, but a claim like that is exactly what this mode exists to stop
  // making without evidence.
  struct Probe {
    int domain;
    int type;
    int proto;
    const char* label;
    bool required;
  };
  static const Probe kProbes[] = {
      {AF_INET, SOCK_DGRAM, IPPROTO_UDP, "AF_INET  SOCK_DGRAM ", true},
      {AF_INET, SOCK_STREAM, IPPROTO_TCP, "AF_INET  SOCK_STREAM", true},
      {AF_INET6, SOCK_DGRAM, IPPROTO_UDP, "AF_INET6 SOCK_DGRAM ", false},
      {AF_INET6, SOCK_STREAM, IPPROTO_TCP, "AF_INET6 SOCK_STREAM", false},
  };
  bool allRequiredMarked = true;
  std::printf("  step 7  the same question, one socket kind at a time:\n");
  for (const Probe& probe : kProbes) {
    uint32_t got = 0;
    std::string probeError;
    if (!ReadNewSocketMark(probe.domain, probe.type, probe.proto, &got, &probeError)) {
      std::printf("            %s  n/a   (%s)%s\n", probe.label, probeError.c_str(),
                  probe.required ? "  <-- REQUIRED" : "");
      if (probe.required) allRequiredMarked = false;
      continue;
    }
    const bool marked = got == urnw::kEgressMark;
    std::printf("            %s  0x%08x  %s\n", probe.label, got,
                marked ? "marked" : "NOT MARKED  <-- the program did not run for this kind");
    if (!marked && probe.required) allRequiredMarked = false;
  }
  if (!allRequiredMarked) return kSelftestMarkFailed;

  // The destructor detaches and closes both fds; the cgroup the parent removes
  // would drop the attachment anyway. Both, deliberately: this is the shape the
  // daemon relies on when a tunnel stops.
  return kSelftestOk;
}

// ---- the parent half: sets the stage and cleans up unconditionally ---------

// Async-signal-safe cleanup state. The path is a fixed buffer because a signal
// handler may not touch std::string.
char g_selftestCgroupDir[PATH_MAX] = {0};
volatile sig_atomic_t g_selftestChildPid = 0;

// rmdir() on a cgroup that still holds a task returns EBUSY, and a just-reaped
// task can linger for a moment, so this retries for up to half a second. An
// empty cgroup directory is inert — it holds nothing and affects nothing — but
// leaving one behind would still be litter in someone else's hierarchy.
void RemoveSelftestCgroupVerbose() {
  if (g_selftestCgroupDir[0] == '\0') return;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (::rmdir(g_selftestCgroupDir) == 0 || errno == ENOENT) {
      g_selftestCgroupDir[0] = '\0';
      return;
    }
    const timespec ts{0, 10 * 1000 * 1000};  // 10 ms
    ::nanosleep(&ts, nullptr);
  }
  std::fprintf(stderr,
               "urnetworkd --selftest-egress: could not remove the temporary cgroup %s (%s).\n"
               "  It is empty and does nothing, but to be rid of it:  sudo rmdir %s\n",
               g_selftestCgroupDir, std::strerror(errno), g_selftestCgroupDir);
}

// Destructor-driven cleanup for the ordinary paths (return, early return).
struct SelftestCgroupGuard {
  ~SelftestCgroupGuard() { RemoveSelftestCgroupVerbose(); }
};

// ...and for the ones a destructor never sees: ^C, SIGTERM, a closed terminal,
// and the watchdog alarm that covers a child which never exits. Only
// async-signal-safe calls here (kill, rmdir, nanosleep, write, _exit).
void SelftestSignalCleanup(int) {
  if (g_selftestChildPid > 0) ::kill(static_cast<pid_t>(g_selftestChildPid), SIGKILL);
  for (int attempt = 0; attempt < 100 && g_selftestCgroupDir[0] != '\0'; ++attempt) {
    if (::rmdir(g_selftestCgroupDir) == 0 || errno == ENOENT) break;
    const timespec ts{0, 10 * 1000 * 1000};
    ::nanosleep(&ts, nullptr);
  }
  static const char kMsg[] =
      "\nurnetworkd --selftest-egress: aborted (signal or 60s timeout); the temporary cgroup "
      "was removed. Nothing else was changed.\n";
  const ssize_t ignored = ::write(STDERR_FILENO, kMsg, sizeof(kMsg) - 1);
  static_cast<void>(ignored);
  ::_exit(130);
}

int RunSelftestEgress(const char* argv0) {
  utsname host{};
  ::uname(&host);
  std::printf(
      "urnetworkd %s — egress socket-marker self-test\n"
      "kernel %s %s\n"
      "\n"
      "Question: on THIS kernel, does a cgroup-BPF program attached at\n"
      "BPF_CGROUP_INET_SOCK_CREATE actually put fwmark 0x%08x on a socket at the moment\n"
      "it is created — before connect() chooses a route and a source address?\n"
      "That is the whole mechanism the daemon now relies on to keep its own traffic out\n"
      "of its own tunnel.\n"
      "\n"
      "This test starts NO tunnel and touches NO networking: no tun device, no routes,\n"
      "no policy rules, no nftables, no DNS, and not one packet on any wire. It creates\n"
      "one temporary cgroup, runs a child inside it, and removes it again.\n"
      "\n",
      UR_APP_VERSION, host.sysname, host.release, urnw::kEgressMark);

  // STEP 1 — where are we? Everything below hangs off this.
  const urnw::CgroupRef cgroup = urnw::SelfCgroupV2();
  if (!cgroup.valid) {
    std::printf(
        "  step 1  %s FAILED\n"
        "          /proc/self/cgroup gives this process no usable cgroup v2 path: either there\n"
        "          is no unified (0::) hierarchy on this host at all, or the 0:: line is the\n"
        "          bare root \"/\", which is what a container in its own cgroup NAMESPACE sees.\n"
        "          Either way neither the BPF marker nor the nftables cgroup match can name\n"
        "          this process here, and nothing about the program itself was measured.\n",
        Dotted("this process's cgroup v2").c_str());
    return kSelftestNoAnswer;
  }
  if (!urnw::CgroupV2PathExists(cgroup.path)) {
    std::printf("  step 1  %s FAILED\n"
                "          /proc/self/cgroup says 0::/%s but %s%s does not exist — a cgroup\n"
                "          namespace is in the way (a container), so the path is relative to a\n"
                "          root this process cannot see.\n",
                Dotted("this process's cgroup v2").c_str(), cgroup.path.c_str(), kCgroupRoot,
                cgroup.path.c_str());
    return kSelftestNoAnswer;
  }
  std::printf("  step 1  %s ok    0::/%s (level %d)\n",
              Dotted("this process's cgroup v2").c_str(), cgroup.path.c_str(), cgroup.level);

  // STEP 2 — privilege. bpf(BPF_PROG_LOAD) needs CAP_BPF (CAP_SYS_ADMIN before
  // 5.8), BPF_PROG_ATTACH on a cgroup needs CAP_NET_ADMIN, and mkdir under
  // /sys/fs/cgroup needs write access to a directory root owns. There is no
  // unprivileged version of this measurement, so say so precisely instead of
  // failing three steps later with EPERM.
  if (::geteuid() != 0) {
    std::printf("  step 2  %s FAILED\n"
                "          loading a BPF program needs CAP_BPF, attaching it to a cgroup needs\n"
                "          CAP_NET_ADMIN, and creating the temporary cgroup needs write access\n"
                "          to %s%s. This is a read-only measurement, but it is a privileged one.\n"
                "\n"
                "          Run:  sudo %s --selftest-egress\n"
                "\n"
                "          `%s --diagnose` needs no root and reports the host preflight, but it\n"
                "          cannot answer this question: only loading the program can.\n",
                Dotted("privileges").c_str(), kCgroupRoot, cgroup.path.c_str(),
                SelfPath(argv0).c_str(), SelfPath(argv0).c_str());
    return kSelftestNoAnswer;
  }
  std::printf("  step 2  %s ok    running as root\n", Dotted("privileges").c_str());

  // THE CONTROL. This process is NOT in the test cgroup and never will be, so
  // its sockets must come back unmarked — before the program is attached and
  // after it is gone. Without this the child's 0x55524e57 would only prove that
  // something on this machine sets that mark, not that our program did it.
  uint32_t before = 0;
  std::string controlError;
  const bool haveBefore = urnw::EgressSocketMarker::SelfSocketMark(&before, &controlError);
  if (haveBefore) {
    std::printf("  control %s 0x%08x %s\n", Dotted("this process, outside the cgroup").c_str(),
                before,
                before == urnw::kEgressMark
                    ? "(already marked! a marker is attached to an ancestor cgroup — the daemon's\n"
                      "          own, if you are running this from inside urnetworkd.service)"
                    : "(unmarked, as it should be)");
  } else {
    std::printf("  control %s unavailable (%s)\n",
                Dotted("this process, outside the cgroup").c_str(), controlError.c_str());
  }

  // STEP 2b — the temporary cgroup. Under our own, so it inherits whatever
  // limits and delegation already apply to us and creates no new top-level
  // hierarchy.
  const std::string relative =
      cgroup.path + "/" + kSelftestPrefix + std::to_string(static_cast<long>(::getpid()));
  const std::string full = std::string(kCgroupRoot) + relative;
  if (full.size() >= sizeof(g_selftestCgroupDir)) {
    std::printf("  step 2b %s FAILED\n"
                "          the path would be %zu bytes, longer than this program's fixed\n"
                "          cleanup buffer; refusing to create something it cannot promise to\n"
                "          remove from a signal handler.\n",
                Dotted("temporary cgroup").c_str(), full.size());
    return kSelftestNoAnswer;
  }
  // The errno that matters is mkdir's own — checking with access() afterwards
  // would report ENOENT for every cause and lose the EROFS/EACCES that tells
  // the user WHY.
  int created = ::mkdir(full.c_str(), 0755) == 0 ? 0 : errno;
  if (created == EEXIST) {
    // Ours by naming convention and by pid: a leftover from a run that was
    // killed between mkdir and cleanup. An empty cgroup is safe to remove.
    ::rmdir(full.c_str());
    created = ::mkdir(full.c_str(), 0755) == 0 ? 0 : errno;
  }
  if (created != 0) {
    const int saved = created;
    std::printf("  step 2b %s FAILED\n"
                "          mkdir %s: %s\n",
                Dotted("temporary cgroup").c_str(), full.c_str(), std::strerror(saved));
    if (saved == EROFS) {
      std::printf("          /sys/fs/cgroup is mounted read-only here (a container, usually).\n");
    } else if (saved == EACCES || saved == EPERM) {
      std::printf("          the cgroup directory above is not writable even as root, which\n"
                  "          usually means a cgroup v1 hybrid mount or a delegation boundary.\n");
    }
    return kSelftestNoAnswer;
  }
  std::memcpy(g_selftestCgroupDir, full.c_str(), full.size() + 1);
  SelftestCgroupGuard guard;  // removes it on every ordinary return below
  std::signal(SIGINT, &SelftestSignalCleanup);
  std::signal(SIGTERM, &SelftestSignalCleanup);
  std::signal(SIGHUP, &SelftestSignalCleanup);
  std::signal(SIGALRM, &SelftestSignalCleanup);
  std::printf("  step 2b %s ok    created %s\n", Dotted("temporary cgroup").c_str(),
              full.c_str());

  // STEP 3+ happen in a child, because a process that has been moved into
  // another cgroup cannot be moved back to the one systemd believes it is in,
  // and because a marked socket must never be able to belong to this process.
  // Everything the child needs is built BEFORE the fork: between fork() and
  // execv() only async-signal-safe calls are legal (this binary links a Go
  // runtime, so a malloc in the forked child could deadlock outright).
  const std::string procsPath = full + "/cgroup.procs";
  std::string exePath = SelfPath(argv0);
  std::string childFlag = kSelftestChildFlag;
  std::string childArg = relative;
  char* childArgv[] = {exePath.data(), childFlag.data(), childArg.data(), nullptr};

  std::fflush(stdout);
  std::fflush(stderr);
  const pid_t child = ::fork();
  if (child < 0) {
    std::printf("  step 3  %s FAILED  %s\n", Dotted("fork()").c_str(), std::strerror(errno));
    return kSelftestNoAnswer;
  }
  if (child == 0) {
    // "0" means "the writing process" to cgroup_procs_write(), so this needs no
    // formatting and no allocation.
    const int fd = ::open(procsPath.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) ::_exit(kSelftestJoinFailed);
    static const char kSelf[] = "0\n";
    const ssize_t written = ::write(fd, kSelf, sizeof(kSelf) - 1);
    ::close(fd);
    if (written != static_cast<ssize_t>(sizeof(kSelf) - 1)) ::_exit(kSelftestJoinFailed);
    // /proc/self/exe rather than a searched name: this must be THIS binary, not
    // whatever else is called urnetworkd on this machine.
    ::execv("/proc/self/exe", childArgv);
    ::_exit(kSelftestExecFailed);
  }
  g_selftestChildPid = child;
  ::alarm(60);  // a child that wedges must not wedge the cleanup

  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno == EINTR) continue;
    std::printf("  step 3  %s FAILED  %s\n", Dotted("waitpid()").c_str(),
                std::strerror(errno));
    g_selftestChildPid = 0;
    ::alarm(0);
    return kSelftestNoAnswer;
  }
  g_selftestChildPid = 0;
  ::alarm(0);

  int code = kSelftestUnknownFailure;
  if (WIFEXITED(status)) {
    code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    std::printf("          the child was killed by signal %d\n", WTERMSIG(status));
    code = kSelftestUnknownFailure;
  }

  // THE CONTROL, AGAIN, now that the program is gone with the child.
  uint32_t after = 0;
  if (urnw::EgressSocketMarker::SelfSocketMark(&after, nullptr)) {
    std::printf("  control %s 0x%08x %s\n", Dotted("this process, after the test").c_str(),
                after,
                after == urnw::kEgressMark ? "(still marked — see the note above)"
                                           : "(unmarked)");
  }

  std::printf("\n");
  switch (code) {
    case kSelftestOk:
      std::printf(
          "VERDICT: THE MECHANISM WORKS ON THIS KERNEL.\n"
          "  A socket created inside the test cgroup came back carrying 0x%08x.\n"
          "%s"
          "  That is the claim R4 rests on, measured rather than assumed: the daemon's own\n"
          "  sockets are marked BEFORE connect(), so their route lookup never reaches the\n"
          "  capture table and never binds the tunnel's own source address.\n"
          "  It does NOT prove the daemon has the program attached RIGHT NOW; that is a\n"
          "  different question, and its answer is the `[tunnel] egress:` line the daemon\n"
          "  logs when a tunnel comes up:  journalctl -u urnetworkd | grep 'egress'\n",
          urnw::kEgressMark,
          // Only claim the negative control when it actually came back negative:
          // run from INSIDE urnetworkd.service with the daemon's own marker
          // attached to an ancestor cgroup, this process's sockets are marked
          // too, and saying otherwise would be the same kind of unearned green
          // check this mode exists to replace.
          (haveBefore && before != urnw::kEgressMark)
              ? "  An identical socket created by this process, outside that cgroup, did not.\n"
              : "  (This process's own sockets could not serve as a negative control here —\n"
                "  see the control line above.)\n");
      break;
    case kSelftestJoinFailed:
      std::printf("VERDICT: NO ANSWER — the child never got into the test cgroup, so nothing\n"
                  "  about the BPF program was measured. The failure is cgroup plumbing, not\n"
                  "  the marker.\n");
      code = kSelftestNoAnswer;
      break;
    case kSelftestLoadFailed:
      std::printf("VERDICT: THE MECHANISM DOES NOT WORK HERE — bpf(BPF_PROG_LOAD) FAILED.\n"
                  "  The kernel would not accept the program at all. Usual causes, in order:\n"
                  "  CONFIG_CGROUP_BPF=n or CONFIG_BPF_SYSCALL=n, an SELinux/AppArmor policy\n"
                  "  denying bpf() to this domain (check `ausearch -m avc` for bpf), or a\n"
                  "  seccomp filter on the unit. Any verifier text is printed above verbatim.\n"
                  "  The daemon must NOT be run with a tunnel on this host until this passes:\n"
                  "  the nftables cgroup rule alone cannot repair a source address connect()\n"
                  "  has already chosen.\n");
      break;
    case kSelftestAttachFailed:
      std::printf("VERDICT: THE MECHANISM DOES NOT WORK HERE — bpf(BPF_PROG_ATTACH) FAILED.\n"
                  "  The program loaded, so bpf() itself is permitted; the kernel refused to\n"
                  "  attach it to the cgroup. Usual causes: missing CAP_NET_ADMIN, a cgroup v1\n"
                  "  hybrid hierarchy, or an attach-type restriction from another controller.\n");
      break;
    case kSelftestMarkFailed:
      std::printf("VERDICT: THE MECHANISM DOES NOT WORK HERE — THE MARK NEVER LANDED.\n"
                  "  This is the dangerous one: load and attach both SUCCEEDED, so every\n"
                  "  green check short of an actual measurement would have said 'protected',\n"
                  "  and the sockets would still have gone into the tunnel. Whatever the\n"
                  "  cause (a struct bpf_sock layout the program's offsetof() did not match,\n"
                  "  a kernel that ignores mark writes from sock_create, another cgroup\n"
                  "  program overwriting sk_mark), the daemon must not be trusted to exclude\n"
                  "  its own traffic on this host.\n");
      break;
    case kSelftestChildBadArgs:
    case kSelftestExecFailed:
      std::printf("VERDICT: NO ANSWER — the test child could not be started (%s).\n",
                  code == kSelftestExecFailed ? "execv(/proc/self/exe) failed"
                                              : "the child rejected its arguments");
      code = kSelftestNoAnswer;
      break;
    default:
      std::printf("VERDICT: NO ANSWER — the test child exited with status %d and no verdict.\n",
                  code);
      code = kSelftestNoAnswer;
      break;
  }
  std::printf("\nNothing on this machine was changed: the temporary cgroup is removed, no\n"
              "packet was sent, and no firewall, route or DNS state was touched.\n");
  std::fflush(stdout);

  if (code == kSelftestOk) return kSelftestOk;
  if (code == kSelftestNoAnswer) return kSelftestNoAnswer;
  return kSelftestMechanismFailed;
}

struct Daemon {
  GMainLoop* loop = nullptr;
  urnw::TunnelHost* tunnel = nullptr;
  urnw::ControlServer* server = nullptr;
};

gboolean OnTerminate(gpointer data) {
  auto* d = static_cast<Daemon*>(data);
  std::fprintf(stderr, "[daemon] terminating\n");
  // Stop accepting/serving first, then tear the tunnel down cleanly (routes,
  // policy rules, the nftables table and the resolvectl revert all happen
  // under Stop).
  d->server->Stop();
  d->tunnel->Stop("daemon_shutdown");
  g_main_loop_quit(d->loop);
  return G_SOURCE_REMOVE;
}

// Everything the data plane needs from the host, resolved ONCE at startup and
// printed, so a broken environment is named before the first Connect instead
// of during it (the audit's "no preflight for ip/resolvectl being on PATH").
// Returns the number of REQUIRED tools that are missing.
int ReportPreflight() {
  struct Tool {
    const char* name;
    bool required;
    const char* why;
  };
  static const Tool kTools[] = {
      {"ip", true, "iproute2: the tun address, the capture routes and the policy rules"},
      {"nft", true, "nftables: egress self-exclusion (without it the daemon's own sockets "
                    "fall into its own tunnel), the IPv6 and DNS leak floor, the kill switch"},
      {"resolvectl", false,
       "systemd-resolved: the FIRST of three ways DNS is pointed at the tunnel "
       "(resolvconf and a direct /etc/resolv.conf takeover follow it)"},
      {"modprobe", false, "loading the tun kernel module when /dev/net/tun is absent"},
  };
  int missingRequired = 0;
  for (const Tool& tool : kTools) {
    const std::string path = urnw::FindTool(tool.name);
    if (!path.empty()) {
      std::fprintf(stderr, "[preflight] %-11s %s\n", tool.name, path.c_str());
      continue;
    }
    if (tool.required) ++missingRequired;
    std::fprintf(stderr, "[preflight] %-11s MISSING (%s) — %s\n", tool.name,
                 tool.required ? "required" : "optional", tool.why);
  }
  const urnw::CgroupRef cgroup = urnw::SelfCgroupV2();
  if (cgroup.valid) {
    std::fprintf(stderr, "[preflight] cgroup      %s (level %d)\n", cgroup.path.c_str(),
                 cgroup.level);
  } else {
    ++missingRequired;
    std::fprintf(stderr,
                 "[preflight] cgroup      MISSING (required) — no cgroup v2 unified "
                 "hierarchy, so the daemon's own sockets cannot be marked and no tunnel can "
                 "be started safely\n");
  }
  // WHICH DNS TIER THIS MACHINE WILL ACTUALLY TAKE. The tool table above can
  // only answer "is resolvectl on $PATH", and on Arch/CachyOS that answer is
  // YES on a machine where systemd-resolved is not running at all — the systemd
  // package ships the binary regardless. Preflight therefore used to print a
  // reassuring line about the exact mechanism that was about to fail. This runs
  // the same probe the tunnel runs, so what is printed here is what will happen.
  {
    const urnw::DnsHostProbe dns = urnw::ProbeDnsHost();
    const char* tier = "3 (direct /etc/resolv.conf takeover)";
    if (dns.resolved_running && dns.resolvectl_present) {
      tier = "1 (systemd-resolved)";
    } else if (dns.resolvconf_present && !dns.resolvconf_is_resolvectl) {
      tier = "2 (resolvconf)";
    }
    std::fprintf(stderr, "[preflight] dns         tier %s\n", tier);
    std::fprintf(stderr, "[preflight]             %s\n", dns.detail.c_str());
    if (dns.nss_resolve_before_dns && !dns.resolved_running) {
      std::fprintf(stderr,
                   "[preflight]             note: /etc/nsswitch.conf puts `resolve` before "
                   "`dns` while systemd-resolved is NOT running. nss-resolve answers UNAVAIL "
                   "and lookups fall through to /etc/resolv.conf, so tier 3 is authoritative "
                   "here.\n");
    }
  }
  if (::access("/dev/net/tun", F_OK) == 0) {
    std::fprintf(stderr, "[preflight] /dev/net/tun present\n");
  } else {
    std::fprintf(stderr,
                 "[preflight] /dev/net/tun absent — the tun module is not loaded yet "
                 "(one modprobe is attempted at the first start)\n");
  }
  return missingRequired;
}

}  // namespace

int main(int argc, char** argv) {
  bool foreground = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--version") {
      std::printf("urnetworkd %s (control protocol %d, sdk %s)\n", UR_APP_VERSION,
                  urnw::ctl::kControlProtocolVersion, urnet::version().c_str());
      return 0;
    }
    if (arg == "--foreground") {
      foreground = true;
      continue;
    }
    if (arg == "--selftest-egress") {
      // Proves the cgroup-BPF socket marker on THIS kernel. No tunnel, no
      // routes, no nftables, no packets — see the block comment above
      // RunSelftestEgress for exactly what it does and does not touch.
      const int rc = RunSelftestEgress(argv[0]);
      std::fflush(stdout);
      return rc;
    }
    if (arg == kSelftestChildFlag) {
      // INTERNAL, and it is not a way to attach a BPF program anywhere you
      // like: the child refuses any cgroup whose leaf name is not
      // urnw-selftest-*, and it is only ever re-exec()d by the parent half
      // above, already inside the cgroup it names.
      if (i + 1 >= argc) {
        std::fprintf(stderr, "urnetworkd: %s needs a cgroup path (internal flag)\n",
                     kSelftestChildFlag);
        return kSelftestChildBadArgs;
      }
      const int rc = RunSelftestEgressChild(argv[i + 1]);
      std::fflush(stdout);
      return rc;
    }
    if (arg == "--diagnose") {
      // Print-and-exit field-support tool: everything the data plane needs
      // from the host, without touching the network or the control socket.
      std::printf("urnetworkd %s (control protocol %d, sdk %s)\n", UR_APP_VERSION,
                  urnw::ctl::kControlProtocolVersion, urnet::version().c_str());
      std::printf("control socket: %s\n", urnw::ControlServer::SocketPath().c_str());
      std::printf("state dir:      %s\n", StateDir().c_str());
      std::printf("log dir:        %s\n", LogDir().c_str());
      // WHICH AUTHORITY WOULD BE IN FORCE, and therefore which remediation a
      // refused user should be given. Under polkit there is no group to join
      // and nothing to log out of; under the fallback there is, and telling
      // someone to log out and back in when it would change nothing is exactly
      // the confusion this design set out to remove. Read-only: one access(2).
      {
        // TWO FACTS, REPORTED SEPARATELY. The latch needs both, and saying
        // only "MISSING" when the action file is sitting right there — because
        // it is polkit that is absent — sends the reader to look for the wrong
        // thing entirely.
        const bool polkit = urnw::ControlServer::PolkitPolicyPresent();
        const bool actionFile = ::access(urnw::ControlServer::PolicyPath().c_str(), F_OK) == 0;
        const bool runtime = urnw::ControlServer::PolkitRuntimePresent();
        std::printf("authorization:  %s (action file %s, polkit %s)\n",
                    polkit ? urnw::ctl::kAuthModePolkit : urnw::ctl::kAuthModeGroup,
                    actionFile ? "installed" : "MISSING",
                    runtime ? "installed" : "NOT INSTALLED");
        std::printf("                %s\n", urnw::ControlServer::PolicyPath().c_str());
        if (polkit) {
          std::printf("                the person signed in at this device's screen is "
                      "authorized in their current session:\n"
                      "                no group membership and no log out / log back in.\n");
        } else {
          if (actionFile && !runtime) {
            // The case that used to fail SHUT: the file promises polkit can
            // answer, and nothing on this machine can.
            std::printf("                the polkit action file is installed but polkit ITSELF "
                        "is not on this\n"
                        "                machine, so there is nothing to answer an "
                        "authorization check. Falling\n"
                        "                back to the '%s' group rather than refusing every "
                        "request.\n"
                        "                Install polkit to remove the group requirement.\n",
                        urnw::ctl::kControlGroupName);
          } else {
            std::printf("                no polkit action file, so urnetworkd falls back to the "
                        "'%s' group.\n",
                        urnw::ctl::kControlGroupName);
          }
          std::printf("                Members of that group, plus root, may control the "
                      "tunnel — and group\n"
                      "                membership only applies to NEW login sessions.\n");
        }
      }
      // Read-only, no subprocess: the marker is a tmpfs file. A user reading
      // this while blocked needs to know WHICH of the two situations they are
      // in before they are told what to type.
      const bool armed = ::access(urnw::NetFilter::ArmedMarkerPath(), F_OK) == 0;
      std::printf("kill switch:    %s (marker %s)\n",
                  armed ? "ARMED — this machine is deliberately blocked" : "not armed",
                  urnw::NetFilter::ArmedMarkerPath());
      const int missing = ReportPreflight();
      // The preflight can only say the cgroup EXISTS. Whether a program
      // attached to it actually marks sockets on this kernel is a different
      // question, it is the one this daemon's self-exclusion rests on, and
      // there is exactly one honest way to answer it.
      std::printf(
          "\negress self-exclusion: this build marks the daemon's own sockets at creation with\n"
          "a cgroup-BPF program. To prove that works on THIS kernel (no tunnel, no routes, no\n"
          "nftables, no packets):\n\n    sudo %s --selftest-egress\n",
          SelfPath(argv[0]).c_str());
      PrintRecovery(argv[0]);
      std::fflush(stdout);
      return missing == 0 ? 0 : 1;
    }
    if (arg == "--revert" || arg == "--revert-unless-armed") {
      // The documented escape hatch, and the unit's ExecStopPost.
      //
      //   --revert                unconditional: lift everything, clear the
      //                           armed marker. This is what a stuck human
      //                           runs.
      //   --revert-unless-armed   the same sweep, except that a machine which
      //                           was ARMED when the daemon died has its floor
      //                           REPLACED with a fresh armed one in a single
      //                           atomic `nft -f` — no open window across the
      //                           crash, which is the entire point of the kill
      //                           switch. Only the unit uses this.
      const bool preserveArmed = (arg == "--revert-unless-armed");
      if (::geteuid() != 0) {
        std::fprintf(stderr,
                     "urnetworkd %s: this changes the kernel firewall and must run as root.\n"
                     "  try: sudo %s %s\n",
                     arg.c_str(), SelfPath(argv[0]).c_str(), arg.c_str());
        return 1;
      }
      if (!preserveArmed && ::access(urnw::ControlServer::SocketPath().c_str(), F_OK) == 0) {
        std::fprintf(stderr,
                     "urnetworkd --revert: WARNING — %s still exists, so urnetworkd may still be "
                     "running. It re-installs its own ruleset within seconds of anything removing "
                     "it; stop it first (systemctl stop urnetworkd) or this will not stick.\n"
                     "  This sweep ALSO restores /etc/resolv.conf if a tunnel took it over. On a "
                     "host with no systemd-resolved that is how DNS reaches the tunnel, so running "
                     "this against a LIVE session pulls the tunnel's resolver out from under it "
                     "and names stop resolving until you reconnect.\n",
                     urnw::ControlServer::SocketPath().c_str());
      }
      // Whether the sweep is SUPPOSED to leave a table behind, decided before
      // it runs (it clears the marker on the paths where it does not).
      const bool keepsArmedFloor =
          preserveArmed && ::access(urnw::NetFilter::ArmedMarkerPath(), F_OK) == 0;
      // Idempotent by construction (add-then-delete), so running it on a
      // machine with nothing installed is a successful no-op.
      // The return (did it preserve an armed floor?) is deliberately unused
      // here: keepsArmedFloor above already decided that from the marker, before
      // the sweep could clear it, and the message below has to describe the
      // INTENT of this invocation.
      static_cast<void>(urnw::NetFilter::SweepStaleState(preserveArmed));
      // CONFIRM IT. SweepStaleState() returns void and only logs, and this is
      // the command a cut-off user is told to trust: reporting a failed sweep
      // as a completed one would send them away from the one thing that was
      // going to fix it. `nft list table` is read-only.
      if (const std::string nft = urnw::FindTool("nft"); !nft.empty() && !keepsArmedFloor) {
        if (urnw::RunCommand({nft, "list", "table", "inet", urnw::kNftTableName}).ok()) {
          std::fprintf(stderr,
                       "urnetworkd %s: FAILED — `table inet %s` is still in the kernel. This "
                       "machine may still be filtered. Try: %s\n",
                       arg.c_str(), urnw::kNftTableName, urnw::NetFilter::RecoveryCommand());
          return 1;
        }
      }
      std::fprintf(stderr, "urnetworkd %s: done%s\n", arg.c_str(),
                   keepsArmedFloor ? " (the kill switch was armed, so the block floor was kept "
                                     "and re-installed)"
                                   : "");
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: urnetworkd [--foreground] [--diagnose] [--selftest-egress] [--revert]\n"
          "                  [--version]\n"
          "URnetwork privileged daemon: control socket at %s,\n"
          "device RPC on 127.0.0.1:%d while the tunnel is up.\n"
          "  --diagnose             print the host preflight (ip/nft/resolvectl, cgroup, tun),\n"
          "                         the authorization mode, the kill-switch state and the\n"
          "                         recovery steps, then exit\n"
          "  --selftest-egress      prove, on this kernel, that the cgroup-BPF socket marker\n"
          "                         that keeps the daemon's own traffic out of its own tunnel\n"
          "                         actually marks sockets at creation. Starts no tunnel and\n"
          "                         touches no routes, no nftables and no DNS; sends no packet.\n"
          "                         Needs root (bpf() does). Exit 0 = works, 1 = does not,\n"
          "                         2 = could not be measured\n"
          "  --revert               lift the URnetwork firewall table, policy rules, capture\n"
          "                         routes, and clear the armed marker; then exit. Run this when\n"
          "                         a machine is stuck blocked. Requires root.\n"
          "  --revert-unless-armed  the same sweep, but a machine that was armed when the daemon\n"
          "                         died stays armed (used by the unit's ExecStopPost)\n"
          "  --foreground           do not send the systemd readiness notification\n",
          urnw::ControlServer::SocketPath().c_str(), urnw::ctl::kDeviceRpcPort);
      PrintRecovery(argv[0]);
      return 0;
    }
    std::fprintf(stderr, "urnetworkd: unknown argument '%s'\n", arg.c_str());
    return 2;
  }

  // A dead control client must surface as a send() error, not kill the daemon.
  std::signal(SIGPIPE, SIG_IGN);

  const std::string stateDir = StateDir();
  const std::string logDir = LogDir();
  g_mkdir_with_parents(stateDir.c_str(), 0700);
  g_mkdir_with_parents(logDir.c_str(), 0700);
  urnet::setLogDir(logDir);
  // The log ring is the READ half of the Advanced-Mode session log: the SDK
  // writes its [rel] reliability stream to glog files in this directory, and
  // the ring tails them so the app can show them live over the control socket.
  // Without these two calls the ring exists, serves log_tail, and truthfully
  // reports that it holds nothing — a working pipe with no water in it.
  urnw::DaemonLog::Instance().SetSdkLogDir(logDir);
  urnw::DaemonLog::Instance().StartSdkLogPolling();
  urnw::DaemonLogf("[daemon] urnetworkd %s starting (log dir %s)\n", UR_APP_VERSION, logDir.c_str());
  urnet::setMemoryLimit(kMemoryLimit);

  // Clear a location override left behind by a previous run BEFORE serving any
  // client: nothing else on the system ever reverts /etc/geolocation, so an
  // override surviving a crash/reboot would keep reporting a provider's city
  // indefinitely. Only a file carrying the URnetwork marker is touched — an
  // admin's hand-written static location is not ours to delete. Once a client
  // claims the override (location_override_write), the server clears it again
  // when that client disconnects.
  urnw::DirectGeoClueWriter geoWriter;
  if (urnw::DirectGeoClueWriter::SystemOverridePresent()) {
    if (geoWriter.Clear()) {
      std::fprintf(stderr, "[daemon] cleared stale location override at startup\n");
    } else {
      std::fprintf(stderr, "[daemon] could not clear stale location override\n");
    }
  }

  // Crash safety is NOT inherited on Linux: unlike the Windows dynamic WFP
  // session, an nftables table and a set of `ip rule`s outlive the process
  // that installed them. Sweep our own leftovers (by table name, by our fwmark
  // and by our route-table id — never by priority alone, which would let us
  // delete a co-installed WireGuard's rules) before serving anyone.
  //
  // preserveArmed=true is the whole crash story and it is NOT the default of
  // the parameter: calling SweepStaleState() bare (which is what this line used
  // to do) deletes the table unconditionally, so a daemon that was SIGKILLed
  // with the kill switch ARMED came back up and OPENED the machine — the one
  // moment the floor must be continuous. With the intent passed explicitly, the
  // stale table is REPLACED by a fresh armed one in a single atomic `nft -f`
  // when (and only when) the /run marker says this machine was armed when we
  // died. A reboot clears the marker with the rest of /run, so nothing comes up
  // blocked before a user has asked for anything.
  const bool sweptArmedFloor = urnw::NetFilter::SweepStaleState(/*preserveArmed=*/true);
  if (sweptArmedFloor) {
    std::fprintf(stderr,
                 "[daemon] this machine is BLOCKED by a kill-switch floor carried over from a "
                 "daemon that died while armed. Turn the kill switch off in the app, or:\n%s\n",
                 urnw::NetFilter::RecoveryHelpText().c_str());
  }
  if (const int missing = ReportPreflight(); missing > 0) {
    // Do not refuse to start: the control socket must still come up so the GUI
    // gets a real answer instead of "the service is not running". The first
    // start_tunnel is what fails, with a code naming the missing piece.
    std::fprintf(stderr,
                 "[daemon] %d required host component(s) are missing: the control socket will "
                 "serve, but start_tunnel will refuse until they are installed\n",
                 missing);
  }

  urnw::TunnelHost tunnel(stateDir);
  // A floor carried over from a crash has an OWNER from here on: status tells
  // the truth, the reaper re-installs it if it is flushed, and shutdown tears
  // it down. Adopting after construction (not inside it) keeps the sweep and
  // the host independent of each other's ordering.
  if (sweptArmedFloor) tunnel.AdoptArmedFloor();
  // Off by default: a tunnel survives a GUI crash or restart and is adoptable.
  // Set $URNETWORK_ORPHAN_TIMEOUT_SECONDS to have the daemon stop a tunnel
  // nobody has owned for that long — the "captured machine with no UI"
  // recovery, for setups that prefer it to the tray action.
  if (const char* env = std::getenv("URNETWORK_ORPHAN_TIMEOUT_SECONDS");
      env != nullptr && *env != '\0') {
    const int seconds = std::atoi(env);
    if (seconds > 0) {
      tunnel.SetOrphanTimeoutSeconds(seconds);
      std::fprintf(stderr, "[daemon] orphan timeout: %ds\n", seconds);
    }
  }
  urnw::ControlServer server(tunnel, geoWriter);
  server.SetDaemonVersion(UR_APP_VERSION);
  // exact-match enforced against the GUI's hello: the gob device RPC carries
  // no version of its own, so a drifted SDK pair must be refused here
  server.SetSdkVersion(urnet::version());
  if (!server.Start()) {
    std::fprintf(stderr, "urnetworkd: could not start the control server\n");
    return 1;
  }

  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  Daemon daemon{loop, &tunnel, &server};
  g_unix_signal_add(SIGTERM, &OnTerminate, &daemon);
  g_unix_signal_add(SIGINT, &OnTerminate, &daemon);

  if (!foreground) NotifySystemdReady();
  // Into the log ring as well as the journal, once per start: the ring is what
  // the GUI's log tail shows, and this is the line that has to be in front of a
  // user whose machine is blocked. Cheap insurance against the one failure mode
  // that has no other way out.
  // auth=… is in the READY line, not only in ControlServer's own start
  // breadcrumb, because it is the first thing to look at when a user reports
  // "it asked me for a password" or "it says I am not allowed": the answer is
  // always which authority this daemon latched at start, and the log ring is
  // the surface the GUI can show them.
  urnw::DaemonLogf(
      "[daemon] urnetworkd %s ready (state=%s, auth=%s). If this machine ends up blocked with "
      "no way to reach the daemon: stop the service, then run `%s --revert` (firewall half "
      "alone: %s)\n",
      UR_APP_VERSION, stateDir.c_str(), server.AuthModeName(), SelfPath(argv[0]).c_str(),
      urnw::NetFilter::RecoveryCommand());
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  return 0;
}
