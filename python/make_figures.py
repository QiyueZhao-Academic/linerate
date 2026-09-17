#!/usr/bin/env python3
"""Draw the figures from the dataset.

Order matters and is enforced here rather than left to a Makefile: E9 reads the
merged dataset and writes its own fragment, and the admission figure draws from
that fragment, so E9 runs first. Nothing in this script invents a number; if a
quantity is absent from results.json the figure that needs it is skipped and
the reason is printed.

    python/make_figures.py                       E9, then every figure
    python/make_figures.py --no-ml               the figures, without fitting E9
    python/make_figures.py --results other.json  a dataset from elsewhere
"""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from lr import admission, figures                  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", default=str(ROOT / "results" / "results.json"))
    ap.add_argument("--figures-dir", default=str(ROOT / "results" / "figures"))
    ap.add_argument("--no-ml", action="store_true",
                    help="skip the admission model even if the data supports it")
    args = ap.parse_args()

    results = pathlib.Path(args.results)
    if not results.exists():
        print(f"no dataset at {results}.\n"
              f"Run the sweep, then tools/merge_results.py.", file=sys.stderr)
        return 2
    data = json.loads(results.read_text())

    # E9 reads the merged dataset and writes its own fragment, so it runs before
    # the figures that would draw it. Re-merging afterwards folds it in.
    if not args.no_ml:
        body = admission.run(data, ROOT / "results" / "raw" / "e9_admission.json")
        if body.get("available"):
            print(f"E9: {body['best_model']}, test R^2 {body['test_r2']:.3f} "
                  f"against {body['baseline_r2']:.3f} for the static baseline")
            merge = ROOT / "tools" / "merge_results.py"
            if merge.exists():
                subprocess.run([sys.executable, str(merge),
                                "--raw", str(ROOT / "results" / "raw"),
                                "--out", str(results)],
                               check=False, capture_output=True)
                data = json.loads(results.read_text())
        else:
            print(f"E9 not available: {body.get('reason')}")

    figdir = pathlib.Path(args.figures_dir)
    facts = figures.build(data, figdir)
    drawn = [k for k in facts if not k.startswith("__")]
    print(f"figures: {len(drawn)} drawn into {figdir}")
    for note in facts.get("__skipped__", []):
        print(f"  skipped {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
