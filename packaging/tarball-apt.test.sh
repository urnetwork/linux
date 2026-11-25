#!/usr/bin/env bash
# Deterministic regression for install.sh's apt channel on Debian/Ubuntu.
#
# A fresh install on a host whose /etc/os-release says debian or ubuntu (ID or
# ID_LIKE) and that has apt-get adds the apt.ur.io repository and installs
# urnetwork-daemon with apt; `--update` on a dpkg-owned install runs
# `apt-get install --only-upgrade`. --no-apt, other distributions and an
# existing tarball install keep the tarball path; a failed apt attempt removes
# the repository files it added and falls back to the tarball install.
#
# No network and no root: uname, id, curl, apt-get and dpkg are fakes on PATH,
# and install.sh's test-only UR_INSTALL_TEST_ROOT roots /etc/os-release,
# /etc/apt and the install manifests in a scratch directory. The installer is
# run from a copy with no payload/, so "the tarball path ran" shows up as its
# "no payload/" refusal (fresh install) or as a GitHub API request (--update).
set -euo pipefail
umask 022

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
installer_src="$here/tarball/install.sh"

run_dir="$(mktemp -d "${TMPDIR:-/tmp}/urnetwork-tarball-apt.test.XXXXXX")"
trap 'rm -rf "$run_dir"' EXIT

failures=0
fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

# ---- fakes -----------------------------------------------------------------

bin="$run_dir/bin"
mkdir -p "$bin"

cat >"$bin/uname" <<'EOF'
#!/bin/sh
case "$1" in -m) echo x86_64 ;; *) echo Linux ;; esac
EOF
cat >"$bin/id" <<'EOF'
#!/bin/sh
[ "$1" = -u ] && { echo 0; exit 0; }
exec /usr/bin/id "$@"
EOF
# curl: the apt key URL answers $FAKE_KEY (or fails when FAKE_KEY_FAIL=1);
# every URL is logged; anything else fails like an unresolvable host.
cat >"$bin/curl" <<'EOF'
#!/bin/sh
out='' url=''
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        -w|-H|-A|--proto|--proto-redir) shift 2 ;;
        -*) shift ;;
        *) url="$1"; shift ;;
    esac
done
printf '%s\n' "$url" >>"$FAKE_LOG.curl"
if [ "$url" = 'https://apt.ur.io/urnetwork-archive-keyring.gpg' ] && [ "${FAKE_KEY_FAIL:-0}" != 1 ]; then
    printf 'fake-keyring-bytes\n' >"$out"; exit 0
fi
echo "curl: (6) Could not resolve host" >&2
exit 6
EOF
# apt-get: logs its arguments; FAKE_APT_FAIL=update|install fails that step;
# a successful (or FAKE_APT_PARTIAL=1) install marks the package installed.
cat >"$bin/apt-get" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$FAKE_LOG.apt"
case "$*" in
    update) [ "${FAKE_APT_FAIL:-}" = update ] && exit 100 ;;
    *install*)
        [ "${FAKE_APT_PARTIAL:-0}" = 1 ] && printf 'Status: install ok installed\n' >"$FAKE_DPKG_STATUS"
        [ "${FAKE_APT_FAIL:-}" = install ] && exit 100
        printf 'Status: install ok installed\n' >"$FAKE_DPKG_STATUS" ;;
esac
exit 0
EOF
cat >"$bin/dpkg" <<'EOF'
#!/bin/sh
[ "$1" = -s ] || exit 0
[ -s "$FAKE_DPKG_STATUS" ] || { echo "dpkg-query: package '$2' is not installed" >&2; exit 1; }
printf 'Package: %s\n' "$2"; cat "$FAKE_DPKG_STATUS"
EOF
chmod 0755 "$bin"/*

# The installer copy, without its payload.
mkdir -p "$run_dir/urnetwork-daemon"
cp "$installer_src" "$run_dir/urnetwork-daemon/install.sh"
installer="$run_dir/urnetwork-daemon/install.sh"

# ---- runner ----------------------------------------------------------------

case_dir=''
root=''
setup_case() {  # setup_case <name> <os-release body>
    case_dir="$run_dir/case-$1"
    root="$case_dir/root"
    mkdir -p "$root/etc" "$case_dir/tmp"
    printf '%s\n' "$2" >"$root/etc/os-release"
    : >"$case_dir/log.apt"; : >"$case_dir/log.curl"; : >"$case_dir/dpkg-status"
}

rc=0
run_install() {
    rc=0
    env PATH="$bin:$PATH" TMPDIR="$case_dir/tmp" UR_INSTALL_TEST_ROOT="$root" \
        FAKE_LOG="$case_dir/log" FAKE_DPKG_STATUS="$case_dir/dpkg-status" \
        bash "$installer" "$@" >"$case_dir/out" 2>"$case_dir/err" </dev/null || rc=$?
}

debian='ID=debian
VERSION_ID="12"
PRETTY_NAME="Debian GNU/Linux 12 (bookworm)"'
mint='ID=linuxmint
ID_LIKE="ubuntu debian"'
fedora='ID=fedora
VERSION_ID=41'

sources="$root/etc/apt/sources.list.d/urnetwork.sources"
apt_ran() { [ -s "$case_dir/log.apt" ]; }
apt_has() { grep -qxF "$1" "$case_dir/log.apt"; }
tarball_ran() { grep -q 'no payload/' "$case_dir/err"; }
stderr_has() { grep -qiF "$1" "$case_dir/err"; }
show() { sed 's/^/    | /' "$case_dir/err" >&2; }
expected_sources='Types: deb
URIs: https://apt.ur.io
Suites: stable
Components: main
Signed-By: /etc/apt/keyrings/urnetwork-archive-keyring.gpg'

# ---- cases -----------------------------------------------------------------

# 1. Debian fresh install: repo configured, package installed by apt, no tarball.
setup_case debian "$debian"
sources="$root/etc/apt/sources.list.d/urnetwork.sources"
run_install --yes
[ "$rc" -eq 0 ] || { fail "debian: exit $rc"; show; }
[ "$(cat "$sources" 2>/dev/null)" = "$expected_sources" ] || fail "debian: urnetwork.sources is wrong: $(cat "$sources" 2>/dev/null)"
[ -s "$root/etc/apt/keyrings/urnetwork-archive-keyring.gpg" ] || fail "debian: keyring not installed"
grep -qxF 'https://apt.ur.io/urnetwork-archive-keyring.gpg' "$case_dir/log.curl" || fail "debian: key not fetched from apt.ur.io"
[ "$(tr '\n' '|' <"$case_dir/log.apt")" = 'update|install -y urnetwork-daemon|' ] || \
    fail "debian: apt-get calls were: $(tr '\n' '|' <"$case_dir/log.apt")"
tarball_ran && fail "debian: the tarball install ALSO ran after apt succeeded"

# 2. A derivative that says so only in ID_LIKE (Mint): apt path.
setup_case mint "$mint"
run_install --yes
apt_has 'install -y urnetwork-daemon' || { fail "mint: ID_LIKE=ubuntu did not take the apt path"; show; }

# 3. Not Debian-family: tarball path, apt never touched.
setup_case fedora "$fedora"
run_install --yes
apt_ran && fail "fedora: ran apt-get on a non-Debian host"
tarball_ran || { fail "fedora: the tarball path did not run"; show; }

# 4. --no-apt forces the tarball path on Debian.
setup_case noapt "$debian"
run_install --yes --no-apt
apt_ran && fail "noapt: --no-apt still ran apt-get"
[ -e "$root/etc/apt/sources.list.d/urnetwork.sources" ] && fail "noapt: wrote the apt source anyway"
tarball_ran || { fail "noapt: the tarball path did not run"; show; }

# 5. The repository is not live (apt-get update fails): its files are removed
#    again and the tarball install runs, with a warning.
setup_case repo-down "$debian"
FAKE_APT_FAIL=update run_install --yes
apt_has 'update' || fail "repo-down: never tried apt-get update"
[ -e "$root/etc/apt/sources.list.d/urnetwork.sources" ] && fail "repo-down: left the failing apt source behind"
[ -e "$root/etc/apt/keyrings/urnetwork-archive-keyring.gpg" ] && fail "repo-down: left the keyring behind"
stderr_has 'falling back to the tarball' || { fail "repo-down: no fallback warning"; show; }
tarball_ran || { fail "repo-down: the tarball path did not run"; show; }

# 6. The key cannot be fetched: no apt-get at all, tarball fallback.
setup_case nokey "$debian"
FAKE_KEY_FAIL=1 run_install --yes
apt_ran && fail "nokey: ran apt-get without a key"
[ -e "$root/etc/apt/sources.list.d/urnetwork.sources" ] && fail "nokey: wrote a source without a key"
tarball_ran || { fail "nokey: the tarball path did not run"; show; }

# 7. apt-get install fails but dpkg now has the package: no tarball on top.
setup_case partial "$debian"
FAKE_APT_FAIL=install FAKE_APT_PARTIAL=1 run_install --yes
[ "$rc" -ne 0 ] || fail "partial: exited 0"
tarball_ran && fail "partial: installed the tarball over a dpkg-owned package"
stderr_has 'dpkg now has' || { fail "partial: refusal does not say why"; show; }

# 8. An existing tarball install stays one: apt would take over its files.
setup_case tarball-present "$debian"
mkdir -p "$root/usr/lib/urnetwork"; : >"$root/usr/lib/urnetwork/.install-manifest"
run_install --yes
apt_ran && fail "tarball-present: ran apt-get over an existing tarball install"
tarball_ran || { fail "tarball-present: the tarball upgrade did not run"; show; }

# 9. --update on an apt-managed install: apt-get --only-upgrade, no GitHub.
setup_case update-apt "$debian"
printf 'Status: install ok installed\n' >"$case_dir/dpkg-status"
run_install --update --yes
[ "$rc" -eq 0 ] || { fail "update-apt: exit $rc"; show; }
apt_has 'install -y --only-upgrade urnetwork-daemon' || fail "update-apt: apt-get calls were: $(tr '\n' '|' <"$case_dir/log.apt")"
grep -q 'api.github.com' "$case_dir/log.curl" && fail "update-apt: asked GitHub for a tarball too"

# 10. --update on a tarball install on Debian: the tarball update channel.
setup_case update-tarball "$debian"
run_install --update --yes
apt_ran && fail "update-tarball: ran apt-get for a non-apt install"
grep -q 'api.github.com' "$case_dir/log.curl" || { fail "update-tarball: did not use the tarball update channel"; show; }

# 11. --update --no-apt on an apt-managed install: apt is left alone.
setup_case update-noapt "$debian"
printf 'Status: install ok installed\n' >"$case_dir/dpkg-status"
run_install --update --yes --no-apt
apt_ran && fail "update-noapt: --no-apt still ran apt-get"

# 12. --dry-run on Debian changes nothing.
setup_case dryrun "$debian"
run_install --dry-run
apt_ran && fail "dryrun: executed apt-get"
[ -e "$root/etc/apt/sources.list.d/urnetwork.sources" ] && fail "dryrun: wrote the apt source"
grep -q 'would run: apt-get install -y urnetwork-daemon' "$case_dir/out" || { fail "dryrun: the plan does not show the apt install"; sed 's/^/    | /' "$case_dir/out" >&2; }

if [ "$failures" -gt 0 ]; then
    echo "$failures failure(s)" >&2
    exit 1
fi
echo "ok: install.sh uses apt on Debian/Ubuntu, falls back and stays out of the way as specified"
