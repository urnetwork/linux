#!/bin/bash
# Build urnetwork-daemon_<version>_<arch>.deb (MIGRATION.md normative name)
# from a `meson install --destdir` staging tree, with nfpm.
#
# Runs on any host, including the macOS build server: nfpm is pure Go, needs
# no dpkg toolchain and no fakeroot (file ownership is declarative root:root
# regardless of the build uid), and the same nfpm.yaml later emits the .rpm.
# That is why nfpm over dpkg-deb.
#
# Pipeline entry point (name and invocation pinned in MIGRATION.md): called
# with VERSION, ARCH (amd64|arm64), STAGING_DIR (the `meson install --destdir`
# tree) and OUT_DIR in the environment; writes
# urnetwork-daemon_${VERSION}_${ARCH}.deb into $OUT_DIR. VERSION itself
# contains a '-' (e.g. 2026.7.6-985989570); dpkg reads that as
# upstream-revision, which is fine. Flags override the environment for
# manual runs:
#   make-deb.sh [--staging <dir>] [--arch <a>] [--out <dir>] [--version <v>]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

usage() {
    sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
command -v nfpm >/dev/null 2>&1 || die \
    "nfpm not found -- install with: brew install nfpm  (or: go install github.com/goreleaser/nfpm/v2/cmd/nfpm@latest)"

STAGING="$(cd "${STAGING}" && pwd)"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

# ${TMPDIR%/}: macOS TMPDIR ends in '/', and the doubled slash breaks nfpm's
# glob matching of contents[].src.
TMP_BASE="${TMPDIR:-/tmp}"; TMP_BASE="${TMP_BASE%/}"
PKGROOT="$(mktemp -d "${TMP_BASE}/urnetwork-deb-root.XXXXXX")"
trap 'rm -rf "${PKGROOT}"' EXIT

assemble_daemon_root "${STAGING}" "${PKGROOT}"
check_payload_arch "${PKGROOT}" "${ARCH}"

DEB="${OUT}/urnetwork-daemon_${VERSION}_${ARCH}.deb"
# VERSION contains a '-' (2026.7.6-985989570). Split at the LAST hyphen into
# nfpm version+release so the control file says exactly
# "Version: 2026.7.6-985989570"; see the comment in deb/nfpm.yaml.
VERSION_BASE="${VERSION}"
VERSION_RELEASE=''
case "${VERSION}" in
    *-*) VERSION_BASE="${VERSION%-*}"; VERSION_RELEASE="${VERSION##*-}" ;;
esac
# Render the concrete nfpm config: nfpm does not expand environment variables
# in contents[].src (verified, v2.47), so substitute the placeholders here.
NFPM_CONF="${PKGROOT}/.nfpm.yaml"
sed -e "s|\${VERSION_BASE}|${VERSION_BASE}|g" \
    -e "s|\${VERSION_RELEASE}|${VERSION_RELEASE}|g" \
    -e "s|\${DEB_ARCH}|${ARCH}|g" \
    -e "s|\${PKGROOT}|${PKGROOT}|g" \
    "${SCRIPT_DIR}/deb/nfpm.yaml" > "${NFPM_CONF}"
# cwd matters: nfpm resolves the config's relative paths (scripts/postinst)
# against the working directory.
(cd "${SCRIPT_DIR}/deb" && nfpm package -f "${NFPM_CONF}" -p deb -t "${DEB}")

# Best-effort verification where the tooling exists (a Linux box or dpkg via
# brew); the build itself never depends on it.
#
# Do NOT pipe dpkg-deb into head. It forks tar, and when head closes the pipe
# tar dies of SIGPIPE; dpkg-deb reports that as exit 2, `pipefail` propagates
# it, and `set -e` then kills this script AFTER the .deb is written but BEFORE
# sha256_file/maybe_sign -- so the build fails with the artifact present and
# unsigned. It only bites on packages with more entries than the head limit
# (this one has 123) and only where dpkg-deb exists, so it never reproduces on
# the macOS build host: it aborted every Linux build instead. Write the listing
# out first, then truncate the file.
if command -v dpkg-deb >/dev/null 2>&1; then
    dpkg-deb --info "${DEB}"
    _contents="$(mktemp)"
    dpkg-deb --contents "${DEB}" >"${_contents}"
    head -40 "${_contents}"
    _total="$(wc -l <"${_contents}" | tr -d ' ')"
    if [ "${_total}" -gt 40 ]; then
        log "(${_total} entries total; listing truncated to 40)"
    fi
    rm -f "${_contents}"
fi

sha256_file "${DEB}"
maybe_sign "${DEB}"
log "built: ${DEB}"
