#!/bin/bash
# Build urnetwork-daemon-<version>.<rpm-arch>.rpm -- the Fedora-family native
# channel that docs/linux_agent_help.md 10.2/10.4 names as PRIMARY, and the one
# format this project had no answer for at all.
#
# Same shape, same input contract and same entry-point convention as
# packaging/make-deb.sh: called with VERSION, ARCH (amd64|arm64), STAGING_DIR
# (the `meson install --destdir` tree) and OUT_DIR in the environment. Flags
# override the environment for manual runs:
#   make-rpm.sh [--staging <dir>] [--arch <a>] [--out <dir>] [--version <v>]
#
# WHY nfpm AND NOT rpmbuild: the long answer is in the header of
# packaging/rpm/nfpm.yaml. The short one is that both packages are assembled
# from the same staging tree through packaging/lib/common.sh
# assemble_daemon_root(), so the .deb and the .rpm cannot drift; a .spec would
# be a second, independent copy of the installed-path table. nfpm is also pure
# Go, so this runs on the macOS build server and inside the workflow's
# ubuntu:22.04 container with no rpm toolchain and no mock.
#
# THREE THINGS THIS SCRIPT DOES THAT make-deb.sh DOES NOT
# -------------------------------------------------------
# 1. It MOVES THE UNIT to /usr/lib/systemd/system. common.sh stages it at
#    /lib/systemd/system, which is right for dpkg and wrong for rpm: on a
#    merged-usr distro /lib is a symlink to /usr/lib, and an rpm owning paths
#    through a symlink fights the `filesystem` package. 10.2 asks for
#    /usr/lib/systemd/system independently.
# 2. It BUILDS THE SELINUX POLICY MODULE and puts it in the package. This is
#    the Fedora-family package; without packaging/selinux/urnetwork.te the
#    daemon lands in init_t and the tunnel cannot open /dev/net/tun. That is
#    measured on the owner's Bazzite machine, not theoretical, so a missing
#    policy module is a HARD BUILD FAILURE here rather than a warning (see
#    UR_RPM_ALLOW_NO_SELINUX_PP for the deliberate escape hatch).
# 3. It SPLITS VERSION INTO rpm's Version/Release, because rpm forbids '-' in
#    either field and the pipeline's VERSION has two of them. See rpm_fields().
#
# ENVIRONMENT KNOBS (all optional)
#   UR_RPM_ALLOW_NO_SELINUX_PP=1   build without a prebuilt policy module when
#                                  checkmodule/semodule_package are absent.
#                                  The package then falls back to compiling
#                                  the shipped .te in %post, which needs
#                                  checkpolicy on the TARGET. Say yes only if
#                                  you understand that trade.
#   UR_RPM_CANONICAL_NAME=1        name the output rpm the canonical
#                                  NAME-VERSION-RELEASE.ARCH.rpm instead of the
#                                  release-asset name. For building a real dnf
#                                  repo; NOT for CI (see "THE OUTPUT NAME").
#   UR_RPM_SIGN_KEY_FILE=<path>    an exported GPG private key; when set, nfpm
#                                  signs the rpm HEADER so `gpgcheck=1` works.
#                                  Passphrase via NFPM_PASSPHRASE.
#   UR_SIGN_KEY=<keyid>            as in make-deb.sh: a DETACHED .asc beside
#                                  the artifact. Independent of the above --
#                                  a detached signature does not satisfy dnf.
#
# WHAT THIS SCRIPT CANNOT TELL YOU. It builds and inspects an rpm; it never
# installs one. Every scriptlet in packaging/rpm/scripts/ is unexecuted until
# a Fedora VM runs it. Do not read a green build here as a green install.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

usage() {
    sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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

# The ASSET arch spelling stays Debian (amd64/arm64) -- it is the contract at
# the top of .github/workflows/beta-build.yml and every packaging script here
# takes the same --arch values. The RPM arch spelling is separate.
case "${ARCH}" in
    amd64) RPM_ARCH='x86_64' ;;
    arm64) RPM_ARCH='aarch64' ;;
    *) die "ARCH (or --arch) must be amd64 or arm64 (got '${ARCH:-<empty>}')" ;;
esac

command -v nfpm >/dev/null 2>&1 || die \
    "nfpm not found -- install with: brew install nfpm  (or: go install github.com/goreleaser/nfpm/v2/cmd/nfpm@latest)"

STAGING="$(cd "${STAGING}" && pwd)"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

# ---------------------------------------------------------------------------
# VERSION -> rpm Version / Release
# ---------------------------------------------------------------------------
# rpm(8) forbids '-' in BOTH fields (it is the NVR field separator), and this
# pipeline's VERSION is "2026.8.16-1020679030-beta". So:
#
#   Version = everything before the FIRST '-'           -> 2026.8.16
#   Release = everything after it, with every character
#             rpm dislikes folded to '.'                -> 1020679030.beta
#             ...prefixed "0." when the tail names a
#             prerelease                                -> 0.1020679030.beta
#
# The "0." is the Fedora prerelease convention and it is load-bearing for
# ordering, not decoration. rpmvercmp splits a field into alternating numeric
# and alphabetic runs and compares numeric runs NUMERICALLY:
#
#   Version:  2026.8.9  <  2026.8.16          (9 < 16 numerically, not "9">"1")
#   Release:  0.1020679030.beta  <  0.1020679040.beta      (per-run numeric)
#   Release:  0.1020679030.beta  <  1020679030             (first run 0 < 10206...)
#
# so a later non-beta build with Release=<code> always outranks any beta, and
# the beta line itself is monotonic because the pipeline's <code> is monotonic
# (epoch delta * 10, derived once per run).
#
# NOTE the deliberate asymmetry with the .deb, which splits at the LAST hyphen
# to reproduce a Debian upstream-revision. Debian versions may contain '-';
# rpm versions may not, so the same input cannot produce the same split.
rpm_fields() {
    local v="$1" ver rel
    case "${v}" in
        *-*) ver="${v%%-*}"; rel="${v#*-}" ;;
        *)   ver="${v}";     rel='' ;;
    esac
    # Fold anything outside rpm's safe alphabet to '.', collapse runs, trim.
    ver="$(printf '%s' "${ver}" | LC_ALL=C sed -e 's/[^0-9A-Za-z.+_~]/./g' -e 's/\.\{2,\}/./g' -e 's/^\.//' -e 's/\.$//')"
    rel="$(printf '%s' "${rel}" | LC_ALL=C sed -e 's/[^0-9A-Za-z.+_~]/./g' -e 's/\.\{2,\}/./g' -e 's/^\.//' -e 's/\.$//')"
    [ -n "${ver}" ] || die "cannot derive an rpm Version from '${v}'"
    if [ -z "${rel}" ]; then
        rel='1'
    else
        case ".${rel}." in
            *.alpha*|*.beta*|*.rc[0-9]*|*.rc.*|*.pre.*|*.dev.*|*.snapshot*|*.nightly*)
                rel="0.${rel}" ;;
        esac
    fi
    printf '%s %s' "${ver}" "${rel}"
}
RPM_VERSION=''; RPM_RELEASE=''
# `die` inside a command substitution only kills the SUBSHELL, so check the
# result rather than trusting set -e to have stopped us.
read -r RPM_VERSION RPM_RELEASE <<<"$(rpm_fields "${VERSION}")" || true
[ -n "${RPM_VERSION}" ] && [ -n "${RPM_RELEASE}" ] || \
    die "could not derive rpm Version/Release from VERSION='${VERSION}'"
log "version: ${VERSION}  ->  rpm Version=${RPM_VERSION} Release=${RPM_RELEASE}"

# ---------------------------------------------------------------------------
# Package root
# ---------------------------------------------------------------------------
# ${TMPDIR%/}: macOS TMPDIR ends in '/', and the doubled slash breaks nfpm's
# glob matching of contents[].src (learned by make-deb.sh).
TMP_BASE="${TMPDIR:-/tmp}"; TMP_BASE="${TMP_BASE%/}"
PKGROOT="$(mktemp -d "${TMP_BASE}/urnetwork-rpm-root.XXXXXX")"
WORK="$(mktemp -d "${TMP_BASE}/urnetwork-rpm-work.XXXXXX")"
trap 'rm -rf "${PKGROOT}" "${WORK}"' EXIT

assemble_daemon_root "${STAGING}" "${PKGROOT}"
check_payload_arch "${PKGROOT}" "${ARCH}"

# --- the unit moves to the rpm vendor unit directory ------------------------
# NOT marked %config anywhere: /usr/lib/systemd/system is vendor-owned, admin
# overrides live in /etc/systemd/system (drop-ins, `systemctl edit`), and rpm
# never writes there. "Never overwrite an admin-edited unit" is satisfied by
# the path, with no scriptlet logic that could get it wrong.
[ -f "${PKGROOT}/lib/systemd/system/urnetworkd.service" ] || \
    die "assemble_daemon_root did not stage lib/systemd/system/urnetworkd.service -- packaging/lib/common.sh changed shape"
install -d "${PKGROOT}/usr/lib/systemd/system"
install -m 0644 "${PKGROOT}/lib/systemd/system/urnetworkd.service" \
    "${PKGROOT}/usr/lib/systemd/system/urnetworkd.service"
rm -f "${PKGROOT}/lib/systemd/system/urnetworkd.service"
# Nothing else may be dropped on the floor. If common.sh ever stages a second
# file under /lib, this fails loudly instead of the rpm silently shipping
# without it.
LIB_LEFTOVERS="$(find "${PKGROOT:?}/lib" -type f 2>/dev/null || true)"
[ -z "${LIB_LEFTOVERS}" ] || die "assemble_daemon_root staged files under /lib that this script does not know how to remap:
${LIB_LEFTOVERS}
Decide where they belong under /usr (rpm may not own paths through the
/lib -> /usr/lib symlink) and extend the remap above."
rm -rf "${PKGROOT:?}/lib"

# --- the preset (the ONLY way to be enabled by default on Fedora) -----------
# See packaging/rpm/85-urnetwork.preset for what this does and does not claim.
install -d "${PKGROOT}/usr/lib/systemd/system-preset"
install -m 0644 "${SCRIPT_DIR}/rpm/85-urnetwork.preset" \
    "${PKGROOT}/usr/lib/systemd/system-preset/85-urnetwork.preset"

# --- the SELinux policy module ----------------------------------------------
# Shipped BOTH ways, on purpose:
#   urnetwork.pp   compiled here, installed by %post with `semodule -X 200 -i`.
#                  This is the Fedora convention (a compiled module under
#                  /usr/share/selinux/packages) and it means an install needs
#                  no policy toolchain beyond semodule.
#   urnetwork.te   the source. It is the auditable artifact -- one screen, and
#                  a user can read exactly what was granted to init_t -- and it
#                  is %post's fallback if the prebuilt module is rejected.
#
# THE ONE REAL HAZARD, and why the fallback exists: semodule_package stamps the
# module with the BUILD host's libsepol policy version, and a target whose
# libsepol is OLDER refuses it. Building on ubuntu:22.04 (libsepol 3.3) for
# Fedora (3.6+) is the safe direction; building on a bleeding-edge host for
# RHEL is not. %post recompiles from the .te when the prebuilt module will not
# load.
SEPOL_TE="${PACKAGING_DIR}/selinux/urnetwork.te"
[ -f "${SEPOL_TE}" ] || die "packaging/selinux/urnetwork.te is missing -- the rpm cannot ship the policy module the Fedora path requires"
SEPOL_DEST="${PKGROOT}/usr/share/selinux/packages/urnetwork"
install -d "${SEPOL_DEST}"
install -m 0644 "${SEPOL_TE}" "${SEPOL_DEST}/urnetwork.te"

if command -v checkmodule >/dev/null 2>&1 && command -v semodule_package >/dev/null 2>&1; then
    checkmodule -M -m -o "${WORK}/urnetwork.mod" "${SEPOL_TE}" >/dev/null
    semodule_package -o "${WORK}/urnetwork.pp" -m "${WORK}/urnetwork.mod" >/dev/null
    install -m 0644 "${WORK}/urnetwork.pp" "${SEPOL_DEST}/urnetwork.pp"
    log "selinux: built urnetwork.pp ($(wc -c <"${SEPOL_DEST}/urnetwork.pp" | tr -d ' ') bytes) from packaging/selinux/urnetwork.te"
elif [ "${UR_RPM_ALLOW_NO_SELINUX_PP:-0}" = 1 ]; then
    warn "checkmodule/semodule_package are absent -- shipping urnetwork.te ONLY."
    warn "The rpm will compile the policy in %post, which needs checkpolicy AND"
    warn "policycoreutils on the TARGET machine. If they are missing there, the"
    warn "daemon starts but the tunnel cannot open /dev/net/tun."
else
    die "checkmodule/semodule_package not found -- cannot build the SELinux policy module.

This is the FEDORA-FAMILY package and the module is not optional: without it
systemd runs urnetworkd in init_t, which is denied /dev/net/tun, raw sockets
and outbound 443, and every Connect fails. Measured on Bazzite (Enforcing)
with the daemon running as root with the full capability set.

Install the tools on the build host:
  Fedora/RHEL:    dnf install checkpolicy policycoreutils
  Debian/Ubuntu:  apt-get install -y checkpolicy semodule-utils

(The split differs by family and guessing wrong looks exactly like not
trying: semodule_package is in policycoreutils on Fedora -- measured with
rpm -qf on a Bazzite host -- but in semodule-utils on Debian/Ubuntu, where
policycoreutils ships semodule/sestatus/setfiles and NOT semodule_package.)
  macOS:          no port exists -- build the rpm on Linux, or set
                  UR_RPM_ALLOW_NO_SELINUX_PP=1 and accept the %post fallback.

Or set UR_RPM_ALLOW_NO_SELINUX_PP=1 to ship the .te source alone."
fi

# ---------------------------------------------------------------------------
# Render the concrete nfpm config and build
# ---------------------------------------------------------------------------
# nfpm does not expand environment variables in contents[].src (verified for
# the .deb against nfpm v2.47, the version pinned in beta-build.yml), so
# substitute here.
NFPM_CONF="${PKGROOT}/.nfpm.yaml"
sed -e "s|\${RPM_VERSION}|${RPM_VERSION}|g" \
    -e "s|\${RPM_RELEASE}|${RPM_RELEASE}|g" \
    -e "s|\${RPM_ARCH}|${RPM_ARCH}|g" \
    -e "s|\${NFPM_ARCH}|${ARCH}|g" \
    -e "s|\${PKGROOT}|${PKGROOT}|g" \
    "${SCRIPT_DIR}/rpm/nfpm.yaml" > "${NFPM_CONF}"

# Header signing, when a key is provided. A DETACHED .asc (maybe_sign below,
# same as the .deb) proves authorship of the file to a human; it does NOT
# satisfy dnf's gpgcheck, which verifies the signature embedded in the rpm
# header. A real repo needs this block.
if [ -n "${UR_RPM_SIGN_KEY_FILE:-}" ]; then
    [ -f "${UR_RPM_SIGN_KEY_FILE}" ] || die "UR_RPM_SIGN_KEY_FILE does not exist: ${UR_RPM_SIGN_KEY_FILE}"
    # awk, not `sed 's/x/a\nb/'`: a '\n' in a sed replacement is a GNU
    # extension and this script runs on the macOS build server too.
    awk -v kf="${UR_RPM_SIGN_KEY_FILE}" '
        /^#UR_RPM_SIGNATURE_PLACEHOLDER$/ { print "  signature:"; print "    key_file: " kf; next }
        { print }
    ' "${NFPM_CONF}" > "${NFPM_CONF}.signed" && mv "${NFPM_CONF}.signed" "${NFPM_CONF}"
    grep -q '^    key_file: ' "${NFPM_CONF}" || \
        die "the #UR_RPM_SIGNATURE_PLACEHOLDER line is gone from packaging/rpm/nfpm.yaml -- rpm header signing cannot be wired up"
    log "rpm header signing: enabled (key file ${UR_RPM_SIGN_KEY_FILE}; passphrase from \$NFPM_PASSPHRASE)"
fi

# THE OUTPUT NAME.
#
# The release-asset contract (top of .github/workflows/beta-build.yml) is
# "<name>_<version>_<arch>.deb" with the FULL version string and the Debian
# arch spelling, because the in-app checker matches assets on the version it
# is looking for. The rpm keeps that information and that version substring,
# in rpm punctuation and with the rpm arch spelling:
#
#     urnetwork-daemon-2026.8.16-1020679030-beta.x86_64.rpm
#
# This is NOT the canonical NAME-VERSION-RELEASE.ARCH.rpm, and it cannot be:
# the canonical name would be urnetwork-daemon-2026.8.16-0.1020679030.beta
# .x86_64.rpm, which no longer contains the release's version string, because
# rpm forbids the '-' that string needs. `dnf install ./file.rpm` and
# `dnf install <url>` do not care about the filename -- rpm reads the header --
# so the asset name is free to serve the checker.
#
# A dnf REPOSITORY is the case that does care: createrepo_c records the file
# name it finds, so mirror scripts and humans expect NVR. UR_RPM_CANONICAL_NAME=1
# emits that name instead. The canonical NVR is printed on every build either
# way, so whoever builds the repo never has to derive it.
CANONICAL="urnetwork-daemon-${RPM_VERSION}-${RPM_RELEASE}.${RPM_ARCH}.rpm"
if [ "${UR_RPM_CANONICAL_NAME:-0}" = 1 ]; then
    RPM="${OUT}/${CANONICAL}"
else
    RPM="${OUT}/urnetwork-daemon-${VERSION}.${RPM_ARCH}.rpm"
fi

# cwd matters: nfpm resolves the config's relative script paths
# (scripts/post, ...) against the working directory.
(cd "${SCRIPT_DIR}/rpm" && nfpm package -f "${NFPM_CONF}" -p rpm -t "${RPM}")

log "canonical NVR name (what a dnf repo expects): ${CANONICAL}"

# ---------------------------------------------------------------------------
# Verification, where the tooling exists
# ---------------------------------------------------------------------------
# `rpm -qp` is a pure query against a FILE: it does not touch the rpmdb, does
# not install anything and is safe on any host. The build never depends on rpm
# being present (the macOS build server has none).
#
# Do NOT pipe rpm -qpl into head: the same SIGPIPE-through-pipefail trap that
# aborted make-deb.sh's Linux builds after the artifact was already written
# applies here. Write the listing out, then truncate the file.
if command -v rpm >/dev/null 2>&1; then
    rpm -qp --info "${RPM}"
    log ""
    log "--- requires ---"
    rpm -qp --requires "${RPM}"
    log ""
    log "--- scriptlets ---"
    # A missing tag prints the literal "(none)", so test for that, not for
    # emptiness -- otherwise every scriptlet reports "present" forever.
    scriptlets_missing=0
    for tag in PREIN POSTIN PREUN POSTUN; do
        _body="$(rpm -qp --qf "%{${tag}}" "${RPM}" 2>/dev/null || true)"
        if [ -n "${_body}" ] && [ "${_body}" != '(none)' ]; then
            log "  ${tag}: present ($(printf '%s' "${_body}" | wc -l | tr -d ' ') lines)"
        else
            warn "  ${tag}: MISSING -- nfpm did not attach packaging/rpm/scripts/*"
            scriptlets_missing=1
        fi
    done
    # The systemd contract, checked in the built artifact rather than assumed:
    # each macro's unit-name argument must actually be there.
    for pair in "POSTIN:preset" "POSTIN:urnetworkd.service" \
                "PREUN:disable" "PREUN:urnetworkd.service" \
                "POSTUN:urnetworkd.service"; do
        _tag="${pair%%:*}"; _needle="${pair#*:}"
        rpm -qp --qf "%{${_tag}}" "${RPM}" 2>/dev/null | grep -q -- "${_needle}" || {
            warn "scriptlet %{${_tag}} does not mention '${_needle}'"
            scriptlets_missing=1
        }
    done
    log ""
    _contents="$(mktemp)"
    rpm -qpl "${RPM}" >"${_contents}"
    log "--- payload (first 40 of $(wc -l <"${_contents}" | tr -d ' ') entries) ---"
    head -40 "${_contents}"

    # The files whose absence is silent and fatal. A file count alone passes a
    # package that is all locale catalogs and no daemon -- the shape of failure
    # the Windows MSI shipped for months.
    fail="${scriptlets_missing}"
    for want in \
        /usr/lib/urnetwork/urnetworkd \
        /usr/lib/urnetwork/libURnetworkSdk.so \
        /usr/bin/urnetwork \
        /usr/lib/systemd/system/urnetworkd.service \
        /usr/lib/systemd/system-preset/85-urnetwork.preset \
        /usr/share/selinux/packages/urnetwork/urnetwork.te ; do
        grep -Fxq "${want}" "${_contents}" || { warn "MISSING from the rpm payload: ${want}"; fail=1; }
    done
    # The compiled policy module, when this build was able to produce one. Its
    # absence in that case would mean %post has to find checkpolicy on every
    # target machine, which is the thing shipping a .pp exists to avoid.
    if [ -f "${SEPOL_DEST}/urnetwork.pp" ] && \
       ! grep -Fxq /usr/share/selinux/packages/urnetwork/urnetwork.pp "${_contents}"; then
        warn "MISSING from the rpm payload: /usr/share/selinux/packages/urnetwork/urnetwork.pp (it WAS built)"
        fail=1
    fi
    # The unit must NOT be shipped under /lib (a symlink on merged-usr hosts).
    if grep -Eq '^/lib/' "${_contents}"; then
        warn "the rpm ships paths under /lib -- on a merged-usr distro that is a symlink to /usr/lib and rpm will fight the filesystem package"
        fail=1
    fi
    # The unit must NOT be a config file: rpm would keep an admin's edited copy
    # of a VENDOR unit forever and never ship a fixed one.
    if rpm -qpc "${RPM}" 2>/dev/null | grep -q 'systemd/system/urnetworkd.service'; then
        warn "urnetworkd.service is marked %config -- it must not be; admin overrides belong in /etc/systemd/system"
        fail=1
    fi
    log ""
    log "--- config files (%config(noreplace)) ---"
    rpm -qpc "${RPM}" || true
    rm -f "${_contents}"
    [ "${fail}" = 0 ] || die "rpm payload verification failed (see the warnings above)"
    log ""
    log "rpm payload verified."
else
    warn "rpm is not on PATH -- payload and scriptlet verification skipped (nfpm still validated the config)."
    warn "Nothing here executes the scriptlets in any case: that needs a Fedora VM."
fi

sha256_file "${RPM}"
maybe_sign "${RPM}"
if [ -z "${UR_RPM_SIGN_KEY_FILE:-}" ]; then
    log "rpm header is NOT signed -- 'dnf install' of this file works, but a repo serving it needs gpgcheck=1 and therefore a header signature."
fi
log "built: ${RPM}"
