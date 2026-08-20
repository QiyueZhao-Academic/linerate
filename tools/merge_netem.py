#!/usr/bin/env python3
"""Fold the per-condition E5 runs into one fragment.

cloud/remote-sweep.sh runs E5 once per network condition, into its own
directory, because each run needs the condition attached before it starts. This
combines them into the single e5_latency.json the report expects, with every
point carrying the label of the condition it was measured under.

The combined fragment keeps the clean run's capacity and knee, since those are
properties of the machine rather than of the injected condition, and records
which conditions contributed.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--raw", default="results/raw")
    args = ap.parse_args()
    raw = pathlib.Path(args.raw)

    parts = sorted(raw.glob("netem-*/e5_latency.json"))
    if not parts:
        print("no per-condition E5 runs to fold in")
        return 0

    combined = None
    points: list[dict] = []
    conditions: list[str] = []
    for p in parts:
        try:
            doc = json.loads(p.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            print(f"  skipping {p}: {exc}")
            continue
        e5 = doc.get("e5", {})
        if not e5.get("available"):
            continue
        label = e5.get("netem_label", "none")
        conditions.append(label)
        for pt in e5.get("points", []):
            pt = dict(pt)
            pt["netem_label"] = label
            points.append(pt)
        # The clean run is the reference for capacity and the knee; where there
        # is no clean run the first available one stands in and the fragment
        # says which it was.
        if combined is None or label == "none":
            combined = doc

    if combined is None:
        print("no usable per-condition run")
        return 0

    combined["e5"]["points"] = points
    combined["e5"]["netem_conditions"] = conditions
    combined["e5"]["netem_label"] = "multiple" if len(set(conditions)) > 1 else \
        (conditions[0] if conditions else "none")
    combined["e5"]["reference_condition"] = \
        "none" if "none" in conditions else (conditions[0] if conditions else "unknown")

    out = raw / "e5_latency.json"
    out.write_text(json.dumps(combined, indent=2) + "\n")
    print(f"folded {len(parts)} condition(s), {len(points)} points, into {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
