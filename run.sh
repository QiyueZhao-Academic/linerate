#!/usr/bin/env bash
# run.sh — measure, analyse, typeset.
#
#   ./run.sh --quick                     about two minutes, exercises everything
#   ./run.sh                             the core experiments
#   ./run.sh --full --peer=IP --local=IP everything, including the two-node and
#                                        privileged experiments
#
# Every experiment writes a fragment whether or not it could run. One that could
# not records why, so the dataset is always complete and the report says what was
# missing instead of quietly leaving a gap.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

MODE="core"; PASS=""
for a in "$@"; do
  case "$a" in
    --quick) MODE="quick"; PASS="$PASS --quick" ;;
    --full)  MODE="full" ;;
    *)       PASS="$PASS $a" ;;
  esac
done

BENCH="./build/lr_bench"
[ -x "$BENCH" ] || { echo "no build. Run ./setup.sh first." >&2; exit 1; }

echo "==> self-test"
./build/lr_selftest > /tmp/lr_selftest.log 2>&1 || {
  tail -20 /tmp/lr_selftest.log
  echo "the self-test failed; refusing to measure" >&2
  exit 1
}
echo "    $(tail -1 /tmp/lr_selftest.log)"

echo "==> host"
python3 tools/doctor.py 2>/dev/null | tail -3 | sed 's/^/    /' || true

mkdir -p results/raw

if [ "$MODE" = "full" ]; then
  # shellcheck disable=SC2086
  bash cloud/remote-sweep.sh $PASS
else
  # shellcheck disable=SC2086
  $BENCH env --out=results/raw $PASS
  for e in e1 e2 e3 e4 e5 e6 e7 controls; do
    echo "==> $e"
    # shellcheck disable=SC2086
    $BENCH "$e" --out=results/raw $PASS || \
      echo "    $e did not complete; its fragment records why"
  done
  # Each experiment that did not run gets its own reason. A single generic
  # reason would be wrong for at least one of them, and a wrong reason in the
  # dataset is worse than none: it sends the next reader to fix the wrong thing.
  [ -f results/raw/e2_mpi_scaling.json ] || python3 tools/emit_fragment.py \
    --key e2_mpi --out results/raw/e2_mpi_scaling.json \
    --unavailable "cross-node scaling needs a second node; run ./run.sh --full --peer=IP"
  [ -f results/raw/e6_network_baselines.json ] || python3 tools/emit_fragment.py \
    --key e6_network --out results/raw/e6_network_baselines.json \
    --unavailable "iperf3 and WireGuard baselines need a peer node over a real link; run ./run.sh --full --peer=IP"
  if [ ! -f results/raw/e8_gpu.json ]; then
    if [ -x ./build/lr_gpu_aead ]; then
      ./build/lr_gpu_aead --out=results/raw || true
    else
      python3 tools/emit_fragment.py --key e8 --out results/raw/e8_gpu.json \
        --unavailable "lr_gpu_aead was not built: no CUDA toolkit was found when this build was configured"
    fi
  fi
  [ -f results/raw/e9_admission.json ] || python3 tools/emit_fragment.py \
    --key e9 --out results/raw/e9_admission.json \
    --unavailable "the admission model is fitted by python/make_report.py once E5 has enough conditions to split on"
  echo "==> merging"
  python3 tools/merge_results.py --raw results/raw --out results/results.json
fi

echo "==> report"
python3 python/make_report.py || echo "    the report did not build; the dataset is still in results/"

echo "==> checks"
python3 tools/check_wiring.py  || echo "    wiring problems above"
python3 tools/check_numbers.py || echo "    numerical problems above"

echo
echo "results/results.json          the dataset"
echo "results/linerate-report.pdf   the report"
