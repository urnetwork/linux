#!/usr/bin/env bash
# diagnose-egress.sh -- why is urnetworkd's own traffic (not) escaping its own
# tunnel?  READ-ONLY, and it runs AS YOU ARE:
#
#     bash diagnose-egress.sh          # every check that can be done unprivileged
#     sudo bash diagnose-egress.sh     # adds the nftables ruleset and bpftool
#
# It never refuses to run for lack of root. Most of what matters here -- the
# daemon's cgroup, whether its sockets are attributed to that cgroup, what
# source address a socket opened right now is given, the routing policy, the
# journal -- is readable by any user on a normal systemd host, and a diagnostic
# that exits without printing anything is worse than one that prints eight
# answers and names the two it could not reach. Every root-only check announces
# itself as such and tells you the one command that would answer it.
#
# WHAT IT DOES NOT DO, and this is a promise you can check by reading it: it
# never runs `nft -f`, `nft add/delete/flush`, `ip route|rule|link add|del`,
# `systemctl start|stop`, `bpftool prog load|attach`, or anything else that
# changes kernel state. Every command below is a read: cat, stat, ss, `nft
# list`, `ip ... show`, `bpftool ... show`, journalctl.
#
# THE ONE THING IT OPENS. Step 7 opens a single UDP socket to a documentation
# address and asks the kernel which SOURCE address it bound to it, then closes
# it. connect() on a UDP socket sends NO packet -- it only performs the route
# lookup and the source-address selection that this whole bug is about. That is
# the honest way to ask the question, and it is why this script no longer uses
# `ip route get` for it: `ip route get` is a FIB query about a hypothetical
# packet, and the report itself used to say so one line above running it.
#
# It exists because the decisive experiment happens on a machine that already
# lost its network to this bug once.
#
# SPDX-License-Identifier: MPL-2.0

set -o pipefail

MARK_HEX="0x55524e57"
TABLE="urnetwork"
MARK_CHAIN="urnw_mark_out"
# RFC 5737 TEST-NET-1, inside the capture set. Deliberately NOT 192.0.2.1: that
# is the egress witness's own probe address, and a witness socket open at the
# same moment would be indistinguishable from ours in ss's output -- we would be
# reading the daemon's answer and calling it ours.
PROBE_ADDR="192.0.2.2"
PROBE_PORT="9"             # RFC 863 discard; nothing listens, and we send nothing
ROUTE_TABLE="51821"
TUN_NAME="urnet0"

say()   { printf '%s\n' "$*"; }
head2() { printf '\n== %s\n' "$*"; }
warn()  { printf '   !! %s\n' "$*"; }
ok()    { printf '   ok %s\n' "$*"; }
note()  { printf '   -- %s\n' "$*"; }
have()  { command -v "$1" >/dev/null 2>&1; }

# The absolute path of THIS file, so a "re-run with sudo" line names something
# that exists. Nothing installs this script -- it lives in the source tree and
# is run from wherever you put it -- so it must never print a path it guessed.
SELF="$0"
if have readlink; then SELF="$(readlink -f "$0" 2>/dev/null || printf '%s' "$0")"; fi

IS_ROOT=0
[ "$(id -u)" = 0 ] && IS_ROOT=1

# Checks that need root announce themselves through this, and say what to run.
skipped_root=0
need_root() {
  skipped_root=$((skipped_root + 1))
  note "NEEDS ROOT: $1"
  note "           re-run as:  sudo bash $SELF"
}

say "urnetworkd egress self-exclusion diagnostic -- read-only"
say "$(date -Is)  kernel $(uname -r)  $(nft --version 2>/dev/null || echo 'nft: absent')"
say "the daemon's own sockets are supposed to carry fwmark ${MARK_HEX}"
if [ "$IS_ROOT" = 1 ]; then
  say "running as root: every check below is available"
else
  say "running as uid $(id -u): the nftables and bpftool checks will be named and skipped"
fi

# Where is the daemon binary, really? On an immutable host (Bazzite) the tarball
# installer relocates it to /usr/local/lib/urnetwork/urnetworkd and rewrites the
# unit, so the packaged path is a guess. Ask systemd first, then look, and only
# print a command for a binary that is actually there.
DAEMON=""
if have systemctl; then
  DAEMON="$(systemctl show -p ExecStart --value urnetworkd 2>/dev/null \
            | sed -n 's/.*path=\([^ ;]*\).*/\1/p' | head -1)"
fi
if [ ! -x "${DAEMON:-}" ]; then
  for cand in /usr/lib/urnetwork/urnetworkd /usr/local/lib/urnetwork/urnetworkd \
              /opt/urnetwork/urnetworkd; do
    [ -x "$cand" ] && DAEMON="$cand" && break
  done
fi
[ -x "${DAEMON:-}" ] || DAEMON="$(command -v urnetworkd 2>/dev/null || true)"

# --------------------------------------------------------------------------
head2 "1. the daemon, and the cgroup the rules are supposed to name"
PID="$(systemctl show -p MainPID --value urnetworkd 2>/dev/null)"
if [ -z "$PID" ] || [ "$PID" = 0 ]; then
  PID="$(pgrep -x urnetworkd | head -1)"
fi
CGROUP=""
if [ -z "$PID" ]; then
  warn "urnetworkd is not running. Steps 3 and 5 need a LIVE daemon, ideally one"
  warn "that is CONNECTED -- that is when the sockets under test exist."
else
  CGROUP="$(sed -n 's|^0::/||p' "/proc/$PID/cgroup" 2>/dev/null)"
  if [ -z "$CGROUP" ]; then
    warn "/proc/$PID/cgroup is not readable or has no unified (0::) line."
  else
    say "   pid $PID   cgroup 0::/${CGROUP}"
    LEVEL="$(printf '%s' "$CGROUP" | awk -F/ '{print NF}')"
    say "   nft would need:  socket cgroupv2 level ${LEVEL} \"${CGROUP}\""
    if [ -d "/sys/fs/cgroup/${CGROUP}" ]; then
      ok "/sys/fs/cgroup/${CGROUP} exists (inode $(stat -c %i "/sys/fs/cgroup/${CGROUP}" 2>/dev/null))"
    else
      warn "/sys/fs/cgroup/${CGROUP} does NOT exist -- nft resolves the path to a"
      warn "cgroup id at LOAD time, so the rule would name nothing."
    fi
  fi
fi
if [ -n "${DAEMON:-}" ] && [ -x "$DAEMON" ]; then
  say "   daemon binary: $DAEMON"
else
  warn "no urnetworkd binary found (systemd ExecStart, the packaged paths, \$PATH)."
  warn "   The commands below that would use it are omitted rather than guessed."
fi

# --------------------------------------------------------------------------
head2 "2. can socket-to-cgroup association work on this host at all?"
# A cgroup v1 net_cls/net_prio hierarchy makes the kernel stop attributing
# sockets to cgroup v2, so every socket reports the root cgroup and
# `socket cgroupv2` silently matches nothing. This is THE classic killer.
if awk 'NR>1 && $2 != 0 && ($1=="net_cls" || $1=="net_prio") {found=1} END{exit !found}' /proc/cgroups; then
  warn "a cgroup v1 net_cls/net_prio hierarchy is mounted. On such a host the"
  warn "kernel disables per-socket cgroup v2 attribution and 'socket cgroupv2'"
  warn "CANNOT match. This alone would explain the failure."
else
  ok "no cgroup v1 net_cls/net_prio hierarchy (pure unified)"
fi

# The cgroup-namespace check, done so that "unreadable" cannot masquerade as
# "identical". /proc/PID/ns/* on a root-owned process is not readable by an
# ordinary user, and `readlink` fails SILENTLY -- the old version of this script
# compared two empty strings and printed "ok" to an unprivileged reader.
if [ -n "$PID" ]; then
  NS_DAEMON="$(readlink "/proc/$PID/ns/cgroup" 2>/dev/null)"
  NS_INIT="$(readlink /proc/1/ns/cgroup 2>/dev/null)"
  if [ -z "$NS_DAEMON" ] || [ -z "$NS_INIT" ]; then
    note "cannot compare cgroup namespaces (/proc/PID/ns/cgroup is not readable here)."
    note "     Step 3 answers the same question empirically: if ss reports the daemon's"
    note "     sockets under the SAME path /proc/PID/cgroup shows, the path is not"
    note "     namespace-relative."
  elif [ "$NS_DAEMON" != "$NS_INIT" ]; then
    warn "the daemon is in its OWN cgroup namespace: /proc/PID/cgroup is then"
    warn "namespace-relative while the rule counts absolute levels."
  else
    ok "same cgroup namespace as pid 1 (paths and levels are absolute)"
  fi
fi

if grep -q '^CONFIG_CGROUP_BPF=y' "/boot/config-$(uname -r)" 2>/dev/null; then
  ok "CONFIG_CGROUP_BPF=y (the socket marker can attach)"
elif have zgrep && zgrep -q '^CONFIG_CGROUP_BPF=y' /proc/config.gz 2>/dev/null; then
  ok "CONFIG_CGROUP_BPF=y (from /proc/config.gz)"
else
  note "kernel config not readable here (no /boot/config-$(uname -r), no /proc/config.gz)."
  note "     The config is a proxy anyway; step 4 names the command that MEASURES it."
fi

# --------------------------------------------------------------------------
head2 "3. ARE THE DAEMON'S SOCKETS ACTUALLY ATTRIBUTED TO THAT CGROUP?"
# THIS IS THE DECIDER, and it needs a CONNECTED daemon to be meaningful.
# ss prints INET_DIAG_CGROUP_ID, which the kernel derives from the same
# sock_cgroup_ptr(&sk->sk_cgrp_data) that `socket cgroupv2` walks -- and it
# prints it for every socket on the host, so this needs no root and no pid
# mapping (`-p` would need root, and it is not what we are asking).
if ! have ss; then
  warn "ss is not installed (iproute2); this step cannot run."
elif [ -z "$CGROUP" ]; then
  warn "no daemon cgroup known from step 1, so there is nothing to match against."
else
  SS_ALL="$(ss -tuan --cgroup state all 2>/dev/null)"
  if ! printf '%s' "$SS_ALL" | grep -q 'cgroup:'; then
    warn "this ss does not support --cgroup (iproute2 older than 5.5). Without it"
    warn "this host cannot answer the decider; everything else below still works."
  else
    OUT="$(printf '%s\n' "$SS_ALL" | grep -F "cgroup:/${CGROUP}")"
    if [ -z "$OUT" ]; then
      warn "the daemon's cgroup owns no IP socket right now."
      if [ -z "$PID" ]; then
        warn "   (the daemon is not running -- start it and CONNECT, then re-run)"
      else
        warn "   RUN THIS AGAIN WHILE CONNECTED: an idle daemon proves nothing here."
      fi
    else
      printf '%s\n' "$OUT" | sed 's/^/   /' | head -20
      ok "sockets carry the service cgroup -> 'socket cgroupv2' CAN match them."
      say "      => if the mark chain still counts nothing, the failure is the"
      say "         chain/hook, not attribution."
      # And the thing the storm actually looked like: the daemon's own sockets
      # bound to the TUNNEL's address. This is the direct evidence, not a proxy.
      TUNADDR="$(ip -4 -o addr show dev "$TUN_NAME" 2>/dev/null | awk '{print $4}' | cut -d/ -f1)"
      if [ -n "$TUNADDR" ]; then
        SELFBOUND="$(printf '%s\n' "$OUT" | grep -F "$TUNADDR" | head -5)"
        if [ -n "$SELFBOUND" ]; then
          warn "AND SOME OF THEM ARE BOUND TO THE TUNNEL'S OWN ADDRESS ($TUNADDR):"
          printf '%s\n' "$SELFBOUND" | sed 's/^/      /'
          warn "   That is the amplification loop itself: the daemon's SDK traffic went"
          warn "   INTO the tun it created. No output-hook fwmark can undo it."
        else
          ok "none of them is bound to the tunnel address ($TUNADDR)"
        fi
      fi
    fi
  fi
fi

# --------------------------------------------------------------------------
head2 "4. DO THE DAEMON'S SOCKETS CARRY THE MARK AT CREATION?"
# The mechanism this build relies on. A socket that gets the mark only at the
# output hook has already chosen the tunnel's source address and is dead.
if [ "$IS_ROOT" = 1 ] && have bpftool && [ -n "$CGROUP" ]; then
  bpftool cgroup show "/sys/fs/cgroup/${CGROUP}" 2>/dev/null | sed 's/^/   /'
  if bpftool cgroup show "/sys/fs/cgroup/${CGROUP}" 2>/dev/null | grep -qi cgroup_inet_sock_create; then
    ok "a sock_create program is attached: sockets are marked before connect()."
  else
    warn "NO sock_create program is attached to the daemon's cgroup."
    warn "   => the daemon is running on the nftables belt alone, which cannot"
    warn "      repair a source address connect() already chose."
  fi
elif [ "$IS_ROOT" = 1 ]; then
  note "bpftool is not installed, so the live attachment cannot be listed here."
else
  need_root "listing the BPF programs attached to the daemon's cgroup (bpftool cgroup show)"
fi
# Independent of whether it is attached RIGHT NOW: does the mechanism work on
# this kernel at all? That is a separate question with a one-command answer, and
# it is the one thing on this list that is a measurement rather than an
# inspection. It starts no tunnel and sends no packet.
if [ -n "${DAEMON:-}" ] && [ -x "$DAEMON" ]; then
  say "   to PROVE the mechanism on this kernel (no tunnel, no routes, no nftables):"
  say "      sudo $DAEMON --selftest-egress"
else
  note "the urnetworkd binary was not found, so the --selftest-egress command that"
  note "     would prove the mechanism on this kernel is not printed. Install the"
  note "     daemon, or run it from your build tree as: <builddir>/urnetworkd --selftest-egress"
fi
# The daemon logs the answer for its own process at every bring-up.
if have journalctl; then
  EG="$(journalctl -u urnetworkd --no-pager -n 2000 2>/dev/null | grep -F '[tunnel] egress' | tail -3)"
  if [ -n "$EG" ]; then
    say "   what the daemon last reported about its own marker:"
    printf '%s\n' "$EG" | sed 's/^/      /'
  fi
fi

# --------------------------------------------------------------------------
head2 "5. the ruleset, and what its counters actually say"
if [ "$IS_ROOT" != 1 ]; then
  need_root "reading the nftables ruleset and the urnw_out_daemon/urnw_out_total counters"
  note "           (nft needs CAP_NET_ADMIN even to LIST a table)"
elif ! have nft; then
  warn "nft is not installed: this daemon cannot filter or self-exclude at all."
elif nft list table inet "$TABLE" >/dev/null 2>&1; then
  say "   -- $MARK_CHAIN --"
  nft list chain inet "$TABLE" "$MARK_CHAIN" 2>/dev/null | sed 's/^/   /'
  say "   -- named counters --"
  nft list counters table inet "$TABLE" 2>/dev/null | sed 's/^/   /'
  DAEMON_PKTS="$(nft -j list counters table inet "$TABLE" 2>/dev/null \
            | tr ',' '\n' | grep -A2 urnw_out_daemon | grep -o '"packets":[0-9]*' \
            | head -1 | cut -d: -f2)"
  TOTAL="$(nft -j list counters table inet "$TABLE" 2>/dev/null \
            | tr ',' '\n' | grep -A2 urnw_out_total | grep -o '"packets":[0-9]*' \
            | head -1 | cut -d: -f2)"
  if [ -n "$TOTAL" ] && [ "${TOTAL:-0}" -gt 0 ]; then
    say "   ratio: ${DAEMON_PKTS:-0} of ${TOTAL} packets crossing the chain were claimed"
    say "          by the cgroup match."
    if [ "${DAEMON_PKTS:-0}" = 0 ]; then
      warn "ZERO of ${TOTAL}. The cgroup match is not matching this daemon."
    fi
  else
    say "   (no denominator yet -- an older build had no urnw_out_total at all,"
    say "    which is why 'egress mark applied to 2 packet(s)' meant nothing:"
    say "    a numerator with nothing to compare it against.)"
  fi
else
  say "   table inet $TABLE is not installed (no tunnel up, or it was flushed)."
fi

# --------------------------------------------------------------------------
head2 "6. routing policy and the capture table (state, not hypotheticals)"
ip -4 rule show 2>/dev/null | sed 's/^/   /'
say "   -- capture table $ROUTE_TABLE (first 3 of the 31 prefixes) --"
CAPTURE="$(ip -4 route show table "$ROUTE_TABLE" 2>/dev/null)"
if [ -n "$CAPTURE" ]; then
  printf '%s\n' "$CAPTURE" | head -3 | sed 's/^/   /'
  ok "the capture table is installed ($(printf '%s\n' "$CAPTURE" | wc -l) routes)"
else
  say "   (empty -- no tunnel is up)"
fi
note "deliberately NO 'ip route get' here. It answers a question about a packet"
note "     nobody sent, from a shell that is not the daemon; step 7 asks the kernel"
note "     the real one instead."

# --------------------------------------------------------------------------
head2 "7. WHAT SOURCE ADDRESS DOES A SOCKET OPENED RIGHT NOW ACTUALLY GET?"
# This is the leg that names the architectural defect, and it has to be asked
# with a real socket. ip_route_me_harder(), which the `type route` chain
# triggers when a mark is set at NF_INET_LOCAL_OUT, RE-RUNS the route lookup but
# keeps an already-chosen RTN_LOCAL source address (FLOWI_FLAG_ANYSRC). So a
# socket that resolved the tun first leaves the physical NIC sourced from the
# TUNNEL's address, and is never answered. The only way to see that is to look
# at a socket, so: open one, ask the kernel what it bound, close it. No packet
# is sent -- connect() on UDP is a route lookup and a source selection.
TUNADDR="$(ip -4 -o addr show dev "$TUN_NAME" 2>/dev/null | awk '{print $4}' | cut -d/ -f1)"
say "   tun $TUN_NAME address:        ${TUNADDR:-<no tun device>}"
SRC=""
if [ -z "${BASH_VERSION:-}" ]; then
  warn "not running under bash, so the /dev/udp probe is unavailable."
elif ! have ss; then
  warn "ss is not installed, so the probe socket's source cannot be read back."
else
  if exec 3<>"/dev/udp/${PROBE_ADDR}/${PROBE_PORT}" 2>/dev/null; then
    SRC="$(ss -uan state all "dst ${PROBE_ADDR}:${PROBE_PORT}" 2>/dev/null \
           | awk 'NR>1 {print $4}' | head -1 | sed 's/:[0-9]*$//')"
    exec 3<&-
  else
    warn "could not open a UDP socket to ${PROBE_ADDR}:${PROBE_PORT} (connect failed)."
    warn "   With the kill switch armed and no route to the capture set, that is"
    warn "   itself an answer: this host currently has no path for that address."
  fi
fi
say "   source a socket opened NOW is given: ${SRC:-<not measured>}"
if [ -n "$TUNADDR" ] && [ -n "$SRC" ] && [ "$SRC" = "$TUNADDR" ]; then
  warn "A SOCKET OPENED NOW IS BOUND TO THE TUNNEL'S OWN ADDRESS."
  warn "   No output-hook fwmark can repair this: the mark re-routes the packet"
  warn "   out the physical NIC but keeps that source, so nothing can reply."
  warn "   The mark has to be on the socket BEFORE connect() -- see step 4."
elif [ -n "$TUNADDR" ] && [ -n "$SRC" ]; then
  ok "a socket opened now is NOT bound to the tunnel address"
fi
note "SCOPE, stated plainly: this socket was opened by THIS SHELL, which is not in"
note "     the daemon's cgroup, so neither the cgroup-BPF marker nor the nftables"
note "     cgroup rule applies to it. It tells you what the ROUTING POLICY does to an"
note "     unmarked socket -- which is exactly the situation the daemon's own sockets"
note "     were in before the marker existed. What a socket created INSIDE the daemon's"
note "     cgroup gets is step 3 (its live sockets) and \`urnetworkd --selftest-egress\`"
note "     (the mechanism itself). No script can ask it from out here."

# --------------------------------------------------------------------------
head2 "8. what the journal said last time, and how long the window really was"
say "   LOOK AT THE TIMESTAMPS, not the packet count. The old build created the"
say "   mark chain at '[filter] connecting' and read its counter at 'egress mark"
say "   applied', and on 2026-08-15 those lines -- and '[tun] up' -- were all in"
say "   the SAME SECOND. '2 packet(s)' was a sample of a sub-second window, not"
say "   of the forty minutes that followed; and the Connected apply then"
say "   destroyed that table and reset the counter, so the ruleset the tunnel"
say "   actually ran under was never measured at all."
if have journalctl; then
  JOUT="$(journalctl -u urnetworkd --no-pager -n 4000 2>/dev/null \
          | grep -E "egress|\[filter\] connecting|tunnel up|runaway|witness" | tail -25)"
  if [ -n "$JOUT" ]; then
    printf '%s\n' "$JOUT" | sed 's/^/   /'
  else
    note "no matching journal lines (or this user cannot read the unit's journal;"
    note "     on most systemd hosts that needs the systemd-journal or wheel group)."
  fi
else
  note "journalctl is not available here."
fi

# --------------------------------------------------------------------------
head2 "verdict"
say "Read step 3 first, then step 7, then step 4."
say "  * step 3 says the daemon's sockets do NOT report the service cgroup"
say "        -> the nftables cgroup match never had a chance; attribution is the bug."
say "  * step 3 shows its sockets bound to the TUN address"
say "        -> that is the amplification loop, in the kernel's own words."
say "  * step 3 fine but step 5 shows 0 of N claimed"
say "        -> the chain or hook is the bug, not the path spelling."
say "  * step 7 says a socket opened now is given the tunnel's own address"
say "        -> a mark applied at the output hook arrives too late no matter what"
say "           else is true, and the fix is step 4 (mark at socket creation)."
say "  * step 4 says no sock_create program is attached"
say "        -> the daemon is running on the nftables belt alone. Prove the"
say "           mechanism first (--selftest-egress), then look at why it is absent."
if [ "$skipped_root" -gt 0 ]; then
  say ""
  say "$skipped_root check(s) above needed root and were skipped by name, not silently."
  say "For the full picture:  sudo bash $SELF"
fi
say ""
say "Nothing on this machine was changed by running this. At most one UDP socket"
say "was opened (step 7) and closed again; no packet was sent."
