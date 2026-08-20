#!/usr/bin/env bash
# bootstrap.sh — turn a fresh Ubuntu VM into a measurement host.
#
# Run once per node, as root. Everything installed here is either a build
# dependency or an experiment that would otherwise record itself as unavailable.
# The tuning at the end is what moves the host from the 'constrained' class
# towards 'measurement'; each step says what it buys, because an operator who
# does not know why a knob is turned cannot judge whether it stayed turned.
set -euo pipefail

[ "$(id -u)" = "0" ] || { echo "run as root: sudo bash cloud/bootstrap.sh" >&2; exit 1; }

export DEBIAN_FRONTEND=noninteractive

# A freshly booted Ubuntu image on a public cloud is already running
# unattended-upgrades, which holds the dpkg lock for the first minute or two of
# the instance's life. Without a timeout the first apt-get here fails, and the
# failure reads as a broken image rather than as a queue.
APT="apt-get -o DPkg::Lock::Timeout=600 -y -qq"

echo "==> packages"
$APT update >/dev/null 2>&1 || $APT update || true

# The build dependencies are separated from everything else because they are the
# only ones whose absence is fatal. An optional package that fails to install
# removes one experiment and the dataset records why; a compiler that fails to
# install removes the study, and should say so here rather than three steps
# later as a missing binary.
$APT install --no-install-recommends \
  build-essential cmake git pkg-config libssl-dev \
  || { echo "could not install the build dependencies; nothing can be built here" >&2; exit 1; }

$APT install --no-install-recommends \
  liburing-dev \
  libopenmpi-dev openmpi-bin \
  python3 python3-pip python3-venv \
  iproute2 ethtool net-tools iperf3 psmisc \
  linux-tools-common "linux-tools-$(uname -r)" \
  || echo "    some optional packages are unavailable; the experiments that need them will record it"

# WireGuard is the production encrypted tunnel E6 compares against. Absent, that
# baseline records itself as unavailable rather than failing the run.
$APT install --no-install-recommends wireguard-tools || \
  echo "    wireguard-tools unavailable; the WireGuard baseline will be skipped"

# Docker gives the container isolation tier. gVisor gives the sandbox tier.
if ! command -v docker >/dev/null 2>&1; then
  $APT install --no-install-recommends docker.io || \
    echo "    docker unavailable; the container tier will be skipped"
fi
if ! command -v runsc >/dev/null 2>&1; then
  ARCH="$(uname -m)"
  URL="https://storage.googleapis.com/gvisor/releases/release/latest/${ARCH}"
  if curl -fsSL "${URL}/runsc" -o /usr/local/bin/runsc 2>/dev/null; then
    chmod 755 /usr/local/bin/runsc
    echo "    installed runsc (gVisor)"
  else
    echo "    gVisor unavailable; the sandbox tier will be skipped"
  fi
fi
# Having the binary is not the same as Docker knowing about it. Without this
# registration `docker run --runtime=runsc` fails, E7 records the sandbox tier
# as unavailable, and the reason looks like a missing gVisor rather than an
# unconfigured one.
if command -v runsc >/dev/null 2>&1 && command -v docker >/dev/null 2>&1; then
  if runsc install >/dev/null 2>&1; then
    systemctl restart docker >/dev/null 2>&1 || true
    sleep 3
    if docker info --format '{{json .Runtimes}}' 2>/dev/null | grep -q runsc; then
      echo "    registered runsc with docker; E7's sandbox tier is available"
    else
      echo "    runsc did not register with docker; the sandbox tier will be skipped"
    fi
  else
    echo "    runsc install failed; the sandbox tier will be skipped"
  fi
fi

echo "==> python"
pip3 install --quiet --break-system-packages -r "$(dirname "$0")/../requirements.txt" 2>/dev/null || \
  pip3 install --quiet -r "$(dirname "$0")/../requirements.txt" || \
  echo "    could not install Python dependencies; the analysis will run on the Mac instead"

echo "==> interface"
NIC="$(ip -o -4 route show default | awk '{print $5}' | head -1)"
if [ -n "$NIC" ]; then
  DRV="$(basename "$(readlink -f "/sys/class/net/$NIC/device/driver" 2>/dev/null)" 2>/dev/null || echo unknown)"
  echo "    $NIC uses driver $DRV"
  [ "$DRV" = "gve" ] || echo "    note: this study describes gVNIC (gve); $DRV has a different per-packet cost"
  # Larger socket buffers: the drain window stages thousands of packets, and a
  # default-sized buffer would drop most of them before the hot path ran.
  sysctl -qw net.core.rmem_max=134217728
  sysctl -qw net.core.wmem_max=134217728
  sysctl -qw net.core.netdev_max_backlog=250000
  # Ring buffers to their maximum, so a burst is absorbed by the NIC rather than
  # showing up as loss that looks like a data-plane failure.
  ethtool -G "$NIC" rx 4096 tx 4096 2>/dev/null || true
fi

echo "==> cpu"
# The performance governor holds the clock still. The counter is invariant either
# way, so cycle counts stay meaningful, but wall-clock rates move under a scaling
# governor and the capacity probe in E5 would then chase its own tail.
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  [ -w "$g" ] && echo performance > "$g" 2>/dev/null || true
done
[ -w /sys/devices/system/cpu/intel_pstate/no_turbo ] && \
  echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true

# irqbalance moves interrupt affinity while the sweep runs, which shows up as a
# step change halfway through a factor level.
systemctl stop irqbalance 2>/dev/null || true
systemctl disable irqbalance 2>/dev/null || true

# io_uring must be permitted for E3's most informative row.
if [ -w /proc/sys/kernel/io_uring_disabled ]; then
  echo 0 > /proc/sys/kernel/io_uring_disabled 2>/dev/null || true
fi

echo "==> done"
echo "    Kernel isolation (isolcpus, nohz_full, rcu_nocbs) needs a reboot and is"
echo "    described in cloud/kernel-cmdline.md. Without it this host classifies as"
echo "    'constrained' and every dataset it writes says so."
