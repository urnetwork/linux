#!/bin/bash
# Build urnetwork-daemon-<version>-<arch>.install.tar.gz (MIGRATION.md
# normative name) -- the native install/upgrade channel:
#
#   curl -fsSL https://get.ur.network/urnetwork-daemon.tar.gz | tar xz \
#       && sudo urnetwork-daemon/install.sh
#
# Layout (normative): a SINGLE top-level directory `urnetwork-daemon/` --
# never a tarbomb -- containing install.sh, uninstall.sh, VERSION and a
# payload/ tree mirroring the installed paths. The script ships INSIDE the
# tarball so it can never run against a payload it was not built with
# (APPIMAGE.md 11g).
#
# Pipeline entry point (name and invocation pinned in MIGRATION.md): called
# with VERSION, ARCH (amd64|arm64), STAGING_DIR (the `meson install --destdir`
# tree) and OUT_DIR in the environment; writes
# urnetwork-daemon-${VERSION}-${ARCH}.install.tar.gz into $OUT_DIR. Runs on
# macOS or Linux. Flags override the environment for manual runs:
#   make-install-tarball.sh [--staging <dir>] [--arch <a>] [--out <dir>] \
#                           [--version <v>]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

usage() {
    sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

STAGING="${STAGING_DIR:-}"
ARCH="${ARCH:-}"
OUT="${OUT_DIR:-}"
VERSION="${VERSION:-${EXTERNAL_WARP_VERSION:-}}"
while [ $# -gt 0 ]; do
    case "$1" in
        --staging) STAGING="${2:?}"; shift 2 ;;
        --arch)    ARCH="${2:?}"; shift 2 ;;
        --out)     OUT="${2:?}"; shift 2 ;;
        --version) VERSION="${2:?}"; shift 2 ;;
        -h|--help) usage 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
done

[ -n "${STAGING}" ] || die "STAGING_DIR (or --staging) is required: the meson install --destdir tree"
[ -n "${OUT}" ] || die "OUT_DIR (or --out) is required"
[ -n "${VERSION}" ] || die "VERSION (or --version) is required"
case "${ARCH}" in
    amd64|arm64) ;;
    *) die "ARCH (or --arch) must be amd64 or arm64 (got '${ARCH:-<empty>}')" ;;
esac

STAGING="$(cd "${STAGING}" && pwd)"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

TMP_BASE="${TMPDIR:-/tmp}"; TMP_BASE="${TMP_BASE%/}"
WORK="$(mktemp -d "${TMP_BASE}/urnetwork-tarball.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
TOP="${WORK}/urnetwork-daemon"

install -d "${TOP}/payload"
assemble_daemon_root "${STAGING}" "${TOP}/payload"
check_payload_arch "${TOP}/payload" "${ARCH}"

install -m 0755 "${SCRIPT_DIR}/tarball/install.sh" "${TOP}/install.sh"
install -m 0755 "${SCRIPT_DIR}/tarball/uninstall.sh" "${TOP}/uninstall.sh"
printf '%s\n' "${VERSION}" > "${TOP}/VERSION"

TARBALL="${OUT}/urnetwork-daemon-${VERSION}-${ARCH}.install.tar.gz"

# Deterministic-ish archive: root ownership either way; GNU tar (Linux/CI)
# additionally gets stable ordering and mtimes.
if tar --version 2>/dev/null | grep -q 'GNU tar'; then
    tar --owner=root:0 --group=root:0 --sort=name \
        --mtime="@${SOURCE_DATE_EPOCH:-0}" \
        -czf "${TARBALL}" -C "${WORK}" urnetwork-daemon
else
    # bsdtar (macOS). --no-xattrs/--no-mac-metadata matter: without them the
    # archive carries LIBARCHIVE.xattr.com.apple.provenance headers and every
    # Linux user's `tar xz` prints an "Ignoring unknown extended header"
    # warning per file (caught in the debian:bookworm verification run).
    tar --no-xattrs --no-acls --no-fflags --no-mac-metadata \
        --uid 0 --gid 0 --uname root --gname root \
        -czf "${TARBALL}" -C "${WORK}" urnetwork-daemon
fi

# Prove the single-top-level-directory contract before publishing anything.
TOPLEVELS="$(tar tzf "${TARBALL}" | sed 's|/.*||' | sort -u)"
if [ "${TOPLEVELS}" != 'urnetwork-daemon' ]; then
    die "tarball has unexpected top-level entries: ${TOPLEVELS}"
fi

sha256_file "${TARBALL}"
maybe_sign "${TARBALL}"
log "built: ${TARBALL}"
log "top-level directory verified: urnetwork-daemon/ only"
