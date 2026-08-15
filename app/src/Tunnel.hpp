// Opens and configures /dev/net/tun in-process; the fd is handed to the SDK's
// IoLoop. Routes, policy rules, DNS and the nftables leak floor are applied
// like wg-quick (via `ip`, `resolvectl` and `nft`). Since the daemon split
// (linux/MIGRATION.md) this runs inside urnetworkd, which is root under
// systemd — the GUI never touches it. Non-persistent tun: the interface (and
// every route referencing it, in every table) disappears when the fd closes.
//
// THREE THINGS LIVE HERE, in dependency order, because they cannot be
// separated without shipping a tunnel that carries nothing:
//
//   1. RunCommand — fork/execvp with a real argv and a decoded wait status.
//      NEVER a shell: local_addr and dns_servers come back from the device
//      over the user-editable DNS sheet, so std::system() was a root
//      injection surface (audit R: Tunnel.cpp:18).
//
//   2. NetFilter — `table inet urnetwork`, swapped atomically per state with
//      one `nft -f`. It carries two independent jobs:
//        * EGRESS SELF-EXCLUSION (defect R4, "existential"): the daemon's own
//          SDK sockets (jwt refresh, window enumeration, DoH, contract waits,
//          the provider transports) must NOT fall into our own tun the
//          instant the capture routes land. urnet::setEgressInterfaceIndex is
//          a no-op off Windows (connect:egress_other.go), so the mark has to
//          come from outside the process: a `type route hook output` chain
//          matching the daemon's own cgroupv2 path and setting fwmark, paired
//          with the policy rule Tunnel installs. Without it the control plane
//          starves the moment Connect succeeds.
//        * LEAK PREVENTION while Connected — off-tunnel :53/:853/mDNS/LLMNR/
//          NetBIOS and all of IPv6 — which per docs/linux_agent_help.md §6.3
//          is NOT a preference: it applies regardless of the kill switch. The
//          kill switch adds the block-everything floor on top.
//
//   3. Tunnel — the tun fd, its address, the 31 capture prefixes (in a
//      DEDICATED route table, not main, so the fwmark rule can steer the
//      daemon around them), the systemd-resolved DNS override, and a
//      deterministic self-check that BOTH halves took before the caller is
//      allowed to call the tunnel up.
//
// Everything here reports failure as a distinct code + message pair; nothing
// returns a bare bool that renders as "empty" three layers up.
//
// C++ port of the (retired) Go tunnel.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace urnw {

// ---- process execution -----------------------------------------------------

struct CommandResult {
  bool spawned = false;    // fork+exec reached the program at all
  int exit_code = -1;      // WEXITSTATUS (only meaningful when spawned && !signalled)
  int term_signal = 0;     // WTERMSIG, 0 when the child exited normally
  std::string output;      // merged stdout+stderr, trailing whitespace trimmed

  bool ok() const { return spawned && term_signal == 0 && exit_code == 0; }
  // One line naming what actually happened — never a bare number. (The old
  // std::system() path logged a WAIT STATUS as if it were an exit code, so an
  // exit 1 printed as 256.)
  std::string Describe() const;
};

// fork/execvp with a real argv vector: no shell, so no quoting and no
// injection. stdin_data is fed to the child (for `nft -f -`) and both output
// streams are drained concurrently with poll(), so neither side can deadlock
// on a full pipe.
CommandResult RunCommand(const std::vector<std::string>& argv,
                         const std::string& stdin_data = std::string());

// Absolute path of `tool` on $PATH (access X_OK), or "" when it is absent.
// Used by the daemon preflight so a broken environment is named BEFORE the
// first Connect instead of during it.
std::string FindTool(const char* tool);

// inet_pton, in the daemon, on every address the device hands back. The GUI's
// client-side validation (Formatters.cpp) is a courtesy, not a boundary.
bool IsIpv4Address(const std::string& value);

// ---- the daemon's own cgroup ----------------------------------------------

// The cgroup v2 path of THIS process, as nftables wants it: no leading slash,
// plus the component count. Derived at runtime from /proc/self/cgroup rather
// than hardcoding "system.slice/urnetworkd.service", so a --foreground dev run
// (user.slice/...) marks its sockets correctly too.
struct CgroupRef {
  bool valid = false;
  std::string path;  // e.g. "system.slice/urnetworkd.service"
  int level = 0;     // component count; nft's `socket cgroupv2 level N`
};
CgroupRef SelfCgroupV2();

// ---- constants shared by the routes and the firewall -----------------------

// fwmark carried by the daemon's own sockets. "URNW" as a u32; distinctive
// enough that a collision with another tool's mark scheme is implausible, and
// it is set (not or'd) only on our own sockets.
inline constexpr uint32_t kEgressMark = 0x55524e57u;
// The capture routes live HERE, never in main: main keeps the physical
// default, which is what the marked daemon sockets fall through to.
// One above wg-quick's 51820 so a co-installed WireGuard cannot collide.
inline constexpr int kTunnelRouteTable = 51821;
// Rule priorities, both below main (32766) and both below wg-quick's
// 32764/32765 so the two never fight over the same slot.
inline constexpr int kSuppressRulePriority = 32762;
inline constexpr int kFwmarkRulePriority = 32763;
inline constexpr const char* kNftTableName = "urnetwork";

// The split-default capture set: the whole IPv4 space MINUS 10.0.0.0/8,
// 172.16.0.0/12 and 192.168.0.0/16, i.e. the same 31-prefix complement
// Android (MainService's excludeRoute set), iOS (NEIPv4Settings.excludedRoutes)
// and windows NetPolicy.h use, so LAN traffic reaches local devices directly.
// 169.254/16 and 224.0.0.0/3 stay CAPTURED on purpose (own tun addr; metadata
// service; LLMNR/mDNS live in the multicast range).
const std::vector<std::string>& CaptureV4Prefixes();

// ---- nftables: egress self-exclusion + the leak floor ----------------------

enum class FilterState {
  Off,        // no table at all
  Idle,       // mark chain only (+ the block floor when the kill switch is on)
  Connected,  // mark chain + tun permits + the DNS/IPv6 leak floor
};

const char* ToString(FilterState s);

struct FilterConfig {
  FilterState state = FilterState::Off;
  std::string tun_name;                  // required by Connected
  bool kill_switch = false;              // adds the block-everything floor
  bool block_ipv6 = true;                // Connected: v6 has no tunnel, so it must not leak
  bool block_offtunnel_dns = true;       // Connected: :53/:853/5353/5355/137-139 off-tun
  uint32_t mark = kEgressMark;
  CgroupRef cgroup;                      // whose sockets get the mark
};

// PURE: the exact `nft -f` script for a state, so the ruleset can be reviewed
// (and diffed against the Windows filter inventory) without touching the
// kernel. Uses the standard atomic-swap idiom — declare, delete, recreate in
// one file — so there is never an all-open or all-blocked window.
std::string BuildNftRuleset(const FilterConfig& cfg);

class NetFilter {
 public:
  NetFilter() = default;
  ~NetFilter();

  NetFilter(const NetFilter&) = delete;
  NetFilter& operator=(const NetFilter&) = delete;

  // Swaps the whole table to `cfg`. On failure the PREVIOUS ruleset stays in
  // force (nft -f is transactional) and *error carries nft's own stderr.
  bool Apply(const FilterConfig& cfg, std::string* error);
  // `delete table inet urnetwork`. Missing table is success.
  bool Remove(std::string* error);

  // Startup recovery. nftables rules are NOT tied to process lifetime (unlike
  // the Windows dynamic WFP session), so an unclean exit leaves the table and
  // the policy rules installed; the daemon sweeps them by NAME/mark/table id
  // before it serves anyone. The systemd unit should also carry
  // `ExecStopPost=-/usr/sbin/nft delete table inet urnetwork`.
  static void SweepStaleState();

  FilterState state() const { return state_; }
  bool installed() const { return state_ != FilterState::Off; }
  const std::string& lastError() const { return lastError_; }

 private:
  FilterState state_ = FilterState::Off;
  std::string lastError_;
};

// ---- the tun ---------------------------------------------------------------

struct TunnelConfig {
  std::string name = "urnet0";
  std::string local_addr = "169.254.2.1";
  int prefix = 24;
  int mtu = 1440;
  std::vector<std::string> dns_servers;
  // Refuse to install the capture routes unless the daemon's own sockets are
  // demonstrably steered around them. Only a dev run
  // (URNETWORK_ALLOW_UNPROTECTED_EGRESS=1) may clear this, and it is logged
  // loudly, because the alternative is a tunnel that comes up and carries
  // nothing while the UI says Connected.
  bool require_egress_protection = true;
};

// What is ACTUALLY in force, as opposed to what was attempted. Every field
// here is reported through ctl::StatusReply so the UI can say "routes are in
// but DNS is not" instead of a single green dot.
struct TunnelReport {
  std::string interface;
  bool routes_installed = false;
  bool egress_protected = false;  // the fwmark escape hatch verified via `ip route get`
  bool dns_applied = false;
  std::string dns_detail;         // why not, when dns_applied is false
  std::string route_detail;       // the first failing step, verbatim
};

// A failure that has to survive three layers and still be actionable.
struct TunnelError {
  std::string message;
  std::string code;  // one of ctl::kCode* — the UI branches on this, not on prose
};

// RAII tun device. Closing the fd removes the (non-persistent) interface and
// every route referencing it, in every table; the POLICY RULES are not tied to
// the link and are removed here explicitly.
class Tunnel {
 public:
  // `err` is always filled on failure (never left empty).
  static std::unique_ptr<Tunnel> Open(const TunnelConfig& cfg, TunnelError* err);
  ~Tunnel();

  Tunnel(const Tunnel&) = delete;
  Tunnel& operator=(const Tunnel&) = delete;

  int fd() const { return fd_; }
  const std::string& name() const { return name_; }
  const TunnelReport& report() const { return report_; }

  // Re-push the systemd-resolved override. Called when resolved restarts
  // (it forgets per-link settings across a restart, and a DHCP lease can beat
  // our `~.` default-route domain — docs/linux_agent_help.md R5).
  bool ApplyDns();

 private:
  Tunnel() = default;
  bool Configure(const TunnelConfig& cfg, TunnelError* err);
  bool InstallRoutes(TunnelError* err);
  bool InstallPolicyRules(TunnelError* err);
  void RemovePolicyRules();
  bool VerifyEgressSplit(TunnelError* err);

  int fd_ = -1;
  std::string name_;
  std::vector<std::string> dnsServers_;
  bool rulesInstalled_ = false;
  bool dnsTouched_ = false;
  TunnelReport report_;
};

}  // namespace urnw
