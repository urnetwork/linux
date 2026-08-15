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

}  // namespace

// ---- process execution -----------------------------------------------------

std::string CommandResult::Describe() const {
  if (!spawned) return "could not run the command";
  std::string out;
  if (term_signal != 0) {
    out = "killed by signal " + std::to_string(term_signal);
  } else if (exit_code == 127) {
    out = "command not found";
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
    ::_exit(127);  // "command not found", decoded in Describe()
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

// ---- nftables --------------------------------------------------------------

const char* ToString(FilterState s) {
  switch (s) {
    case FilterState::Off: return "off";
    case FilterState::Idle: return "idle";
    case FilterState::Connected: return "connected";
  }
  return "off";
}

std::string BuildNftRuleset(const FilterConfig& cfg) {
  const std::string mark = MarkHex(cfg.mark);
  std::string s;
  // Atomic swap idiom: declare, delete, recreate — all in one -f file, so
  // there is never an all-open or an all-blocked window between states.
  s += "add table inet ";
  s += kNftTableName;
  s += "\ndelete table inet ";
  s += kNftTableName;
  s += "\ntable inet ";
  s += kNftTableName;
  s += " {\n";

  // 1) EGRESS SELF-EXCLUSION. `type route` so the mark triggers a re-route of
  //    the packet the daemon just produced; the paired `ip rule` sends
  //    anything NOT carrying this mark into the tunnel table.
  s += "  chain urnw_mark_out {\n";
  s += "    type route hook output priority -150; policy accept;\n";
  if (cfg.cgroup.valid) {
    s += "    socket cgroupv2 level " + std::to_string(cfg.cgroup.level) + " \"" +
         cfg.cgroup.path + "\" counter meta mark set " + mark + "\n";
  }
  s += "  }\n";

  // 2) The leak floor. Order is load-bearing and is the Windows sublayer
  //    ordering flattened into one chain: our own sockets, then loopback, then
  //    the tun, THEN the DNS block (before the LAN permit — otherwise the LAN
  //    permit re-opens the router's resolver, which is exactly the hole
  //    WfpPolicy's separate Dns sublayer exists to close), then v6, then the
  //    LAN/DHCP permits, then the policy.
  s += "  chain urnw_out {\n";
  s += "    type filter hook output priority 0; policy ";
  s += (cfg.kill_switch ? "drop" : "accept");
  s += ";\n";
  s += "    meta mark " + mark + " accept\n";
  s += "    oifname \"lo\" accept\n";
  const bool connected = cfg.state == FilterState::Connected && !cfg.tun_name.empty();
  if (connected) {
    s += "    oifname \"" + cfg.tun_name + "\" accept\n";
  }
  if (connected && cfg.block_offtunnel_dns) {
    // Everything below is off-tunnel by construction (the tun accepted above)
    // and is not ours (the mark accepted above). Windows filter inventory:
    // block-dns 53, DoT 853, mDNS 5353, LLMNR 5355, NetBIOS 137/138/139.
    s += "    udp dport 53 drop\n";
    s += "    tcp dport 53 drop\n";
    s += "    udp dport 853 drop\n";
    s += "    tcp dport 853 drop\n";
    s += "    udp dport 5353 drop\n";
    s += "    udp dport 5355 drop\n";
    s += "    tcp dport 5355 drop\n";
    s += "    udp dport { 137, 138 } drop\n";
    s += "    tcp dport 139 drop\n";
  }
  if (connected && cfg.block_ipv6) {
    // The SDK tunnel is IPv4-only, so every AAAA-reachable destination would
    // otherwise leave in the clear over the physical NIC while the UI says
    // Connected. Link-local/ULA/multicast stay up for NDP, DHCPv6 and LAN.
    // reject, not drop: Happy Eyeballs then fails over to v4 immediately
    // instead of waiting out a timeout.
    s += "    ip6 daddr fe80::/10 accept\n";
    s += "    ip6 daddr fc00::/7 accept\n";
    s += "    ip6 daddr ff00::/8 accept\n";
    s += "    meta nfproto ipv6 reject\n";
  }
  // DHCP must survive the kill-switch floor or the lease dies under it.
  s += "    udp dport { 67, 68 } accept\n";
  s += "    udp dport { 546, 547 } accept\n";
  // The LAN bypass, the same set the routes exclude (NetPolicy.h: routes and
  // firewall are built from ONE table so they can never disagree).
  s += "    ip daddr { 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16, "
       "224.0.0.0/4, 255.255.255.255 } accept\n";
  s += "  }\n";
  s += "}\n";
  return s;
}

NetFilter::~NetFilter() {
  if (state_ != FilterState::Off) {
    std::string ignored;
    Remove(&ignored);
  }
}

bool NetFilter::Apply(const FilterConfig& cfg, std::string* error) {
  if (cfg.state == FilterState::Off) return Remove(error);
  const std::string nft = FindTool("nft");
  if (nft.empty()) {
    lastError_ = "nftables (nft) is not installed";
    if (error) *error = lastError_;
    return false;
  }
  const std::string script = BuildNftRuleset(cfg);
  const CommandResult r = RunCommand({nft, "-f", "-"}, script);
  if (!r.ok()) {
    // nft -f is transactional: on failure the PREVIOUS ruleset is still in
    // force, so state_ is deliberately left alone.
    lastError_ = "nft -f: " + r.Describe();
    if (error) *error = lastError_;
    std::fprintf(stderr, "[filter] apply %s failed: %s\n", ToString(cfg.state),
                 lastError_.c_str());
    return false;
  }
  lastError_.clear();
  state_ = cfg.state;
  std::fprintf(stderr, "[filter] %s (kill_switch=%d ipv6_blocked=%d dns_floor=%d cgroup=%s)\n",
               ToString(cfg.state), cfg.kill_switch ? 1 : 0,
               (cfg.state == FilterState::Connected && cfg.block_ipv6) ? 1 : 0,
               (cfg.state == FilterState::Connected && cfg.block_offtunnel_dns) ? 1 : 0,
               cfg.cgroup.valid ? cfg.cgroup.path.c_str() : "(none)");
  return true;
}

bool NetFilter::Remove(std::string* error) {
  const std::string nft = FindTool("nft");
  if (nft.empty()) {
    // Nothing we could have installed, so nothing to undo.
    state_ = FilterState::Off;
    return true;
  }
  // add-then-delete: idempotent, so "the table was never there" is success.
  const std::string script = std::string("add table inet ") + kNftTableName +
                             "\ndelete table inet " + kNftTableName + "\n";
  const CommandResult r = RunCommand({nft, "-f", "-"}, script);
  state_ = FilterState::Off;
  if (!r.ok()) {
    lastError_ = "nft delete table: " + r.Describe();
    if (error) *error = lastError_;
    return false;
  }
  lastError_.clear();
  return true;
}

void NetFilter::SweepStaleState() {
  // nftables rules and ip rules are NOT tied to process lifetime (the Windows
  // dynamic WFP session is; that safety is not inherited here), so an unclean
  // exit leaves the machine holding a policy nobody owns. Sweep by our own
  // table NAME / mark / table id only — never by priority alone, which would
  // let us delete a co-installed WireGuard's rules.
  if (const std::string nft = FindTool("nft"); !nft.empty()) {
    const std::string script = std::string("add table inet ") + kNftTableName +
                               "\ndelete table inet " + kNftTableName + "\n";
    const CommandResult r = RunCommand({nft, "-f", "-"}, script);
    if (!r.ok()) {
      std::fprintf(stderr, "[filter] startup sweep: %s\n", r.Describe().c_str());
    }
  }
  const std::string ip = FindTool("ip");
  if (ip.empty()) return;
  const std::string table = std::to_string(kTunnelRouteTable);
  for (int i = 0; i < 8; ++i) {
    if (!RunCommand({ip, "-4", "rule", "delete", "table", table}).ok()) break;
    std::fprintf(stderr, "[filter] startup sweep: removed a stale policy rule\n");
  }
  for (int i = 0; i < 8; ++i) {
    const CommandResult r =
        RunCommand({ip, "-4", "rule", "delete", "pref", std::to_string(kSuppressRulePriority),
                    "table", "main", "suppress_prefixlength", "0"});
    if (!r.ok()) break;
  }
  RunCommand({ip, "-4", "route", "flush", "table", table});
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
                    "permission denied opening /dev/net/tun: the daemon is missing "
                    "CAP_NET_ADMIN");
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
      return fail("tun_permission_denied",
                  "TUNSETIFF was refused: the daemon is missing CAP_NET_ADMIN");
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
  // Idempotence: an unclean previous run may have left these behind, and
  // `ip rule add` happily installs a DUPLICATE rather than answering EEXIST.
  RunCommand({ip, "-4", "rule", "delete", "pref", std::to_string(kSuppressRulePriority),
              "table", "main", "suppress_prefixlength", "0"});
  for (int i = 0; i < 8; ++i) {
    if (!RunCommand({ip, "-4", "rule", "delete", "table", table}).ok()) break;
  }

  // wg-quick's shape, one priority band lower so a co-installed WireGuard
  // cannot collide:
  //   32762  main, ignoring default routes  -> explicit host/LAN routes still win
  //   32763  everything NOT carrying our mark -> the tunnel table
  //   32766  main (kernel)                  -> where our own marked sockets land
  const std::vector<std::vector<std::string>> rules = {
      {ip, "-4", "rule", "add", "pref", std::to_string(kSuppressRulePriority), "table", "main",
       "suppress_prefixlength", "0"},
      {ip, "-4", "rule", "add", "not", "fwmark", MarkHex(kEgressMark), "table", table, "pref",
       std::to_string(kFwmarkRulePriority)},
  };
  for (const auto& rule : rules) {
    const CommandResult r = RunCommand(rule);
    if (!r.ok()) {
      report_.route_detail = JoinArgv(rule) + ": " + r.Describe();
      if (err != nullptr) {
        err->code = "route_install_failed";
        err->message = "could not install the tunnel policy rule: " + r.Describe();
      }
      return false;
    }
  }
  rulesInstalled_ = true;
  return true;
}

void Tunnel::RemovePolicyRules() {
  if (!rulesInstalled_) return;
  rulesInstalled_ = false;
  const std::string ip = FindTool("ip");
  if (ip.empty()) return;
  const std::string table = std::to_string(kTunnelRouteTable);
  for (int i = 0; i < 8; ++i) {
    if (!RunCommand({ip, "-4", "rule", "delete", "table", table}).ok()) break;
  }
  RunCommand({ip, "-4", "rule", "delete", "pref", std::to_string(kSuppressRulePriority),
              "table", "main", "suppress_prefixlength", "0"});
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
  // TODO(egress-counter): this proves the ROUTING POLICY is right — a marked
  // packet leaves via the physical NIC and an unmarked one via the tun. It
  // does NOT prove nftables is actually applying the mark to the daemon's own
  // sockets: `nft -f` accepting the cgroupv2 expression and the path resolving
  // to a live cgroup are strong evidence, but not a measurement. Close the
  // gap by reading the `counter` on the urnw_mark_out rule a second after the
  // device comes up (`nft -j list chain inet urnetwork urnw_mark_out`) and
  // failing the start when it is still zero, since the SDK is guaranteed to
  // have transmitted by then. Requires a live daemon to calibrate the delay,
  // which is why it is not written here.
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
    if (const std::string resolvectl = FindTool("resolvectl"); !resolvectl.empty()) {
      RunCommand({resolvectl, "revert", name_});
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
