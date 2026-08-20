# The measurement environment

Two machines and a Mac. The Mac never measures.

```
   MacBook (development host)
      │  gcloud ssh / scp, one short call at a time
      ├────────────────────────────┐
      ▼                            ▼
  linerate-a                  linerate-b
  load generator              data plane, drives the sweep
  lr_loadgend :9099           lr_bench, lr_sink
  echo server  :9098          sweep.log, read back in polls
  iperf3 -s    :5201
      │                            │
      └────── gVNIC, same zone ────┘
              10.128.0.0/9
```

Everything on either node that outlives a single call — the generator, the
`iperf3` server, the sweep itself — is started by `cloud/nodectl.sh` as a
transient `systemd` unit rather than as a background child of the ssh session.
An ssh session does not close while a process it started still holds the
channel, so a backgrounded daemon leaves the driver waiting for a call that has
already done its work; and an hour-long sweep inside one connection is an hour
in which a dropped tunnel destroys the run. Both are structural, and both are
removed by the process belonging to init instead of to the session.

Being started is verified rather than assumed. `systemd-run` returns when the
unit is accepted, not when its process exists, so every spawn is followed by a
search for the process itself, a fallback to `setsid` if it is not there, and a
`diagnose-` subcommand that returns the log, the unit's status and journal, the
listening sockets and the launcher script in one round trip. The driver fetches
that itself before giving up, because by then it is about to delete the
instance that holds it.

## Why this shape

**Two nodes, not one.** A data plane measured against a generator on the same
host shares that host's cache, its memory bandwidth and its scheduler with the
thing it is measuring. On a small instance it also shares a core, at which point
the "queue" the latency experiment forms is a scheduling artefact. The harness
detects that case and marks the run uninterpretable rather than reporting a knee
that is not there.

**A control channel, not per-unit `ssh`.** A full sweep is several hundred units.
Opening an `ssh` connection for each would spend more time on `ssh` than on
measurement, and would put the operator's laptop and its network inside the
measurement loop. Instead node B opens one TCP connection to `lr_loadgend` on
node A and drives the whole sweep over it (`include/lr/ctrl.hpp`).

**gVNIC explicitly.** `mac/lib/vm.sh` passes `nic-type=GVNIC`. The default
`virtio_net` driver has a different per-packet cost, and a fixed cost reported
without naming the driver is not reproducible. The harness records
`nic_driver` with every fragment and `cloud/bootstrap.sh` warns when it is not
`gve`.

**Same zone.** Both instances land in one zone so the link between them is a
single hop. Cross-zone would add a variable that is not being studied.

## Instance sizing

Default `c3-standard-4`. Four vCPUs is the minimum that makes every experiment
meaningful, and the reason is specific rather than superstitious:

| core | what wants it |
|---|---|
| 1 | the data plane |
| 2 | the io_uring submission poller, when `uring-sqpoll` is measured |
| 3 | the load generator, on the generating node |
| 4 | the kernel's own work, interrupts, the `ssh` session |

On fewer cores the submission poller competes with the data plane, and E3's
kernel-bypass row measures contention rather than bypass. The harness records
`sqpoll_contends_for_cpu` in that case and the analysis declines to read a bound
from it — but a run that records "this bounds nothing" is a wasted run.

## Software

Installed by `cloud/bootstrap.sh`:

| package | what it is for | absent |
|---|---|---|
| `build-essential`, `cmake` | the build | nothing works |
| `libssl-dev` | OpenSSL 3, the only hard dependency | nothing works |
| `liburing-dev` | the `uring` and `uring-sqpoll` transports | E3 loses two of four rows |
| `libopenmpi-dev` | E2's cross-node half | `e2_mpi` records unavailable |
| `iperf3` | the no-crypto network baseline; the driver starts a server on node A | that row of E6 records unavailable |
| `wireguard-tools` | the production-tunnel baseline | that row of E6 records unavailable |
| `docker.io` | E7's container tier | that tier records unavailable |
| `runsc` (gVisor) | E7's sandbox tier; bootstrap also registers it as a Docker runtime, without which `--runtime=runsc` fails and the tier looks absent rather than unconfigured | that tier records unavailable |
| `iproute2` (`tc`) | E5's network conditions | E5 runs clean only, and E9 has one condition to split on |
| `python3` + `requirements.txt` | the analysis | analyse on the Mac instead |

Nothing on that list is required. Each absence removes an experiment and the
dataset records why.

## Tuning applied by bootstrap

| knob | value | what it prevents |
|---|---|---|
| `net.core.rmem_max`, `wmem_max` | 128 MiB | the drain window's staged packets being dropped before the hot path runs |
| `net.core.netdev_max_backlog` | 250000 | drops at the driver that look like data-plane failures |
| `ethtool -G rx/tx` | 4096 | the same, one layer down |
| `scaling_governor` | `performance` | the clock moving under a sweep; the counter is invariant so cycle counts survive, but wall-clock rates do not |
| `intel_pstate/no_turbo` | 1 | the first replicate of a point being faster than the rest |
| `irqbalance` | stopped | interrupt affinity moving mid-sweep, which appears as a step change on one factor level |
| `kernel.io_uring_disabled` | 0 | E3's most informative row being unavailable |

## Privilege

The core experiments need none. The virtualisation tiers, the service chain and
the network conditions need root: a namespace has to be created, a container
runtime invoked, a qdisc attached. `cloud/nodectl.sh start-sweep` runs the sweep
as a transient `systemd` unit, which is root, so `--full` measures them without
a second decision being asked for. The dataset is chowned back to the login user
when the sweep ends, so fetching it needs no privilege either.

Nothing in the harness behaves differently as root — it sets no scheduling
policy and locks no memory — so the privilege changes which experiments can run
and not what any of them measures.

## Tuning that needs a reboot

Kernel isolation (`isolcpus`, `nohz_full`, `rcu_nocbs`) is in
`cloud/kernel-cmdline.md`. Without it the host classifies as `constrained`
rather than `measurement`, and every dataset says so. It still measures; the
numbers are simply noisier, and the noise arrives in bursts rather than being
spread evenly.

## What the cloud cannot give

The `host` tier is itself a hypervisor guest, and nothing inside the guest can
see or prevent a neighbouring tenant's use of the same physical core. That is
why the sweep order is interleaved and why an anchor is measured at both ends:
interference that cannot be removed is at least prevented from landing entirely
on one factor level, and a machine that changed underneath the measurement voids
the sweep rather than being reported. It is also why E7 reports the incremental
cost of each isolation layer rather than claiming to measure virtualisation.

## Cost

Two `c3-standard-4` instances bill by the second while they exist. A full sweep
is under an hour. `./mac/linerate down` deletes both; `./mac/linerate status`
says whether anything is still running. Deleting the instances leaves the project
in place with no billable resources in it.
