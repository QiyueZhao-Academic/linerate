#!/usr/bin/env bash
# isolate.sh — run a command inside one isolation tier.
#
# The tier has to be entered before the process starts, not during it, which is
# why this wraps rather than being called from inside the harness. Each tier sets
# LR_ISOLATION_TIER so the process records which sandbox produced its numbers; a
# cycle count that does not say which tier it came from is not comparable with
# one that does.
#
#   ./cloud/isolate.sh host      ./build/lr_bench virt-unit
#   ./cloud/isolate.sh netns     ./build/lr_bench virt-unit
#   ./cloud/isolate.sh docker    ./build/lr_bench virt-unit
#   ./cloud/isolate.sh gvisor    ./build/lr_bench virt-unit
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TIER="${1:-host}"; shift || true
[ $# -gt 0 ] || { echo "nothing to run" >&2; exit 64; }

NS="lrns0"

case "$TIER" in
  host)
    LR_ISOLATION_TIER=host exec "$@"
    ;;

  netns)
    # A namespace joined to the host by a veth pair. Same kernel, same scheduler,
    # so whatever this costs is the veth path and the extra forwarding decision
    # and nothing else.
    [ "$(id -u)" = "0" ] || { echo "netns needs root" >&2; exit 1; }
    ip netns del "$NS" 2>/dev/null || true
    ip link del "veth-$NS" 2>/dev/null || true
    ip netns add "$NS"
    ip link add "veth-$NS" type veth peer name "veth-p"
    ip link set "veth-p" netns "$NS"
    ip addr add 10.200.0.1/24 dev "veth-$NS"
    ip link set "veth-$NS" up
    ip netns exec "$NS" ip addr add 10.200.0.2/24 dev "veth-p"
    ip netns exec "$NS" ip link set "veth-p" up
    ip netns exec "$NS" ip link set lo up
    set +e
    ip netns exec "$NS" env LR_ISOLATION_TIER=netns "$@"
    rc=$?
    set -e
    ip netns del "$NS" 2>/dev/null || true
    ip link del "veth-$NS" 2>/dev/null || true
    exit $rc
    ;;

  docker|gvisor)
    command -v docker >/dev/null 2>&1 || { echo "docker is not installed" >&2; exit 1; }
    RUNTIME=""
    if [ "$TIER" = "gvisor" ]; then
      command -v runsc >/dev/null 2>&1 || { echo "runsc is not installed" >&2; exit 1; }
      RUNTIME="--runtime=runsc"
    fi
    docker image inspect linerate:measure >/dev/null 2>&1 || \
      docker build -q -t linerate:measure -f "$ROOT/cloud/containers/Dockerfile" "$ROOT" >/dev/null
    # The binary is bind-mounted rather than baked in, so the container measures
    # exactly the build the host measured. A rebuild inside the image would
    # introduce a second compiler invocation as a confounder.
    exec docker run --rm $RUNTIME \
      -v "$ROOT:/work:ro" -w /work \
      -e "LR_ISOLATION_TIER=$TIER" \
      --ulimit memlock=-1:-1 \
      linerate:measure "$@"
    ;;

  *)
    echo "unknown tier '$TIER'. One of: host netns docker gvisor" >&2
    exit 64
    ;;
esac
