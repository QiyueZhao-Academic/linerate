#!/usr/bin/env bash
# netem.sh — inject network conditions on the egress interface.
#
# The label this prints is what the harness records with every point measured
# while the condition is active, so a curve taken under 5 ms of added delay
# cannot later be confused with a clean one. The format is fixed here and parsed
# by python/lr/admission.py; changing one without the other breaks the feature
# extraction, which is why the two are documented together.
#
#   ./cloud/netem.sh clear                 remove all conditions
#   ./cloud/netem.sh delay 5               5 ms of one-way delay
#   ./cloud/netem.sh loss 0.1              0.1% loss
#   ./cloud/netem.sh jitter 5 1            5 ms delay, 1 ms jitter
#   ./cloud/netem.sh combined 5 0.1        both
#   ./cloud/netem.sh label                 print the current label
set -euo pipefail

NIC="${LR_NIC:-$(ip -o -4 route show default | awk '{print $5}' | head -1)}"
[ -n "$NIC" ] || { echo "no default interface found; set LR_NIC" >&2; exit 1; }

need_root() {
  [ "$(id -u)" = "0" ] || { echo "run with sudo: sudo bash cloud/netem.sh $*" >&2; exit 1; }
}

clear_qdisc() { tc qdisc del dev "$NIC" root 2>/dev/null || true; }

case "${1:-}" in
  clear)
    need_root "$@"; clear_qdisc
    echo "none" > /tmp/lr_netem_label
    echo "cleared on $NIC; label none"
    ;;
  delay)
    need_root "$@"; ms="${2:-5}"
    clear_qdisc
    tc qdisc add dev "$NIC" root netem delay "${ms}ms"
    echo "d${ms}ms" > /tmp/lr_netem_label
    echo "delay ${ms}ms on $NIC; label d${ms}ms"
    ;;
  jitter)
    need_root "$@"; ms="${2:-5}"; j="${3:-1}"
    clear_qdisc
    # Correlation keeps consecutive delays related, as a real path's queueing is;
    # independent samples would produce reordering that netem then has to fix.
    tc qdisc add dev "$NIC" root netem delay "${ms}ms" "${j}ms" 25%
    echo "d${ms}ms-j${j}" > /tmp/lr_netem_label
    echo "delay ${ms}ms ± ${j}ms on $NIC; label d${ms}ms-j${j}"
    ;;
  loss)
    need_root "$@"; pct="${2:-0.1}"
    clear_qdisc
    tc qdisc add dev "$NIC" root netem loss "${pct}%"
    echo "l${pct}" > /tmp/lr_netem_label
    echo "loss ${pct}% on $NIC; label l${pct}"
    ;;
  combined)
    need_root "$@"; ms="${2:-5}"; pct="${3:-0.1}"
    clear_qdisc
    tc qdisc add dev "$NIC" root netem delay "${ms}ms" loss "${pct}%"
    echo "d${ms}ms-l${pct}" > /tmp/lr_netem_label
    echo "delay ${ms}ms, loss ${pct}% on $NIC; label d${ms}ms-l${pct}"
    ;;
  label)
    cat /tmp/lr_netem_label 2>/dev/null || echo none
    ;;
  show)
    tc qdisc show dev "$NIC"
    ;;
  *)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 64
    ;;
esac
