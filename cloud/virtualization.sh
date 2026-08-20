#!/usr/bin/env bash
# virtualization.sh — run the same measurement in each isolation tier.
#
# Writes one file per tier into results/raw/tiers/, which `lr_bench e7` reads
# back and assembles. A tier that cannot be entered on this host is skipped and
# leaves no file, and E7 records the absence rather than the run failing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/results/raw/tiers"
mkdir -p "$OUT"

CIPHER="aes-256-gcm"; PAYLOAD=512; REPS=7; PACKETS=30000
for a in "$@"; do
  case "$a" in
    --cipher=*)  CIPHER="${a#*=}" ;;
    --payload=*) PAYLOAD="${a#*=}" ;;
    --reps=*)    REPS="${a#*=}" ;;
    --packets=*) PACKETS="${a#*=}" ;;
    --quick)     REPS=3; PACKETS=4000 ;;
  esac
done

ARGS="virt-unit --cipher=$CIPHER --payload=$PAYLOAD --reps=$REPS --packets=$PACKETS"

for tier in host netns docker gvisor; do
  printf '==> %s\n' "$tier"
  if bash "$ROOT/cloud/isolate.sh" "$tier" "$ROOT/build/lr_bench" $ARGS \
       > "$OUT/$tier.json" 2>/dev/null; then
    python3 - "$OUT/$tier.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(f"    {d['cycles_median']:.0f} cycles/packet, cv {d['cycles_cv']:.4f}")
PY
  else
    rm -f "$OUT/$tier.json"
    echo "    unavailable on this host; E7 will record the absence"
  fi
done
echo
echo "Now run:  ./build/lr_bench e7 --out=results/raw"
