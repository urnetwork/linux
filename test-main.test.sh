#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
runner="$root/test-main.sh"
setup_runner="$(dirname "$root")/build/all/linux/setup.sh"

# Docker's default private cgroup namespace exposes the acceptance process as
# cgroup-v2 root (0::/). The daemon intentionally rejects that identity because
# an nft socket rule for it would match every process. Keep the real data-plane
# container in the host cgroup namespace so /proc/self/cgroup names its unique
# docker/<id> directory.
data_plane_command="$({
  sed -n '/docker run --name "$container_name"/,/bash \/opt\/urnetwork-acceptance\/run.sh/p' "$runner"
} || true)"

if [ -z "$data_plane_command" ]; then
  echo "could not locate the Linux acceptance data-plane container command" >&2
  exit 1
fi

count="$(printf '%s\n' "$data_plane_command" | grep -c -- '--cgroupns=host' || true)"
if [ "$count" -ne 1 ]; then
  echo "data-plane container must use exactly one --cgroupns=host flag (found $count)" >&2
  exit 1
fi

count="$(printf '%s\n' "$data_plane_command" | grep -c -- '--privileged' || true)"
if [ "$count" -ne 1 ]; then
  echo "data-plane container must use exactly one --privileged flag (found $count)" >&2
  exit 1
fi

setup_command="$({
  sed -n '/smoke-testing the acceptance tunnel and cgroup-BPF privileges/,/then/p' "$setup_runner"
} || true)"
if [ -z "$setup_command" ]; then
  echo "could not locate the Linux setup privilege smoke test" >&2
  exit 1
fi
for required in '--privileged' '--cgroupns=host' 'cgroup.procs'; do
  count="$(printf '%s\n' "$setup_command" | grep -c -- "$required" || true)"
  if [ "$count" -ne 1 ]; then
    echo "Linux setup smoke must carry exactly one $required boundary (found $count)" >&2
    exit 1
  fi
done

echo "linux acceptance cgroup/BPF privilege regression: PASS"
