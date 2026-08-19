#!/bin/bash
# Build URnetwork-<version>-<arch>.AppImage (+ .zsync) -- the unprivileged
# GUI artifact (MIGRATION.md normative names).
#
# *** LINUX ONLY. This script CANNOT run on macOS. ***
# It bundles the build host's own GTK4/libadwaita stack via ldd, so it must
# run on a Linux box (container) of the TARGET arch -- and that box should be
# the distro floor (Debian 12 / Ubuntu 22.04), because libstdc++ and glibc
# are host-provided at runtime (see excludelist). It fails fast anywhere else
# rather than producing something broken.
#
# The AppDir is HAND-ROLLED on purpose (APPIMAGE.md 11e):
# linuxdeploy-plugin-gtk is not usable -- its GTK4 branch is an unmaintained
# stub whose rpath-fixup loop reads GTK3-only variables (under
# DEPLOY_GTK_VERSION=4 no GTK4 module gets its deps deployed), it exports
# GTK_THEME which overrides libadwaita's theming, and it pins
# GDK_BACKEND=x11. Gaphor (GTK4+libadwaita) deleted its AppImage in 2023 over
# exactly this. What we owe by hand instead: GTK4 + libadwaita + the whole
# GLib family WITH its GIO modules (never split), gdk-pixbuf loaders,
# compiled GSettings schemas, the Adwaita icon theme, and an AppRun that sets
# GSETTINGS_SCHEMA_DIR / GDK_PIXBUF_MODULE_FILE / GIO_MODULE_DIR /
# XDG_DATA_DIRS and does NOT force GTK_THEME or GDK_BACKEND.
#
# WebKitGTK DECISION (documented, APPIMAGE.md section 4 + 11e): OMITTED from
# the AppImage. webkitgtk-6.0 hardcodes absolute paths to its multi-process
# helper executables at compile time; no tool relocates it, and the
# WEBKIT_EXEC_PATH escape only exists in developer builds. It is used only
# for the upgrade sheet's embedded Stripe checkout, which already falls back
# to the system browser -- so the AppImage GUI must be built without a
# webkitgtk-6.0 NEEDED entry (dlopen with clean degradation is also fine).
# This script enforces that with readelf and refuses to package otherwise.
#
# Fontconfig/freetype, Mesa/GL, libstdc++, X11/wayland client libs are
# host-provided -- rationale in ./excludelist.
#
# Pipeline entry point (name and invocation pinned in MIGRATION.md): called
# with VERSION, ARCH (amd64|arm64), STAGING_DIR (the `meson install --destdir`
# tree) and OUT_DIR in the environment; writes
# URnetwork-${VERSION}-${ARCH}.AppImage and its .zsync into $OUT_DIR. The
# builder image (Ubuntu 24.04) already carries appimagetool, zsyncmake,
# mksquashfs and patchelf; linuxdeploy is present there too but its GTK
# plugin must NOT be used (see above). Flags override the environment for
# manual runs:
#   make-appimage.sh [--staging <dir>] [--arch <a>] [--out <dir>] \
#                    [--version <v>]
# Env overrides: UR_GUI_BIN (staged GUI path, default usr/bin/urnetwork-gui),
# APPIMAGETOOL (path to appimagetool), UR_ZSYNC_URL (update-info URL),
# UR_GLIBC_CEILING (default 2.35), UR_SKIP_GLIBC_GATE=1.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source-path=SCRIPTDIR
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"

usage() {
    sed -n '2,52p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

[ "$(uname -s)" = 'Linux' ] || die \
"the AppImage build runs on Linux only: it bundles the host's GTK4 stack via ldd.
Run it in the target-arch Linux builder container -- see build/BUILD-PLATFORMS.md.
macOS can only shellcheck this script."

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

[ -n "${STAGING}" ] || die "STAGING_DIR (or --staging) is required: the meson install --destdir tree of linux/app"
[ -n "${OUT}" ] || die "OUT_DIR (or --out) is required"
[ -n "${VERSION}" ] || die "VERSION (or --version) is required"
case "${ARCH}" in
    amd64) APPIMAGE_ARCH='x86_64' ;;
    arm64) APPIMAGE_ARCH='aarch64' ;;
    *) die "ARCH (or --arch) must be amd64 or arm64 (got '${ARCH:-<empty>}')" ;;
esac

for tool in ldd readelf glib-compile-schemas gio-querymodules; do
    command -v "${tool}" >/dev/null 2>&1 || die "required tool missing on the build host: ${tool}"
done
APPIMAGETOOL="${APPIMAGETOOL:-$(command -v appimagetool || true)}"
[ -n "${APPIMAGETOOL}" ] || die \
"appimagetool not found -- set APPIMAGETOOL or install it from
https://github.com/AppImage/appimagetool/releases (the 'continuous' build)"

STAGING="$(cd "${STAGING}" && pwd)"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

# ---------------------------------------------------------------------------
# Inputs from the staging tree
# ---------------------------------------------------------------------------
GUI_REL="${UR_GUI_BIN:-usr/bin/urnetwork-gui}"
GUI_BIN="${STAGING}/${GUI_REL}"
if [ ! -f "${GUI_BIN}" ]; then
    {
        printf 'error: GUI binary not found at %s\n' "${GUI_BIN}"
        printf 'Expected the staged GUI at usr/bin/urnetwork-gui (override with UR_GUI_BIN).\n'
        printf 'usr/bin in the staging tree contains:\n'
        find "${STAGING}/usr/bin" -maxdepth 1 -mindepth 1 2>/dev/null | sed 's/^/  /' || printf '  (missing)\n'
        printf 'This comes from "meson install --destdir" of linux/app (workstream A).\n'
    } >&2
    exit 1
fi
SDK_SO="${STAGING}/usr/lib/urnetwork/libURnetworkSdk.so"
[ -f "${SDK_SO}" ] || die "staging tree is missing usr/lib/urnetwork/libURnetworkSdk.so (MIGRATION.md table)"
[ -f "${STAGING}/usr/share/urnetwork/world-110m.json" ] || die \
"staging tree is missing usr/share/urnetwork/world-110m.json -- without it the
provider-locations globe silently loses its land layer (APPIMAGE.md 11e)"

# The build host bundles ITS OWN libraries: its arch must match the target.
GUI_ARCH="$(elf_arch "${GUI_BIN}")"
[ "${GUI_ARCH}" = "${ARCH}" ] || die "staged GUI is ${GUI_ARCH} but --arch is ${ARCH}"
HOST_ARCH_RAW="$(uname -m)"
case "${HOST_ARCH_RAW}" in
    x86_64)  HOST_ARCH='amd64' ;;
    aarch64) HOST_ARCH='arm64' ;;
    *)       HOST_ARCH='unsupported' ;;
esac
[ "${HOST_ARCH}" = "${ARCH}" ] || die \
"build host is ${HOST_ARCH_RAW} but --arch is ${ARCH}: the AppDir bundles host
libraries, so build each arch on (a container of) that arch"

# WebKit decision enforcement (see header): a NEEDED entry on webkitgtk means
# the GUI was configured with the embedded-checkout webview linked in.
if readelf -d "${GUI_BIN}" | grep -qi 'webkit'; then
    die "staged GUI links webkitgtk (NEEDED) -- build the GUI for the AppImage
with the webkit feature disabled (the upgrade sheet falls back to the system
browser) or convert it to dlopen; bundling WebKitGTK cannot work
(absolute helper paths, APPIMAGE.md section 4)"
fi

# glibc symbol ceiling (APPIMAGE.md 10c gate): glibc and libstdc++ are
# host-provided at runtime, so our own objects must not demand more than the
# supported floor. NOTE the live tension: the builder image is Ubuntu 24.04
# (glibc 2.39), so a GUI compiled there will normally reference newer symbols
# and trip this gate. That is the gate working -- it makes the floor decision
# explicit instead of silent. Either build the GUI against the floor
# (zig/sysroot, as the SDK already does), or knowingly raise the AppImage's
# floor with UR_GLIBC_CEILING=2.39.
if [ "${UR_SKIP_GLIBC_GATE:-0}" != 1 ]; then
    GLIBC_CEIL="${UR_GLIBC_CEILING:-2.35}"
    # grep exits 1 on zero matches; that must not trip set -e/pipefail.
    MAXSYM="$( { readelf --dyn-syms -W "${GUI_BIN}"; readelf --dyn-syms -W "${SDK_SO}"; } \
        | { grep -o 'GLIBC_[0-9.]*' || true; } | sed 's/^GLIBC_//' | sort -V | tail -n1 )"
    if [ -n "${MAXSYM}" ] && [ "$(printf '%s\n%s\n' "${MAXSYM}" "${GLIBC_CEIL}" | sort -V | tail -n1)" != "${GLIBC_CEIL}" ]; then
        die "binaries reference GLIBC_${MAXSYM} > ceiling ${GLIBC_CEIL} -- the AppImage would not run on the floor distro (Ubuntu 22.04 = 2.35). Build the GUI against the floor, or accept a raised floor with UR_GLIBC_CEILING=${MAXSYM} (document it), or UR_SKIP_GLIBC_GATE=1"
    fi
    log "glibc symbol ceiling ok (max GLIBC_${MAXSYM:-none} <= ${GLIBC_CEIL})"
fi

# ---------------------------------------------------------------------------
# AppDir assembly
# ---------------------------------------------------------------------------
TMP_BASE="${TMPDIR:-/tmp}"; TMP_BASE="${TMP_BASE%/}"
WORK="$(mktemp -d "${TMP_BASE}/urnetwork-appdir.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
APPDIR="${WORK}/AppDir"
install -d "${APPDIR}/usr/bin" "${APPDIR}/usr/lib/urnetwork" "${APPDIR}/usr/share"

install -m 0755 "${GUI_BIN}" "${APPDIR}/usr/bin/urnetwork-gui"
install -m 0644 "${SDK_SO}" "${APPDIR}/usr/lib/urnetwork/libURnetworkSdk.so"

# App data: globe outlines + tray art (resolved via $APPDIR + UR_PKGDATADIR).
cp -R "${STAGING}/usr/share/urnetwork" "${APPDIR}/usr/share/urnetwork"

# Gettext catalogs. NOTE (APPIMAGE.md section 3a): main.cpp must bindtextdomain
# relative to $APPDIR at runtime -- a compile-time /usr/share/locale makes all
# of this dead weight and the app silently English-only. That fix lives in
# src/ (workstream A); this script just ships the catalogs where it looks.
while IFS= read -r mo; do
    rel="${mo#"${STAGING}"/}"
    install -D -m 0644 "${mo}" "${APPDIR}/${rel}"
done < <(find "${STAGING}/usr/share/locale" -type f -name 'urnetwork.mo' 2>/dev/null)

# Desktop entry: same app-id filename as the system one (wayland app_id ->
# icon association), but Exec points at the bundled binary -- inside the
# AppDir the launcher script does not exist. AppRun is what actually runs.
DESKTOP_SRC="${APP_PACKAGING_DIR}/network.ur.urnetwork.desktop"
[ -f "${DESKTOP_SRC}" ] || die "missing ${DESKTOP_SRC}"
install -d "${APPDIR}/usr/share/applications"
sed -e 's/^Exec=urnetwork /Exec=urnetwork-gui /' \
    -e 's/^TryExec=urnetwork$/TryExec=urnetwork-gui/' \
    "${DESKTOP_SRC}" > "${APPDIR}/usr/share/applications/network.ur.urnetwork.desktop"
printf 'X-AppImage-Version=%s\n' "${VERSION}" >> "${APPDIR}/usr/share/applications/network.ur.urnetwork.desktop"
cp "${APPDIR}/usr/share/applications/network.ur.urnetwork.desktop" "${APPDIR}/network.ur.urnetwork.desktop"

install -d "${APPDIR}/usr/share/icons/hicolor/256x256/apps" "${APPDIR}/usr/share/icons/hicolor/48x48/apps"
install -m 0644 "${APP_PACKAGING_DIR}/icons/hicolor/256x256/apps/urnetwork.png" \
    "${APPDIR}/usr/share/icons/hicolor/256x256/apps/urnetwork.png"
install -m 0644 "${APP_PACKAGING_DIR}/icons/hicolor/48x48/apps/urnetwork.png" \
    "${APPDIR}/usr/share/icons/hicolor/48x48/apps/urnetwork.png"
install -m 0644 "${APP_PACKAGING_DIR}/icons/hicolor/256x256/apps/urnetwork.png" "${APPDIR}/urnetwork.png"
cp "${APPDIR}/urnetwork.png" "${APPDIR}/.DirIcon"

install -m 0755 "${SCRIPT_DIR}/appimage/AppRun" "${APPDIR}/AppRun"

# ---------------------------------------------------------------------------
# GTK4 runtime pieces from the host
# ---------------------------------------------------------------------------
libdir_candidates=( "/usr/lib/${HOST_ARCH_RAW}-linux-gnu" /usr/lib64 /usr/lib )
host_libdir=''
for d in "${libdir_candidates[@]}"; do
    [ -e "${d}/libgtk-4.so.1" ] && { host_libdir="${d}"; break; }
done
[ -n "${host_libdir}" ] || die "libgtk-4.so.1 not found on the build host (looked in: ${libdir_candidates[*]}) -- install the GTK4 runtime"

# gdk-pixbuf loaders (+ librsvg for Adwaita's symbolic SVG icons) and the
# query tool AppRun uses to regenerate the cache each launch.
pixbuf_moduledir=''
for d in "${host_libdir}"/gdk-pixbuf-2.0/*/loaders; do
    [ -d "${d}" ] && pixbuf_moduledir="${d}"
done
[ -n "${pixbuf_moduledir}" ] || die "gdk-pixbuf loaders not found under ${host_libdir}/gdk-pixbuf-2.0"
pixbuf_rel="${pixbuf_moduledir#"${host_libdir}"/}"     # gdk-pixbuf-2.0/<ver>/loaders
install -d "${APPDIR}/usr/lib/${pixbuf_rel}"
cp "${pixbuf_moduledir}"/*.so "${APPDIR}/usr/lib/${pixbuf_rel}/"
QUERY_LOADERS="$(command -v gdk-pixbuf-query-loaders || true)"
if [ -z "${QUERY_LOADERS}" ]; then
    for c in "${host_libdir}/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders" \
             /usr/libexec/gdk-pixbuf-query-loaders; do
        [ -x "${c}" ] && QUERY_LOADERS="${c}"
    done
fi
[ -n "${QUERY_LOADERS}" ] || die "gdk-pixbuf-query-loaders not found (needed in the bundle; AppRun regenerates the loader cache at launch)"
install -m 0755 "${QUERY_LOADERS}" "${APPDIR}/usr/bin/gdk-pixbuf-query-loaders"

# GIO modules: bundle the GLib family WITH its modules, never split.
# glib-networking provides TLS; gvfs is deliberately not bundled.
gio_moduledir="${host_libdir}/gio/modules"
if [ -d "${gio_moduledir}" ]; then
    install -d "${APPDIR}/usr/lib/gio/modules"
    find "${gio_moduledir}" -maxdepth 1 -name 'libgio*.so' -exec cp {} "${APPDIR}/usr/lib/gio/modules/" \;
    gio-querymodules "${APPDIR}/usr/lib/gio/modules"   # cache paths are relative: build-time generation is enough
else
    warn "no GIO modules at ${gio_moduledir} (glib-networking missing?) -- TLS-over-GIO consumers would break"
fi

# GSettings schemas: a missing schema is a hard abort in GTK4. Bundle the
# host's full compiled set (gtk4 + gsettings-desktop-schemas for libadwaita's
# org.gnome.desktop.interface reads).
[ -d /usr/share/glib-2.0/schemas ] || die "/usr/share/glib-2.0/schemas missing on build host"
install -d "${APPDIR}/usr/share/glib-2.0/schemas"
find /usr/share/glib-2.0/schemas -maxdepth 1 \( -name '*.gschema.xml' -o -name '*.override' \) \
    -exec cp {} "${APPDIR}/usr/share/glib-2.0/schemas/" \;
glib-compile-schemas "${APPDIR}/usr/share/glib-2.0/schemas"

# Icon themes: Adwaita (symbolic icons the UI names ~15 of; APPIMAGE.md 3b --
# this is the icon-theme-staging fix, done here in packaging) + hicolor base.
[ -d /usr/share/icons/Adwaita ] || die "Adwaita icon theme not found on build host (install adwaita-icon-theme)"
cp -R /usr/share/icons/Adwaita "${APPDIR}/usr/share/icons/Adwaita"
if [ -f /usr/share/icons/hicolor/index.theme ]; then
    cp /usr/share/icons/hicolor/index.theme "${APPDIR}/usr/share/icons/hicolor/index.theme"
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f "${APPDIR}/usr/share/icons/Adwaita" || true
    gtk-update-icon-cache -q -t -f "${APPDIR}/usr/share/icons/hicolor" || true
fi

# ---------------------------------------------------------------------------
# Shared-library closure via ldd (transitive), minus the excludelist
# ---------------------------------------------------------------------------
is_excluded() {
    local base="$1" pat
    while IFS= read -r pat; do
        case "${pat}" in ''|\#*) continue ;; esac
        # shellcheck disable=SC2254  # glob match against the pattern is the point
        case "${base}" in ${pat}) return 0 ;; esac
    done < "${SCRIPT_DIR}/appimage/excludelist"
    return 1
}

MISSING=''
copy_deps_of() {
    local obj="$1" name path
    # ldd prints the full transitive closure of each object.
    while read -r name _ path _; do
        case "${name}" in linux-vdso*|ld-linux*|/*) continue ;; esac
        [ -n "${path}" ] || continue
        if [ "${path}" = 'not' ]; then    # "libfoo => not found"
            MISSING="${MISSING} ${name}"
            continue
        fi
        [ -f "${path}" ] || continue
        if is_excluded "${name}"; then continue; fi
        [ -f "${APPDIR}/usr/lib/${name}" ] && continue
        cp -L "${path}" "${APPDIR}/usr/lib/${name}"
    done < <(LD_LIBRARY_PATH="${APPDIR}/usr/lib/urnetwork" ldd "${obj}" 2>/dev/null)
}

log "collecting shared-library closure..."
copy_deps_of "${APPDIR}/usr/bin/urnetwork-gui"
copy_deps_of "${APPDIR}/usr/lib/urnetwork/libURnetworkSdk.so"
for so in "${APPDIR}/usr/lib/${pixbuf_rel}"/*.so "${APPDIR}"/usr/lib/gio/modules/*.so; do
    [ -f "${so}" ] && copy_deps_of "${so}"
done
if [ -n "${MISSING}" ]; then
    die "unresolved libraries on the build host:${MISSING} -- install them (or extend excludelist if truly host-provided)"
fi
log "bundled $(find "${APPDIR}/usr/lib" -maxdepth 1 -name '*.so*' | wc -l | tr -d ' ') libraries"

# ---------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------
APPIMAGE="${OUT}/URnetwork-${VERSION}-${ARCH}.AppImage"
# Self-hosted zsync only: GitHub Releases answers the multi-range requests
# zsync needs with HTTP 501 (APPIMAGE.md section 5). The URL points at the
# stable "latest" alias so embedded update info never goes stale.
ZSYNC_URL="${UR_ZSYNC_URL:-https://get.ur.network/URnetwork-latest-${ARCH}.AppImage.zsync}"

# appimagetool fetches the type2 runtime from GitHub on EVERY invocation and
# has no retry, so a blip fails the whole build after all the AppDir work:
#   Failed to download runtime: server returned status code 0
# The builder image bakes the runtime in (Dockerfile.gui) and points
# UR_APPIMAGE_RUNTIME at it, which also pins exactly which runtime ships rather
# than taking whatever "continuous" serves that minute. Fall back to letting
# appimagetool download when the variable is unset, so a bare host still works.
runtime_args=()
if [ -n "${UR_APPIMAGE_RUNTIME:-}" ]; then
    [ -f "${UR_APPIMAGE_RUNTIME}" ] || die \
        "UR_APPIMAGE_RUNTIME is set but not a file: ${UR_APPIMAGE_RUNTIME}"
    runtime_args=(--runtime-file "${UR_APPIMAGE_RUNTIME}")
    log "using the pre-fetched runtime: ${UR_APPIMAGE_RUNTIME}"
else
    log "UR_APPIMAGE_RUNTIME unset -- appimagetool will download the runtime (network-dependent)"
fi

ARCH="${APPIMAGE_ARCH}" "${APPIMAGETOOL}" \
    --updateinformation "zsync|${ZSYNC_URL}" \
    "${runtime_args[@]}" \
    "${APPDIR}" "${APPIMAGE}"

[ -f "${APPIMAGE}" ] || die "appimagetool did not produce ${APPIMAGE}"

# THE .zsync LANDS IN THE CURRENT DIRECTORY, NOT BESIDE THE APPIMAGE.
# appimagetool takes the basename of its output path for the zsync's filename and
# writes it relative to CWD, so with an absolute --output the AppImage goes to
# $OUT_DIR and its .zsync is left wherever the build happened to be standing.
# Measured twice: by hand during development (the file had to be moved manually
# every time), and then by CI, which failed the payload check with
# "URnetwork-<version>-<arch>.AppImage.zsync was not produced". Moving it here
# means the script owns the whole contract rather than leaving one asset for the
# caller to find, since the update channel needs the pair.
ZSYNC_NAME="$(basename "${APPIMAGE}").zsync"
if [ -f "${APPIMAGE}.zsync" ]; then
    log "zsync: ${APPIMAGE}.zsync"
elif [ -f "${ZSYNC_NAME}" ]; then
    mv -f "${ZSYNC_NAME}" "${APPIMAGE}.zsync"
    log "zsync: ${APPIMAGE}.zsync (moved out of $(pwd))"
elif [ -f "${PWD}/${ZSYNC_NAME}" ]; then
    mv -f "${PWD}/${ZSYNC_NAME}" "${APPIMAGE}.zsync"
    log "zsync: ${APPIMAGE}.zsync (moved out of ${PWD})"
else
    warn "no .zsync produced -- older appimagetool? The update channel needs ${ZSYNC_NAME}"
fi

# Detached signatures (APPIMAGE.md 11f): appimagetool --sign embeds the key
# in the file it validates, which is not a trust story -- sign detached, for
# BOTH the AppImage and its .zsync, fingerprint published out of band.
sha256_file "${APPIMAGE}"
maybe_sign "${APPIMAGE}"
if [ -f "${APPIMAGE}.zsync" ]; then
    sha256_file "${APPIMAGE}.zsync"
    maybe_sign "${APPIMAGE}.zsync"
fi
log "built: ${APPIMAGE}"
