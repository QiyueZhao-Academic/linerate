#!/usr/bin/env bash
# remote-sweep.sh — the full sweep, as node B runs it.
#
# run.sh is the entry point an operator uses. This is what run.sh calls for the
# --full path, and it is separate because it needs root for the tiers, the chain
# and the network conditions, while the core experiments do not: an operator
# should be able to get E1 through E5 without granting anything.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PEER=""; LOCAL=""; EXTRA=""
for a in "$@"; do
  case "$a" in
    --peer=*)  PEER="$a" ;;
    --local=*) LOCAL="$a" ;;
    *)         EXTRA="$EXTRA $a" ;;
  esac
done

BENCH="./build/lr_bench"
[ -x "$BENCH" ] || { echo "no build; run setup.sh first" >&2; exit 1; }

echo "==> environment"
$BENCH env --out=results/raw $PEER $LOCAL $EXTRA

echo "==> network baselines"
if [ -n "$PEER" ]; then
  bash cloud/baselines.sh "--peer=${PEER#--peer=}" "--local=${LOCAL#--local=}" || \
    echo "    baselines incomplete; the fragment records why"
else
  python3 tools/emit_fragment.py --key e6_network \
    --out results/raw/e6_network_baselines.json \
    --unavailable "no peer node, so there is no link to measure"
fi

echo "==> virtualisation tiers"
if [ "$(id -u)" = "0" ]; then
  bash cloud/virtualization.sh $EXTRA || echo "    some tiers were unavailable"
  bash cloud/sfc.sh $EXTRA || echo "    the service chain could not be built"
else
  echo "    not root; the tiers and the chain are skipped and E7 will say so"
fi

echo "==> latency under network conditions"
# Several conditions, because the admission model in E9 needs more than one to
# split on without leaking neighbouring rates across the boundary. "clear" is one
# of them, so this loop already produces the clean curve and the core loop below
# does not need to produce it a second time.
NETEM_RAN=0
if [ "$(id -u)" = "0" ] && command -v tc >/dev/null 2>&1; then
  for cond in "clear" "delay 2" "delay 10" "jitter 5 2" "loss 0.5"; do
    # shellcheck disable=SC2086
    bash cloud/netem.sh $cond >/dev/null 2>&1 || continue
    label="$(bash cloud/netem.sh label)"
    echo "    condition $label"
    LR_NETEM_LABEL="$label" $BENCH e5 --out=results/raw/netem-$label $PEER $LOCAL $EXTRA \
      >/dev/null 2>&1 || true
    NETEM_RAN=1
  done
  bash cloud/netem.sh clear >/dev/null 2>&1 || true
else
  echo "    tc unavailable or not root; E5 runs under clean conditions only"
fi

echo "==> core experiments"
for e in e1 e2 e3 e4 e5 e6 e7 controls; do
  # E5 writes results/raw/e5_latency.json, which is the same path the folded
  # multi-condition fragment occupies. Running it here after the conditions had
  # been measured overwrote five curves with one, and E9 then declined to fit an
  # admission model for want of the points that had just been thrown away. The
  # conditions loop already includes the clean case, so where it ran, this does
  # not need to.
  if [ "$e" = "e5" ] && [ "$NETEM_RAN" = "1" ]; then
    echo "    e5 (measured under $(ls -d results/raw/netem-* 2>/dev/null | wc -l | tr -d ' ') network conditions above)"
    continue
  fi
  echo "    $e"
  $BENCH "$e" --out=results/raw $PEER $LOCAL $EXTRA || \
    echo "      $e did not complete; its fragment records why"
done

# Folded after the core loop, not inside the conditions loop, so that nothing
# written later can land on top of it.
if [ "$NETEM_RAN" = "1" ]; then
  echo "==> folding the network conditions into one E5 fragment"
  python3 tools/merge_netem.py --raw results/raw || true
fi

echo "==> cross-node scaling"
if [ -x ./build/lr_mpi_scale ] && [ -n "$PEER" ]; then
  HOSTS="localhost,${PEER#--peer=}"
  mpirun --allow-run-as-root -np 2 --host "$HOSTS" \
    ./build/lr_mpi_scale --out=results/raw 2>/dev/null || \
    echo "    MPI could not reach both nodes; E2's cross-node half is skipped"
else
  echo "    no MPI binary or no peer; E2's cross-node half is skipped"
fi

echo "==> gpu"
if [ -x ./build/lr_gpu_aead ]; then
  ./build/lr_gpu_aead --out=results/raw || true
else
  python3 tools/emit_fragment.py --key e8 --out results/raw/e8_gpu.json \
    --unavailable "no CUDA toolkit on this host, so lr_gpu_aead was not built"
fi

echo "==> merging"
python3 tools/merge_results.py --raw results/raw --out results/results.json
echo "done"
