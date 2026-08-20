#!/usr/bin/env bash
# pin.sh — report the CPU layout and suggest an assignment.
#
# The harness pins its own threads; this exists so an operator can see what the
# machine looks like before deciding whether the assignment makes sense.
set -euo pipefail

echo "topology"
lscpu | grep -E "^(Architecture|CPU\(s\)|Thread|Core|Socket|Model name|NUMA)" | sed 's/^/  /'
echo
echo "sibling map (a shared sibling list means two logical CPUs share one core)"
for c in /sys/devices/system/cpu/cpu[0-9]*; do
  n="$(basename "$c" | tr -d 'cpu')"
  s="$(cat "$c/topology/thread_siblings_list" 2>/dev/null || echo '?')"
  printf '  cpu%-3s siblings %s\n' "$n" "$s"
done
echo
echo "kernel isolation"
grep -o -E '(isolcpus|nohz_full|rcu_nocbs)=[^ ]*' /proc/cmdline | sed 's/^/  /' || echo "  none set"
echo
echo "suggested assignment"
echo "  cpu0  data plane"
echo "  cpu1  io_uring submission poller, when uring-sqpoll is measured"
echo "  cpu2  load generator, on the generating node"
echo "  cpu3+ everything else, including the kernel's own work"
echo
echo "The submission poller needs a core of its own. Where it does not get one,"
echo "E3 records that the syscall-free path was competing rather than bypassing,"
echo "and the analysis declines to read a bound from it."
