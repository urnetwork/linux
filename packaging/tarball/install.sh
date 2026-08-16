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
# It REFUSES to run where dpkg (or rpm) owns urnetwork-daemon: two package
# managers owning the same paths is the worst failure available here, and it
# is silent until an upgrade half-replaces files. Use apt there.
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
    if [ "${LAYOUT}" != 'immutable' ]; then printf '%s' "$1"; return 0; fi
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
    sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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

# /dev/net/tun
if [ -e /dev/net/tun ]; then
    note "/dev/net/tun: present"
else
    preflight_fail "/dev/net/tun is missing -- load the tun module (modprobe tun); in a container, pass --device /dev/net/tun"
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
    while IFS= read -r rel; do
        [ -n "${rel}" ] || continue
        case "${rel}" in /etc/systemd/system/*) ;; /etc/*|/var/lib/*) continue ;; esac
        if ! printf '%s\n' "${MAPPED_LIST}" | grep -Fxq "${rel}" \
            && [ "${rel}" != "${LIB_DIR}/uninstall.sh" ] \
            && [ -f "${PREFIX}${rel}" ]; then
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
    if command -v getenforce >/dev/null 2>&1 && [ "$(getenforce 2>/dev/null)" != 'Disabled' ]; then
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
        warn "update-desktop-database not found (desktop-file-utils): urnetwork:// SSO/deep links will NOT resolve until it runs. Headless servers can ignore this."
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f "$(map_path '/usr/share')/icons/hicolor" 2>/dev/null || \
            warn "gtk-update-icon-cache failed -- the launcher icon may not appear until the cache refreshes"
    else
        warn "gtk-update-icon-cache not found: the launcher icon may not appear until the icon cache refreshes. Headless servers can ignore this."
    fi
fi

trap - ERR
REPLACING=0
rm -rf "${BACKUP_DIR}"

log ""
log "urnetwork-daemon ${NEW_VERSION} ${MODE} complete."
if [ -z "${PREFIX}" ]; then
    log "The daemon is running idle; nothing connects until you sign in from the app."
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
    log "Uninstall later with: sudo ${LIB_DIR}/uninstall.sh"
fi
log "You can delete the extracted installer directory: rm -rf '${SELF_DIR}'"
