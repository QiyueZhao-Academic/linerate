# Methodology

This document is the argument for why the numbers in the report should be
believed. Where a choice could have gone another way, the reason it went this
way is here, along with what would have gone wrong otherwise.

## The model

A forwarding element that terminates one security association and originates
another pays two kinds of cost per packet. One does not depend on the payload:
the system calls, the header parse, the anti-replay check, the nonce derivation,
the cipher's own per-message setup and teardown. The other is proportional to the
payload: the bulk encryption and decryption and the memory traffic feeding them.

    C(s) = a + b·s        s* = a / b

`s*` is the payload at which the two are equal. It is the quantity the study
reports, because it is a ratio of two cycle counts that both scale with the
clock and therefore survives being carried from one machine to another. Neither
`a` nor `b` does.

### Why the path decrypts and re-encrypts

Each payload byte crosses the cipher twice. That is a real topology — it is what
a tunnel endpoint bridging two security associations does — and it makes `b`
about twice what a single-pass endpoint would show, which separates it cleanly
from `a` in the fit. The report states the factor and publishes the per-pass
figures alongside, so a reader modelling a one-pass endpoint can halve `b` and
double `s*` rather than having to guess whether the doubling was already applied.

## Statistical discipline

**Replicates.** Eleven by default, an odd number so the median is an observed
value and not the average of two.

**Spread.** A percentile-method bootstrap interval on the median, not a standard
deviation. The distribution of a per-packet cost is right-skewed: scheduling
events add time and nothing subtracts it. A symmetric interval on such a
distribution asserts a lower bound that never occurs.

**The stability gate.** A point whose coefficient of variation exceeds the
threshold (0.10 by default) is marked unstable. It is recorded, not deleted.
Excluded points appear hollow in the figures. A point dropped without a trace is
a point nobody can audit, and the difference between "excluded because unstable"
and "never measured" is exactly the difference a reader needs.

**Interleaved order.** Every replicate of every unit goes into one list, which is
shuffled with a seed recorded in the dataset. A nested loop would confound the
factor with time: if the machine warms up, or a neighbouring tenant arrives
halfway through, the whole effect lands on whichever factor level happened to be
running then. Interleaving spreads unavoidable drift across all levels instead of
concentrating it on one.

**The anchor.** One fixed configuration is measured before and after every sweep.
If the two disagree by more than ten per cent, the machine changed underneath the
measurement and the sweep is marked void rather than reported. This catches the
failure interleaving cannot: a monotone change over the whole sweep, which
interleaving turns into noise on every level rather than removing.

**Warm-up.** Three preload-and-drain cycles are discarded before each timed
region. They populate the instruction cache, the branch predictors, the page
tables behind the arena and the socket's own allocations. Those costs are real
but they amortise over a run of any useful length, so charging them to the
per-packet term would misattribute them.

**What is inside the timed region.** Only `drain()`. Staging packets into the
receive queue happens between timed windows, so the loader never appears in a
measurement of the data plane.

## The hardware-crypto factor

The capability mask that clears AES-NI and the carry-less multiply is read by
libcrypto when it loads, and by the dispatcher of the implementations written
here for the same reason. It cannot be varied inside a running process. So each
unit of the sweep is a fresh child process with the mask set or cleared.

That constraint turns out to be a benefit. Because each replicate is its own
process, the sweep order is free to interleave across the mask factor as well as
across the others, which a single long-lived process could not do.

The child reports whether the mask actually took effect, and a unit whose child
disagrees with the request is discarded. Without that check, a mask that silently
failed would enter the dataset as a null result — the most expensive kind of
error, because it looks like a finding.

**Both implementations are masked.** Masking only OpenSSL would compare its
software path against our hardware path, which measures nothing at all. The
implementations written here read the same environment variable and honour it.

## Two implementations of each cipher

For AES-256-GCM and ChaCha20-Poly1305, the study measures OpenSSL's EVP and an
implementation written for this project:

- **AES-256-GCM**: AES-NI with a PCLMULQDQ GHASH using four-power aggregation
  and a single reduction, with counter-mode encryption and the hash stitched into
  one pass over the data; ARMv8 AES with PMULL on `aarch64`; a portable C
  fallback with a table-driven GHASH.
- **ChaCha20-Poly1305**: four-block AVX2, single-block NEON, portable C; Poly1305
  in 32-bit limbs.

The self-test checks both against OpenSSL byte for byte at thirty lengths in both
directions before any timing runs, plus the published test vectors, plus tamper
rejection. A performance study of a cipher that computes the wrong ciphertext
measures nothing, so this gate runs first and `run.sh` stops if it fails.

Having the second implementation is what separates "this cipher costs *x*" from
"this library's version of this cipher costs *x*". The gap between the two is
reported rather than hidden: OpenSSL's assembly interleaves eight blocks using
the vector AES extensions where this implementation interleaves four using the
128-bit instructions, and the residual difference is a property of that choice
rather than of correctness.

## Host classification

Three classes, enforced in code rather than by convention:

| class | what it means | what it may do |
|---|---|---|
| `development` | cannot time reliably: macOS, a non-invariant counter, no hard thread affinity | build, self-test, analyse, typeset |
| `constrained` | Linux, invariant counter, hard affinity, but no kernel isolation or fewer than two usable cores | measure, with its caveats attached to every dataset |
| `measurement` | constrained plus kernel isolation on the data-plane cores, homogeneous cores, at least two of them | measure |

A host that cannot hold a measurement still is not permitted to write one. The
alternative — allowing it and adding a footnote — puts the burden on every future
reader of the dataset to remember the footnote.

Separately, the *isolation tier* the process is running inside (`host`, `netns`,
`container`, `gvisor`) is recorded with every measurement, because E7 varies
exactly that and a number that does not say which sandbox produced it is not
comparable with one that does.

## Topology

**With a peer.** The load generator runs on the second node and sends across a
physical interface. The data plane binds that interface and forwards re-encrypted
packets back, so the transmit side is a real driver transmit. One TCP control
connection drives the generator for a whole sweep; per-unit `ssh` would spend
more time on `ssh` than on measurement and would put the operator's laptop inside
the measurement loop.

**Without a peer.** The generator is a thread in the same process, packets never
leave the host, and every affected point records `source: loopback`. This is a
lower bound that omits the driver, the wire and the far side's stack.

**Loss is expected in wire mode.** The generator deliberately overruns the
receiver so the receive queue never empties; otherwise the measurement would time
an idle poll rather than the service of a packet. What is measured is cost per
packet *served*, and the delivery ratio is recorded separately so the report can
state how hard the receiver was driven.

## Latency

**Open loop, always.** A closed-loop generator waits for the previous packet to
be served before sending the next, so it cannot offer more than the receiver's
capacity and the queueing the experiment exists to expose never forms. Reporting
a closed-loop curve as latency-under-load is a standard error and it always
produces a flatteringly flat tail.

**Round trip, one clock.** Across two hosts the counters are not synchronised and
no attempt is made to synchronise them. The generator stamps the packet, the data
plane preserves the stamp through the re-encryption (the header travels in the
clear as associated data, so reading it needs no decryption), and the generator
computes the difference on its own clock. What that yields is a round trip. The
report says so, and the no-crypto echo baseline measured the same way is what
makes the difference interpretable.

**When the curve says nothing.** An open-loop generator sharing a core with the
data plane is a competitor for the CPU, not a load source, and the queue that
forms is a scheduling artefact. That condition is detected and recorded, and the
analysis declines to read a knee from such a run.

## Controls

Every control carries a prediction made before the measurement and stored beside
the outcome. A control whose prediction is written afterwards cannot fail, and a
control that cannot fail is decoration.

| control | prediction | what it protects |
|---|---|---|
| null cipher | `a` falls; `b` falls to roughly two memory passes | makes `a` decomposable instead of one opaque number |
| masked hardware crypto | `b` rises by at least 8× | if it does not, the mask never reached the code and every masked measurement is void |
| idle poll | cost × frequency below 1% of the per-packet cost | confirms the harness is not timing its own loop |
| tamper cost | forged within 25% of valid | a cheap rejection path would be a side channel and would bias any run with authentication failures |
| cipher levels | all four constructible | a missing level would silently shrink the design |

The idle-poll control was originally written to compare the cost of one empty
poll against the cost of one packet, and it failed. The right quantity is the
amortised one — cost per poll multiplied by polls per packet — because a poll
costing a tenth of a packet but occurring once per thousand packets contaminates
nothing. The control was rewritten rather than its threshold loosened; that
distinction is the whole value of having controls.

## What is generated and what is written

No measured number appears literally in the report's prose. Every quantity is a
macro generated from `results.json`, so a sentence and a table cannot disagree
and a re-run updates the text along with the figures.
`tools/check_numbers.py` fails the build when a bare numeral appears where a
macro belongs, and separately re-derives a sample of macros from the dataset —
including checking that every `s*` equals its own `a/b`.

`tools/check_wiring.py` checks the other direction: that every fragment the
merger expects has a producer, every macro the report uses is defined, every
figure it includes can be drawn, and the protocol constants in the headers still
match what the tests and the documentation say.

## Completeness

Every experiment writes a fragment whether or not it could run. One that could
not records `available: false` and a reason. The dataset is therefore always
complete, the report renders "not available, because X" instead of a missing
figure, and a reader can tell the difference between an experiment that was
skipped and one that was never attempted.
