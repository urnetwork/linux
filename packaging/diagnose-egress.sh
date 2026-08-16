#!/usr/bin/env bash
# diagnose-egress.sh -- why is urnetworkd's own traffic (not) escaping its own
# tunnel?  READ-ONLY.  Run it as root:
#
#     sudo bash diagnose-egress.sh
#
# WHAT IT DOES NOT DO, and this is a promise you can check by reading it: it
# never runs `nft -f`, `nft add`, `nft delete`, `nft flush`, `ip route add`,
# `ip rule add`, `ip link`, `systemctl`, or anything else that changes kernel
# state.  Every command below is a read: cat, stat, ss, `nft list`, `ip ...
# show|get`, journalctl.  `ip route get` is a routing QUESTION, not a change.
# No tunnel is required and none is started.  If this script ever grows a line
# that mutates something, that is a bug in the script.
#
# It exists because the decisive experiment needs root and the machine it needs
# to run on already lost its network to this bug once.
#
# SPDX-License-Identifier: MPL-2.0

set -o pipefail

MARK_HEX="0x55524e57"
TABLE="urnetwork"
MARK_CHAIN="urnw_mark_out"
PROBE_ADDR="192.0.2.1"     # RFC 5737 TEST-NET-1, inside the capture set
ROUTE_TABLE="51821"
RULE_PREF="32763"

say()  { printf '%s\n' "$*"; }
head2() { printf '\n== %s\n' "$*"; }
warn() { printf '   !! %s\n' "$*"; }
ok()   { printf '   ok %s\n' "$*"; }

if [ "$(id -u)" != 0 ]; then
  say "This needs root to read socket cgroup ids and the nftables ruleset."
  say "Re-run:  sudo bash $0"
  exit 2
fi

say "urnetworkd egress self-exclusion diagnostic -- read-only"
say "$(date -Is)  kernel $(uname -r)  $(nft --version 2>/dev/null || echo 'nft: absent')"

# --------------------------------------------------------------------------
head2 "1. the daemon, and the cgroup the rules are supposed to name"
PID="$(systemctl show -p MainPID --value urnetworkd 2>/dev/null)"
if [ -z "$PID" ] || [ "$PID" = 0 ]; then
  PID="$(pgrep -x urnetworkd | head -1)"
fi
if [ -z "$PID" ]; then
  warn "urnetworkd is not running. Steps 3 and 4 need a LIVE daemon, ideally"
  warn "one that is CONNECTED -- that is when the sockets under test exist."
  CGROUP=""
else
  CGROUP="$(sed -n 's|^0::/||p' "/proc/$PID/cgroup" 2>/dev/null)"
  say "   pid $PID   cgroup 0::/${CGROUP}"
  LEVEL="$(printf '%s' "$CGROUP" | awk -F/ '{print NF}')"
  say "   nft would need:  socket cgroupv2 level ${LEVEL} \"${CGROUP}\""
  if [ -d "/sys/fs/cgroup/${CGROUP}" ]; then
    ok "/sys/fs/cgroup/${CGROUP} exists (inode $(stat -c %i "/sys/fs/cgroup/${CGROUP}"))"
  else
    warn "/sys/fs/cgroup/${CGROUP} does NOT exist -- nft resolves the path to a"
    warn "cgroup id at LOAD time, so the rule would name nothing."
  fi
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
if [ -n "$PID" ] && [ "$(readlink "/proc/$PID/ns/cgroup" 2>/dev/null)" != "$(readlink /proc/1/ns/cgroup 2>/dev/null)" ]; then
  warn "the daemon is in its OWN cgroup namespace: /proc/PID/cgroup is then"
  warn "namespace-relative while the rule counts absolute levels."
else
  ok "same cgroup namespace as pid 1 (paths and levels are absolute)"
fi
grep -q '^CONFIG_CGROUP_BPF=y' "/boot/config-$(uname -r)" 2>/dev/null \
  && ok "CONFIG_CGROUP_BPF=y (the socket marker can attach)" \
  || say "   ?  CONFIG_CGROUP_BPF not readable here; bpftool below is the real answer"

# --------------------------------------------------------------------------
head2 "3. ARE THE DAEMON'S SOCKETS ACTUALLY ATTRIBUTED TO THAT CGROUP?"
# THIS IS THE DECIDER, and it needs a CONNECTED daemon to be meaningful.
# ss prints INET_DIAG_CGROUP_ID, which the kernel derives from the same
# sock_cgroup_ptr(&sk->sk_cgrp_data) that `socket cgroupv2` walks.
if [ -n "$PID" ]; then
  OUT="$(ss -tuanp --cgroup 2>/dev/null | grep -F "pid=$PID" || true)"
  if [ -z "$OUT" ]; then
    warn "the daemon has no IP sockets right now. RUN THIS AGAIN WHILE CONNECTED:"
    warn "an idle daemon proves nothing here."
  else
    printf '%s\n' "$OUT" | sed 's/^/   /' | head -20
    if printf '%s' "$OUT" | grep -q "cgroup:/${CGROUP}"; then
      ok "sockets carry the service cgroup -> 'socket cgroupv2' CAN match them."
      say "      => if the mark chain still counts nothing, the failure is the"
      say "         chain/hook, not attribution."
    else
      warn "sockets do NOT report cgroup:/${CGROUP}."
      warn "   => ATTRIBUTION IS THE BUG. nft_sock_get_eval_cgroupv2() walks"
      warn "      cgroup_ancestor(sock_cgroup_ptr(&sk->sk_cgrp_data), level) and"
      warn "      gets NULL or the wrong id, so the rule matches nothing and"
      warn "      fails OPEN -- exactly what shipped."
    fi
  fi
fi

# --------------------------------------------------------------------------
head2 "4. DO THE DAEMON'S SOCKETS CARRY THE MARK AT CREATION?"
# The mechanism this build relies on. A socket that gets the mark only at the
# output hook has already chosen the tunnel's source address and is dead.
if command -v bpftool >/dev/null 2>&1 && [ -n "$CGROUP" ]; then
  bpftool cgroup show "/sys/fs/cgroup/${CGROUP}" 2>/dev/null | sed 's/^/   /'
  if bpftool cgroup show "/sys/fs/cgroup/${CGROUP}" 2>/dev/null | grep -qi cgroup_inet_sock_create; then
    ok "a sock_create program is attached: sockets are marked before connect()."
  else
    warn "NO sock_create program is attached to the daemon's cgroup."
    warn "   => the daemon is running on the nftables belt alone, which cannot"
    warn "      repair a source address connect() already chose."
  fi
else
  say "   (bpftool not installed -- skipping; install bpftool for this answer)"
fi

# --------------------------------------------------------------------------
head2 "5. the ruleset, and what its counters actually say"
if nft list table inet "$TABLE" >/dev/null 2>&1; then
  say "   -- $MARK_CHAIN --"
  nft list chain inet "$TABLE" "$MARK_CHAIN" 2>/dev/null | sed 's/^/   /'
  say "   -- named counters --"
  nft list counters table inet "$TABLE" 2>/dev/null | sed 's/^/   /'
  DAEMON="$(nft -j list counters table inet "$TABLE" 2>/dev/null \
            | tr ',' '\n' | grep -A2 urnw_out_daemon | grep -o '"packets":[0-9]*' \
            | head -1 | cut -d: -f2)"
  TOTAL="$(nft -j list counters table inet "$TABLE" 2>/dev/null \
            | tr ',' '\n' | grep -A2 urnw_out_total | grep -o '"packets":[0-9]*' \
            | head -1 | cut -d: -f2)"
  if [ -n "$TOTAL" ] && [ "${TOTAL:-0}" -gt 0 ]; then
    say "   ratio: ${DAEMON:-0} of ${TOTAL} packets crossing the chain were claimed"
    say "          by the cgroup match."
    if [ "${DAEMON:-0}" = 0 ]; then
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
head2 "6. routing policy, and what it does to a socket we open right now"
ip -4 rule show | sed 's/^/   /'
say "   -- capture table $ROUTE_TABLE (first 3 of the 31 prefixes) --"
ip -4 route show table "$ROUTE_TABLE" 2>/dev/null | head -3 | sed 's/^/   /'
say "   -- where an UNMARKED packet for $PROBE_ADDR goes --"
ip -4 route get "$PROBE_ADDR" 2>&1 | sed 's/^/   /'
say "   -- where a MARKED one goes (this is the check that lied; it is only"
say "      ever a hypothetical about a mark on no socket) --"
ip -4 route get "$PROBE_ADDR" mark "$MARK_HEX" 2>&1 | sed 's/^/   /'

# --------------------------------------------------------------------------
head2 "7. the question ip route get cannot ask: what SOURCE ADDRESS would a"
say   "   socket opened right now be given?"
# This is the leg that names the architectural defect. ip_route_me_harder(),
# which the `type route` chain triggers, RE-RUNS THE ROUTE LOOKUP but keeps an
# already-chosen RTN_LOCAL source address (it sets FLOWI_FLAG_ANYSRC and
# reuses it). So a socket that resolved the tun first leaves the physical NIC
# sourced from the TUNNEL's address and is never answered.
TUNDEV="$(ip -4 -o addr show 2>/dev/null | awk '/urnet/{print $2; exit}')"
TUNADDR="$(ip -4 -o addr show dev "${TUNDEV:-urnet0}" 2>/dev/null | awk '{print $4}' | cut -d/ -f1)"
SRC="$(ip -4 route get "$PROBE_ADDR" 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')"
say "   tun address:            ${TUNADDR:-<no tun>}"
say "   source a new socket gets: ${SRC:-<none>}"
if [ -n "$TUNADDR" ] && [ "$SRC" = "$TUNADDR" ]; then
  warn "A SOCKET OPENED NOW WOULD BE BOUND TO THE TUNNEL'S OWN ADDRESS."
  warn "   No output-hook fwmark can repair this: the mark re-routes the packet"
  warn "   out the physical NIC but keeps that source, so nothing can reply."
  warn "   The mark has to be on the socket BEFORE connect() -- see step 4."
elif [ -n "$TUNADDR" ]; then
  ok "a new socket is NOT bound to the tunnel address"
fi

# --------------------------------------------------------------------------
head2 "8. what the journal said last time, and how long the window really was"
say "   LOOK AT THE TIMESTAMPS, not the packet count. The old build created the"
say "   mark chain at '[filter] connecting' and read its counter at 'egress mark"
say "   applied', and on 2026-08-15 those lines -- and '[tun] up' -- were all in"
say "   the SAME SECOND. '2 packet(s)' was a sample of a sub-second window, not"
say "   of the forty minutes that followed; and the Connected apply then"
say "   destroyed that table and reset the counter, so the ruleset the tunnel"
say "   actually ran under was never measured at all."
journalctl -u urnetworkd --no-pager -n 4000 2>/dev/null \
  | grep -E "egress|\[filter\] connecting|tunnel up|runaway|witness" \
  | tail -25 | sed 's/^/   /'

head2 "verdict"
say "Read step 3 first, then step 7."
say "  * step 3 says 'sockets do NOT report the service cgroup'  -> the nftables"
say "    cgroup match never had a chance; attribution is the bug."
say "  * step 3 fine but step 5 shows 0 of N claimed              -> the chain or"
say "    hook is the bug, not the path spelling."
say "  * step 7 says the source is the tunnel's own address       -> the mark is"
say "    arriving too late no matter which of the above is true, and the fix is"
say "    step 4 (mark at socket creation), not a different nft rule."
say ""
say "Nothing on this machine was changed by running this."
