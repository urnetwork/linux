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
//   2. NetFilter — `table inet urnetwork`, ONE table with FOUR base chains
//      (urnw_mark_out at the route hook, urnw_out/urnw_in/urnw_fwd at the
//      filter hooks), swapped atomically per state with one `nft -f`. `inet`
//      and not ip+ip6 so both families are seen at every hook and the v6
//      fail-closed rules and the kill-switch floor sit in the SAME chain,
//      ordered against each other explicitly, in ONE transaction — two tables
//      would race at exactly the transition edges where a one-packet window is
//      the whole defect. It carries two independent jobs:
//        * EGRESS SELF-EXCLUSION (defect R4, "existential"): the daemon's own
//          SDK sockets (jwt refresh, window enumeration, DoH, contract waits,
//          the provider transports) must NOT fall into our own tun the
//          instant the capture routes land. Without it the control plane
//          starves the moment Connect succeeds — or worse, the daemon's own
//          packets are captured, read back out of the tun by the SDK's IoLoop,
//          re-sent and captured again. That amplifying loop was MEASURED on a
//          real machine on 2026-08-15: 1.34 Gbps out, 0 in, 3.38 Tb sent over
//          forty minutes before a human noticed.
//
//          THE MECHANISM HAS TWO LAYERS, AND THE ORDER IS NOT A PREFERENCE.
//          A fwmark only excludes a socket if the socket carries it BEFORE
//          connect(): the route lookup that picks the tun (and, with it, the
//          tun's source address) happens in connect(), and `ip_route_me_harder`
//          — the re-lookup a `type route` chain triggers at LOCAL_OUT — keeps
//          the source address that was already chosen (it sets
//          FLOWI_FLAG_ANYSRC for an RTN_LOCAL saddr and reuses it). So:
//            1. EgressSocketMarker: a four-instruction BPF_PROG_TYPE_CGROUP_SOCK
//               program attached at BPF_CGROUP_INET_SOCK_CREATE to the daemon's
//               own cgroup, which sets sk->sk_mark on every AF_INET/AF_INET6
//               socket this process creates, at socket() time. The vendored SDK
//               is untouched — the kernel does it from outside the process,
//               which is the only way in, because urnet::setEgressInterfaceIndex
//               is a no-op off Windows (connect:egress_other.go is an 11-line
//               `return nil`) and the SDK exposes no dialer Control hook.
//            2. The `urnw_mark_out` `type route hook output` chain matching the
//               daemon's cgroupv2 path. It CANNOT repair a source address that
//               connect() already bound, so it is a BELT, not the mechanism: it
//               still saves sockets that were created before the marker
//               attached, and it is the only layer at all on a host without
//               CONFIG_CGROUP_BPF.
//          Neither layer is trusted. Tunnel::VerifyEgressWitness connect()s two
//          real sockets that differ only in the mark and compares the source
//          addresses the kernel binds to them, then reads nftables counters
//          kept over the daemon's REAL traffic. It sends nothing, it needs no
//          hole in the floor, and the tunnel does not come up — or stay up —
//          unless all four of its legs pass.
//        * LEAK PREVENTION — off-tunnel :53/:853/mDNS/LLMNR/NetBIOS, the cloud
//          metadata address, and all globally routable IPv6 — which per
//          docs/linux_agent_help.md §6.3 is NOT a preference: it applies
//          regardless of the kill switch. The kill switch adds the
//          block-everything floor on top, and the OFF variant of a ruleset is
//          the ON variant minus exactly two things (the three chain policies
//          and the trailing floor rule), so the diff is reviewable at a glance
//          and OFF is always a strict subset of ON.
//
//      NOT INHERITED FROM WINDOWS: nftables is not process-bound, so there is
//      no equivalent of FWPM_SESSION_FLAG_DYNAMIC. A daemon that is SIGKILLed
//      while armed leaves the machine BLOCKED — deliberately, and made safe by
//      four independent recoveries: the GUI toggle always works (AF_UNIX is
//      never routed, so no ruleset this file can produce can block the app
//      from telling the daemon to disarm), Restart=on-failure plus the armed
//      marker re-installs the floor with no open window, ExecStopPost lifts it
//      on a CLEAN exit only, and NetFilter::RecoveryCommand() /
//      RecoveryHelpText() are the printed last resort — printed BY THE DAEMON
//      (--help, --diagnose, the journal at the moment the floor lands and at
//      every failed teardown) and carried to the UI over the control protocol,
//      because a string that only ever reaches stderr inside this file is not a
//      recovery for a user who cannot reach the network. Nothing here notices tampering by itself: a root
//      `nft flush ruleset` (which Fedora's shipped nftables.conf begins with)
//      destroys the table silently, and NetFilter::Verify() on the reaper tick
//      is the entire mitigation.
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

// True when /sys/fs/cgroup/<path> exists AND the path is safe to interpolate
// into a quoted nft string. MANDATORY before emitting any `socket cgroupv2`
// rule: nft resolves the path to a cgroup id at LOAD time and fails the ENTIRE
// transaction when it is absent —
//     Error: cgroupv2 path fails: No such file or directory
// — which would take the kill switch AND the egress self-exclusion down with
// it. (Measured with `nft --check -f` on this host; nothing was applied.)
bool CgroupV2PathExists(const std::string& path);

// The cgroups whose port-53 egress is permitted for the length of a
// Connecting-with-floor transition. On a systemd-resolved host the SDK's wire
// DNS query does NOT leave the daemon's sockets: a cgo-enabled Go binary uses
// getaddrinfo -> nss-resolve -> D-Bus, and resolved issues the query from its
// OWN cgroup. That is the Linux reappearance of Windows' svchost/Dnscache
// problem. Probes a fixed candidate list and returns only the cgroups that
// exist. Empty is legal (then the daemon depends on Go's in-process resolver,
// which the mark/cgroup permit already covers) and is logged either way.
std::vector<CgroupRef> DnsHelperCgroupsV2();

// ---- egress self-exclusion at socket-creation time -------------------------
//
// ============================================================================
// EVERY LEAK THE CHOSEN MECHANISM TRADES AWAY. Written here, in full, because
// the previous design's costs were discoverable only by reading three files
// and a kernel source tree, and because a mechanism whose price is not written
// down is a mechanism nobody can review.
//
//  1. THE FWMARK EXEMPTION IS FORGEABLE. Anything holding CAP_NET_ADMIN can
//     setsockopt(SO_MARK, 0x55524e57) and inherit this daemon's bypass: both
//     the `ip rule not fwmark ... table 51821` and the `meta mark ... accept`
//     fallback in urnw_out honour it. This leak EXISTS TODAY — it is exactly
//     why urnw_out ranks the cgroup match ABOVE the mark match — but making
//     SO_MARK the primary route-steering mechanism promotes it from fallback
//     to load-bearing, and that promotion is a real change. On this machine
//     CAP_NET_ADMIN already means root, and root can delete the whole table
//     anyway, so it buys an attacker nothing they did not have. The ACCEPT
//     ranking stays cgroup-first regardless; only the ROUTE-STEERING job moves
//     to the socket mark.
//
//  2. EVERY SOCKET THIS PROCESS OPENS GOES OFF-TUNNEL, not only the SDK's.
//     The cgroup-bpf program is per-process-tree, so the `ip`, `nft` and
//     `resolvectl` children this daemon forks are marked too. None of them
//     originate network traffic (resolvectl talks to resolved over D-Bus on a
//     unix socket; ip and nft use netlink), so the practical scope is the
//     SDK's own connections — which is the intent — but the mechanism is not
//     capable of being narrower than "this process tree".
//
//  3. THE WITNESS NEEDS NO HOLE IN THE FLOOR, AND NO LONGER HAS ONE. Builds
//     before this one carried `ip daddr 192.0.2.1 udp dport 9 counter accept`
//     in urnw_out for as long as the table was installed — a standing accept in
//     the kill-switch floor whose entire purpose was to let a synthetic probe
//     reach a counter. It is deleted. The two designs were weighed:
//       (a) OPEN THE PERMIT FOR THE INSTANT THE WITNESS RUNS, CLOSE IT AFTER.
//           REJECTED, and not on taste. Opening and closing a rule means two
//           extra `nft -f` transactions per witness run, i.e. FOUR MORE ATOMIC
//           TABLE REPLACES A MINUTE at the 30 s re-check — and every table
//           replace destroys and recreates every counter in the table, which is
//           the exact defect that made the shipped measurement meaningless
//           ("the generation it measured is DESTROYED before any traffic
//           flows"). It would also leave the floor holed if the daemon died
//           between the open and the close, in the one state — armed, no
//           tunnel — where the floor is all the user has.
//       (b) A WITNESS THAT NEEDS NO PERMIT. TAKEN. It needs no permit because
//           it sends no packet: two connect() calls (which perform the route
//           lookup and bind a source address without putting anything on the
//           wire) plus two nftables counters that watch the daemon's own REAL
//           traffic by MARK rather than by probe destination. Nothing to
//           permit, nothing to open, nothing to close, and the instrument now
//           watches the SDK's actual packets for the whole life of the ruleset
//           instead of four synthetic ones per half-minute.
//
//  4. THE WITNESS EMITS NOTHING. connect() on a datagram socket performs the
//     FIB lookup and binds the source address; it transmits no packet. The
//     daemon therefore no longer sends anything to 192.0.2.1, or anywhere else,
//     on account of being measured. The address survives only as a destination
//     to resolve — chosen because it is routed nowhere and sits inside the
//     capture set.
//
//  5. AVAILABILITY, TRADED ON PURPOSE, AND THE TRADE JUST GOT STRICTER. On a
//     host where the socket mark cannot be established at CREATION time — no
//     CONFIG_CGROUP_BPF, an SELinux policy that denies bpf(), a cgroup v1
//     net_cls hierarchy that suppresses socket-to-cgroup association — the
//     witness fails and the tunnel REFUSES TO START OR IS TORN DOWN. The
//     nftables belt alone is NOT accepted as a pass any more: it acts at
//     NF_INET_LOCAL_OUT, after connect() has bound the source address, so a
//     "belt only" green was a green on a daemon whose next dialled connection
//     is born unanswerable. That was the last false-confidence branch in this
//     file and it is gone. The trade, stated plainly: availability on exotic
//     hosts, in exchange for never again reporting a protected tunnel that is
//     not one. URNETWORK_ALLOW_UNPROTECTED_EGRESS=1 is the documented
//     development escape and skips the witness entirely rather than running it
//     and ignoring the answer.
//
//  6. THE DNS HELPER PERMIT IS UNCHANGED AND IS STILL A LEAK. On a
//     systemd-resolved host the SDK's wire query leaves RESOLVED's cgroup, not
//     ours, so no mechanism here covers it; the bounded Connecting-only
//     port-53 permit (DnsHelperCgroupsV2) remains the mitigation and remains a
//     small, scoped hole.
//
// REJECTED, AND WHY — so nobody re-proposes them as expedients:
//   * `meta skuid 0` / `ip rule uidrange 0-0`: both work today with zero SDK
//     changes, and both exempt EVERY root process on the machine — sshd,
//     NetworkManager including its periodic connectivity probe, chronyd,
//     rpm-ostree and system Flatpak updates (on Bazzite that is the OS update
//     path), cups, root containers, system cron. The user's real IP would be
//     exposed on a timer by NM's probe alone while the UI says Connected, and
//     urnw_out would ACCEPT all of it, so the kill switch would not catch it
//     either. That converts a full tunnel into a split tunnel whose other half
//     is the operating system. Not an acceptable price for a control-plane bug.
//   * A dedicated non-root uid + `ip rule uidrange`: mechanically the
//     strongest option (fib rules match flowi4_uid inside the ORIGINAL route
//     lookup, so it needs no socket association and no reroute) and it leaks
//     only that one uid. It requires User= in the unit, which urnetworkd.service
//     rejects with measured SELinux evidence (nnp_transition / execute_no_trans
//     on /usr/bin/nft and /usr/sbin/ip). Not adoptable without reopening that.
//   * Excluding provider endpoints by destination: the endpoint set is dynamic
//     (platform API, DoH, and p2p peers over pion/WebRTC with ICE/STUN/TURN),
//     the binding exposes provider IDENTITIES and not their addresses, and
//     every exemption would be a permanent hole in the capture set — which the
//     brief forbids and which a recycled IP turns into a silent bypass.
//   * Reviving `suppress_prefixlength`: retired at kSuppressRulePriority for
//     the documented reason that it suppresses only prefix length 0. Stays
//     retired.
// ============================================================================

// THE PRIMARY R4 MECHANISM. Attaches a four-instruction
// BPF_PROG_TYPE_CGROUP_SOCK program at BPF_CGROUP_INET_SOCK_CREATE to the
// daemon's own cgroup v2 directory. The program's whole body is
// `sk->mark = kEgressMark; return 1;`, so every AF_INET/AF_INET6 socket this
// process creates carries the mark from the moment inet_create() returns —
// before connect(), which is the ONLY point at which it can do any good.
//
// WHY THIS AND NOT THE nftables MARK CHAIN ALONE. A `type route hook output`
// chain runs at NF_INET_LOCAL_OUT, i.e. AFTER the route lookup and AFTER
// source-address selection. For a socket that connect()s once the capture
// routes are in, `ip rule not fwmark ... table 51821` has already sent the
// lookup into the capture table, the dst is the tun, and inet_saddr is bound
// to the TUN'S OWN address. Setting the mark then triggers
// ip_route_me_harder(), which re-runs the route lookup — but for an RTN_LOCAL
// source it sets FLOWI_FLAG_ANYSRC and REUSES the address already chosen. The
// packet leaves the physical NIC sourced from 169.254.2.1 and no reply can
// ever come back. A "successfully marked" daemon socket is a dead daemon
// socket. Marking at socket creation removes the whole problem: the FIB lookup
// in connect() never consults table 51821, so no tun route and no tun source
// address is ever chosen, and no per-packet reroute is needed.
//
// WHY IT IS POSSIBLE AT ALL WITH THE VENDORED SDK. It needs nothing from the
// SDK. The SDK's sockets are created by Go inside this process, in this
// process's cgroup, and the kernel applies the program on our behalf. The
// alternatives all require something we do not have: SO_MARK from C++ needs a
// net.Dialer.Control hook the SDK ABI does not export; `ip rule uidrange`
// needs a dedicated non-root uid the unit cannot take (SELinux nnp_transition,
// see urnetworkd.service); `meta skuid 0` would exempt every root process on
// the machine.
//
// EVERYTHING HERE IS MEASURED, NOT ASSUMED. Attach() ends by creating a
// throwaway UDP socket and reading SO_MARK back off it. If the verifier
// rejected the program, the kernel lacks CONFIG_CGROUP_BPF, SELinux denied
// bpf(), or the program simply did not run, that read-back says so and Attach()
// reports failure instead of leaving a silent no-op in place. That discipline
// is the whole point: the shipped bug was a green check on the wrong question.
class EgressSocketMarker {
 public:
  EgressSocketMarker() = default;
  ~EgressSocketMarker();

  EgressSocketMarker(const EgressSocketMarker&) = delete;
  EgressSocketMarker& operator=(const EgressSocketMarker&) = delete;

  // Loads, attaches and then PROVES the program by reading SO_MARK back off a
  // fresh socket. Idempotent: a second call on an attached marker is a no-op
  // that re-runs the proof. *error is always filled on false.
  bool Attach(const CgroupRef& cgroup, uint32_t mark, std::string* error);
  // Detaches and closes both fds. Safe on an unattached marker. Called by the
  // destructor; the cgroup dying with the unit would clean up anyway.
  void Detach();
  bool attached() const { return attached_; }
  // One line for the journal and for TunnelReport::egress_mechanism.
  const std::string& detail() const { return detail_; }

  // Does a socket created RIGHT NOW by this process carry `mark`? This is the
  // read-back Attach() proves itself with, exposed so the daemon can answer
  // "is the marker actually working" with no tunnel in existence (--diagnose,
  // the preflight, a log line at attach time).
  //
  // THE WITNESS DELIBERATELY DOES NOT CALL IT. Tunnel::VerifyEgressWitness
  // reads SO_MARK off the VERY SOCKET whose routing decision it is measuring,
  // because a mark read from one throwaway socket and a source address read
  // from a different one are two facts about two objects — and joining them
  // costs an assumption that nothing checks. One socket, one mark, one binding.
  static bool SelfSocketMark(uint32_t* mark, std::string* error);

 private:
  int progFd_ = -1;
  int cgroupFd_ = -1;
  bool attached_ = false;
  std::string detail_;
};

// ---- constants shared by the routes and the firewall -----------------------

// fwmark carried by the daemon's own sockets. "URNW" as a u32; distinctive
// enough that a collision with another tool's mark scheme is implausible, and
// it is set (not or'd) only on our own sockets.
inline constexpr uint32_t kEgressMark = 0x55524e57u;
// The capture routes live HERE, never in main: main keeps the physical
// default, which is what the marked daemon sockets fall through to.
// One above wg-quick's 51820 so a co-installed WireGuard cannot collide.
inline constexpr int kTunnelRouteTable = 51821;
// The ONE policy rule this file installs: everything NOT carrying kEgressMark
// is looked up in kTunnelRouteTable, and a destination that table does not
// claim (the three RFC1918 prefixes the capture set omits) falls through to
// main at 32766 on its own — `ip rule` continues to the next rule when the
// named table has no matching route. Below main (32766) and below wg-quick's
// 32764/32765 so the two never fight over the same slot.
inline constexpr int kFwmarkRulePriority = 32763;
// RETIRED — SWEPT, NEVER INSTALLED. Builds before this one put
//     ip rule pref 32762 table main suppress_prefixlength 0
// AHEAD of the capture rule, copying wg-quick so that "explicit host/LAN routes
// still win". `suppress_prefixlength 0` suppresses ONLY prefix length 0, i.e.
// the default route, so EVERY other route in main outranked the capture table:
// another VPN's 0.0.0.0/1 + 128.0.0.0/1 pair, or any pushed /24, silently took
// traffic out of this tunnel and the nftables floor was the only thing left
// catching it — and the floor is the user's optional kill switch, so a leak
// depended on a preference. Nothing needs main to be consulted FIRST: the
// daemon's own transport is steered around the tunnel by the mark, LAN traffic
// is excluded from the capture set by construction, and everything else is
// exactly what a full tunnel is supposed to carry. The constant survives so
// InstallPolicyRules and SweepStaleState can delete a copy left behind by an
// older build or an unclean exit.
inline constexpr int kSuppressRulePriority = 32762;
inline constexpr const char* kNftTableName = "urnetwork";

// The four base chains of `table inet urnetwork`. ONE table, both families:
// an `inet` table sees v4 and v6 at every hook, so the IPv6 fail-closed rules
// and the kill-switch floor sit in the same chain, ordered against each other
// explicitly, in one `nft -f` transaction. Two tables would race at exactly
// the transition edges (arm/disarm, connect/disconnect) where a one-packet
// window is the entire defect.
inline constexpr const char* kNftMarkChainName = "urnw_mark_out";
inline constexpr const char* kNftOutChainName = "urnw_out";
inline constexpr const char* kNftInChainName = "urnw_in";
inline constexpr const char* kNftFwdChainName = "urnw_fwd";
// The FIFTH chain, and the only one that decides nothing: every rule in it is
// a bare `counter name`, its policy is accept and it carries no verdict, so it
// cannot change the fate of any packet. It exists to answer, over the daemon's
// REAL traffic, the question `ip route get` cannot ask — did a packet this
// daemon actually sent leave through the tunnel or around it.
//
// IT MATCHES kEgressMark, NOT A PROBE DESTINATION, AND THAT IS WHY THE FLOOR
// HAS NO HOLE IN IT. Matching a synthetic probe's destination forced the
// witness to emit datagrams, which forced a standing `accept` for them in the
// kill-switch floor. Matching the mark instead means the counters are fed by
// every packet the SDK sends anyway, so the witness reads and never writes.
//
// POSTROUTING, NOT OUTPUT, AND THIS IS LOAD-BEARING. nf_hook_state.out is
// captured from skb_dst(skb)->dev BEFORE any output hook runs, so
// ip_route_me_harder()'s re-lookup leaves `oifname` STALE for the rest of that
// traversal: at the output hook a successfully re-routed packet still reports
// the tun. Counting the final interface at the output hook would have
// reproduced the original mistake — a green check on the wrong question — in a
// new place. At postrouting the device is the real one.
inline constexpr const char* kNftProbeChainName = "urnw_probe";

// Named counters, so the readers find them BY NAME instead of by position in a
// chain listing. `nft` is free to print rules in any order it likes and a
// positional parse would silently invert the verdict.
//   urnw_out_total  — every packet traversing urnw_mark_out (the DENOMINATOR
//                     the shipped build never had: it counted matches with
//                     nothing to compare them against, so "2 packets" was
//                     equally consistent with a working rule and a dead one).
//   urnw_out_daemon — the packets the cgroup match claimed.
//   urnw_probe_tun  — MARKED DAEMON PACKETS that left through OUR OWN TUN.
//                     This is the 2026-08-15 storm signature exactly: the
//                     packet the SDK's io loop reads back out of the tun and
//                     re-sends. It is created at zero by every atomic table
//                     swap and only ever increments, so there is no delta to
//                     compute and nothing to straddle. ONE IS THE WHOLE DEFECT.
//   urnw_probe_phy  — marked daemon packets that left via a real interface, as
//                     they must. `lo` is excluded: the daemon's own device RPC
//                     on 127.0.0.1 carries the mark like everything else it
//                     opens, and counting it would inflate "left the machine"
//                     with traffic that never left the machine.
inline constexpr const char* kNftMarkTotalCounter = "urnw_out_total";
inline constexpr const char* kNftMarkDaemonCounter = "urnw_out_daemon";
inline constexpr const char* kNftProbeTunCounter = "urnw_probe_tun";
inline constexpr const char* kNftProbePhyCounter = "urnw_probe_phy";

// The address the witness's two sockets RESOLVE — never an address it sends
// to. 192.0.2.1 is RFC 5737 TEST-NET-1: reserved for documentation, routed
// nowhere, and inside the capture set (192.0.0.0/9), so a connect() to it
// exercises exactly the policy we installed without naming anyone's real host.
// Port 9 is RFC 863 discard, and is now only the port half of a sockaddr that
// no datagram is ever handed to.
//
// kEgressProbePackets IS GONE, WITH THE PACKETS. It said "send four datagrams
// per bring-up and per 30 s re-check", and those datagrams are what required a
// permanent accept for 192.0.2.1:9 in the kill-switch floor. The witness now
// measures two connect() calls (which transmit nothing) and reads counters the
// kernel keeps over the SDK's own traffic, so there is no sample size left to
// choose and no hole left to open.
inline constexpr int kEgressProbePort = 9;

// NF_IP_PRI_MANGLE. The mark chain must run BEFORE the routing re-lookup the
// `ip rule` performs, and > NF_IP_PRI_CONNTRACK (-200) so `ct state` is
// available to the filter chains downstream.
inline constexpr int kMarkChainPriority = -150;
// NF_IP_PRI_FILTER. Post-DNAT, so the LAN and metadata matches are on the
// address the packet actually goes to. Priority is NOT the defence here: at
// every hook every registered base chain runs, `accept` is terminal only for
// the current chain, and `drop`/`reject` are terminal for the PACKET — so no
// other table can turn our drop into an accept, whatever its priority.
inline constexpr int kFilterChainPriority = 0;
// Bound on the Connecting DNS-helper permit: the reaper narrows a wedged
// bring-up back to Armed after this many seconds.
inline constexpr int kConnectingWindowSeconds = 60;

// The split-default capture set: the whole IPv4 space MINUS 10.0.0.0/8,
// 172.16.0.0/12 and 192.168.0.0/16, i.e. the same 31-prefix complement
// Android (MainService's excludeRoute set), iOS (NEIPv4Settings.excludedRoutes)
// and windows NetPolicy.h use, so LAN traffic reaches local devices directly.
// 169.254/16 and 224.0.0.0/3 stay CAPTURED on purpose (own tun addr; metadata
// service; LLMNR/mDNS live in the multicast range).
const std::vector<std::string>& CaptureV4Prefixes();

// ---- nftables: egress self-exclusion + the leak floor ----------------------

// `Idle` used to conflate two states that need genuinely different rulesets:
// a bring-up in flight (which needs the tun permits by NAME and, on a
// resolved host, a bounded DNS path) and a tunnel that dropped under us
// (which must have nothing but the floor). They are split.
enum class FilterState {
  Off,         // no table at all
  Connecting,  // bring-up in flight: mark chain + tun permits by name + v6 floor
  Armed,       // no tunnel, block floor only. Nothing on the machine resolves.
  Connected,   // mark chain + tun permits + the pinned DNS + the leak floor
};

const char* ToString(FilterState s);

// The single authority on whether a transition carries the block floor. PURE.
// `previous` is NetFilter::state() BEFORE the transition, and it is the whole
// point: installing the bring-up state with the floor unconditionally lifted
// is correct for a FIRST connect (a failed start must never cut the machine
// off the net) but on a RECONNECT AFTER AN UNEXPECTED DROP it would lift the
// kill switch for the entire attempt — exactly the window the kill switch
// exists to close. Splitting on `previous` gets both.
//   Off        -> false
//   Armed      -> true (structural; an Armed state without a floor is nothing)
//   Connected  -> killSwitchRequested
//   Connecting -> killSwitchRequested && previous == Armed
bool FloorForTransition(FilterState previous, FilterState next, bool killSwitchRequested);

struct FilterConfig {
  FilterState state = FilterState::Off;
  // NOT the user's toggle: the toggle is an INPUT to FloorForTransition, this
  // is its OUTPUT, and BuildNftRuleset reads only this. One knob, one meaning.
  bool floor = false;
  // Matched with oifname/iifname, NEVER oif/iif. Measured: `oif "urnet0"`
  // resolves the interface index at LOAD time and is refused while the
  // interface is absent, `oifname "urnet0"` is a per-packet string match and
  // loads fine. That is the exact Linux inverse of the Windows LUID-not-index
  // rule and it is load-bearing: the tun permit can be installed BEFORE the
  // tun exists, which removes the blackhole window between "capture routes
  // land" and "widen to Connected".
  std::string tun_name;
  // The resolvers ACTUALLY validated and applied on the tun (Tunnel::resolvers).
  // Connected pins :53 to these, over the tun only.
  std::vector<std::string> tunnel_resolvers;
  // v6 has no tunnel (the SDK captures IPv4 only), so on a dual-stack network
  // every AAAA-reachable destination would leave in the clear while the UI
  // says Connected. In force for Connecting/Armed/Connected and NEVER gated on
  // the kill switch — leak prevention is not a preference (§6.3).
  bool block_ipv6 = true;
  // :53/:853/5353/5355/137-139 off-tunnel. AND'd by the builder with
  // "we are Connected" AND "a tunnel resolver survived validation": installing
  // the port-53 block with no permitted path is a total resolution outage
  // rather than a leak.
  bool block_offtunnel_dns = true;
  // The three RFC1918 prefixes the capture set deliberately EXCLUDES. Routes
  // and firewall are built from ONE table or they silently disagree.
  bool allow_lan = true;
  uint32_t mark = kEgressMark;
  // The daemon's own. Permits by CGROUP, not by mark: SO_MARK needs
  // CAP_NET_ADMIN/CAP_NET_RAW, so a privileged third party could forge our
  // mark and inherit the exemption; cgroup membership cannot be forged. The
  // mark match stays as a ranked-below fallback for packets whose socket
  // lookup misses in the output hook.
  CgroupRef cgroup;
  // Emitted ONLY when state == Connecting && floor, and only for cgroups that
  // exist (see DnsHelperCgroupsV2).
  std::vector<CgroupRef> dns_helper_cgroups;
};

// PURE: the exact `nft -f` script for a state, so the ruleset can be reviewed
// (and diffed against the Windows filter inventory) without touching the
// kernel. Free of every syscall, so it stays unit-testable on a machine where
// applying a rule is impossible. Uses the standard atomic-swap idiom —
// declare, delete, recreate in one file — so there is never an all-open or an
// all-blocked window.
std::string BuildNftRuleset(const FilterConfig& cfg);

// PURE predicates over the SAME config BuildNftRuleset reads (they share its
// derivation), so status, logs and tests ask one function instead of grepping
// the emitted text.
bool RulesetHasBlockFloor(const FilterConfig& cfg);
bool RulesetBlocksIpv6(const FilterConfig& cfg);
bool RulesetPinsDns(const FilterConfig& cfg);
bool RulesetOpensHelperDns(const FilterConfig& cfg);  // log at BOTH edges

// Read-only probe (`ip route show default table all`, both families): TRUE
// only when there is an IPv6 default route and NO IPv4 one. Arming the v6
// fail-closed floor on such a network cuts the machine off with no v4 path
// left for the daemon either, and the only way back is the GUI toggle or
// `urnetworkd --revert`. Windows refuses first in exactly this case. NOTE the
// asymmetry: NO default route at all (the link just went away) is NOT this
// condition — that is precisely when arming must still work.
bool IsIpv6OnlyNetwork(std::string* detail);

// The DISTINCT failure codes NetFilter::lastErrorCode() can carry. Kept as
// literals, exactly like TunnelError::code, so this header stays free of
// ControlProtocol.hpp (and of nlohmann): the values are byte-identical to the
// ctl::kCode* constants the reply carries. WIRING: ControlProtocol.hpp must
// gain kCodeNftMissing/kCodeNftRejected/kCodeCgroupUnavailable/
// kCodeIpv4DefaultRouteMissing with these same strings.
inline constexpr const char* kFilterCodeNftMissing = "nft_missing";
inline constexpr const char* kFilterCodeNftRejected = "nft_rejected";
inline constexpr const char* kFilterCodeCgroupUnavailable = "cgroup_unavailable";
inline constexpr const char* kFilterCodeIpv4DefaultRouteMissing = "ipv4_default_route_missing";

class NetFilter {
 public:
  NetFilter() = default;
  ~NetFilter();

  NetFilter(const NetFilter&) = delete;
  NetFilter& operator=(const NetFilter&) = delete;

  // Swaps the whole table to `cfg`. On failure the PREVIOUS ruleset stays in
  // force (nft -f is transactional), state_ is deliberately not touched, and
  // *error carries nft's own stderr. lastErrorCode() names WHICH failure.
  bool Apply(const FilterConfig& cfg, std::string* error);
  // `delete table inet urnetwork`, via the add-then-delete idiom so a missing
  // table is success. Idempotent; safe to call from any exit path, including
  // one that already failed.
  //
  // A TEARDOWN THAT DID NOT HAPPEN STAYS KNOWN-NOT-DONE. On failure state_,
  // floor_ and appliedConfig() are left exactly as they were, because the table
  // is still in the kernel and this object still owns it: that is what makes
  // ~NetFilter retry, what keeps floorInstalled() (and therefore
  // StatusReply::kill_switch) telling the user the truth about why they are cut
  // off, and what stops a caller reading a failed removal as a completed one.
  // Bounded-retries internally before it reports the failure; *error and
  // lastErrorCode() name it, and the recovery command is logged.
  bool Remove(std::string* error);

  // `nft --check -f -`: parse + evaluate, NEVER commit. Use in the daemon
  // preflight and in tests, so a malformed ruleset is named before it is ever
  // the thing standing between the user and their network.
  static bool CheckRuleset(const std::string& script, std::string* error);

  // Is the table still ours and intact? nftables gives no tamper callback, and
  // a root `nft flush ruleset` destroys our table with no notification —
  // which is exactly what Fedora's shipped /etc/sysconfig/nftables.conf begins
  // with, so `systemctl restart nftables` silently disarms the kill switch.
  // CALL ON THE REAPER TICK and re-Apply appliedConfig() on false. This is the
  // replacement for the crash/tamper safety the Windows dynamic WFP session
  // gets for free, and it is a poll, so there is a tick-length window.
  bool Verify(std::string* error) const;

  // The two named counters on urnw_mark_out: how many packets the cgroup match
  // CLAIMED, and how many crossed the chain AT ALL. The ratio is the number the
  // shipped build could not compute, because it counted matches with no
  // denominator — which is why "egress mark applied to 2 packet(s)" was read as
  // reassurance when it was in fact uninterpretable.
  //
  // THIS IS DIAGNOSTIC ONLY AND IS NOT A GATE. It is a CUMULATIVE counter, so
  // once two packets have ever matched it can never fall back to zero; and
  // every Apply() destroys and recreates the table, which resets it. The gate
  // is Tunnel::VerifyEgressWitness, whose four legs include the one counter
  // that IS unforgiving in this direction (urnw_probe_tun must be exactly
  // zero) and two live sockets whose bindings cannot be green while the
  // capture policy is absent.
  bool MarkChainCounters(uint64_t* daemonPackets, uint64_t* totalPackets,
                         std::string* error) const;

  // Startup recovery. nftables rules are NOT tied to process lifetime (unlike
  // the Windows dynamic WFP session), so an unclean exit leaves the table and
  // the policy rules installed; the daemon sweeps them by NAME/mark/table id
  // before it serves anyone.
  //   preserveArmed=true  + the armed marker present => REPLACE the stale
  //       table with a fresh Armed ruleset in ONE atomic `nft -f` (never
  //       delete-then-add), so the block is continuous across a crash with no
  //       open window.
  //   otherwise => delete everything (the landed behaviour).
  // Returns TRUE when it left an ARMED FLOOR installed in the kernel (the
  // crash-restart path). The caller MUST hand that to NetFilter::AdoptArmedFloor
  // on the instance that will own the tunnel: the sweep is static, so without
  // adoption the owning object's state_/floor_ stay Off and every status
  // surface reports "not blocked" WHILE THE MACHINE IS BLOCKED BY THIS FLOOR —
  // the exact inversion that makes a cut-off user unable to diagnose anything.
  [[nodiscard]] static bool SweepStaleState(bool preserveArmed = false);

  // Adopt a floor this process did not install (the crash-restart sweep above),
  // so floorInstalled(), the status reply and the teardown path all agree that
  // this object owns a live ruleset.
  void AdoptArmedFloor();

  // The exact string to print in --help, in the kill-switch UI copy, and in
  // the log line emitted the moment the floor is installed:
  // "sudo nft delete table inet urnetwork".
  static const char* RecoveryCommand();

  // THE SAME WAY OUT, IN A FORM EVERY SURFACE CAN CARRY. RecoveryCommand() on
  // its own was only ever fprintf'd from inside Tunnel.cpp, which is the one
  // place a user who cannot reach the network will never look. These two are
  // free of every daemon type (plain std::string), so main.cpp can print them
  // from --help/--diagnose without an instance, ControlProtocol can put the
  // text in a reply field, and the GUI can render it verbatim instead of
  // guessing.
  //   RecoveryCommands()[0] is byte-identical to RecoveryCommand(); the rest
  //   remove the policy rule, the capture table and the armed marker, in the
  //   order a stuck machine wants them.
  static std::vector<std::string> RecoveryCommands();
  // One ready-to-print block: every route out, cheapest first (the app toggle
  // works over AF_UNIX, which no ruleset this file emits can block), then the
  // root commands. Ends without a trailing blank line.
  static std::string RecoveryHelpText();

  // Tmpfs marker that says "this machine was armed when the daemon died", so a
  // Restart=on-failure comes back armed instead of open. Deliberately under
  // /run: a crash-restart preserves the floor, a REBOOT does not (the kernel
  // ruleset is gone by then anyway, and coming up armed before any user asked
  // would be a machine that cannot reach the network at login).
  static const char* ArmedMarkerPath();

  FilterState state() const { return state_; }
  bool installed() const { return state_ != FilterState::Off; }
  // What StatusReply::kill_switch must be derived from: what is IN FORCE, not
  // what was asked for.
  bool floorInstalled() const { return state_ != FilterState::Off && floor_; }
  const std::string& lastError() const { return lastError_; }
  // Machine-readable twin of lastError(), so the UI branches on a code and
  // never on prose. "" when the last Apply/Remove succeeded.
  const char* lastErrorCode() const { return lastErrorCode_; }
  // The config of the last SUCCESSFUL Apply — what Verify() is checking
  // against and what the reaper re-applies when Verify() says the table is
  // gone. Meaningless while state() == Off.
  const FilterConfig& appliedConfig() const { return applied_; }

 private:
  bool ApplyScript(const std::string& script, const char* what, std::string* error);

  FilterState state_ = FilterState::Off;
  bool floor_ = false;
  FilterConfig applied_;
  std::string lastError_;
  const char* lastErrorCode_ = "";
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
  // PROVEN BY MEASUREMENT (Tunnel::VerifyEgressWitness: two real sockets whose
  // committed source bindings must disagree, plus a counter over the daemon's
  // real traffic that must be zero), never by a routing hypothetical. The
  // previous field meant "`ip route get` says a marked packet WOULD leave via
  // the NIC", which is true whenever the `ip rule` exists and stayed true
  // through 3.38 Tb of the daemon looping its own traffic through its own
  // tunnel. Every path that sets this to true runs the same four legs.
  bool egress_protected = false;
  // Which layer actually carried the exclusion, and the witness's numbers.
  // Published so the answer is on a surface a user can read, not only in a
  // journal line nobody was looking at.
  std::string egress_mechanism;
  std::string egress_detail;
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
  // The resolvers that ACTUALLY survived inet_pton and were handed to
  // resolved — not what the device asked for. This is what
  // FilterConfig::tunnel_resolvers must be filled from, so the pinned-DNS
  // permit and the DNS override can never name different servers.
  const std::vector<std::string>& resolvers() const { return dnsServers_; }

  // Re-push the systemd-resolved override. Called when resolved restarts
  // (it forgets per-link settings across a restart, and a DHCP lease can beat
  // our `~.` default-route domain — docs/linux_agent_help.md R5).
  bool ApplyDns();

  // THE GATE, AND THE ONLY ONE. Every surface that says "egress is protected"
  // — the bring-up refusal, the post-connect gate TunnelHost runs after the
  // Connected ruleset replaces the Connecting one, and the reaper's live
  // re-check — calls THIS function and nothing else. There is deliberately no
  // second, weaker check that some callers run and others assume: a gate only
  // one caller passes through is a gate the other callers are trusting blind,
  // which is how a 30-second re-check came to re-prove self-exclusion against a
  // capture policy it never re-checked was still installed.
  //
  // FOUR LEGS. None is a routing hypothetical, and NONE OF THEM CAN BE GREEN
  // WHILE THE CAPTURE POLICY IS ABSENT — which is the property the check that
  // shipped the storm did not have, because `ip route get` answers the same way
  // whenever an `ip rule` exists, whether or not anything is captured.
  //
  //   0. THE INSTRUMENT EXISTS AND IS AIMED AT US. One `nft list table`: the
  //      table must be in the kernel, the urnw_probe chain must be in it, that
  //      chain must name THIS interface (the ruleset is built with the PLANNED
  //      tun name, before the tun exists) and must match THIS mark. A root
  //      `nft flush ruleset` — the first line of Fedora's shipped
  //      /etc/sysconfig/nftables.conf — makes this leg fail, where before it
  //      would have left the re-check reporting green over an empty kernel.
  //   1. THE CAPTURE POLICY IS IN FORCE (the control). A socket identical to
  //      the SDK's with its mark CLEARED must connect() to a source address of
  //      the TUN. If ordinary traffic is not landing in the tunnel, "the daemon
  //      is excluded from the tunnel" is vacuously true and means nothing.
  //   2. THE DAEMON'S OWN SOCKETS ESCAPE IT (the treatment). The same call one
  //      line later, mark left as created, must NOT bind the tun's address, and
  //      must read back kEgressMark. Legs 1 and 2 differ in exactly one bit and
  //      must come out OPPOSITE; agreeing greens are a broken instrument. This
  //      is the question `ip route get` structurally cannot ask — it probes a
  //      hypothetical mark on no socket at all — and it is asked at connect(),
  //      the one moment at which the answer can still change, because
  //      ip_route_me_harder() keeps an already-chosen RTN_LOCAL source.
  //   3. NO REAL DAEMON PACKET HAS ENTERED OUR TUN. urnw_probe_tun must be
  //      exactly zero, over the SDK's actual traffic, for the whole life of the
  //      ruleset. Cumulative from an atomic swap, so no delta, no wrap, no
  //      window whose length nobody wrote down.
  //
  // A LEG THAT CANNOT RUN FAILS. Not warns, not returns true: the whole
  // incident is what "I could not measure this" looks like when it renders the
  // same as "I measured this and it is fine".
  //
  // IT SENDS NOTHING. connect() performs the route lookup and binds a source
  // address without putting a packet on the wire, so the witness needs no
  // permit anywhere in the kill-switch floor and cannot itself leak.
  //
  // `when` is a short tag for the message ("bring-up", "connected", "recheck").
  // Cheap enough for the reaper tick: one `nft` read, two sockets, no name
  // resolution, no network round trip, nothing that can block.
  bool VerifyEgressWitness(const char* when, TunnelError* err);

 private:
  Tunnel() = default;
  bool Configure(const TunnelConfig& cfg, TunnelError* err);
  bool InstallRoutes(TunnelError* err);
  bool InstallPolicyRules(TunnelError* err);
  void RemovePolicyRules();
  // VerifyCaptureTook IS GONE, FOLDED IN AS LEG 1 OF THE WITNESS. It asked "did
  // the capture half take" with `ip route get`, and — fatally — it had exactly
  // one caller: the bring-up. So the live re-check thirty seconds later
  // re-proved the daemon's self-exclusion against a capture policy nobody was
  // re-checking still existed, and could report green with the routes, the
  // rule and the table all gone. Its question is now leg 1 of
  // VerifyEgressWitness, asked of a real socket instead of a hypothetical, and
  // every caller asks it every time.

  int fd_ = -1;
  std::string name_;
  std::string localAddr_;  // the tun's own address; leg A compares against it
  std::vector<std::string> dnsServers_;
  bool rulesInstalled_ = false;
  bool dnsTouched_ = false;
  TunnelReport report_;
};

}  // namespace urnw
