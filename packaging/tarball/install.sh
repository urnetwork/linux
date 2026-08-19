#!/bin/bash
# URnetwork daemon installer -- the native (non-apt) path, APPIMAGE.md 11g.
#
#   curl -fsSL https://get.ur.network/urnetwork-daemon.tar.gz | tar xz \
#       && sudo urnetwork-daemon/install.sh
#
# The SAME command installs and upgrades: the script detects an existing
# install, compares versions and does the right thing -- there is no
# --upgrade flag. Upgrades preserve /var/lib state, /etc configuration and
# the urnetwork system group; only binaries, units and integration files are
# replaced, and the previous install is restored if anything fails while the
# daemon is stopped.
#
# It REFUSES to run where dpkg, rpm or pacman already owns these paths: two
# package managers owning the same files is the worst failure available here,
# and it is silent until an upgrade half-replaces them. Use the distribution's
# package manager there.
#
# Unlike a .deb, nothing refreshes the desktop caches for us (dpkg file
# triggers fire only for files dpkg installs), so this script runs
# update-desktop-database and gtk-update-icon-cache itself -- the failure it
# prevents is invisible on a dev box and breaks only urnetwork:// SSO/deep
# links in the field (APPIMAGE.md section 5 callout).
#
# Options:
#   --dry-run       print every action without touching anything (safe on
#                   any OS, including macOS build hosts)
#   --prefix <dir>  root the file layout under <dir> for testing; skips all
#                   system mutation (group, systemd, caches)
#   --layout <l>    'standard' (/usr + /lib/systemd) or 'immutable'
#                   (/usr/local + /etc/systemd, for ostree/bootc hosts like
#                   Fedora Silverblue, Bazzite, Kinoite and MicroOS).
#                   DETECTED automatically; this only overrides the detection.
#   --update        fetch the current tarball from get.ur.network and run its
#                   installer (the opt-in update channel where there is no apt)
#   --force         override downgrade/preflight refusals
#   --yes           assume yes on prompts
#   --skip-selftest do not run the post-install egress self-test. That test
#                   starts no tunnel and sends no packet, but it does load a
#                   BPF program -- skip it where that is not permitted.
#
# This script is standalone on purpose (no sourced libraries): it ships inside
# the tarball next to its payload, so it can never run against a payload it
# was not built with.
set -Eeuo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAYLOAD_DIR="${SELF_DIR}/payload"

PKG_NAME='urnetwork-daemon'
UNIT='urnetworkd.service'
GLIBC_FLOOR='2.35'      # SDK is cross-built against glibc 2.35 (jammy)
GEOCLUE_FLOOR='2.7.0'   # static-source location override needs >= 2.7.0
DEFAULT_URL_BASE='https://get.ur.network'

# ---------------------------------------------------------------------------
# Immutable / image-based hosts (ostree: Fedora Silverblue, Bazzite, Kinoite;
# bootc; openSUSE MicroOS)
# ---------------------------------------------------------------------------
# On these systems /usr is a READ-ONLY mount owned by the image, and writing
# into it either fails outright or is silently discarded at the next rebase.
# The supported writable locations are /usr/local (an ostree symlink to
# /var/usrlocal) and /etc — so the payload's paths are REMAPPED:
#
#     /usr/bin/...              -> /usr/local/bin/...
#     /usr/lib/urnetwork/...    -> /usr/local/lib/urnetwork/...
#     /usr/share/...            -> /usr/local/share/...
#     /lib/systemd/system/...   -> /etc/systemd/system/...     (admin unit dir)
#     /etc/...                  -> unchanged (already writable)
#
# and the unit's ExecStart is rewritten to match. Both destinations are in the
# default PATH / XDG_DATA_DIRS / systemd unit load path on every one of these
# distros, so nothing else has to know.
#
# This is detection, not a flag: a user who follows the documented one-liner on
# Bazzite must not have to know their /usr is read-only. --layout forces it
# either way for testing.
LAYOUT=''   # '', 'standard' or 'immutable' (empty = detect)

detect_layout() {
    [ -n "${LAYOUT}" ] && { printf '%s' "${LAYOUT}"; return 0; }
    # ostree booted systems say so; bootc/MicroOS are caught by the mount test.
    if [ -f /run/ostree-booted ]; then printf 'immutable'; return 0; fi
    if command -v findmnt >/dev/null 2>&1; then
        case "$(findmnt -no OPTIONS --target /usr 2>/dev/null)" in
            ro,*|*,ro|*,ro,*|ro) printf 'immutable'; return 0 ;;
        esac
    fi
    # Last resort: ask the filesystem directly. A writable /usr on a normal
    # distro answers instantly; the temp file is removed either way.
    if ! (mkdir -p /usr/lib/urnetwork 2>/dev/null &&
          touch /usr/lib/urnetwork/.urnetwork-write-test 2>/dev/null); then
        rm -f /usr/lib/urnetwork/.urnetwork-write-test 2>/dev/null || true
        printf 'immutable'; return 0
    fi
    rm -f /usr/lib/urnetwork/.urnetwork-write-test 2>/dev/null || true
    printf 'standard'
}

# map_path <payload-relative path> -> install path
map_path() {
    if [ "${LAYOUT}" != 'immutable' ]; then
        # Standard layout: identity, EXCEPT that the unit follows the host's
        # real system-unit directory (see systemd_unit_dir -- /lib is a symlink
        # to usr/lib on Arch, Debian, Ubuntu and Fedora alike).
        case "$1" in
            /lib/systemd/system)   printf '%s' "${SYSTEMD_UNIT_DIR}" ;;
            /lib/systemd/system/*) printf '%s/%s' "${SYSTEMD_UNIT_DIR}" "${1#/lib/systemd/system/}" ;;
            *)                     printf '%s' "$1" ;;
        esac
        return 0
    fi
    case "$1" in
        # The bare directories are asked for by name too (LIB_DIR, the cache
        # refresh targets), and they do NOT match the /* patterns below.
        /usr/bin)               printf '/usr/local/bin' ;;
        /usr/lib/urnetwork)     printf '/usr/local/lib/urnetwork' ;;
        /usr/share)             printf '/usr/local/share' ;;
        /usr/bin/*)             printf '/usr/local/bin/%s'   "${1#/usr/bin/}" ;;
        /usr/lib/urnetwork/*)   printf '/usr/local/lib/urnetwork/%s' "${1#/usr/lib/urnetwork/}" ;;
        /usr/share/*)           printf '/usr/local/share/%s' "${1#/usr/share/}" ;;
        /lib/systemd/system/*)     printf '/etc/systemd/system/%s' "${1#/lib/systemd/system/}" ;;
        /usr/lib/systemd/system/*) printf '/etc/systemd/system/%s' "${1#/usr/lib/systemd/system/}" ;;
        *)                      printf '%s' "$1" ;;
    esac
}

# Installed bookkeeping (under $PREFIX). LIB_DIR is remapped after the layout
# is known — the uninstaller has to live where it was actually installed.
LIB_DIR='/usr/lib/urnetwork'
MANIFEST_REL=''
VERSION_REL=''

DRY_RUN=0
PREFIX=''
FORCE=0
ASSUME_YES=0
DO_UPDATE=0
GROUP_ADDED=0
SKIP_SELFTEST=0
# Accumulated across preflight so the last thing on screen can be ONE install
# command instead of five scattered warnings the tester has to reassemble.
MISSING_PKGS=''
# Set when DNS cannot go through the tunnel on this host. Reprinted at the very
# end: a privacy defect that scrolls off the top of a successful install has
# not been reported.
DNS_WARNING=''

# ---------------------------------------------------------------------------
# polkit: the authorizer that replaced "join a group and log out"
# ---------------------------------------------------------------------------
# urnetworkd decides which authority it runs under from an INSTALL FACT -- the
# presence of our action file at ControlProtocol.hpp's kPolkitPolicyPath --
# never from whether polkitd answered a call. Keying on the answer would make
# `kill polkitd` a policy-downgrade attack; an install fact cannot be induced
# at runtime by an unprivileged process.
#
# That makes this file's presence a PROMISE the installer is making on the
# daemon's behalf: "a polkit check on this machine can be completed". A daemon
# that believes it and finds otherwise fails CLOSED, so the promise must not be
# made on a machine with no polkit -- there, the file is withheld and the
# legacy `urnetwork` group check stays in force.
POLKIT_PRESENT=0
POLKIT_VERSION=''
POLICY_REL='/usr/share/polkit-1/actions/network.ur.urnetwork.policy'

log()  { printf '%s\n' "$*"; }
note() { printf -- '- %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
    # Print the header comment block, whatever length it has grown to: a fixed
    # line range silently truncates --help the first time an option is added.
    sed -n '2,/^set -Eeuo/p' "${BASH_SOURCE[0]}" | sed -e '$d' -e 's/^# \{0,1\}//'
}

# Every mutating command goes through run(); --dry-run prints instead.
run() {
    if [ "${DRY_RUN}" = 1 ]; then
        printf 'would run: %s\n' "$*"
    else
        "$@"
    fi
}

confirm() {
    # confirm <prompt> -> 0 yes / 1 no. Non-interactive default is NO.
    if [ "${ASSUME_YES}" = 1 ]; then return 0; fi
    if [ ! -t 0 ]; then return 1; fi
    printf '%s [y/N] ' "$1"
    local reply
    read -r reply
    case "${reply}" in
        y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

# ver_ge A B -> 0 if A >= B (needs sort -V; probed because this can run on
# macOS in --dry-run where BSD sort may lack it)
HAVE_SORT_V=0
if printf '1\n2\n' | sort -V >/dev/null 2>&1; then HAVE_SORT_V=1; fi
ver_ge() {
    [ "${HAVE_SORT_V}" = 1 ] || return 0   # cannot compare: do not block
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n1)" = "$1" ]
}

# detect_polkit -> 0 if this machine has polkit itself.
#
# LOOKS FOR POLKIT, NEVER FOR OUR OWN ACTION FILE. `install -D` creates
# /usr/share/polkit-1/actions on the way in, so after one run both that
# directory and our .policy exist on a box that has never had polkit -- testing
# either would latch polkit mode on a machine where no check can ever be
# answered, and under the daemon's fail-closed rule that machine could never
# connect again. The three probes below are, in order: the CLI tools every
# packaging of polkit ships, the daemon binary itself (libexecdir varies by
# distro), and polkit's own action file, which it has shipped since 0.105.
detect_polkit() {
    if command -v pkaction >/dev/null 2>&1; then return 0; fi
    if command -v pkcheck >/dev/null 2>&1; then return 0; fi
    local d
    for d in /usr/lib/polkit-1 /usr/libexec/polkit-1 /usr/lib64/polkit-1 \
             /usr/lib/x86_64-linux-gnu/polkit-1 /usr/lib/aarch64-linux-gnu/polkit-1 \
             /usr/local/lib/polkit-1 /usr/local/libexec/polkit-1; do
        if [ -x "${d}/polkitd" ]; then return 0; fi
    done
    if [ -f /usr/share/polkit-1/actions/org.freedesktop.policykit.policy ]; then return 0; fi
    if [ -f /usr/local/share/polkit-1/actions/org.freedesktop.policykit.policy ]; then return 0; fi
    return 1
}

elf_arch() {
    # ELF e_machine (offset 0x12, LE) -> amd64 | arm64 | unknown
    local magic machine
    magic="$(od -An -t x1 -j 0 -N 4 "$1" 2>/dev/null | tr -d ' \n')"
    [ "${magic}" = "7f454c46" ] || { printf 'unknown'; return 0; }
    machine="$(od -An -t x1 -j 18 -N 2 "$1" 2>/dev/null | tr -d ' \n')"
    case "${machine}" in
        3e00) printf 'amd64' ;;
        b700) printf 'arm64' ;;
        *)    printf 'unknown' ;;
    esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --prefix)  PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
        --layout)  LAYOUT="${2:?--layout needs standard|immutable}"
                   case "${LAYOUT}" in standard|immutable) ;; *) die "--layout must be 'standard' or 'immutable'" ;; esac
                   shift 2 ;;
        --update)  DO_UPDATE=1; shift ;;
        --skip-selftest) SKIP_SELFTEST=1; shift ;;
        --force)   FORCE=1; shift ;;
        --yes|-y)  ASSUME_YES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
done

# ---------------------------------------------------------------------------
# Host facts (read-only; safe everywhere including macOS --dry-run)
# ---------------------------------------------------------------------------
HOST_OS="$(uname -s)"
HOST_ARCH_RAW="$(uname -m)"
case "${HOST_ARCH_RAW}" in
    x86_64|amd64)  HOST_ARCH='amd64' ;;
    aarch64|arm64) HOST_ARCH='arm64' ;;
    *)             HOST_ARCH='unsupported' ;;
esac

# ---------------------------------------------------------------------------
# Distribution identity and package manager
# ---------------------------------------------------------------------------
# A tarball has no dependency mechanism, so every "X is missing" message this
# script prints IS the dependency declaration -- and "install nftables" is
# useless advice if the reader's distribution calls the package something else
# or their package manager is not the one the message assumed. Resolve both
# once, here, and name the exact package everywhere below.
#
# /etc/os-release is shell syntax by spec, but this script runs as root and
# reading three fields does not justify sourcing a file to find that out.
os_release_field() {
    [ -r /etc/os-release ] || return 0
    sed -n "s/^$1=//p" /etc/os-release 2>/dev/null | head -n1 | tr -d "\"'"
}
DISTRO_ID="$(os_release_field ID)"
DISTRO_LIKE="$(os_release_field ID_LIKE)"
DISTRO_PRETTY="$(os_release_field PRETTY_NAME)"

# Probe order is not alphabetical. rpm-ostree comes BEFORE dnf because an
# ostree host (Bazzite, Silverblue, Kinoite) ships dnf inside the image where
# it appears to work and does not persist across a rebase; pacman comes first
# because no Arch host has any of the others.
PKG_MGR=''
if   command -v pacman     >/dev/null 2>&1; then PKG_MGR='pacman'
elif command -v apt-get    >/dev/null 2>&1; then PKG_MGR='apt'
elif command -v rpm-ostree >/dev/null 2>&1; then PKG_MGR='rpm-ostree'
elif command -v dnf        >/dev/null 2>&1; then PKG_MGR='dnf'
elif command -v zypper     >/dev/null 2>&1; then PKG_MGR='zypper'
fi
case "${PKG_MGR}" in
    pacman)        PKG_FAMILY='arch' ;;
    apt)           PKG_FAMILY='debian' ;;
    dnf|rpm-ostree) PKG_FAMILY='fedora' ;;
    zypper)        PKG_FAMILY='suse' ;;
    *)             PKG_FAMILY='' ;;
esac
# No package manager on PATH (a stripped container, a rescue shell, a broken
# PATH) does not mean we do not know what this machine is. Fall back to what
# the distribution says it is, so the package NAMES stay correct even though
# no install command can be offered. ID_LIKE is space-separated and CachyOS,
# EndeavourOS and Manjaro all declare ID_LIKE=arch.
if [ -z "${PKG_FAMILY}" ]; then
    case " ${DISTRO_ID} ${DISTRO_LIKE} " in
        *" arch "*|*" cachyos "*|*" manjaro "*|*" endeavouros "*) PKG_FAMILY='arch' ;;
        *" debian "*|*" ubuntu "*)                                PKG_FAMILY='debian' ;;
        *" fedora "*|*" rhel "*|*" centos "*)                     PKG_FAMILY='fedora' ;;
        *" suse "*|*" opensuse "*)                                PKG_FAMILY='suse' ;;
    esac
fi

# pkg_for <tool> -> the package THIS host installs <tool> from, or '' when we
# do not know. Verified names, not guesses; where a family disagrees with the
# others it gets its own line rather than a shared default.
pkg_for() {
    case "${PKG_FAMILY}:$1" in
        arch:ip|debian:ip|suse:ip)                        printf 'iproute2' ;;
        fedora:ip)                                        printf 'iproute' ;;
        arch:nft|debian:nft|fedora:nft|suse:nft)          printf 'nftables' ;;
        # resolvectl is in the base systemd package everywhere; it is the
        # systemd-resolved SERVICE that Debian and Fedora split out, and that
        # is a separate message (see the DNS preflight).
        arch:resolvectl|debian:resolvectl|fedora:resolvectl|suse:resolvectl) printf 'systemd' ;;
        arch:modprobe|debian:modprobe|fedora:modprobe|suse:modprobe) printf 'kmod' ;;
        arch:update-desktop-database|debian:update-desktop-database|fedora:update-desktop-database|suse:update-desktop-database)
            printf 'desktop-file-utils' ;;
        arch:gtk-update-icon-cache|debian:gtk-update-icon-cache|fedora:gtk-update-icon-cache)
            printf 'gtk-update-icon-cache' ;;
        suse:gtk-update-icon-cache)                       printf 'gtk3-tools' ;;
        arch:groupadd|arch:usermod|arch:gpasswd|suse:groupadd|suse:usermod|suse:gpasswd)
            printf 'shadow' ;;
        debian:groupadd|debian:usermod|debian:gpasswd)    printf 'passwd' ;;
        fedora:groupadd|fedora:usermod|fedora:gpasswd)    printf 'shadow-utils' ;;
        arch:curl|debian:curl|fedora:curl|suse:curl)      printf 'curl' ;;
        *) printf '' ;;
    esac
}

# pkg_install_cmd <package>... -> the literal command line to paste.
pkg_install_cmd() {
    case "${PKG_MGR}" in
        pacman)     printf 'sudo pacman -S --needed %s' "$*" ;;
        apt)        printf 'sudo apt install %s' "$*" ;;
        dnf)        printf 'sudo dnf install %s' "$*" ;;
        zypper)     printf 'sudo zypper install %s' "$*" ;;
        rpm-ostree) printf 'sudo rpm-ostree install %s   (then reboot)' "$*" ;;
        *)          printf 'install: %s' "$*" ;;
    esac
}

# need_pkg <tool> -> the per-item remedy, for interpolation into a message.
# PURE: it only prints. The recording half is `want` below, and the two are
# separate for a reason -- every use of this is inside $(...), which runs in a
# subshell, so anything it assigned to MISSING_PKGS would be thrown away the
# moment it returned. Falls back to naming the tool when the map has no entry,
# so an unrecognised distribution still gets something actionable.
need_pkg() {
    local pkg
    pkg="$(pkg_for "$1")"
    if [ -n "${pkg}" ]; then
        printf 'install it with: %s' "$(pkg_install_cmd "${pkg}")"
    else
        printf 'install the package that provides %s' "$1"
    fi
}

# want <tool> -- record what provides <tool> so the end of preflight can print
# ONE install command instead of five scattered ones. Must be called from the
# script's own shell, never from inside a command substitution.
want() {
    local pkg
    pkg="$(pkg_for "$1")"
    [ -n "${pkg}" ] && MISSING_PKGS="${MISSING_PKGS} ${pkg}"
    return 0
}

# have_tool <tool> -- the daemon looks in PATH and then /usr/sbin:/sbin:/usr/bin
# :/bin (Tunnel.cpp FindTool). Ask the same question the daemon will ask, or
# this script reports a tool as missing that the daemon finds, or worse.
have_tool() {
    command -v "$1" >/dev/null 2>&1 && return 0
    local d
    for d in /usr/sbin /sbin /usr/bin /bin; do
        [ -x "$d/$1" ] && return 0
    done
    return 1
}
tool_path() {
    command -v "$1" 2>/dev/null && return 0
    local d
    for d in /usr/sbin /sbin /usr/bin /bin; do
        [ -x "$d/$1" ] && { printf '%s/%s' "$d" "$1"; return 0; }
    done
    return 1
}

# ---------------------------------------------------------------------------
# Where the systemd unit actually lands
# ---------------------------------------------------------------------------
# The payload ships the unit at /lib/systemd/system/urnetworkd.service. On
# EVERY usr-merged distribution -- Arch and CachyOS, Debian >= 12, Ubuntu >=
# 20.04, Fedora -- /lib is a SYMLINK to usr/lib, so `install` writes the file
# to /usr/lib/systemd/system and a manifest that records /lib/... describes a
# path that only resolves for as long as that symlink exists. It also makes
# `pacman -Qo` (and any other file-ownership query) disagree with our own
# bookkeeping about which file we own. Record where the file really is.
systemd_unit_dir() {
    local d
    if command -v pkg-config >/dev/null 2>&1; then
        d="$(pkg-config --variable=systemdsystemunitdir systemd 2>/dev/null)" || d=''
        case "${d}" in /*) printf '%s' "${d}"; return 0 ;; esac
    fi
    if [ -L /lib ]; then printf '/usr/lib/systemd/system'; return 0; fi
    printf '/lib/systemd/system'
}
SYSTEMD_UNIT_DIR="$(systemd_unit_dir)"

# selinux_active -- is there a policy to install a module into RIGHT NOW?
#
# `command -v getenforce && [ "$(getenforce)" != Disabled ]` is a proxy for
# that question, not the question. It happens to be right on a stock Arch or
# CachyOS box (no libselinux, so the block skips), but it FAILS OPEN in the one
# direction that matters: the comparison is true whenever getenforce writes
# nothing to stdout -- it errored, it is a stub, libselinux is half-installed
# -- and the block then tells a user with no SELinux at all that "the tunnel
# will not be able to open /dev/net/tun". Ask the kernel instead. selinuxfs is
# mounted, and /sys/fs/selinux/enforce therefore exists, only when SELinux is
# actually enabled; getenforce is then consulted only to separate
# Enforcing/Permissive from a disabled policy.
selinux_active() {
    [ -e /sys/fs/selinux/enforce ] || return 1   # selinuxfs mounted == enabled
    case "$(getenforce 2>/dev/null || true)" in
        Enforcing|Permissive) return 0 ;;
        *) return 1 ;;
    esac
}

# --update: re-fetch the published tarball and hand over to ITS installer.
# Opt-in only -- nothing ever auto-upgrades a daemon that may hold a live
# tunnel without the user asking (APPIMAGE.md 11g).
if [ "${DO_UPDATE}" = 1 ]; then
    [ "${HOST_ARCH}" != 'unsupported' ] || die "unsupported architecture '${HOST_ARCH_RAW}' -- URnetwork ships amd64 and arm64 only"
    # The documented one-liner URL (urnetwork-daemon.tar.gz) is served
    # per-arch by the download host; this client fetches the explicit
    # per-arch alias. Override the full URL with UR_TARBALL_URL.
    UPDATE_URL="${UR_TARBALL_URL:-${DEFAULT_URL_BASE}/urnetwork-daemon-${HOST_ARCH}.tar.gz}"
    if [ "${DRY_RUN}" = 1 ]; then
        log "would fetch ${UPDATE_URL}, extract to a temp dir, and run its install.sh"
        exit 0
    fi
    command -v curl >/dev/null 2>&1 || die "--update needs curl"
    UPDATE_TMP="$(mktemp -d /tmp/urnetwork-update.XXXXXX)"
    log "fetching ${UPDATE_URL} ..."
    curl -fsSL "${UPDATE_URL}" | tar xz -C "${UPDATE_TMP}"
    [ -f "${UPDATE_TMP}/urnetwork-daemon/install.sh" ] || die "downloaded tarball has no urnetwork-daemon/install.sh"
    UPDATE_ARGS=()
    [ "${FORCE}" = 1 ] && UPDATE_ARGS[${#UPDATE_ARGS[@]}]='--force'
    [ "${ASSUME_YES}" = 1 ] && UPDATE_ARGS[${#UPDATE_ARGS[@]}]='--yes'
    exec bash "${UPDATE_TMP}/urnetwork-daemon/install.sh" ${UPDATE_ARGS[@]+"${UPDATE_ARGS[@]}"}
fi

# ---------------------------------------------------------------------------
# Payload facts
# ---------------------------------------------------------------------------
[ -d "${PAYLOAD_DIR}" ] || die "no payload/ next to install.sh -- extract the full tarball and run urnetwork-daemon/install.sh from it"
[ -f "${SELF_DIR}/VERSION" ] || die "no VERSION next to install.sh -- the tarball is incomplete"
NEW_VERSION="$(tr -d '[:space:]' < "${SELF_DIR}/VERSION")"
[ -n "${NEW_VERSION}" ] || die "VERSION file is empty"

DAEMON_REL='/usr/lib/urnetwork/urnetworkd'
[ -f "${PAYLOAD_DIR}${DAEMON_REL}" ] || die "payload is missing ${DAEMON_REL} -- the tarball is incomplete"
PAYLOAD_ARCH="$(elf_arch "${PAYLOAD_DIR}${DAEMON_REL}")"

log "urnetwork-daemon ${NEW_VERSION} installer (payload: ${PAYLOAD_ARCH})"
[ "${DRY_RUN}" = 1 ] && log "[dry-run] no changes will be made"

# Layout must be resolved BEFORE any path is formed: LIB_DIR, the manifest,
# the version marker and every install target depend on it. Under --prefix the
# tree is rooted somewhere writable already, so the standard layout applies.
if [ -n "${PREFIX}" ] && [ -z "${LAYOUT}" ]; then
    LAYOUT='standard'
else
    LAYOUT="$(detect_layout)"
fi
LIB_DIR="$(map_path '/usr/lib/urnetwork')"
MANIFEST_REL="${LIB_DIR}/.install-manifest"
VERSION_REL="${LIB_DIR}/.installed-version"
if [ "${LAYOUT}" = 'immutable' ]; then
    log "layout: immutable host (/usr is read-only) -- installing under /usr/local and /etc/systemd/system"
fi

# ---------------------------------------------------------------------------
# Gate: OS / arch / privileges
# ---------------------------------------------------------------------------
if [ "${HOST_OS}" != 'Linux' ]; then
    if [ "${DRY_RUN}" = 1 ]; then
        warn "[dry-run] host is ${HOST_OS}, not Linux -- printing the plan only; a real run would abort here"
    else
        die "urnetwork-daemon runs on Linux only (this host is ${HOST_OS})"
    fi
fi

if [ "${HOST_ARCH}" = 'unsupported' ]; then
    if [ "${DRY_RUN}" = 1 ]; then
        warn "[dry-run] unsupported architecture '${HOST_ARCH_RAW}' -- a real run would abort here"
    else
        die "unsupported architecture '${HOST_ARCH_RAW}' -- URnetwork ships amd64 and arm64 only"
    fi
elif [ "${PAYLOAD_ARCH}" != "${HOST_ARCH}" ] && [ "${HOST_OS}" = 'Linux' ]; then
    die "this tarball's payload is ${PAYLOAD_ARCH} but this machine is ${HOST_ARCH} -- download urnetwork-daemon-<version>-${HOST_ARCH}.install.tar.gz instead"
fi

if [ "${DRY_RUN}" = 0 ] && [ -z "${PREFIX}" ] && [ "$(id -u)" != 0 ]; then
    die "must run as root (sudo urnetwork-daemon/install.sh)"
fi

# ---------------------------------------------------------------------------
# Refuse to fight dpkg / rpm (APPIMAGE.md 11g)
# ---------------------------------------------------------------------------
if command -v dpkg >/dev/null 2>&1; then
    # dpkg -s exits 1 for an unknown package; that must not trip set -e.
    DPKG_STATUS="$( (dpkg -s "${PKG_NAME}" 2>/dev/null || true) | sed -n 's/^Status: //p')"
    case "${DPKG_STATUS}" in
        '') : ;;  # unknown to dpkg: fine
        *config-files*)
            die "dpkg still owns configuration for ${PKG_NAME} (removed but not purged).
Run:  sudo apt purge ${PKG_NAME}
then re-run this installer." ;;
        *installed*)
            die "${PKG_NAME} is installed and owned by dpkg/apt (Status: ${DPKG_STATUS}).
Refusing to overwrite a dpkg-owned install: two package managers owning the
same paths fails silently until an upgrade half-replaces files.
Upgrade with apt instead:
  sudo apt install ./urnetwork-daemon_<version>_${HOST_ARCH}.deb
or, to switch to this tarball channel:
  sudo apt purge ${PKG_NAME}   # then re-run this installer" ;;
        *) warn "dpkg reports unexpected status for ${PKG_NAME}: '${DPKG_STATUS}' -- continuing" ;;
    esac
fi
if command -v rpm >/dev/null 2>&1 && rpm -q "${PKG_NAME}" >/dev/null 2>&1; then
    die "${PKG_NAME} is installed and owned by rpm -- use dnf/zypper to upgrade or remove it first"
fi
# pacman (Arch, CachyOS, Manjaro, EndeavourOS). There is no URnetwork package
# in the Arch repositories or the AUR today, so this is a guard against a
# FUTURE one rather than a live conflict -- and the day one lands, two owners
# of the same paths is exactly the silent-corruption failure dpkg is refused
# for above.
#
# Queried by PATH, not by name. An AUR package could be called urnetwork,
# urnetwork-bin, urnetwork-daemon or urnetwork-git, and a name check would miss
# all but one; the path is the thing that actually collides. `pacman -Qo` exits
# non-zero and writes to stderr both when nothing owns the file and when the
# file does not exist, so both are absorbed.
if command -v pacman >/dev/null 2>&1; then
    PACMAN_OWNER=''
    PACMAN_OWNED_PATH=''
    for _p in /usr/lib/urnetwork/urnetworkd /usr/bin/urnetwork \
              "${SYSTEMD_UNIT_DIR}/${UNIT}" /lib/systemd/system/"${UNIT}"; do
        _owner="$( (pacman -Qoq "${_p}" 2>/dev/null || true) | head -n1 )"
        if [ -n "${_owner}" ]; then
            PACMAN_OWNER="${_owner}"; PACMAN_OWNED_PATH="${_p}"; break
        fi
    done
    if [ -n "${PACMAN_OWNER}" ]; then
        die "the pacman package '${PACMAN_OWNER}' already owns ${PACMAN_OWNED_PATH}.
Refusing to overwrite it: pacman would not know these files changed, and the
next 'pacman -Syu' or 'pacman -R' would half-replace or half-remove the
install. Pick one channel:
  sudo pacman -R ${PACMAN_OWNER}    # then re-run this installer
or upgrade through pacman and do not use this tarball on this machine."
    fi
fi

# ---------------------------------------------------------------------------
# Existing install detection (idempotency) -- before preflight, so a refused
# downgrade is reported as exactly that rather than as whichever preflight
# item happens to fail first.
# ---------------------------------------------------------------------------
INSTALLED_VERSION=''
if [ -f "${PREFIX}${VERSION_REL}" ]; then
    INSTALLED_VERSION="$(tr -d '[:space:]' < "${PREFIX}${VERSION_REL}")"
fi

MODE='install'
if [ -n "${INSTALLED_VERSION}" ]; then
    if [ "${INSTALLED_VERSION}" = "${NEW_VERSION}" ]; then
        MODE='reinstall'
        log ""
        log "urnetwork-daemon ${INSTALLED_VERSION} is already installed -- reinstalling (repair)."
    elif ver_ge "${NEW_VERSION}" "${INSTALLED_VERSION}"; then
        MODE='upgrade'
        log ""
        log "Upgrading urnetwork-daemon ${INSTALLED_VERSION} -> ${NEW_VERSION}."
    else
        if [ "${FORCE}" = 1 ]; then
            MODE='upgrade'
            warn "downgrading ${INSTALLED_VERSION} -> ${NEW_VERSION} because --force"
        elif [ "${DRY_RUN}" = 1 ]; then
            MODE='upgrade'
            warn "[dry-run] would refuse to downgrade ${INSTALLED_VERSION} -> ${NEW_VERSION} without --force"
        else
            die "installed version ${INSTALLED_VERSION} is newer than this tarball (${NEW_VERSION}) -- refusing to downgrade (re-run with --force to override)"
        fi
    fi
    log "State under /var/lib/urnetwork, configuration under /etc, and the"
    log "urnetwork system group are preserved; only binaries, units and"
    log "integration files are replaced."
else
    log ""
    log "Fresh install of urnetwork-daemon ${NEW_VERSION}."
fi

# ---------------------------------------------------------------------------
# Preflight (APPIMAGE.md 11g: say WHY on failure)
# ---------------------------------------------------------------------------
preflight_fail() {
    # Hard preflight failure; --force and --dry-run downgrade to warnings.
    if [ "${FORCE}" = 1 ]; then
        warn "$1 (continuing because --force)"
    elif [ "${DRY_RUN}" = 1 ]; then
        warn "[dry-run] preflight would fail: $1"
    else
        die "$1"
    fi
}

log ""
log "Preflight:"

# Say which host we think we are on -- every package name printed below is
# chosen from this, so a wrong guess has to be visible rather than inferred
# from advice that does not work.
if [ -n "${DISTRO_PRETTY}" ]; then
    note "host: ${DISTRO_PRETTY}${PKG_MGR:+ (package manager: ${PKG_MGR})}"
elif [ -n "${PKG_MGR}" ]; then
    note "host: unknown distribution (package manager: ${PKG_MGR})"
else
    note "host: unknown distribution and no recognised package manager -- missing-package advice below names the TOOL, not a package"
fi

# systemd
if [ -d /run/systemd/system ]; then
    note "systemd: running"
else
    preflight_fail "systemd is not running (/run/systemd/system missing) -- urnetworkd is managed as a systemd unit and this installer supports nothing else"
fi

# glibc floor
GLIBC_VERSION=''
if command -v getconf >/dev/null 2>&1; then
    GLIBC_VERSION="$(getconf GNU_LIBC_VERSION 2>/dev/null | sed -n 's/^glibc //p')" || true
fi
if [ -z "${GLIBC_VERSION}" ] && command -v ldd >/dev/null 2>&1; then
    GLIBC_VERSION="$(ldd --version 2>/dev/null | sed -n '1s/.* \([0-9][0-9.]*\)$/\1/p')" || true
fi
if [ -z "${GLIBC_VERSION}" ]; then
    preflight_fail "could not determine the glibc version (musl-based distros are not supported) -- urnetworkd needs glibc >= ${GLIBC_FLOOR}"
elif ver_ge "${GLIBC_VERSION}" "${GLIBC_FLOOR}"; then
    note "glibc: ${GLIBC_VERSION} (>= ${GLIBC_FLOOR})"
else
    preflight_fail "glibc ${GLIBC_VERSION} is older than the required ${GLIBC_FLOOR} (Ubuntu 22.04 / Debian 12 or newer) -- the daemon binary would not start"
fi

# /dev/net/tun. AN ABSENT NODE IS NOT A FAILED HOST when the driver is merely
# not loaded yet: the daemon attempts one modprobe at its first start, and on a
# freshly installed machine that has never opened a tun device the node does
# not exist at install time. Refusing there abandoned a working install for a
# condition that fixes itself thirty seconds later. Only a kernel with no tun
# driver at all -- a container without --device, or a stripped custom kernel,
# which is a live possibility on CachyOS -- is a hard stop.
# The module directory is a parameter with a default so the suffix matching
# below can be exercised against fixtures; the caller uses the default.
tun_module_available() {
    [ -d /sys/module/tun ] && return 0          # already loaded, or built in
    if command -v modinfo >/dev/null 2>&1 && modinfo tun >/dev/null 2>&1; then return 0; fi
    local moddir="${1:-}"
    [ -n "${moddir}" ] || moddir="/lib/modules/$(uname -r 2>/dev/null || printf 'none')"
    # Arch and CachyOS ship zstd-compressed modules (tun.ko.zst), Debian and
    # Fedora ship .ko.xz or plain .ko. Match the stem, not one suffix -- the
    # naive `-e .../tun.ko` test reports "no tun driver" on every Arch box.
    ls "${moddir}"/kernel/drivers/net/tun.ko* >/dev/null 2>&1 && return 0
    grep -q '/tun\.ko' "${moddir}/modules.builtin" 2>/dev/null && return 0
    return 1
}
if [ -e /dev/net/tun ]; then
    note "/dev/net/tun: present"
elif tun_module_available; then
    note "/dev/net/tun: not created yet, but this kernel has the tun driver -- the daemon loads it at its first start"
else
    preflight_fail "/dev/net/tun is missing and no tun driver was found for kernel $(uname -r 2>/dev/null || printf '?') -- load it (sudo modprobe tun) or, in a container, pass --device /dev/net/tun. On a custom kernel, tun must be built in or available as a module."
fi

# ---------------------------------------------------------------------------
# The data plane's external tools -- THE TARBALL'S DEPENDENCY DECLARATION
# ---------------------------------------------------------------------------
# The .deb declares Depends: iproute2 and gets nftables for free from the base
# system. A tarball declares nothing, so this check IS the dependency, and on
# Arch/CachyOS it is not academic: nftables and iproute2 are separate packages
# there and a minimal install has neither.
#
# These are REQUIRED, not nice to have. Without nft the daemon refuses to build
# a tunnel at all -- TunnelHost throws "refusing to start: the daemon's own
# traffic would be captured by its own tunnel (nftables (nft) is not
# installed)" -- because its own packets would otherwise enter its own tunnel.
# Without ip there is no tun address, no capture route and no policy rule.
# Failing here, where the package can be named, beats failing at the first
# Connect, where the app can only show the daemon's sentence.
if have_tool ip; then
    note "ip: $(tool_path ip) (iproute2)"
else
    want ip
    preflight_fail "iproute2 (the 'ip' command) is not installed -- the daemon configures the tun address, the capture routes and the policy rules with it, so no tunnel can ever be built. $(need_pkg ip)"
fi
if have_tool nft; then
    note "nft: $(tool_path nft) (nftables)"
else
    want nft
    preflight_fail "nftables (the 'nft' command) is not installed -- without it the daemon REFUSES to connect, because its own sockets would be captured by its own tunnel. It is also the IPv6/DNS leak floor and the kill switch. $(need_pkg nft)"
fi
if have_tool modprobe; then
    note "modprobe: $(tool_path modprobe)"
else
    want modprobe
    note "modprobe: not found -- the daemon cannot load the tun module itself if /dev/net/tun ever disappears. $(need_pkg modprobe)"
fi

# cgroup v2 unified hierarchy. The daemon marks its own sockets with a
# cgroup-BPF program; with no unified hierarchy there is nothing to attach to
# and it refuses to connect for the same reason as a missing nft. Asked the way
# the daemon asks it (the 0:: line is the v2 entry in /proc/self/cgroup).
if grep -q '^0::' /proc/self/cgroup 2>/dev/null; then
    note "cgroup v2: unified hierarchy present"
else
    preflight_fail "this host is not running the cgroup v2 unified hierarchy, so the daemon cannot mark its own sockets and will refuse to connect. Boot with systemd.unified_cgroup_hierarchy=1 (or remove a cgroup_no_v1/hybrid kernel argument)."
fi

# ---------------------------------------------------------------------------
# DNS THROUGH THE TUNNEL -- READ THIS BEFORE CHANGING IT
# ---------------------------------------------------------------------------
# systemd-resolved is the DNS takeover path URnetwork prefers: the daemon points
# the system resolver at the tunnel's resolvers with `resolvectl dns <tun> ...`.
# ARCH AND CACHYOS DO NOT ENABLE systemd-resolved BY DEFAULT, and the `systemd`
# package ships /usr/bin/resolvectl regardless -- so finding the binary proves
# nothing. Three separate things have to be true and each fails differently:
#
#   1. resolvectl exists
#   2. systemd-resolved is RUNNING (the binary is present on every Arch box;
#      the service is disabled, so `resolvectl dns` exits non-zero)
#   3. /etc/resolv.conf actually points at resolved's stub -- otherwise
#      `resolvectl dns` succeeds and glibc STILL reads the resolver listed in
#      resolv.conf. That third one is the nastiest because it looks like
#      success from inside the daemon.
#
# WHAT THIS CHECK DOES AND DOES NOT CLAIM. It reports the HOST condition, which
# is all an installer can know: whether this machine's preferred DNS path is
# available, and the exact commands that make it available. Whether the daemon
# can take DNS over some other way on a host without resolved is the DAEMON's
# question, it is versioned with the daemon and not with this script, and the
# daemon answers it out loud at connect time through dns_applied/dns_detail --
# so this message points at that answer instead of predicting it. Do not put a
# claim here about what the daemon falls back to; it will go stale.
#
# What IS unconditional: if the daemon ends up with dns_applied=false, traffic
# is tunnelled and names are not, the nftables floor that rejects off-tunnel
# :53 is gated on dns_applied so it is not installed either, and nothing on
# screen says so unless the kill switch is on -- with the kill switch ON the
# bring-up refuses instead ("the kill switch needs DNS on the tunnel, and it
# could not be applied"), which is the safe failure. The kill switch is OFF by
# default.
#
# This is a WARNING, not a refusal: the daemon installs and everything else
# works. It is repeated at the end of the run so it cannot scroll away.
# The file is a parameter with a default so this predicate can be exercised
# against fixtures; every caller uses the default.
resolv_conf_uses_resolved() {
    local f="${1:-/etc/resolv.conf}" target ns
    if [ -L "${f}" ]; then
        target="$(readlink -f "${f}" 2>/dev/null || true)"
        case "${target}" in /run/systemd/resolve/*) return 0 ;; esac
    fi
    # A plain copy of the stub counts too: what matters is that glibc is
    # pointed at resolved and at nothing else. Any nameserver line that is not
    # the 127.0.0.53/54 stub is a resolver the tunnel does not control.
    [ -r "${f}" ] || return 1
    ns="$(grep -E '^[[:space:]]*nameserver[[:space:]]' "${f}" 2>/dev/null || true)"
    [ -n "${ns}" ] || return 1
    printf '%s\n' "${ns}" | grep -qvE '127\.0\.0\.5[34]' && return 1
    return 0
}
RESOLVED_ACTIVE=''
if [ -d /run/systemd/system ]; then
    RESOLVED_ACTIVE="$(systemctl is-active systemd-resolved.service 2>/dev/null || true)"
fi
# On Debian and Fedora the service is a separate package; on Arch and openSUSE
# it is part of systemd and only needs enabling.
RESOLVED_PKG_HINT='sudo systemctl enable --now systemd-resolved'
case "${PKG_FAMILY}" in
    debian) RESOLVED_PKG_HINT='sudo apt install systemd-resolved && sudo systemctl enable --now systemd-resolved' ;;
    fedora) RESOLVED_PKG_HINT='sudo dnf install systemd-resolved && sudo systemctl enable --now systemd-resolved' ;;
esac
# The tail every branch ends with: how to verify, and what it means if the
# answer is no. Written once so the three branches cannot drift apart.
DNS_VERIFY="This is NOT fatal. systemd-resolved is only the FIRST of three ways
URnetwork points DNS at the tunnel; it falls back to resolvconf, and then to
taking over /etc/resolv.conf directly (restored on disconnect, and on the next
daemon start if this machine crashes). To see which one this host will use,
BEFORE connecting:
    sudo ${LIB_DIR}/urnetworkd --diagnose
and read the '[preflight] dns  tier N' line -- that is the tier the tunnel will
actually take, not a guess.

If NO tier can apply DNS, the daemon REFUSES to connect and says why, rather
than carrying your traffic while your names still go to your current (ISP)
resolver. That refusal is not tied to the kill switch: it happens either way."
if ! have_tool resolvectl; then
    want resolvectl
    DNS_WARNING="resolvectl is not installed, so URnetwork's preferred DNS path
(systemd-resolved) is not available on this host. Enable it with:
    $(need_pkg resolvectl)
    ${RESOLVED_PKG_HINT}
    sudo ln -sf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf

${DNS_VERIFY}"
    note "DNS: resolvectl MISSING -- see the warning below"
elif [ "${RESOLVED_ACTIVE}" != 'active' ]; then
    DNS_WARNING="resolvectl is installed but systemd-resolved is NOT RUNNING (systemctl
is-active systemd-resolved: ${RESOLVED_ACTIVE:-unknown}). Arch and CachyOS ship the
binary in the systemd package and leave the service disabled, so this is the
DEFAULT state on a fresh CachyOS box -- the binary being there proves nothing.
Enable it with:
    ${RESOLVED_PKG_HINT}
    sudo ln -sf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf
Then:  resolvectl status

${DNS_VERIFY}"
    note "DNS: systemd-resolved is ${RESOLVED_ACTIVE:-not running} -- see the warning below"
elif ! resolv_conf_uses_resolved; then
    DNS_WARNING="systemd-resolved is running, but /etc/resolv.conf does not point at it, so
glibc keeps resolving through the resolver listed there no matter what
resolvectl is told. This is the quiet one: the daemon's 'resolvectl dns' call
SUCCEEDS and looks applied from the inside. Point resolv.conf at the stub:
    sudo ln -sf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf
(NetworkManager picks resolved up by itself once resolv.conf is the stub.)
Then:  resolvectl status

${DNS_VERIFY}"
    note "DNS: /etc/resolv.conf does not point at systemd-resolved -- see the warning below"
else
    note "DNS: systemd-resolved active and /etc/resolv.conf points at it -- the tunnel's resolvers can be applied on this host"
fi

# ---------------------------------------------------------------------------
# Linux security modules -- what this installer does and does NOT do
# ---------------------------------------------------------------------------
# SELinux: a policy module is shipped and installed only where there is a
# policy to install it into. Arch and CachyOS have none and the step is skipped
# -- said out loud, because "nothing happened" and "the step silently broke"
# look identical in a log.
if selinux_active; then
    note "SELinux: $(getenforce 2>/dev/null || printf 'enabled') -- the policy module shipped in this tarball will be built and installed"
    if ! (command -v checkmodule >/dev/null 2>&1 && command -v semodule_package >/dev/null 2>&1 \
          && command -v semodule >/dev/null 2>&1); then
        note "SELinux: the policy tools are missing (checkmodule/semodule_package/semodule, from policycoreutils-devel) -- without them the tunnel cannot open /dev/net/tun on this host"
    fi
else
    note "SELinux: not active on this host -- no policy module is needed or installed (this is the normal Arch/CachyOS, Debian and Ubuntu case)"
fi
# AppArmor: URnetwork ships no profile and needs none. Arch does not enable
# AppArmor by default; Ubuntu does, and urnetworkd runs unconfined there.
if [ -d /sys/kernel/security/apparmor ]; then
    note "AppArmor: enabled -- URnetwork ships no AppArmor profile and needs none (urnetworkd runs unconfined; a custom restrictive profile would have to allow /dev/net/tun, CAP_NET_ADMIN and exec of ip/nft)"
fi

# Account tools. groupadd is REQUIRED: the unit's control socket is
# root:urnetwork 0750 and without the group nothing can reach the daemon.
if command -v groupadd >/dev/null 2>&1 || command -v addgroup >/dev/null 2>&1; then
    :
else
    want groupadd
    preflight_fail "neither groupadd nor addgroup is available, so the 'urnetwork' system group cannot be created -- the control socket is root:urnetwork 0750 and the app could never connect. $(need_pkg groupadd)"
fi
if ! command -v usermod >/dev/null 2>&1 && ! command -v gpasswd >/dev/null 2>&1; then
    want usermod
    note "usermod/gpasswd: not found -- this installer cannot add you to the urnetwork group and you will have to do it by hand. $(need_pkg usermod)"
fi

# Desktop integration. Optional and harmless on a headless box, but reported
# HERE rather than only after the install, so the tester sees the whole list of
# missing packages before deciding whether to fix them first.
if ! command -v update-desktop-database >/dev/null 2>&1; then
    want update-desktop-database
    note "update-desktop-database: not found -- urnetwork:// sign-in and wallet deep links will not resolve until it runs once. $(need_pkg update-desktop-database)"
fi
if ! command -v gtk-update-icon-cache >/dev/null 2>&1; then
    want gtk-update-icon-cache
    note "gtk-update-icon-cache: not found -- the launcher icon may not appear until the icon cache refreshes. $(need_pkg gtk-update-icon-cache)"
fi

# GeoClue floor for the location override -- an OPTIONAL feature: state the
# outcome plainly and keep going, never half-install or silently degrade.
GEOCLUE_VERSION=''
if command -v dpkg-query >/dev/null 2>&1; then
    GEOCLUE_VERSION="$(dpkg-query -W -f '${Version}' geoclue-2.0 2>/dev/null)" || true
fi
if [ -z "${GEOCLUE_VERSION}" ] && command -v rpm >/dev/null 2>&1; then
    GEOCLUE_VERSION="$(rpm -q --qf '%{VERSION}' geoclue2 2>/dev/null)" || true
    case "${GEOCLUE_VERSION}" in *not\ installed*) GEOCLUE_VERSION='' ;; esac
fi
if [ -z "${GEOCLUE_VERSION}" ] && command -v pacman >/dev/null 2>&1; then
    GEOCLUE_VERSION="$(pacman -Q geoclue 2>/dev/null | cut -d' ' -f2)" || true
fi
# strip epoch and distro revision: 2.7.2-1ubuntu1 / 1:2.7.2-1 -> 2.7.2
GEOCLUE_VERSION="$(printf '%s' "${GEOCLUE_VERSION}" | sed -e 's/^[0-9]*://' -e 's/-.*$//')"
if [ -z "${GEOCLUE_VERSION}" ]; then
    note "GeoClue: not found -- the optional location-override feature will be unavailable (needs geoclue >= ${GEOCLUE_FLOOR}); everything else installs and works"
elif ver_ge "${GEOCLUE_VERSION}" "${GEOCLUE_FLOOR}"; then
    note "GeoClue: ${GEOCLUE_VERSION} (location override supported)"
else
    note "GeoClue: ${GEOCLUE_VERSION} < ${GEOCLUE_FLOOR}: the location-override feature CANNOT work on this distro (Ubuntu 22.04 and Debian 12 can never satisfy it). The daemon still installs and every other feature works."
fi

# One command for everything preflight found missing. Five separate "install
# X" lines scattered through a log is a scavenger hunt; this is a paste.
# shellcheck disable=SC2086  # deliberate: split the accumulated list into words
MISSING_PKGS="$(printf '%s\n' ${MISSING_PKGS} | LC_ALL=C sort -u | tr '\n' ' ' | sed -e 's/^ *//' -e 's/ *$//')"
if [ -n "${MISSING_PKGS}" ]; then
    log ""
    log "Missing packages on this host -- one command installs all of them:"
    log "    $(pkg_install_cmd "${MISSING_PKGS}")"
fi

# The DNS finding, in full, where it cannot be mistaken for a note. It is
# printed again after the install completes.
if [ -n "${DNS_WARNING}" ]; then
    log ""
    log "  =========================================================================="
    log "  WARNING -- DNS MAY NOT GO THROUGH THE TUNNEL ON THIS HOST"
    log "  =========================================================================="
    printf '%s\n' "${DNS_WARNING}" | sed 's/^/  /'
    log "  =========================================================================="
fi

# polkit -- OPTIONAL, like GeoClue: state the outcome plainly, never fail. Its
# absence costs the no-log-out first run, not the product.
if detect_polkit; then
    POLKIT_PRESENT=1
    POLKIT_VERSION="$( (pkaction --version 2>/dev/null || true) | sed -n 's/.*version[[:space:]]*//p' | tr -d '[:space:]')"
    note "polkit: ${POLKIT_VERSION:-present} -- permission is granted to whoever is signed in at this screen, in the session they are already in (no group, no log-out)"
    # polkit gained multi-directory action lookup in 124. Before that it reads
    # /usr/share/polkit-1/actions ONLY -- and on an immutable host that is a
    # read-only image mount, so this installer maps the action file to
    # /usr/local/share and an older polkit would never see it.
    if [ "${LAYOUT}" = 'immutable' ] && [ -n "${POLKIT_VERSION}" ] && ! ver_ge "${POLKIT_VERSION}" '124'; then
        warn "polkit ${POLKIT_VERSION} predates multi-directory action lookup (124) and this host's /usr is read-only, so the action file must go to /usr/local/share/polkit-1/actions where this polkit will not read it. urnetworkd will keep asking for an administrator password; upgrade polkit, or use the 'urnetwork' group path."
    fi
else
    note "polkit: not found -- urnetworkd falls back to the 'urnetwork' group, which DOES still need a log-out (instructions at the end of this run)"
fi

# ---------------------------------------------------------------------------
# Build the file plan from the payload
# ---------------------------------------------------------------------------
# INSTALL_LIST: newline list of payload-relative paths (== install paths).
INSTALL_LIST="$(cd "${PAYLOAD_DIR}" && find . -type f | sed 's|^\.||' | LC_ALL=C sort)"
[ -n "${INSTALL_LIST}" ] || die "payload/ is empty"

file_mode() {
    case "$1" in
        /usr/bin/urnetwork|/usr/lib/urnetwork/urnetworkd) printf '0755' ;;
        *) printf '0644' ;;
    esac
}

# /etc files are admin-owned once present: keep the existing copy on upgrade
# (conffile semantics without dpkg).
is_config() { case "$1" in /etc/*) return 0 ;; *) return 1 ;; esac; }

OLD_MANIFEST=''
if [ -f "${PREFIX}${MANIFEST_REL}" ]; then
    OLD_MANIFEST="$(cat "${PREFIX}${MANIFEST_REL}")"
fi

# ---------------------------------------------------------------------------
# Dry-run: print the full plan and stop
# ---------------------------------------------------------------------------
if [ "${DRY_RUN}" = 1 ]; then
    log ""
    log "Plan (${MODE}):"
    [ -n "${PREFIX}" ] && log "  file layout rooted at prefix: ${PREFIX}"
    if [ -z "${PREFIX}" ]; then
        note "create system group 'urnetwork' if missing (FALLBACK authorizer only -- consulted where polkit is absent; nobody is added to it and no member is removed)"
        note "stop ${UNIT} if running (it may hold a live tun fd)"
        note "back up currently installed files for rollback-on-failure"
    fi
    echo "${INSTALL_LIST}" | while IFS= read -r rel; do
        dst="$(map_path "${rel}")"
        if [ "${rel}" = "${POLICY_REL}" ] && [ "${POLKIT_PRESENT}" = 0 ]; then
            note "SKIP ${PREFIX}${dst} -- no polkit here; installing it would promise the daemon a check it cannot complete, and the daemon fails closed on that promise"
            continue
        fi
        if is_config "${rel}" && [ -e "${PREFIX}${dst}" ]; then
            note "keep existing ${PREFIX}${dst} (admin-owned config)"
        else
            note "install ${PREFIX}${dst} ($(file_mode "${rel}"))"
        fi
    done
    if [ "${POLKIT_PRESENT}" = 1 ]; then
        note "force $(map_path "${POLICY_REL}") to 0644 root:root and check that $(dirname "$(map_path "${POLICY_REL}")") is root-owned and not group/world writable (polkit silently ignores a writable action file)"
    else
        note "remove any previously installed $(map_path "${POLICY_REL}") (polkit is gone; the daemon must fall back to the group)"
    fi
    [ "${LAYOUT}" = 'immutable' ] && \
        note "rewrite ExecStart in the unit to $(map_path '/usr/lib/urnetwork/urnetworkd')"
    note "install ${PREFIX}${LIB_DIR}/uninstall.sh, ${PREFIX}${MANIFEST_REL}, ${PREFIX}${VERSION_REL}"
    if [ -z "${PREFIX}" ]; then
        note "systemctl daemon-reload; reload udev rules and NetworkManager config"
        if [ "${MODE}" = 'install' ]; then
            note "systemctl enable ${UNIT} && start it (the daemon STARTS IDLE -- no tunnel until a client authenticates)"
        else
            note "preserve enable/disable state; start ${UNIT} only if it was running"
        fi
        note "run update-desktop-database and gtk-update-icon-cache (what dpkg triggers would have done)"
        if [ "${SKIP_SELFTEST}" = 0 ]; then
            note "run '${LIB_DIR}/urnetworkd --selftest-egress' to prove the cgroup-BPF socket marker works on this kernel (no tunnel, no routes, no nftables, no packets; --skip-selftest opts out)"
        fi
    fi
    log ""
    log "[dry-run] no changes were made."
    exit 0
fi

# ---------------------------------------------------------------------------
# Real install
# ---------------------------------------------------------------------------
WAS_ACTIVE='inactive'
BACKUP_DIR=''
WRITTEN_LOG=''
REPLACING=0

restore_backup() {
    # Undo this run's writes: restore what existed, delete what did not.
    [ -n "${BACKUP_DIR}" ] || return 0
    local rel
    while IFS= read -r rel; do
        [ -n "${rel}" ] || continue
        if [ -f "${BACKUP_DIR}${rel}" ]; then
            mkdir -p "$(dirname "${PREFIX}${rel}")"
            cp -p "${BACKUP_DIR}${rel}" "${PREFIX}${rel}"
        else
            rm -f "${PREFIX}${rel}"
        fi
    done <<< "${WRITTEN_LOG}"
}

on_error() {
    local rc=$?
    trap - ERR
    warn "install failed (exit ${rc})"
    if [ "${REPLACING}" = 1 ]; then
        warn "restoring the previous installation..."
        restore_backup || warn "restore incomplete -- inspect ${BACKUP_DIR}"
        if [ -z "${PREFIX}" ] && [ -d /run/systemd/system ]; then
            systemctl daemon-reload >/dev/null 2>&1 || true
            if [ "${WAS_ACTIVE}" = 'active' ]; then
                systemctl start "${UNIT}" >/dev/null 2>&1 || true
            fi
        fi
        warn "previous installation restored (backup kept at ${BACKUP_DIR})"
    fi
    exit "${rc}"
}
trap on_error ERR

# The `urnetwork` system group: STILL CREATED, AND NOBODY IS PUT IN IT.
#
# It is now only the FALLBACK authorizer. On a machine with no polkit,
# urnetworkd binds the control socket 0660 root:urnetwork and authorizes uid 0
# plus members of this group, byte-for-byte as before. Where polkit is present
# -- every desktop this app targets -- membership is INERT: the socket is 0666
# root:root and every privileged verb is authorized per-action against the
# peer's SO_PEERCRED uid.
#
# WHAT IS GONE, DELIBERATELY: `usermod -aG urnetwork "$SUDO_USER"` and the
# "log out and back in" banner that had to follow it. Supplementary groups are
# applied at LOGIN, so that pair made a correct install unusable until the user
# ended their session -- which is the exact first-run experience this change
# exists to remove, and which no shipping VPN asks for.
#
# The group is NOT deleted and existing members are NOT removed on upgrade:
# `gpasswd -d` would be a surprise with no upside, the gid still owns
# /run/urnetwork on a group-mode box, and a downgrade to an older daemon has to
# keep working. On a machine that already carries members, this upgrade simply
# makes their membership stop mattering -- see the closing message.
if [ -z "${PREFIX}" ]; then
    if ! getent group urnetwork >/dev/null 2>&1; then
        log "creating system group 'urnetwork' (fallback authorizer; used only where polkit is absent)"
        if command -v groupadd >/dev/null 2>&1; then
            run groupadd --system urnetwork
        else
            run addgroup --system urnetwork
        fi
    fi
    # ...AND PUT THE HUMAN IN IT -- BUT ONLY WHEN POLKIT IS NOT THE AUTHORITY.
    # This is the whole point of the polkit path: group membership is applied
    # by the LOGIN, so gating on it forces a log-out/reboot before a fresh
    # install can connect. Under polkit the person at the screen is authorized
    # in the session they are already in, so adding them to a group would be
    # pure ceremony -- and would print a log-out instruction they do not need.
    #
    # The group is still CREATED above unconditionally, because the socket
    # falls back to 0660 root:urnetwork if polkit ever goes away, and a group
    # that exists costs nothing.
    if [ "${POLKIT_PRESENT}" = 1 ]; then
        note "skipping group membership: polkit authorizes you in your current session, so no log-out is needed"
    else

        # ...AND PUT THE HUMAN IN IT. The control socket is 0750 root:urnetwork, so
        # a user who is not a member gets EACCES on connect(2) and the app can only
        # report "the service is not running" — which is false and unfixable from
        # the UI. Creating the group without ever adding anyone to it is the single
        # most likely way a correct install still cannot connect.
        #
        # sudo/pkexec keep the invoking user in SUDO_USER/PKEXEC_UID; a plain root
        # shell has neither, and root does not need the group.
        #
        # `sudo -i`, `su -` and a root console have neither variable, and that is
        # not a rare corner: it is how a lot of people install things. Two more
        # answers are tried before giving up, both of which name a real human and
        # neither of which guesses: logname(1) reports the owner of the login
        # session this process descends from, and a machine with exactly ONE
        # non-system user logged in has no ambiguity to resolve. Anything less
        # certain than that is left to the human.
        TARGET_USER=""
        if [ -n "${SUDO_USER:-}" ] && [ "${SUDO_USER}" != 'root' ]; then
            TARGET_USER="${SUDO_USER}"
        elif [ -n "${PKEXEC_UID:-}" ]; then
            TARGET_USER="$(getent passwd "${PKEXEC_UID}" 2>/dev/null | cut -d: -f1)"
        else
            CANDIDATE="$(logname 2>/dev/null || true)"
            if [ -z "${CANDIDATE}" ] || [ "${CANDIDATE}" = 'root' ]; then
                CANDIDATE=''
                if command -v loginctl >/dev/null 2>&1; then
                    SESSION_USERS="$(loginctl list-users --no-legend 2>/dev/null \
                        | awk '$1 >= 1000 { print $2 }' | LC_ALL=C sort -u || true)"
                    if [ "$(printf '%s' "${SESSION_USERS}" | grep -c . || true)" = 1 ]; then
                        CANDIDATE="${SESSION_USERS}"
                    fi
                fi
            fi
            # Only accept a name the password database actually knows.
            if [ -n "${CANDIDATE}" ] && getent passwd "${CANDIDATE}" >/dev/null 2>&1; then
                TARGET_USER="${CANDIDATE}"
                note "no SUDO_USER/PKEXEC_UID; using the login session's owner: ${TARGET_USER}"
            fi
        fi
        if [ -n "${TARGET_USER}" ]; then
            if id -nG "${TARGET_USER}" 2>/dev/null | tr ' ' '\n' | grep -qx urnetwork; then
                note "${TARGET_USER} is already in the urnetwork group"
            elif command -v usermod >/dev/null 2>&1; then
                log "adding ${TARGET_USER} to the urnetwork group"
                run usermod -aG urnetwork "${TARGET_USER}"
                GROUP_ADDED=1
            elif command -v gpasswd >/dev/null 2>&1; then
                log "adding ${TARGET_USER} to the urnetwork group"
                run gpasswd -a "${TARGET_USER}" urnetwork
                GROUP_ADDED=1
            else
                warn "could not add ${TARGET_USER} to the urnetwork group (no usermod/gpasswd): run 'sudo usermod -aG urnetwork ${TARGET_USER}' or the app cannot reach the service"
            fi
        else
            warn "could not tell which user to add to the urnetwork group (no SUDO_USER/PKEXEC_UID). Run: sudo usermod -aG urnetwork <you>"
        fi

    fi
fi

# Stop the daemon cleanly BEFORE touching its binary -- it may hold a live
# tun fd. It comes back up (idle) at the end.
if [ -z "${PREFIX}" ] && [ -d /run/systemd/system ]; then
    WAS_ACTIVE="$(systemctl is-active "${UNIT}" 2>/dev/null || true)"
    if [ "${WAS_ACTIVE}" = 'active' ]; then
        log "stopping ${UNIT} (open tunnel, if any, goes down for the upgrade)"
        run systemctl stop "${UNIT}"
    fi
fi

TMP_BASE="${TMPDIR:-/tmp}"; TMP_BASE="${TMP_BASE%/}"
BACKUP_DIR="$(mktemp -d "${TMP_BASE}/urnetwork-daemon-backup.XXXXXX")"
REPLACING=1

install_one() {
    # install_one <payload-relative path>; records the write (at its MAPPED
    # destination, which is what a rollback and the uninstaller act on) and
    # backs up any prior file.
    local rel="$1" mapped dst
    mapped="$(map_path "$1")"
    dst="${PREFIX}${mapped}"
    if [ -f "${dst}" ]; then
        mkdir -p "${BACKUP_DIR}$(dirname "${mapped}")"
        cp -p "${dst}" "${BACKUP_DIR}${mapped}"
    fi
    WRITTEN_LOG="${WRITTEN_LOG}${mapped}
"
    install -D -m "$(file_mode "${rel}")" "${PAYLOAD_DIR}${rel}" "${dst}"

    # The unit ships with ExecStart=/usr/lib/urnetwork/urnetworkd. On an
    # immutable host the daemon is not there, and systemd would fail the unit
    # with status=203/EXEC — a failure that looks like a broken build rather
    # than a misplaced file. Rewrite every /usr/lib/urnetwork path in the unit
    # to where the binary actually landed.
    if [ "${LAYOUT}" = 'immutable' ]; then
        case "${rel}" in
            */systemd/system/*.service)
                sed -i "s|/usr/lib/urnetwork/|$(map_path '/usr/lib/urnetwork')/|g" "${dst}"
                ;;
        esac
    fi
}

log "installing files..."
while IFS= read -r rel; do
    [ -n "${rel}" ] || continue
    # The polkit action file is the ONE payload file whose installation is
    # conditional (see POLKIT_PRESENT at the top): installing it on a machine
    # with no polkit tells the daemon a check can be completed there, and the
    # daemon fails closed on that promise rather than downgrading silently.
    if [ "${rel}" = "${POLICY_REL}" ] && [ "${POLKIT_PRESENT}" = 0 ]; then
        log "  skipping ${PREFIX}$(map_path "${rel}") (no polkit on this system; the 'urnetwork' group stays the authorizer)"
        continue
    fi
    # is_config asks about the PAYLOAD path on purpose: the systemd unit maps
    # into /etc/systemd/system on an immutable host, and treating it as an
    # admin-owned conffile there would mean an upgrade never replaces the unit.
    if is_config "${rel}" && [ -e "${PREFIX}$(map_path "${rel}")" ]; then
        log "  kept existing ${PREFIX}$(map_path "${rel}") (admin-owned config; new default stays in the tarball)"
        continue
    fi
    install_one "${rel}"
done <<< "${INSTALL_LIST}"

# Files the previous version installed that this one no longer ships
# (never under /etc or /var/lib -- upgrades only replace binaries, units and
# integration files).
if [ -n "${OLD_MANIFEST}" ]; then
    # The manifest records MAPPED paths, so compare against the mapped list.
    MAPPED_LIST="$(printf '%s\n' "${INSTALL_LIST}" | while IFS= read -r r; do
        [ -n "${r}" ] && map_path "${r}" && printf '\n'
    done)"
    # SAME FILE UNDER A DIFFERENT NAME IS NOT A STALE FILE. An install made
    # before the unit path was canonicalised recorded
    # /lib/systemd/system/urnetworkd.service; this one records
    # /usr/lib/systemd/system/urnetworkd.service. On every usr-merged distro
    # /lib is a symlink to usr/lib, so those two names are ONE inode -- the one
    # this run just wrote. A plain string comparison would call the old name
    # stale and `rm` the unit we are installing, leaving the machine with a
    # daemon and no service. -ef compares device and inode, which is the only
    # test that survives the symlink.
    same_as_installed() {
        local candidate="$1" m
        [ -e "${candidate}" ] || return 1
        while IFS= read -r m; do
            [ -n "${m}" ] || continue
            [ "${candidate}" -ef "${PREFIX}${m}" ] && return 0
        done <<< "${MAPPED_LIST}"
        return 1
    }
    while IFS= read -r rel; do
        [ -n "${rel}" ] || continue
        case "${rel}" in /etc/systemd/system/*) ;; /etc/*|/var/lib/*) continue ;; esac
        if ! printf '%s\n' "${MAPPED_LIST}" | grep -Fxq "${rel}" \
            && [ "${rel}" != "${LIB_DIR}/uninstall.sh" ] \
            && [ -f "${PREFIX}${rel}" ] \
            && ! same_as_installed "${PREFIX}${rel}"; then
            log "  removing stale ${PREFIX}${rel} (no longer shipped)"
            mkdir -p "${BACKUP_DIR}$(dirname "${rel}")"
            cp -p "${PREFIX}${rel}" "${BACKUP_DIR}${rel}"
            rm -f "${PREFIX}${rel}"
        fi
    done <<< "${OLD_MANIFEST}"
fi

# ---------------------------------------------------------------------------
# The polkit action file: mode, ownership, and the stale-copy sweep
# ---------------------------------------------------------------------------
# polkit's rejection of a group- or world-writable .policy file is SILENT -- it
# logs and skips the file, every action reverts to its built-in default, and
# the only symptom is that Connect starts asking for an administrator password
# on a machine where it never did. So the two things polkit actually inspects
# are set here explicitly rather than inherited from the payload's modes, and
# the DIRECTORY is checked too: polkit does not police the directory, and
# anyone who can write it can drop in a file that redefines our defaults.
POLICY_MAPPED="$(map_path "${POLICY_REL}")"
POLICY_DIR="$(dirname "${POLICY_MAPPED}")"
if [ "${POLKIT_PRESENT}" = 1 ]; then
    if [ -f "${PREFIX}${POLICY_MAPPED}" ]; then
        chmod 0644 "${PREFIX}${POLICY_MAPPED}"
        if [ -z "${PREFIX}" ]; then
            chown root:root "${PREFIX}${POLICY_MAPPED}" || \
                warn "could not chown ${POLICY_MAPPED} to root:root"
        fi
        POLICY_DIR_MODE="$(stat -c '%a' "${PREFIX}${POLICY_DIR}" 2>/dev/null || printf '')"
        POLICY_DIR_UID="$(stat -c '%u' "${PREFIX}${POLICY_DIR}" 2>/dev/null || printf '')"
        case "${POLICY_DIR_MODE}" in
            '') : ;;
            *[2367]|*[2367]?)
                warn "${POLICY_DIR} is group- or world-writable (mode ${POLICY_DIR_MODE}). Anyone who can write there can redefine URnetwork's polkit defaults -- fix with: sudo chmod 0755 ${POLICY_DIR}" ;;
        esac
        if [ -n "${POLICY_DIR_UID}" ] && [ "${POLICY_DIR_UID}" != 0 ] && [ -z "${PREFIX}" ]; then
            warn "${POLICY_DIR} is owned by uid ${POLICY_DIR_UID}, not root -- that account can redefine URnetwork's polkit defaults"
        fi
        note "polkit action file: ${PREFIX}${POLICY_MAPPED} (0644 root:root)"
        # On an ostree/bootc host the file CANNOT go to /usr/share -- that is a
        # read-only image mount -- so it lands under /usr/local/share like
        # everything else this installer remaps. polkit itself reads it there
        # (124+ scans /etc, /run, /usr/local/share and /usr/share), but the
        # daemon decides which authority it runs under by stat()ing this file,
        # and if it looks only at /usr/share it will silently fall back to the
        # `urnetwork` group -- which nobody is added to any more. State it.
        if [ "${LAYOUT}" = 'immutable' ]; then
            note "this host's /usr is read-only, so the action file is at ${POLICY_MAPPED} rather than /usr/share/polkit-1/actions. polkit reads both. If the app still reports a permission problem after this install, the daemon looked for the file in the wrong one of them."
        fi
        # TELL POLKITD THE FILE EXISTS. Writing a .policy is not enough:
        # polkitd reads its action directories at START and does not reliably
        # notice a file appearing in one later -- especially when the installer
        # had to CREATE /usr/local/share/polkit-1/actions, since there was no
        # directory to be watching in the first place.
        #
        # MEASURED on Bazzite: after a clean install the daemon correctly
        # reported `authorization: polkit`, and `pkaction --action-id
        # network.ur.urnetwork.control-tunnel` answered "No action with action
        # id" -- polkitd had been up since the previous boot, two days earlier.
        # Every authorization check would have been made against an action
        # polkit did not know, so Connect would have failed on a host the
        # installer had just declared ready.
        #
        # reload, never restart: polkit.service is Type=notify-reload with
        # CanReload=yes, so this re-reads actions and rules without dropping
        # the sessions of everything else on the box that depends on polkit.
        if [ -z "${PREFIX}" ] && [ -d /run/systemd/system ]; then
            if [ "${DRY_RUN}" = 1 ]; then
                log "  would reload polkit so it picks up the new action file"
            elif systemctl reload polkit.service >/dev/null 2>&1 \
                 || systemctl reload polkit >/dev/null 2>&1; then
                note "polkit reloaded -- the action file is live now, no reboot needed"
            else
                warn "could not reload polkit. It will not know about ${POLICY_MAPPED} until it restarts, and until then the app will report a permission problem. Fix with: sudo systemctl reload polkit"
            fi
        fi
    else
        warn "polkit was detected but ${PREFIX}${POLICY_MAPPED} was not installed -- the daemon will fall back to the 'urnetwork' group, which needs a log-out"
    fi
elif [ -f "${PREFIX}${POLICY_MAPPED}" ]; then
    # An earlier install put it there while this machine still had polkit.
    # Leaving it behind would tell the daemon that a check can be completed
    # here; it would then refuse every privileged verb with no way back except
    # reinstalling polkit. Removing it restores the group path instead.
    log "  removing ${PREFIX}${POLICY_MAPPED} (polkit is no longer installed; the 'urnetwork' group becomes the authorizer again)"
    mkdir -p "${BACKUP_DIR}${POLICY_DIR}"
    cp -p "${PREFIX}${POLICY_MAPPED}" "${BACKUP_DIR}${POLICY_MAPPED}"
    WRITTEN_LOG="${WRITTEN_LOG}${POLICY_MAPPED}
"
    rm -f "${PREFIX}${POLICY_MAPPED}"
fi

# Bookkeeping: uninstaller + manifest + version marker.
install -D -m 0755 "${SELF_DIR}/uninstall.sh" "${PREFIX}${LIB_DIR}/uninstall.sh"
mkdir -p "${PREFIX}${LIB_DIR}"
# The manifest holds INSTALL paths, not payload paths — the uninstaller reads
# it to decide what to remove, and on an immutable host those differ.
printf '%s\n' "${INSTALL_LIST}" | while IFS= read -r rel; do
    [ -n "${rel}" ] && map_path "${rel}" && printf '\n'
done > "${PREFIX}${MANIFEST_REL}"
printf '%s\n' "${NEW_VERSION}" > "${PREFIX}${VERSION_REL}"

# ---------------------------------------------------------------------------
# System integration (skipped under --prefix)
# ---------------------------------------------------------------------------
if [ -z "${PREFIX}" ]; then
    # /run/urnetwork: the unit's RuntimeDirectory= owns this (0755 root:root)
    # and re-applies its mode on every start; pre-create so the control path
    # exists before the first start. The mode encodes which authority is in
    # force -- 0755 root:root under polkit, 0750 root:urnetwork on the group
    # fallback -- so pre-create it the way this machine will actually run, and
    # let urnetworkd settle it authoritatively when it binds the socket.
    if [ "${POLKIT_PRESENT}" = 1 ]; then
        run install -d -m 0755 -o root -g root /run/urnetwork
    else
        run install -d -m 0750 -o root -g urnetwork /run/urnetwork
    fi

    if [ -d /run/systemd/system ]; then
        run systemctl daemon-reload
    fi
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload >/dev/null 2>&1 || true
    fi
    if command -v nmcli >/dev/null 2>&1; then
        # Make the unmanaged-device marking live before the first tunnel;
        # config reload does not touch existing connections.
        nmcli general reload conf >/dev/null 2>&1 || true
    fi

    # ORDER IS LOAD-BEARING: this must happen BEFORE the unit is started. A
    # process keeps the SELinux context it was exec'd with, so relabelling
    # after `systemctl start` fixes the FILE and leaves the RUNNING daemon in
    # init_t — the tunnel still cannot open /dev/net/tun, and only a restart
    # (which nothing prompts for) would apply it. Measured exactly that way.
    # SELINUX. A daemon installed outside a distribution package carries no
    # policy, so systemd runs it in `init_t`, which is forbidden the three
    # things this daemon exists to do (open /dev/net/tun, create a raw socket,
    # reach the API on 443). Measured on Bazzite with the full capability set:
    # SELinux refuses before the capability is ever consulted.
    #
    # DO NOT "fix" this by labelling the binary unconfined_exec_t. That is the
    # common recipe and it is wrong here: Bazzite ships no unconfined module, so
    # init_t may not even EXECUTE such a file and the daemon fails to start with
    # 203/EXEC. That was tried, measured, and reverted.
    #
    # Instead install the policy module shipped beside this script. It grants
    # exactly the permissions the kernel denied and nothing else.
    #
    # The gate is selinux_active(), NOT `command -v getenforce`: see its
    # definition. On Arch and CachyOS this whole block is skipped, which is
    # correct and is stated in preflight so it does not look like a silent
    # failure.
    if selinux_active; then
        SEPOL_TE="${SELF_DIR}/selinux/urnetwork.te"
        if [ ! -f "${SEPOL_TE}" ]; then
            warn "SELinux is enabled but selinux/urnetwork.te is missing from this tarball: the tunnel will not be able to open /dev/net/tun"
        elif command -v checkmodule >/dev/null 2>&1 && command -v semodule_package >/dev/null 2>&1 \
             && command -v semodule >/dev/null 2>&1; then
            SEPOL_DIR="$(mktemp -d "${TMPDIR:-/tmp}/urnetwork-selinux.XXXXXX")"
            if [ "${DRY_RUN}" = 1 ]; then
                note "build and install the SELinux policy module (checkmodule + semodule -i)"
            elif checkmodule -M -m -o "${SEPOL_DIR}/urnetwork.mod" "${SEPOL_TE}" >/dev/null 2>&1 \
                 && semodule_package -o "${SEPOL_DIR}/urnetwork.pp" -m "${SEPOL_DIR}/urnetwork.mod" >/dev/null 2>&1 \
                 && semodule -i "${SEPOL_DIR}/urnetwork.pp" >/dev/null 2>&1; then
                note "SELinux: policy module 'urnetwork' installed (remove with: sudo semodule -r urnetwork)"
            else
                warn "SELinux policy module failed to build or install. The daemon will run but the tunnel cannot open /dev/net/tun. Build it by hand from ${SEPOL_TE}, or see: sudo journalctl -t audit | grep 'denied.*tun'"
            fi
            rm -rf "${SEPOL_DIR}"
        else
            warn "SELinux is enforcing but the policy tools are missing (checkmodule/semodule_package/semodule, from policycoreutils-devel). The daemon will run but the tunnel cannot open /dev/net/tun. Install those tools and re-run this installer."
        fi
    fi

    if [ -d /run/systemd/system ]; then
        if [ "${MODE}" = 'install' ]; then
            # Enable+start on install is Debian Policy 9.3.3.1-correct only
            # because urnetworkd starts idle: no tunnel until a client
            # authenticates over the control socket.
            run systemctl enable "${UNIT}"
            run systemctl start "${UNIT}"
        else
            # Upgrade: never undo an admin's enable/disable choice; start
            # only what was running before we stopped it.
            if [ "${WAS_ACTIVE}" = 'active' ]; then
                run systemctl start "${UNIT}"
            else
                log "${UNIT} was not running before the upgrade; leaving it stopped"
            fi
        fi
    fi

    # What dpkg file triggers would have done (APPIMAGE.md section 5 callout):
    # update-desktop-database rebuilds mimeinfo.cache -- the thing
    # x-scheme-handler/urnetwork lookups consult. Without it the launcher
    # still appears in menus (so the breakage is invisible on a dev box) but
    # urnetwork:// SSO callbacks and wallet deep links silently fail, and any
    # later `apt install` of anything repairs it -- so it escapes testing.
    # SELinux (Fedora Silverblue / Bazzite / Kinoite): files written outside a
    # package manager keep whatever label the parent dir gave them. The daemon
    # is started by systemd and the launcher is exec'd from a desktop file, so
    # a wrong label costs an AVC denial that looks like a mystery failure.
    # restorecon applies the policy's own contexts; a system without SELinux
    # simply has no such command.
    if [ "${LAYOUT}" = 'immutable' ] && command -v restorecon >/dev/null 2>&1; then
        restorecon -R "$(map_path '/usr/lib/urnetwork')" \
                      "$(map_path '/usr/bin')/urnetwork" \
                      /etc/systemd/system >/dev/null 2>&1 || \
            warn "restorecon failed -- if the service is denied by SELinux, run: sudo restorecon -R $(map_path '/usr/lib/urnetwork')"
    fi

    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q "$(map_path '/usr/share')/applications" || \
            warn "update-desktop-database failed -- urnetwork:// links may not resolve"
    else
        warn "update-desktop-database not found: urnetwork:// SSO/deep links will NOT resolve until it runs. $(need_pkg update-desktop-database). Headless servers can ignore this."
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f "$(map_path '/usr/share')/icons/hicolor" 2>/dev/null || \
            warn "gtk-update-icon-cache failed -- the launcher icon may not appear until the cache refreshes"
    else
        warn "gtk-update-icon-cache not found: the launcher icon may not appear until the icon cache refreshes. $(need_pkg gtk-update-icon-cache). Headless servers can ignore this."
    fi
fi

# ---------------------------------------------------------------------------
# Post-install verification: does the egress socket marker work ON THIS KERNEL?
# ---------------------------------------------------------------------------
# The daemon keeps its own packets out of its own tunnel with a four-instruction
# BPF program attached at BPF_CGROUP_INET_SOCK_CREATE. Whether that loads,
# attaches and actually marks sockets is a property of the RUNNING KERNEL, not
# of this package -- and a custom or hardened kernel (CachyOS ships several) is
# exactly the case where assuming it is the mistake. The last time this
# daemon's own packets went into its own tun it moved 3.38 Tb in forty minutes.
#
# --selftest-egress is the measurement the daemon ships for precisely this: it
# starts no tunnel, touches no routes, no nftables and no DNS, sends no packet,
# and creates then removes ONE temporary cgroup. Exit 0 works, 1 does not,
# 2 could not be measured. It is never fatal here -- the daemon checks the same
# thing again at connect time and refuses on its own -- but a tester who sees
# it fail during install can report it before wasting an evening on symptoms.
if [ -z "${PREFIX}" ] && [ "${SKIP_SELFTEST}" = 0 ] && [ "${HOST_OS}" = 'Linux' ] \
   && [ -x "${LIB_DIR}/urnetworkd" ]; then
    log ""
    log "Verifying the egress socket marker on this kernel (no tunnel, no packets)..."
    SELFTEST_RC=0
    if command -v timeout >/dev/null 2>&1; then
        timeout 90 "${LIB_DIR}/urnetworkd" --selftest-egress || SELFTEST_RC=$?
    else
        "${LIB_DIR}/urnetworkd" --selftest-egress || SELFTEST_RC=$?
    fi
    case "${SELFTEST_RC}" in
        0)   note "egress self-exclusion: WORKS on this kernel" ;;
        2)   warn "the egress self-test could not be run on this host (exit 2 -- it proves nothing either way). Re-run it later with: sudo ${LIB_DIR}/urnetworkd --selftest-egress" ;;
        124) warn "the egress self-test timed out after 90s. Re-run it by hand: sudo ${LIB_DIR}/urnetworkd --selftest-egress" ;;
        *)   warn "EGRESS SELF-EXCLUSION DOES NOT WORK ON THIS KERNEL (exit ${SELFTEST_RC}).
The daemon will refuse to connect rather than route its own traffic into its
own tunnel, so this is safe -- but it means URnetwork cannot run on this
kernel as configured. The output above says which of load/attach/mark failed;
please report it with 'uname -r' and that output." ;;
    esac
fi

trap - ERR
REPLACING=0
rm -rf "${BACKUP_DIR}"

log ""
log "urnetwork-daemon ${NEW_VERSION} ${MODE} complete."
if [ -z "${PREFIX}" ]; then
    log "The daemon is running idle; nothing connects until you sign in from the app."
    log "Unit installed at: $(map_path "/lib/systemd/system/${UNIT}")"
    if [ "${POLKIT_PRESENT}" = 1 ]; then
        log ""
        log "No group change and no log-out are required: polkit grants permission to"
        log "whoever is signed in at this device's screen the moment they press Connect."
        # A machine upgraded from a group-based install still has members. Say
        # plainly that nothing has to be undone -- the silent alternative is a
        # user who assumes the old instructions still apply and logs out for
        # nothing, or worse, tries to "clean up" a gid that owns files.
        if [ -n "$(getent group urnetwork 2>/dev/null | cut -d: -f4)" ]; then
            log ""
            log "Permission is now granted by polkit, not by the 'urnetwork' group. Existing"
            log "members are left as they are -- nothing to undo, and no log-out is needed. The"
            log "group is now used only on systems without polkit."
        fi
    elif [ "${GROUP_ADDED}" = 1 ]; then
        log ""
        log "IMPORTANT: you were added to the 'urnetwork' group, and group membership"
        log "only applies to NEW login sessions. Log out and back in (or reboot) before"
        log "starting the app, or it will report that the service is not running."
        log "Nothing you can type in the current terminal fixes this for the desktop:"
        log "'newgrp urnetwork' only affects that one shell, not the session the app"
        log "is launched from. After logging back in, confirm with:"
        log "    id -nG | tr ' ' '\\n' | grep -x urnetwork"
        log ""
        log "Installing polkit and re-running this installer removes that requirement"
        log "entirely -- it is what lets other VPN clients skip this step."
    else
        log ""
        warn "no polkit authorization service was found on this system."
        log "urnetworkd will fall back to the 'urnetwork' group. Add the user who runs the app:"
        log "    sudo usermod -aG urnetwork <user>"
        log "Group membership only applies to NEW login sessions, so log out and back in after."
        log "Installing polkit and re-running this installer removes that requirement."
    fi
    log ""
    log "Next: install the URnetwork GUI AppImage to ~/.local/lib/urnetwork/URnetwork.AppImage"
    log "and run 'urnetwork' (https://ur.io/download)."
    log ""
    log "If anything does not work, this prints the whole host picture and needs no root:"
    log "    ${LIB_DIR}/urnetworkd --diagnose"
    log "If the machine ever ends up blocked (the kill switch is fail-closed on purpose):"
    log "    sudo systemctl stop ${UNIT} && sudo ${LIB_DIR}/urnetworkd --revert"
    log "Uninstall later with: sudo ${LIB_DIR}/uninstall.sh"
fi

# LAST, so it is what remains on screen. A privacy finding printed before two
# hundred lines of install output has not been reported to anybody.
if [ -n "${DNS_WARNING}" ] && [ -z "${PREFIX}" ]; then
    log ""
    log "  =========================================================================="
    log "  WARNING -- CHECK DNS BEFORE YOU TRUST THIS TUNNEL"
    log "  =========================================================================="
    printf '%s\n' "${DNS_WARNING}" | sed 's/^/  /'
    log "  =========================================================================="
fi
log "You can delete the extracted installer directory: rm -rf '${SELF_DIR}'"
