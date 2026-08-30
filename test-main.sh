#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Product acceptance test for the LOCAL Linux app and urnetworkd against the
# production (main) environment. It builds the AppImage and Ubuntu 22.04 daemon
# package, installs the package into an isolated privileged container, performs
# instant-account login/logout/secret-key login, password login, a real provider
# connection with changed public egress, disconnect, and package removal.
#
# Usage:
#   ./test-main.sh                 build and run once
#   ./test-main.sh --repeat=5      repeat the full account+tunnel case five times
#   ./test-main.sh --skip-build    reuse UR_ACCEPT_LINUX_OUT artifacts
#   ./test-main.sh --headless      accepted for cross-platform runner parity
#   ./test-main.sh --keep-fixture  retain the recoverable account for another app
#
# Environment:
#   UR_ACCEPT_VAULT=<path>         alternate main acceptance credentials
#   UR_ACCEPT_FIXTURE=<path>       persistent private instant-account fixture
#   UR_ACCEPT_REPEAT=<n>           repetition count
#   UR_ACCEPT_KEEP_FIXTURE=1       retain the account after a successful run
#   UR_ACCEPT_LINUX_OUT=<path>     build output cache
#   EXTERNAL_WARP_VERSION=<v>      local artifact version (default 0.0.0-0)
set -euo pipefail
umask 077

here="$(cd "$(dirname "$0")" && pwd)"
root="${URNETWORK_ROOT:-$(dirname "$here")}"
vault="${UR_ACCEPT_VAULT:-$root/vault/main/tests.yml}"
fixture="${UR_ACCEPT_FIXTURE:-$here/tests/__acceptance__/fixtures/linux-main.secret}"
repeat_count="${UR_ACCEPT_REPEAT:-1}"
skip_build="${SKIP_BUILD:-0}"
keep_fixture="${UR_ACCEPT_KEEP_FIXTURE:-0}"
result_matrix="${UR_ACCEPT_RESULT_FILE:-}"
version="${EXTERNAL_WARP_VERSION:-0.0.0-0}"
out_dir="${UR_ACCEPT_LINUX_OUT:-$here/out/acceptance}"

if [ -n "${WARP_VERSION:-}" ]; then
  sdk_version="$WARP_VERSION"
else
  case "$version" in
    *-*) sdk_version="${version%-*}+${version##*-}" ;;
    *) sdk_version="$version" ;;
  esac
fi

case "$version" in
  ''|*[!A-Za-z0-9.+-]*) echo "EXTERNAL_WARP_VERSION contains unsupported characters" >&2; exit 2 ;;
esac

for arg in "$@"; do
  case "$arg" in
    --repeat=*) repeat_count="${arg#*=}" ;;
    --skip-build) skip_build=1 ;;
    --headless) ;;
    --keep-fixture) keep_fixture=1 ;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done
case "$repeat_count" in
  ''|*[!0-9]*) echo "--repeat must be a positive integer" >&2; exit 2 ;;
  0) echo "--repeat must be at least 1" >&2; exit 2 ;;
esac

die() { echo "[linux acceptance] ERROR: $*" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || die "docker is required"
command -v timeout >/dev/null 2>&1 || die "GNU timeout is required"
timeout 15 docker info >/dev/null 2>&1 || die "docker is not running"
node "$root/build/all/acceptance/preflight-main.mjs" || exit 1
[ -f "$vault" ] || die "no acceptance vault at $vault"
config_reader="$root/tests/read-tests-config.sh"
[ -x "$config_reader" ] || die "test config reader is missing: $config_reader"
UR_ACCEPT_VAULT="$vault" "$config_reader" --ready validate
acc_user="$(UR_ACCEPT_VAULT="$vault" "$config_reader" get data_plane_account.email)"
acc_pass="$(UR_ACCEPT_VAULT="$vault" "$config_reader" get data_plane_account.password)"

timestamp="$(date +%Y%m%d-%H%M%S)"
artifacts="$here/tests/__acceptance__/$timestamp"
run_dir="$(mktemp -d "${TMPDIR:-/tmp}/urnetwork-linux-acceptance.XXXXXX")"
container_name="urnetwork-acceptance-${timestamp}-$$"
provider_container_name="urnetwork-peer-provider-${timestamp}-$$"
network_name="urnetwork-acceptance-${timestamp}-$$"
provider_dir="$run_dir/provider"
mkdir -p "$artifacts" "$(dirname "$fixture")" "$out_dir"
mkdir -p "$provider_dir"
chmod 700 "$run_dir" "$(dirname "$fixture")"
credentials="$run_dir/credentials"
printf '%s\n%s\n' "$acc_user" "$acc_pass" >"$credentials"
chmod 600 "$credentials"
tests_json="$run_dir/tests.json"
UR_ACCEPT_VAULT="$vault" "$config_reader" write-json "$tests_json"
unset acc_pass

release_retained_client() {
  local active_client="${1:-$artifacts/active-client-id}"
  [ -f "$active_client" ] || return 0
  echo "[linux acceptance] releasing retained network client"
  UR_ACCEPT_CREDENTIALS_FILE="$credentials" \
    timeout 90 node "$root/build/all/acceptance/client-cleanup.mjs" "$active_client"
}

cleanup() {
  exit_status=$?
  for cleanup_container in "$container_name" "$provider_container_name"; do
    remaining_container=""
    if ! remaining_container="$(timeout 15 docker ps -aq --filter "name=^/${cleanup_container}$")"; then
      echo "[linux acceptance] could not inspect container $cleanup_container" >&2
      exit_status=1
    elif [ -n "$remaining_container" ]; then
      if ! timeout 30 docker rm -f "$cleanup_container" >/dev/null; then
        echo "[linux acceptance] could not remove container $cleanup_container" >&2
        exit_status=1
      fi
    fi
  done
  if timeout 15 docker network inspect "$network_name" >/dev/null 2>&1; then
    if ! timeout 30 docker network rm "$network_name" >/dev/null; then
      echo "[linux acceptance] could not remove network $network_name" >&2
      exit_status=1
    fi
  fi
  if ! release_retained_client; then
    echo "[linux acceptance] could not release the retained network client" >&2
    exit_status=1
  fi
  if ! release_retained_client "$provider_dir/active-client-id"; then
    echo "[linux acceptance] could not release the retained peer provider client" >&2
    exit_status=1
  fi
  if ! rm -rf "$run_dir"; then
    echo "[linux acceptance] could not remove $run_dir" >&2
    exit_status=1
  fi
  if [ -n "$result_matrix" ]; then
    mkdir -p "$(dirname "$result_matrix")"
    matrix_status=PASS
    matrix_detail="Linux SDK/service acceptance completed"
    if [ "$exit_status" -ne 0 ]; then matrix_status=FAIL; matrix_detail="Linux acceptance runner failed; see artifacts"; fi
    for matrix_case in email phone solana bittensor instant password data-plane peer-to-peer; do
      printf 'linux\t%s\t%s\t%s\n' "$matrix_case" "$matrix_status" "$matrix_detail" >>"$result_matrix"
    done
    chmod 600 "$result_matrix"
  fi
  echo
  if [ "$exit_status" -eq 0 ]; then
    echo "[linux acceptance] ✓ ACCEPTANCE PASSED (artifacts: $artifacts)"
  else
    echo "[linux acceptance] ✗ ACCEPTANCE FAILED (artifacts: $artifacts)"
  fi
  exit "$exit_status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

if [ "$skip_build" -ne 1 ]; then
  echo "[linux acceptance] building local Linux artifacts"
  SRC_HOME="$root" \
  EXTERNAL_WARP_VERSION="$version" \
  ARCHES=arm64 \
  OUT_DIR="$out_dir" \
    timeout 3600 "$root/build/all/build-linux.sh" 2>&1 | tee "$artifacts/build.log"
else
  echo "[linux acceptance] reusing $out_dir"
fi

deb="$out_dir/urnetwork-daemon_${version}_arm64.deb"
appimage="$out_dir/URnetwork-${version}-arm64.AppImage"
[ -f "$deb" ] || die "missing locally built daemon package $deb"
[ -x "$appimage" ] || die "missing locally built AppImage $appimage"

echo "[linux acceptance] building the local SDK control agent"
(cd "$root/build/all/acceptance" && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 \
  timeout 600 go build -trimpath -o "$run_dir/agent" .)

echo "[linux acceptance] starting a separate same-platform peer provider"
timeout 30 docker network create "$network_name" >/dev/null
timeout 30 docker run -d --name "$provider_container_name" --platform linux/arm64 \
  --network "$network_name" \
  --user "$(id -u):$(id -g)" \
  -v "$run_dir/agent:/opt/urnetwork-acceptance/agent:ro" \
  -v "$credentials:/opt/urnetwork-acceptance/credentials:ro" \
  -v "$provider_dir:/provider" \
  urnetwork-linux-builder-daemon:arm64 \
  /opt/urnetwork-acceptance/agent \
    -peer-provider \
    -credentials /opt/urnetwork-acceptance/credentials \
    -active-client /provider/active-client-id \
    -state-dir /provider/state \
    -sdk-version "$sdk_version" \
    -app-version "$version" \
    -peer-provider-ready /provider/provider-client-id \
    -peer-provider-stop /provider/stop \
    -peer-provider-result /provider/result.json \
  >/dev/null
provider_ready=0
for _ in $(seq 1 180); do
  if [ -s "$provider_dir/provider-client-id" ]; then
    provider_ready=1
    break
  fi
  if [ "$(timeout 10 docker inspect -f '{{.State.Running}}' "$provider_container_name" 2>/dev/null || true)" != true ]; then
    timeout 15 docker logs "$provider_container_name" >"$artifacts/provider.log" 2>&1 || true
    die "peer provider exited before becoming ready"
  fi
  sleep 1
done
[ "$provider_ready" -eq 1 ] || die "timed out waiting for the peer provider"

echo "[linux acceptance] running $repeat_count complete repetition(s)"
set +e
# A private Docker cgroup namespace reports this process as 0::/. The daemon
# deliberately rejects that process-wide identity; expose its unique docker/id
# path. Docker Desktop also requires the privileged profile for BPF_PROG_LOAD,
# cgroup attachment and the child-cgroup proof used by --selftest-egress. This
# is a disposable local data-plane container, not a production service grant.
timeout --signal=TERM --kill-after=60s "$((900 + repeat_count * 900))" \
  docker run --name "$container_name" --rm --platform linux/arm64 \
  --network "$network_name" \
  --cgroupns=host \
  --privileged \
  --cap-add NET_ADMIN --device /dev/net/tun \
  -v "$out_dir:/out:ro" \
  -v "$artifacts:/artifacts" \
  -v "$run_dir/agent:/opt/urnetwork-acceptance/agent:ro" \
  -v "$credentials:/opt/urnetwork-acceptance/credentials:ro" \
  -v "$tests_json:/opt/urnetwork-acceptance/tests.json:ro" \
  -v "$root/build/all/acceptance/run-linux.sh:/opt/urnetwork-acceptance/run.sh:ro" \
  -v "$(dirname "$fixture"):/fixtures" \
  -v "$provider_dir:/peer-provider:ro" \
  -e UR_ACCEPT_DEB="/out/$(basename "$deb")" \
  -e UR_ACCEPT_VERSION="$version" \
  -e UR_ACCEPT_REPEAT="$repeat_count" \
  -e UR_ACCEPT_FIXTURE="/fixtures/$(basename "$fixture")" \
  -e UR_ACCEPT_TESTS=/opt/urnetwork-acceptance/tests.json \
  -e UR_ACCEPT_ARTIFACTS=/artifacts \
  -e UR_ACCEPT_PEER_PROVIDER_CLIENT=/peer-provider/provider-client-id \
  urnetwork-linux-builder-daemon:arm64 \
  bash /opt/urnetwork-acceptance/run.sh \
  2>&1 | tee "$artifacts/run.log"
acceptance_status=${PIPESTATUS[0]}
set -e

touch "$provider_dir/stop"
set +e
provider_exit="$(timeout --signal=TERM --kill-after=30s 60 docker wait "$provider_container_name")"
provider_wait_status=$?
set -e
timeout 15 docker logs "$provider_container_name" >"$artifacts/provider.log" 2>&1 || true
if [ "$provider_wait_status" -ne 0 ] || [ "$provider_exit" != 0 ] || \
   ! grep -q '"ok":true' "$provider_dir/result.json" 2>/dev/null; then
  echo "[linux acceptance] peer provider did not verify bidirectional traffic" >&2
  acceptance_status=1
fi
timeout 30 docker rm "$provider_container_name" >/dev/null || acceptance_status=1

if ! release_retained_client; then
  acceptance_status=1
fi
if ! release_retained_client "$provider_dir/active-client-id"; then
  acceptance_status=1
fi

if [ "$acceptance_status" -eq 0 ] && [ -f "$fixture" ] && [ "$keep_fixture" -ne 1 ]; then
  if timeout 90 node "$root/build/all/acceptance/fixture.mjs" delete "$fixture"; then
    rm -f "$fixture"
  else
    echo "could not delete instant-account fixture; retained at $fixture" >&2
    acceptance_status=1
  fi
fi

exit "$acceptance_status"
