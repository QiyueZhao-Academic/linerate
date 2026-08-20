# Kernel isolation for the measurement host

Without these, the harness classifies the host as `constrained` rather than
`measurement`, and every dataset it writes carries that classification and its
caveats. It will still measure. The numbers are simply noisier, and the noise is
not random: it arrives in bursts when the scheduler, the tick or an RCU callback
lands on a data-plane core.

## What to add

On a four-vCPU instance, reserving cpu1 and cpu2 for the data plane and the
io_uring submission poller:

```
isolcpus=1,2 nohz_full=1,2 rcu_nocbs=1,2 rcu_nocb_poll intel_pstate=disable
```

| parameter | what it prevents |
|---|---|
| `isolcpus` | the general scheduler placing any other runnable task on those cores |
| `nohz_full` | the periodic timer tick interrupting a core that has one runnable task |
| `rcu_nocbs` | RCU callbacks running on those cores; they move to a housekeeping core |
| `rcu_nocb_poll` | the offloaded callbacks being woken by an IPI back to the isolated core |
| `intel_pstate=disable` | the driver moving the clock during a sweep |

Leaving cpu0 and cpu3 unisolated is deliberate: interrupts, the kernel's own
work and the ssh session all need somewhere to go, and isolating every core
simply moves that work back onto the isolated ones.

## Applying it

```
sudo sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 isolcpus=1,2 nohz_full=1,2 rcu_nocbs=1,2 rcu_nocb_poll intel_pstate=disable"/' /etc/default/grub
sudo update-grub
sudo reboot
```

Then confirm, and check that the harness agrees:

```
cat /proc/cmdline
./build/lr_selftest | head -3
```

The self-test prints the host class it derived. If it still says `constrained`
after a reboot, `tools/doctor.py` says which of the three parameters did not
take.

## Why this is not enough on its own

Kernel isolation removes interference from *this* guest. On a public cloud the
guest shares physical cores with other tenants through the hypervisor, and
nothing inside the guest can see or prevent that. It is why the sweep order is
interleaved and why an anchor is measured at both ends: interference that cannot
be removed is at least prevented from landing entirely on one factor level, and
a machine that changed underneath the measurement voids the sweep rather than
being reported.
