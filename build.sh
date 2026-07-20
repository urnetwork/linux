#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Smoketest build for the linux app — regenerates the gettext catalogs from
# the localization store (../localizations/keys) like the pipeline does, then
# runs the same build the pipeline runs: build/all/build-linux.sh (cgo SDK .so
# cross-built natively via zig for both arches, snaps packaged in the
# snapcraft rock Docker container). See build/BUILD-PLATFORMS.md.
#
# Usage:
#   ./build.sh
#   ARCHES="arm64"              snap arches (default "amd64 arm64" like the
#                               pipeline; arm64-only is much faster locally —
#                               amd64 packaging runs under qemu emulation)
#   EXTERNAL_WARP_VERSION=<v>   version stamp (default 0.0.0-0 for local builds)
#   OUT_DIR=<dir>               snap output (default <this repo>/out/smoketest;
#                               existing *.snap there are cleared)
#   URNETWORK_ROOT=<dir>        sibling-repo root (default: parent of this repo)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="${URNETWORK_ROOT:-$(dirname "$here")}"

echo "== sync localizations (store -> app/po/*.po)"
(cd "$root/localizations" &&
    { [ -d node_modules ] || npm ci --no-audit --no-fund; } &&
    npm run gen:linux)

echo "== pipeline linux build (zig cgo cross + snapcraft container)"
SRC_HOME="$root" \
EXTERNAL_WARP_VERSION="${EXTERNAL_WARP_VERSION:-0.0.0-0}" \
ARCHES="${ARCHES:-amd64 arm64}" \
OUT_DIR="${OUT_DIR:-$here/out/smoketest}" \
    "$root/build/all/build-linux.sh"

echo "== linux build OK"
