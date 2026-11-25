#!/usr/bin/env bash
# Deterministic regression for `install.sh --update`, the install tarball's
# update channel.
#
# The default source must be the newest STABLE release of urnetwork/linux,
# resolved through the GitHub releases API by the repository's numeric id with
# no redirect followed (never a made-up download host, never urnetwork/build
# nightlies, a fork or whoever holds a name the repository moved from), and
# the downloaded tarball must match the sha256 digest GitHub reports for that
# asset before anything from it runs. An explicit --url / UR_TARBALL_URL
# override keeps working.
#
# No network: curl and uname are fakes on PATH. The fake curl answers only the
# URLs a test case maps and fails any other request the way an unresolvable
# host does (exit 6), logging every URL it was asked for. A mapped 3xx answer
# is followed to its Location only when curl was given -L, as curl does.
set -euo pipefail
umask 077

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
installer="$here/tarball/install.sh"

run_dir="$(mktemp -d "${TMPDIR:-/tmp}/urnetwork-tarball-update.test.XXXXXX")"
trap 'rm -rf "$run_dir"' EXIT

failures=0
fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

# ---- fakes -----------------------------------------------------------------

bin="$run_dir/bin"
mkdir -p "$bin"

cat >"$bin/uname" <<'EOF'
#!/bin/sh
case "$1" in
    -m) echo x86_64 ;;
    *)  echo Linux ;;
esac
EOF

# The map file: one "<url> <http code> <body file> [<location>]" per line.
cat >"$bin/curl" <<'EOF'
#!/bin/sh
out='' wfmt='' fail_on_http=0 follow=0 url=''
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        -w) wfmt="$2"; shift 2 ;;
        -H|-A|--proto|--proto-redir|--max-time|--connect-timeout|--retry) shift 2 ;;
        --location) follow=1; shift ;;
        --*) shift ;;
        -*) case "$1" in *f*) fail_on_http=1 ;; esac
            case "$1" in *L*) follow=1 ;; esac; shift ;;
        *) url="$1"; shift ;;
    esac
done
printf '%s\n' "$url" >>"$FAKE_CURL_LOG"
[ "$follow" = 1 ] && printf '%s\n' "$url" >>"$FAKE_CURL_FOLLOW_LOG"
while :; do
    line="$(awk -v u="$url" '$1 == u { print; exit }' "$FAKE_CURL_MAP")"
    if [ -z "$line" ]; then
        echo "curl: (6) Could not resolve host: $(printf '%s' "$url" | sed 's|^[a-z]*://||; s|/.*||')" >&2
        exit 6
    fi
    code="$(printf '%s' "$line" | awk '{print $2}')"
    body="$(printf '%s' "$line" | awk '{print $3}')"
    location="$(printf '%s' "$line" | awk '{print $4}')"
    case "$code" in
        3??) if [ "$follow" = 1 ] && [ -n "$location" ]; then
                 url="$location"
                 printf '%s\n' "$url" >>"$FAKE_CURL_LOG"
                 continue
             fi ;;
    esac
    break
done
if [ "$fail_on_http" = 1 ] && [ "$code" -ge 400 ]; then
    echo "curl: (22) The requested URL returned error: $code" >&2
    exit 22
fi
if [ -n "$out" ]; then cat "$body" >"$out"; else cat "$body"; fi
[ -n "$wfmt" ] && printf '%s' "$code"
exit 0
EOF
chmod 0755 "$bin/uname" "$bin/curl"

# ---- fixtures --------------------------------------------------------------

version='2026.10.1-1060587890'
tag="v$version"
asset="urnetwork-daemon-$version-amd64.install.tar.gz"
repo_id='1297137671'  # urnetwork/linux, as app/src/ReleaseSelection.hpp pins it
api_url="https://api.github.com/repositories/$repo_id/releases/latest"
named_api_url='https://api.github.com/repos/urnetwork/linux/releases/latest'
asset_url="https://github.com/urnetwork/linux/releases/download/$tag/$asset"
nightly_url="https://github.com/urnetwork/build/releases/download/$tag/$asset"

# A tarball whose install.sh only records that it ran, and with what.
mkdir -p "$run_dir/src/urnetwork-daemon"
cat >"$run_dir/src/urnetwork-daemon/install.sh" <<'EOF'
#!/bin/bash
printf 'ran %s\n' "$*" >"$UPDATE_MARKER"
EOF
chmod 0755 "$run_dir/src/urnetwork-daemon/install.sh"
tarball="$run_dir/$asset"
tar -czf "$tarball" -C "$run_dir/src" urnetwork-daemon
good_digest="sha256:$(sha256_of "$tarball")"
printf 'something else entirely\n' >"$run_dir/other"
bad_digest="sha256:$(sha256_of "$run_dir/other")"

# release_json <file> <draft> <prerelease> <assets json> -- the shape GitHub's
# API answers, including the parts a naive grep trips on: a markdown body full
# of quotes, commas and braces, and a nested uploader object per asset.
release_json() {
    printf '{"url":"https://api.github.com/repos/urnetwork/linux/releases/1","html_url":"https://github.com/urnetwork/linux/releases/tag/%s","tag_name":"%s","name":"%s","draft":%s,"prerelease":%s,"assets":[%s],"body":"Notes: \\"quoted\\", {braces}, [brackets], \\\\ and a \\"name\\":\\"decoy\\" pair."}\n' \
        "$tag" "$tag" "$tag" "$2" "$3" "$4" >"$1"
}

asset_json() {  # asset_json <name> <url> <digest>
    printf '{"url":"https://api.github.com/repos/urnetwork/linux/releases/assets/9","name":"%s","uploader":{"login":"someone","name":"decoy-uploader"},"content_type":"application/gzip","size":1,"digest":"%s","browser_download_url":"%s"}' \
        "$1" "$3" "$2"
}

arm_asset="$(asset_json "urnetwork-daemon-$version-arm64.install.tar.gz" \
    "https://github.com/urnetwork/linux/releases/download/$tag/urnetwork-daemon-$version-arm64.install.tar.gz" \
    "$bad_digest")"

# ---- runner ----------------------------------------------------------------

case_dir=''
# setup_case <name>: fresh map, log, marker and TMPDIR for one case.
setup_case() {
    case_dir="$run_dir/case-$1"
    mkdir -p "$case_dir/tmp"
    : >"$case_dir/map"
    : >"$case_dir/curl.log"
    : >"$case_dir/follow.log"
}

map_url() {  # map_url <url> <code> <file> [<location>]
    printf '%s %s %s %s\n' "$1" "$2" "$3" "${4:-}" >>"$case_dir/map"
}

# run_update [installer args...] -> sets rc; stdout/stderr in $case_dir.
rc=0
run_update() {
    rc=0
    env PATH="$bin:$PATH" TMPDIR="$case_dir/tmp" \
        FAKE_CURL_MAP="$case_dir/map" FAKE_CURL_LOG="$case_dir/curl.log" \
        FAKE_CURL_FOLLOW_LOG="$case_dir/follow.log" \
        UPDATE_MARKER="$case_dir/marker" \
        bash "$installer" --update "$@" \
        >"$case_dir/out" 2>"$case_dir/err" </dev/null || rc=$?
}

requested() { grep -qxF "$1" "$case_dir/curl.log"; }
ran_update() { [ -f "$case_dir/marker" ]; }
stderr_has() { grep -qiF "$1" "$case_dir/err"; }
show() { sed 's/^/    | /' "$case_dir/err" >&2; }

never_unofficial() {  # the default path never asks any host but GitHub's official repo
    if grep -q 'get\.ur\.network' "$case_dir/curl.log"; then
        fail "$1: requested the unresolvable get.ur.network host"
    fi
    if grep -q 'urnetwork/build' "$case_dir/curl.log"; then
        fail "$1: requested the urnetwork/build nightly repo"
    fi
}

# ---- cases -----------------------------------------------------------------

# 1. Default: latest stable urnetwork/linux release, own-arch asset, verified.
setup_case default
release_json "$case_dir/release.json" false false "$arm_asset,$(asset_json "$asset" "$asset_url" "$good_digest")"
map_url "$api_url" 200 "$case_dir/release.json"
map_url "$asset_url" 200 "$tarball"
run_update --yes
never_unofficial default
requested "$api_url" || fail "default: did not ask $api_url for the latest stable release"
requested "$named_api_url" && fail "default: asked the API by the repository's name, not its id"
grep -qxF "$api_url" "$case_dir/follow.log" && fail "default: the API request follows redirects (-L)"
requested "$asset_url" || fail "default: did not download $asset from urnetwork/linux"
if [ "$rc" -ne 0 ] || ! ran_update; then
    fail "default: the verified stable tarball's installer did not run (exit $rc)"; show
elif ! grep -qx 'ran --yes' "$case_dir/marker"; then
    fail "default: --yes was not handed to the downloaded installer"
fi

# 2. The downloaded bytes do not match GitHub's digest: nothing from them runs.
setup_case mismatch
release_json "$case_dir/release.json" false false "$(asset_json "$asset" "$asset_url" "$bad_digest")"
map_url "$api_url" 200 "$case_dir/release.json"
map_url "$asset_url" 200 "$tarball"
run_update --yes
never_unofficial mismatch
if [ "$rc" -eq 0 ] || ran_update; then
    fail "mismatch: a tarball that does not match the API digest was installed (exit $rc)"
fi
requested "$asset_url" || fail "mismatch: never downloaded the asset, so the digest was never checked"
stderr_has 'sha256' || { fail "mismatch: the refusal does not say the sha256 did not match"; show; }
[ -z "$(ls -A "$case_dir/tmp")" ] || fail "mismatch: the refused download was left behind in TMPDIR"

# 3. No digest on the asset: refused rather than installed unverified.
setup_case nodigest
release_json "$case_dir/release.json" false false "$(asset_json "$asset" "$asset_url" "")"
map_url "$api_url" 200 "$case_dir/release.json"
map_url "$asset_url" 200 "$tarball"
run_update --yes
if [ "$rc" -eq 0 ] || ran_update; then
    fail "nodigest: an asset without a sha256 digest was installed (exit $rc)"
fi
stderr_has 'digest' || { fail "nodigest: the refusal does not mention the digest"; show; }

# 4. urnetwork/linux has no stable release yet (API 404): clear error, no guess.
setup_case norelease
printf '{"message":"Not Found","documentation_url":"https://docs.github.com"}\n' >"$case_dir/404.json"
map_url "$api_url" 404 "$case_dir/404.json"
run_update --yes
never_unofficial norelease
requested "$api_url" || fail "norelease: did not ask $api_url"
if [ "$rc" -eq 0 ] || ran_update; then fail "norelease: exited $rc"; fi
[ "$(wc -l <"$case_dir/curl.log" | tr -d ' ')" = 1 ] || \
    fail "norelease: guessed a download URL after the API said there is no release: $(tr '\n' ' ' <"$case_dir/curl.log")"
stderr_has 'no stable release' || { fail "norelease: the error does not say there is no stable release"; show; }

# 5. The stable release does not carry the install tarball: clear error naming it.
setup_case noasset
release_json "$case_dir/release.json" false false "$arm_asset"
map_url "$api_url" 200 "$case_dir/release.json"
run_update --yes
never_unofficial noasset
if [ "$rc" -eq 0 ] || ran_update; then fail "noasset: exited $rc"; fi
stderr_has "$asset" || { fail "noasset: the error does not name $asset"; show; }

# 6. Named right, hosted elsewhere (the nightly repo): refused, not followed.
setup_case offrepo
release_json "$case_dir/release.json" false false "$(asset_json "$asset" "$nightly_url" "$good_digest")"
map_url "$api_url" 200 "$case_dir/release.json"
map_url "$nightly_url" 200 "$tarball"
run_update --yes
if [ "$rc" -eq 0 ] || ran_update; then fail "offrepo: installed an asset hosted outside urnetwork/linux"; fi
requested "$nightly_url" && fail "offrepo: downloaded from the urnetwork/build nightly repo"

# 6b. On urnetwork/linux's download path but not this release's own file:
#     another tag's path, another file, a path that climbs out, a query.
other_tag_url="https://github.com/urnetwork/linux/releases/download/v2026.9.1-1000000000/$asset"
other_file_url="https://github.com/urnetwork/linux/releases/download/$tag/urnetwork-daemon-$version-arm64.install.tar.gz"
climb_url="https://github.com/urnetwork/linux/releases/download/$tag/../../../build/$asset"
query_url="$asset_url?x=1"
n=0
for wrong in "$other_tag_url" "$other_file_url" "$climb_url" "$query_url"; do
    n=$((n + 1))
    setup_case "wrongpath-$n"
    release_json "$case_dir/release.json" false false "$(asset_json "$asset" "$wrong" "$good_digest")"
    map_url "$api_url" 200 "$case_dir/release.json"
    map_url "$wrong" 200 "$tarball"
    run_update --yes
    if [ "$rc" -eq 0 ] || ran_update; then fail "wrongpath: installed $wrong"; fi
    requested "$wrong" && fail "wrongpath: downloaded $wrong"
    stderr_has 'not hosted by' || { fail "wrongpath: the refusal of $wrong does not say why"; show; }
done

# 7. A prerelease (or draft) answer is never installed.
setup_case prerelease
release_json "$case_dir/release.json" false true "$(asset_json "$asset" "$asset_url" "$good_digest")"
map_url "$api_url" 200 "$case_dir/release.json"
map_url "$asset_url" 200 "$tarball"
run_update --yes
if [ "$rc" -eq 0 ] || ran_update; then fail "prerelease: installed a prerelease"; fi

# 7b. The API answers with a redirect (a rename, or another repository now
#     holding the id's old name): refused, never followed, nothing installed.
setup_case redirect
moved_api_url='https://api.github.com/repos/someone/linux/releases/latest'
printf '{"message":"Moved Permanently","url":"%s"}\n' "$moved_api_url" >"$case_dir/301.json"
release_json "$case_dir/release.json" false false "$(asset_json "$asset" "$asset_url" "$good_digest")"
map_url "$api_url" 301 "$case_dir/301.json" "$moved_api_url"
map_url "$moved_api_url" 200 "$case_dir/release.json"
map_url "$asset_url" 200 "$tarball"
run_update --yes
if [ "$rc" -eq 0 ] || ran_update; then fail "redirect: followed the API's redirect and installed (exit $rc)"; fi
requested "$moved_api_url" && fail "redirect: asked the URL the API redirected to"
requested "$asset_url" && fail "redirect: downloaded an asset after the API answered a redirect"
stderr_has '301' || { fail "redirect: the refusal does not name the HTTP status"; show; }

# 8. Explicit override, both spellings: fetched as given, no API call.
override_url='https://mirror.example.test/urnetwork-daemon.tar.gz'
for spelling in env flag; do
    setup_case "override-$spelling"
    map_url "$override_url" 200 "$tarball"
    if [ "$spelling" = env ]; then
        UR_TARBALL_URL="$override_url" run_update --yes
    else
        run_update --yes --url "$override_url"
    fi
    if [ "$rc" -ne 0 ] || ! ran_update; then
        fail "override-$spelling: the explicit URL was not installed (exit $rc)"; show
    fi
    requested "$api_url" && fail "override-$spelling: queried the API despite an explicit URL"
done

# 9. Explicit override with a pinned sha256 that does not match: refused.
setup_case override-mismatch
map_url "$override_url" 200 "$tarball"
run_update --yes --url "$override_url" --sha256 "${bad_digest#sha256:}"
if [ "$rc" -eq 0 ] || ran_update; then fail "override-mismatch: installed despite a sha256 mismatch"; fi
stderr_has 'sha256' || { fail "override-mismatch: the refusal does not mention sha256"; show; }

# 10. --dry-run touches nothing, not even the API.
setup_case dryrun
run_update --dry-run
[ "$rc" -eq 0 ] || { fail "dryrun: exited $rc"; show; }
[ -s "$case_dir/curl.log" ] && fail "dryrun: made a request: $(tr '\n' ' ' <"$case_dir/curl.log")"
grep -q 'urnetwork/linux' "$case_dir/out" || fail "dryrun: the plan does not name urnetwork/linux"

# ---- release signature (SHA256SUMS + SHA256SUMS.sig) ------------------------
#
# A test key is generated here and handed to the installer through its
# test-only UR_TEST_RELEASE_SIG_PUBKEY hook. The verifying openssl must do
# ed25519 with pkeyutl -rawin (OpenSSL 3); macOS's /usr/bin/openssl is
# LibreSSL, so Homebrew's openssl@3 is preferred when present.
ossl=''
for cand in "$(brew --prefix openssl@3 2>/dev/null)/bin/openssl" \
            /opt/homebrew/opt/openssl@3/bin/openssl /usr/local/opt/openssl@3/bin/openssl \
            "$(command -v openssl 2>/dev/null || true)"; do
    [ -x "$cand" ] || continue
    if "$cand" genpkey -algorithm ed25519 -out "$run_dir/probe.pem" >/dev/null 2>&1 && \
       "$cand" pkeyutl -help 2>&1 | grep -q -- '-rawin'; then
        ossl="$cand"; break
    fi
done

if [ -z "$ossl" ]; then
    echo "SKIP: signature cases (no OpenSSL 3 with ed25519 pkeyutl -rawin on this host)" >&2
else
    echo "signature cases use $ossl ($("$ossl" version))"
    ln -sf "$ossl" "$bin/openssl"
    # An openssl that cannot verify raw ed25519 (LibreSSL / 1.1.1 shape).
    weak_bin="$run_dir/weakbin"
    mkdir -p "$weak_bin"
    cp "$bin/uname" "$bin/curl" "$weak_bin/"
    cat >"$weak_bin/openssl" <<EOF
#!/bin/sh
if [ "\$1" = pkeyutl ]; then echo "usage: pkeyutl [-in file] [-sigfile file]" >&2; exit 1; fi
exec "$ossl" "\$@"
EOF
    chmod 0755 "$weak_bin/openssl"

    raw_pub() { "$ossl" pkey -in "$1" -pubout -outform DER | tail -c 32 | "$ossl" base64 -A; }
    "$ossl" genpkey -algorithm ed25519 -out "$run_dir/test.key" 2>/dev/null
    "$ossl" genpkey -algorithm ed25519 -out "$run_dir/other.key" 2>/dev/null
    test_pub="$(raw_pub "$run_dir/test.key")"

    sign() {  # sign <key> <sums file> <out .sig>: base64 std signature + newline
        "$ossl" pkeyutl -sign -inkey "$1" -rawin -in "$2" -out "$3.bin"
        { "$ossl" base64 -A -in "$3.bin"; printf '\n'; } >"$3"
    }
    sums_url="https://github.com/urnetwork/linux/releases/download/$tag/SHA256SUMS"
    sig_url="$sums_url.sig"

    # signed_case <name> <sums content> <key>: release + sums + sig mapped.
    signed_case() {
        setup_case "$1"
        release_json "$case_dir/release.json" false false "$(asset_json "$asset" "$asset_url" "$good_digest")"
        map_url "$api_url" 200 "$case_dir/release.json"
        map_url "$asset_url" 200 "$tarball"
        printf '%s' "$2" >"$case_dir/SHA256SUMS"
        sign "$3" "$case_dir/SHA256SUMS" "$case_dir/SHA256SUMS.sig"
        map_url "$sums_url" 200 "$case_dir/SHA256SUMS"
        map_url "$sig_url" 200 "$case_dir/SHA256SUMS.sig"
    }
    good_sums="$(printf '%s  %s\n%s  %s\n' "${bad_digest#sha256:}" "urnetwork-daemon-$version-arm64.install.tar.gz" "${good_digest#sha256:}" "$asset")
"

    # S1. Signed by the release key, tarball line matches: installed.
    signed_case sig-good "$good_sums" "$run_dir/test.key"
    UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -ne 0 ] || ! ran_update; then fail "sig-good: a correctly signed release was not installed (exit $rc)"; show; fi
    grep -q 'release signature verified' "$case_dir/out" || fail "sig-good: did not report the verified signature"
    requested "$sig_url" || fail "sig-good: never fetched SHA256SUMS.sig"

    # S1b. Same with signatures required.
    signed_case sig-good-required "$good_sums" "$run_dir/test.key"
    UR_REQUIRE_SIG=1 UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -ne 0 ] || ! ran_update; then fail "sig-good-required: exit $rc"; show; fi

    # S2. Signed by some other key: refused.
    signed_case sig-wrongkey "$good_sums" "$run_dir/other.key"
    UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -eq 0 ] || ran_update; then fail "sig-wrongkey: installed a release signed by another key"; fi
    stderr_has 'does NOT verify' || { fail "sig-wrongkey: refusal does not say the signature failed"; show; }

    # S2b. Without the test hook the built-in key is used: a test-key
    #      signature does not verify against it.
    signed_case sig-builtin-key "$good_sums" "$run_dir/test.key"
    run_update --yes
    if [ "$rc" -eq 0 ] || ran_update; then fail "sig-builtin-key: a test-key signature passed against the release key"; fi

    # S3. SHA256SUMS altered after signing: refused.
    signed_case sig-tampered "$good_sums" "$run_dir/test.key"
    printf '%s  %s\n' "${good_digest#sha256:}" "extra-file" >>"$case_dir/SHA256SUMS"
    UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -eq 0 ] || ran_update; then fail "sig-tampered: installed despite altered SHA256SUMS"; fi

    # S4. Validly signed, but the tarball's line names other bytes: refused
    #     (the GitHub digest alone would have passed it).
    signed_case sig-wronghash "$(printf '%s  %s\n' "${bad_digest#sha256:}" "$asset")" "$run_dir/test.key"
    UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -eq 0 ] || ran_update; then fail "sig-wronghash: installed a tarball whose signed sha256 differs"; fi
    stderr_has 'does not match the signed SHA256SUMS' || { fail "sig-wronghash: refusal does not say why"; show; }

    # S5. Validly signed, no line for this tarball: refused.
    signed_case sig-noline "$(printf '%s  %s\n' "${good_digest#sha256:}" "something-else.tar.gz")" "$run_dir/test.key"
    UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -eq 0 ] || ran_update; then fail "sig-noline: installed with no SHA256SUMS line for $asset"; fi
    stderr_has 'no line for' || { fail "sig-noline: refusal does not say why"; show; }

    # S6. A corrupt .sig (not 64 bytes): refused.
    signed_case sig-short "$good_sums" "$run_dir/test.key"
    printf 'AAAA\n' >"$case_dir/SHA256SUMS.sig"
    UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -eq 0 ] || ran_update; then fail "sig-short: installed with a truncated signature"; fi

    # S7. No .sig: staged default warns and installs; REQUIRE_SIG=1 refuses.
    setup_case sig-absent
    release_json "$case_dir/release.json" false false "$(asset_json "$asset" "$asset_url" "$good_digest")"
    map_url "$api_url" 200 "$case_dir/release.json"
    map_url "$asset_url" 200 "$tarball"
    UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -ne 0 ] || ! ran_update; then fail "sig-absent: staged mode refused an unsigned release (exit $rc)"; show; fi
    stderr_has 'no SHA256SUMS.sig' || { fail "sig-absent: no warning about the missing signature"; show; }
    rm -f "$case_dir/marker"
    UR_REQUIRE_SIG=1 UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -eq 0 ] || ran_update; then fail "sig-absent: REQUIRE_SIG=1 installed an unsigned release"; fi

    # S8. .sig present but openssl cannot verify ed25519: warn (staged) or
    #     refuse (required).
    signed_case sig-weak-openssl "$good_sums" "$run_dir/other.key"
    saved_bin="$bin"; bin="$weak_bin"
    UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -ne 0 ] || ! ran_update; then fail "sig-weak-openssl: staged mode refused (exit $rc)"; show; fi
    stderr_has 'cannot verify ed25519' || { fail "sig-weak-openssl: no warning about openssl"; show; }
    rm -f "$case_dir/marker"
    UR_REQUIRE_SIG=1 UR_TEST_RELEASE_SIG_PUBKEY="$test_pub" run_update --yes
    if [ "$rc" -eq 0 ] || ran_update; then fail "sig-weak-openssl: REQUIRE_SIG=1 installed unverified"; fi
    bin="$saved_bin"
fi

if [ "$failures" -gt 0 ]; then
    echo "$failures failure(s)" >&2
    exit 1
fi
echo "ok: install.sh --update resolves, verifies (digest and signature) and refuses as specified"
