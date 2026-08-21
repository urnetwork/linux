#!/usr/bin/env bash
# distro-smoke.sh -- "does URnetwork work on THIS Linux?", answered in one run
# on a machine you have never tested before.
#
#     bash distro-smoke.sh                 # the survey. READ-ONLY. Runs as you are.
#     sudo bash distro-smoke.sh --privileged   # adds the root-only checks and the
#                                              # daemon's own --selftest-egress
#     sudo bash distro-smoke.sh --tunnel       # + verify a LIVE tunnel, leg by leg
#     bash distro-smoke.sh --json          # same run, plus a machine-readable tail
#
# WHY THIS EXISTS. URnetwork works on the machine it was written on. Everything
# that broke it there broke it in a way that had nothing to do with URnetwork's
# code: SELinux put the daemon in init_t and forbade /dev/net/tun; a systemd
# hardening knob forbade the domain transition that `nft` needs; /usr was
# read-only so the installer had to move house. Every one of those is a
# PROPERTY OF THE DISTRIBUTION, and the next distribution has its own. This
# script asks the host all of those questions at once, so that porting to a new
# distro starts with a list instead of a bisect.
#
# WHAT IT DOES NOT DO, and you can check this by reading it. In the default and
# --privileged modes it NEVER changes kernel or system state and it sends NOT
# ONE PACKET on any wire. There is no `nft add|delete|flush|-f`, no `ip
# route|rule|link add|del`, no `systemctl start|stop|enable`, no `modprobe`, no
# `semodule -i`, no package installation, no file written outside $TMPDIR. Every
# command below is a read: cat, stat, readlink, getent, pgrep, ldconfig -p,
# `nft --version`, `ip ... show`, `modinfo`, `rpm -q`, `flatpak list`.
#
# The ONE thing --privileged runs that is not a pure read is the daemon's own
# `urnetworkd --selftest-egress`, and its blast radius is documented in
# src/daemon/main.cpp: it creates one temporary cgroup, runs a child in it, and
# removes it again. No tun device, no routes, no nftables, no DNS, no packets.
# It is also the ONLY check here that can PROVE the egress marker works on this
# kernel -- everything else in this file is a preflight, and a preflight that
# says "should work" has never once been the same thing as a measurement.
#
# WHAT --tunnel IS, HONESTLY. It does not press Connect for you, because it
# cannot: `start_tunnel` on the control socket requires the device pairing id
# and the mTLS triple that the GUI generates (ControlServer.cpp,
# ValidateStartTunnelRequest), and a smoke test that forged those would be
# lying about the exact thing you most need measured. So --tunnel waits for YOU
# to connect in the app and then verifies the result leg by leg -- the tun
# device, the policy rule, the capture table, the ruleset, the DNS pin, the
# daemon's own sockets. It never brings a tunnel up and it never tears one down.
#
# EXIT STATUS. 0 = nothing measured here blocks URnetwork on this host (which is
# not the same as "it works" -- see the verdict). 1 = at least one genuine
# blocker. 2 = this script could not run at all. Checks that could not be
# measured (no root, tool absent) are named, never silently dropped, and they do
# NOT set a non-zero status: "I could not tell" is not "it is broken".
#
# SPDX-License-Identifier: MPL-2.0

# No `set -e`: nearly every check here is allowed to fail, and a survey that
# exits at the first missing tool is a survey that reports nothing.
#
# AND NO `set -o pipefail`, WHICH IS NOT AN OVERSIGHT. It was set here, and it
# made this script report a FALSE BLOCKER on the one machine known to work:
# `printf '%s\n' "$config" | grep -q '^CONFIG_CGROUP_BPF=y'` returns 141, not 0,
# because grep -q exits the instant it matches, printf then dies of SIGPIPE, and
# pipefail reports the pipeline as the failure of its noisiest member. Every
# `| grep -q` in a survey script is that shape, so a successful match reads as
# "missing". The same bug was sitting in the SELinux policy-module check and in
# every leg of --tunnel. Caught by running it; it would never have shown up in a
# reading.
set +o pipefail

# --------------------------------------------------------------------------
# Facts about the product, kept in one place. These are contracts, not taste:
# the daemon, the unit and the packaging all agree on them and the checks below
# are only meaningful because they use the same values.
# --------------------------------------------------------------------------
GLIBC_FLOOR='2.35'          # SDK is cross-built against jammy (nfpm.yaml, install.sh)
SYSTEMD_FLOOR='235'         # RuntimeDirectoryPreserve=/StateDirectory= landed in 235
KERNEL_FLOOR='5.4'          # practical floor for cgroup-BPF + `socket cgroupv2`
NFT_FLOOR='0.9.1'           # nft `socket cgroupv2 level N "path"` match
TUN_NAME='urnet0'
NFT_TABLE='urnetwork'       # `table inet urnetwork`
ROUTE_TABLE='51821'
MARK_HEX='0x55524e57'
CONTROL_SOCKET='/run/urnetwork/control.sock'
CONTROL_GROUP='urnetwork'   # ControlProtocol.hpp kControlGroupName
UNIT='urnetworkd.service'
TUN_DEVICE_LABEL='tun_tap_device_t'   # what packaging/selinux/urnetwork.te requires

# Where the daemon can legitimately live: the packaged path, the immutable-host
# remap the tarball installer performs, and the /opt fallback.
DAEMON_PATHS=(
    /usr/lib/urnetwork/urnetworkd
    /usr/local/lib/urnetwork/urnetworkd
    /opt/urnetwork/urnetworkd
)

# --------------------------------------------------------------------------
# Output plumbing
# --------------------------------------------------------------------------
DOT_WIDTH=32
have() { command -v "$1" >/dev/null 2>&1; }
say()  { printf '%s\n' "$*"; }
head2() { printf '\n== %s\n' "$*"; }
note() { printf '        %s\n' "$*"; }

# Is a process with this name running? `pgrep -x systemd-resolved` NEVER matches:
# /proc/<pid>/comm is capped at 15 characters, so the kernel's name for it is
# "systemd-resolve" and pgrep refuses patterns longer than that outright. This
# script asks about systemd-resolved by name, so without the truncation it would
# report "resolved is not running" on every systemd host in existence.
proc_running() {
    local name="$1"
    have pgrep || return 1
    pgrep -x "${name:0:15}" >/dev/null 2>&1 && return 0
    pgrep -f "/${name}\$" >/dev/null 2>&1 && return 0
    return 1
}

dotted() {
    local out="$1 "
    while [ ${#out} -lt "${DOT_WIDTH}" ]; do out="${out}."; done
    printf '%s' "${out}"
}

N_OK=0; N_WARN=0; N_BLOCK=0; N_NA=0; N_ROOT=0
BLOCKERS=(); WARNINGS=(); ROOTSKIPS=(); JSON_ROWS=()

# check <id> <verdict> <label> <detail...>
#   ok         this host satisfies it
#   WARN       degraded, but URnetwork still functions -- with a named cost
#   BLOCKER    URnetwork cannot work here until this changes
#   n/a        not measurable from here (tool absent, or not applicable)
#   needs-root the answer exists but only root can read it
#   --         informational, no verdict
check() {
    local id="$1" verdict="$2" label="$3"; shift 3
    local detail="$*"
    printf '  %-5s %s %-10s %s\n' "${id}" "$(dotted "${label}")" "${verdict}" "${detail}"
    case "${verdict}" in
        ok)         N_OK=$((N_OK + 1)) ;;
        WARN)       N_WARN=$((N_WARN + 1));   WARNINGS+=("${id}  ${label}: ${detail}") ;;
        BLOCKER)    N_BLOCK=$((N_BLOCK + 1)); BLOCKERS+=("${id}  ${label}: ${detail}") ;;
        n/a)        N_NA=$((N_NA + 1)) ;;
        needs-root) N_ROOT=$((N_ROOT + 1));   ROOTSKIPS+=("${id}  ${label}: ${detail}") ;;
    esac
    JSON_ROWS+=("$(json_row "${id}" "${verdict}" "${label}" "${detail}")")
}

json_escape() {
    local s="$1"
    s="${s//\\/\\\\}"; s="${s//\"/\\\"}"
    s="${s//$'\t'/ }"; s="${s//$'\n'/ }"
    printf '%s' "${s}"
}
json_row() {
    printf '{"id":"%s","verdict":"%s","check":"%s","detail":"%s"}' \
        "$(json_escape "$1")" "$(json_escape "$2")" \
        "$(json_escape "$3")" "$(json_escape "$4")"
}

# ver_ge A B -- true when version A >= version B. Takes the FIRST dotted numeric
# run out of each string, so it copes with "nftables v1.1.6 (Commodore
# Bullmoose #7)", "systemd 259 (259.8-1.fc44)" and "7.1.5-ogc5.1.fc44.x86_64"
# without a per-caller regex.
ver_ge() {
    awk -v a="$1" -v b="$2" '
    function num(s) {
        if (match(s, /[0-9]+(\.[0-9]+)*/)) return substr(s, RSTART, RLENGTH);
        return "";
    }
    BEGIN {
        na = split(num(a), A, "."); nb = split(num(b), B, ".");
        if (na == 0) exit 1;
        n = (na > nb ? na : nb);
        for (i = 1; i <= n; i++) {
            x = (i <= na ? A[i] + 0 : 0); y = (i <= nb ? B[i] + 0 : 0);
            if (x > y) exit 0;
            if (x < y) exit 1;
        }
        exit 0;
    }'
}

# --------------------------------------------------------------------------
# Arguments
# --------------------------------------------------------------------------
PRIVILEGED=0
TUNNEL=0
JSON=0
TUNNEL_WAIT=120

usage() {
    sed -n '2,49p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//;s/^#$//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --privileged) PRIVILEGED=1; shift ;;
        --tunnel)     TUNNEL=1; PRIVILEGED=1; shift ;;
        --wait)       TUNNEL_WAIT="${2:?--wait needs seconds}"; shift 2 ;;
        --json)       JSON=1; shift ;;
        -h|--help)    usage 0 ;;
        *) printf 'distro-smoke.sh: unknown argument %s (try --help)\n' "$1" >&2; exit 2 ;;
    esac
done

if [ -z "${BASH_VERSION:-}" ]; then
    printf 'distro-smoke.sh needs bash (run it as: bash distro-smoke.sh)\n' >&2
    exit 2
fi

SELF="${BASH_SOURCE[0]}"
have readlink && SELF="$(readlink -f "${BASH_SOURCE[0]}" 2>/dev/null || printf '%s' "${BASH_SOURCE[0]}")"

IS_ROOT=0
[ "$(id -u)" = 0 ] && IS_ROOT=1

# --------------------------------------------------------------------------
# Header
# --------------------------------------------------------------------------
OS_ID=''; OS_VER=''; OS_LIKE=''; OS_PRETTY=''; OS_VARIANT=''
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release 2>/dev/null
    OS_ID="${ID:-}"; OS_VER="${VERSION_ID:-}"; OS_LIKE="${ID_LIKE:-}"
    OS_PRETTY="${PRETTY_NAME:-${NAME:-}}"; OS_VARIANT="${VARIANT_ID:-}"
fi
KREL="$(uname -r 2>/dev/null)"
KARCH="$(uname -m 2>/dev/null)"

say "urnetwork distro smoke test -- does URnetwork work on THIS Linux?"
say "$(date -Is 2>/dev/null || date)  ${OS_PRETTY:-unknown OS}  kernel ${KREL}  ${KARCH}"
if [ "${TUNNEL}" = 1 ]; then
    say "mode: --tunnel (privileged read-only checks + verification of a live tunnel)"
elif [ "${PRIVILEGED}" = 1 ]; then
    say "mode: --privileged (adds the root-only reads and urnetworkd --selftest-egress)"
else
    say "mode: read-only survey. Nothing is changed and no packet is sent."
    say "      privileged checks are NAMED below and skipped; --privileged runs them."
fi
if [ "${IS_ROOT}" = 1 ]; then
    say "running as root"
else
    say "running as uid $(id -u) -- root-only answers are reported as 'needs-root', not guessed"
fi

# ==========================================================================
head2 "1. which machine is this"
# ==========================================================================

FAMILY='unknown'
case " ${OS_ID} ${OS_LIKE} " in
    *' debian '*|*' ubuntu '*)               FAMILY='debian' ;;
    *' fedora '*|*' rhel '*|*' centos '*)    FAMILY='fedora' ;;
    *' suse '*|*' opensuse '*|*' sles '*)    FAMILY='suse' ;;
    *' arch '*|*' archlinux '*)              FAMILY='arch' ;;
    *' alpine '*)                            FAMILY='alpine' ;;
    *' gentoo '*)                            FAMILY='gentoo' ;;
esac
if [ "${FAMILY}" = 'unknown' ] && [ -n "${OS_ID}" ]; then
    case "${OS_ID}" in
        debian|ubuntu) FAMILY='debian' ;;
        fedora)        FAMILY='fedora' ;;
        arch)          FAMILY='arch' ;;
        alpine)        FAMILY='alpine' ;;
    esac
fi
if [ -n "${OS_ID}" ]; then
    check 1.1 ok "distro identity" \
        "${OS_ID} ${OS_VER}${OS_VARIANT:+ (${OS_VARIANT})} -- ${FAMILY} family"
else
    check 1.1 WARN "distro identity" \
        "/etc/os-release is unreadable; every family-specific hint below is off"
fi

case "${KARCH}" in
    x86_64|amd64) UR_ARCH='amd64'; check 1.2 ok "cpu architecture" "${KARCH} -> the amd64 assets" ;;
    aarch64|arm64) UR_ARCH='arm64'; check 1.2 ok "cpu architecture" "${KARCH} -> the arm64 assets" ;;
    *) UR_ARCH=''
       check 1.2 BLOCKER "cpu architecture" \
           "${KARCH}: no URnetwork asset is built for it (amd64 and arm64 only)" ;;
esac

if ver_ge "${KREL}" "${KERNEL_FLOOR}"; then
    check 1.3 ok "kernel version" "${KREL} (floor ${KERNEL_FLOOR})"
else
    check 1.3 BLOCKER "kernel version" \
        "${KREL} is below ${KERNEL_FLOOR}: cgroup-BPF socket marking and the nft cgroupv2 match"
    note "The daemon's self-exclusion has two independent legs and this kernel"
    note "supports neither reliably. Do not run a tunnel here."
fi

CONTAINER=''
[ -f /run/.containerenv ] && CONTAINER='podman/toolbox/distrobox'
[ -f /.dockerenv ] && CONTAINER='docker'
if [ -z "${CONTAINER}" ] && [ -r /proc/1/environ ] && grep -qa 'container=' /proc/1/environ 2>/dev/null; then
    CONTAINER='systemd-nspawn or similar'
fi
if [ -n "${CONTAINER}" ]; then
    check 1.4 WARN "container" "inside a container (${CONTAINER})"
    note "Read every answer below as a statement about the CONTAINER, not the host."
    note "cgroup paths, LSM labels, /dev/net/tun and the firewall are all namespaced"
    note "or filtered here. Run this on the real machine before you trust it."
else
    check 1.4 ok "container" "not in a container -- these answers describe the real host"
fi

# ==========================================================================
head2 "2. how URnetwork gets installed here"
# ==========================================================================
# The release asset names are a contract (the in-app service checker parses
# them):
#   urnetwork-daemon-<v>-<arch>.install.tar.gz
#   urnetwork-daemon_<v>_<arch>.deb
#   urnetwork-daemon-<v>.<rpmarch>.rpm
#   urnetwork-daemon-<v>-<pacmanarch>.pkg.tar.zst
#   URnetwork-<v>-<arch>.AppImage  (+ .zsync, + .flatpak)
# All four daemon packages are the same payload out of one staging tree
# (packaging/lib/common.sh assemble_daemon_root), so "which channel" is a
# question about THIS HOST, not about which build is newer. Whether a given
# release actually carries a given package is still a fact about the RELEASE,
# which is why the branches below hedge rather than promise.

PKG_CHANNEL=''
if have dpkg; then
    PKG_CHANNEL='deb'
    check 2.1 ok "native package format" "dpkg present -> urnetwork-daemon_<v>_${UR_ARCH:-<arch>}.deb"
elif have rpm; then
    PKG_CHANNEL='rpm'
    # Deliberately 'ok', not a warning: whether the release carries an .rpm is a
    # fact about the RELEASE, and this script's job is to report facts about the
    # HOST. Putting a packaging TODO in the blockers list would train the reader
    # to skim the one list that must never be skimmed.
    check 2.1 ok "native package format" "rpm present -> urnetwork-daemon-<v>.$(uname -m).rpm, if the release carries one"
    note "Check the release assets before reaching for it: the published set has"
    note "historically been the install tarball, the .deb and the AppImage. If there"
    note "is no .rpm for your version, use the install tarball -- it supports"
    note "rpm-family hosts (ostree included) and installs the SELinux policy module."
elif have apk; then
    PKG_CHANNEL='apk'
    check 2.1 BLOCKER "native package format" "apk (Alpine): musl, and no musl build exists"
elif have pacman; then
    PKG_CHANNEL='pacman'
    check 2.1 ok "native package format" "pacman present -> urnetwork-daemon-<v>-$(uname -m).pkg.tar.zst, if the release carries one"
    note "Install it with: sudo pacman -U ./urnetwork-daemon-<v>-$(uname -m).pkg.tar.zst"
    note "It declares nftables and fuse2, which a minimal Arch install does not have and"
    note "which the tarball can only tell you about after the fact. If the release has no"
    note "pacman package, the install tarball works here too -- but never both on one"
    note "machine: each refuses to overwrite the other's files."
    if [ -r /usr/lib/os-release ] && grep -qi '^ID=steamos' /usr/lib/os-release 2>/dev/null; then
        check 2.1 WARN "native package format" "SteamOS: /usr is read-only, 'pacman -U' needs 'steamos-readonly disable' and the next system update reverts it -- use the install tarball, which installs under /usr/local"
    fi
elif have zypper; then
    PKG_CHANNEL='zypper'
    check 2.1 WARN "native package format" "zypper present; no .rpm is published -- use the tarball"
else
    check 2.1 WARN "native package format" "no dpkg/rpm/apk/pacman found -- tarball installer only"
fi

if have flatpak; then
    FP_REMOTES="$(flatpak remotes --columns=name 2>/dev/null | tr '\n' ' ')"
    FP_REMOTES="${FP_REMOTES%% }"
    check 2.2 ok "flatpak" "present; remotes: ${FP_REMOTES:-<none>}"
    note "No .flatpak bundle is published in the release yet either -- the manifest"
    note "at packaging/flatpak/com.bringyour.network.yml builds one locally."
else
    check 2.2 n/a "flatpak" "not installed (only matters for the GUI, never the daemon)"
fi

# AppImage: a type-2 AppImage mounts itself with libfuse.so.2. Ubuntu 22.04+ and
# Fedora ship fuse3 only, and this is the single most common "the AppImage does
# nothing when I double-click it" on every vendor's issue tracker.
FUSE2=''
if have ldconfig; then
    FUSE2="$(ldconfig -p 2>/dev/null | awk '/libfuse\.so\.2/ {print $NF; exit}')"
fi
if [ -z "${FUSE2}" ]; then
    for d in /lib64 /usr/lib64 /usr/lib /lib "/usr/lib/${KARCH}-linux-gnu"; do
        [ -e "${d}/libfuse.so.2" ] && { FUSE2="${d}/libfuse.so.2"; break; }
    done
fi
if [ -n "${FUSE2}" ] && [ -e /dev/fuse ]; then
    check 2.3 ok "AppImage runtime (libfuse2)" "${FUSE2} + /dev/fuse"
elif [ -n "${FUSE2}" ]; then
    check 2.3 WARN "AppImage runtime (libfuse2)" "libfuse.so.2 present but /dev/fuse is missing"
else
    check 2.3 WARN "AppImage runtime (libfuse2)" "libfuse.so.2 not found -- the GUI AppImage will not mount"
    note "Fix per family:  apt install libfuse2t64 (or libfuse2)  |  dnf install fuse-libs"
    note "                  pacman -S fuse2"
    note "Or run the AppImage with --appimage-extract-and-run. The .deb and the pacman"
    note "package declare this dependency for you; the tarball and the bare AppImage"
    note "cannot."
fi

# The layout the tarball installer WILL choose, by the same rules install.sh
# uses -- minus its write probe, which creates a file. Mount options are the
# authority here; `test -w` lies for a non-root reader.
USR_OPTS=''
if have findmnt; then
    USR_OPTS="$(findmnt -no OPTIONS --target /usr 2>/dev/null)"
elif [ -r /proc/self/mountinfo ]; then
    USR_OPTS="$(awk '$5=="/usr" || $5=="/" {mp=$5; opts=$6} END {print opts}' /proc/self/mountinfo)"
fi
LAYOUT='standard'
LAYOUT_WHY='/usr is writable'
if [ -f /run/ostree-booted ]; then
    LAYOUT='immutable'; LAYOUT_WHY='/run/ostree-booted exists'
else
    case ",${USR_OPTS}," in
        *,ro,*) LAYOUT='immutable'; LAYOUT_WHY='/usr is mounted read-only' ;;
    esac
fi
if [ "${LAYOUT}" = 'immutable' ]; then
    check 2.4 ok "filesystem layout" "immutable host (${LAYOUT_WHY})"
    note "install.sh remaps: /usr/bin -> /usr/local/bin, /usr/lib/urnetwork ->"
    note "/usr/local/lib/urnetwork, /lib/systemd/system -> /etc/systemd/system,"
    note "and rewrites the unit's ExecStart to match. No .deb or .rpm can do this,"
    note "so on this host the tarball installer is the ONLY correct channel."
else
    check 2.4 ok "filesystem layout" "standard host (${LAYOUT_WHY:-assumed})"
fi

# ==========================================================================
head2 "3. the init system the daemon needs"
# ==========================================================================
# urnetworkd is Type=notify and depends on RuntimeDirectory=,
# RuntimeDirectoryPreserve=yes, StateDirectory=, LogsDirectory= and
# ExecStopPost=. That last one is what guarantees the fail-closed nftables
# table cannot outlive the service after a SIGKILL. None of it has an
# equivalent in the sysvinit/OpenRC scripts that do not exist yet.
INIT_COMM=''
[ -r /proc/1/comm ] && INIT_COMM="$(cat /proc/1/comm 2>/dev/null)"
if [ "${INIT_COMM}" = 'systemd' ]; then
    check 3.1 ok "PID 1" "systemd"
else
    check 3.1 BLOCKER "PID 1" "${INIT_COMM:-unknown} -- urnetworkd ships a systemd unit only"
    note "Type=notify readiness, RuntimeDirectoryPreserve=yes (the kill-switch marker)"
    note "and ExecStopPost=--revert-unless-armed have no equivalent here. A daemon"
    note "started by hand can arm a fail-closed ruleset that nothing will remove."
fi

SD_VER=''
for probe in journalctl systemd-analyze systemctl; do
    if have "${probe}"; then
        SD_VER="$("${probe}" --version 2>/dev/null | awk 'NR==1{print $2; exit}')"
        [ -n "${SD_VER}" ] && break
    fi
done
if [ -n "${SD_VER}" ]; then
    if ver_ge "${SD_VER}" "${SYSTEMD_FLOOR}"; then
        check 3.2 ok "systemd version" "${SD_VER} (floor ${SYSTEMD_FLOOR})"
    else
        check 3.2 BLOCKER "systemd version" \
            "${SD_VER} < ${SYSTEMD_FLOOR}: no StateDirectory=/RuntimeDirectoryPreserve="
    fi
else
    check 3.2 n/a "systemd version" "could not be read"
fi

if [ -d /etc/systemd/system ]; then
    check 3.3 ok "unit load path" "/etc/systemd/system exists (where the immutable layout installs)"
else
    check 3.3 WARN "unit load path" "/etc/systemd/system is missing"
fi

# ==========================================================================
head2 "4. the mandatory access control layer -- the one that broke this before"
# ==========================================================================
# Read this section first on any new distro. On Bazzite the daemon ran as root
# with CAP_NET_ADMIN and still could not open /dev/net/tun, create a raw socket
# or connect to 443, because SELinux put it in init_t and refuses before the
# capability is ever consulted. Nothing in the daemon's own logs said "SELinux".

LSMS=''
[ -r /sys/kernel/security/lsm ] && LSMS="$(cat /sys/kernel/security/lsm 2>/dev/null)"
check 4.1 -- "active LSMs" "${LSMS:-<unreadable; /sys/kernel/security may not be mounted>}"

SEMODE=''
if have getenforce; then
    SEMODE="$(getenforce 2>/dev/null)"
elif [ -r /sys/fs/selinux/enforce ]; then
    case "$(cat /sys/fs/selinux/enforce 2>/dev/null)" in
        1) SEMODE='Enforcing' ;; 0) SEMODE='Permissive' ;;
    esac
fi
SELINUX_ON=0
case "${SEMODE}" in
    Enforcing)  SELINUX_ON=1; check 4.2 WARN "SELinux" "Enforcing -- the policy module is REQUIRED here" ;;
    Permissive) SELINUX_ON=1; check 4.2 WARN "SELinux" "Permissive -- it will work and log every denial it would have made"
                note "Permissive is not a supported end state: the first person to set it back"
                note "to Enforcing loses the tunnel. Install the policy module anyway." ;;
    Disabled)   check 4.2 ok "SELinux" "Disabled -- no policy module needed" ;;
    *)  # "no getenforce" is not the same as "no SELinux". A container on a
        # Fedora host has neither the tool nor /sys/fs/selinux, while the kernel
        # the daemon will actually run under is enforcing. Saying "not present"
        # there would be the single most misleading line this script could print.
        case ",${LSMS}," in
            *,selinux,*)
                check 4.2 WARN "SELinux" "the kernel lists selinux (4.1) but its mode is unreadable here"
                note "getenforce is absent and /sys/fs/selinux is not mounted -- the usual"
                note "shape inside a container. Ask the HOST: the daemon runs there, and if"
                note "that host is enforcing, the policy module is REQUIRED." ;;
            *)  check 4.2 ok "SELinux" "not present on this host" ;;
        esac ;;
esac

if [ "${SELINUX_ON}" = 1 ]; then
    SEMISSING=''
    for t in checkmodule semodule_package semodule; do
        have "${t}" || SEMISSING="${SEMISSING} ${t}"
    done
    if [ -z "${SEMISSING}" ]; then
        check 4.3 ok "SELinux policy tools" "checkmodule, semodule_package, semodule all present"
    else
        check 4.3 BLOCKER "SELinux policy tools" "missing:${SEMISSING}"
        note "Without these the installer cannot build packaging/selinux/urnetwork.te,"
        note "the daemon starts in init_t, and the tunnel cannot open /dev/net/tun."
        note "Install:  dnf install policycoreutils-devel   (or checkpolicy + policycoreutils)"
    fi

    # The policy module's require block names tun_tap_device_t. A distro that
    # labels the device differently would compile a module that grants nothing.
    if [ -e /dev/net/tun ] && have stat; then
        TUNLABEL="$(stat -c %C /dev/net/tun 2>/dev/null)"
        case "${TUNLABEL}" in
            *:${TUN_DEVICE_LABEL}:*)
                check 4.4 ok "/dev/net/tun label" "${TUNLABEL}" ;;
            ''|'?')
                check 4.4 n/a "/dev/net/tun label" "no label reported" ;;
            *)
                check 4.4 WARN "/dev/net/tun label" "${TUNLABEL}, not ${TUN_DEVICE_LABEL}"
                note "urnetwork.te's require block names ${TUN_DEVICE_LABEL}. On this policy the"
                note "module may build and still grant nothing. Check with: ausearch -m avc -ts recent" ;;
        esac
    else
        check 4.4 n/a "/dev/net/tun label" "device absent (see 5.3)"
    fi

    if [ "${PRIVILEGED}" = 1 ] && [ "${IS_ROOT}" = 1 ]; then
        if semodule -l 2>/dev/null | grep -qx 'urnetwork'; then
            check 4.5 ok "urnetwork policy module" "installed"
        else
            check 4.5 BLOCKER "urnetwork policy module" "NOT installed on an SELinux host"
            note "The tarball installer does this; a hand-copied binary does not."
            note "  cd packaging/selinux && checkmodule -M -m -o urnetwork.mod urnetwork.te \\"
            note "    && semodule_package -o urnetwork.pp -m urnetwork.mod && semodule -i urnetwork.pp"
        fi
    else
        check 4.5 needs-root "urnetwork policy module" "semodule -l needs root"
    fi
else
    check 4.3 n/a "SELinux policy tools" "not applicable"
    check 4.4 n/a "/dev/net/tun label" "not applicable"
    check 4.5 n/a "urnetwork policy module" "not applicable"
fi

# AppArmor. This is the SELinux of the Debian family and it has not yet been
# measured against this daemon anywhere. Treat a hit here as a live risk.
AA_ON=0
if [ -d /sys/kernel/security/apparmor ]; then AA_ON=1; fi
if [ -r /sys/module/apparmor/parameters/enabled ] &&
   [ "$(cat /sys/module/apparmor/parameters/enabled 2>/dev/null)" = 'Y' ]; then AA_ON=1; fi
if [ "${AA_ON}" = 1 ]; then
    check 4.6 WARN "AppArmor" "enabled -- URnetwork ships NO AppArmor profile and has never been tested under one"
    note "The failure to expect is the SELinux one in a different accent: the daemon"
    note "is root and unconfined_t-equivalent today, but a host that confines"
    note "/usr/bin/nft, /usr/sbin/ip or the daemon's own path will break the tunnel"
    note "with no message from urnetworkd. If a tunnel fails here, look FIRST at:"
    note "  journalctl -k | grep -i 'apparmor.*DENIED'   and   dmesg | grep DENIED"
    if have aa-status && [ "${IS_ROOT}" = 1 ]; then
        AA_ENF="$(aa-status --enforced 2>/dev/null | tail -1)"
        [ -n "${AA_ENF}" ] && note "profiles in enforce mode: ${AA_ENF}"
    fi
else
    check 4.6 ok "AppArmor" "not enabled here"
fi

# Ubuntu 24.04+ restricts unprivileged user namespaces via AppArmor. The daemon
# does not use them; the GUI's sandboxes (AppImage helpers, bwrap, Flatpak) do.
USERNS_NOTE=''
if [ -r /proc/sys/kernel/apparmor_restrict_unprivileged_userns ]; then
    USERNS_NOTE="apparmor_restrict_unprivileged_userns=$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null)"
elif [ -r /proc/sys/kernel/unprivileged_userns_clone ]; then
    USERNS_NOTE="unprivileged_userns_clone=$(cat /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null)"
fi
case "${USERNS_NOTE}" in
    *'=1'|*'userns_clone=0')
        check 4.7 WARN "unprivileged user namespaces" "restricted (${USERNS_NOTE})"
        note "This is Ubuntu 24.04+ behaviour. The DAEMON is unaffected (it is root and"
        note "uses no user namespace). What breaks is the GUI side: bwrap-based helpers"
        note "and some AppImage runtimes. The Flatpak is fine (its bwrap is setuid)." ;;
    '')  check 4.7 ok "unprivileged user namespaces" "no restriction knob on this kernel" ;;
    *)   check 4.7 ok "unprivileged user namespaces" "permitted (${USERNS_NOTE})" ;;
esac

LOCKDOWN=''
[ -r /sys/kernel/security/lockdown ] && \
    LOCKDOWN="$(sed -n 's/.*\[\(.*\)\].*/\1/p' /sys/kernel/security/lockdown 2>/dev/null)"
case "${LOCKDOWN}" in
    confidentiality)
        check 4.8 WARN "kernel lockdown" "confidentiality -- bpf() is restricted in this mode"
        note "The egress marker is a BPF program. Prove it with --privileged before"
        note "trusting a tunnel here; a load failure is the expected symptom." ;;
    integrity) check 4.8 ok "kernel lockdown" "integrity (Secure Boot); does not restrict cgroup-BPF" ;;
    none)      check 4.8 ok "kernel lockdown" "none" ;;
    *)         check 4.8 n/a "kernel lockdown" "not reported by this kernel" ;;
esac

# ==========================================================================
head2 "5. the packet path"
# ==========================================================================
# ip and nft are REQUIRED -- the daemon's own preflight counts a missing one as
# fatal (src/daemon/main.cpp ReportPreflight). Without nft there is no egress
# self-exclusion, no IPv6 floor, no DNS floor and no kill switch, and the
# daemon's own traffic falls into the daemon's own tunnel.
IP_BIN="$(command -v ip 2>/dev/null || true)"
[ -z "${IP_BIN}" ] && for d in /usr/sbin /sbin; do [ -x "${d}/ip" ] && IP_BIN="${d}/ip" && break; done
if [ -n "${IP_BIN}" ]; then
    check 5.1 ok "iproute2 (ip)" "${IP_BIN}"
else
    check 5.1 BLOCKER "iproute2 (ip)" "not on PATH or in /usr/sbin:/sbin"
    note "The tun address, the capture routes and the policy rules are all built with it."
fi

NFT_BIN="$(command -v nft 2>/dev/null || true)"
[ -z "${NFT_BIN}" ] && for d in /usr/sbin /sbin; do [ -x "${d}/nft" ] && NFT_BIN="${d}/nft" && break; done
if [ -n "${NFT_BIN}" ]; then
    NFT_VER="$("${NFT_BIN}" --version 2>/dev/null)"
    if ver_ge "${NFT_VER}" "${NFT_FLOOR}"; then
        check 5.2 ok "nftables (nft)" "${NFT_BIN} -- ${NFT_VER}"
    else
        check 5.2 BLOCKER "nftables (nft)" "${NFT_VER:-unknown} < ${NFT_FLOOR}: no \`socket cgroupv2\` match"
    fi
else
    check 5.2 BLOCKER "nftables (nft)" "not on PATH or in /usr/sbin:/sbin"
    note "Install:  apt install nftables  |  dnf install nftables  |  zypper in nftables"
    note "Without it the daemon has no kill switch, no IPv6 floor and no way to keep"
    note "its own sockets out of its own tunnel."
fi

if [ -e /dev/net/tun ]; then
    check 5.3 ok "/dev/net/tun" "present"
elif have modinfo && modinfo -F filename tun >/dev/null 2>&1; then
    check 5.3 ok "/dev/net/tun" "absent, but the tun module exists ($(modinfo -F filename tun 2>/dev/null))"
    note "The daemon attempts one modprobe at first start; systemd-modules-load or the"
    note "unit's first Connect will create the device."
else
    check 5.3 BLOCKER "/dev/net/tun" "absent and no tun module found for kernel ${KREL}"
fi

# Kernel config, when the host publishes it. Not every distro does, and its
# absence is 'n/a' -- the authority for the BPF question is --selftest-egress.
KCONF=''
for c in "/boot/config-${KREL}" "/lib/modules/${KREL}/config" /proc/config.gz; do
    [ -r "${c}" ] && KCONF="${c}" && break
done
if [ -n "${KCONF}" ]; then
    if [ "${KCONF}" = /proc/config.gz ]; then
        KCONF_TEXT="$(zcat /proc/config.gz 2>/dev/null)"
    else
        KCONF_TEXT="$(cat "${KCONF}" 2>/dev/null)"
    fi
    KMISSING=''
    # Matched in-shell rather than through `| grep -q`: see the pipefail note at
    # the top. The newline padding makes ^ and $ out of glob anchors.
    KCONF_PADDED=$'\n'"${KCONF_TEXT}"$'\n'
    for opt in CONFIG_BPF_SYSCALL CONFIG_CGROUP_BPF CONFIG_TUN CONFIG_NF_TABLES; do
        case "${KCONF_PADDED}" in
            *$'\n'"${opt}=y"$'\n'*|*$'\n'"${opt}=m"$'\n'*) ;;
            *) KMISSING="${KMISSING} ${opt}" ;;
        esac
    done
    if [ -z "${KMISSING}" ]; then
        check 5.4 ok "kernel features" "BPF_SYSCALL, CGROUP_BPF, TUN, NF_TABLES all built (${KCONF})"
    else
        check 5.4 BLOCKER "kernel features" "missing:${KMISSING} (${KCONF})"
        note "CGROUP_BPF is not optional here: it is how the daemon keeps its own"
        note "sockets out of its own tunnel before connect() picks a source address."
    fi
else
    check 5.4 n/a "kernel features" "this kernel publishes no config; --privileged measures it instead"
fi

# Who else manages the ruleset. The daemon owns `table inet urnetwork` and
# nothing else, so coexistence is normally fine -- except for the distro
# nftables.service whose shipped config begins with `flush ruleset`, which is
# exactly what the daemon's reaper exists to survive.
FW_OTHER=''
for svc in firewalld nftables ufw iptables; do
    for w in "/etc/systemd/system/multi-user.target.wants/${svc}.service" \
             "/etc/systemd/system/basic.target.wants/${svc}.service"; do
        if [ -e "${w}" ]; then
            case " ${FW_OTHER} " in *" ${svc}(enabled) "*) ;; *) FW_OTHER="${FW_OTHER} ${svc}(enabled)" ;; esac
        fi
    done
done
for svc in firewalld ufw; do
    proc_running "${svc}" && FW_OTHER="${FW_OTHER} ${svc}(running)"
done
FW_OTHER="${FW_OTHER# }"
FLUSHER=''
for f in /etc/sysconfig/nftables.conf /etc/nftables.conf; do
    [ -r "${f}" ] && grep -q '^[[:space:]]*flush ruleset' "${f}" 2>/dev/null && FLUSHER="${FLUSHER} ${f}"
done
if [ -n "${FLUSHER}" ]; then
    check 5.5 WARN "other firewall managers" "${FW_OTHER:-none enabled}; \`flush ruleset\` in:${FLUSHER}"
    note "Reloading that service wipes the daemon's table along with everything else."
    note "The daemon re-installs its ruleset within seconds (tamper reaper), so this is"
    note "a brief window rather than a leak -- but it is the window to look at if a"
    note "connected session ever goes unprotected for a moment on this distro."
elif [ -n "${FW_OTHER}" ]; then
    check 5.5 ok "other firewall managers" "${FW_OTHER} -- separate tables, no conflict expected"
else
    check 5.5 ok "other firewall managers" "none enabled"
fi

# ==========================================================================
head2 "6. the name path"
# ==========================================================================
# resolvectl is OPTIONAL to the daemon and load-bearing to the user: without it
# ApplyDns() gives up and the session comes up with dns_applied=false, which the
# daemon reports honestly as "DNS is NOT going through the tunnel".
if have resolvectl; then
    check 6.1 ok "resolvectl" "$(command -v resolvectl)"
else
    check 6.1 WARN "resolvectl" "absent: the tunnel will come up with dns_applied=false"
    note "The daemon says so rather than pretending. Queries keep going to whatever"
    note "resolver the host had -- outside the tunnel, and visible to the local network"
    note "unless the nftables DNS floor catches them. Install systemd-resolved, or accept"
    note "that this distro is a DNS-leak platform for URnetwork today."
fi

RESOLV_TARGET=''
have readlink && RESOLV_TARGET="$(readlink -f /etc/resolv.conf 2>/dev/null)"
RESOLVED_RUNNING=0
proc_running systemd-resolved && RESOLVED_RUNNING=1
case "${RESOLV_TARGET}" in
    /run/systemd/resolve/*)
        if [ "${RESOLVED_RUNNING}" = 1 ]; then
            check 6.2 ok "resolver wiring" "systemd-resolved, /etc/resolv.conf -> ${RESOLV_TARGET}"
        else
            check 6.2 WARN "resolver wiring" "resolv.conf points at systemd-resolved but it is not running"
        fi ;;
    '')  check 6.2 WARN "resolver wiring" "/etc/resolv.conf could not be resolved" ;;
    *)   if [ "${RESOLVED_RUNNING}" = 1 ]; then
             check 6.2 WARN "resolver wiring" "resolved is running but /etc/resolv.conf is ${RESOLV_TARGET}"
             note "Programs that read /etc/resolv.conf directly bypass the per-link DNS the"
             note "daemon sets with resolvectl. This is the classic 'DNS still leaks with"
             note "resolved installed' shape -- check on this distro before shipping it."
         else
             check 6.2 WARN "resolver wiring" "${RESOLV_TARGET} (no systemd-resolved)"
             note "The daemon can only pin DNS through resolvectl. Here it will not pin at all."
         fi ;;
esac

if have lsattr && lsattr /etc/resolv.conf 2>/dev/null | grep -q 'i'; then
    check 6.3 WARN "resolv.conf mutability" "immutable bit set (chattr +i)"
else
    check 6.3 ok "resolv.conf mutability" "not immutable"
fi

# ==========================================================================
head2 "7. the process path -- cgroup v2"
# ==========================================================================
# Everything the daemon does to exclude itself names a cgroup: the BPF program
# is attached to one and the nftables rule matches one. No cgroup v2, no
# self-exclusion, and the preflight refuses.
CG_FS="$(stat -fc %T /sys/fs/cgroup 2>/dev/null)"
if [ "${CG_FS}" = 'cgroup2fs' ]; then
    check 7.1 ok "cgroup hierarchy" "unified cgroup v2 (/sys/fs/cgroup is cgroup2fs)"
elif [ -n "${CG_FS}" ]; then
    check 7.1 BLOCKER "cgroup hierarchy" "/sys/fs/cgroup is ${CG_FS}, not cgroup2fs (v1 or hybrid)"
    note "The daemon's preflight counts this as a missing REQUIRED facility and will"
    note "not start a tunnel: neither the BPF marker nor the nft rule can name a process."
    note "Boot with systemd.unified_cgroup_hierarchy=1."
else
    check 7.1 BLOCKER "cgroup hierarchy" "/sys/fs/cgroup is not mounted"
fi

MY_CG="$(sed -n 's|^0::||p' /proc/self/cgroup 2>/dev/null)"
if [ -n "${MY_CG}" ] && [ "${MY_CG}" != '/' ]; then
    check 7.2 ok "this shell's cgroup v2 path" "0::${MY_CG}"
elif [ "${MY_CG}" = '/' ]; then
    check 7.2 WARN "this shell's cgroup v2 path" "0::/ -- a cgroup NAMESPACE is in the way"
    note "That is what a container sees. The daemon would have the same problem here:"
    note "a path it cannot name is a marker it cannot attach."
else
    check 7.2 BLOCKER "this shell's cgroup v2 path" "/proc/self/cgroup has no 0:: line"
fi

CG_NS_SELF="$(readlink /proc/self/ns/cgroup 2>/dev/null)"
CG_NS_INIT="$(readlink /proc/1/ns/cgroup 2>/dev/null)"
if [ -n "${CG_NS_SELF}" ] && [ -n "${CG_NS_INIT}" ] && [ "${CG_NS_SELF}" != "${CG_NS_INIT}" ]; then
    check 7.3 WARN "cgroup namespace" "this process is in its own cgroup namespace"
elif [ -n "${CG_NS_SELF}" ]; then
    check 7.3 ok "cgroup namespace" "same namespace as PID 1"
else
    check 7.3 n/a "cgroup namespace" "not readable"
fi

if [ -d /sys/fs/cgroup/system.slice ]; then
    check 7.4 ok "system.slice" "present -- where urnetworkd.service will live"
else
    check 7.4 WARN "system.slice" "absent: the daemon's cgroup path will not look like the tested one"
fi

# ==========================================================================
head2 "8. the C runtime, and the GUI"
# ==========================================================================
# Every shipped binary -- daemon, GUI, SDK -- is cross-built against glibc 2.35.
# A host below that floor cannot run ANY published asset; a musl host cannot run
# them at all. This is the check that decides whether a distro is even a
# candidate before anything else is worth reading.
LIBC_KIND='glibc'; LIBC_VER=''
if have getconf; then LIBC_VER="$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')"; fi
if [ -z "${LIBC_VER}" ] && have ldd; then
    LDD_OUT="$(ldd --version 2>&1 | head -1)"
    case "${LDD_OUT}" in
        *musl*) LIBC_KIND='musl' ;;
        *)      LIBC_VER="$(printf '%s' "${LDD_OUT}" | awk '{print $NF}')" ;;
    esac
fi
for m in /lib/ld-musl-*.so.1 /lib64/ld-musl-*.so.1; do [ -e "${m}" ] && LIBC_KIND='musl'; done
if [ "${LIBC_KIND}" = 'musl' ]; then
    check 8.1 BLOCKER "C library" "musl -- every published URnetwork binary is glibc-only"
elif [ -n "${LIBC_VER}" ] && ver_ge "${LIBC_VER}" "${GLIBC_FLOOR}"; then
    check 8.1 ok "C library" "glibc ${LIBC_VER} (floor ${GLIBC_FLOOR})"
elif [ -n "${LIBC_VER}" ]; then
    check 8.1 BLOCKER "C library" "glibc ${LIBC_VER} < ${GLIBC_FLOOR}"
    note "The SDK is cross-built against jammy's 2.35. Nothing published here will"
    note "load; the AppImage will fail with a GLIBC_2.xx not found from ld.so."
else
    check 8.1 n/a "C library" "version could not be read"
fi

SESSION="${XDG_SESSION_TYPE:-}"
if [ -z "${SESSION}" ]; then
    [ -n "${WAYLAND_DISPLAY:-}" ] && SESSION='wayland'
    [ -n "${DISPLAY:-}" ] && SESSION="${SESSION:-x11}"
fi
if [ -n "${SESSION}" ]; then
    check 8.2 ok "graphical session" "${SESSION}"
else
    check 8.2 -- "graphical session" "none visible (headless, or a root shell) -- daemon-only host"
fi

# Host GTK matters only for a from-source build: the AppImage bundles its own
# GTK4 stack via ldd, and the Flatpak brings the GNOME runtime.
if have pkg-config && pkg-config --exists gtk4 libadwaita-1 2>/dev/null; then
    check 8.3 ok "host GTK4 + libadwaita" "$(pkg-config --modversion gtk4 2>/dev/null) / $(pkg-config --modversion libadwaita-1 2>/dev/null)"
else
    check 8.3 -- "host GTK4 + libadwaita" "not detected -- only needed to BUILD the GUI here"
    note "The AppImage bundles its own GTK4 stack and the Flatpak uses the GNOME runtime."
fi

# ==========================================================================
head2 "9. what is already installed on this host"
# ==========================================================================
DAEMON=''
for p in "${DAEMON_PATHS[@]}"; do [ -x "${p}" ] && DAEMON="${p}" && break; done
[ -z "${DAEMON}" ] && DAEMON="$(command -v urnetworkd 2>/dev/null || true)"
if [ -n "${DAEMON}" ]; then
    DVER="$("${DAEMON}" --version 2>/dev/null | head -1)"
    check 9.1 ok "daemon binary" "${DAEMON}${DVER:+  (${DVER})}"
    # WHO OWNS THESE PATHS. The tarball installer REFUSES to run where dpkg or
    # rpm owns urnetwork-daemon, because two package managers owning one set of
    # files is silent until an upgrade half-replaces them. Say which one it is
    # while the answer is still cheap.
    OWNER=''
    have dpkg && dpkg -S "${DAEMON}" >/dev/null 2>&1 && OWNER='dpkg'
    have rpm && rpm -qf "${DAEMON}" >/dev/null 2>&1 && OWNER="${OWNER:+${OWNER}+}rpm"
    if [ -n "${OWNER}" ]; then
        note "owned by ${OWNER} -- upgrade with the package manager, NOT the tarball installer"
    else
        note "not owned by any package manager -- installed by the tarball installer"
        note "(this is also why it runs in init_t under SELinux: no package, no policy)"
    fi
else
    check 9.1 -- "daemon binary" "not installed yet -- that is the expected state on a fresh host"
fi

UNIT_PATH=''
for u in "/etc/systemd/system/${UNIT}" "/lib/systemd/system/${UNIT}" "/usr/lib/systemd/system/${UNIT}"; do
    [ -f "${u}" ] && UNIT_PATH="${u}" && break
done
if [ -n "${UNIT_PATH}" ]; then
    check 9.2 ok "systemd unit" "${UNIT_PATH}"
    if [ -e "/etc/systemd/system/multi-user.target.wants/${UNIT}" ]; then
        note "enabled (multi-user.target.wants symlink present)"
    else
        note "installed but NOT enabled -- it will not come back after a reboot"
    fi
else
    check 9.2 -- "systemd unit" "not installed"
fi

DPID=''
have pgrep && DPID="$(pgrep -x urnetworkd 2>/dev/null | head -1)"
if [ -n "${DPID}" ]; then
    DOMAIN=''
    have ps && DOMAIN="$(ps -o label= -p "${DPID}" 2>/dev/null | awk '{print $1}')"
    check 9.3 ok "daemon process" "pid ${DPID}${DOMAIN:+  domain ${DOMAIN}}"
    if [ -n "${DOMAIN}" ] && [ "${SELINUX_ON}" = 1 ]; then
        case "${DOMAIN}" in
            *:init_t:*) note "init_t is expected for a non-packaged install: it is exactly the domain"
                        note "packaging/selinux/urnetwork.te grants tun, rawip and 443 to." ;;
        esac
    fi
    DCG="$(sed -n 's|^0::||p' "/proc/${DPID}/cgroup" 2>/dev/null)"
    [ -n "${DCG}" ] && note "cgroup 0::${DCG}"
else
    check 9.3 -- "daemon process" "not running"
fi

if [ -S "${CONTROL_SOCKET}" ]; then
    # -L: on some hosts /run/urnetwork is reached through a symlink, and stat
    # without it describes the link (lrwxrwxrwx root:root) instead of the socket
    # whose mode is the whole point of the check.
    SOCKINFO="$(stat -Lc '%A %U:%G' "${CONTROL_SOCKET}" 2>/dev/null)"
    check 9.4 ok "control socket" "${CONTROL_SOCKET}  ${SOCKINFO}"
else
    check 9.4 -- "control socket" "absent (the daemon creates it at start)"
fi

if getent group "${CONTROL_GROUP}" >/dev/null 2>&1; then
    if [ "${IS_ROOT}" = 1 ]; then
        check 9.5 ok "control group" "'${CONTROL_GROUP}' exists; root is always authorized"
    elif id -nG 2>/dev/null | tr ' ' '\n' | grep -qx "${CONTROL_GROUP}"; then
        check 9.5 ok "control group" "'${CONTROL_GROUP}' exists and $(id -un) is in it"
    else
        check 9.5 WARN "control group" "$(id -un) is NOT in '${CONTROL_GROUP}': the GUI cannot reach the daemon"
        note "  sudo usermod -aG ${CONTROL_GROUP} $(id -un)   then log out and back in"
        note "Group membership is read at LOGIN; a fresh shell is not enough."
    fi
else
    check 9.5 -- "control group" "'${CONTROL_GROUP}' does not exist yet (the installer creates it)"
fi

GUI_FOUND=''
[ -x /usr/bin/urnetwork ] && GUI_FOUND="/usr/bin/urnetwork"
[ -x /usr/local/bin/urnetwork ] && GUI_FOUND="${GUI_FOUND} /usr/local/bin/urnetwork"
if have flatpak && flatpak list --app --columns=application 2>/dev/null | grep -qx 'com.bringyour.network'; then
    GUI_FOUND="${GUI_FOUND} flatpak:com.bringyour.network"
fi
if [ -n "${GUI_FOUND}" ]; then
    check 9.6 ok "GUI" "${GUI_FOUND# }"
else
    check 9.6 -- "GUI" "not installed"
fi

TUN_UP=0
if [ -n "${IP_BIN}" ] && "${IP_BIN}" -o link show "${TUN_NAME}" >/dev/null 2>&1; then
    TUN_UP=1
    TUNADDR="$("${IP_BIN}" -4 -o addr show dev "${TUN_NAME}" 2>/dev/null | awk '{print $4}' | head -1)"
    check 9.7 ok "live tunnel" "${TUN_NAME} exists${TUNADDR:+, ${TUNADDR}}"
else
    check 9.7 -- "live tunnel" "${TUN_NAME} does not exist -- no tunnel is up right now"
fi

# ==========================================================================
# 10. privileged checks
# ==========================================================================
if [ "${PRIVILEGED}" = 0 ]; then
    head2 "10. the privileged checks -- NOT RUN (this is the default)"
    say "  These are the ones that need root. None of them ran. What they would do:"
    say ""
    say "   10.1  nft list table inet ${NFT_TABLE}      does the daemon's ruleset exist, and"
    say "                                          how many packets has each chain seen"
    say "   10.2  semodule -l | grep urnetwork      is the SELinux policy module installed"
    say "   10.3  systemctl is-active/is-enabled    what systemd thinks of ${UNIT}"
    say "   10.4  journalctl -u urnetworkd + AVCs   what actually failed last time"
    say "   10.5  urnetworkd --selftest-egress      THE measurement: load a cgroup-BPF program"
    say "                                          on THIS kernel and read SO_MARK back off a"
    say "                                          fresh socket. Creates one temporary cgroup"
    say "                                          and removes it; no tun, no routes, no"
    say "                                          nftables, no DNS, no packet."
    say ""
    say "  Run them with:   sudo bash ${SELF} --privileged"
    say "  Nothing above changed this machine, and neither will that."
elif [ "${IS_ROOT}" != 1 ]; then
    head2 "10. the privileged checks -- REFUSED"
    check 10.0 needs-root "privileged checks" "--privileged was given but this is uid $(id -u)"
    note "Re-run as:  sudo bash ${SELF} --privileged"
else
    head2 "10. the privileged checks"

    if [ -n "${NFT_BIN}" ]; then
        if "${NFT_BIN}" list table inet "${NFT_TABLE}" >/dev/null 2>&1; then
            NCHAINS="$("${NFT_BIN}" list table inet "${NFT_TABLE}" 2>/dev/null | grep -c 'chain ')"
            check 10.1 ok "daemon nftables table" "table inet ${NFT_TABLE} present, ${NCHAINS} chain(s)"
        else
            check 10.1 -- "daemon nftables table" "table inet ${NFT_TABLE} is not installed (no tunnel, or open)"
        fi
    else
        check 10.1 n/a "daemon nftables table" "nft is absent"
    fi

    if [ "${SELINUX_ON}" = 1 ]; then
        check 10.2 -- "SELinux policy module" "answered as 4.5 above -- semodule -l ran in this pass"
    else
        check 10.2 n/a "SELinux policy module" "SELinux is not active"
    fi

    if have systemctl; then
        SVC_ACTIVE="$(systemctl is-active "${UNIT}" 2>/dev/null)"
        SVC_ENABLED="$(systemctl is-enabled "${UNIT}" 2>/dev/null)"
        case "${SVC_ACTIVE}" in
            active)  check 10.3 ok "service state" "${UNIT} is active, ${SVC_ENABLED:-unknown}" ;;
            inactive) check 10.3 -- "service state" "${UNIT} is inactive, ${SVC_ENABLED:-unknown}" ;;
            failed)  check 10.3 BLOCKER "service state" "${UNIT} has FAILED (${SVC_ENABLED:-unknown})"
                     note "Read 10.4. A failed unit may still be holding a fail-closed ruleset:"
                     note "  sudo ${DAEMON:-urnetworkd} --revert" ;;
            '')      check 10.3 -- "service state" "${UNIT} is not installed" ;;
            *)       check 10.3 -- "service state" "${SVC_ACTIVE} (${SVC_ENABLED:-unknown})" ;;
        esac
    else
        check 10.3 n/a "service state" "systemctl is absent"
    fi

    AVCS=''
    if have ausearch; then
        AVCS="$(ausearch -m avc,user_avc -ts recent 2>/dev/null | grep -c 'denied')"
    elif have journalctl; then
        AVCS="$(journalctl -k --no-pager -n 3000 2>/dev/null | grep -ci 'avc:  *denied\|apparmor.*DENIED')"
    fi
    if [ -n "${AVCS}" ] && [ "${AVCS}" != 0 ]; then
        check 10.4 WARN "recent MAC denials" "${AVCS} denial(s) in the recent kernel log"
        note "Not all of them are ours. Narrow it down with:"
        note "  ausearch -m avc -ts recent | grep -i urnetwork"
        note "  journalctl -k | grep -iE 'denied.*(tun|urnetwork|init_t)'"
    else
        check 10.4 ok "recent MAC denials" "none found in the recent kernel log"
    fi

    if [ -n "${DAEMON}" ]; then
        say ""
        say "  10.5  urnetworkd --selftest-egress -- the only check here that MEASURES"
        say "        anything instead of inspecting it. Its own output follows verbatim:"
        say ""
        "${DAEMON}" --selftest-egress 2>&1 | sed 's/^/  | /'
        ST=${PIPESTATUS[0]}
        say ""
        case "${ST}" in
            0) check 10.5 ok "egress marker self-test" "the mechanism WORKS on this kernel (exit 0)" ;;
            1) check 10.5 BLOCKER "egress marker self-test" "the mechanism DOES NOT WORK here (exit 1)"
               note "Do not run a tunnel on this host. The daemon's own packets would go into"
               note "the daemon's own tunnel, which is the failure that moved 3.38 Tb in forty"
               note "minutes the last time it happened. The verdict block above names the step." ;;
            2) check 10.5 n/a "egress marker self-test" "NO ANSWER (exit 2): the test could not run"
               note "Nothing was proven either way -- read the step that failed above." ;;
            *) check 10.5 n/a "egress marker self-test" "unexpected exit ${ST}" ;;
        esac
    else
        check 10.5 n/a "egress marker self-test" "no urnetworkd binary on this host to run it"
        note "This is THE check for a new distro. Install the daemon first, then re-run."
    fi
fi

# ==========================================================================
# 11. --tunnel: verify a live tunnel, leg by leg. Never creates one.
# ==========================================================================
if [ "${TUNNEL}" = 1 ] && [ "${IS_ROOT}" = 1 ]; then
    head2 "11. a live tunnel, verified leg by leg"
    say "  This script does NOT bring the tunnel up and will not pretend to:"
    say "  start_tunnel requires the device pairing id and the mTLS triple that only"
    say "  the GUI holds (ControlServer.cpp, ValidateStartTunnelRequest). Forging them"
    say "  would fake the exact thing you came here to measure."
    say ""

    if [ "${TUN_UP}" != 1 ]; then
        say "  Connect in the URnetwork app now. Waiting up to ${TUNNEL_WAIT}s for ${TUN_NAME}..."
        WAITED=0
        while [ "${WAITED}" -lt "${TUNNEL_WAIT}" ]; do
            if [ -n "${IP_BIN}" ] && "${IP_BIN}" -o link show "${TUN_NAME}" >/dev/null 2>&1; then
                TUN_UP=1; break
            fi
            sleep 2; WAITED=$((WAITED + 2))
        done
        say ""
    fi

    if [ "${TUN_UP}" != 1 ]; then
        check 11.1 WARN "tunnel device" "${TUN_NAME} never appeared within ${TUNNEL_WAIT}s"
        note "Nothing was verified. If the app said it connected, the bring-up failed"
        note "after the daemon accepted it: sudo journalctl -u ${UNIT} -n 200"
    else
        TUNADDR="$("${IP_BIN}" -4 -o addr show dev "${TUN_NAME}" 2>/dev/null | awk '{print $4}' | head -1)"
        if [ -n "${TUNADDR}" ]; then
            check 11.1 ok "tunnel device" "${TUN_NAME} up with ${TUNADDR}"
        else
            check 11.1 BLOCKER "tunnel device" "${TUN_NAME} exists but carries no IPv4 address"
        fi

        if "${IP_BIN}" rule show 2>/dev/null | grep -q "lookup ${ROUTE_TABLE}"; then
            RULE="$("${IP_BIN}" rule show 2>/dev/null | grep "lookup ${ROUTE_TABLE}" | head -1)"
            check 11.2 ok "policy rule" "${RULE}"
        else
            check 11.2 BLOCKER "policy rule" "no rule points at table ${ROUTE_TABLE}"
            note "Without it nothing is steered into the tunnel: the tun exists and carries nothing."
        fi

        if "${IP_BIN}" route show table "${ROUTE_TABLE}" 2>/dev/null | grep -q .; then
            NROUTES="$("${IP_BIN}" route show table "${ROUTE_TABLE}" 2>/dev/null | grep -c .)"
            check 11.3 ok "capture routes" "${NROUTES} route(s) in table ${ROUTE_TABLE}"
        else
            check 11.3 BLOCKER "capture routes" "table ${ROUTE_TABLE} is empty"
        fi

        if [ -n "${NFT_BIN}" ] && "${NFT_BIN}" list table inet "${NFT_TABLE}" >/dev/null 2>&1; then
            MARKRULES="$("${NFT_BIN}" list table inet "${NFT_TABLE}" 2>/dev/null | grep -c "${MARK_HEX}\|mark")"
            check 11.4 ok "kill switch / egress table" "table inet ${NFT_TABLE} live, ${MARKRULES} mark rule(s)"
        else
            check 11.4 BLOCKER "kill switch / egress table" "table inet ${NFT_TABLE} is NOT installed while a tunnel is up"
            note "This is the unprotected state: no IPv6 floor, no DNS floor, no self-exclusion."
        fi

        if have resolvectl; then
            DNSLINE="$(resolvectl status "${TUN_NAME}" 2>/dev/null | grep -i 'DNS Servers' | head -1 | sed 's/^ *//')"
            if [ -n "${DNSLINE}" ]; then
                check 11.5 ok "DNS pinned to the tunnel" "${DNSLINE}"
            else
                check 11.5 WARN "DNS pinned to the tunnel" "resolvectl reports no DNS on ${TUN_NAME}"
                note "The session is up with dns_applied=false: queries are leaving outside the tunnel."
            fi
        else
            check 11.5 WARN "DNS pinned to the tunnel" "resolvectl absent -- DNS is not pinned on this distro"
        fi

        if [ -n "${DPID}" ] && have ss; then
            OWN="$(ss -tanp 2>/dev/null | grep -c "pid=${DPID}")"
            check 11.6 -- "daemon sockets" "${OWN} TCP socket(s) attributed to pid ${DPID}"
            note "Whether they carry ${MARK_HEX} is what 10.5 proved for this kernel;"
            note "packaging/diagnose-egress.sh reads it off the live sockets."
        fi

        say ""
        say "  THE ONE THING THIS SCRIPT WILL NOT DO FOR YOU: confirm that traffic is"
        say "  actually carried. Every mode of this script is packet-free on purpose."
        say "  Ask the network yourself, before and after connecting:"
        say "      curl -s https://api.ipify.org; echo"
    fi
elif [ "${TUNNEL}" = 1 ]; then
    head2 "11. a live tunnel -- REFUSED"
    check 11.0 needs-root "tunnel verification" "--tunnel needs root to read the ruleset and routes"
fi

# ==========================================================================
head2 "verdict"
# ==========================================================================
say "checks: ${N_OK} ok, ${N_WARN} warning(s), ${N_BLOCK} blocker(s), ${N_NA} not measurable, ${N_ROOT} needing root"
say ""

if [ "${N_BLOCK}" -gt 0 ]; then
    say "BLOCKERS -- URnetwork cannot work on this host until these change:"
    for b in "${BLOCKERS[@]}"; do say "  * ${b}"; done
    say ""
fi
if [ "${N_WARN}" -gt 0 ]; then
    say "WARNINGS -- it will run, and each of these costs you something specific:"
    for w in "${WARNINGS[@]}"; do say "  * ${w}"; done
    say ""
fi
if [ "${N_ROOT}" -gt 0 ]; then
    say "NOT MEASURED (needs root) -- named, not silently dropped:"
    for r in "${ROOTSKIPS[@]}"; do say "  * ${r}"; done
    say ""
fi

# The single most useful line on a fresh machine: what do I actually install.
say "INSTALL CHANNEL FOR THIS HOST:"
if [ "${LAYOUT}" = 'immutable' ]; then
    say "  the install tarball -- it is the only channel that handles a read-only /usr"
    say "    curl -fsSL https://get.ur.network/urnetwork-daemon.tar.gz | tar xz \\"
    say "      && sudo urnetwork-daemon/install.sh"
elif [ "${PKG_CHANNEL}" = 'deb' ]; then
    say "  urnetwork-daemon_<version>_${UR_ARCH:-amd64}.deb   (sudo apt install ./…deb)"
    say "  it declares libc6 >= ${GLIBC_FLOOR}, iproute2 and libfuse2 for you"
elif [ "${PKG_CHANNEL}" = 'rpm' ]; then
    say "  urnetwork-daemon-<version>.$(uname -m).rpm if your release publishes one"
    say "  (sudo dnf install ./…rpm); otherwise the install tarball, which covers"
    say "  every rpm-family host including the immutable ones:"
    say "    curl -fsSL https://get.ur.network/urnetwork-daemon.tar.gz | tar xz \\"
    say "      && sudo urnetwork-daemon/install.sh"
else
    say "  the install tarball (no supported native package for this distro yet)"
fi
if [ "${SELINUX_ON}" = 1 ]; then
    say "  on this SELinux host the installer also builds and loads packaging/selinux/urnetwork.te"
    say "  -- without that module the daemon starts and the tunnel cannot open /dev/net/tun"
fi
say ""

RC=0
if [ "${N_BLOCK}" -gt 0 ]; then
    RC=1
    say "VERDICT: BLOCKED on this host. ${N_BLOCK} blocker(s) above, each named with the fix."
elif [ "${PRIVILEGED}" = 1 ] && [ "${IS_ROOT}" = 1 ] && [ -n "${DAEMON}" ]; then
    say "VERDICT: nothing on this host blocks URnetwork, AND the egress marker was"
    say "  MEASURED here rather than assumed (10.5). That is as strong as this script gets."
else
    say "VERDICT: nothing measured here blocks URnetwork on this host."
    say ""
    say "  Read that precisely. It is a PREFLIGHT, not a proof: every check above"
    say "  inspects the host, and not one of them has loaded a BPF program, opened a"
    say "  tun device or moved a packet. The claim 'it should work' is exactly what"
    say "  --selftest-egress exists to replace. To turn this into a measurement:"
    say "      sudo bash ${SELF} --privileged"
fi
say ""
say "Nothing on this machine was changed by this run."
if [ "${TUNNEL}" = 1 ]; then
    say "No tunnel was started or stopped: --tunnel only verifies one you brought up."
fi

if [ "${JSON}" = 1 ]; then
    say ""
    say "--- json ---"
    printf '{"host":{"id":"%s","version":"%s","family":"%s","arch":"%s","kernel":"%s",' \
        "$(json_escape "${OS_ID}")" "$(json_escape "${OS_VER}")" \
        "$(json_escape "${FAMILY}")" "$(json_escape "${KARCH}")" "$(json_escape "${KREL}")"
    printf '"layout":"%s","package_channel":"%s","selinux":"%s","apparmor":%s,"init":"%s"},' \
        "$(json_escape "${LAYOUT}")" "$(json_escape "${PKG_CHANNEL}")" \
        "$(json_escape "${SEMODE:-none}")" "$([ "${AA_ON}" = 1 ] && echo true || echo false)" \
        "$(json_escape "${INIT_COMM}")"
    printf '"summary":{"ok":%d,"warn":%d,"blocker":%d,"not_measurable":%d,"needs_root":%d},' \
        "${N_OK}" "${N_WARN}" "${N_BLOCK}" "${N_NA}" "${N_ROOT}"
    printf '"checks":['
    for i in "${!JSON_ROWS[@]}"; do
        [ "${i}" -gt 0 ] && printf ','
        printf '%s' "${JSON_ROWS[$i]}"
    done
    printf ']}\n'
fi

exit "${RC}"
