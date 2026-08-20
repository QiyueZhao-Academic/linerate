# Limitations

What this study does not establish, stated plainly, so that a reader does not
have to infer it from what is missing.

## The numbers are specific to one machine

Compilation uses native architecture flags, so absolute cycle counts belong to
the microarchitecture that produced them. Portable binaries would understate what
a tuned data plane achieves, which is the wrong error for this study to make.

`s*` transfers because it is a ratio of two quantities that scale together with
the clock. `a` and `b` separately do not. Every generalisation in the report is
about `s*` or about a ratio; where an absolute figure is quoted it is qualified
by the host.

Every fragment carries the environment that produced it, and
`tools/merge_results.py` refuses to combine fragments from different machines or
builds unless told to. That prevents silent mixing but does not create
generality: one machine is one machine.

## The path crosses each payload byte twice

The data plane decrypts and re-encrypts. That is a real forwarding topology and
it makes `b` large enough to separate cleanly from `a`, but it is not the only
one. A single-pass endpoint would have roughly half this `b` and therefore
roughly twice this `s*`. Per-pass figures are published alongside so the
adjustment can be made rather than guessed at.

## The kernel-bypass bound is conditional

`uring-sqpoll` issues no system calls on the submission side, which is what makes
it a measurement of the boundary rather than an extrapolation. But a submission
poller is a second runnable thread, and where it shares a core with the data
plane it is a competitor, not a bypass. On such a host the difference between
`posix` and `uring-sqpoll` can be negative and bounds nothing. The condition is
recorded in the dataset and stated in the report; it is not a defect of the
mechanism.

Nor is `uring-sqpoll` a true kernel bypass. A userspace driver — DPDK, netmap,
AF_XDP with a dedicated queue — removes the kernel from the data path entirely.
What is measured here is the best an unprivileged process can do without one, and
the report says so.

## Bare metal is not available

E7's `host` tier is a hypervisor guest. What is reported is the incremental cost
of each isolation layer *above* the virtual machine, not the cost of
virtualising a data plane. A reader who took these figures as the latter would
understate it by however much the hypervisor already costs, which this setup
cannot see.

## The GPU measurement is a bound, not a system

`lr_gpu_aead` computes AES-256 in counter mode and does not authenticate. A
parallel GHASH over a batch is a prefix problem whose GPU implementation is a
project of its own, and a poor one would understate the offload. Omitting it
makes the numbers optimistic, which strengthens the conclusion that the bus
dominates rather than weakening it — but it is not a working offload.

Nor does the measurement carry the latency cost of batching. A real offload must
hold packets until a batch fills, and that delay is not in these figures.

## The cipher comparison is not a fair fight, and says so

The implementations written here interleave four AES blocks using the 128-bit
instructions. OpenSSL's assembly interleaves eight using the vector AES
extensions. The gap is reported as a measurement of that difference, not as a
claim about implementation quality in general, and both implementations are
checked byte for byte against each other before any timing runs.

## The masked arm is a proxy

Clearing the capability bits makes the same code take its software path. That is
what the code does on a machine without the instructions, so it is a reasonable
model of an embedded or older target. It is not the same as measuring such a
machine: the cache hierarchy, the memory system and the branch predictors are
still this machine's.

## Latency is a round trip, across hosts

Two hosts have unsynchronised counters and no attempt is made to synchronise
them. What the wire topology measures is a round trip on the generator's own
clock, against a no-crypto echo baseline measured the same way. A one-way sojourn
is not observable this way and is not claimed. The loopback fallback does measure
one way, on one clock — and the two are not comparable, which is why the metric
is recorded with every point.

## E9 depends on E5 having several conditions

The admission model splits by network condition to avoid leaking neighbouring
offered rates between train and test. With one condition it falls back to a
contiguous positional split, records `single_condition: true`, and its score
should be read as optimistic. With an uninterpretable E5 — a single-core loopback
run — it refuses to fit at all.

Only `./run.sh --full` produces the conditions, and only as root and with `tc`
present. It measures E5 once per condition into `results/raw/netem-LABEL/` and
folds them into one fragment with every point labelled. That folding used to
happen inside the conditions loop, before the core experiments ran — and the
core loop then wrote its own single-condition E5 over the top, so a full sweep
that had measured five conditions arrived at the analysis with one and E9
declined to fit for want of points that had just been discarded. The core loop
now skips E5 where the conditions loop has run, and the folding happens after
it, so nothing written later can land on top.

## What is not measured

- IPv6, fragmentation, and any path MTU below 1500.
- Multiple simultaneous security associations, and the key lookup that implies.
- Handshake throughput. The handshake is implemented and tested; only the data
  path is timed.
- Congestion response. This is a UDP data plane with no rate control of its own.
- Anything above one shard per core, or NUMA effects: the instances used have one
  socket.
- Long-run behaviour. The longest replicate is a few seconds; thermal effects and
  memory fragmentation over hours are not visible.
