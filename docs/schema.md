# Dataset schema

`results/results.json` is the contract between the C++ harness and everything
that reads it. This document describes it; `tools/check_wiring.py` enforces the
parts of it that can be checked without running anything.

Schema version 2. Version 1 described the previous wire format, which carried an
explicit nonce and therefore a 28-byte header and a 1428-byte maximum payload.
Deriving the nonce from the epoch and sequence number removed four bytes from the
header and gave four back to the payload. A version-1 dataset and a version-2
dataset are not comparable and the merger will not combine them.

## Top level

```
{
  "environment": { ... },       the machine, the build, the run parameters
  "provenance":  { ... },       which fragments were present, which were not
  "e1" … "e9":   { ... },       one block per experiment
  "controls":    { ... }
}
```

Every experiment block has `available`. When it is `false`, the block carries
`reason` and nothing else. Consumers must check `available` before reading
anything else, and the figure that block would have fed is skipped with that
reason.

## `environment`

Written by `write_environment()` in `src/bench/emit_json.cpp` and attached to
*every* fragment, not only to the merged file. That is what lets the merger
detect fragments produced on different machines, which is otherwise invisible.

| field | meaning |
|---|---|
| `schema_version` | 2 |
| `timestamp_utc` | when the fragment was written |
| `linerate_version`, `git_commit` | which source produced it; `-dirty` when the tree had uncommitted changes |
| `compiler`, `cxx_flags`, `build_type` | how it was built |
| `os`, `arch`, `cpu_model` | the machine |
| `logical_cpus`, `physical_cores`, `smt_enabled`, `homogeneous_cores` | its topology |
| `invariant_tsc`, `tsc_hz`, `nominal_cpu_hz`, `clock_source` | the counter the cycle figures come from |
| `hard_affinity` | whether pinning is enforced rather than advisory |
| `isolcpus`, `nohz_full`, `rcu_nocbs`, `kernel_cmdline` | kernel isolation |
| `governor`, `turbo_control`, `irqbalance` | frequency and interrupt policy |
| `hw_aes`, `hw_clmul`, `hw_avx2` | the instructions the cipher paths dispatch on |
| `batched_io`, `uring_io`, `uring_sqpoll`, `openmp` | which transports and pools exist here |
| `openssl_version`, `openssl_hw_aes_active`, `hw_crypto_masked` | the cipher library and whether the mask was in force |
| `host_class` | `development` / `constrained` / `measurement` |
| `isolation_tier` | `host` / `netns` / `container` / `gvisor` |
| `nic_name`, `nic_driver`, `nic_addr` | the interface; `gve` is gVNIC |
| `caveats` | why this host did not reach `measurement` |
| `transports`, `ciphers` | the levels actually available |
| `protocol` | `header_bytes` 24, `tag_bytes` 16, `max_payload_bytes` 1432, `mtu_bytes` 1500, `ip_udp_overhead_bytes` 28 |
| `run` | replicates, cv threshold, seed, target packets, warm-up cycles, anchor tolerance, peer host, local address, offer factor, unit seconds |

`tsc_hz` is calibrated per process and its last digits differ between runs on the
same machine. The merger compares it rounded to the nearest megahertz; comparing
it exactly would flag every legitimate multi-fragment sweep as coming from
different machines, and an alarm that always fires stops being read.

## `provenance`

| field | meaning |
|---|---|
| `fragments_present`, `fragments_missing` | which experiment blocks made it into the merge |
| `unavailable` | key → reason, for every block that reported `available: false` |
| `mixed_sources` | present only when `--allow-mixed` was used |
| `problems` | anything the merger noticed but did not refuse over |

## `e1` — cost model

```
available, quick, anchor_cycles_before, anchor_cycles_after, anchor_drift,
anchor_ok, failed_units, interleaved, source, ciphers_swept, payloads_swept,
points: [ ... ]
```

Each point:

| field | meaning |
|---|---|
| `payload_bytes` | plaintext bytes carried |
| `cipher`, `cipher_family` | the level; family is `openssl` or `linerate` |
| `hw_crypto` | false means the capability mask was in force |
| `aead_backend` | which code path ran, e.g. `aesni+pclmulqdq`, `avx2`, `portable-c`, `openssl-evp` |
| `n` | replicates that succeeded |
| `cycles_median`, `cycles_ci_lo`, `cycles_ci_hi`, `cycles_cv` | the summary |
| `stable` | false when `cycles_cv` exceeded the threshold; the point is excluded from fits and drawn hollow |
| `pps_median` | implied single-shard capacity |
| `auth_fail` | must be zero; anything else means the measurement was of a broken path |
| `cycles_replicates` | every replicate, so a reader can re-derive the summary |
| `error` | empty on success |

**`anchor_ok: false` means the sweep is void.** E1 prints `SWEEP VOID`, and
nothing drawn from such a dataset is a result.

## `e2` — scaling, and `e2_mpi`

`e2`: `pool` (`openmp` or `std::thread`), `pinned`, `cipher`, `payload_bytes`,
`max_shards`, `replicates`, `serial_fraction`, and points with `shards`,
`strong_wall_ns`, `weak_wall_ns`, `strong_speedup`, `strong_efficiency`,
`weak_efficiency`, `cycles_per_packet_strong`, `cycles_per_packet_weak`.

`e2_mpi`: `ranks`, `distinct_hosts`, `cross_node`, `hosts`, the same wall and
cycle figures, and `coordination_ns_per_iteration` — one barrier and one
allreduce with no work between them, so any scaling shortfall can be attributed
to coordination or not. `distinct_hosts` is measured rather than assumed: a
two-node result produced with both ranks on one node is a common and otherwise
invisible error.

## `e3` — I/O path

`backends`, `uring_available` and `uring_reason`, `uring_sqpoll_available` and
`uring_sqpoll_reason`, `posix_b1_cycles`, `sqpoll_b1_cycles`,
`posix_minus_sqpoll_b1_cycles`, `sqpoll_contends_for_cpu`,
`sqpoll_interpretation`, `best_batched_backend`, `best_batched_cycles`.

Points carry `backend`, `batch`, `payload_bytes`, the cycle summary,
`syscalls_per_packet` and `pps_median`.

`syscalls_per_packet` is counted inside each backend, not derived from the
design, because a batch that returns short does not issue the number of calls the
design intends.

**`sqpoll_contends_for_cpu: true` means `posix_minus_sqpoll_b1_cycles` bounds
nothing.** With fewer than two usable cores the submission poller competes with
the data plane and the "saving" can be negative. That is a property of the host,
not of the mechanism, and consumers must read `sqpoll_interpretation` before
quoting the difference.

## `e4` — memory roof

`stream_bandwidth_gbps`, `largest_working_set_bytes`, `payload_passes_total` (8),
`payload_passes_application` (6), `passes_note`, `roof_payload_gbps`,
`roof_payload_gbps_application_only`, and points with `per_array_bytes`,
`resident_bytes`, `gbps`.

The eight passes are enumerated rather than assumed: receive copy (read+write),
`aead.open` (read+write), `aead.seal` (read+write), send copy (read+write).

## `e5` — latency

`cipher`, `payload_bytes`, `seconds_per_point`, `capacity_pps`, `knee_pps`,
`knee_fraction`, `baseline_p99_ns`, `topology`, `topology_reason`,
`netem_label`, `netem_conditions`, `generator_shares_cpu`, `interpretable`,
`interpretation_note`, `latency_metric`.

Points carry `offered_pps`, `offered_fraction`, `achieved_pps`, `p50_ns`,
`p99_ns`, `p999_ns`, `loss_ratio`, `packets`, `source`, `netem_label`.

**`interpretable: false` means no knee may be read from this run.**
`python/lr/admission.py` refuses to fit on such a dataset.

`latency_metric` says whether the figures are a round trip measured on the
generator's clock (wire) or a one-way sojourn on the local clock (loopback).
The two are not comparable and nothing should average them.

## `e6` — baselines, and `e6_network`

`e6`: `openssl_cli_available`, `passes_per_payload_byte` (2), `passes_note`, an
`openssl_speed` array with `cipher`, `cycles_per_byte` and a `blocks` table in
thousands of bytes per second, and a `linerate` array with `fixed_cycles`,
`cycles_per_byte`, `cycles_per_byte_per_pass`, `crossover_bytes` and the raw
points.

`e6_network` (written by `cloud/baselines.sh`): `iperf3_gbps`, `wireguard_gbps`,
`linerate_gbps`, a note for each explaining what ran or why it did not, and
`comparability_note` recording the structural differences between the three.

Use `cycles_per_byte_per_pass` when comparing against `openssl speed`, which
crosses each byte once.

## `e7` — virtualisation

`tiers_measured`, `chain_points`, `host_cycles`, `chain_cycles_per_hop`,
`bare_metal_available` (always false on a cloud VM) and `bare_metal_note`.

`tiers`: `tier`, `nic_driver`, the cycle summary, `syscalls_per_packet`,
`relative_to_host`.
`chain`: `chain_length` and the cycle summary.

**`relative_to_host` is the only figure that transfers.** The absolute cycle
count is a property of the machine, and the host tier is itself a hypervisor
guest, so these are incremental costs of isolation layers above the VM and not
the cost of virtualisation.

## `e8` — GPU

`device`, `compute_capability`, `sm_count`, `memory_clock_khz`,
`memory_bus_width_bits`, `payload_bytes`, `cipher` (`aes-256-ctr`),
`authentication` (false) and `authentication_note`, `launch_fixed_ns`,
`marginal_ns_per_packet`, `worthwhile_batch_packets`.

Points: `packets`, `h2d_ns`, `kernel_ns`, `d2h_ns`, `total_ns`,
`ns_per_packet`, `gbps_effective`, `transfer_share`.

Confidentiality only. Omitting authentication makes these an *optimistic* bound,
which strengthens the conclusion that the bus dominates rather than weakening it.

## `e9` — admission control

`n_samples`, `n_train`, `n_test`, `conditions`, `single_condition`, `features`,
`target`, `capacity_pps`, `latency_budget_ns`, `test_r2`, `ridge_r2`, `gbr_r2`,
`baseline_r2`, `best_model`, `admission_accuracy_learned`,
`admission_accuracy_static`, `admission_gain`, `over_admission_learned`,
`over_admission_static`, `ridge_coefficients`, `feature_importance`,
`observed_p99_ns`, `predicted_p99_ns`, `baseline_note`.

`baseline_r2` is the number that matters. A learned model's `test_r2` alone only
demonstrates that latency depends on load.

## `controls`

`ran`, `passed`, `aead_fixed_cycles`, `aead_cycles_per_byte`,
`null_fixed_cycles`, `null_cycles_per_byte`, `aead_only_fixed_cycles`, and a
`checks` array in which each entry carries `name`, `prediction`, `outcome`,
`observed`, `reference`, `ratio`, `ran`, `passed`, `error`.

`prediction` is stored because a prediction recorded after the fact cannot fail.

## Stability of this contract

Renaming a field silently is the failure mode this document exists to prevent:
the Python reads a missing key as zero, nothing crashes, and a figure draws a
plausible wrong number. `tools/check_wiring.py` catches the cases it can see
statically — a fragment nobody writes, an experiment nobody dispatches, a cipher
the factory cannot build, a protocol constant that drifted from the header.
