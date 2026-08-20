#!/usr/bin/env bash
# baselines.sh — the network half of E6.
#
# Three references over the same link, in the same session, at the same payload:
#
#   iperf3 over UDP        no cryptography, no user-space forwarding. The upper
#                          bound on what any encrypted data plane here could do.
#   WireGuard + iperf3     a production encrypted tunnel, in the kernel.
#   this data plane        the thing under study, over the same wire.
#
# Each is measured the same way and the differences between them are structural,
# not incidental: WireGuard does not cross the user-kernel boundary per packet,
# and this data plane decrypts and re-encrypts where WireGuard does one of the
# two. The fragment records both facts so the report cannot present the numbers
# as a like-for-like race.
#
#   ./cloud/baselines.sh --peer=10.128.0.2 --local=10.128.0.3 [--seconds=10]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PEER=""; LOCAL=""; SECONDS_RUN=10; PAYLOAD=1432
for arg in "$@"; do
  case "$arg" in
    --peer=*)    PEER="${arg#*=}" ;;
    --local=*)   LOCAL="${arg#*=}" ;;
    --seconds=*) SECONDS_RUN="${arg#*=}" ;;
    --payload=*) PAYLOAD="${arg#*=}" ;;
    *) echo "unknown option $arg" >&2; exit 64 ;;
  esac
done

FRAG="$ROOT/results/raw/e6_network_baselines.json"
emit_unavailable() {
  python3 "$ROOT/tools/emit_fragment.py" --key e6_network --out "$FRAG" \
    --unavailable "$1"
  exit 0
}

[ -n "$PEER" ] || emit_unavailable "no peer address given, so there is no link to measure"

# --- iperf3, no cryptography ------------------------------------------------
IPERF_GBPS="null"; IPERF_NOTE="not run"
if command -v iperf3 >/dev/null 2>&1; then
  echo "==> iperf3 over UDP to $PEER"
  # -b 0 asks for as much as the link will take. Below that, iperf3 paces and
  # would report the pacing rate rather than the link's capacity.
  if OUT="$(iperf3 -c "$PEER" -u -b 0 -l "$PAYLOAD" -t "$SECONDS_RUN" -J 2>/dev/null)"; then
    IPERF_GBPS="$(printf '%s' "$OUT" | python3 -c '
import json,sys
d=json.load(sys.stdin)
print(d["end"]["sum"]["bits_per_second"]/1e9)
' 2>/dev/null || echo null)"
    IPERF_NOTE="iperf3 -u -b 0 -l $PAYLOAD for ${SECONDS_RUN}s"
  else
    IPERF_NOTE="iperf3 could not reach a server on $PEER (the driver starts one with cloud/nodectl.sh start-iperf3; by hand it is: iperf3 -s)"
  fi
else
  IPERF_NOTE="iperf3 is not installed"
fi

# --- WireGuard, a production encrypted tunnel -------------------------------
WG_GBPS="null"; WG_NOTE="not run"
if command -v wg >/dev/null 2>&1 && command -v iperf3 >/dev/null 2>&1 && [ "$(id -u)" = "0" ]; then
  echo "==> WireGuard tunnel"
  if bash "$ROOT/cloud/wireguard.sh" up --peer="$PEER" --local="$LOCAL" >/dev/null 2>&1; then
    TUN_PEER="$(bash "$ROOT/cloud/wireguard.sh" peer-ip 2>/dev/null || echo '')"
    if [ -n "$TUN_PEER" ] && OUT="$(iperf3 -c "$TUN_PEER" -u -b 0 -l "$PAYLOAD" \
                                     -t "$SECONDS_RUN" -J 2>/dev/null)"; then
      WG_GBPS="$(printf '%s' "$OUT" | python3 -c '
import json,sys
d=json.load(sys.stdin)
print(d["end"]["sum"]["bits_per_second"]/1e9)
' 2>/dev/null || echo null)"
      WG_NOTE="same iperf3 invocation inside a WireGuard tunnel"
    else
      WG_NOTE="the tunnel came up but no iperf3 server answered inside it"
    fi
    bash "$ROOT/cloud/wireguard.sh" down >/dev/null 2>&1 || true
  else
    WG_NOTE="could not bring up a WireGuard tunnel"
  fi
else
  WG_NOTE="wg, iperf3 or root privileges unavailable"
fi

# --- this data plane, over the same wire ------------------------------------
LR_GBPS="null"; LR_NOTE="not run"
if [ -x "$ROOT/build/lr_bench" ] && [ -n "$LOCAL" ]; then
  echo "==> linerate over the same link"
  # One E1 unit in wire mode gives packets per second; the payload converts it to
  # a bit rate on the same axis as the two references above.
  if OUT="$("$ROOT/build/lr_bench" e1-unit --cipher=aes-256-gcm \
             --payload="$PAYLOAD" --packets=200000 --warmup=3 2>/dev/null)"; then
    LR_GBPS="$(printf '%s' "$OUT" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(d['pps'] * $PAYLOAD * 8 / 1e9)
" 2>/dev/null || echo null)"
    LR_NOTE="single shard, aes-256-gcm, decrypt and re-encrypt, payload $PAYLOAD"
  fi
fi

mkdir -p "$ROOT/results/raw"
cat <<JSON | python3 "$ROOT/tools/emit_fragment.py" --key e6_network --out "$FRAG"
{
  "available": true,
  "payload_bytes": $PAYLOAD,
  "seconds": $SECONDS_RUN,
  "peer": "$PEER",
  "iperf3_gbps": $IPERF_GBPS,
  "iperf3_note": "$IPERF_NOTE",
  "wireguard_gbps": $WG_GBPS,
  "wireguard_note": "$WG_NOTE",
  "linerate_gbps": $LR_GBPS,
  "linerate_note": "$LR_NOTE",
  "comparability_note": "iperf3 does no cryptography; WireGuard encrypts in the kernel and does not cross the user-kernel boundary per packet; linerate decrypts and re-encrypts in user space. The three are references, not a race."
}
JSON
echo "wrote $FRAG"
