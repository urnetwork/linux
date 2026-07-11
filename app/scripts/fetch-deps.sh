#!/usr/bin/env bash
# Vendor the cgo SDK for local dev / CI into third_party/urnetwork-sdk/<arch>/.
# The release build (build/all/run.sh) uploads URnetworkSdkLinux.zip to the
# GitHub release; here we accept either a local zip or a URL.
#
#   scripts/fetch-deps.sh /path/to/URnetworkSdkLinux.zip        # local zip
#   SDK_ZIP_URL=https://.../URnetworkSdkLinux.zip scripts/fetch-deps.sh
#
# The zip is expected to contain amd64/ and arm64/ subdirs, each with
# libURnetworkSdk.so and urnetwork_sdk.hpp (as produced by sdk/cgo `make build_linux`).
#
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="${here}/third_party/urnetwork-sdk"
mkdir -p "${dest}"

zip="${1:-}"
if [ -z "${zip}" ] && [ -n "${SDK_ZIP_URL:-}" ]; then
  zip="$(mktemp -t urnetwork-sdk-XXXX.zip)"
  echo "fetching ${SDK_ZIP_URL}"
  curl -fsSL "${SDK_ZIP_URL}" -o "${zip}"
fi
if [ -z "${zip}" ] || [ ! -f "${zip}" ]; then
  echo "usage: $0 <URnetworkSdkLinux.zip>   (or set SDK_ZIP_URL)" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
unzip -q -o "${zip}" -d "${tmp}"

for arch in amd64 arm64; do
  so="$(find "${tmp}" -name libURnetworkSdk.so -path "*/${arch}/*" | head -1)"
  hpp="$(find "${tmp}" -name urnetwork_sdk.hpp | head -1)"
  if [ -z "${so}" ]; then
    echo "warn: no ${arch} libURnetworkSdk.so in zip, skipping" >&2
    continue
  fi
  mkdir -p "${dest}/${arch}"
  install -m644 "${so}" "${dest}/${arch}/libURnetworkSdk.so"
  [ -n "${hpp}" ] && install -m644 "${hpp}" "${dest}/${arch}/urnetwork_sdk.hpp"
  echo "vendored ${arch} -> ${dest}/${arch}"
done
