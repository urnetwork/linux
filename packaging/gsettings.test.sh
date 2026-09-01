#!/usr/bin/env bash
# Deterministic AppImage GSettings source-closure and strictness regression.
set -euo pipefail
umask 077

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "$here/lib/common.sh"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

run_dir="$(mktemp -d "${TMPDIR:-/tmp}/urnetwork-gsettings.test.XXXXXX")"
trap 'rm -rf "$run_dir"' EXIT
source_dir="$run_dir/source"
destination_dir="$run_dir/destination"
mkdir -p "$source_dir"

printf '%s\n' \
    '<schemalist>' \
    '  <enum id="com.bringyour.network.TestMode">' \
    '    <value nick="standard" value="0"/>' \
    '    <value nick="private" value="1"/>' \
    '  </enum>' \
    '</schemalist>' >"$source_dir/com.bringyour.network.enums.xml"
printf '%s\n' \
    '<schemalist>' \
    '  <schema id="com.bringyour.network.Test" path="/com/bringyour/network/test/">' \
    '    <key name="mode" enum="com.bringyour.network.TestMode">' \
    "      <default>'standard'</default>" \
    '    </key>' \
    '  </schema>' \
    '</schemalist>' >"$source_dir/com.bringyour.network.gschema.xml"

stage_gsettings_schemas "$source_dir" "$destination_dir"
[ -f "$destination_dir/com.bringyour.network.enums.xml" ] || \
    fail "enum declarations were not staged"
[ -s "$destination_dir/gschemas.compiled" ] || \
    fail "schema compiler produced no cache"

bad_source="$run_dir/bad-source"
mkdir -p "$bad_source"
cp "$source_dir/com.bringyour.network.gschema.xml" "$bad_source/"
if stage_gsettings_schemas "$bad_source" "$run_dir/bad-destination" >/dev/null 2>&1; then
    fail "strict staging accepted a schema whose enum declaration was missing"
fi

echo "AppImage GSettings schema regression: PASS"
