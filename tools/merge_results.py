#!/usr/bin/env python3
"""Merge the per-experiment fragments in results/raw into one results.json.

Each experiment writes its own file, so a run that dies partway leaves the
fragments that did complete. This assembles them, and refuses to assemble
fragments that did not come from the same machine and the same build: silently
merging a sweep from one host with a sweep from another produces a dataset in
which every cross-experiment comparison is wrong, and nothing downstream would
notice.

The environment block is taken from the fragments themselves rather than
re-probed here, because the machine that analyses is often not the machine that
measured.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

# Fields that must agree across fragments for a merge to be legitimate. Anything
# that would change the meaning of a cycle count belongs here.
#
# tsc_hz is calibrated afresh in every process and its last few digits differ
# between runs on the same machine, so it is compared rounded to the nearest
# megahertz. Comparing it exactly would flag every legitimate multi-fragment
# sweep as coming from different machines, which is worse than not checking it
# at all: an alarm that always fires stops being read.
IDENTITY_FIELDS = [
    "arch",
    "cpu_model",
    "git_commit",
    "compiler",
    "cxx_flags",
    "tsc_mhz",
    "openssl_version",
    "host_class",
]

# Every fragment the full pipeline expects, and the top-level key each carries.
# check_wiring.py asserts that this list matches what the C++ and the scripts
# actually produce, so the two cannot drift apart.
EXPECTED = {
    "e1_cost_model.json": "e1",
    "e2_scaling.json": "e2",
    "e2_mpi_scaling.json": "e2_mpi",
    "e3_iopath.json": "e3",
    "e4_memroof.json": "e4",
    "e5_latency.json": "e5",
    "e6_baseline.json": "e6",
    "e6_network_baselines.json": "e6_network",
    "e7_virtualization.json": "e7",
    "e8_gpu.json": "e8",
    "e9_admission.json": "e9",
    "controls.json": "controls",
}


def load(path: pathlib.Path):
    try:
        with path.open() as fh:
            return json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        return {"__error__": f"{path.name}: {exc}"}


def identity(env: dict) -> tuple:
    values = []
    for field in IDENTITY_FIELDS:
        if field == "tsc_mhz":
            try:
                values.append(str(round(float(env.get("tsc_hz", 0)) / 1e6)))
            except (TypeError, ValueError):
                values.append("0")
        else:
            values.append(str(env.get(field, "")))
    return tuple(values)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--raw", default="results/raw", help="fragment directory")
    ap.add_argument("--out", default="results/results.json")
    ap.add_argument("--strict", action="store_true",
                    help="fail when a fragment is missing or unavailable")
    ap.add_argument("--allow-mixed", action="store_true",
                    help="merge fragments from different machines anyway "
                         "(records the mismatch in the output)")
    args = ap.parse_args()

    raw = pathlib.Path(args.raw)
    if not raw.is_dir():
        print(f"no fragment directory at {raw}", file=sys.stderr)
        return 2

    merged: dict = {"environment": None, "provenance": {}}
    identities: dict[tuple, list[str]] = {}
    problems: list[str] = []
    present: list[str] = []
    unavailable: dict[str, str] = {}

    for name, key in EXPECTED.items():
        path = raw / name
        if not path.exists():
            problems.append(f"missing fragment: {name}")
            continue
        doc = load(path)
        if "__error__" in doc:
            problems.append(doc["__error__"])
            continue

        env = doc.get("environment")
        if env and not env.get("arch"):
            # tools/emit_fragment.py writes a placeholder environment when
            # results/raw/environment.json was missing at the time. It carries no
            # identity, so comparing it with the real ones would make every
            # field differ and the merge would be refused as a mixture of two
            # machines — a confident and completely wrong diagnosis of a missing
            # file. It is recorded as the problem it is and left out of the
            # comparison.
            problems.append(f"{name}: no environment block was available when it "
                            f"was written ({env.get('note', 'no note recorded')})")
            env = None
        if env:
            identities.setdefault(identity(env), []).append(name)
            if merged["environment"] is None:
                merged["environment"] = env

        block = doc.get(key)
        if block is None:
            problems.append(f"{name}: no '{key}' block")
            continue
        merged[key] = block
        present.append(key)
        if isinstance(block, dict) and block.get("available") is False:
            unavailable[key] = block.get("reason", "no reason recorded")

    if len(identities) > 1:
        lines = []
        for ident, names in identities.items():
            lines.append("    " + ", ".join(names) + " -> " +
                         ", ".join(f"{f}={v}" for f, v in zip(IDENTITY_FIELDS, ident)))
        message = ("fragments came from more than one machine or build:\n" +
                   "\n".join(lines))
        if args.allow_mixed:
            problems.append(message)
            merged["provenance"]["mixed_sources"] = True
        else:
            print(message, file=sys.stderr)
            print("\nRefusing to merge. Re-run the sweep on one machine, or pass "
                  "--allow-mixed if the mixture is deliberate.", file=sys.stderr)
            return 3
    merged["provenance"]["fragments_present"] = sorted(present)
    merged["provenance"]["fragments_missing"] = sorted(
        k for f, k in EXPECTED.items() if k not in present)
    merged["provenance"]["unavailable"] = unavailable
    merged["provenance"]["problems"] = problems

    if merged["environment"] is None:
        print("no fragment carried an environment block", file=sys.stderr)
        return 4

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".json.tmp")
    with tmp.open("w") as fh:
        json.dump(merged, fh, indent=2, sort_keys=False)
        fh.write("\n")
    tmp.replace(out)

    print(f"merged {len(present)} fragment(s) into {out}")
    for key, reason in sorted(unavailable.items()):
        print(f"  unavailable: {key}: {reason}")
    for p in problems:
        print(f"  note: {p}")

    if args.strict and (problems or unavailable):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
