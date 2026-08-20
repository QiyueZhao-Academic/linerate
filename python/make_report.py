#!/usr/bin/env python3
"""Build the report from the dataset.

Order matters and is enforced here rather than left to a Makefile: the macros
depend on facts the figures compute while drawing, so figures run first. Nothing
in this script invents a number; if a quantity is absent from results.json the
macro renders as an em dash and the section says why.

    python/make_report.py                       figures, macros, tables, PDF
    python/make_report.py --no-pdf              everything but latexmk
    python/make_report.py --results other.json  a dataset from elsewhere
"""
from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from lr import admission, figures, report          # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", default=str(ROOT / "results" / "results.json"))
    ap.add_argument("--report-dir", default=str(ROOT / "report"))
    ap.add_argument("--figures-dir", default=str(ROOT / "results" / "figures"))
    ap.add_argument("--no-pdf", action="store_true")
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

    repdir = pathlib.Path(args.report_dir)
    gendir = repdir / "generated"
    gendir.mkdir(parents=True, exist_ok=True)
    for name, text in report.build(data, facts).items():
        (gendir / name).write_text(text)
    print(f"generated: {len(list(gendir.glob('*.tex')))} LaTeX files in {gendir}")

    # The figures live under results/, but LaTeX resolves \includegraphics
    # relative to the document. Copying rather than symlinking keeps the report
    # directory self-contained, which matters when it is the thing submitted.
    repfigs = repdir / "figures"
    repfigs.mkdir(exist_ok=True)
    for pdf in figdir.glob("*.pdf"):
        shutil.copy2(pdf, repfigs / pdf.name)

    if args.no_pdf:
        return 0
    if not shutil.which("latexmk"):
        print("latexmk not found; wrote the generated files but not the PDF.")
        return 0

    def latexmk(*args: str) -> subprocess.CompletedProcess:
        return subprocess.run(["latexmk", *args], cwd=repdir,
                              capture_output=True, text=True)

    build = ["-pdf", "-interaction=nonstopmode", "-halt-on-error", "main.tex"]
    proc = latexmk(*build)
    if proc.returncode != 0:
        # A latexmk run that failed leaves .fdb_latexmk, .aux and .fls describing
        # a document that was never built, and it believes them next time. So a
        # first run that failed because a class was missing goes on failing after
        # the class is installed, which reads as a second, different problem and
        # sends the operator looking for one. The state is discarded and the
        # build tried once more before anything is reported.
        print("latexmk failed; discarding its state and trying once more",
              file=sys.stderr)
        latexmk("-C")
        proc = latexmk(*build)
    if proc.returncode != 0:
        # LaTeX's own error lines are the useful part of several hundred lines
        # of output, so they are what gets printed.
        for line in proc.stdout.splitlines():
            if line.startswith("!") or "Error" in line or "Undefined" in line:
                print(line, file=sys.stderr)
        print("latexmk failed; see report/main.log", file=sys.stderr)
        return 1

    pdf = repdir / "main.pdf"
    if pdf.exists():
        dest = ROOT / "results" / "linerate-report.pdf"
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(pdf, dest)
        print(f"report: {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
