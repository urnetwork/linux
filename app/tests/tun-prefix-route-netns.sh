#!/bin/sh
# The tun's own subnet and the daemon's marked sockets, on the real kernel.
#
# A tun address added the ordinary way puts its prefix route in MAIN, the table
# `ip rule add not fwmark 0x55524e57 table 51821` leaves the daemon's marked
# sockets to, so a marked packet for that subnet leaves through the tun (the
# egress witness's urnw_probe_tun counter; on a live box, an egress_unprotected
# stop about a minute after every connect). With noprefixroute it follows the
# host's own route instead, and `ip address replace` on an existing address
# keeps its old flags, which is why Tunnel::Configure flushes first.
#
# Runs in a throwaway network namespace, so nothing on the host is touched.
# Needs root, unshare, iproute2 and /dev/net/tun; skips (77) without them.
# SPDX-License-Identifier: MPL-2.0
set -u

[ "$(id -u)" = 0 ] || { echo "skip: needs root"; exit 77; }
command -v unshare >/dev/null 2>&1 || { echo "skip: no unshare"; exit 77; }
command -v ip >/dev/null 2>&1 || { echo "skip: no iproute2"; exit 77; }
[ -c /dev/net/tun ] || { echo "skip: no /dev/net/tun"; exit 77; }
unshare -n true 2>/dev/null || { echo "skip: cannot create a network namespace"; exit 77; }

if [ "${1:-}" != "--inside" ]; then
  exec unshare -n "$0" --inside
fi

MARK=0x55524e57
fail=0
check() {  # <what> <expected dev> <route get args...>
  what=$1; want=$2; shift 2
  got=$(ip -4 route get "$@" 2>&1 | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -n 1)
  if [ "$got" = "$want" ]; then
    echo "ok: $what -> $got"
  else
    echo "FAIL: $what -> '$got', want $want ($(ip -4 route get "$@" 2>&1 | head -n 1))"
    fail=1
  fi
}

ip link set lo up
# The uplink, and a service range reached through its gateway (a VPC, a
# sandbox's egress tunnel): nothing on this host holds an address in it, which
# is exactly what lets the SDK hand the tun 10.0.0.0/24.
ip tuntap add dev up0 mode tap || { echo "skip: cannot create a tap"; exit 77; }
ip address add 192.168.50.2/24 dev up0
ip link set up0 up
ip route add default via 192.168.50.1 dev up0 onlink
ip route add 10.0.0.0/8 via 192.168.50.1 dev up0 onlink
ip tuntap add dev urnet0 mode tun || { echo "skip: cannot create a tun"; exit 77; }
ip rule add not fwmark $MARK table 51821 pref 32763 || { echo "skip: no policy routing"; exit 77; }
ip route add 0.0.0.0/1 dev urnet0 table 51821 2>/dev/null || true

# 1. the defect: an ordinary address puts 10.0.0.0/24 in main via the tun.
ip -4 address replace 10.0.0.89/24 dev urnet0
ip link set urnet0 up
ip route replace 0.0.0.0/1 dev urnet0 table 51821
check "plain address, marked socket to 10.0.0.5 (the defect)" urnet0 10.0.0.5 mark $MARK

# 2. replace in place keeps the flags: the route comes back with the link.
ip -4 address replace 10.0.0.89/24 dev urnet0 noprefixroute
ip -4 route flush table main dev urnet0
ip link set urnet0 down; ip link set urnet0 up
ip route replace 0.0.0.0/1 dev urnet0 table 51821
# Information, not a pass condition: a kernel that honours the new flag on
# replace would be fine too, and Configure flushes first either way.
info=$(ip -4 route get 10.0.0.5 mark $MARK 2>&1 | head -n 1)
echo "info: replace in place, after a link bounce: $info"

# 3. the fix: flush, then add with noprefixroute (Tunnel::Configure's steps).
ip link set urnet0 down
ip -4 address flush dev urnet0
ip -4 address replace 10.0.0.89/24 dev urnet0 noprefixroute
ip link set urnet0 up
ip link set urnet0 down; ip link set urnet0 up
ip route replace 0.0.0.0/1 dev urnet0 table 51821
check "noprefixroute, marked socket to 10.0.0.5" up0 10.0.0.5 mark $MARK
check "noprefixroute, marked socket to 1.1.1.1" up0 1.1.1.1 mark $MARK
check "noprefixroute, unmarked socket to 1.1.1.1 (still captured)" urnet0 1.1.1.1
src=$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([^ ]*\).*/\1/p')
if [ "$src" = 10.0.0.89 ]; then
  echo "ok: unmarked socket still sources the tun's own address"
else
  echo "FAIL: unmarked socket sourced '$src', want 10.0.0.89"; fail=1
fi
if [ -n "$(ip -4 route show table main dev urnet0)" ]; then
  echo "FAIL: main still routes through the tun: $(ip -4 route show table main dev urnet0)"
  fail=1
fi
exit $fail
