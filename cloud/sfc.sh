#!/usr/bin/env bash
# sfc.sh — a service function chain of encrypting data planes.
#
# Builds a chain of network namespaces joined by veth pairs, with a copy of the
# data plane in each. Every hop terminates one security association and
# originates another, which is what a chain of encrypting virtual network
# functions does, so the slope of cost against chain length is the price of
# composition rather than of the function.
#
# Chain of length n:
#
#   host ──veth──[ns1: lr_sink]──veth──[ns2: lr_sink]── … ──[nsN]
#
# Each namespace gets a /30 on each side, so the addressing is unambiguous and a
# packet can only go forwards.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/results/raw/tiers"
MAXLEN=4; CIPHER="aes-256-gcm"; PAYLOAD=512; REPS=5; PACKETS=20000
for a in "$@"; do
  case "$a" in
    --max=*)     MAXLEN="${a#*=}" ;;
    --cipher=*)  CIPHER="${a#*=}" ;;
    --payload=*) PAYLOAD="${a#*=}" ;;
    --quick)     MAXLEN=2; REPS=3; PACKETS=4000 ;;
  esac
done

[ "$(id -u)" = "0" ] || { echo "service chaining needs root (it creates namespaces)" >&2; exit 1; }
mkdir -p "$OUT"

teardown() {
  for i in $(seq 1 "$MAXLEN"); do
    ip netns del "lrsfc$i" 2>/dev/null || true
    ip link del "sfc${i}a" 2>/dev/null || true
  done
}
trap teardown EXIT
teardown

for len in $(seq 1 "$MAXLEN"); do
  printf '==> chain length %d\n' "$len"
  teardown
  for i in $(seq 1 "$len"); do
    ip netns add "lrsfc$i"
    ip link add "sfc${i}a" type veth peer name "sfc${i}b"
    ip link set "sfc${i}b" netns "lrsfc$i"
    ip addr add "10.20$i.0.1/30" dev "sfc${i}a" 2>/dev/null || true
    ip link set "sfc${i}a" up
    ip netns exec "lrsfc$i" ip addr add "10.20$i.0.2/30" dev "sfc${i}b"
    ip netns exec "lrsfc$i" ip link set "sfc${i}b" up
    ip netns exec "lrsfc$i" ip link set lo up
  done

  # The measurement runs in the last namespace of the chain, so the cost it
  # reports has traversed every hop before it.
  LAST="lrsfc$len"
  if ip netns exec "$LAST" env LR_ISOLATION_TIER=netns \
       "$ROOT/build/lr_bench" virt-unit --cipher="$CIPHER" --payload="$PAYLOAD" \
       --reps="$REPS" --packets="$PACKETS" --chain="$len" \
       > "$OUT/chain$len.json" 2>/dev/null; then
    python3 - "$OUT/chain$len.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(f"    {d['cycles_median']:.0f} cycles/packet over {d['chain_length']} hop(s)")
PY
  else
    rm -f "$OUT/chain$len.json"
    echo "    could not measure at this length"
  fi
done
echo
echo "Now run:  ./build/lr_bench e7 --out=results/raw"
