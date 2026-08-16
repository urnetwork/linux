#!/bin/bash
# Build (and optionally install) the URnetwork Flatpak — the GUI-only artifact.
#
# The daemon is NOT in here and cannot be: a Flatpak sandbox has no
# /dev/net/tun, no CAP_NET_ADMIN and no way to install a system unit. The app
# reaches the HOST's urnetworkd over /run/urnetwork/control.sock, which the
# manifest exposes read-only (Trayscale's pattern). Install the daemon from the
# native .deb/.rpm — see packaging/flatpak/network.ur.urnetwork.yml for the
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
MANIFEST="${MANIFEST:-$REPO_ROOT/packaging/flatpak/network.ur.urnetwork.yml}"
APP_ID="network.ur.urnetwork"
RUNTIME_VERSION="${RUNTIME_VERSION:-49}"
# The build dir MUST live inside the repo. flatpak-builder runs sandboxed and
# gets a PRIVATE /tmp, so a build dir under the host's /tmp vanishes between
# stages ("Build directory not initialized, use flatpak build-init").
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-flatpak}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/out}"

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

echo "==> building $APP_ID"
( cd "$REPO_ROOT" && "${BUILDER[@]}" "${BUILD_ARGS[@]}" "$BUILD_DIR" "$MANIFEST" )

if [[ "$DO_BUNDLE" == 1 ]]; then
  VERSION="${VERSION:-0.0.0-dev}"
  BUNDLE="$OUT_DIR/URnetwork-${VERSION}.flatpak"
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
