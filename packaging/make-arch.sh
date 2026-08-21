#!/bin/bash
# Build urnetwork-daemon-<version>-<arch>.pkg.tar.zst -- the ARCH-FAMILY native
# channel (Arch, CachyOS, EndeavourOS, Manjaro), and the format this project
# answered with a tarball because nothing else existed.
#
# Same shape, same input contract and same entry-point convention as
# packaging/make-deb.sh and packaging/make-rpm.sh: called with VERSION, ARCH
# (amd64|arm64), STAGING_DIR (the `meson install --destdir` tree) and OUT_DIR
# in the environment. Flags override the environment for manual runs:
#   make-arch.sh [--staging <dir>] [--arch <a>] [--out <dir>] [--version <v>]
#
# WHY nfpm AND NOT A PKGBUILD: the long answer is in the header of
# packaging/arch/nfpm.yaml. The short one is the same as the .rpm's: all three
# native packages are assembled from ONE `meson install --destdir` staging tree
# through packaging/lib/common.sh assemble_daemon_root(), so they cannot drift.
# A PKGBUILD would be a fourth, independent copy of the installed-path table --
# and it would also need an Arch machine or a container to run makepkg, which
# neither the macOS build server nor the workflow's ubuntu:22.04 container is.
# nfpm is pure Go down to its zstd, so this runs anywhere the .deb does.
#
# THREE THINGS THIS SCRIPT DOES THAT make-deb.sh DOES NOT
# -------------------------------------------------------
# 1. It MOVES THE UNIT to /usr/lib/systemd/system, like make-rpm.sh, but for a
#    harder reason than rpm's: on Arch /lib is a SYMLINK to usr/lib owned by
#    the `filesystem` package, and a .pkg.tar.zst carrying any member under
#    lib/ does not merely offend a guideline -- pacman ABORTS the transaction
#    with "/lib exists in filesystem (owned by filesystem)" and installs
#    nothing. It is also the path Arch's own alpm hooks watch
#    (30-systemd-daemon-reload-system.hook targets usr/lib/systemd/system/*).
# 2. It FOLDS THE WHOLE VERSION INTO pkgver, because pacman's pkgver may not
#    contain '-' at all and nfpm silently destroys the other obvious shapes.
#    See pkg_fields(), which is a THIRD different answer from make-deb.sh's
#    last-hyphen split and make-rpm.sh's first-hyphen split.
# 3. It PINS ONE TIMESTAMP AND VERIFIES THE .MTREE, because `pacman -Qkk` --
#    the integrity check a careful user runs against a VPN daemon -- is broken
#    by two separate nfpm behaviours and both are silent:
#      * `type: tree` writes Go's unmasked fs.FileMode (mode=20000000755) for
#        every directory, so every directory reports as altered forever. The
#        config uses a glob instead; this script asserts the fix held.
#      * the tar header mtime and .MTREE's `time=` come from different clocks,
#        so a one-second gap between assembling the root and writing the
#        archive makes every FILE report as altered. This script pins one
#        timestamp (SOURCE_DATE_EPOCH when set) and asserts every .MTREE entry
#        carries it. That is intermittent otherwise -- a build where both land
#        in the same second passes by luck -- which is exactly why it is
#        asserted rather than reviewed.
#
# ENVIRONMENT KNOBS (all optional)
#   UR_ARCH_CANONICAL_NAME=1   name the output the canonical pacman
#                              <pkgname>-<pkgver>-<pkgrel>-<arch>.pkg.tar.zst
#                              instead of the release-asset name. For building
#                              a real pacman repo with repo-add; NOT for CI
#                              (see "THE OUTPUT NAME").
#   UR_SIGN_KEY=<keyid>        as in make-deb.sh/make-rpm.sh: a DETACHED .asc
#                              beside the artifact. NOTE this is NOT a pacman
#                              package signature -- see the note at the bottom.
#
# WHAT THIS SCRIPT CANNOT TELL YOU. It builds and inspects a package; it never
# installs one. Every function in packaging/arch/scripts/ is unexecuted until a
# real pacman runs it. Do not read a green build here as a green install.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

usage() {
    sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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

# The ASSET arch spelling stays Debian (amd64/arm64) on the way IN -- it is the
# contract at the top of .github/workflows/beta-build.yml, every packaging
# script here takes the same --arch values, and common.sh's check_payload_arch
# understands only those two. The PACMAN arch spelling is separate, and it is
# what goes in .PKGINFO and in the artifact name.
case "${ARCH}" in
    amd64) PKG_ARCH='x86_64' ;;
    arm64) PKG_ARCH='aarch64' ;;
    *) die "ARCH (or --arch) must be amd64 or arm64 (got '${ARCH:-<empty>}')" ;;
esac

command -v nfpm >/dev/null 2>&1 || die \
    "nfpm not found -- install with: brew install nfpm  (or: go install github.com/goreleaser/nfpm/v2/cmd/nfpm@latest)"

STAGING="$(cd "${STAGING}" && pwd)"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

# ---------------------------------------------------------------------------
# VERSION -> pacman pkgver / pkgrel
# ---------------------------------------------------------------------------
# THE THIRD DIFFERENT ANSWER, AND IT HAS TO BE. The .deb's '~' lesson and the
# .rpm's Version/Release lesson recur here a third time, and no two of the
# three scripts derive their version fields the same way:
#
#   make-deb.sh   splits at the LAST hyphen   -> 2026.8.20-1024376890-beta
#   make-rpm.sh   splits at the FIRST hyphen  -> 2026.8.20 + 0.1024376890.beta
#   make-arch.sh  splits at NEITHER           -> 2026.8.20.1024376890.beta-1
#
# pacman's pkgver may not contain '-' AT ALL: '-' is the pkgver/pkgrel
# separator and pacman splits at the LAST one, rejecting anything else with
# "package version contains invalid characters / invalid or corrupted package".
# So the whole VERSION is folded to dots and pkgrel is pinned to the literal 1;
# the build code lives inside pkgver, so pkgrel never has to move.
#
# THE TWO SHAPES THAT LOOK RIGHT AND ARE NOT (both measured on nfpm 2.47.0):
#   * version=2026.8.20, release="1024376890.beta" -- the rpm shape. nfpm
#     coerces a non-integer release to 1 SILENTLY and emits
#     `pkgver = 2026.8.20-1`. The build code and the beta marker are gone, the
#     package still builds and still installs, and every build of a given day
#     becomes the same pacman version: `pacman -U` of a newer build then says
#     "is up to date -- reinstalling" instead of upgrading. A filename that
#     lies about its own contents is worse than a build failure.
#   * the raw VERSION with version_schema: none -- emits
#     `pkgver = 2026.8.20-1024376890-beta-1`, which nfpm writes without
#     complaint and pacman refuses to install at all.
#
# THE ALPHABET IS rpm's MINUS '~', and that is deliberate, not an oversight:
#   * alpm does not treat '~' the dpkg way. `vercmp 1.0~beta-1 1.0-1` -> 1, so
#     on Arch '~' sorts ABOVE -- the exact inverse of the lesson recorded in
#     packaging/deb/nfpm.yaml. Carrying the trick over would import something
#     that does the opposite of what its comment says.
#   * nfpm strips '~', ':' and spaces from the FILENAME it generates while
#     writing them verbatim into .PKGINFO, so "1.0~beta" and "1.0 beta" collide
#     on one filename in a single output directory.
#
# ORDERING, verified with vercmp against the real pipeline shapes (alpm
# compares each dot-separated segment numerically, so both rollovers work):
#   2026.8.9.1020000000.beta-1  < 2026.8.20.1024376890.beta-1
#   2026.8.20.1024351940.beta-1 < 2026.8.20.1024376890.beta-1
#   2026.8.20.1024376890.beta-1 < 2026.9.1.1030000000.beta-1
#
# ONE HONEST ASYMMETRY WITH THE .rpm, written down rather than left to be
# rediscovered: `vercmp 2026.8.20.1024376890.beta-1 2026.8.20.1024376890-1`
# -> 1, i.e. the '.beta' suffix sorts ABOVE the same version without it, the
# inverse of make-rpm.sh's deliberate "0."-prefixed Release. It is harmless
# because the build code is monotonic and unique per pipeline run, so no two
# artifacts ever differ only by that suffix. And attribute it correctly: the
# rpm's "0." trick is unavailable because NFPM coerces the release to an
# integer, NOT because pacman requires one -- pacman accepts a fractional
# pkgrel (Arch's own convention allows 1.1 for a minor bump). If ordering ever
# needs a hard reset, nfpm's archlinux packager does support `epoch:` and emits
# it correctly; that, not a fractional pkgrel, is the escape hatch.
pkg_fields() {
    local v="$1" ver
    ver="$(printf '%s' "${v}" | LC_ALL=C sed -e 's/[^0-9A-Za-z.+_]/./g' -e 's/\.\{2,\}/./g' -e 's/^\.//' -e 's/\.$//')"
    [ -n "${ver}" ] || die "cannot derive a pacman pkgver from '${v}'"
    printf '%s 1' "${ver}"
}
PKG_VERSION=''; PKG_RELEASE=''
# `die` inside a command substitution only kills the SUBSHELL, so check the
# result rather than trusting set -e to have stopped us (make-rpm.sh's lesson).
read -r PKG_VERSION PKG_RELEASE <<<"$(pkg_fields "${VERSION}")" || true
[ -n "${PKG_VERSION}" ] && [ -n "${PKG_RELEASE}" ] || \
    die "could not derive a pacman pkgver/pkgrel from VERSION='${VERSION}'"
case "${PKG_VERSION}" in
    *-*) die "pkgver '${PKG_VERSION}' still contains '-' -- pacman would reject the package outright (pkgver may not contain the pkgver/pkgrel separator)" ;;
esac
log "version: ${VERSION}  ->  pacman pkgver=${PKG_VERSION} pkgrel=${PKG_RELEASE}"

# ---------------------------------------------------------------------------
# Package root
# ---------------------------------------------------------------------------
# ${TMPDIR%/}: macOS TMPDIR ends in '/', and the doubled slash breaks nfpm's
# glob matching of contents[].src -- which this config depends on far more than
# its siblings do, since the whole /usr tree is a glob here.
TMP_BASE="${TMPDIR:-/tmp}"; TMP_BASE="${TMP_BASE%/}"
PKGROOT="$(mktemp -d "${TMP_BASE}/urnetwork-arch-root.XXXXXX")"
trap 'rm -rf "${PKGROOT}"' EXIT

assemble_daemon_root "${STAGING}" "${PKGROOT}"
check_payload_arch "${PKGROOT}" "${ARCH}"

# --- the unit moves out of /lib, and this one is a HARD BLOCKER -------------
# common.sh stages the unit at lib/systemd/system, which is right for dpkg and
# fatal for pacman. Measured in a real Arch container with the unit left there:
#     error: failed to commit transaction (conflicting files)
#     urnetwork-daemon: /lib exists in filesystem (owned by filesystem)
# and nothing was installed -- because /lib is a SYMLINK to usr/lib owned by
# the `filesystem` package. (pacman tolerates dir-vs-dir overlap: after a
# successful install /usr/lib/systemd/system is co-owned by a dozen packages.
# A symlink is the case it refuses.)
#
# There is no YAML-level dodge: nfpm emits an implicit parent-directory member
# for every dst, so declaring the unit as one plain file at
# /lib/systemd/system/urnetworkd.service still produces lib/, lib/systemd/ and
# lib/systemd/system/ members. The relocation has to happen HERE, in
# ${PKGROOT}, before nfpm runs.
#
# IT MUST NOT BE FIXED IN common.sh. packaging/deb/nfpm.yaml declares
# `src: ${PKGROOT}/lib, type: tree`, and nfpm exits 1 with "Add tree: lstat
# .../lib: no such file or directory" the moment that directory stops being
# staged -- so moving this into assemble_daemon_root would break the .deb.
# Per-script remap keeps this additive, exactly as the rpm target already is.
#
# NOT marked config anywhere: /usr/lib/systemd/system is the vendor unit
# directory, admin overrides live in /etc/systemd/system (drop-ins, `systemctl
# edit`), and pacman never writes there.
[ -f "${PKGROOT}/lib/systemd/system/urnetworkd.service" ] || \
    die "assemble_daemon_root did not stage lib/systemd/system/urnetworkd.service -- packaging/lib/common.sh changed shape"
install -d "${PKGROOT}/usr/lib/systemd/system"
install -m 0644 "${PKGROOT}/lib/systemd/system/urnetworkd.service" \
    "${PKGROOT}/usr/lib/systemd/system/urnetworkd.service"
rm -f "${PKGROOT}/lib/systemd/system/urnetworkd.service"
# Nothing else may be dropped on the floor. `! -type d` rather than the rpm's
# `-type f`, because a symlink or an empty subtree common.sh might add later
# would slip past a file-only test and be silently deleted by the rm below.
LIB_LEFTOVERS="$(find "${PKGROOT:?}/lib" -mindepth 1 ! -type d 2>/dev/null || true)"
[ -z "${LIB_LEFTOVERS}" ] || die "assemble_daemon_root staged files under /lib that this script does not know how to remap:
${LIB_LEFTOVERS}
Decide where they belong under /usr (pacman REFUSES any package member under
/lib -- it is a symlink owned by the filesystem package) and extend the remap
above."
rm -rf "${PKGROOT:?}/lib"

# ---------------------------------------------------------------------------
# ONE TIMESTAMP FOR THE WHOLE PACKAGE
# ---------------------------------------------------------------------------
# See the `mtime:` comment in packaging/arch/nfpm.yaml for what goes wrong
# without this: nfpm takes each tar header's mtime from when it writes the
# archive and each .MTREE `time=` from the source file's ModTime, pacman sets
# the installed file's mtime from the former and checks it against the latter,
# and a one-second gap between assemble_daemon_root's copy and nfpm's write
# makes `pacman -Qkk` report every file as altered, permanently. Pinning one
# value collapses both fields onto it.
#
# SOURCE_DATE_EPOCH is honoured because it is the reproducible-builds
# convention and because it is the only way two builds of the same input can
# produce the same bytes here. `date -u -d @N` is GNU and `date -u -r N` is
# BSD; this script also runs on the macOS build server, so try both rather
# than assume (make-rpm.sh learned the same lesson about sed's '\n').
PKG_MTIME_EPOCH="${SOURCE_DATE_EPOCH:-$(date -u +%s)}"
case "${PKG_MTIME_EPOCH}" in
    ''|*[!0-9]*) die "SOURCE_DATE_EPOCH must be a whole number of seconds (got '${PKG_MTIME_EPOCH}')" ;;
esac
PKG_MTIME="$(date -u -d "@${PKG_MTIME_EPOCH}" '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null \
          || date -u -r "${PKG_MTIME_EPOCH}" '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || true)"
[ -n "${PKG_MTIME}" ] || die "neither 'date -u -d @N' (GNU) nor 'date -u -r N' (BSD) works on this host -- cannot pin the package timestamp"
log "timestamp: ${PKG_MTIME} (epoch ${PKG_MTIME_EPOCH}${SOURCE_DATE_EPOCH:+, from SOURCE_DATE_EPOCH})"

# ---------------------------------------------------------------------------
# Render the concrete nfpm config and build
# ---------------------------------------------------------------------------
# nfpm does not expand environment variables in contents[].src (verified for
# the .deb against nfpm v2.47, the version pinned in beta-build.yml), so
# substitute here.
NFPM_CONF="${PKGROOT}/.nfpm.yaml"
sed -e "s|\${PKG_VERSION}|${PKG_VERSION}|g" \
    -e "s|\${PKG_RELEASE}|${PKG_RELEASE}|g" \
    -e "s|\${PKG_ARCH}|${PKG_ARCH}|g" \
    -e "s|\${PKG_MTIME}|${PKG_MTIME}|g" \
    -e "s|\${NFPM_ARCH}|${ARCH}|g" \
    -e "s|\${PKGROOT}|${PKGROOT}|g" \
    "${SCRIPT_DIR}/arch/nfpm.yaml" > "${NFPM_CONF}"

# THE OUTPUT NAME.
#
# The release-asset contract (top of .github/workflows/beta-build.yml) is that
# the FULL version string is discoverable in the asset name, because the
# in-app checker matches assets on the version it is looking for. Like the
# .rpm, this package keeps that information in its own punctuation and with its
# own arch spelling:
#
#     urnetwork-daemon-2026.8.20-1024376890-beta-x86_64.pkg.tar.zst
#
# `pacman -U ./file.pkg.tar.zst` does not care about the filename -- pacman
# reads .PKGINFO, exactly as rpm reads the header -- so the asset name is free
# to serve the checker.
#
# A pacman REPOSITORY is the case that does care: repo-add records the file
# name it finds, and mirrors and humans expect the canonical
# <pkgname>-<pkgver>-<pkgrel>-<arch>.pkg.tar.zst. UR_ARCH_CANONICAL_NAME=1
# emits that instead. The canonical name is printed on every build either way,
# so whoever builds the repo never has to derive it.
CANONICAL="urnetwork-daemon-${PKG_VERSION}-${PKG_RELEASE}-${PKG_ARCH}.pkg.tar.zst"
if [ "${UR_ARCH_CANONICAL_NAME:-0}" = 1 ]; then
    PKG="${OUT}/${CANONICAL}"
else
    PKG="${OUT}/urnetwork-daemon-${VERSION}-${PKG_ARCH}.pkg.tar.zst"
fi

# cwd matters: nfpm resolves the config's relative script paths
# (scripts/post_install, ...) against the working directory.
(cd "${SCRIPT_DIR}/arch" && nfpm package -f "${NFPM_CONF}" -p archlinux -t "${PKG}")

log "canonical pacman name (what repo-add expects): ${CANONICAL}"

# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------
# UNLIKE make-deb.sh and make-rpm.sh, this does NOT need the target
# distribution's tools. A .pkg.tar.zst is a plain zstd-compressed tar whose
# first three members are the metadata files, so tar alone reads everything
# that matters: .PKGINFO (the dependency and version metadata pacman actually
# reads), .INSTALL (the six hook functions) and the payload listing. There is
# no `pacman -Qp` equivalent needed and none available -- but there is also no
# excuse for skipping the assertions the way the rpm path has to when rpm is
# absent.
#
# The one external requirement is zstd, because nfpm's output is zstd and GNU
# tar shells out for it. Every fallback is tried before giving up.
PKG_TAR=''
if tar --zstd -tf "${PKG}" >/dev/null 2>&1; then
    PKG_TAR='tar --zstd'
elif command -v bsdtar >/dev/null 2>&1 && bsdtar -tf "${PKG}" >/dev/null 2>&1; then
    PKG_TAR='bsdtar'
fi

if [ -n "${PKG_TAR}" ]; then
    WORK="$(mktemp -d "${TMP_BASE}/urnetwork-arch-verify.XXXXXX")"
    # shellcheck disable=SC2064  # WORK is expanded now on purpose
    trap "rm -rf '${PKGROOT}' '${WORK}'" EXIT
    ${PKG_TAR} -xf "${PKG}" -C "${WORK}" .PKGINFO .INSTALL
    listing="${WORK}/listing"
    ${PKG_TAR} -tf "${PKG}" > "${listing}"

    fail=0
    log ""
    log "--- .PKGINFO ---"
    cat "${WORK}/.PKGINFO"

    # The metadata pacman reads, asserted rather than eyeballed.
    grep -qx "pkgname = urnetwork-daemon" "${WORK}/.PKGINFO" || { warn ".PKGINFO pkgname is not urnetwork-daemon"; fail=1; }
    grep -qx "pkgver = ${PKG_VERSION}-${PKG_RELEASE}" "${WORK}/.PKGINFO" || {
        warn ".PKGINFO pkgver is not '${PKG_VERSION}-${PKG_RELEASE}' -- nfpm re-parsed the version (is version_schema: none still set?)"
        fail=1; }
    grep -qx "arch = ${PKG_ARCH}" "${WORK}/.PKGINFO" || { warn ".PKGINFO arch is not ${PKG_ARCH}"; fail=1; }
    # `maintainer:` does NOT feed this field; only archlinux.packager does, and
    # losing it prints "Unknown Packager" in every `pacman -Qi`.
    grep -q '^packager = URnetwork' "${WORK}/.PKGINFO" || {
        warn ".PKGINFO packager is not set (archlinux.packager missing from the config -- 'Unknown Packager')"; fail=1; }
    # The three conffiles must be pacman `backup =` entries, or an admin's edits
    # are overwritten on every upgrade instead of landing as .pacnew.
    for want in etc/urnetwork/autostart/com.bringyour.network.desktop \
                etc/NetworkManager/conf.d/95-urnetwork.conf \
                etc/udev/rules.d/85-urnetwork-unmanaged.rules ; do
        grep -qx "backup = ${want}" "${WORK}/.PKGINFO" || { warn "${want} is not marked 'backup =' -- an upgrade would overwrite an admin's edits"; fail=1; }
    done
    # The dependencies that are the whole reason to ship a native package.
    for want in iproute2 nftables systemd shadow fuse2; do
        grep -qx "depend = ${want}" "${WORK}/.PKGINFO" || { warn "missing dependency: ${want}"; fail=1; }
    done
    # Debian syntax passes through nfpm VERBATIM and unvalidated, producing a
    # package that builds cleanly and can never be installed. Catch it here.
    if grep -E '^depend = ' "${WORK}/.PKGINFO" | grep -qE '\||\(|\)'; then
        warn "a dependency uses Debian alternation ('a | b') or Debian version syntax ('(>= x)') -- pacman resolves those as literal package names and the package becomes uninstallable:"
        grep -E '^depend = ' "${WORK}/.PKGINFO" | grep -E '\||\(|\)' >&2
        fail=1
    fi

    log ""
    log "--- .INSTALL hooks ---"
    # ALL SIX, and the two upgrade hooks are the ones that go missing silently.
    # nfpm's generic `scripts:` maps ONLY to the four install/remove functions;
    # pre_upgrade/post_upgrade come from the separate archlinux.scripts block,
    # and without them every `pacman -U` upgrade, every downgrade and every
    # reinstall runs NOTHING AT ALL while pacman reports success.
    for fn in pre_install post_install pre_upgrade post_upgrade pre_remove post_remove; do
        if grep -qE "^[[:space:]]*(function[[:space:]]+)?${fn}[[:space:]]*\(\)" "${WORK}/.INSTALL"; then
            log "  ${fn}: present"
        else
            warn "  ${fn}: MISSING from .INSTALL"
            # A plain `[ a ] || [ b ] && warn` here would be a set -e landmine:
            # when neither test matches, the compound's status is 1 and the
            # script would exit before reporting the other five hooks.
            case "${fn}" in
                pre_upgrade|post_upgrade)
                    warn "    (declare it under archlinux.scripts in packaging/arch/nfpm.yaml -- the top-level scripts: block cannot emit it)" ;;
            esac
            fail=1
        fi
    done
    # The deb/rpm argument conventions do not exist here. A `case "$1" in
    # configure)` or a `[ "$1" -eq 1 ]` copied from a sibling script never
    # matches anything pacman passes, and the package then installs with exit
    # code 0 having done nothing -- the exact silent failure that makes this
    # worth asserting on the built artifact rather than trusting a review.
    #
    # COMMENT LINES ARE STRIPPED FIRST, and that is not fussiness: nfpm inlines
    # each script file VERBATIM into its function, comments included, and
    # packaging/arch/scripts/ discusses both wrong conventions at length in
    # order to warn the next reader off them. Grepping the raw .INSTALL made
    # this check fire on its own documentation.
    _code="${WORK}/.INSTALL.code"
    grep -v '^[[:space:]]*#' "${WORK}/.INSTALL" > "${_code}" || true
    if grep -qE 'configure\)|"\$1" = "(remove|purge|configure)"' "${_code}"; then
        warn ".INSTALL branches on dpkg's 'configure'/'remove'/'purge' -- pacman passes version strings and that branch can never match"
        fail=1
    fi
    if grep -qE '\[ *"?\$\{?1[:-]?[^ ]*"? *-(eq|ge|le|gt|lt) *[0-9]' "${_code}"; then
        warn ".INSTALL compares \$1 numerically like an rpm scriptlet -- pacman passes version strings, not 0/1/2"
        fail=1
    fi

    log ""
    log "--- payload ($(grep -cv '/$' "${listing}") file entries) ---"
    head -25 "${listing}"

    # The files whose absence is silent and fatal. A file count alone passes a
    # package that is all locale catalogs and no daemon -- the shape of failure
    # the Windows MSI shipped for months.
    for want in \
        usr/lib/urnetwork/urnetworkd \
        usr/lib/urnetwork/libURnetworkSdk.so \
        usr/bin/urnetwork \
        usr/lib/systemd/system/urnetworkd.service \
        usr/share/polkit-1/actions/com.bringyour.network.policy \
        usr/share/urnetwork/world-110m.json ; do
        grep -Fxq "${want}" "${listing}" || { warn "MISSING from the payload: ${want}"; fail=1; }
    done
    # THE HARD BLOCKER, asserted in the built artifact rather than trusted from
    # the remap above. Widened past /lib on purpose: /bin, /sbin and /lib64 are
    # every one of them filesystem-owned symlinks on Arch and would abort the
    # transaction in exactly the same way.
    if grep -Eq '^(bin|sbin|lib|lib64)/' "${listing}"; then
        warn "the package ships members under a filesystem-owned symlink (/bin, /sbin, /lib or /lib64). pacman ABORTS the whole transaction with 'exists in filesystem (owned by filesystem)' and installs nothing:"
        grep -E '^(bin|sbin|lib|lib64)/' "${listing}" >&2
        fail=1
    fi

    # THE .MTREE DIRECTORY MODES. nfpm's `type: tree` writes Go's unmasked
    # fs.FileMode into .MTREE -- mode=20000000755, i.e. fs.ModeDir|0755
    # formatted with %o and never masked to permission bits. The visible
    # results are one "warning: directory permissions differ on <dir> /
    # filesystem: 755  package: 755" per directory at install time (both sides
    # printing 755 is the tell) and `pacman -Qkk urnetwork-daemon` reporting
    # those directories as altered FOREVER -- which, for a VPN daemon, is
    # exactly the check a careful user runs. The config avoids it with a glob
    # instead of a tree; assert that it stayed that way.
    ${PKG_TAR} -xf "${PKG}" -C "${WORK}" .MTREE 2>/dev/null || true
    if [ -f "${WORK}/.MTREE" ]; then
        folded="$(gzip -cd "${WORK}/.MTREE" 2>/dev/null | grep -c 'mode=2[0-9]\{10\}' || true)"
        if [ "${folded:-0}" -gt 0 ]; then
            warn ".MTREE carries ${folded} directory entries with an unmasked Go file mode (mode=20000000755)."
            warn "Every one of them makes pacman print a bogus 'directory permissions differ' warning at install"
            warn "time and shows up as an altered file under 'pacman -Qkk' forever. This is what nfpm's"
            warn "'type: tree' does; packaging/arch/nfpm.yaml must use the '\${PKGROOT}/usr/**/*' glob instead."
            warn "Do NOT 'fix' it with file_info.mode on the tree entry -- that clobbers every FILE to 0755 too."
            fail=1
        else
            log "mtree: no unmasked directory modes"
        fi
        # THE OTHER HALF OF `pacman -Qkk`, and the half that is INTERMITTENT --
        # which is why it is asserted rather than reviewed. Every .MTREE entry
        # must carry the one pinned timestamp; if any carries the source file's
        # own ModTime instead, nfpm ignored `mtime:` and pacman will report
        # every file as "Modification time mismatch" on every installed
        # machine. A build where the copy and the archive write happen to land
        # in the same second passes by luck, so a spot check would not do.
        stray="$(gzip -cd "${WORK}/.MTREE" 2>/dev/null | grep -c "time=" || true)"
        pinned="$(gzip -cd "${WORK}/.MTREE" 2>/dev/null | grep -c "time=${PKG_MTIME_EPOCH}\." || true)"
        if [ "${stray:-0}" -ne "${pinned:-0}" ]; then
            warn ".MTREE has ${stray} timestamped entries but only ${pinned} carry the pinned time=${PKG_MTIME_EPOCH}."
            warn "nfpm did not honour 'mtime:' for all of them. pacman sets each file's mtime from the TAR"
            warn "header and checks it against .MTREE, so every mismatched entry becomes a permanent"
            warn "'Modification time mismatch' under 'pacman -Qkk urnetwork-daemon'."
            fail=1
        else
            log "mtree: all ${pinned} timestamps pinned to ${PKG_MTIME_EPOCH} (pacman -Qkk will be clean)"
        fi
    fi

    log ""
    [ "${fail}" = 0 ] || die "package verification failed (see the warnings above)"
    log "package verified."
else
    warn "cannot read ${PKG##*/}: no zstd-capable tar on this host (install zstd, or bsdtar)."
    warn "The package WAS built -- nfpm's zstd is pure Go -- but every payload, dependency and"
    warn "hook assertion above was skipped. Do not ship an unverified build from a host like this."
fi

log ""
log "NOT executed by this script, and only a real pacman can: the six functions in"
log "packaging/arch/scripts/. A green build here is not a green install."

sha256_file "${PKG}"
maybe_sign "${PKG}"
# Said explicitly because the word "signed" means something different here than
# it does for the .deb. UR_SIGN_KEY produces a DETACHED .asc beside the file,
# which proves authorship to a human. It is NOT a pacman package signature: a
# pacman repo with SigLevel=Required wants a detached <pkg>.sig made with the
# repo's key and listed in the database, and `pacman -U` of a local file
# ignores the .asc entirely.
if [ -z "${UR_SIGN_KEY:-}" ]; then
    log "not signed. Note that even with UR_SIGN_KEY this produces a detached .asc, NOT the <pkg>.sig a pacman repo with SigLevel=Required requires."
fi
log "built: ${PKG}"
