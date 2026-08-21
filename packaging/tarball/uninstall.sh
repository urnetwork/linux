#!/bin/bash
# URnetwork daemon uninstaller (tarball channel; APPIMAGE.md 11g).
#
# Ships inside the install tarball and is also installed to
# /usr/lib/urnetwork/uninstall.sh, so it works long after the tarball is gone.
#
# Removes the unit (stop + disable), binaries and desktop-integration files,
# then reruns update-desktop-database and gtk-update-icon-cache -- without
# that, a removed app keeps claiming the urnetwork:// scheme (APPIMAGE.md
# section 5 callout). Prompts before touching state: /var/lib/urnetwork, the
# urnetwork system group, and /etc/urnetwork. /etc/geolocation is removed
# whenever URnetwork wrote it (leaving a faked location behind would be a
# bug, so that is not behind the prompt).
#
# It also LIFTS THE FIREWALL before removing anything. The kill switch is an
# nftables table that deliberately outlives the process, and the unit's
# ExecStopPost preserves an armed floor across a stop -- so uninstalling an
# armed machine used to delete the only binary that knows how to unblock it.
#
# Options:
#   --purge         also remove state without prompting
#   --keep-state    keep state without prompting
#   --yes           assume yes on prompts
#   --dry-run       print what would happen
#   --prefix <dir>  operate on a --prefix test layout (no system mutation)
set -Eeuo pipefail

PKG_NAME='urnetwork-daemon'
UNIT='urnetworkd.service'

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR=''
MANIFEST_REL=''
SHARE_DIR=''
BIN_DIR=''

DRY_RUN=0
PREFIX=''
PURGE=''
ASSUME_YES=0

log()  { printf '%s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

run() {
    if [ "${DRY_RUN}" = 1 ]; then
        printf 'would run: %s\n' "$*"
    else
        "$@"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --purge)      PURGE=1; shift ;;
        --keep-state) PURGE=0; shift ;;
        --yes|-y)     ASSUME_YES=1; shift ;;
        --dry-run)    DRY_RUN=1; shift ;;
        --prefix)     PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
        -h|--help)    sed -n '2,/^set -Eeuo/p' "${BASH_SOURCE[0]}" \
                          | sed -e '$d' -e 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
done

# LIB_DIR is DERIVED FROM THIS SCRIPT'S OWN LOCATION, not hardcoded: install.sh
# puts the uninstaller at <LIB_DIR>/uninstall.sh, and on an immutable host
# (ostree/bootc — Silverblue, Bazzite, Kinoite, MicroOS) that is
# /usr/local/lib/urnetwork, not /usr/lib/urnetwork. Asking where we are is the
# only answer that is right in both layouts. Resolved AFTER argument parsing so
# a --prefix test layout can be stripped back off — every path below is
# re-prefixed, and deriving it earlier would double the prefix.
SELF_INSTALLED_DIR="${SELF_DIR}"
[ -n "${PREFIX}" ] && SELF_INSTALLED_DIR="${SELF_DIR#"${PREFIX%/}"}"
case "${SELF_INSTALLED_DIR}" in
    */lib/urnetwork) LIB_DIR="${SELF_INSTALLED_DIR}" ;;
    *) # running from the extracted tarball, not from the install: take
       # whichever layout is actually present on this machine.
       if [ -f "${PREFIX}/usr/local/lib/urnetwork/.install-manifest" ]; then
           LIB_DIR='/usr/local/lib/urnetwork'
       else
           LIB_DIR='/usr/lib/urnetwork'
       fi ;;
esac
MANIFEST_REL="${LIB_DIR}/.install-manifest"

# The share/ and bin/ prefixes follow the same split. Only the directory
# sweeps, the cache refreshes and the no-manifest fallback list need them —
# individual files normally come from the manifest.
case "${LIB_DIR}" in
    /usr/local/*) SHARE_DIR='/usr/local/share'; BIN_DIR='/usr/local/bin' ;;
    *)            SHARE_DIR='/usr/share';       BIN_DIR='/usr/bin' ;;
esac

if [ "${DRY_RUN}" = 0 ] && [ -z "${PREFIX}" ]; then
    [ "$(uname -s)" = 'Linux' ] || die "this uninstaller runs on Linux only (use --dry-run elsewhere)"
    [ "$(id -u)" = 0 ] || die "must run as root (sudo ${LIB_DIR}/uninstall.sh)"
fi

# Never remove a dpkg-owned install from here.
if command -v dpkg >/dev/null 2>&1; then
    # dpkg -s exits 1 for an unknown package; that must not trip set -e.
    DPKG_STATUS="$( (dpkg -s "${PKG_NAME}" 2>/dev/null || true) | sed -n 's/^Status: //p')"
    case "${DPKG_STATUS}" in
        *installed*) die "${PKG_NAME} is owned by dpkg/apt -- remove it with: sudo apt purge ${PKG_NAME}" ;;
    esac
fi
if command -v rpm >/dev/null 2>&1 && rpm -q "${PKG_NAME}" >/dev/null 2>&1; then
    die "${PKG_NAME} is owned by rpm -- remove it with: sudo dnf remove ${PKG_NAME}"
fi
# ...and never remove a pacman-owned one either. Queried by PATH rather than by
# name for the same reason install.sh does it: a future Arch/AUR package could
# be called urnetwork, urnetwork-bin, urnetwork-daemon or urnetwork-git, and
# the file is what actually collides. `pacman -Qo` exits non-zero both when
# nothing owns the path and when the path does not exist.
if command -v pacman >/dev/null 2>&1 && [ -z "${PREFIX}" ]; then
    for _p in "${LIB_DIR}/urnetworkd" "${BIN_DIR}/urnetwork" \
              /usr/lib/systemd/system/"${UNIT}" /lib/systemd/system/"${UNIT}"; do
        _owner="$( (pacman -Qoq "${_p}" 2>/dev/null || true) | head -n1 )"
        if [ -n "${_owner}" ]; then
            die "the pacman package '${_owner}' owns ${_p} -- remove it with: sudo pacman -R ${_owner}"
        fi
    done
fi

# File list: the manifest install.sh wrote; a hardcoded fallback otherwise.
# Fallback must track the MIGRATION.md installed-path table (and
# linux/packaging/lib/common.sh, which encodes it for the build side).
MANIFEST=''
if [ -f "${PREFIX}${MANIFEST_REL}" ]; then
    MANIFEST="$(cat "${PREFIX}${MANIFEST_REL}")"
else
    warn "no manifest at ${PREFIX}${MANIFEST_REL}; using the built-in file list"
    # ALL THREE unit locations are listed. The immutable layout ships it to
    # /etc/systemd/system; the standard layout writes /lib/systemd/system,
    # which on every usr-merged distribution (Arch, CachyOS, Debian >= 12,
    # Ubuntu >= 20.04, Fedora) is a symlink and lands the file in
    # /usr/lib/systemd/system -- which is also where install.sh now records it.
    # An older install's manifest says /lib/..., a newer one says /usr/lib/...,
    # and a host where /lib is a real directory says /lib/... and means it.
    # Removing a path that does not exist is a no-op; leaving a stale unit
    # behind keeps a dead service in systemctl's list forever.
    #
    # The polkit action file follows ${SHARE_DIR}, so it is covered in both
    # layouts. It must go with the daemon: while it is on disk, any urnetworkd
    # that comes back (a downgrade, a reinstall) reads its presence as "polkit
    # is the authority here" -- see kPolkitPolicyPath in ControlProtocol.hpp.
    MANIFEST="${LIB_DIR}/urnetworkd
${LIB_DIR}/libURnetworkSdk.so
${BIN_DIR}/urnetwork
/lib/systemd/system/urnetworkd.service
/usr/lib/systemd/system/urnetworkd.service
/etc/systemd/system/urnetworkd.service
${SHARE_DIR}/applications/com.bringyour.network.desktop
${SHARE_DIR}/metainfo/com.bringyour.network.metainfo.xml
${SHARE_DIR}/icons/hicolor/48x48/apps/com.bringyour.network.png
${SHARE_DIR}/icons/hicolor/64x64/apps/com.bringyour.network.png
${SHARE_DIR}/icons/hicolor/128x128/apps/com.bringyour.network.png
${SHARE_DIR}/icons/hicolor/256x256/apps/com.bringyour.network.png
${SHARE_DIR}/icons/hicolor/512x512/apps/com.bringyour.network.png
${SHARE_DIR}/polkit-1/actions/com.bringyour.network.policy
/etc/urnetwork/autostart/com.bringyour.network.desktop
/etc/NetworkManager/conf.d/95-urnetwork.conf
/etc/udev/rules.d/85-urnetwork-unmanaged.rules"
    # plus whole directories swept below: ${SHARE_DIR}/urnetwork, locale .mo
fi

# Stop and disable the unit first -- it may hold a live tun fd. Tolerate a
# half-removed install where the unit is already gone.
if [ -z "${PREFIX}" ] && [ -d /run/systemd/system ]; then
    run systemctl stop "${UNIT}" || true
    run systemctl disable "${UNIT}" || true
fi

# LIFT THE FIREWALL BEFORE DELETING THE ONLY THING THAT CAN LIFT IT.
#
# The kill switch is an nftables table that outlives the process ON PURPOSE,
# and the unit's ExecStopPost is `--revert-unless-armed`: a machine that was
# ARMED when the daemon stopped keeps its block floor, by design, so a crash
# does not open a window. That design turns an uninstall into a trap -- stop
# the unit, delete /usr/lib/urnetwork/urnetworkd, and the floor is still in the
# kernel with nothing left on the machine that knows how to remove it. The user
# is off the network, and the recovery command the unit file documented has
# just been deleted.
#
# `--revert` is the unconditional sweep (table, policy rules, capture routes,
# armed marker) and is idempotent by construction, so running it on a machine
# with nothing installed is a successful no-op. It needs root, which we already
# checked, and it must run while the binary still exists.
if [ -z "${PREFIX}" ] && [ "$(uname -s)" = 'Linux' ] && [ -x "${LIB_DIR}/urnetworkd" ]; then
    log "lifting the URnetwork firewall (nftables table, policy rules, capture routes)..."
    if [ "${DRY_RUN}" = 1 ]; then
        printf 'would run: %s --revert\n' "${LIB_DIR}/urnetworkd"
    elif ! "${LIB_DIR}/urnetworkd" --revert; then
        warn "'${LIB_DIR}/urnetworkd --revert' failed. If this machine is now blocked, run
  sudo nft delete table inet urnetwork
before or after this uninstall finishes -- the binary is about to be removed."
    fi
elif [ -z "${PREFIX}" ] && [ "$(uname -s)" = 'Linux' ]; then
    # No binary to revert with: say what to type rather than leaving it silent.
    warn "no ${LIB_DIR}/urnetworkd to revert the firewall with. If this machine is blocked, run: sudo nft delete table inet urnetwork"
fi

log "removing installed files..."
while IFS= read -r rel; do
    [ -n "${rel}" ] || continue
    [ -e "${PREFIX}${rel}" ] || continue
    run rm -f "${PREFIX}${rel}"
done <<< "${MANIFEST}"

# Shared data and gettext catalogs (directory sweeps).
[ -d "${PREFIX}${SHARE_DIR}/urnetwork" ] && run rm -rf "${PREFIX}${SHARE_DIR}/urnetwork"
if [ "${DRY_RUN}" = 1 ]; then
    log "would remove ${SHARE_DIR}/locale/*/LC_MESSAGES/urnetwork.mo"
else
    find "${PREFIX}${SHARE_DIR}/locale" -name 'urnetwork.mo' -type f -delete 2>/dev/null || true
fi

# /etc/geolocation: only if URnetwork authored it (marker line) -- nothing
# else ever reverts that file, and a stale faked location is a real bug.
if [ -f "${PREFIX}/etc/geolocation" ] && grep -qi 'urnetwork' "${PREFIX}/etc/geolocation" 2>/dev/null; then
    run rm -f "${PREFIX}/etc/geolocation"
fi

# State: prompt (or flags). Non-interactive default is KEEP.
remove_state=0
if [ "${PURGE}" = '1' ]; then
    remove_state=1
elif [ "${PURGE}" = '0' ]; then
    remove_state=0
elif [ "${ASSUME_YES}" = 1 ]; then
    remove_state=1
elif [ -t 0 ]; then
    printf 'Also remove daemon state (/var/lib/urnetwork, /etc/urnetwork, the urnetwork group)? [y/N] '
    read -r reply
    case "${reply}" in y|Y|yes|YES) remove_state=1 ;; esac
else
    log "keeping state (/var/lib/urnetwork, /etc/urnetwork, urnetwork group); re-run with --purge to remove it"
fi

if [ "${remove_state}" = 1 ]; then
    run rm -rf "${PREFIX}/var/lib/urnetwork"
    run rm -rf "${PREFIX}/etc/urnetwork"
    if [ -z "${PREFIX}" ] && getent group urnetwork >/dev/null 2>&1; then
        if command -v groupdel >/dev/null 2>&1; then
            run groupdel urnetwork
        elif command -v delgroup >/dev/null 2>&1; then
            run delgroup --system urnetwork
        fi
    fi
fi

# The SELinux policy module install.sh built and loaded on an enforcing host.
# It is an integration artifact, not user state, so it goes with the files
# rather than behind the state prompt -- and nothing else on the system will
# ever remove it. Hosts with no SELinux (Arch, CachyOS, Debian, Ubuntu) have no
# semodule and skip this entirely; `semodule -l` is read-only, so a host that
# never had the module installed does nothing either.
if [ -z "${PREFIX}" ] && command -v semodule >/dev/null 2>&1; then
    if semodule -l 2>/dev/null | grep -qx 'urnetwork'; then
        log "removing the 'urnetwork' SELinux policy module..."
        run semodule -r urnetwork || \
            warn "could not remove the SELinux policy module; remove it with: sudo semodule -r urnetwork"
    fi
fi

# /run/urnetwork survives a stop on purpose (RuntimeDirectoryPreserve=yes in the
# unit, so a crash-restart keeps the kill-switch marker). With the daemon gone
# that directory is just a leftover holding a marker nothing will ever read.
if [ -z "${PREFIX}" ] && [ -d /run/urnetwork ]; then
    run rm -rf /run/urnetwork
fi

if [ -z "${PREFIX}" ] && [ -d /run/systemd/system ]; then
    run systemctl daemon-reload
fi

# Rerun what dpkg triggers would have: without this, mimeinfo.cache keeps
# resolving urnetwork:// to a desktop file that no longer exists.
if [ -z "${PREFIX}" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        [ "${DRY_RUN}" = 1 ] || update-desktop-database -q "${SHARE_DIR}/applications" || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        [ "${DRY_RUN}" = 1 ] || gtk-update-icon-cache -q -t -f "${SHARE_DIR}/icons/hicolor" 2>/dev/null || true
    fi
fi

# Bookkeeping last; /usr/lib/urnetwork holds this very script -- unlinking a
# running bash script is safe (the inode outlives the unlink).
run rm -rf "${PREFIX}${LIB_DIR}"

if [ "${DRY_RUN}" = 1 ]; then
    log "[dry-run] no changes were made."
else
    log "urnetwork-daemon removed."
    if [ "${remove_state}" = 1 ]; then
        log "State was removed too."
    else
        log "State kept: /var/lib/urnetwork, /etc/urnetwork, and the urnetwork group survive a reinstall."
    fi
fi
