// SPDX-License-Identifier: MPL-2.0
#include "Tunnel.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
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
// (192.0.0.0/9), so `ip route get` exercises exactly the policy we installed
// without naming anyone's real resolver and without emitting a packet.
constexpr const char* kEgressProbeAddress = "192.0.2.1";

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

// `ip route get` prints one line beginning with the destination; the outgoing
// interface is the token after " dev ". Returns "" when the answer has none
// (unreachable, or an error line).
std::string DeviceFromRouteGet(const std::string& output) {
  const size_t nl = output.find('\n');
  const std::string line = output.substr(0, nl == std::string::npos ? output.size() : nl);
  const size_t pos = line.find(" dev ");
  if (pos == std::string::npos) return {};
  const size_t start = pos + 5;
  size_t end = line.find(' ', start);
  if (end == std::string::npos) end = line.size();
  return line.substr(start, end - start);
}

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
  d.helper_dns = cfg.state == FilterState::Connecting && cfg.floor &&
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

// PURE: `counter packets N bytes M` out of a chain listing.
bool ParseChainCounters(const std::string& listing, uint64_t* packets, uint64_t* bytes) {
  const size_t at = listing.find("counter packets ");
  if (at == std::string::npos) return false;
  const char* p = listing.c_str() + at + std::strlen("counter packets ");
  char* end = nullptr;
  const unsigned long long pk = std::strtoull(p, &end, 10);
  if (end == p) return false;
  const size_t bat = listing.find(" bytes ", at);
  unsigned long long by = 0;
  if (bat != std::string::npos) {
    const char* bp = listing.c_str() + bat + std::strlen(" bytes ");
    by = std::strtoull(bp, nullptr, 10);
  }
  if (packets != nullptr) *packets = static_cast<uint64_t>(pk);
  if (bytes != nullptr) *bytes = static_cast<uint64_t>(by);
  return true;
}

// Shared by NetFilter::MarkChainCounters and Tunnel::VerifyEgressSplit so both
// read the same rule the same way.
bool ReadMarkChainCounters(uint64_t* packets, uint64_t* bytes, std::string* error) {
  const auto fail = [error](std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
  };
  const std::string nft = FindTool("nft");
  if (nft.empty()) return fail("nftables (nft) is not installed");
  const CommandResult r =
      RunCommand({nft, "list", "chain", "inet", kNftTableName, kNftMarkChainName});
  if (!r.ok()) {
    return fail(std::string("nft list chain ") + kNftMarkChainName + ": " + r.Describe());
  }
  if (!ParseChainCounters(r.output, packets, bytes)) {
    // No counter means no cgroup rule: nothing is marking the daemon's own
    // sockets, so the egress self-exclusion is not actually in force.
    return fail(std::string("the ") + kNftMarkChainName +
                " chain carries no counter: the daemon's own sockets are not being marked");
  }
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
  const bool cg = CgroupQuotable(cfg.cgroup);

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

  // -- 0. EGRESS SELF-EXCLUSION (R4). `type route` so the mark triggers the
  //    routing re-lookup for the packet the daemon just produced; the paired
  //    `ip rule` sends anything NOT carrying this mark into the tunnel table.
  line(std::string("\tchain ") + kNftMarkChainName + " {");
  line("\t\ttype route hook output priority " + std::to_string(kMarkChainPriority) +
       "; policy accept;");
  if (cg) {
    line("\t\t" + CgroupMatch(cfg.cgroup) + " counter meta mark set " + mark);
  }
  line("\t}");
  line("");

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

  // A floor we cannot exempt ourselves from is a machine that is blocked AND
  // structurally unable to reconnect: with no cgroup match nothing sets the
  // mark, so neither permit fires for the daemon's own sockets. Refuse with
  // the cause named rather than install it.
  if (cfg.floor && !CgroupInstallable(cfg.cgroup)) {
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
  if (!cfg.dns_helper_cgroups.empty()) {
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
  if (!CgroupInstallable(cfg.cgroup) && cfg.cgroup.valid) {
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
               "cgroup=%s)\n",
               ToString(cfg.state), cfg.floor ? 1 : 0, RulesetBlocksIpv6(cfg) ? 1 : 0,
               RulesetPinsDns(cfg) ? 1 : 0, RulesetOpensHelperDns(cfg) ? 1 : 0,
               cfg.allow_lan ? 1 : 0, cfg.cgroup.valid ? cfg.cgroup.path.c_str() : "(none)");
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

bool NetFilter::MarkChainCounters(uint64_t* packets, uint64_t* bytes, std::string* error) const {
  return ReadMarkChainCounters(packets, bytes, error);
}

bool NetFilter::SweepStaleState(bool preserveArmed) {
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

  // --- validate every value that came back from the device BEFORE it reaches
  //     any command line. The daemon must not rely on the GUI's client-side
  //     inet_pton (Formatters.cpp): the DNS sheet is user-editable and the
  //     values arrive here over the device RPC.
  if (!ValidInterfaceName(cfg.name)) {
    return fail("tun_config_invalid", "invalid tun interface name '" + cfg.name + "'");
  }
  if (!IsIpv4Address(cfg.local_addr)) {
    return fail("tun_config_invalid",
                "the device reported an invalid tunnel address '" + cfg.local_addr + "'");
  }
  if (cfg.prefix < 0 || cfg.prefix > 32) {
    return fail("tun_config_invalid",
                "invalid tunnel prefix length " + std::to_string(cfg.prefix));
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
  for (const auto& dns : cfg.dns_servers) {
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
               t->name_.c_str(), cfg.local_addr.c_str(), cfg.prefix, cfg.mtu, dnsList.c_str(),
               t->report_.routes_installed ? 1 : 0, t->report_.egress_protected ? 1 : 0,
               t->report_.dns_applied ? 1 : 0);
  return t;
}

bool Tunnel::Configure(const TunnelConfig& cfg, TunnelError* err) {
  const std::string ip = FindTool("ip");
  const std::string addr = cfg.local_addr + "/" + std::to_string(cfg.prefix);
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
  // the tun and (b) the daemon's own marked traffic still does not. Failing
  // (b) is defect R4 and means the control plane starves the instant we
  // return success.
  if (cfg.require_egress_protection) {
    if (!VerifyEgressSplit(err)) return false;
    report_.egress_protected = true;
  } else {
    std::fprintf(stderr,
                 "[tun] URNETWORK_ALLOW_UNPROTECTED_EGRESS is set: the daemon's own sockets "
                 "are NOT excluded from this tunnel. The SDK control plane will starve. "
                 "Development only.\n");
    report_.egress_protected = false;
  }

  ApplyDns();  // never fatal here: reported through TunnelReport::dns_applied
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

bool Tunnel::VerifyEgressSplit(TunnelError* err) {
  const std::string ip = FindTool("ip");
  const CommandResult captured = RunCommand({ip, "-4", "route", "get", kEgressProbeAddress});
  const CommandResult escaped =
      RunCommand({ip, "-4", "route", "get", kEgressProbeAddress, "mark", MarkHex(kEgressMark)});
  const std::string capturedDev = DeviceFromRouteGet(captured.output);
  const std::string escapedDev = DeviceFromRouteGet(escaped.output);

  if (capturedDev != name_) {
    if (err != nullptr) {
      err->code = "route_install_failed";
      err->message = "the capture routes did not take: traffic for " +
                     std::string(kEgressProbeAddress) + " still leaves via '" +
                     (capturedDev.empty() ? std::string("(no route)") : capturedDev) + "'";
    }
    return false;
  }
  if (escapedDev.empty() || escapedDev == name_) {
    // This is the R4 failure, caught deterministically instead of as a hang:
    // the daemon's own SDK sockets would fall into our own tun and the
    // control plane would starve the moment we returned success.
    if (err != nullptr) {
      err->code = "egress_unprotected";
      err->message =
          "the daemon's own traffic would be captured by its own tunnel (egress "
          "self-exclusion is not in force). Check that nftables is installed and that "
          "cgroup v2 is in use.";
    }
    return false;
  }
  std::fprintf(stderr, "[tun] egress split verified: unmarked -> %s, daemon (mark %s) -> %s\n",
               capturedDev.c_str(), MarkHex(kEgressMark).c_str(), escapedDev.c_str());

  // The two probes above prove the ROUTING POLICY is right — a marked packet
  // leaves via the physical NIC and an unmarked one via the tun. They do NOT
  // prove nftables is actually APPLYING the mark to the daemon's own sockets.
  // The counter on the mark chain is that measurement.
  uint64_t packets = 0;
  uint64_t bytes = 0;
  std::string counterError;
  if (!ReadMarkChainCounters(&packets, &bytes, &counterError)) {
    // Not fatal: the rule may be absent because the whole floor could not be
    // installed, which the caller already reports with its own code. Never let
    // a diagnostic be the thing that fails a start.
    std::fprintf(stderr, "[tun] mark chain counter unavailable: %s\n", counterError.c_str());
  } else if (packets == 0) {
    // TODO(egress-counter): this is still only a WARNING. Turning a zero
    // counter into a hard refusal needs a live daemon to calibrate how long
    // after bring-up the SDK is guaranteed to have transmitted; refusing on a
    // number that is legitimately zero for another 200 ms would fail starts
    // that are fine. Re-read it on the reaper tick once that delay is known.
    std::fprintf(stderr,
                 "[tun] WARNING: the %s chain has not marked a single packet yet; the daemon's "
                 "own sockets may not be excluded from this tunnel\n",
                 kNftMarkChainName);
  } else {
    std::fprintf(stderr, "[tun] egress mark applied to %llu packet(s), %llu byte(s)\n",
                 static_cast<unsigned long long>(packets),
                 static_cast<unsigned long long>(bytes));
  }
  return true;
}

bool Tunnel::ApplyDns() {
  report_.dns_applied = false;
  report_.dns_detail.clear();
  if (dnsServers_.empty()) {
    report_.dns_detail = "the device reported no tunnel resolvers";
    return false;
  }
  const std::string resolvectl = FindTool("resolvectl");
  if (resolvectl.empty()) {
    // Never silently keep the host resolver and call the session Connected:
    // the caller reports this verbatim through StatusReply::dns_detail.
    report_.dns_detail =
        "systemd-resolved (resolvectl) is not installed: DNS is NOT going through the tunnel";
    return false;
  }

  // resolved learns about a new link over rtnetlink, which can land a moment
  // after `ip link set up` — a first "Unknown interface" is a race, not a
  // verdict.
  std::vector<std::string> dnsCmd = {resolvectl, "dns", name_};
  dnsCmd.insert(dnsCmd.end(), dnsServers_.begin(), dnsServers_.end());
  CommandResult set;
  for (int attempt = 0; attempt < 3; ++attempt) {
    set = RunCommand(dnsCmd);
    if (set.ok()) break;
    SleepMillis(200);
  }
  if (!set.ok()) {
    report_.dns_detail = "resolvectl dns: " + set.Describe();
    return false;
  }
  dnsTouched_ = true;

  // `~.` is resolved's default-route domain: without it a DHCP search domain
  // on the physical link can still win for unqualified names.
  const CommandResult domain = RunCommand({resolvectl, "domain", name_, "~."});
  if (!domain.ok()) {
    report_.dns_detail = "resolvectl domain: " + domain.Describe();
    return false;
  }
  // Force plain :53 on this link: never OS-level encrypted DNS for the tunnel.
  // The UpgradeMux performs the unencrypted-DNS -> DoH upgrade in-tunnel and
  // needs to see :53. Not fatal — older systemd has no such verb.
  if (const CommandResult dot = RunCommand({resolvectl, "dnsovertls", name_, "no"});
      !dot.ok()) {
    std::fprintf(stderr, "[tun] resolvectl dnsovertls: %s\n", dot.Describe().c_str());
  }
  // The host resolver's cache still holds pre-tunnel answers (and, worse,
  // pre-tunnel addresses for the platform hosts). Windows flushes at exactly
  // this edge.
  if (const CommandResult flush = RunCommand({resolvectl, "flush-caches"}); !flush.ok()) {
    std::fprintf(stderr, "[tun] resolvectl flush-caches: %s\n", flush.Describe().c_str());
  }

  // Read back rather than trust the exit code: this is the field that
  // separates "connected" from "connected and resolving through the tunnel".
  const CommandResult status = RunCommand({resolvectl, "status", name_});
  if (status.ok() && status.output.find(dnsServers_.front()) == std::string::npos) {
    report_.dns_detail =
        "systemd-resolved accepted the override but does not report " + dnsServers_.front() +
        " on " + name_;
    return false;
  }
  report_.dns_applied = true;
  return true;
}

Tunnel::~Tunnel() {
  if (dnsTouched_ && !name_.empty()) {
    const std::string resolvectl = FindTool("resolvectl");
    if (resolvectl.empty()) {
      // It was present when ApplyDns set the override, so this is a real gap,
      // not a no-op. Benign in practice (resolved drops per-link settings when
      // the link goes away with the fd below) but never silent.
      std::fprintf(stderr,
                   "[tun] WARNING: resolvectl is gone, so the DNS override on %s could not be "
                   "reverted explicitly\n",
                   name_.c_str());
    } else {
      if (const CommandResult r = RunCommand({resolvectl, "revert", name_}); !r.ok()) {
        std::fprintf(stderr, "[tun] resolvectl revert %s: %s\n", name_.c_str(),
                     r.Describe().c_str());
      }
      RunCommand({resolvectl, "flush-caches"});
    }
  }
  RemovePolicyRules();
  if (fd_ >= 0) {
    ::close(fd_);  // non-persistent tun: closing removes the interface + routes
    fd_ = -1;
  }
  std::fprintf(stderr, "[tun] down %s\n", name_.c_str());
}

}  // namespace urnw
