#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Smoketest build for the linux app — regenerates the gettext catalogs from
# the localization store (../localizations/keys) like the pipeline does, then
# runs the same build the pipeline runs: build/all/build-linux.sh (cgo SDK .so
# cross-built natively via zig for both arches, then per arch two Docker
# images: the daemon .deb + install tarball on ubuntu 22.04, and the GUI
# AppImage on 24.04). See build/BUILD-PLATFORMS.md and linux/APPIMAGE.md.
#
# (Was snapcraft until the 2026-08-05 AppImage migration; nothing here builds
# a .snap any more.)
#
# Usage:
#   ./build.sh
#   ARCHES="arm64"              target arches (default "amd64 arm64" like the
#                               pipeline; arm64-only is much faster locally —
#                               amd64 runs under qemu emulation)
#   EXTERNAL_WARP_VERSION=<v>   version stamp (default 0.0.0-0 for local builds)
#   OUT_DIR=<dir>               artifact output (default <this repo>/out/smoketest;
#                               build-linux.sh clears stale artifacts there)
#   URNETWORK_ROOT=<dir>        sibling-repo root (default: parent of this repo)
#
# Artifacts per arch: urnetwork-daemon_<v>_<arch>.deb,
# urnetwork-daemon-<v>-<arch>.install.tar.gz, URnetwork-<v>-<arch>.AppImage
# (+ .zsync, + .sha256 sidecars).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="${URNETWORK_ROOT:-$(dirname "$here")}"

echo "== sync localizations (store -> app/po/*.po)"
(cd "$root/localizations" &&
    { [ -d node_modules ] || npm ci --no-audit --no-fund; } &&
    npm run gen:linux)

echo "== pipeline linux build (zig cgo cross + daemon/gui containers)"
SRC_HOME="$root" \
EXTERNAL_WARP_VERSION="${EXTERNAL_WARP_VERSION:-0.0.0-0}" \
ARCHES="${ARCHES:-amd64 arm64}" \
OUT_DIR="${OUT_DIR:-$here/out/smoketest}" \
    "$root/build/all/build-linux.sh"

echo "== linux build OK"
