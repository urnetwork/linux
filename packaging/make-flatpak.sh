#!/bin/bash
# Build (and optionally install) the URnetwork Flatpak — the GUI-only artifact.
#
# The daemon is NOT in here and cannot be: a Flatpak sandbox has no
# /dev/net/tun, no CAP_NET_ADMIN and no way to install a system unit. The app
# reaches the HOST's urnetworkd over /run/urnetwork/control.sock, which the
# manifest exposes read-only (Trayscale's pattern). Install the daemon from the
# native .deb/.rpm — see packaging/flatpak/com.bringyour.network.yml for the
# full reasoning.
#
# Works on an immutable host (Bazzite/Silverblue): flatpak-builder itself runs
# as a flatpak (org.flatpak.Builder), so nothing is layered onto /usr.
#
#   ./packaging/make-flatpak.sh                 # build only
#   ./packaging/make-flatpak.sh --install       # build + install --user
#   ./packaging/make-flatpak.sh --install --run # ...and launch it
#   ./packaging/make-flatpak.sh --bundle        # build + export a .flatpak file
#
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${MANIFEST:-$REPO_ROOT/packaging/flatpak/com.bringyour.network.yml}"
APP_ID="com.bringyour.network"
RUNTIME_VERSION="${RUNTIME_VERSION:-49}"
# The build dir MUST live inside the repo. flatpak-builder runs sandboxed and
# gets a PRIVATE /tmp, so a build dir under the host's /tmp vanishes between
# stages ("Build directory not initialized, use flatpak build-init").
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-flatpak}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/out}"
# Read here, not just in the --bundle branch: the version has to be stamped into
# the manifest BEFORE the build, not after it.
VERSION="${VERSION:-}"
# Debian arch spelling, to match -Dsdk_arch and every other artifact name in
# this pipeline. Defaults to THIS host: flatpak-builder builds for the machine
# it runs on, so a cross-arch value here would only mislabel the bundle.
if [[ -z "${ARCH:-}" ]]; then
  case "$(uname -m)" in
    x86_64)          ARCH=amd64 ;;
    aarch64|arm64)   ARCH=arm64 ;;
    *) echo "unsupported machine $(uname -m); set ARCH=amd64|arm64" >&2; exit 1 ;;
  esac
fi

DO_INSTALL=0
DO_RUN=0
DO_BUNDLE=0
for arg in "$@"; do
  case "$arg" in
    --install) DO_INSTALL=1 ;;
    --run)     DO_INSTALL=1; DO_RUN=1 ;;
    --bundle)  DO_BUNDLE=1 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

command -v flatpak >/dev/null || { echo "flatpak is not installed" >&2; exit 1; }
[[ -f "$MANIFEST" ]] || { echo "manifest not found: $MANIFEST" >&2; exit 1; }

# flatpak-builder: prefer a native one, fall back to the flatpak'd builder
# (the only option on an immutable host).
if command -v flatpak-builder >/dev/null; then
  BUILDER=(flatpak-builder)
else
  if ! flatpak info org.flatpak.Builder >/dev/null 2>&1; then
    echo "installing org.flatpak.Builder (no native flatpak-builder found)"
    flatpak install -y --noninteractive flathub org.flatpak.Builder
  fi
  BUILDER=(flatpak run org.flatpak.Builder)
fi

echo "==> ensuring runtime + sdk ${RUNTIME_VERSION}"
flatpak install -y --noninteractive flathub \
  "org.gnome.Platform//${RUNTIME_VERSION}" "org.gnome.Sdk//${RUNTIME_VERSION}" >/dev/null

# --disable-rofiles-fuse: rofiles-fuse needs FUSE inside the builder sandbox,
# which is not reliably available on an immutable host. It only costs build
# isolation, not correctness.
BUILD_ARGS=(--force-clean --disable-rofiles-fuse)
[[ "$DO_INSTALL" == 1 ]] && BUILD_ARGS+=(--user --install)
if [[ "$DO_BUNDLE" == 1 ]]; then
  mkdir -p "$OUT_DIR"
  BUILD_ARGS+=(--repo="$BUILD_DIR-repo")
fi

# The committed manifest carries NO -Dapp_version, so meson falls back to its
# 0.0.0 dev sentinel unless something supplies one. That matters more than it
# looks: the Flatpak is the ONLY artifact that installs the AppStream metainfo
# (packaging/lib/common.sh's assemble_daemon_root() whitelist excludes
# usr/share/metainfo, and make-appimage.sh does not package it), and that file
# is what GNOME Software, KDE Discover and the Flathub page read. A 0.0.0 in
# there is valid AppStream, so no validator catches it -- it just shows up on
# the store page.
#
# Stamped by the SCRIPT and not by the release workflow, deliberately. A
# CI-only sed would leave a local `make-flatpak.sh --install` shipping 0.0.0
# while CI shipped the real version; doing it here covers both cases, because
# CI reaches the Flatpak through this script too.
BUILD_MANIFEST="$MANIFEST"
if [[ -n "$VERSION" ]]; then
  anchor='      - -Dhost_integration=false'
  if ! grep -qxF "$anchor" "$MANIFEST"; then
    echo "the app module's config-opts anchor ('-Dhost_integration=false') is gone from $MANIFEST, so -Dapp_version cannot be stamped and the build would report 0.0.0. Add -Dapp_version to the manifest directly and drop this block." >&2
    exit 1
  fi
  # Same directory as the original ON PURPOSE: the app module is `path: ../..`,
  # which flatpak-builder resolves relative to the manifest, so a copy anywhere
  # else would not find the repo. Written as a copy rather than an in-place edit
  # so a developer's working tree is never left modified.
  BUILD_MANIFEST="$(dirname "$MANIFEST")/.stamped-$(basename "$MANIFEST")"
  trap 'rm -f "$BUILD_MANIFEST"' EXIT
  sed "s|^${anchor}\$|${anchor}\n      - -Dapp_version=${VERSION}\n      - -Dsdk_arch=${ARCH}|" \
      "$MANIFEST" > "$BUILD_MANIFEST"
  for opt in "-Dapp_version=${VERSION}" "-Dsdk_arch=${ARCH}"; do
    if ! grep -qxF "      - ${opt}" "$BUILD_MANIFEST"; then
      echo "failed to stamp ${opt} into $BUILD_MANIFEST" >&2
      exit 1
    fi
  done
  echo "==> stamped -Dapp_version=${VERSION} -Dsdk_arch=${ARCH}"
else
  echo "==> WARNING: VERSION is unset, so this build reports the 0.0.0 dev sentinel." >&2
  echo "==>          Set VERSION=<release version> for anything you intend to ship." >&2
fi

echo "==> building $APP_ID"
( cd "$REPO_ROOT" && "${BUILDER[@]}" "${BUILD_ARGS[@]}" "$BUILD_DIR" "$BUILD_MANIFEST" )

if [[ "$DO_BUNDLE" == 1 ]]; then
  VERSION="${VERSION:-0.0.0-dev}"  # filename only; the stamp happened above
  # ARCH IS PART OF THE NAME. Without it the amd64 and arm64 legs write the same
  # file and one silently overwrites the other wherever the artifacts are merged.
  # The rule everywhere else in packaging/ is that the build script names its
  # own artifact rather than leaving a workflow to rename it afterwards, so the
  # arch suffix is applied here.
  BUNDLE="$OUT_DIR/URnetwork-${VERSION}-${ARCH}.flatpak"
  echo "==> exporting $BUNDLE"
  flatpak build-bundle "$BUILD_DIR-repo" "$BUNDLE" "$APP_ID" \
    --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo
  echo "built: $BUNDLE"
fi

if [[ "$DO_INSTALL" == 1 ]]; then
  # STOP ANY RUNNING INSTANCE. flatpak never restarts a running app on update,
  # and this GUI registers a UNIQUE GTK application id — so relaunching from the
  # menu finds the OLD process still owning the bus name, hands it the click and
  # re-presents ITS window. The user then tests the previous build while
  # believing they are testing this one. That cost a full debugging cycle once:
  # a Connect press went to a pre-update process whose control socket had been
  # closed by a daemon restart, and the new build never even built a window
  # (106ms CPU, no output past font loading).
  if pgrep -x urnetwork-gui >/dev/null 2>&1; then
    echo "==> stopping the running URnetwork instance so the new build is what launches"
    flatpak kill "$APP_ID" >/dev/null 2>&1 || true
    for _ in 1 2 3 4 5; do
      pgrep -x urnetwork-gui >/dev/null 2>&1 || break
      sleep 1
    done
    pgrep -x urnetwork-gui >/dev/null 2>&1 && pkill -x urnetwork-gui || true
  fi
  echo "==> installed. The GUI needs the HOST daemon to connect:"
  echo "    sudo systemctl status urnetworkd    # install it from the .deb/.rpm"
fi

[[ "$DO_RUN" == 1 ]] && exec flatpak run "$APP_ID"
exit 0
