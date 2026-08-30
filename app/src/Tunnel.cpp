// SPDX-License-Identifier: MPL-2.0
#include "Tunnel.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

namespace urnw {
namespace {

// Reserved for documentation (RFC 5737 TEST-NET-1) and inside the capture set
// (192.0.0.0/9), so the witness's two sockets resolve exactly the policy we
// installed without naming anyone's real host. Nothing is routed there and
// nothing can listen there — and since the witness only ever connect()s, which
// sends no packet, nothing is ever addressed to it on the wire either.
constexpr const char* kEgressProbeAddress = "192.0.2.1";

// Byte-identical to ctl::kCodeEgressUnprotected / ctl::kCodeRouteInstallFailed.
// Literals and not an include, exactly like kFilterCode* in the header: this
// file stays free of ControlProtocol.hpp (and of nlohmann).
constexpr const char* kCodeEgressUnprotected = "egress_unprotected";
constexpr const char* kCodeRouteInstallFailed = "route_install_failed";

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' ||
                        s.back() == '\t')) {
    s.pop_back();
  }
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
  return s.substr(start);
}

std::string JoinArgv(const std::vector<std::string>& argv) {
  std::string out;
  for (const auto& a : argv) {
    if (!out.empty()) out += ' ';
    out += a;
  }
  return out;
}

void SleepMillis(int millis) {
  timespec ts{};
  ts.tv_sec = millis / 1000;
  ts.tv_nsec = static_cast<long>(millis % 1000) * 1000000L;
  ::nanosleep(&ts, nullptr);
}

// RETIRED WITH THE LAST `ip route get` IN THIS FILE. It parsed the outgoing
// interface out of `ip route get`'s answer, which is how both halves of the old
// egress gate were read. `ip route get` asks the kernel where a HYPOTHETICAL
// packet, belonging to no socket, would go; the two questions this file
// actually needs — "does ordinary traffic land in the tun" and "does this
// daemon's own socket escape it" — are now answered by connect()ing two real
// sockets and reading the source addresses the kernel bound to them
// (BindWitnessSocket). Nothing here parses routing prose any more.
bool ValidInterfaceName(const std::string& name) {
  if (name.empty() || name.size() >= IFNAMSIZ) return false;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

std::string MarkHex(uint32_t mark) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", mark);
  return buf;
}

// ---- nftables helpers (file-local) -----------------------------------------

// Tmpfs, beside the control socket: a crash-restart comes back armed, a REBOOT
// does not. See NetFilter::ArmedMarkerPath.
constexpr const char* kArmedMarkerPath = "/run/urnetwork/kill-switch-armed";
constexpr const char* kArmedMarkerDir = "/run/urnetwork";

// Anything interpolated into a QUOTED nft string (a cgroup path) must not be
// able to close the quote, escape it, or end the line. The values come from
// /proc/self/cgroup and a fixed candidate list, so this is depth, not the
// boundary — but a ruleset is the last place to find out the assumption was
// wrong.
bool SafeNftString(const std::string& value) {
  if (value.empty() || value.size() > 512) return false;
  for (const unsigned char c : value) {
    if (c < 0x20 || c == 0x7f) return false;
    if (c == '"' || c == '\\') return false;
  }
  return true;
}

// PURE half: can this ref be written into a rule at all? BuildNftRuleset uses
// only this, so it stays syscall-free and unit-testable.
bool CgroupQuotable(const CgroupRef& ref) {
  return ref.valid && ref.level >= 1 && SafeNftString(ref.path);
}

// IMPURE half, for Apply(): nft resolves the path to a cgroup id at LOAD time
// and fails the WHOLE transaction when it is absent, so a ref that does not
// exist must never reach the script.
bool CgroupInstallable(const CgroupRef& ref) {
  return CgroupQuotable(ref) && CgroupV2PathExists(ref.path);
}

// Everything BuildNftRuleset derives from a config, in ONE place, so the
// predicates below and the emitted text can never disagree.
struct FilterDerived {
  bool tun_named = false;  // emit the oifname/iifname permits
  bool dns_floor = false;  // pin :53 to the tunnel resolvers and close the rest
  bool block_v6 = false;
  bool helper_dns = false;
  std::vector<std::string> resolvers;  // only those that passed inet_pton
};

FilterDerived DeriveFilter(const FilterConfig& cfg) {
  FilterDerived d;
  if (cfg.state == FilterState::Off) return d;
  d.tun_named = !cfg.tun_name.empty() && ValidInterfaceName(cfg.tun_name) &&
                (cfg.state == FilterState::Connecting || cfg.state == FilterState::Connected);
  for (const auto& r : cfg.tunnel_resolvers) {
    if (IsIpv4Address(r)) d.resolvers.push_back(r);
  }
  // Gated on dns_applied by the CALLER (block_offtunnel_dns) and on a
  // surviving resolver here: closing :53 with no permitted path is a total
  // resolution outage, which is worse than the leak it would prevent.
  d.dns_floor = cfg.state == FilterState::Connected && d.tun_named &&
                cfg.block_offtunnel_dns && !d.resolvers.empty();
  d.block_v6 = cfg.block_ipv6;
  const NftCgroupMode cgroupMode =
      SelectNftCgroupMode(cfg.socket_mark_proven, cfg.cgroup_socket_match_supported,
                          cfg.floor, !cfg.dns_helper_cgroups.empty());
  d.helper_dns = cgroupMode == NftCgroupMode::CgroupAndMark &&
                 cfg.state == FilterState::Connecting && cfg.floor &&
                 !cfg.dns_helper_cgroups.empty();
  return d;
}

// `nft list table` prints one chain per `chain <name> {` block, with the base
// spec and `policy <verb>;` on the next line. Returns "" when the chain is
// absent or carries no policy.
std::string ChainPolicyFromListing(const std::string& listing, const char* chain) {
  const std::string needle = std::string("chain ") + chain + " {";
  const size_t at = listing.find(needle);
  if (at == std::string::npos) return {};
  const size_t end = listing.find("\n\t}", at);
  const size_t stop = end == std::string::npos ? listing.size() : end;
  const size_t p = listing.find("policy ", at);
  if (p == std::string::npos || p > stop) return {};
  const size_t from = p + 7;
  size_t to = from;
  while (to < listing.size() && listing[to] != ';' && listing[to] != '\n') ++to;
  return Trim(listing.substr(from, to - from));
}

// PURE: pull one NAMED counter out of `nft list counters table ...`, which
// prints
//     counter urnw_probe_phy {
//         packets 4 bytes 112
//     }
// BY NAME, never by position: nft is free to print objects in any order, and a
// positional parse would silently swap "left through the tunnel" for "left
// around it" — i.e. invert the verdict — the day it does.
bool ParseNamedCounter(const std::string& listing, const char* name, uint64_t* packets,
                       uint64_t* bytes) {
  const std::string needle = std::string("counter ") + name + " {";
  const size_t at = listing.find(needle);
  if (at == std::string::npos) return false;
  const size_t end = listing.find('}', at);
  const size_t stop = end == std::string::npos ? listing.size() : end;
  const size_t p = listing.find("packets ", at);
  if (p == std::string::npos || p > stop) return false;
  const char* from = listing.c_str() + p + std::strlen("packets ");
  char* tail = nullptr;
  const unsigned long long pk = std::strtoull(from, &tail, 10);
  if (tail == from) return false;
  unsigned long long by = 0;
  const size_t bat = listing.find("bytes ", p);
  if (bat != std::string::npos && bat < stop) {
    by = std::strtoull(listing.c_str() + bat + std::strlen("bytes "), nullptr, 10);
  }
  if (packets != nullptr) *packets = static_cast<uint64_t>(pk);
  if (bytes != nullptr) *bytes = static_cast<uint64_t>(by);
  return true;
}

// The body of one chain out of `nft list table`, between its opening brace and
// the closing "\n\t}". "" when the chain is absent — which is itself an answer
// and never an inconvenience to be worked around.
std::string ChainBodyFromListing(const std::string& listing, const char* chain) {
  const std::string needle = std::string("chain ") + chain + " {";
  const size_t at = listing.find(needle);
  if (at == std::string::npos) return {};
  const size_t from = at + needle.size();
  const size_t end = listing.find("\n\t}", from);
  // A MISSING TERMINATOR IS A PARSE FAILURE, NOT "the rest of the file".
  // Returning the remainder used to hand the aim guards a body containing every
  // OTHER chain's rules, so a witness could confirm it was pointed at the right
  // interface and the right mark by finding those strings in a NEIGHBOUR's
  // chain — a green check produced by a broken parse, which is precisely the
  // false-confidence class that let the amplification storm ship.
  if (end == std::string::npos) return {};
  return listing.substr(from, end - from);
}

// ONE `nft list table` for the whole table, so every reader gets its counters
// AND the rules that feed them from the SAME snapshot and cannot straddle a
// packet or a table swap.
//
// `list table` and not `list counters table`: the witness has to check that the
// instrument it is about to read is still pointed at the interface it thinks it
// is, and a counters-only listing carries the numbers with none of the rules
// that produce them. Reading a counter without reading its rule is how a green
// check on the wrong question gets shipped.
bool ReadTableListing(std::string* listing, std::string* error) {
  const auto fail = [error](std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
  };
  const std::string nft = FindTool("nft");
  if (nft.empty()) return fail("nftables (nft) is not installed");
  const CommandResult r = RunCommand({nft, "list", "table", "inet", kNftTableName});
  if (!r.ok()) {
    return fail(std::string("nft list table inet ") + kNftTableName + ": " + r.Describe() +
                ". The capture policy this daemon installed is not in the kernel, so nothing "
                "here can be measured");
  }
  if (listing != nullptr) *listing = r.output;
  return true;
}

// DIAGNOSTIC ONLY — see the contract on NetFilter::MarkChainCounters. Absent
// counters mean the cgroup rule was never emitted, which is itself the answer.
bool ReadMarkChainCounters(uint64_t* daemonPackets, uint64_t* totalPackets, std::string* error) {
  std::string listing;
  if (!ReadTableListing(&listing, error)) return false;
  if (!ParseNamedCounter(listing, kNftMarkDaemonCounter, daemonPackets, nullptr)) {
    if (error != nullptr) {
      *error = std::string("the table carries no ") + kNftMarkDaemonCounter +
               " counter: nothing is marking the daemon's own sockets";
    }
    return false;
  }
  ParseNamedCounter(listing, kNftMarkTotalCounter, totalPackets, nullptr);
  return true;
}

bool ArmedMarkerPresent() {
  struct stat st {};
  return ::stat(kArmedMarkerPath, &st) == 0;
}

// Best effort, and deliberately non-fatal: the marker only changes how a
// CRASH-restart behaves. Failing to write it must never fail an install that
// otherwise took.
void SetArmedMarker(bool armed) {
  if (!armed) {
    ::unlink(kArmedMarkerPath);
    return;
  }
  ::mkdir(kArmedMarkerDir, 0755);
  const int fd = ::open(kArmedMarkerPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    std::fprintf(stderr, "[filter] could not write the armed marker %s: %s\n", kArmedMarkerPath,
                 std::strerror(errno));
    return;
  }
  ::close(fd);
}

}  // namespace

// ---- process execution -----------------------------------------------------

std::string CommandResult::Describe() const {
  if (!spawned) return "could not run the command";
  std::string out;
  if (term_signal != 0) {
    out = "killed by signal " + std::to_string(term_signal);
  } else if (exit_code == 127) {
    out = "not found on PATH";
  } else if (exit_code == 126) {
    // The tool is THERE and we were refused permission to exec it. On a
    // Fedora-family box that is nearly always SELinux refusing the domain
    // transition, so say where to look instead of leaving a bare EACCES.
    out = "permission denied executing it (check SELinux: journalctl -t audit | grep denied)";
  } else if (exit_code > 128 && exit_code < 192) {
    out = std::string("exec failed: ") + std::strerror(exit_code - 128);
  } else {
    out = "exit " + std::to_string(exit_code);
  }
  if (!output.empty()) out += ": " + output;
  return out;
}

CommandResult RunCommand(const std::vector<std::string>& argv, const std::string& stdin_data) {
  CommandResult result;
  if (argv.empty()) return result;

  // Build the char* vector BEFORE fork: the daemon is multi-threaded (the Go
  // runtime), and allocating between fork and exec can deadlock on the
  // allocator lock held by another thread at fork time.
  std::vector<char*> args;
  args.reserve(argv.size() + 1);
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);

  int inPipe[2] = {-1, -1};
  int outPipe[2] = {-1, -1};
  if (::pipe(inPipe) != 0) return result;
  if (::pipe(outPipe) != 0) {
    ::close(inPipe[0]);
    ::close(inPipe[1]);
    return result;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(inPipe[0]);
    ::close(inPipe[1]);
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    return result;
  }
  if (pid == 0) {
    // child: stdin from the parent, stdout+stderr merged back
    ::dup2(inPipe[0], STDIN_FILENO);
    ::dup2(outPipe[1], STDOUT_FILENO);
    ::dup2(outPipe[1], STDERR_FILENO);
    ::close(inPipe[0]);
    ::close(inPipe[1]);
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    // the daemon ignores SIGPIPE; children must not inherit that decision
    ::signal(SIGPIPE, SIG_DFL);
    ::execvp(args[0], args.data());
    // CARRY THE REAL REASON OUT. This used to be a blanket _exit(127), which
    // Describe() rendered as "command not found" for EVERY exec failure — and
    // that message cost real debugging time on Bazzite, where the file existed,
    // was executable, ran fine by hand, and was refused by SELinux with EACCES
    // (NoNewPrivileges blocking the init_t -> iptables_t domain transition).
    // "command not found" for a file that is present and runnable sends the
    // reader hunting for a missing package that is already installed.
    //
    // 128 + errno keeps the one-byte exit status: 126 (EACCES) and 127 (ENOENT)
    // stay conventional, anything else is decoded by Describe().
    const int err = errno;
    if (err == ENOENT) ::_exit(127);
    if (err == EACCES || err == EPERM) ::_exit(126);
    ::_exit(128 + (err & 0x3F));
  }

  ::close(inPipe[0]);
  ::close(outPipe[1]);
  const int inFd = inPipe[1];
  const int outFd = outPipe[0];
  ::fcntl(inFd, F_SETFL, ::fcntl(inFd, F_GETFL, 0) | O_NONBLOCK);
  ::fcntl(outFd, F_SETFL, ::fcntl(outFd, F_GETFL, 0) | O_NONBLOCK);

  // Drain stdout while feeding stdin: `nft -f -` is small, but a blocking
  // write-then-read would still deadlock the day a ruleset outgrows a pipe.
  size_t written = 0;
  bool stdinOpen = true;
  bool stdoutOpen = true;
  if (stdin_data.empty()) {
    ::close(inFd);
    stdinOpen = false;
  }
  while (stdinOpen || stdoutOpen) {
    pollfd fds[2];
    int count = 0;
    int inIndex = -1;
    int outIndex = -1;
    if (stdinOpen) {
      inIndex = count;
      fds[count].fd = inFd;
      fds[count].events = POLLOUT;
      fds[count].revents = 0;
      ++count;
    }
    if (stdoutOpen) {
      outIndex = count;
      fds[count].fd = outFd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      ++count;
    }
    if (count == 0) break;
    const int ready = ::poll(fds, static_cast<nfds_t>(count), -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (inIndex >= 0 && fds[inIndex].revents != 0) {
      const ssize_t n = ::write(inFd, stdin_data.data() + written, stdin_data.size() - written);
      if (n > 0) {
        written += static_cast<size_t>(n);
        if (written >= stdin_data.size()) {
          ::close(inFd);
          stdinOpen = false;
        }
      } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        ::close(inFd);
        stdinOpen = false;
      }
    }
    if (outIndex >= 0 && fds[outIndex].revents != 0) {
      char buf[4096];
      const ssize_t n = ::read(outFd, buf, sizeof(buf));
      if (n > 0) {
        result.output.append(buf, static_cast<size_t>(n));
      } else if (n == 0) {
        stdoutOpen = false;
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        stdoutOpen = false;
      }
    }
  }
  if (stdinOpen) ::close(inFd);
  ::close(outFd);

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  result.spawned = true;
  // The old code logged `int rc = std::system(...)` verbatim — a WAIT STATUS,
  // so an exit 1 printed as 256. Decode it properly.
  if (waited == pid) {
    if (WIFEXITED(status)) {
      result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      result.term_signal = WTERMSIG(status);
      result.exit_code = -1;
    }
  }
  result.output = Trim(std::move(result.output));
  return result;
}

// EACCES/EPERM on /dev/net/tun while running as root is almost never a missing
// capability — the daemon holds CAP_NET_ADMIN by virtue of being root, and
// systemd is not dropping it. It is a MANDATORY ACCESS CONTROL refusal, and on
// the Fedora family it is specifically SELinux denying the `init_t` domain
// read/write on tun_tap_device_t, because a binary installed outside a package
// carries no policy of its own. Measured on Bazzite:
//
//   avc denied { read write } name="tun" scontext=init_t
//        tcontext=tun_tap_device_t tclass=chr_file
//
// Naming CAP_NET_ADMIN alone sent a reader hunting for a capability that was
// already held. Say what it actually is, and how to confirm it.
std::string TunPermissionDeniedDetail(const char* verb) {
  std::string out(verb);
  out +=
      " was refused. The daemon runs as root and holds CAP_NET_ADMIN, so this is a "
      "mandatory-access-control refusal, not a missing capability";
  if (::access("/sys/fs/selinux/enforce", F_OK) == 0) {
    out +=
        ". SELinux is present on this host: confirm with `sudo journalctl -t audit | "
        "grep 'denied.*tun'`, and label the daemon with `sudo chcon -t unconfined_exec_t "
        "<path to urnetworkd>` (the installer does this automatically)";
  } else {
    out += " (check AppArmor or any other LSM on this host)";
  }
  return out;
}


std::string FindTool(const char* tool) {
  if (tool == nullptr || *tool == '\0') return {};
  if (std::strchr(tool, '/') != nullptr) {
    return ::access(tool, X_OK) == 0 ? std::string(tool) : std::string();
  }
  std::string path;
  if (const char* env = std::getenv("PATH"); env != nullptr && *env != '\0') path = env;
  // systemd hands services a PATH without the sbin dirs on some distributions,
  // and `ip`/`nft` live there. Append rather than replace.
  path += ":/usr/sbin:/sbin:/usr/bin:/bin";
  std::stringstream ss(path);
  std::string dir;
  while (std::getline(ss, dir, ':')) {
    if (dir.empty()) continue;
    const std::string candidate = dir + "/" + tool;
    if (::access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return {};
}

bool IsIpv4Address(const std::string& value) {
  if (value.empty() || value.size() > 15) return false;
  in_addr addr{};
  return ::inet_pton(AF_INET, value.c_str(), &addr) == 1;
}

// ---- the daemon's own cgroup ----------------------------------------------

CgroupRef SelfCgroupV2() {
  CgroupRef ref;
  std::ifstream f("/proc/self/cgroup");
  if (!f) return ref;
  std::string line;
  while (std::getline(f, line)) {
    // cgroup v2 unified hierarchy: "0::/system.slice/urnetworkd.service"
    if (line.rfind("0::", 0) != 0) continue;
    std::string path = Trim(line.substr(3));
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    while (!path.empty() && path.back() == '/') path.pop_back();
    if (path.empty()) return ref;  // the root cgroup would match every process
    int level = 1;
    for (const char c : path) {
      if (c == '/') ++level;
    }
    ref.valid = true;
    ref.path = std::move(path);
    ref.level = level;
    return ref;
  }
  return ref;  // v1-only hierarchy: `socket cgroupv2` cannot match here
}

// ---- the capture set -------------------------------------------------------

const std::vector<std::string>& CaptureV4Prefixes() {
  static const std::vector<std::string> kPrefixes = {
      "0.0.0.0/5",      "8.0.0.0/7",       "11.0.0.0/8",     "12.0.0.0/6",
      "16.0.0.0/4",     "32.0.0.0/3",      "64.0.0.0/2",     "128.0.0.0/3",
      "160.0.0.0/5",    "168.0.0.0/6",     "172.0.0.0/12",   "172.32.0.0/11",
      "172.64.0.0/10",  "172.128.0.0/9",   "173.0.0.0/8",    "174.0.0.0/7",
      "176.0.0.0/4",    "192.0.0.0/9",     "192.128.0.0/11", "192.160.0.0/13",
      "192.169.0.0/16", "192.170.0.0/15",  "192.172.0.0/14", "192.176.0.0/12",
      "192.192.0.0/10", "193.0.0.0/8",     "194.0.0.0/7",    "196.0.0.0/6",
      "200.0.0.0/5",    "208.0.0.0/4",     "224.0.0.0/3",
  };
  return kPrefixes;
}

// ---- cgroups ---------------------------------------------------------------

bool CgroupV2PathExists(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.size() > 512) return false;
  // Reject traversal outright: this string is pasted after /sys/fs/cgroup/ and
  // then handed to the kernel as an identity.
  if (path.find("..") != std::string::npos) return false;
  struct stat st {};
  const std::string full = std::string("/sys/fs/cgroup/") + path;
  return ::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<CgroupRef> DnsHelperCgroupsV2() {
  // A fixed candidate list, never a scan: the permit these produce is a hole
  // in the floor, bounded to Connecting and to :53, and it must be auditable
  // by reading this array.
  static const char* const kCandidates[] = {
      "system.slice/systemd-resolved.service",
      "system.slice/dnsmasq.service",
      "system.slice/unbound.service",
      "system.slice/dnscrypt-proxy.service",
      "system.slice/NetworkManager.service",
  };
  std::vector<CgroupRef> found;
  for (const char* candidate : kCandidates) {
    const std::string path = candidate;
    if (!CgroupV2PathExists(path)) continue;
    CgroupRef ref;
    ref.valid = true;
    ref.path = path;
    ref.level = 1;
    for (const char c : path) {
      if (c == '/') ++ref.level;
    }
    found.push_back(std::move(ref));
  }
  if (found.empty()) {
    // Legal: the daemon then depends on Go's in-process resolver, whose
    // queries leave the daemon's OWN sockets and are already covered by the
    // cgroup/mark permit. Say which, so a wedged reconnect is diagnosable.
    std::fprintf(stderr,
                 "[filter] no local DNS helper cgroup is present; a Connecting-with-floor "
                 "reconnect depends on the daemon's in-process resolver\n");
  } else {
    for (const auto& ref : found) {
      std::fprintf(stderr, "[filter] dns helper cgroup: %s (level %d)\n", ref.path.c_str(),
                   ref.level);
    }
  }
  return found;
}

// ---- egress self-exclusion at socket-creation time -------------------------

namespace {

// bpf(2) has no glibc wrapper. Four instructions and two commands do not
// justify a libbpf dependency, a shipped .o and a new packaging surface.
long BpfSyscall(int cmd, union bpf_attr* attr, unsigned int size) {
  return ::syscall(__NR_bpf, cmd, attr, size);
}

// The whole program:
//     r2 = kEgressMark
//     *(u32 *)(r1 + offsetof(struct bpf_sock, mark)) = r2
//     r0 = 1                       // 0 would REFUSE the socket creation
//     exit
// r1 is the struct bpf_sock context. `mark`, `priority` and `bound_dev_if` are
// the writable fields of that context for BPF_PROG_TYPE_CGROUP_SOCK; the
// kernel turns the store into a write of sk->sk_mark. offsetof() is taken from
// linux/bpf.h rather than hardcoded, so a context layout change is a compile
// error and not a silently misplaced store.
//
// Returning 1 is not decoration. For BPF_CGROUP_INET_SOCK_CREATE a return of 0
// makes socket() fail with -EPERM, i.e. the daemon would lose the ability to
// open any IP socket at all — which is the one bug in this file that would be
// worse than the one it fixes.
bpf_insn MakeInsn(uint8_t code, uint8_t dst, uint8_t src, int16_t off, int32_t imm) {
  bpf_insn insn{};
  insn.code = code;
  insn.dst_reg = dst & 0x0f;
  insn.src_reg = src & 0x0f;
  insn.off = off;
  insn.imm = imm;
  return insn;
}

constexpr uint8_t kBpfAluMovImm = 0xb7;  // BPF_ALU64 | BPF_MOV | BPF_K
constexpr uint8_t kBpfStxMemW = 0x63;    // BPF_STX   | BPF_MEM | BPF_W
constexpr uint8_t kBpfExit = 0x95;       // BPF_JMP   | BPF_EXIT

// Open a fresh AF_INET datagram socket and report the SO_MARK the kernel gave
// it. This is the read-back both Attach() and the witness use, so neither can
// drift from the other.
bool ReadFreshSocketMark(uint32_t* mark, std::string* error) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
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

}  // namespace

EgressSocketMarker::~EgressSocketMarker() { Detach(); }

bool EgressSocketMarker::SelfSocketMark(uint32_t* mark, std::string* error) {
  return ReadFreshSocketMark(mark, error);
}

bool EgressSocketMarker::Attach(const CgroupRef& cgroup, uint32_t mark, std::string* error) {
  const auto fail = [&](std::string message) {
    detail_ = std::move(message);
    if (error != nullptr) *error = detail_;
    return false;
  };

  if (!CgroupQuotable(cgroup) || !CgroupV2PathExists(cgroup.path)) {
    return fail("this process has no usable cgroup v2 directory, so its sockets cannot be "
                "marked at creation");
  }

  // Already attached: re-run the proof rather than stacking a second program.
  if (attached_) {
    uint32_t got = 0;
    std::string readError;
    if (ReadFreshSocketMark(&got, &readError) && got == mark) return true;
    Detach();
  }

  const bpf_insn program[] = {
      MakeInsn(kBpfAluMovImm, /*dst=*/2, 0, 0, static_cast<int32_t>(mark)),
      MakeInsn(kBpfStxMemW, /*dst=*/1, /*src=*/2,
               static_cast<int16_t>(offsetof(struct bpf_sock, mark)), 0),
      MakeInsn(kBpfAluMovImm, /*dst=*/0, 0, 0, 1),
      MakeInsn(kBpfExit, 0, 0, 0, 0),
  };

  // "Dual MPL/GPL" is in the kernel's license_is_gpl_compatible() list and is
  // the honest label for four instructions written in an MPL-2.0 file.
  char license[] = "Dual MPL/GPL";
  char progName[] = "urnw_egress";
  char log[4096];
  log[0] = '\0';

  union bpf_attr load {};
  load.prog_type = BPF_PROG_TYPE_CGROUP_SOCK;
  load.expected_attach_type = BPF_CGROUP_INET_SOCK_CREATE;
  load.insn_cnt = sizeof(program) / sizeof(program[0]);
  load.insns = reinterpret_cast<uint64_t>(program);
  load.license = reinterpret_cast<uint64_t>(license);
  load.log_level = 1;
  load.log_size = sizeof(log);
  load.log_buf = reinterpret_cast<uint64_t>(log);
  std::memcpy(load.prog_name, progName, sizeof(progName) > sizeof(load.prog_name)
                                            ? sizeof(load.prog_name)
                                            : sizeof(progName));

  const long loaded = BpfSyscall(BPF_PROG_LOAD, &load, sizeof(load));
  if (loaded < 0) {
    const int saved = errno;
    log[sizeof(log) - 1] = '\0';
    std::string why = std::string("bpf(BPF_PROG_LOAD): ") + std::strerror(saved);
    if (log[0] != '\0') why += std::string("; verifier: ") + Trim(log);
    return fail(std::move(why));
  }
  progFd_ = static_cast<int>(loaded);

  const std::string full = std::string("/sys/fs/cgroup/") + cgroup.path;
  cgroupFd_ = ::open(full.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (cgroupFd_ < 0) {
    const int saved = errno;
    Detach();
    return fail("could not open " + full + ": " + std::strerror(saved));
  }

  // BPF_F_ALLOW_MULTI so we neither clobber nor are clobbered by another
  // program on the same cgroup (systemd itself attaches cgroup programs).
  union bpf_attr attach {};
  attach.target_fd = static_cast<uint32_t>(cgroupFd_);
  attach.attach_bpf_fd = static_cast<uint32_t>(progFd_);
  attach.attach_type = BPF_CGROUP_INET_SOCK_CREATE;
  attach.attach_flags = BPF_F_ALLOW_MULTI;
  if (BpfSyscall(BPF_PROG_ATTACH, &attach, sizeof(attach)) < 0) {
    const int saved = errno;
    Detach();
    return fail(std::string("bpf(BPF_PROG_ATTACH) on ") + cgroup.path + ": " +
                std::strerror(saved));
  }
  attached_ = true;

  // PROVE IT. A loaded, attached program that does not actually run is exactly
  // the silent no-op this whole change exists to stop shipping, so the last
  // step is a measurement and not an assumption.
  uint32_t got = 0;
  std::string readError;
  if (!ReadFreshSocketMark(&got, &readError)) {
    Detach();
    return fail("the socket marker attached but could not be proven: " + readError);
  }
  if (got != mark) {
    Detach();
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "the socket marker attached but a fresh socket came back with mark 0x%08x, "
                  "not 0x%08x",
                  got, mark);
    return fail(buf);
  }

  char ok[192];
  std::snprintf(ok, sizeof(ok),
                "cgroup-bpf sock_create marker on %s: every socket this daemon opens now "
                "carries mark 0x%08x before connect()",
                cgroup.path.c_str(), mark);
  detail_ = ok;
  return true;
}

void EgressSocketMarker::Detach() {
  if (attached_ && cgroupFd_ >= 0 && progFd_ >= 0) {
    union bpf_attr detach {};
    detach.target_fd = static_cast<uint32_t>(cgroupFd_);
    detach.attach_bpf_fd = static_cast<uint32_t>(progFd_);
    detach.attach_type = BPF_CGROUP_INET_SOCK_CREATE;
    BpfSyscall(BPF_PROG_DETACH, &detach, sizeof(detach));
  }
  attached_ = false;
  if (cgroupFd_ >= 0) {
    ::close(cgroupFd_);
    cgroupFd_ = -1;
  }
  if (progFd_ >= 0) {
    ::close(progFd_);
    progFd_ = -1;
  }
}

// ---- nftables --------------------------------------------------------------

const char* ToString(FilterState s) {
  switch (s) {
    case FilterState::Off: return "off";
    case FilterState::Connecting: return "connecting";
    case FilterState::Armed: return "armed";
    case FilterState::Connected: return "connected";
  }
  return "off";
}

bool FloorForTransition(FilterState previous, FilterState next, bool killSwitchRequested) {
  switch (next) {
    case FilterState::Off:
      // A user disconnect ALWAYS lifts, even with the toggle on (Windows
      // parity). Only an unexpected drop goes to Armed.
      return false;
    case FilterState::Armed:
      // Structural: an Armed state without the floor is not a state, it is an
      // open machine with a label on it.
      return true;
    case FilterState::Connected:
      return killSwitchRequested;
    case FilterState::Connecting:
      // A first connect from a clean machine fails OPEN — a failed start must
      // never cut the user off the network. A reconnect made from Armed keeps
      // the floor it already had, because that is precisely the window the
      // kill switch exists to close.
      return killSwitchRequested && previous == FilterState::Armed;
  }
  return false;
}

namespace {

// The three prefixes the capture set deliberately EXCLUDES (CaptureV4Prefixes
// is their 31-prefix complement). Routes and firewall are built from ONE
// table: blocking these in the firewall while routing them out the physical
// NIC would make the LAN die with no error anywhere.
constexpr const char* kLanV4Set = "{ 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 }";

std::string CgroupMatch(const CgroupRef& ref) {
  return "socket cgroupv2 level " + std::to_string(ref.level) + " \"" + ref.path + "\"";
}

}  // namespace

std::string BuildNftRuleset(const FilterConfig& cfg) {
  // Off is the atomic no-op teardown: add (so the delete cannot fail on a
  // missing table — measured, a bare `delete table` is REFUSED) then delete.
  if (cfg.state == FilterState::Off) {
    return std::string("add table inet ") + kNftTableName + "\ndelete table inet " +
           kNftTableName + "\n";
  }

  const FilterDerived d = DeriveFilter(cfg);
  const std::string mark = MarkHex(cfg.mark);
  const char* policy = cfg.floor ? "drop" : "accept";
  const NftCgroupMode cgroupMode =
      SelectNftCgroupMode(cfg.socket_mark_proven, cfg.cgroup_socket_match_supported,
                          cfg.floor, !cfg.dns_helper_cgroups.empty());
  const bool cg = cgroupMode == NftCgroupMode::CgroupAndMark &&
                  CgroupQuotable(cfg.cgroup);

  std::string s;
  const auto line = [&s](const std::string& text) {
    s += text;
    s += '\n';
  };

  // Atomic swap idiom: declare, delete, recreate — all in one -f file, so
  // there is never an all-open or an all-blocked window between states.
  line(std::string("add table inet ") + kNftTableName);
  line(std::string("delete table inet ") + kNftTableName);
  line(std::string("table inet ") + kNftTableName + " {");

  // Named counter objects, so every reader finds them by NAME. A positional
  // parse of a chain listing (which is what the shipped build did) inverts
  // silently the day nft reorders its output or a rule is inserted above.
  if (cg) {
    line(std::string("\tcounter ") + kNftMarkTotalCounter + " {");
    line("\t}");
    line(std::string("\tcounter ") + kNftMarkDaemonCounter + " {");
    line("\t}");
  }
  if (d.tun_named) {
    line(std::string("\tcounter ") + kNftProbeTunCounter + " {");
    line("\t}");
    line(std::string("\tcounter ") + kNftProbePhyCounter + " {");
    line("\t}");
  }
  if (cg || d.tun_named) line("");

  // -- 0. EGRESS SELF-EXCLUSION (R4), SECOND LAYER. The FIRST layer is
  //    EgressSocketMarker, which puts the mark on the socket at socket()
  //    time — before connect(), which is the only moment at which it can stop
  //    the tun route (and the tun SOURCE ADDRESS) from being chosen at all.
  //
  //    This chain is the belt. `type route` so the mark triggers the routing
  //    re-lookup for the packet the daemon just produced; the paired `ip rule`
  //    sends anything NOT carrying this mark into the tunnel table. What it
  //    CANNOT do is undo a source address connect() already bound:
  //    ip_route_me_harder() sets FLOWI_FLAG_ANYSRC for an RTN_LOCAL saddr and
  //    reuses it, so a socket that resolved the tun first leaves the physical
  //    NIC sourced from the tunnel's own address and never hears back. That is
  //    why this is no longer the mechanism, and why nothing here is believed
  //    without the packet witness.
  //
  //    The bare `counter name` on the first line is the DENOMINATOR. Without
  //    it the chain reported matches with nothing to compare them against, and
  //    "egress mark applied to 2 packet(s)" was equally consistent with a rule
  //    that worked and a rule that was dead — which is exactly how it was read.
  //
  //    FOR THE RECORD, because the incident write-up got this backwards: the
  //    "2 packets" line was NOT two packets in forty minutes. The owner's own
  //    journal puts `[filter] connecting`, `egress split verified`, `egress
  //    mark applied to 2 packet(s)` and `[tun] up` ALL AT 19:15:22 — the
  //    counter was read under a second after the chain was created, and the
  //    table it belonged to was destroyed by the Connected apply moments
  //    later. Two packets in under a second says nothing either way. Nothing
  //    ever read the counter of the ruleset the tunnel actually ran under, and
  //    that — not the number — is the defect.
  line(std::string("\tchain ") + kNftMarkChainName + " {");
  line("\t\ttype route hook output priority " + std::to_string(kMarkChainPriority) +
       "; policy accept;");
  if (cg) {
    line(std::string("\t\tcounter name \"") + kNftMarkTotalCounter + "\"");
    line("\t\t" + CgroupMatch(cfg.cgroup) + " counter name \"" + kNftMarkDaemonCounter +
         "\" meta mark set " + mark);
  }
  line("\t}");
  line("");

  // -- 0b. THE INSTRUMENT. Counts only: policy accept, no verdict on any rule,
  //    so this chain cannot change the fate of a single packet.
  //
  //    IT MATCHES THE MARK, NOT A PROBE DESTINATION, AND THAT IS THE WHOLE
  //    REASON THE FLOOR NO LONGER CARRIES A HOLE. The previous cut matched
  //    `ip daddr 192.0.2.1 udp dport 9`, which meant the witness had to EMIT
  //    datagrams to that address, which in turn meant urnw_out needed a
  //    standing `accept` for them — a permanent hole in the kill-switch floor,
  //    installed for as long as the table existed, so that a measurement could
  //    be taken twice a minute. These two rules watch the daemon's OWN REAL
  //    TRAFFIC instead (every packet leaving a socket that carries kEgressMark,
  //    whether the mark came from the sock_create program or from the belt
  //    chain above), so the witness reads a counter the kernel is already
  //    keeping and sends nothing at all. No emission, therefore no permit.
  //
  //    urnw_probe_tun IS THE STORM SIGNATURE, EXACTLY. A marked daemon packet
  //    leaving through our own tun is the packet the SDK's IoLoop reads back
  //    out and re-sends: on 2026-08-15 that loop ran forty minutes at 1.34 Gbps
  //    with 0 in. It must be zero for the life of the ruleset, and one is the
  //    whole defect.
  //
  //    POSTROUTING, NOT OUTPUT, AND THIS IS LOAD-BEARING. nf_hook_state.out is
  //    captured from skb_dst(skb)->dev before the output hooks run, so at the
  //    output hook a packet the mark chain successfully re-routed STILL reports
  //    the tun. Counting the interface at the output hook would have rebuilt
  //    the original false-confidence check in a new place.
  //
  //    `lo` is excluded from the physical leg on purpose: the daemon's own
  //    device RPC on 127.0.0.1 is marked like everything else it opens, and
  //    counting it would inflate "left the machine properly" with traffic that
  //    never left the machine.
  if (d.tun_named) {
    line(std::string("\tchain ") + kNftProbeChainName + " {");
    line("\t\ttype filter hook postrouting priority " + std::to_string(kMarkChainPriority) +
         "; policy accept;");
    line("\t\tmeta mark " + mark + " oifname \"" + cfg.tun_name + "\" counter name \"" +
         kNftProbeTunCounter + "\"");
    line("\t\tmeta mark " + mark + " oifname != \"" + cfg.tun_name +
         "\" oifname != \"lo\" counter name \"" + kNftProbePhyCounter + "\"");
    line("\t}");
    line("");
  }

  // -- 1. EGRESS. Order is load-bearing: it is the Windows Baseline/Dns
  //    two-sublayer trick flattened into one chain.
  line(std::string("\tchain ") + kNftOutChainName + " {");
  line(std::string("\t\ttype filter hook output priority ") +
       std::to_string(kFilterChainPriority) + "; policy " + policy + ";");
  line("");
  // Rank 1: our own sockets. By CGROUP first — SO_MARK needs CAP_NET_ADMIN, so
  // a privileged third party could forge the mark and inherit our exemption,
  // while cgroup membership cannot be forged. Without this rule the machine is
  // armed, blocked and structurally unable to reconnect.
  if (cg) {
    line("\t\t" + CgroupMatch(cfg.cgroup) + " counter accept");
  }
  line("\t\tmeta mark " + mark + " counter accept");
  // Loopback: the device RPC on 127.0.0.1, local stub resolvers, dev servers.
  // Linux routes host-address -> own-other-host-address via `lo`, so this is
  // the exact analogue of matching FWP_CONDITION_FLAG_IS_LOOPBACK rather than
  // 127/8.
  line("\t\toifname \"lo\" counter accept");
  // THE WITNESS PERMIT IS GONE, AND NOTHING REPLACED IT. Builds before this one
  // carried `ip daddr 192.0.2.1 udp dport 9 counter accept` here — a standing
  // accept in the kill-switch floor, present for as long as the table was
  // installed, whose only purpose was to let a synthetic probe reach a counter.
  // The witness no longer sends anything (Tunnel::VerifyEgressWitness measures
  // two connect() calls and reads the mark counters above), so there is no
  // packet left to permit. A hole that exists only to make a measurement
  // possible is a hole the measurement should not have needed.
  line("");
  // The cloud metadata service: never, in any state, on any interface. The
  // capture set routes 169.254/16 into the tunnel on purpose, so permitting
  // link-local in the firewall (as the first cut did) re-opened this.
  line("\t\tip daddr 169.254.169.254 counter drop");
  line("");

  // The Connecting-with-floor DNS path. On a systemd-resolved host the SDK's
  // wire query leaves RESOLVED's cgroup, not ours — the Linux reappearance of
  // the Windows svchost/Dnscache problem. Scoped to one service instead of
  // Windows' address-scoped machine-wide permit, and bounded by the caller's
  // kConnectingWindowSeconds watchdog. Armed itself carries NO DNS permit:
  // nothing on the machine resolves, which is what the state means.
  if (d.helper_dns) {
    for (const auto& helper : cfg.dns_helper_cgroups) {
      if (!CgroupQuotable(helper)) continue;
      line("\t\t" + CgroupMatch(helper) + " udp dport 53 counter accept");
      line("\t\t" + CgroupMatch(helper) + " tcp dport 53 counter accept");
    }
    line("");
  }

  if (d.dns_floor) {
    std::string set = "{ ";
    for (size_t i = 0; i < d.resolvers.size(); ++i) {
      if (i != 0) set += ", ";
      set += d.resolvers[i];
    }
    set += " }";
    // Pin :53 to the tunnel's own resolvers over the tun ONLY (Windows filter
    // 10), then close every other name channel. This MUST sit above the LAN
    // permit or that permit re-opens the router's resolver at 192.168.1.1:53 —
    // the exact hole the separate Dns sublayer exists to close.
    line("\t\toifname \"" + cfg.tun_name + "\" ip daddr " + set + " udp dport 53 counter accept");
    line("\t\toifname \"" + cfg.tun_name + "\" ip daddr " + set + " tcp dport 53 counter accept");
    // reject for unicast 53/853 so a stray query fails in milliseconds; drop
    // for the multicast-destined channels, where no ICMP error is generated
    // anyway.
    line("\t\tudp dport 53 counter reject");
    line("\t\ttcp dport 53 counter reject");
    line("\t\tudp dport 853 counter reject");
    line("\t\ttcp dport 853 counter reject");
    line("\t\tudp dport 5353 counter drop");
    line("\t\tudp dport 5355 counter drop");
    line("\t\ttcp dport 5355 counter drop");
    line("\t\tudp dport { 137, 138 } counter drop");
    line("\t\ttcp dport 139 counter drop");
    line("");
  }

  // The rest of the tunnel — AFTER the DNS rules, so DNS is pinned rather than
  // "anything over the tun". By NAME, so it can be installed before the tun
  // exists.
  if (d.tun_named) {
    line("\t\toifname \"" + cfg.tun_name + "\" counter accept");
    line("");
  }

  if (d.block_v6) {
    // Permitted because the machine hangs otherwise: NDP/DAD/RS on the link,
    // link-scope multicast (including the whole solicited-node block DAD
    // depends on), site-scope all-DHCP-servers, DHCPv6 and ULA. Everything
    // else — all global unicast — is refused.
    line("\t\tip6 daddr fe80::/10 counter accept");
    line("\t\tip6 daddr ff02::/16 counter accept");
    line("\t\tip6 daddr ff05::1:3 counter accept");
    line("\t\tip6 daddr fc00::/7 counter accept");
    line("\t\tudp sport 546 udp dport 547 counter accept");
    // reject on OUTPUT: the ICMPv6 admin-prohibited is generated locally and
    // delivered to the local socket, never onto the wire, so connect() fails
    // in milliseconds and Happy Eyeballs falls back to v4 at once.
    line("\t\tmeta nfproto ipv6 counter reject");
    line("");
  }

  // DHCPv4. Precise sport/dport pairs, not a bare `dport {67,68}`. The initial
  // DISCOVER/OFFER uses AF_PACKET and never reaches this hook; what this rule
  // keeps alive is the unicast RENEW. Without it everything works for hours
  // and then the lease dies — a delayed failure nobody attributes to the VPN.
  // The server direction is kept for a host running a libvirt/hotspot bridge.
  line("\t\tudp sport 68 udp dport 67 counter accept");
  line("\t\tudp sport 67 udp dport 68 counter accept");
  line("");

  if (cfg.allow_lan) {
    line(std::string("\t\tip daddr ") + kLanV4Set + " counter accept");
    // 224.0.0.0/24 is the link-local CONTROL block, not the whole /4: a
    // blanket /4 permit re-opens SSDP 239.255.255.250:1900, which broadcasts
    // the device name and model — an identity leak the port blocks miss.
    line("\t\tip daddr 224.0.0.0/24 counter accept");
    line("\t\tip protocol igmp counter accept");
    line("");
  }

  if (cfg.floor) line("\t\tcounter reject");
  line("\t}");
  line("");

  // -- 2. INGRESS. Blocked too, not just egress: a listening service on the
  //    box would otherwise stay reachable at the user's real global v6 address
  //    while they believe they are tunnelled (Windows' ALE_AUTH_RECV_ACCEPT_V6
  //    block).
  line(std::string("\tchain ") + kNftInChainName + " {");
  line(std::string("\t\ttype filter hook input priority ") +
       std::to_string(kFilterChainPriority) + "; policy " + policy + ";");
  line("");
  line("\t\tct state established,related counter accept");
  line("\t\tiifname \"lo\" counter accept");
  if (d.tun_named) {
    line("\t\tiifname \"" + cfg.tun_name + "\" counter accept");
  }
  line("");
  if (d.block_v6) {
    line("\t\tip6 saddr fe80::/10 counter accept");
    line("\t\tip6 daddr ff02::/16 counter accept");
    line("\t\tip6 saddr fc00::/7 counter accept");
    line("\t\tudp sport 547 udp dport 546 counter accept");
    // drop, NOT reject, on the way in: a reject would emit a packet FROM the
    // machine's real IPv6 address, which is a beacon — exactly the
    // deanonymisation the block exists to prevent.
    line("\t\tmeta nfproto ipv6 counter drop");
    line("");
  }
  line("\t\tudp sport 67 udp dport 68 counter accept");
  line("\t\tudp sport 68 udp dport 67 counter accept");
  line("");
  if (cfg.allow_lan) {
    line(std::string("\t\tip saddr ") + kLanV4Set + " counter accept");
    line("\t\tip daddr 224.0.0.0/24 counter accept");
    line("\t\tip protocol igmp counter accept");
    line("");
  }
  if (cfg.floor) line("\t\tcounter drop");
  line("\t}");
  line("");

  // -- 3. FORWARD. NOT scope creep: the output hook only sees LOCALLY
  //    GENERATED packets, so a docker/podman/libvirt bridge, a VM or a phone
  //    hotspot share would leave via the physical NIC completely untouched by
  //    the egress chain while the machine is armed.
  line(std::string("\tchain ") + kNftFwdChainName + " {");
  line(std::string("\t\ttype filter hook forward priority ") +
       std::to_string(kFilterChainPriority) + "; policy " + policy + ";");
  line("");
  line("\t\tct state established,related counter accept");
  if (d.tun_named) {
    line("\t\toifname \"" + cfg.tun_name + "\" counter accept");
    line("\t\tiifname \"" + cfg.tun_name + "\" counter accept");
  }
  line("");
  if (d.block_v6) {
    line("\t\tip6 daddr fe80::/10 counter accept");
    line("\t\tip6 daddr ff02::/16 counter accept");
    line("\t\tip6 daddr fc00::/7 counter accept");
    line("\t\tmeta nfproto ipv6 counter drop");
    line("");
  }
  if (cfg.allow_lan) {
    line(std::string("\t\tip daddr ") + kLanV4Set + " counter accept");
    line("");
  }
  if (cfg.floor) line("\t\tcounter drop");
  line("\t}");
  line("}");
  return s;
}

bool RulesetHasBlockFloor(const FilterConfig& cfg) {
  return cfg.state != FilterState::Off && cfg.floor;
}

bool RulesetBlocksIpv6(const FilterConfig& cfg) { return DeriveFilter(cfg).block_v6; }

bool RulesetPinsDns(const FilterConfig& cfg) { return DeriveFilter(cfg).dns_floor; }

bool RulesetOpensHelperDns(const FilterConfig& cfg) { return DeriveFilter(cfg).helper_dns; }

bool IsIpv6OnlyNetwork(std::string* detail) {
  const std::string ip = FindTool("ip");
  // Unknown is NOT the refusal: Tunnel::Open already refuses outright when
  // iproute2 is missing, and guessing here would block the reaper from arming.
  if (ip.empty()) return false;
  const CommandResult v4 = RunCommand({ip, "-4", "route", "show", "default", "table", "all"});
  const CommandResult v6 = RunCommand({ip, "-6", "route", "show", "default", "table", "all"});
  if (!v4.ok() || !v6.ok()) return false;
  const bool haveV4 = !v4.output.empty();
  const bool haveV6 = !v6.output.empty();
  // No default route at all is the link having just gone away — precisely when
  // arming must still work. Only "v6 yes, v4 no" is the refusal.
  if (haveV4 || !haveV6) return false;
  if (detail != nullptr) {
    *detail = "this network has an IPv6 default route and no IPv4 one";
  }
  return true;
}

namespace {

// ONE teardown transaction, in one place, so ~NetFilter, Remove() and the
// startup sweep can never emit three subtly different deletes. add-then-delete:
// a bare `delete table` on a missing table is REFUSED (measured), so this makes
// "the table was never there" a success and the whole thing idempotent.
std::string NftDeleteTableScript() {
  return std::string("add table inet ") + kNftTableName + "\ndelete table inet " + kNftTableName +
         "\n";
}

// `nft -f` is transactional: a failure means NOTHING changed and whatever was
// in force still is. That makes a failed teardown a real, live block, so it is
// worth a bounded retry before reporting it — the realistic transient cause is
// another process holding the nftables transaction lock. Bounded, never
// unbounded: this runs on the shutdown path.
constexpr int kNftAttempts = 3;
constexpr int kNftRetryMillis = 150;

CommandResult RunNftScript(const std::string& nft, const std::string& script) {
  CommandResult r;
  for (int attempt = 0; attempt < kNftAttempts; ++attempt) {
    r = RunCommand({nft, "-f", "-"}, script);
    if (r.ok()) return r;
    if (attempt + 1 < kNftAttempts) SleepMillis(kNftRetryMillis);
  }
  return r;
}

// `ip rule add` installs a DUPLICATE rather than answering EEXIST, so every
// delete is a loop. Bounded so a pathological table cannot spin the daemon;
// hitting the bound is reported rather than swallowed.
constexpr int kMaxDuplicatePolicyRules = 16;

// Deletes every copy of the RETIRED suppress rule
//     ip rule pref 32762 table main suppress_prefixlength 0
// which builds before this one installed AHEAD of the capture rule (see
// kSuppressRulePriority). It is no longer installed by anything here; this
// sweeps a copy left by an older build or by an unclean exit, so an upgrade
// closes the bypass instead of inheriting it. Never deletes by priority alone —
// the full selector is matched, so a co-installed tool's rule at the same
// priority is not ours to remove.
void DeleteRetiredSuppressRules(const std::string& ip) {
  if (ip.empty()) return;
  for (int i = 0; i < kMaxDuplicatePolicyRules; ++i) {
    const CommandResult r =
        RunCommand({ip, "-4", "rule", "delete", "pref", std::to_string(kSuppressRulePriority),
                    "table", "main", "suppress_prefixlength", "0"});
    if (!r.ok()) break;
    std::fprintf(stderr, "[tun] removed a retired pref %d suppress_prefixlength rule\n",
                 kSuppressRulePriority);
  }
}

// ---- DNS takeover: file-local machinery ------------------------------------
//
// Everything here is syscall-level on purpose. The tier-3 path writes a file
// the whole machine depends on to resolve anything at all, so "mostly worked"
// is not a state it is allowed to end in: every write is a temp file plus a
// rename (which is atomic, and which replaces a SYMLINK rather than following
// it), every takeover records its exact undo on disk before it mutates
// anything, and every step that cannot be completed rolls back what it did.

constexpr const char* kResolvConfPath = "/etc/resolv.conf";
constexpr const char* kResolvConfBackupPath = "/etc/resolv.conf.urnetwork-backup";
constexpr const char* kResolvConfStatePath = "/etc/resolv.conf.urnetwork-state";
// STABLE ACROSS VERSIONS, DELIBERATELY. Changing this string would make a new
// daemon fail to recognise an old daemon's file as its own and refuse to
// restore it — leaving the user's original in a backup nothing puts back.
constexpr const char* kResolvConfMarker =
    "# Managed by urnetworkd (URnetwork): the tunnel owns this file while it is up.";
// systemd-resolved's RuntimeDirectory=. systemd creates it when the unit starts
// and REMOVES it when the unit stops, so its presence answers "is resolved
// running" with one stat() and cannot be confused with "the systemd package
// ships resolvectl" — which is the exact confusion that made Arch leak.
constexpr const char* kResolvedRuntimeDir = "/run/systemd/resolve";
constexpr const char* kResolvedStubAddress = "127.0.0.53";
// glibc's MAXNS. Nameservers past the third are silently ignored, and a
// resolver the user believes is in force but is not is the same class of defect
// as the one this whole section exists to close.
constexpr size_t kMaxResolvConfNameservers = 3;
// Byte-identical to ctl::kCodeDnsApplyFailed, like every other code literal in
// this file: the header stays free of ControlProtocol.hpp.
constexpr const char* kCodeDnsApplyFailed = "dns_apply_failed";
// The documented development escape from the fail-closed refusal, spelled like
// URNETWORK_ALLOW_UNPROTECTED_EGRESS so there is one idiom and not two.
constexpr const char* kAllowUnprotectedDnsEnv = "URNETWORK_ALLOW_UNPROTECTED_DNS";
// Pins the tier for a test run: "resolved" | "resolvconf" | "file". Anything
// else is refused loudly rather than silently ignored. It exists because the
// three tiers cannot all be exercised on one machine — this host has resolved
// and can therefore never reach tiers 2 or 3 by itself.
constexpr const char* kDnsBackendEnv = "URNETWORK_DNS_BACKEND";

std::string PathDirName(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  // Follows symlinks on purpose: for /etc/resolv.conf the question this answers
  // is always "what does a resolver actually read", never "what is at the path".
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

// A rename() is only atomic once the new file's bytes are on the disk; without
// this a power cut between the two leaves a zero-length /etc/resolv.conf, which
// is a machine that resolves nothing and shows no reason why.
void FsyncPath(const std::string& path, bool directory) {
  const int fd = ::open(path.c_str(), (directory ? O_RDONLY | O_DIRECTORY : O_RDONLY) | O_CLOEXEC);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

bool WriteAllFd(int fd, const std::string& data) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

// Temp file + fsync + rename, in the SAME directory so the rename cannot cross
// a filesystem. rename() over a symlink replaces the LINK, which is the whole
// reason tier 3 can take over an /etc/resolv.conf that points at
// /run/systemd/resolve/stub-resolv.conf without writing a byte into resolved's
// own file.
//
// inPlaceFallbackOk is for ONE case and the caller must have proven it: a
// /etc/resolv.conf that is a BIND MOUNT (every podman/docker container), where
// rename() answers EBUSY and the only way to change the file is to truncate and
// rewrite it. That write is not atomic — a reader can catch it empty — so it is
// never used for a path that is a symlink (it would follow the link into
// another daemon's file) and it is logged when it happens.
bool WriteFileAtomic(const std::string& path, const std::string& data, mode_t mode,
                     bool inPlaceFallbackOk, std::string* error) {
  const auto fail = [error](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  const std::string dir = PathDirName(path);
  const std::string tmpl = dir + "/.urnetwork-resolv-XXXXXX";
  std::vector<char> name(tmpl.begin(), tmpl.end());
  name.push_back('\0');
  const int fd = ::mkstemp(name.data());
  if (fd < 0) {
    const int e = errno;
    return fail("could not create a temporary file in " + dir + ": " + std::strerror(e) +
                (e == EROFS ? " (the filesystem is read-only)" : ""));
  }
  bool ok = WriteAllFd(fd, data);
  // mkstemp() creates 0600. WITHOUT this chmod /etc/resolv.conf becomes
  // root-only and every unprivileged process on the machine stops resolving —
  // a self-inflicted outage that looks exactly like the leak fix failing.
  if (ok && ::fchmod(fd, mode) != 0) ok = false;
  if (ok && ::fsync(fd) != 0) ok = false;
  const int closeRc = ::close(fd);
  if (ok && closeRc != 0) ok = false;
  if (!ok) {
    const int e = errno;
    ::unlink(name.data());
    return fail("could not write " + dir + "/(temporary): " + std::strerror(e));
  }
  if (::rename(name.data(), path.c_str()) == 0) {
    FsyncPath(dir, /*directory=*/true);
    return true;
  }
  const int e = errno;
  ::unlink(name.data());
  if (!inPlaceFallbackOk || (e != EBUSY && e != EXDEV && e != EINVAL)) {
    std::string hint;
    if (e == EROFS) hint = " (the filesystem is read-only)";
    if (e == EPERM || e == EACCES) {
      hint = " (an immutable attribute — `lsattr` / `chattr -i` — or a mandatory access control "
             "policy can produce this even as root)";
    }
    if (e == EBUSY) hint = " (the path is a mount point: a bind-mounted /etc/resolv.conf)";
    return fail("could not replace " + path + ": " + std::strerror(e) + hint);
  }
  // The bind-mount case. Truncate and rewrite the file the mount points at.
  std::fprintf(stderr,
               "[dns] %s cannot be replaced atomically (%s), so it is being rewritten in place; a "
               "reader can briefly see it empty\n",
               path.c_str(), std::strerror(e));
  const int direct = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
  if (direct < 0) return fail("could not open " + path + " to rewrite it: " + std::strerror(errno));
  ok = WriteAllFd(direct, data);
  if (ok) ok = (::fsync(direct) == 0);
  const int rc2 = ::close(direct);
  if (!ok || rc2 != 0) return fail("could not rewrite " + path + ": " + std::strerror(errno));
  return true;
}

// The recorded undo. Written BEFORE /etc/resolv.conf is touched, so a crash in
// the middle leaves a state file describing a takeover that may not have
// happened — which the restore detects by looking for our marker — rather than
// a takeover with no record, which would lose the user's file permanently.
struct DnsTakeoverState {
  bool present = false;
  std::string backend;  // "file" (we own /etc/resolv.conf) | "resolvconf"
  std::string kind;     // "symlink" | "file" | "absent" (what was there before)
  std::string target;   // the symlink's target, verbatim
  mode_t mode = 0644;   // the original file's mode
  std::string iface;    // the tun, and the name registered with resolvconf
};

bool ReadTakeoverState(DnsTakeoverState* st) {
  std::string text;
  if (!ReadWholeFile(kResolvConfStatePath, &text)) return false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = Trim(line.substr(eq + 1));
    if (key == "backend") st->backend = value;
    else if (key == "kind") st->kind = value;
    else if (key == "target") st->target = value;
    else if (key == "iface") st->iface = value;
    else if (key == "mode") st->mode = static_cast<mode_t>(std::strtoul(value.c_str(), nullptr, 8));
  }
  st->present = true;
  return true;
}

bool WriteTakeoverState(const DnsTakeoverState& st, std::string* error) {
  char modeBuf[16];
  std::snprintf(modeBuf, sizeof(modeBuf), "%04o", static_cast<unsigned>(st.mode & 07777));
  std::string text =
      "# urnetworkd DNS takeover state. While this file exists, urnetworkd has\n"
      "# changed how this machine resolves names and has NOT put it back yet.\n"
      "# `sudo urnetworkd --revert` restores it. Do not edit by hand.\n";
  text += "version=1\n";
  text += "backend=" + st.backend + "\n";
  text += "kind=" + st.kind + "\n";
  text += "target=" + st.target + "\n";
  text += "mode=" + std::string(modeBuf) + "\n";
  text += "iface=" + st.iface + "\n";
  text += "backup=" + std::string(kResolvConfBackupPath) + "\n";
  // 0644, not 0600: RecoveryHelpText() reads this to print the manual way out,
  // and main.cpp prints that from `--help`, which a user runs unprivileged.
  return WriteFileAtomic(kResolvConfStatePath, text, 0644, /*inPlaceFallbackOk=*/false, error);
}

void ClearTakeoverState() {
  ::unlink(kResolvConfStatePath);
  ::unlink(kResolvConfBackupPath);
  FsyncPath(PathDirName(kResolvConfStatePath), /*directory=*/true);
}

// A takeover record left behind by a daemon that was SIGKILLed, or by a start
// that died between the record and the mutation it describes. Finish it BEFORE
// any tier claims DNS again.
//
// BOTH LOWER TIERS SHARE THIS ONE RECORD, which is why this cannot live inside
// either of them: writing a new record over an old one strands whatever the old
// one described, and if the old one was a tier-3 file takeover, the user's real
// /etc/resolv.conf ends up referenced by nothing and is gone for good. This is
// the only moment at which it is still recoverable.
bool FinishLeftoverTakeover(std::string* reason) {
  DnsTakeoverState leftover;
  if (!ReadTakeoverState(&leftover)) return true;
  std::string detail;
  const bool restored = RestoreDirectResolvConf(&detail);
  std::fprintf(stderr, "[dns] a previous DNS takeover was never undone (%s left behind); %s: %s\n",
               kResolvConfStatePath, restored ? "restored it first" : "COULD NOT restore it",
               detail.c_str());
  if (restored) return true;
  if (reason != nullptr) {
    *reason = "a previous DNS takeover could not be undone (" + detail +
              "), so this one is refused rather than losing the original";
  }
  return false;
}

// Does the file a resolver would actually read carry this nameserver? Line
// oriented, so "10.0.0.1" does not match a comment mentioning 110.0.0.10.
bool ResolvConfHasNameserver(const std::string& content, const std::string& address) {
  std::istringstream in(content);
  std::string line;
  while (std::getline(in, line)) {
    const std::string t = Trim(line);
    if (t.rfind("nameserver", 0) != 0) continue;
    // The separator is REQUIRED. Without this check "nameserver10.0.0.1" — a
    // line glibc ignores outright — satisfies the read-back verification that
    // tiers 2 and 3 use to decide the resolver landed, so a malformed write
    // would be reported as a successful DNS takeover.
    const std::string rest = t.substr(10);
    if (!rest.empty() && !std::isspace(static_cast<unsigned char>(rest.front()))) continue;
    if (Trim(rest) == address) return true;
  }
  return false;
}

// /etc/nsswitch.conf's `hosts:` line lists `resolve` before `dns`. Bracketed
// action groups ([!UNAVAIL=return]) are skipped rather than counted, and a
// group may legally contain spaces.
bool NssResolveBeforeDns(const std::string& nsswitch) {
  std::istringstream in(nsswitch);
  std::string line;
  while (std::getline(in, line)) {
    if (const size_t hash = line.find('#'); hash != std::string::npos) line = line.substr(0, hash);
    std::string t = Trim(line);
    if (t.rfind("hosts:", 0) != 0) continue;
    t = t.substr(6);
    int resolveAt = -1;
    int dnsAt = -1;
    int index = 0;
    size_t i = 0;
    while (i < t.size()) {
      while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
      if (i >= t.size()) break;
      if (t[i] == '[') {
        while (i < t.size() && t[i] != ']') ++i;
        if (i < t.size()) ++i;
        continue;
      }
      const size_t start = i;
      while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) && t[i] != '[') ++i;
      const std::string token = t.substr(start, i - start);
      if (token == "resolve" && resolveAt < 0) resolveAt = index;
      if (token == "dns" && dnsAt < 0) dnsAt = index;
      ++index;
    }
    if (resolveAt < 0) return false;
    return dnsAt < 0 || resolveAt < dnsAt;
  }
  return false;
}

// A path is safe to print inside a single-quoted shell suggestion. We never
// exec any of this (RunCommand takes an argv and never a shell), so this is
// only about not handing a user a command that does something other than what
// it reads as.
bool SafeInSingleQuotes(const std::string& value) {
  if (value.empty() || value.size() > 4096) return false;
  for (const unsigned char c : value) {
    if (c < 0x20 || c == 0x7f || c == '\'') return false;
  }
  return true;
}

// The exact commands that undo a tier-2/tier-3 takeover BY HAND, built from the
// recorded state so they can never name the wrong target. Empty when nothing is
// owned — which is the normal case, and why RecoveryHelpText stays unchanged on
// a machine that never reached tier 3.
std::vector<std::string> ManualResolvConfRestoreCommands() {
  DnsTakeoverState st;
  if (!ReadTakeoverState(&st)) return {};
  std::vector<std::string> out;
  if (st.backend == "resolvconf") {
    if (!SafeInSingleQuotes(st.iface)) return {};
    out.push_back("sudo resolvconf -d '" + st.iface + "'");
    out.push_back("sudo resolvconf -u");
  } else if (st.kind == "symlink") {
    if (!SafeInSingleQuotes(st.target)) return {};
    out.push_back("sudo ln -sfn '" + st.target + "' " + kResolvConfPath);
  } else if (st.kind == "absent") {
    out.push_back(std::string("sudo rm -f ") + kResolvConfPath);
  } else {
    out.push_back(std::string("sudo mv -f ") + kResolvConfBackupPath + " " + kResolvConfPath);
  }
  out.push_back(std::string("sudo rm -f ") + kResolvConfStatePath + " " + kResolvConfBackupPath);
  return out;
}

}  // namespace

NetFilter::~NetFilter() {
  // Teardown on EVERY exit path, including the ones that got here by throwing:
  // Remove() is the add-then-delete idiom, so it is idempotent and a table
  // that was never installed is success.
  if (state_ == FilterState::Off) return;
  std::string error;
  if (Remove(&error)) return;
  // Remove() has already logged the failure and the whole way out. Add the one
  // thing it cannot know: this object is being destroyed, so that was the LAST
  // retry — nothing in this process will lift the table now.
  std::fprintf(stderr,
               "[filter] that was the last teardown attempt (the filter is being destroyed); "
               "`table inet %s` is left in the kernel with floor=%d\n",
               kNftTableName, floor_ ? 1 : 0);
}

const char* NetFilter::RecoveryCommand() {
  // Built from kNftTableName so the printed command can never drift from the
  // table it is supposed to delete.
  static const std::string kCommand = std::string("sudo nft delete table inet ") + kNftTableName;
  return kCommand.c_str();
}

std::vector<std::string> NetFilter::RecoveryCommands() {
  // Every one of these is built from the same constants the installers use, so
  // a printed recovery can never name a table, a rule or a path this daemon
  // does not actually create. [0] is RecoveryCommand() verbatim — the one that
  // lifts the block; the rest clear the routing policy and the crash-restart
  // marker, so the next start comes up open instead of re-arming.
  const std::string table = std::to_string(kTunnelRouteTable);
  return {
      RecoveryCommand(),
      "sudo ip -4 rule delete table " + table,
      "sudo ip -4 route flush table " + table,
      std::string("sudo rm -f ") + kArmedMarkerPath,
  };
}

std::string NetFilter::RecoveryHelpText() {
  std::string s =
      "If the URnetwork kill switch is blocking this machine and the daemon cannot lift it:\n"
      "  1. turn the kill switch off in the URnetwork app. It reaches the daemon over a unix\n"
      "     socket, which is never routed, so no ruleset this daemon installs can block it.\n"
      "  2. otherwise, as root:\n";
  for (const auto& command : RecoveryCommands()) {
    s += "       ";
    s += command;
    s += '\n';
  }
  s += "     The first command alone restores the network. ";
  s += kArmedMarkerPath;
  s += " is what makes a\n     crash-restart come back armed, so remove it too if the block "
       "returns by itself.";
  // ONLY when it applies. A machine that never reached tier 3 of the DNS
  // takeover (every systemd-resolved host, including this build's own CI) gets
  // exactly the text it got before, so this cannot add noise to the common
  // case — and a machine that DID reach it gets the one command a user cannot
  // possibly derive, because only the state file knows whether the original was
  // a symlink and to what.
  if (const std::vector<std::string> dns = ManualResolvConfRestoreCommands(); !dns.empty()) {
    s += "\n\nURnetwork is also currently managing this machine's DNS (";
    s += kResolvConfStatePath;
    s += " exists).\n`sudo urnetworkd --revert` puts it back. By hand, as root:\n";
    for (const auto& command : dns) {
      s += "       ";
      s += command;
      s += '\n';
    }
    s.pop_back();
  }
  return s;
}

const char* NetFilter::ArmedMarkerPath() { return kArmedMarkerPath; }

bool NetFilter::ApplyScript(const std::string& script, const char* what, std::string* error) {
  const std::string nft = FindTool("nft");
  if (nft.empty()) {
    lastError_ = "nftables (nft) is not installed";
    lastErrorCode_ = kFilterCodeNftMissing;
    if (error) *error = lastError_;
    return false;
  }
  const CommandResult r = RunCommand({nft, "-f", "-"}, script);
  if (!r.ok()) {
    // nft -f is transactional: on failure the PREVIOUS ruleset is still in
    // force, so state_ is deliberately left alone. r.Describe() carries nft's
    // own stderr verbatim.
    lastError_ = std::string("nft -f (") + what + "): " + r.Describe();
    lastErrorCode_ = kFilterCodeNftRejected;
    if (error) *error = lastError_;
    return false;
  }
  lastError_.clear();
  lastErrorCode_ = "";
  return true;
}

bool NetFilter::CheckRuleset(const std::string& script, std::string* error) {
  const std::string nft = FindTool("nft");
  if (nft.empty()) {
    if (error) *error = "nftables (nft) is not installed";
    return false;
  }
  // --check parses and EVALUATES without committing: the only safe way to find
  // out whether a ruleset would load, since the alternative is discovering it
  // while it is the thing standing between the user and their network.
  const CommandResult r = RunCommand({nft, "--check", "-f", "-"}, script);
  if (!r.ok()) {
    if (error) *error = "nft --check: " + r.Describe();
    return false;
  }
  return true;
}

bool NetFilter::CheckCgroupSocketMatch(const CgroupRef& cgroup, std::string* error) {
  if (!CgroupInstallable(cgroup)) {
    if (error != nullptr) {
      *error = cgroup.valid
                   ? "the cgroup v2 path '" + cgroup.path + "' is not usable"
                   : "this process has no usable cgroup v2 path";
    }
    return false;
  }

  // Keep the probe narrower than the production ruleset: if this fails while
  // the path above exists, the only expression being evaluated is the
  // socket-cgroup matcher. A regular (unhooked) chain avoids touching packet
  // traversal even in check mode, and --check commits nothing.
  std::string script;
  script += "table inet urnetwork_cgroup_probe {\n";
  script += "\tchain probe {\n";
  script += "\t\t" + CgroupMatch(cgroup) + " counter\n";
  script += "\t}\n";
  script += "}\n";
  return CheckRuleset(script, error);
}

bool NetFilter::Apply(const FilterConfig& requested, std::string* error) {
  // Copy: the reaper legitimately calls Apply(filter.appliedConfig(), …), and
  // this function rewrites the config it actually installs.
  FilterConfig cfg = requested;
  if (cfg.state == FilterState::Off) return Remove(error);

  const auto refuse = [&](const char* code, std::string message) {
    lastError_ = std::move(message);
    lastErrorCode_ = code;
    if (error) *error = lastError_;
    std::fprintf(stderr, "[filter] refused %s: %s (%s)\n", ToString(cfg.state),
                 lastError_.c_str(), code);
    return false;
  };

  const NftCgroupMode cgroupMode =
      SelectNftCgroupMode(cfg.socket_mark_proven, cfg.cgroup_socket_match_supported,
                          cfg.floor, !cfg.dns_helper_cgroups.empty());
  if (cgroupMode == NftCgroupMode::Refuse) {
    std::string why;
    if (!cfg.socket_mark_proven) {
      why = "this kernel does not support nftables socket cgroupv2 matching and the "
            "cgroup-BPF socket marker was not proven";
    } else if (cfg.floor) {
      why = "this kernel does not support nftables socket cgroupv2 matching, so the "
            "crash-safe kill-switch floor cannot exempt the daemon after its BPF program "
            "dies";
    } else {
      why = "this kernel does not support nftables socket cgroupv2 matching, so the DNS "
            "helper cgroup cannot be opened for a floored reconnect";
    }
    return refuse(kFilterCodeCgroupUnavailable, std::move(why));
  }

  // A floor we cannot exempt ourselves from is a machine that is blocked AND
  // structurally unable to reconnect: with no cgroup match nothing sets the
  // mark, so neither permit fires for the daemon's own sockets. Refuse with
  // the cause named rather than install it.
  if (cfg.floor && cgroupMode == NftCgroupMode::CgroupAndMark &&
      !CgroupInstallable(cfg.cgroup)) {
    return refuse(kFilterCodeCgroupUnavailable,
                  cfg.cgroup.valid
                      ? "the daemon's cgroup v2 path '" + cfg.cgroup.path +
                            "' is not usable, so its own sockets could not be exempted from the "
                            "block floor"
                      : std::string("this machine is not running the cgroup v2 unified "
                                    "hierarchy, so the daemon's own sockets cannot be exempted "
                                    "from the block floor"));
  }
  // Windows refuses first on an IPv6-only network rather than block the
  // machine off the net; Linux had no such preflight. Arming the v6
  // fail-closed floor there leaves no v4 path for the daemon either, and the
  // only way out is the GUI toggle or `urnetworkd --revert`.
  if (cfg.block_ipv6) {
    std::string detail;
    if (IsIpv6OnlyNetwork(&detail)) {
      return refuse(kFilterCodeIpv4DefaultRouteMissing,
                    "refusing to block IPv6: " + detail +
                        ", so blocking it would cut this machine off the network entirely");
    }
  }
  // A helper cgroup whose path is absent fails the ENTIRE transaction (nft
  // resolves it to a cgroup id at load time), which would take the kill switch
  // and the egress self-exclusion down with it. Drop the absent ones here so
  // appliedConfig() is what is really in force.
  if (cgroupMode == NftCgroupMode::CgroupAndMark &&
      !cfg.dns_helper_cgroups.empty()) {
    std::vector<CgroupRef> usable;
    for (const auto& helper : cfg.dns_helper_cgroups) {
      if (CgroupInstallable(helper)) {
        usable.push_back(helper);
      } else {
        std::fprintf(stderr, "[filter] dropping absent dns helper cgroup '%s'\n",
                     helper.path.c_str());
      }
    }
    cfg.dns_helper_cgroups = std::move(usable);
  }
  // The mark chain is worth installing even without a floor (it is the whole
  // egress self-exclusion), but only when the path resolves.
  if (cgroupMode == NftCgroupMode::CgroupAndMark &&
      !CgroupInstallable(cfg.cgroup) && cfg.cgroup.valid) {
    std::fprintf(stderr,
                 "[filter] the daemon cgroup path '%s' does not resolve; the mark chain will be "
                 "empty and the egress split will not hold\n",
                 cfg.cgroup.path.c_str());
    cfg.cgroup = CgroupRef();
  }

  const std::string script = BuildNftRuleset(cfg);
  if (!ApplyScript(script, ToString(cfg.state), error)) {
    std::fprintf(stderr, "[filter] apply %s failed: %s\n", ToString(cfg.state),
                 lastError_.c_str());
    return false;
  }

  state_ = cfg.state;
  floor_ = cfg.floor;
  applied_ = cfg;
  SetArmedMarker(cfg.floor);
  std::fprintf(stderr,
               "[filter] %s (floor=%d ipv6_blocked=%d dns_pinned=%d helper_dns=%d lan=%d "
               "socket_mark=%d cgroup_match=%d cgroup=%s)\n",
               ToString(cfg.state), cfg.floor ? 1 : 0, RulesetBlocksIpv6(cfg) ? 1 : 0,
               RulesetPinsDns(cfg) ? 1 : 0, RulesetOpensHelperDns(cfg) ? 1 : 0,
               cfg.allow_lan ? 1 : 0, cfg.socket_mark_proven ? 1 : 0,
               cgroupMode == NftCgroupMode::CgroupAndMark ? 1 : 0,
               cfg.cgroup.valid ? cfg.cgroup.path.c_str() : "(none)");
  if (cfg.floor) {
    // Printed at the moment the floor goes in, so the recovery command is in
    // the journal BEFORE anyone needs it.
    std::fprintf(stderr,
                 "[filter] the block floor is in force. If this daemon dies while armed the "
                 "machine stays blocked; recover with: %s\n",
                 RecoveryCommand());
  }
  if (RulesetOpensHelperDns(cfg)) {
    std::fprintf(stderr,
                 "[filter] a bounded off-tunnel DNS permit is OPEN for %d dns helper cgroup(s) "
                 "for at most %ds\n",
                 static_cast<int>(cfg.dns_helper_cgroups.size()), kConnectingWindowSeconds);
  }
  return true;
}

bool NetFilter::Remove(std::string* error) {
  const bool wasOpen = RulesetOpensHelperDns(applied_);
  // Did THIS object install something that is still in force? The distinction
  // is the whole correctness of this function: with nothing claimed, a delete
  // that fails changed nothing and there is nothing to keep claiming; with a
  // ruleset claimed, a delete that fails leaves the user blocked.
  const bool claimed = state_ != FilterState::Off;

  // Record the INTENT to be disarmed before touching the kernel, and record it
  // whatever happens below. The marker only steers a CRASH-restart, and after a
  // teardown request the restart must come back OPEN: if the delete below fails
  // and this daemon then dies, the startup sweep's plain delete is the user's
  // way out, and a surviving marker would re-arm the floor against the wish
  // that got us here.
  SetArmedMarker(false);

  const std::string nft = FindTool("nft");
  if (nft.empty()) {
    if (!claimed) {
      // Nothing we could have installed, so nothing to undo.
      state_ = FilterState::Off;
      floor_ = false;
      applied_ = FilterConfig();
      lastError_.clear();
      lastErrorCode_ = "";
      return true;
    }
    // Apply() found nft, so the table went in; now the only tool that can take
    // it out is gone. Reporting "removed" here would be a failed teardown
    // reported as a completed one, with the floor still blocking the machine.
    lastError_ = std::string("nftables (nft) is no longer available, so `table inet ") +
                 kNftTableName + "` could not be removed and is STILL IN FORCE";
    lastErrorCode_ = kFilterCodeNftMissing;
    if (error) *error = lastError_;
    std::fprintf(stderr, "[filter] TEARDOWN FAILED (floor=%d): %s\n%s\n", floor_ ? 1 : 0,
                 lastError_.c_str(), RecoveryHelpText().c_str());
    return false;
  }

  const CommandResult r = RunNftScript(nft, NftDeleteTableScript());
  if (!r.ok()) {
    // `nft -f` is transactional: the table is exactly as it was, so this object
    // KEEPS claiming it. That is what makes ~NetFilter retry, what keeps
    // floorInstalled() (and StatusReply::kill_switch) truthful while the user
    // is cut off, and what stops the caller treating this as done.
    lastError_ = "nft delete table: " + r.Describe();
    lastErrorCode_ = kFilterCodeNftRejected;
    if (error) *error = lastError_;
    if (claimed) {
      std::fprintf(stderr,
                   "[filter] TEARDOWN FAILED after %d attempts: `table inet %s` is STILL IN "
                   "FORCE (state=%s floor=%d): %s\n%s\n",
                   kNftAttempts, kNftTableName, ToString(state_), floor_ ? 1 : 0,
                   lastError_.c_str(), RecoveryHelpText().c_str());
    } else {
      // Nothing was claimed, so nothing regressed; still never call it success.
      std::fprintf(stderr, "[filter] teardown of an unclaimed table failed: %s\n",
                   lastError_.c_str());
    }
    return false;
  }

  state_ = FilterState::Off;
  floor_ = false;
  applied_ = FilterConfig();
  if (wasOpen) {
    std::fprintf(stderr, "[filter] the bounded off-tunnel DNS permit is CLOSED\n");
  }
  lastError_.clear();
  lastErrorCode_ = "";
  return true;
}

void NetFilter::AdoptArmedFloor() {
  // The ruleset is already in the kernel — installed by the static startup
  // sweep after a crash. Take ownership so floorInstalled() is TRUE, the status
  // reply says the user is blocked (because they are), the reaper's Verify()
  // re-installs it if something flushes it, and the destructor tears it down.
  state_ = FilterState::Armed;
  floor_ = true;

}

bool NetFilter::Verify(std::string* error) const {
  const auto fail = [error](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (state_ == FilterState::Off) return true;  // nothing is claimed
  const std::string nft = FindTool("nft");
  if (nft.empty()) return fail("nftables (nft) is not installed");
  const CommandResult r = RunCommand({nft, "list", "table", "inet", kNftTableName});
  if (!r.ok()) {
    // The overwhelmingly likely cause is `nft flush ruleset`, which Fedora's
    // shipped /etc/sysconfig/nftables.conf begins with — so a
    // `systemctl restart nftables` silently destroyed our table.
    return fail(std::string("the ") + kNftTableName +
                " table is gone (something flushed the ruleset): " + r.Describe());
  }
  const char* const chains[] = {kNftMarkChainName, kNftOutChainName, kNftInChainName,
                                kNftFwdChainName};
  const char* const expected = floor_ ? "drop" : "accept";
  for (const char* chain : chains) {
    const std::string policy = ChainPolicyFromListing(r.output, chain);
    if (policy.empty()) {
      return fail(std::string("the ") + chain + " chain is missing from the " + kNftTableName +
                  " table");
    }
    // The mark chain is always policy accept; the three filter chains carry
    // the floor.
    const char* want = (std::strcmp(chain, kNftMarkChainName) == 0) ? "accept" : expected;
    if (policy != want) {
      return fail(std::string("the ") + chain + " chain policy is '" + policy + "', expected '" +
                  want + "'");
    }
  }
  return true;
}

bool NetFilter::MarkChainCounters(uint64_t* daemonPackets, uint64_t* totalPackets,
                                  std::string* error) const {
  return ReadMarkChainCounters(daemonPackets, totalPackets, error);
}

bool NetFilter::SweepStaleState(bool preserveArmed) {
  // FIRST, BEFORE ANY FIREWALL WORK: put /etc/resolv.conf back. This is not a
  // new revert mechanism — it is THE revert path the daemon already has, and it
  // is called from exactly the three moments a crashed DNS takeover has to be
  // undone: every daemon start, `urnetworkd --revert` (the documented escape
  // hatch a stuck user is told to run), and the unit's ExecStopPost
  // `--revert-unless-armed`. Putting it first matters because a machine that
  // resolves nothing cannot look anything up to work out what to do next, and
  // because unlike the nftables half it is correct on EVERY path: the tun died
  // with the process that owned it, so a tunnel resolver left in
  // /etc/resolv.conf points into a tunnel that no longer exists.
  //
  // It is a no-op — one failed stat() — on any machine that never reached tier
  // 3, which is every systemd-resolved host.
  if (std::string dnsDetail; RestoreDirectResolvConf(&dnsDetail) && !dnsDetail.empty()) {
    std::fprintf(stderr, "[dns] startup sweep: %s\n", dnsDetail.c_str());
  } else if (!dnsDetail.empty()) {
    std::fprintf(stderr, "[dns] startup sweep FAILED to restore DNS: %s\n", dnsDetail.c_str());
  }
  // nftables rules and ip rules are NOT tied to process lifetime (the Windows
  // dynamic WFP session is; that safety is not inherited here), so an unclean
  // exit leaves the machine holding a policy nobody owns. Sweep by our own
  // table NAME / mark / table id only — never by priority alone, which would
  // let us delete a co-installed WireGuard's rules.
  const bool armed = preserveArmed && ArmedMarkerPresent();
  bool armedFloorLeft = false;
  if (const std::string nft = FindTool("nft"); !nft.empty()) {
    // The unconditional way out, kept in a variable because it is also the
    // FALLBACK when the armed replacement below fails.
    const std::string open = NftDeleteTableScript();
    std::string script = open;
    bool keepMarker = false;
    if (armed) {
      FilterConfig cfg;
      cfg.state = FilterState::Armed;
      cfg.floor = true;
      cfg.cgroup = SelfCgroupV2();
      if (CgroupInstallable(cfg.cgroup)) {
        // REPLACE the stale table with a fresh Armed ruleset in ONE atomic
        // `nft -f` — never delete-then-add — so the block is continuous across
        // the crash with no open window.
        script = BuildNftRuleset(cfg);
        keepMarker = true;
        armedFloorLeft = true;
        std::fprintf(stderr,
                     "[filter] startup sweep: this machine was armed when the daemon died; "
                     "replacing the stale table with a fresh armed floor. Recover with: %s\n",
                     RecoveryCommand());
      } else {
        std::fprintf(stderr,
                     "[filter] startup sweep: armed marker present but this daemon's cgroup is "
                     "not usable, so an armed floor would block the daemon itself; opening "
                     "instead\n");
      }
    }
    CommandResult r = RunNftScript(nft, script);
    if (!r.ok() && keepMarker) {
      armedFloorLeft = false;  // the re-arm failed; the fallback OPENS the machine
      // The re-arm did not take, and `nft -f` is transactional: the STALE table
      // from the daemon that died is still in the kernel, blocking a machine
      // that now has NO daemon owning the rules. Fall back to opening it. A
      // leak the user can see and act on beats a block they cannot escape —
      // and unlike the block, the reaper can arm again a moment later.
      std::fprintf(stderr,
                   "[filter] startup sweep: could not install the armed floor (%s); removing the "
                   "stale table instead so this machine is not left blocked with no owner\n",
                   r.Describe().c_str());
      keepMarker = false;
      r = RunNftScript(nft, open);
    }
    if (!r.ok()) {
      // Both the re-arm and the delete failed. Nothing else in this process
      // will retry, so print the whole way out now, while there is still a
      // journal a user can read.
      std::fprintf(stderr,
                   "[filter] startup sweep FAILED after %d attempts; a stale `table inet %s` may "
                   "still be blocking this machine: %s\n%s\n",
                   kNftAttempts, kNftTableName, r.Describe().c_str(), RecoveryHelpText().c_str());
      keepMarker = false;
    }
    if (!keepMarker) SetArmedMarker(false);
  } else if (!armed) {
    SetArmedMarker(false);
  }
  // The policy rules and the capture table go regardless: there is no tun in
  // either outcome, so they can only misroute. The armed floor blocks egress
  // at the filter hook, not by routing, so removing them opens nothing.
  const std::string ip = FindTool("ip");
  if (ip.empty()) return armedFloorLeft;
  const std::string table = std::to_string(kTunnelRouteTable);
  int removed = 0;
  for (; removed < kMaxDuplicatePolicyRules; ++removed) {
    if (!RunCommand({ip, "-4", "rule", "delete", "table", table}).ok()) break;
    std::fprintf(stderr, "[filter] startup sweep: removed a stale policy rule\n");
  }
  if (removed == kMaxDuplicatePolicyRules) {
    std::fprintf(stderr,
                 "[filter] startup sweep: more than %d policy rules point at table %s; remove the "
                 "rest with: sudo ip -4 rule delete table %s\n",
                 kMaxDuplicatePolicyRules, table.c_str(), table.c_str());
  }
  // An older build's pref 32762 suppress rule outranks the capture table for
  // every non-default route in main; an upgrade must not inherit it.
  DeleteRetiredSuppressRules(ip);
  RunCommand({ip, "-4", "route", "flush", "table", table});
  return armedFloorLeft;
}

// ---- the tun ---------------------------------------------------------------

std::unique_ptr<Tunnel> Tunnel::Open(const TunnelConfig& cfg, TunnelError* err) {
  const auto fail = [err](const char* code, std::string message) {
    if (err != nullptr) {
      err->code = code;
      err->message = std::move(message);
    }
    return std::unique_ptr<Tunnel>();
  };

  // --- UPSTREAM'S IPv4-ONLY CONFIG FLOOR, first, before any device exists.
  //     Same predicate, same placement and same [tun] diagnostic as upstream
  //     (upstream app/src/Tunnel.cpp:32); the fork adds the ctl error code so
  //     the refusal reaches the UI as something other than "could not open or
  //     configure the tun device".
  //
  //     IT IS NOT REDUNDANT WITH OUR RUNTIME IPv6 FLOORS, which is why it is
  //     kept rather than folded into the field checks below:
  //       * NetFilter's `meta nfproto ipv6 counter reject` and IsIpv6OnlyNetwork
  //         are RUNTIME floors over packets and over the host's default routes.
  //         They say nothing about the ADDRESSES the device handed us, and they
  //         are armed AFTER the link is up. This runs before ::open.
  //       * The per-field checks below are stricter than upstream on the
  //         address (inet_pton rejects the leading-zero forms IsIpv4Literal
  //         accepts) but were LOOSER in two places upstream catches:
  //           - prefix 0 was accepted here, and a /0 on the tun installs a
  //             connected route covering the whole IPv4 space;
  //           - a non-IPv4 resolver was dropped silently AFTER the device was
  //             created (see the filter loop below), so a device that offered
  //             only IPv6 resolvers brought a tunnel up with no DNS at all.
  //         Upstream refuses the configuration outright for both. That is the
  //         fail-closed direction, so upstream wins here and the checks below
  //         stay as the more specific message for the cases they do catch.
  if (!IsIpv4OnlyTunnelConfig(cfg)) {
    std::fprintf(stderr,
                 "[tun] refusing non-IPv4 tunnel configuration "
                 "(addr=%s/%d, dns-count=%zu)\n",
                 cfg.local_addr_v4.c_str(), cfg.prefix_v4, cfg.dns_servers_v4.size());
    return fail("tun_config_invalid",
                "refusing a non-IPv4 tunnel configuration: address '" + cfg.local_addr_v4 + "/" +
                    std::to_string(cfg.prefix_v4) +
                    "' and every tunnel resolver must be an IPv4 literal");
  }

  // --- validate every value that came back from the device BEFORE it reaches
  //     any command line. The daemon must not rely on the GUI's client-side
  //     inet_pton (Formatters.cpp): the DNS sheet is user-editable and the
  //     values arrive here over the device RPC.
  if (!ValidInterfaceName(cfg.name)) {
    return fail("tun_config_invalid", "invalid tun interface name '" + cfg.name + "'");
  }
  if (!IsIpv4Address(cfg.local_addr_v4)) {
    return fail("tun_config_invalid",
                "the device reported an invalid tunnel address '" + cfg.local_addr_v4 + "'");
  }
  // >= 1, not >= 0: agrees with IsIpv4OnlyTunnelConfig so the two guards can
  // never disagree about what a legal prefix is if their order ever changes.
  if (cfg.prefix_v4 < 1 || cfg.prefix_v4 > 32) {
    return fail("tun_config_invalid",
                "invalid tunnel prefix length " + std::to_string(cfg.prefix_v4));
  }
  if (cfg.mtu < 576 || cfg.mtu > 65535) {
    return fail("tun_config_invalid", "invalid tunnel mtu " + std::to_string(cfg.mtu));
  }

  if (FindTool("ip").empty()) {
    return fail("missing_tool",
                "the iproute2 `ip` command is not installed: the tunnel cannot be configured");
  }

  int fd = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT) {
    // One modprobe attempt, then re-open: a missing node is almost always an
    // unloaded module, and the user cannot be expected to know that.
    if (const std::string modprobe = FindTool("modprobe"); !modprobe.empty()) {
      RunCommand({modprobe, "tun"});
      fd = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    }
  }
  if (fd < 0) {
    const int e = errno;
    // Every one of these used to surface as the single string "could not
    // open/configure the tun device" — the same message a route or a DNS
    // failure produced.
    switch (e) {
      case ENOENT:
        return fail("tun_module_missing",
                    "the tun kernel module is not loaded and could not be loaded "
                    "(/dev/net/tun is missing)");
      case EACCES:
      case EPERM:
        return fail("tun_permission_denied",
                    "opening /dev/net/tun: " + TunPermissionDeniedDetail("open"));
      case ENODEV:
        return fail("tun_module_missing",
                    "/dev/net/tun exists but the tun driver is not available in this kernel "
                    "or container");
      default:
        return fail("tun_open_failed",
                    std::string("could not open /dev/net/tun: ") + std::strerror(e));
    }
  }

  ifreq ifr{};
  // IFF_TUN: layer-3 IP packets; IFF_NO_PI: no 4-byte protocol-info prefix, so
  // the fd carries raw IP packets exactly as the SDK IoLoop expects.
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  std::strncpy(ifr.ifr_name, cfg.name.c_str(), IFNAMSIZ - 1);
  if (::ioctl(fd, TUNSETIFF, &ifr) < 0) {
    const int e = errno;
    ::close(fd);
    if (e == EPERM || e == EACCES) {
      return fail("tun_permission_denied", TunPermissionDeniedDetail("TUNSETIFF"));
    }
    if (e == EBUSY) {
      return fail("tun_busy",
                  "the interface '" + cfg.name + "' is already in use by another process");
    }
    return fail("tun_open_failed", std::string("TUNSETIFF: ") + std::strerror(e));
  }

  auto t = std::unique_ptr<Tunnel>(new Tunnel());
  t->fd_ = fd;
  t->name_ = ifr.ifr_name;  // the actual assigned name
  t->report_.interface = t->name_;
  // Second floor, and still doing real work after the guard above: inet_pton is
  // stricter than IsIpv4Literal (it rejects "010.0.0.1", which the constexpr
  // literal check accepts), and dnsServers_ is what the pinned-DNS nft permit
  // and the resolved override are both filled from.
  for (const auto& dns : cfg.dns_servers_v4) {
    if (IsIpv4Address(dns)) {
      t->dnsServers_.push_back(dns);
    } else {
      std::fprintf(stderr, "[tun] ignoring an invalid tunnel resolver '%s'\n", dns.c_str());
    }
  }
  if (!t->Configure(cfg, err)) {
    return nullptr;  // dtor closes the fd + tears the rules down
  }
  std::string dnsList;
  for (const auto& dns : t->dnsServers_) {
    if (!dnsList.empty()) dnsList += ',';
    dnsList += dns;
  }
  std::fprintf(stderr,
               "[tun] up %s addr=%s/%d mtu=%d dns=%s (routes=%d egress_protected=%d "
               "dns_applied=%d)\n",
               t->name_.c_str(), cfg.local_addr_v4.c_str(), cfg.prefix_v4, cfg.mtu, dnsList.c_str(),
               t->report_.routes_installed ? 1 : 0, t->report_.egress_protected ? 1 : 0,
               t->report_.dns_applied ? 1 : 0);
  return t;
}

bool Tunnel::Configure(const TunnelConfig& cfg, TunnelError* err) {
  const std::string ip = FindTool("ip");
  const std::string addr = cfg.local_addr_v4 + "/" + std::to_string(cfg.prefix_v4);
  // Leg A of the witness compares the source address the kernel picks for our
  // own socket against this, so it has to survive Configure().
  localAddr_ = cfg.local_addr_v4;
  // Order matters (wg-quick order: mtu/addr first, up second): NetworkManager
  // only protects an externally-created tun WHILE THE LINK IS DOWN
  // (APPIMAGE.md §10c), so do every link-down configuration before `up`. The
  // authoritative guard is the installed [keyfile] unmanaged-devices marking
  // + udev rule; this ordering just avoids ever showing NM a bare up link.
  const std::vector<std::vector<std::string>> steps = {
      {ip, "link", "set", "dev", name_, "mtu", std::to_string(cfg.mtu)},
      // `replace`, not `add`: a stale address from an unclean previous run
      // answers EEXIST and would abort the whole bring-up.
      {ip, "-4", "address", "replace", addr, "dev", name_},
      {ip, "link", "set", "dev", name_, "up"},
  };
  // FROM UPSTREAM (556dca7), but BEST-EFFORT rather than one of the fatal
  // `steps` above, which is the whole reason it is not in that vector: the
  // loop below aborts the entire bring-up on any non-zero exit, and
  // `addrgenmode` is an iproute2/kernel capability we do not require anywhere
  // else. Making it fatal would trade a working tunnel for a hardening measure
  // on exactly the older kernels least able to spare one.
  //
  // What it buys: set while the link is still DOWN (the `up` step is last), it
  // stops the kernel synthesizing an IPv6 link-local address on urnet0 at all,
  // so there is no tunnel-local v6 source address for anything to bind or
  // advertise. This is defence in depth UNDER the nftables v6 block
  // (FilterConfig::block_ipv6, §6.3), never a replacement for it — the filter
  // is still what refuses v6 egress; this removes the address that block
  // exists to contain. Host IPv6 is untouched: no route and no blackhole is
  // installed, so the physical interface keeps its normal policy.
  if (const CommandResult agm =
          RunCommand({ip, "link", "set", "dev", name_, "addrgenmode", "none"});
      !agm.ok()) {
    std::fprintf(stderr,
                 "[tun] addrgenmode none unavailable (%s); the nftables IPv6 block remains "
                 "the enforcing layer\n",
                 agm.Describe().c_str());
  }
  for (const auto& step : steps) {
    const CommandResult r = RunCommand(step);
    if (!r.ok()) {
      report_.route_detail = JoinArgv(step) + ": " + r.Describe();
      if (err != nullptr) {
        err->code = "tun_config_failed";
        err->message = "could not configure " + name_ + ": " + r.Describe();
      }
      return false;
    }
  }

  if (!InstallPolicyRules(err)) return false;
  if (!InstallRoutes(err)) return false;
  report_.routes_installed = true;

  // The self-check is the whole point of the dedicated table: prove, before
  // anybody calls this tunnel up, that (a) ordinary traffic now leaves through
  // the tun and (b) the daemon's own traffic demonstrably does not.
  //
  // ONE CALL, AND ON PURPOSE. (a) used to live in a separate VerifyCaptureTook
  // that ran ONLY here, at bring-up, and asked `ip route get` — so the live
  // re-check thirty seconds later re-proved (b) against a capture policy it
  // never re-checked was still there, and could report green with the whole
  // ruleset gone. Both halves now live inside VerifyEgressWitness as legs 1 and
  // 0, which means the bring-up gate, the post-connect gate and the reaper's
  // re-check are the same four measurements in the same order, every time. A
  // check that only one caller runs is a check the other callers are quietly
  // assuming.
  //
  // IT IS FATAL, AND IT RUNS HERE, before TunnelHost hands the tun fd to
  // newIoLoop: nothing is reading the tun at this moment, and the witness sends
  // nothing at all, so no failure mode of this check can put a packet anywhere.
  // Failing it returns false, ~Tunnel drops the policy rule and closes the fd,
  // and the capture routes go with the interface — the machine is back to where
  // it started before any user traffic could touch it.
  if (cfg.require_egress_protection) {
    if (!VerifyEgressWitness("bring-up", err)) return false;
  } else {
    std::fprintf(stderr,
                 "[tun] URNETWORK_ALLOW_UNPROTECTED_EGRESS is set: the daemon's own sockets "
                 "are NOT excluded from this tunnel. The SDK control plane will starve. "
                 "Development only.\n");
    // SKIPPED, not run-and-ignored: a check whose result is discarded is a
    // check that will one day be read as a pass.
    report_.egress_protected = false;
    report_.egress_mechanism = "none (URNETWORK_ALLOW_UNPROTECTED_EGRESS)";
    report_.egress_detail = "the egress witness was skipped by the development override";
  }

  // ============================================================================
  // DNS IS FAIL-CLOSED, AND THAT IS A DELIBERATE CHANGE FROM WHAT SHIPPED.
  //
  // What shipped: ApplyDns() was called for its side effect and its answer was
  // thrown away here ("never fatal"). TunnelHost then refused the bring-up ONLY
  // when the kill switch was on. With the kill switch OFF — the default, and
  // what a first-time tester uses — the tunnel came up, carried traffic, and
  // every name went to the ISP's resolver. On Arch that was not an edge case,
  // it was EVERY machine.
  //
  // THE CASE FOR PROCEEDING (rejected): a tunnel that carries traffic is worth
  // something even with DNS leaking; a user who is chasing a geo-block or a
  // throttling ISP gets what they came for; browsers increasingly do DoH on
  // their own, so the leak is partial; and refusing turns a degraded connection
  // into no connection at all, which some users would call the worse bug.
  //
  // THE CASE FOR REFUSING (taken):
  //   1. It is the rule this codebase already applies to the same class of
  //      defect. IPv6 has no tunnel, so IPv6 is BLOCKED rather than leaked, in
  //      every state, NOT gated on the kill switch — docs/linux_agent_help.md
  //      §6.3 is explicit that leak prevention is not a preference. Names are a
  //      leak in exactly the way v6 is: the UI says Connected while an observer
  //      on the ISP's resolver sees every site the user visits, in order, with
  //      timestamps. Deriving the answer for DNS from the kill switch made one
  //      leak a preference and the other a rule, for no principled reason.
  //   2. The leak is invisible to the user. The one field that reports it,
  //      dns_detail, is rendered NOWHERE in the app today (checked: every use
  //      in the daemon and the app is an assignment or a clear). "Proceed but
  //      report it" was, in practice, "proceed".
  //   3. The refusal is now RARE, which is what makes it affordable. Before the
  //      three tiers below existed, refusing would have meant refusing on every
  //      Arch machine. Now the direct /etc/resolv.conf tier works on any host
  //      with a writable /etc, so the remaining population is: a read-only or
  //      immutable /etc, an /etc/resolv.conf bind-mounted read-only inside a
  //      container, an nss-resolve host whose resolved is broken, or a device
  //      that reported no resolvers at all. Each of those is a machine whose
  //      owner needs to be told something, not a machine that should quietly
  //      leak.
  //   4. It fails LOUD and ACTIONABLE instead of silent. dns_detail names every
  //      tier that was tried and why it did not take, and this message reaches
  //      MainWindow::PollDaemonHealth, which renders status.error verbatim.
  //
  // THE ESCAPE HATCH IS THE ONE THIS FILE ALREADY USES FOR THE SAME SHAPE OF
  // TRADE. URNETWORK_ALLOW_UNPROTECTED_EGRESS skips the egress witness; this is
  // spelled the same way, logged as loudly, and — like it — SKIPS the refusal
  // rather than faking a pass: dns_applied stays false, dns_detail still says
  // exactly what is leaking, and the nftables DNS floor stays off (it is gated
  // on dns_applied), because closing :53 with nothing able to answer would be an
  // outage rather than a protection.
  // ============================================================================
  if (!ApplyDns()) {
    const bool allowUnprotected = ::getenv(kAllowUnprotectedDnsEnv) != nullptr;
    if (!allowUnprotected) {
      const std::string message =
          "this tunnel would carry your traffic while every name you look up still went to "
          "your current resolver, so it was not started. " +
          report_.dns_detail + " (set " + kAllowUnprotectedDnsEnv +
          "=1 in the daemon's environment to connect anyway, with DNS leaking.)";
      if (err != nullptr) {
        err->code = kCodeDnsApplyFailed;
        err->message = message;
      }
      std::fprintf(stderr, "[dns] REFUSING the bring-up: %s\n", message.c_str());
      return false;
    }
    std::fprintf(stderr,
                 "[dns] %s is set: connecting with DNS NOT on the tunnel. Every name this "
                 "machine looks up goes to the resolver it used before the tunnel. %s\n",
                 kAllowUnprotectedDnsEnv, report_.dns_detail.c_str());
  }
  return true;
}

bool Tunnel::InstallPolicyRules(TunnelError* err) {
  const std::string ip = FindTool("ip");
  const std::string table = std::to_string(kTunnelRouteTable);

  // OWNERSHIP BEFORE THE FIRST MUTATION. RemovePolicyRules() early-returns on
  // !rulesInstalled_, so setting this only after the LAST add succeeded leaked
  // every rule that had already landed when a later one failed — a policy rule
  // is not tied to the link, so it outlived the tunnel, the object and the
  // process. From here the destructor cleans up whatever exists.
  rulesInstalled_ = true;

  // Idempotence: an unclean previous run may have left these behind, and
  // `ip rule add` happily installs a DUPLICATE rather than answering EEXIST.
  for (int i = 0; i < kMaxDuplicatePolicyRules; ++i) {
    if (!RunCommand({ip, "-4", "rule", "delete", "table", table}).ok()) break;
  }
  // And an older build left a pref 32762 `table main suppress_prefixlength 0`
  // rule ABOVE ours. Leaving it would hand every non-default route in main —
  // another VPN's 0.0.0.0/1 + 128.0.0.0/1 pair, a pushed /24 — precedence over
  // the capture table, i.e. exactly the bypass this build stopped installing.
  DeleteRetiredSuppressRules(ip);

  // ONE rule. wg-quick pairs its fwmark rule with `table main
  // suppress_prefixlength 0` so that specific routes in main still win, and
  // that pairing is what this build dropped: `suppress_prefixlength 0`
  // suppresses ONLY the default route, so every OTHER route in main outranked
  // the capture table and traffic left outside the tunnel with nothing but the
  // (optional) nftables floor to catch it. Nothing needs main consulted first —
  //   * the daemon's own transport is steered around the tunnel by kEgressMark,
  //     which is what wg-quick needs the suppress rule for (its endpoint route);
  //   * LAN traffic is excluded from the capture set by construction, and a
  //     destination the capture table does not claim falls through to main at
  //     32766 on its own, because `ip rule` continues to the next rule when the
  //     named table has no matching route;
  //   * everything else is precisely what a full tunnel is supposed to carry.
  // The one behaviour this gives up is reaching a NON-RFC1918 on-link neighbour
  // directly; it now goes through the tunnel like every other public address,
  // which is already what Android, iOS and Windows do with this same 31-prefix
  // capture set.
  //   32763  everything NOT carrying our mark -> the tunnel table
  //   32766  main (kernel)                    -> where our own marked sockets land,
  //                                              and where non-captured (LAN)
  //                                              destinations fall through to
  const std::vector<std::string> rule = {ip,    "-4",  "rule", "add", "not",
                                         "fwmark", MarkHex(kEgressMark), "table", table,
                                         "pref", std::to_string(kFwmarkRulePriority)};
  const CommandResult r = RunCommand(rule);
  if (!r.ok()) {
    report_.route_detail = JoinArgv(rule) + ": " + r.Describe();
    if (err != nullptr) {
      err->code = "route_install_failed";
      err->message = "could not install the tunnel policy rule: " + r.Describe();
    }
    return false;
  }
  return true;
}

void Tunnel::RemovePolicyRules() {
  if (!rulesInstalled_) return;
  rulesInstalled_ = false;
  const std::string ip = FindTool("ip");
  const std::string table = std::to_string(kTunnelRouteTable);
  if (ip.empty()) {
    // Policy rules are not tied to the link, so unlike the routes they do NOT
    // disappear with the tun: say so rather than return silently.
    std::fprintf(stderr,
                 "[tun] WARNING: iproute2 `ip` is gone, so the policy rule for table %s could "
                 "not be removed. Remove it with: sudo ip -4 rule delete table %s\n",
                 table.c_str(), table.c_str());
    return;
  }
  int removed = 0;
  for (; removed < kMaxDuplicatePolicyRules; ++removed) {
    if (!RunCommand({ip, "-4", "rule", "delete", "table", table}).ok()) break;
  }
  if (removed == kMaxDuplicatePolicyRules) {
    std::fprintf(stderr,
                 "[tun] WARNING: more than %d policy rules still point at table %s; remove the "
                 "rest with: sudo ip -4 rule delete table %s\n",
                 kMaxDuplicatePolicyRules, table.c_str(), table.c_str());
  }
  // Belt for an upgrade that started while an older build's rule was in place.
  DeleteRetiredSuppressRules(ip);
  // The kernel drops routes with their link, in every table; flushing is the
  // belt for the case where the link outlives us by a moment.
  RunCommand({ip, "-4", "route", "flush", "table", table});
}

bool Tunnel::InstallRoutes(TunnelError* err) {
  const std::string ip = FindTool("ip");
  const std::string table = std::to_string(kTunnelRouteTable);
  std::string firstFailure;
  int failures = 0;
  for (const auto& prefix : CaptureV4Prefixes()) {
    // `replace`, not `add`: another VPN or a stale route answering EEXIST used
    // to abort the entire bring-up with an opaque message.
    const std::vector<std::string> step = {ip,   "-4",     "route", "replace", prefix,
                                           "dev", name_,   "table", table};
    const CommandResult r = RunCommand(step);
    if (!r.ok()) {
      ++failures;
      if (firstFailure.empty()) firstFailure = "route " + prefix + " failed: " + r.Describe();
      std::fprintf(stderr, "[tun] %s\n", firstFailure.c_str());
    }
  }
  if (failures > 0) {
    report_.route_detail = firstFailure + " (" + std::to_string(failures) + " of " +
                           std::to_string(CaptureV4Prefixes().size()) + " capture prefixes)";
    if (err != nullptr) {
      err->code = "route_install_failed";
      err->message = report_.route_detail;
    }
    // A partial capture set is a silent split tunnel: some destinations would
    // leave in the clear while the UI says Connected. Fail the bring-up.
    return false;
  }
  return true;
}

namespace {

// ---- the witness's two instruments -----------------------------------------
//
// BOTH OF THEM FAIL WHEN THEY CANNOT RUN. That inversion is the entire lesson
// of 2026-08-15: the gate that shipped rested on two `ip route get` answers
// that are true whenever an `ip rule` exists, and on a counter with no failing
// branch, so "I could not measure this" and "I measured this and it is fine"
// were the same green.

// What one `nft list table` snapshot says about the mark counters — AND about
// the rules that feed them, because a number read out of an instrument nobody
// checked the aim of is not a measurement.
struct MarkCounters {
  uint64_t tun = 0;  // marked daemon packets that left through OUR OWN tun
  uint64_t phy = 0;  // marked daemon packets that left via a real interface
};

// The instrument must (a) be installed, (b) still be watching THIS interface,
// (c) still be matching THIS mark, and only then are its numbers worth reading.
// (b) is not paranoia: the ruleset is built with the PLANNED tun name before
// the tun exists, so a chain aimed at a name the kernel did not give us would
// count nothing at all — and "counted nothing" is exactly the shape of answer
// that got read as reassurance last time.
bool ReadMarkCounters(const std::string& tunName, uint32_t mark, MarkCounters* out,
                      std::string* error) {
  const auto fail = [error](std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
  };
  std::string listing;
  if (!ReadTableListing(&listing, error)) return false;

  const std::string body = ChainBodyFromListing(listing, kNftProbeChainName);
  if (body.empty()) {
    return fail(std::string("the ") + kNftProbeChainName +
                " chain is not in the kernel, so nothing is watching where this daemon's own "
                "packets go");
  }
  // MATCHED LOOSELY ON PURPOSE, IN BOTH DIRECTIONS. These two guards ask "is
  // the instrument still aimed at us", and the cost of getting the ANSWER wrong
  // is asymmetric: a false negative refuses a tunnel the user wanted, a false
  // positive is the class of bug this whole change exists to delete. So the
  // guards are strict about the thing that matters (the name and the mark must
  // BE there) and deliberately indifferent to how `nft` chose to render them —
  // quoting, an anonymous set, zero padding. Neither substring can appear in a
  // chain aimed somewhere else, and neither can go missing in a chain aimed
  // here, whatever nft's output style does between releases.
  if (body.find(tunName) == std::string::npos) {
    return fail(std::string("the ") + kNftProbeChainName + " chain is not watching " + tunName +
                ", so its counters describe some other interface");
  }
  // nft renders `meta mark` in hex; the decimal form is carried too so a
  // rendering change cannot turn a working host into a refused one.
  if (body.find(MarkHex(mark)) == std::string::npos &&
      body.find(std::to_string(static_cast<unsigned long long>(mark))) == std::string::npos) {
    return fail(std::string("the ") + kNftProbeChainName + " chain does not match mark " +
                MarkHex(mark) + ", so it is not counting this daemon's packets");
  }
  MarkCounters c;
  if (!ParseNamedCounter(listing, kNftProbeTunCounter, &c.tun, nullptr) ||
      !ParseNamedCounter(listing, kNftProbePhyCounter, &c.phy, nullptr)) {
    return fail(std::string("the ") + kNftProbeChainName +
                " counters are not installed, so nothing can weigh the daemon's own packets");
  }
  *out = c;
  return true;
}

// ---- the differential socket probe ------------------------------------------
//
// ONE committed routing decision, taken by the kernel, for a real socket in
// this daemon's own cgroup. connect() on a datagram socket SENDS NOTHING: it
// runs the FIB lookup, commits the result to the socket and binds the source
// address — which is the precise moment R4 is won or lost, and the precise
// question `ip route get` cannot ask, because `ip route get` probes a
// hypothetical mark on no socket at all.
struct SocketBinding {
  std::string source;   // getsockname() after connect()
  uint32_t mark = 0;    // SO_MARK as the socket was born with it
  bool mark_known = false;
};

// clearMark=false: exactly the socket the SDK's dialer gets — created by this
//                  process, in this cgroup, marked (or not) by whatever layer
//                  is actually in force. THE TREATMENT.
// clearMark=true:  the same socket with the exemption taken away. THE CONTROL.
//                  Its binding is what the capture policy does to ordinary
//                  traffic, measured rather than assumed.
bool BindWitnessSocket(bool clearMark, SocketBinding* out, std::string* error) {
  const auto fail = [error](std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
  };
  const int sock = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
  if (sock < 0) return fail(std::string("socket(): ") + std::strerror(errno));
  struct SockGuard {
    int fd;
    ~SockGuard() { if (fd >= 0) ::close(fd); }
  } guard{sock};

  if (clearMark) {
    const int zero = 0;
    if (::setsockopt(sock, SOL_SOCKET, SO_MARK, &zero, sizeof(zero)) != 0) {
      // SO_MARK needs CAP_NET_ADMIN or CAP_NET_RAW even to clear it. A daemon
      // that cannot clear it also cannot have installed the routes, the policy
      // rule or the table, so this is a diagnosis and not a puzzle.
      return fail(std::string("setsockopt(SO_MARK, 0) — needs CAP_NET_ADMIN or CAP_NET_RAW: ") +
                  std::strerror(errno));
    }
  }

  int markValue = 0;
  socklen_t marklen = sizeof(markValue);
  out->mark_known = ::getsockopt(sock, SOL_SOCKET, SO_MARK, &markValue, &marklen) == 0;
  out->mark = out->mark_known ? static_cast<uint32_t>(markValue) : 0u;
  // PROVE THE CONTROL IS A CONTROL. If the clear silently did not take, the two
  // legs are the same socket twice and the differential means nothing.
  if (clearMark && (!out->mark_known || out->mark != 0u)) {
    return fail("the control socket kept a mark after it was cleared, so it is not a control");
  }

  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(static_cast<uint16_t>(kEgressProbePort));
  if (::inet_pton(AF_INET, kEgressProbeAddress, &dst.sin_addr) != 1) {
    return fail("the witness address is unparseable");
  }
  if (::connect(sock, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) != 0) {
    return fail(std::string("connect(): ") + std::strerror(errno));
  }

  sockaddr_in local{};
  socklen_t locallen = sizeof(local);
  if (::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &locallen) != 0) {
    return fail(std::string("getsockname(): ") + std::strerror(errno));
  }
  char srcbuf[INET_ADDRSTRLEN] = {0};
  if (::inet_ntop(AF_INET, &local.sin_addr, srcbuf, sizeof(srcbuf)) == nullptr) {
    return fail(std::string("inet_ntop(): ") + std::strerror(errno));
  }
  out->source = srcbuf;
  return true;
}

}  // namespace

bool Tunnel::VerifyEgressWitness(const char* when, TunnelError* err) {
  const std::string tag = std::string("[tun] egress witness (") + (when == nullptr ? "" : when) +
                          ")";
  // `code` is the machine-readable half, and the two values are NOT
  // interchangeable: route_install_failed says the tunnel is not capturing
  // anything (leg 1), egress_unprotected says it is capturing US (legs 0, 2, 3).
  const auto fail = [&](const char* code, std::string message) {
    report_.egress_protected = false;
    report_.egress_detail = message;
    if (err != nullptr) {
      err->code = code;
      // POINTS AT SOMETHING THAT IS ACTUALLY ON THE MACHINE. This used to send
      // the reader to `packaging/diagnose-egress.sh`, a relative path inside a
      // source tree that nothing installs — an instruction that cannot be
      // followed by the only person who ever sees it. `urnetworkd` is installed
      // by definition, since it is what printed this.
      err->message =
          "this daemon cannot prove its own traffic stays out of its own tunnel, so the "
          "tunnel is not being run: " +
          message +
          ". `urnetworkd --diagnose` needs no root and reports the host preflight; "
          "packaging/diagnose-egress.sh in the source tree goes deeper. The last time this "
          "went unproven the daemon looped 3.38 Tb of its own traffic through its own tunnel "
          "before a human noticed.";
    }
    std::fprintf(stderr, "%s FAILED: %s\n", tag.c_str(), message.c_str());
    return false;
  };

  // The tun's own address is the yardstick BOTH socket legs are measured
  // against. Without it there is no measurement, and a witness that cannot
  // measure must not return true.
  if (localAddr_.empty()) {
    return fail(kCodeEgressUnprotected,
                "the tunnel's own address was never recorded, so neither leg of the witness "
                "can be interpreted");
  }

  // ---- LEG 0: IS THE INSTRUMENT THERE, AND IS IT AIMED AT US? --------------
  // This leg is the answer to the third blocking defect: before it existed the
  // live re-check could return green while the ENTIRE capture policy had been
  // removed from the kernel (`nft flush ruleset`, which is the first line of
  // Fedora's shipped /etc/sysconfig/nftables.conf, destroys our table silently).
  // Reading the counters through ReadMarkCounters means the table, the chain,
  // the interface it names and the mark it matches are all re-proven on every
  // single call — bring-up, post-connect and 30-second re-check alike, because
  // all three now run THIS function and nothing else.
  MarkCounters counters;
  std::string instrumentError;
  if (!ReadMarkCounters(name_, kEgressMark, &counters, &instrumentError)) {
    return fail(kCodeEgressUnprotected, instrumentError);
  }

  // ---- LEG 1: IS THE CAPTURE POLICY ACTUALLY IN FORCE? ---------------------
  // THE CONTROL. A socket identical to the daemon's own in every way except
  // that its mark has been cleared. connect() commits the kernel's real routing
  // decision for ordinary traffic, and the source address it binds says which
  // device won: only the tun holds the tun's address.
  //
  // WITHOUT THIS LEG "EGRESS IS PROTECTED" IS VACUOUS. Every other leg is
  // trivially green on a machine with no capture routes at all — of course the
  // daemon's packets are not in the tunnel if nothing is. That is precisely the
  // shape of the check that shipped the storm, and it is why this leg is first
  // among the measurements rather than a separate bring-up-only function.
  SocketBinding control;
  std::string bindError;
  if (!BindWitnessSocket(/*clearMark=*/true, &control, &bindError)) {
    return fail(kCodeRouteInstallFailed,
                "the control socket could not be established or routed, so it is not possible "
                "to say whether this tunnel captures anything at all: " +
                    bindError);
  }
  if (control.source != localAddr_) {
    return fail(kCodeRouteInstallFailed,
                "the capture policy is NOT in force: an unmarked socket this daemon just "
                "opened was bound to " +
                    control.source + ", not to the tunnel's own address " + localAddr_ +
                    ", so ordinary traffic is leaving around this tunnel and calling the "
                    "daemon's egress 'protected' would mean nothing");
  }

  // ---- LEG 2: DO THE DAEMON'S OWN SOCKETS ESCAPE IT? -----------------------
  // THE TREATMENT, taken with the SAME code, the SAME destination, the SAME
  // cgroup and the SAME syscall sequence one line later — differing only in the
  // mark. Legs 1 and 2 must come out OPPOSITE. Two greens that agree are a
  // broken instrument, not a passing tunnel.
  SocketBinding daemon;
  if (!BindWitnessSocket(/*clearMark=*/false, &daemon, &bindError)) {
    return fail(kCodeEgressUnprotected,
                "a socket created exactly as the SDK's are could not be established or routed, "
                "so this daemon's own egress path cannot be characterised at all: " +
                    bindError);
  }
  if (daemon.source == localAddr_) {
    // THE ROOT CAUSE OF 2026-08-15, CAUGHT AT THE LAYER IT HAPPENS ON. No
    // output-hook mark can repair this: ip_route_me_harder() re-runs the route
    // lookup but sets FLOWI_FLAG_ANYSRC for an RTN_LOCAL source and REUSES the
    // address connect() already bound. Whether the packet then goes into the
    // tun (the amplification loop) or leaves the NIC sourced from the tunnel's
    // address (a socket that can never be answered), the daemon is broken.
    return fail(kCodeEgressUnprotected,
                "a socket this daemon just opened was bound to the tunnel's own address (" +
                    daemon.source +
                    "), so it resolved the capture table before anything could steer it away. "
                    "The mark is not reaching this process's sockets at creation time, and no "
                    "mark applied after connect() can undo it");
  }
  if (!daemon.mark_known || daemon.mark != kEgressMark) {
    // CONTRADICTORY EVIDENCE IS A FAILURE, NOT A PASS, AND THIS IS WHERE THE
    // OLD "belt only" GREEN LIVED. The previous cut passed here with a warning
    // whenever the socket carried no mark but had escaped the capture table
    // anyway — a combination that has no honest explanation. If the mark is
    // absent, the `ip rule` cannot have steered this socket, so something else
    // did, and the next socket the SDK dials has no reason to be as lucky. The
    // nftables belt cannot fill this gap: it acts at NF_INET_LOCAL_OUT, after
    // the source address is bound.
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "a socket this daemon just opened escaped the capture table WITHOUT carrying "
                  "mark 0x%08x (it reads 0x%08x%s). Nothing this daemon installed explains "
                  "that, so the next connection the SDK dials cannot be assumed to escape too",
                  kEgressMark, daemon.mark, daemon.mark_known ? "" : ", unreadable");
    return fail(kCodeEgressUnprotected, buf);
  }

  // ---- LEG 3: HAS ANY REAL DAEMON PACKET ENTERED OUR OWN TUN? --------------
  // The counters the kernel has been keeping since this ruleset was installed,
  // over the SDK's actual traffic — not over a synthetic sample, and not over a
  // window whose length nobody recorded. urnw_probe_tun counts marked packets
  // that left through our own tun, which is the storm signature exactly, and
  // ONE is the whole defect.
  //
  // It is a cumulative counter created at zero by the atomic table swap, so
  // there is no delta to compute, nothing to straddle, and no unsigned
  // subtraction that could wrap past a test and report a pass. It can only be
  // read one way.
  //
  // WHICH ALSO MEANS IT SAYS NOTHING IN THE INSTANT AFTER AN APPLY — at
  // bring-up, and again at the post-connect gate, this counter is zero by
  // construction and the honest verdict is carried entirely by legs 0-2. That
  // is the point of having four: this leg is the one that keeps watching for
  // the next thirty seconds and the next thirty after that, over traffic
  // nobody had to manufacture, and the others are the ones that can answer
  // right now.
  //
  // WHY LEG 2 IS NOT REDUNDANT WITH IT: a packet from a socket that is neither
  // marked at creation nor matched by the cgroup rule at the output hook enters
  // the tun UNCOUNTED. Leg 2 is what makes that state unreachable — the
  // sock_create program marks every socket in this process tree or none, so a
  // fresh socket reading back the mark is the proof that the instrument's
  // match condition holds for the SDK's sockets too.
  if (counters.tun != 0) {
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "%llu packet(s) this daemon sent have left through this daemon's own tunnel "
                  "(%s). That is the amplification loop: every one of them is read back out of "
                  "the tun by the SDK's io loop, re-sent, and captured again",
                  static_cast<unsigned long long>(counters.tun), name_.c_str());
    return fail(kCodeEgressUnprotected, buf);
  }

  // ---- PASS. Four legs, none of them a routing hypothetical, none of them
  // capable of being green while the capture policy is absent. Say what was
  // measured, so the answer is on a surface a human reads instead of inferred
  // from silence.
  report_.egress_mechanism =
      "socket mark set at creation (cgroup-bpf sock_create), proven differentially against an "
      "unmarked control socket";
  char detail[400];
  std::snprintf(detail, sizeof(detail),
                "control socket (mark 0) bound %s = the tunnel, so capture is in force; daemon "
                "socket bound %s with mark 0x%08x, so it is steered around it; 0 marked daemon "
                "packets have entered %s and %llu have left via a physical interface",
                control.source.c_str(), daemon.source.c_str(), daemon.mark, name_.c_str(),
                static_cast<unsigned long long>(counters.phy));
  report_.egress_detail = detail;
  report_.egress_protected = true;
  std::fprintf(stderr, "%s passed: %s\n", tag.c_str(), detail);
  return true;
}

// ---- DNS: the three tiers --------------------------------------------------

const char* ToString(DnsBackend b) {
  switch (b) {
    case DnsBackend::None: return "none";
    case DnsBackend::SystemdResolved: return "systemd-resolved";
    case DnsBackend::Resolvconf: return "resolvconf";
    case DnsBackend::DirectFile: return "/etc/resolv.conf";
  }
  return "none";
}

const char* DirectResolvConfPath() { return kResolvConfPath; }
const char* DirectResolvConfBackupPath() { return kResolvConfBackupPath; }
const char* DirectResolvConfStatePath() { return kResolvConfStatePath; }

DnsHostProbe ProbeDnsHost() {
  DnsHostProbe p;

  // RUNNING, not installed. See kResolvedRuntimeDir: this is the distinction
  // the shipped build never made, and it is the whole Arch defect — the
  // `systemd` package ships /usr/bin/resolvectl on every distro whether or not
  // systemd-resolved is enabled, so FindTool("resolvectl") answers "yes" on a
  // machine where resolved has never run.
  //
  // A stat() and not `systemctl is-active`, and not `resolvectl status`: both
  // of those fork, and `resolvectl` in particular can D-BUS-ACTIVATE resolved
  // on a host where the admin deliberately left it disabled. Starting a system
  // service as a side effect of asking whether it is running is not a probe.
  struct stat st {};
  p.resolved_running = ::stat(kResolvedRuntimeDir, &st) == 0 && S_ISDIR(st.st_mode);
  p.resolvectl_present = !FindTool("resolvectl").empty();

  std::string nsswitch;
  if (ReadWholeFile("/etc/nsswitch.conf", &nsswitch)) {
    p.nss_resolve_before_dns = NssResolveBeforeDns(nsswitch);
  }

  const std::string resolvconf = FindTool("resolvconf");
  p.resolvconf_present = !resolvconf.empty();
  if (p.resolvconf_present) {
    // MEASURED ON THIS HOST: /usr/bin/resolvconf is a symlink to
    // /usr/bin/resolvectl (systemd's `systemd-resolvconf` compatibility shim).
    // Handing that shim an `-a` when resolved is not running is not a fallback,
    // it is the tier that already failed wearing a different name.
    if (char* real = ::realpath(resolvconf.c_str(), nullptr); real != nullptr) {
      const std::string resolved = real;
      ::free(real);
      const size_t slash = resolved.rfind('/');
      const std::string base = (slash == std::string::npos) ? resolved : resolved.substr(slash + 1);
      p.resolvconf_is_resolvectl = (base == "resolvectl");
    }
  }

  struct stat ls {};
  if (::lstat(kResolvConfPath, &ls) == 0 && S_ISLNK(ls.st_mode)) {
    p.resolv_conf_is_symlink = true;
    char buf[4096];
    const ssize_t n = ::readlink(kResolvConfPath, buf, sizeof(buf) - 1);
    if (n > 0) p.resolv_conf_link_target.assign(buf, static_cast<size_t>(n));
  }
  if (char* real = ::realpath(kResolvConfPath, nullptr); real != nullptr) {
    p.resolv_conf_realpath = real;
    ::free(real);
  }
  std::string content;
  if (ReadWholeFile(kResolvConfPath, &content) &&
      ResolvConfHasNameserver(content, kResolvedStubAddress)) {
    p.resolv_conf_points_at_resolved = true;
  }
  // NOT every file under resolved's runtime dir means "queries go through the
  // stub". resolved generates TWO: stub-resolv.conf (127.0.0.53, the stub, what
  // we want) and resolv.conf (UPLINK MODE — the real upstream servers, listed
  // directly, which deliberately BYPASSES resolved). Treating the second as
  // "points at resolved" would let tier 1 claim a host where the per-link
  // override it sets is not on the path any lookup takes.
  const std::string stubPath = std::string(kResolvedRuntimeDir) + "/stub-resolv.conf";
  if (p.resolv_conf_realpath == stubPath) {
    p.resolv_conf_points_at_resolved = true;
  }

  p.detail = std::string("systemd-resolved ") +
             (p.resolved_running ? "running" : "NOT running") + ", resolvectl " +
             (p.resolvectl_present ? "present" : "absent") + ", nsswitch hosts " +
             (p.nss_resolve_before_dns ? "prefers nss-resolve" : "has no nss-resolve before dns") +
             ", resolvconf " +
             (!p.resolvconf_present ? "absent"
                                    : (p.resolvconf_is_resolvectl ? "is resolvectl's shim"
                                                                  : "present")) +
             ", " + kResolvConfPath +
             // ONLY render the arrow when the path actually resolves somewhere ELSE.
             // On Arch/CachyOS /etc/resolv.conf is commonly a REGULAR FILE that
             // systemd-resolved writes in place rather than a symlink into its
             // runtime dir, so realpath() returns the path itself and the old
             // unconditional " -> " printed
             //     /etc/resolv.conf -> /etc/resolv.conf (resolved's)
             // which reads as a self-referential symlink and sent a tester's log
             // triage chasing a misdetection that was not there. The tier
             // selection was always correct; only this line was wrong.
             (p.resolv_conf_realpath.empty()
                  ? std::string(" missing")
                  : (p.resolv_conf_realpath == kResolvConfPath
                         ? std::string(" is a regular file")
                         : " -> " + p.resolv_conf_realpath)) +
             (p.resolv_conf_points_at_resolved ? " (resolved's)" : "");
  return p;
}

std::string BuildResolvConf(const std::string& iface, const std::vector<std::string>& resolvers) {
  std::string s = kResolvConfMarker;
  s += "\n#\n";
  s += "# urnetworkd replaced this file when the URnetwork tunnel came up";
  if (!iface.empty()) s += " on " + iface;
  s += ",\n";
  s += "# and puts the original back when the tunnel goes down. The original is saved at\n";
  s += std::string("#     ") + kResolvConfBackupPath + "\n";
  s += std::string("# and ") + kResolvConfStatePath + " records exactly how to restore it\n";
  s += "# (it may have been a symlink; that is recorded too).\n";
  s += "#\n";
  s += "# If the tunnel is gone and this file is still here, the daemon was killed. Run\n";
  s += "#     sudo urnetworkd --revert\n";
  s += "# to put the original back.\n";
  s += "#\n";
  s += "# ONLY the tunnel's own resolvers are listed, and there is deliberately no\n";
  s += "# `search`/`domain` line: any other nameserver here, and any search domain a DHCP\n";
  s += "# lease pushed, is a name that would leave this machine outside the tunnel. This\n";
  s += "# is the /etc/resolv.conf equivalent of systemd-resolved's `~.` default-route\n";
  s += "# domain, which the resolvectl path sets for the same reason.\n";
  size_t written = 0;
  for (const auto& resolver : resolvers) {
    if (written >= kMaxResolvConfNameservers) break;
    s += "nameserver " + resolver + "\n";
    ++written;
  }
  if (resolvers.size() > written) {
    // Said out loud rather than silently truncated: glibc reads at most MAXNS
    // (3), so listing more would put a resolver in the file that nothing uses,
    // which is precisely the "believed to be in force but is not" failure this
    // whole section exists to stop producing.
    s += "# " + std::to_string(resolvers.size() - written) +
         " further tunnel resolver(s) are omitted: glibc reads at most " +
         std::to_string(kMaxResolvConfNameservers) + " (MAXNS).\n";
  }
  return s;
}

bool RestoreDirectResolvConf(std::string* detail) {
  const auto say = [detail](std::string message) {
    if (detail) *detail = std::move(message);
  };
  DnsTakeoverState st;
  if (!ReadTakeoverState(&st)) return true;  // nothing was ever taken over

  if (st.backend == "resolvconf") {
    // Tier 2's undo. openresolv/Debian resolvconf keep their own state in /run,
    // so a reboot has already cleared it and the -d is a harmless no-op then.
    const std::string resolvconf = FindTool("resolvconf");
    if (!resolvconf.empty() && !st.iface.empty()) {
      if (const CommandResult r = RunCommand({resolvconf, "-d", st.iface}); !r.ok()) {
        std::fprintf(stderr, "[dns] resolvconf -d %s: %s\n", st.iface.c_str(),
                     r.Describe().c_str());
      }
      RunCommand({resolvconf, "-u"});
    }
    ClearTakeoverState();
    say("deregistered " + st.iface + " from resolvconf");
    return true;
  }

  // Tier 3. IS THE FILE STILL OURS? If NetworkManager, dhcpcd or the admin has
  // rewritten /etc/resolv.conf since we took it over, putting our backup back
  // would clobber a NEWER and more correct file with a stale copy of a state
  // the machine has already left. Detected by our own marker, which is the only
  // thing that can distinguish "our file" from "a file that happens to be here".
  std::string current;
  const bool haveCurrent = ReadWholeFile(kResolvConfPath, &current);
  const bool ours = haveCurrent && current.find(kResolvConfMarker) != std::string::npos;
  if (!ours) {
    ClearTakeoverState();
    say(std::string(kResolvConfPath) +
        " is no longer the file urnetworkd wrote (something else has rewritten it since), so "
        "the saved original was NOT put back; it has been discarded instead of overwriting a "
        "newer file");
    return true;
  }

  std::string error;
  bool ok = false;
  if (st.kind == "symlink") {
    if (st.target.empty()) {
      say("the recorded symlink target is empty, so " + std::string(kResolvConfPath) +
          " could not be restored; " + kResolvConfBackupPath + " is left in place");
      return false;
    }
    // Create the link at a temp name and rename it into place: rename() is
    // atomic for symlinks too, so there is never a moment with no
    // /etc/resolv.conf at all. unlink-then-symlink would have exactly that
    // moment, and every resolver on the machine would see it.
    const std::string tmp = std::string(kResolvConfPath) + ".urnetwork-restore";
    ::unlink(tmp.c_str());
    if (::symlink(st.target.c_str(), tmp.c_str()) != 0) {
      error = std::string("symlink: ") + std::strerror(errno);
    } else if (::rename(tmp.c_str(), kResolvConfPath) != 0) {
      error = std::string("rename: ") + std::strerror(errno);
      ::unlink(tmp.c_str());
    } else {
      FsyncPath(PathDirName(kResolvConfPath), /*directory=*/true);
      ok = true;
    }
  } else if (st.kind == "absent") {
    ok = (::unlink(kResolvConfPath) == 0) || errno == ENOENT;
    if (!ok) error = std::string("unlink: ") + std::strerror(errno);
  } else {
    std::string original;
    if (!ReadWholeFile(kResolvConfBackupPath, &original)) {
      say(std::string("the saved original ") + kResolvConfBackupPath +
          " is missing, so " + kResolvConfPath +
          " still holds the tunnel's resolvers; this machine may not resolve names until it is "
          "written by hand or by NetworkManager/dhcpcd");
      return false;
    }
    // In place when the takeover had to be in place (a bind-mounted
    // /etc/resolv.conf): a rename cannot land on a mount point either way.
    ok = WriteFileAtomic(kResolvConfPath, original, st.mode ? st.mode : 0644,
                         /*inPlaceFallbackOk=*/true, &error);
  }
  if (!ok) {
    say(std::string("could not restore ") + kResolvConfPath + ": " + error +
        ". The original is still at " + kResolvConfBackupPath);
    return false;
  }
  ClearTakeoverState();
  say(std::string("restored ") + kResolvConfPath + " (" + st.kind + ")");
  return true;
}

bool Tunnel::ApplyDnsViaResolved(const DnsHostProbe& probe, std::string* reason) {
  const auto no = [reason](std::string message) {
    if (reason) *reason = std::move(message);
    return false;
  };
  if (!probe.resolvectl_present) {
    return no("systemd-resolved: resolvectl is not installed");
  }
  if (!probe.resolved_running) {
    // THE ARCH CASE, NAMED. The binary is there (the `systemd` package ships
    // it everywhere); the service is not running, which is the Arch/CachyOS
    // default. The shipped build read the first fact and reported the second.
    return no(std::string("systemd-resolved: resolvectl is installed but systemd-resolved is not "
                          "running (") +
              kResolvedRuntimeDir + " does not exist)");
  }
  if (!probe.nss_resolve_before_dns && !probe.resolv_conf_points_at_resolved) {
    // Running, and consulted by nothing. `resolvectl dns` would exit 0 and
    // change where NOTHING goes: glibc would keep reading /etc/resolv.conf,
    // which names somebody else's resolver. This is the second silent leak in
    // the shipped build and it is not Arch-specific.
    return no("systemd-resolved: it is running, but this machine does not resolve through it "
              "(/etc/nsswitch.conf has no nss-resolve before dns and " +
              std::string(kResolvConfPath) + " does not point at the " + kResolvedStubAddress +
              " stub), so a per-link override would change nothing");
  }

  // resolved learns about a new link over rtnetlink, which can land a moment
  // after `ip link set up` — a first "Unknown interface" is a race, not a
  // verdict.
  // Resolved again rather than reusing the probe's answer: the probe records
  // only whether it EXISTED, and an argv whose [0] is the empty string is an
  // execvp that fails for a reason nobody can read.
  const std::string resolvectl = FindTool("resolvectl");
  if (resolvectl.empty()) return no("systemd-resolved: resolvectl vanished mid-bring-up");
  std::vector<std::string> dnsCmd = {resolvectl, "dns", name_};
  dnsCmd.insert(dnsCmd.end(), dnsServers_.begin(), dnsServers_.end());
  CommandResult set;
  for (int attempt = 0; attempt < 3; ++attempt) {
    set = RunCommand(dnsCmd);
    if (set.ok()) break;
    SleepMillis(200);
  }
  if (!set.ok()) return no("systemd-resolved: resolvectl dns: " + set.Describe());
  dnsTouched_ = true;

  // `~.` is resolved's default-route domain: without it a DHCP search domain
  // on the physical link can still win for unqualified names.
  const CommandResult domain = RunCommand({resolvectl, "domain", name_, "~."});
  if (!domain.ok()) return no("systemd-resolved: resolvectl domain: " + domain.Describe());
  // Force plain :53 on this link: never OS-level encrypted DNS for the tunnel.
  // The UpgradeMux performs the unencrypted-DNS -> DoH upgrade in-tunnel and
  // needs to see :53. Not fatal — older systemd has no such verb.
  if (const CommandResult dot = RunCommand({resolvectl, "dnsovertls", name_, "no"}); !dot.ok()) {
    std::fprintf(stderr, "[dns] resolvectl dnsovertls: %s\n", dot.Describe().c_str());
  }
  // The host resolver's cache still holds pre-tunnel answers (and, worse,
  // pre-tunnel addresses for the platform hosts). Windows flushes at exactly
  // this edge.
  if (const CommandResult flush = RunCommand({resolvectl, "flush-caches"}); !flush.ok()) {
    std::fprintf(stderr, "[dns] resolvectl flush-caches: %s\n", flush.Describe().c_str());
  }

  // Read back rather than trust the exit code: this is the field that
  // separates "connected" from "connected and resolving through the tunnel".
  const CommandResult status = RunCommand({resolvectl, "status", name_});
  if (status.ok() && status.output.find(dnsServers_.front()) == std::string::npos) {
    return no("systemd-resolved: it accepted the override but does not report " +
              dnsServers_.front() + " on " + name_);
  }

  dnsBackend_ = DnsBackend::SystemdResolved;
  report_.dns_detail = "DNS is on the tunnel through systemd-resolved (per-link on " + name_ +
                       ", default-route domain `~.`)";
  if (!probe.resolv_conf_points_at_resolved) {
    // Everything that goes through glibc/nss-resolve is pinned. Everything that
    // reads /etc/resolv.conf ITSELF — Go binaries with the pure-Go resolver,
    // musl programs, `dig`, some browsers — still sees the old file. Those
    // queries are REJECTED rather than leaked (the nftables DNS floor closes
    // off-tunnel :53 whenever dns_applied), so this is an outage class, not a
    // leak class, and it is worth saying which.
    report_.dns_detail +=
        ". NOTE: " + std::string(kResolvConfPath) +
        " does not point at systemd-resolved, so programs that read it directly instead of "
        "going through nss-resolve will fail to resolve (the tunnel's DNS floor rejects "
        "off-tunnel :53 rather than letting those queries leak)";
  }
  return true;
}

bool Tunnel::ApplyDnsViaResolvconf(const DnsHostProbe& probe, std::string* reason) {
  const auto no = [reason](std::string message) {
    if (reason) *reason = std::move(message);
    return false;
  };
  if (!probe.resolvconf_present) return no("resolvconf: not installed");
  if (probe.resolvconf_is_resolvectl) {
    return no("resolvconf: the `resolvconf` on this machine is a symlink to resolvectl "
              "(systemd's compatibility shim), so it cannot work where systemd-resolved does not");
  }
  if (probe.resolved_running && probe.nss_resolve_before_dns) {
    return no("resolvconf: systemd-resolved is running and /etc/nsswitch.conf resolves names "
              "through it, so rewriting " +
              std::string(kResolvConfPath) + " would not change where queries go");
  }
  const std::string resolvconf = FindTool("resolvconf");
  if (resolvconf.empty()) return no("resolvconf: not installed");

  // Debian's resolvconf orders interfaces through /etc/resolvconf/interface-order
  // and expects an `<iface>.<prog>`-shaped name; openresolv takes the interface
  // name as-is and additionally understands `-m 0 -x` (highest priority,
  // exclusive), which is what a full-tunnel VPN wants. wg-quick makes the same
  // distinction the same way.
  struct stat st {};
  const bool debianStyle = ::stat("/etc/resolvconf/interface-order", &st) == 0;
  const std::string iface = debianStyle ? ("tun." + name_) : name_;

  std::string stdinData;
  size_t written = 0;
  for (const auto& resolver : dnsServers_) {
    if (written >= kMaxResolvConfNameservers) break;
    stdinData += "nameserver " + resolver + "\n";
    ++written;
  }

  // Not on a RE-APPLY of this same tier: the record on disk is then OUR OWN,
  // and "finishing" it would deregister and immediately re-register, which is a
  // window in which /etc/resolv.conf holds the pre-tunnel resolver again.
  if (dnsBackend_ != DnsBackend::Resolvconf) {
    if (std::string leftover; !FinishLeftoverTakeover(&leftover)) {
      return no("resolvconf: " + leftover);
    }
  }

  // STATE BEFORE MUTATION, the same rule tier 3 follows: a SIGKILL one
  // instruction after the `-a` must still leave something on disk that says
  // "urnetworkd registered an interface with resolvconf", or the registration
  // outlives every process that knows about it. Recording a registration that
  // then fails to happen is the harmless direction — the undo is a `-d` on an
  // unregistered interface, which is a no-op.
  DnsTakeoverState takeover;
  takeover.backend = "resolvconf";
  takeover.iface = iface;
  takeover.kind = "resolvconf";
  if (std::string error; !WriteTakeoverState(takeover, &error)) {
    return no("resolvconf: the undo could not be recorded (" + error +
              "), so the registration was not made");
  }

  // Exclusive first (openresolv), plain second (Debian resolvconf rejects the
  // options). Whichever lands, the deregistration is identical.
  CommandResult add = RunCommand({resolvconf, "-a", iface, "-m", "0", "-x"}, stdinData);
  if (!add.ok()) add = RunCommand({resolvconf, "-a", iface}, stdinData);
  if (!add.ok()) {
    ClearTakeoverState();
    return no("resolvconf: `resolvconf -a " + iface + "`: " + add.Describe());
  }
  resolvconfIface_ = iface;
  RunCommand({resolvconf, "-u"});  // Debian's -a updates already; openresolv's is idempotent

  // VERIFY AGAINST THE FILE A RESOLVER ACTUALLY READS. resolvconf can be
  // configured to feed a local caching resolver instead of writing the
  // nameserver through, and "the command exited 0" is not the question.
  std::string effective;
  if (!ReadWholeFile(kResolvConfPath, &effective) ||
      !ResolvConfHasNameserver(effective, dnsServers_.front())) {
    RunCommand({resolvconf, "-d", iface});
    RunCommand({resolvconf, "-u"});
    ClearTakeoverState();
    resolvconfIface_.clear();
    return no("resolvconf: it accepted the registration but " + std::string(kResolvConfPath) +
              " still does not name " + dnsServers_.front());
  }

  // dnsTouched_ is deliberately NOT set: it means "resolvectl holds a per-link
  // override", and this tier's undo lives on disk instead, precisely so it
  // survives the process.
  dnsBackend_ = DnsBackend::Resolvconf;
  report_.dns_detail = "DNS is on the tunnel through resolvconf (registered as " + iface + ")";
  return true;
}

bool Tunnel::ApplyDnsViaDirectFile(const DnsHostProbe& probe, std::string* reason) {
  const auto no = [reason](std::string message) {
    if (reason) *reason = std::move(message);
    return false;
  };
  if (probe.resolved_running && probe.nss_resolve_before_dns) {
    // Writing the file would be theatre: nss-resolve answers first for every
    // glibc lookup and never consults it. Refusing here (rather than writing
    // and reporting success) is the difference between this build and the one
    // that reported dns_applied on a host where nothing was applied.
    return no("direct /etc/resolv.conf: systemd-resolved is running and /etc/nsswitch.conf "
              "resolves names through it (nss-resolve), so rewriting " +
              std::string(kResolvConfPath) +
              " would not change where queries go. Fix systemd-resolved, or remove `resolve` from "
              "the hosts line in /etc/nsswitch.conf");
  }

  const std::string content = BuildResolvConf(name_, dnsServers_);

  // A RE-APPLY (resolved restarted, or the reaper re-pushed) MUST NOT BACK UP
  // AGAIN. The second backup would capture OUR OWN generated file and the
  // user's original would be gone for good — the classic double-takeover bug.
  if (dnsBackend_ == DnsBackend::DirectFile) {
    struct stat ls {};
    const bool nowSymlink = ::lstat(kResolvConfPath, &ls) == 0 && S_ISLNK(ls.st_mode);
    // The mode comes from the record, not from a literal: re-applying must not
    // quietly change the permissions the first takeover chose from the user's
    // own file.
    DnsTakeoverState recorded;
    const mode_t mode =
        (ReadTakeoverState(&recorded) && recorded.mode != 0) ? recorded.mode : 0644;
    std::string error;
    if (!WriteFileAtomic(kResolvConfPath, content, mode, /*inPlaceFallbackOk=*/!nowSymlink,
                         &error)) {
      return no("direct /etc/resolv.conf: could not re-write it: " + error);
    }
    report_.dns_detail = "DNS is on the tunnel through " + std::string(kResolvConfPath) +
                         " (re-applied; the original is still saved at " +
                         kResolvConfBackupPath + ")";
    return true;
  }

  if (std::string leftover; !FinishLeftoverTakeover(&leftover)) {
    return no("direct /etc/resolv.conf: " + leftover);
  }
  // A stray backup with no state file: a run that died between the two writes,
  // or a restore that already ran. It describes nothing, and leaving it would
  // make the next `mv` in the recovery text put back a file nobody recorded.
  ::unlink(kResolvConfBackupPath);

  DnsTakeoverState takeover;
  takeover.backend = "file";
  takeover.iface = name_;
  takeover.mode = 0644;

  struct stat ls {};
  if (::lstat(kResolvConfPath, &ls) == 0) {
    if (S_ISLNK(ls.st_mode)) {
      // THE SYMLINK CASE. Replacing a symlink's TARGET is not replacing the
      // symlink: it would write into /run/systemd/resolve/stub-resolv.conf or
      // /run/NetworkManager/resolv.conf — files other daemons own and
      // regenerate — and leave /etc/resolv.conf still pointing at them. The
      // link itself is replaced (rename() does that atomically) and the target
      // recorded so teardown recreates exactly the link that was here.
      char buf[4096];
      const ssize_t n = ::readlink(kResolvConfPath, buf, sizeof(buf) - 1);
      if (n <= 0) {
        return no(std::string("direct /etc/resolv.conf: it is a symlink whose target could not be "
                              "read: ") +
                  std::strerror(errno));
      }
      takeover.kind = "symlink";
      takeover.target.assign(buf, static_cast<size_t>(n));
      if (takeover.target.find('\n') != std::string::npos) {
        return no("direct /etc/resolv.conf: it is a symlink whose target contains a newline, "
                  "which cannot be recorded for an exact restore");
      }
      if (struct stat through {}; ::stat(kResolvConfPath, &through) == 0) {
        takeover.mode = through.st_mode & 07777;
      }
    } else if (S_ISREG(ls.st_mode)) {
      takeover.kind = "file";
      takeover.mode = ls.st_mode & 07777;
      std::string original;
      if (!ReadWholeFile(kResolvConfPath, &original)) {
        return no(std::string("direct /etc/resolv.conf: it could not be read, so it cannot be "
                              "restored later: ") +
                  std::strerror(errno));
      }
      if (std::string error;
          !WriteFileAtomic(kResolvConfBackupPath, original, takeover.mode,
                           /*inPlaceFallbackOk=*/false, &error)) {
        return no("direct /etc/resolv.conf: the original could not be saved to " +
                  std::string(kResolvConfBackupPath) + " (" + error +
                  "), so it is not being replaced");
      }
    } else {
      return no("direct /etc/resolv.conf: it is neither a regular file nor a symlink (mode " +
                std::to_string(ls.st_mode & 07777) + "); refusing to touch it");
    }
  } else if (errno == ENOENT) {
    takeover.kind = "absent";
  } else {
    return no(std::string("direct /etc/resolv.conf: it could not be examined: ") +
              std::strerror(errno));
  }

  // STATE BEFORE MUTATION, ALWAYS. A crash between these two writes leaves a
  // state file describing a takeover that did not happen — and the restore
  // detects exactly that, because /etc/resolv.conf will not carry our marker,
  // so it discards the record instead of overwriting a file it never replaced.
  // The other order loses the original with no record at all.
  if (std::string error; !WriteTakeoverState(takeover, &error)) {
    ::unlink(kResolvConfBackupPath);
    return no("direct /etc/resolv.conf: the restore state could not be recorded (" + error +
              "), so the file is not being replaced");
  }

  // inPlaceFallbackOk ONLY for a path that is already a regular file: the
  // fallback follows symlinks, and following one is the mistake this whole
  // branch exists to avoid.
  if (std::string error;
      !WriteFileAtomic(kResolvConfPath, content, takeover.mode,
                       /*inPlaceFallbackOk=*/takeover.kind == "file", &error)) {
    ClearTakeoverState();  // nothing was replaced; leave no phantom ownership
    return no("direct /etc/resolv.conf: " + error);
  }

  // READ BACK. Everything above can succeed against a path that some other
  // layer immediately rewrites (NetworkManager's dns=default plugin does this
  // on every connectivity change), and "we wrote it" is not the question.
  std::string effective;
  if (!ReadWholeFile(kResolvConfPath, &effective) ||
      effective.find(kResolvConfMarker) == std::string::npos ||
      !ResolvConfHasNameserver(effective, dnsServers_.front())) {
    std::string restoreDetail;
    RestoreDirectResolvConf(&restoreDetail);
    return no("direct /etc/resolv.conf: it was written, but reading it back does not show " +
              dnsServers_.front() + " — something else on this machine is rewriting it");
  }

  // Not dnsTouched_ — see the resolvconf tier: this undo is the on-disk one.
  dnsBackend_ = DnsBackend::DirectFile;
  // Say what is actually recorded, per kind: there is NO backup file when the
  // original was a symlink (the target is the record) or when there was no file
  // at all, and naming one that does not exist would send a stuck user looking
  // for it.
  std::string restored;
  if (takeover.kind == "symlink") {
    restored = "it was a symlink to " + takeover.target + ", recorded in " +
               std::string(kResolvConfStatePath);
  } else if (takeover.kind == "absent") {
    restored = "there was no " + std::string(kResolvConfPath) + " before, so teardown removes ours";
  } else {
    restored = "the original is saved at " + std::string(kResolvConfBackupPath);
  }
  report_.dns_detail = "DNS is on the tunnel through " + std::string(kResolvConfPath) + " (" +
                       restored +
                       ", and is put back on disconnect, on the next daemon start, and by "
                       "`urnetworkd --revert`)";
  return true;
}

bool Tunnel::ApplyDns() {
  report_.dns_applied = false;
  report_.dns_detail.clear();
  report_.dns_backend = DnsBackend::None;
  if (dnsServers_.empty()) {
    report_.dns_detail =
        "the device reported no tunnel resolvers, so there is nothing to point DNS at";
    return false;
  }

  const DnsHostProbe probe = ProbeDnsHost();
  std::fprintf(stderr, "[dns] host: %s\n", probe.detail.c_str());

  // The test pin. Three tiers cannot all be exercised on one machine — a host
  // with systemd-resolved never reaches tiers 2 or 3 by itself — so there has
  // to be a way to make a tester's box take the path under test. Refused
  // loudly on a bad value rather than ignored: a typo'd override that silently
  // selects the default is a test that proved nothing.
  std::string forced;
  if (const char* env = ::getenv(kDnsBackendEnv); env != nullptr && *env != '\0') {
    forced = env;
    if (forced != "resolved" && forced != "resolvconf" && forced != "file") {
      report_.dns_detail = std::string(kDnsBackendEnv) + "='" + forced +
                           "' is not one of resolved|resolvconf|file; refusing to guess";
      return false;
    }
    std::fprintf(stderr, "[dns] %s=%s: only that tier will be tried\n", kDnsBackendEnv,
                 forced.c_str());
  }

  struct Tier {
    const char* name;
    bool (Tunnel::*apply)(const DnsHostProbe&, std::string*);
  };
  // THE CONVENTIONAL LINUX VPN ORDER. Most specific and least invasive first;
  // the one that edits a file the whole machine shares is the last resort.
  static constexpr Tier kTiers[] = {
      {"resolved", &Tunnel::ApplyDnsViaResolved},
      {"resolvconf", &Tunnel::ApplyDnsViaResolvconf},
      {"file", &Tunnel::ApplyDnsViaDirectFile},
  };

  const auto succeed = [this]() {
    report_.dns_applied = true;
    report_.dns_backend = dnsBackend_;
    std::fprintf(stderr, "[dns] %s\n", report_.dns_detail.c_str());
    return true;
  };
  const auto tierName = [](DnsBackend b) -> const char* {
    switch (b) {
      case DnsBackend::SystemdResolved: return "resolved";
      case DnsBackend::Resolvconf: return "resolvconf";
      case DnsBackend::DirectFile: return "file";
      case DnsBackend::None: break;
    }
    return "";
  };

  std::string reasons;
  // A RE-APPLY RE-RUNS THE TIER THAT IS ALREADY IN FORCE, FIRST, AND UNDOES IT
  // BEFORE LETTING ANOTHER TIER CLAIM DNS. Not an optimisation: tiers 2 and 3
  // share one on-disk undo record (DirectResolvConfStatePath), so letting tier
  // 3 take over while a tier-2 registration was still live would overwrite the
  // record of the registration with the record of the file takeover and strand
  // the registration with nothing left that knows about it. Undo first, claim
  // second, in that order, always.
  if (const DnsBackend previous = dnsBackend_; previous != DnsBackend::None) {
    const std::string same = tierName(previous);
    if (forced.empty() || forced == same) {
      for (const Tier& tier : kTiers) {
        if (same != tier.name) continue;
        std::string reason;
        if ((this->*tier.apply)(probe, &reason)) return succeed();
        reasons = reason;
        std::fprintf(stderr, "[dns] the tier already in force ('%s') no longer takes: %s\n",
                     tier.name, reason.c_str());
      }
    }
    if (previous == DnsBackend::SystemdResolved) {
      if (const std::string resolvectl = FindTool("resolvectl"); !resolvectl.empty()) {
        RunCommand({resolvectl, "revert", name_});
      }
      dnsTouched_ = false;
    } else {
      std::string undoDetail;
      RestoreDirectResolvConf(&undoDetail);
      if (!undoDetail.empty()) std::fprintf(stderr, "[dns] %s\n", undoDetail.c_str());
      resolvconfIface_.clear();
    }
    dnsBackend_ = DnsBackend::None;
  }

  for (const Tier& tier : kTiers) {
    if (!forced.empty() && forced != tier.name) continue;
    std::string reason;
    if ((this->*tier.apply)(probe, &reason)) return succeed();
    if (!reasons.empty()) reasons += "; ";
    reasons += reason;
    std::fprintf(stderr, "[dns] tier '%s' did not take: %s\n", tier.name, reason.c_str());
  }

  // NOTHING COULD APPLY DNS. Say it in words a user can act on, and say every
  // tier that was tried — "DNS is NOT going through the tunnel" with no reason
  // is what the shipped build said, and it is unactionable.
  //
  // THE FAIL-CLOSED RULE IS ASYMMETRIC TODAY, AND THIS IS WHERE THE GAP IS.
  // Tunnel::Configure refuses a BRING-UP that cannot pin DNS. A re-apply that
  // fails MID-SESSION (this path, reached from TunnelHost::OnResolvedAppeared)
  // only publishes dns_applied=false and the session keeps running — with names
  // leaking, because the nftables DNS floor is gated on dns_applied and comes
  // down with it. Closing that needs a decision in TunnelHost, which this file
  // does not own.
  // TODO(TunnelHost owner): treat a failed re-apply the way a failed egress
  // re-check is treated — tear the session down (or re-arm) rather than
  // continuing with DNS off the tunnel.
  report_.dns_detail =
      "DNS is NOT going through the tunnel: every mechanism was tried and none could take. " +
      reasons +
      ". Install and start systemd-resolved (`sudo systemctl enable --now systemd-resolved`), or "
      "install openresolv, or make " +
      std::string(kResolvConfPath) + " a writable regular file or symlink.";
  return false;
}

bool Tunnel::VerifyDnsStillApplied(std::string* detail) const {
  const auto no = [detail](std::string message) {
    if (detail) *detail = std::move(message);
    return false;
  };
  if (!report_.dns_applied || dnsBackend_ == DnsBackend::None) {
    return no("DNS was never applied on this tunnel");
  }
  if (dnsBackend_ == DnsBackend::SystemdResolved) {
    const std::string resolvectl = FindTool("resolvectl");
    if (resolvectl.empty()) return no("resolvectl has gone away");
    const CommandResult status = RunCommand({resolvectl, "status", name_});
    if (!status.ok()) return no("resolvectl status " + name_ + ": " + status.Describe());
    if (status.output.find(dnsServers_.front()) == std::string::npos) {
      return no("systemd-resolved no longer reports " + dnsServers_.front() + " on " + name_);
    }
    return true;
  }
  std::string effective;
  if (!ReadWholeFile(kResolvConfPath, &effective)) {
    return no(std::string(kResolvConfPath) + " could not be read");
  }
  if (dnsBackend_ == DnsBackend::DirectFile &&
      effective.find(kResolvConfMarker) == std::string::npos) {
    // NetworkManager's dns=default plugin and dhcpcd both do this, on every
    // connectivity change. The DNS floor turns it into an outage rather than a
    // leak; naming it is what makes that outage diagnosable.
    return no(std::string(kResolvConfPath) +
              " has been rewritten by something else since the tunnel came up");
  }
  if (!ResolvConfHasNameserver(effective, dnsServers_.front())) {
    return no(std::string(kResolvConfPath) + " no longer names " + dnsServers_.front());
  }
  return true;
}

// THE DNS UNDO, LIFTED OUT OF ~Tunnel SO IT CAN RUN WHILE THE LINK IS STILL UP.
//
// Nothing in the body below changed; what changed is WHO CAN CALL IT AND WHEN.
// The measured defect: `resolvectl revert urnet0` failed with "Failed to
// resolve interface urnet0: No such device" on every single teardown in the
// journal, because TunnelHost hands tunnel_->fd() to urnet::newIoLoop and then
// closes that io loop BEFORE it destroys this object — Go owns the descriptor,
// a non-persistent tun dies with its last descriptor, and the link was already
// gone by the time ~Tunnel got to speak. The revert was ALREADY the first thing
// ~Tunnel did, so no reordering inside this file could have fixed it.
//
// It is idempotent, which is what lets TunnelHost::StopInternalLocked call it
// early on the ordinary path while ~Tunnel keeps calling it for the paths that
// never reach that stop sequence (a throw out of StartTunnel, a crash unwind).
void Tunnel::RevertDns() {
  if (dnsReverted_) return;
  dnsReverted_ = true;
  // TWO INDEPENDENT UNDOS, NEITHER GATED ON WHICH TIER "WON", because a tier
  // can leave state behind WITHOUT winning. ApplyDnsViaResolved sets the
  // per-link DNS and only then discovers that `resolvectl domain` fails: that
  // returns false, ApplyDns falls through to a lower tier, and dnsBackend_ ends
  // up naming the lower one. A teardown that switched on dnsBackend_ alone
  // would walk straight past resolved's live override — which is exactly the
  // bug shape this file keeps finding: cleanup keyed on the happy path.
  //
  //   dnsTouched_          -> resolvectl holds a per-link override on this tun.
  //   the on-disk state    -> tier 2 or 3 changed something that outlives us.
  if (dnsTouched_ && !name_.empty()) {
    const std::string resolvectl = FindTool("resolvectl");
    if (resolvectl.empty()) {
      // It was present when ApplyDns set the override, so this is a real gap,
      // not a no-op. Benign in practice (resolved drops per-link settings when
      // the link goes away with the fd below) but never silent.
      std::fprintf(stderr,
                   "[dns] WARNING: resolvectl is gone, so the DNS override on %s could not be "
                   "reverted explicitly\n",
                   name_.c_str());
    } else {
      if (const CommandResult r = RunCommand({resolvectl, "revert", name_}); !r.ok()) {
        std::fprintf(stderr, "[dns] resolvectl revert %s: %s\n", name_.c_str(),
                     r.Describe().c_str());
      }
      RunCommand({resolvectl, "flush-caches"});
    }
  }
  // UNCONDITIONAL, and it is the SAME call the crash path makes
  // (NetFilter::SweepStaleState). A teardown that took a different route from
  // the crash recovery is two behaviours that have to agree, and they would
  // stop agreeing the first time either one changed. It costs one failed
  // open() on every machine that never left tier 1.
  {
    std::string detail;
    if (RestoreDirectResolvConf(&detail)) {
      if (!detail.empty()) std::fprintf(stderr, "[dns] %s\n", detail.c_str());
    } else {
      std::fprintf(stderr,
                   "[dns] WARNING: %s. `sudo urnetworkd --revert` retries this, and the next "
                   "daemon start does it automatically.\n",
                   detail.c_str());
    }
  }
}

Tunnel::~Tunnel() {
  // THE SAFETY NET, NOT THE ORDINARY PATH. On a normal stop TunnelHost has
  // already called this while urnet0 was still up (that is the whole point of
  // the split), and the call below is a no-op. It stays here for every path
  // that never reaches TunnelHost's stop sequence — Open() throwing halfway,
  // a bring-up gate failing, the process unwinding — where this destructor is
  // still the only thing that will ever undo the DNS override.
  RevertDns();
  RemovePolicyRules();
  if (fd_ >= 0) {
    ::close(fd_);  // non-persistent tun: closing removes the interface + routes
    fd_ = -1;
  }
  std::fprintf(stderr, "[tun] down %s\n", name_.c_str());
}

}  // namespace urnw
