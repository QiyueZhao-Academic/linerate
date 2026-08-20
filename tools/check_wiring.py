#!/usr/bin/env python3
"""Check that the pieces of this project still agree with each other.

A project this size drifts in a specific way: a field is renamed in the C++, the
Python that reads it keeps working because the key is simply absent and reads as
zero, and the report quietly prints a plausible wrong number. Nothing crashes.
This script exists to make that class of drift loud.

It checks, without running the harness:

  every experiment the bench binary declares also has an entry point and a
  fragment filename, and the merger expects exactly that set of fragments;

  every fragment producer — C++, shell script or Python — writes a fragment the
  merger knows about, and every fragment the merger expects has a producer;

  every LaTeX macro the report uses is defined by python/lr/report.py, and every
  macro report.py defines is used somewhere (an unused macro is usually a
  renamed one);

  every figure the report includes is one python/lr/figures.py can produce;

  the protocol constants in the C++ headers match what the tests and the schema
  documentation say they are.

Exit status is non-zero when anything disagrees.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


class Checker:
    def __init__(self) -> None:
        self.problems: list[str] = []
        self.notes: list[str] = []

    def fail(self, msg: str) -> None:
        self.problems.append(msg)

    def note(self, msg: str) -> None:
        self.notes.append(msg)


def read(path: pathlib.Path) -> str:
    try:
        return path.read_text()
    except OSError:
        return ""


def check_experiments(c: Checker) -> None:
    bench_hpp = read(ROOT / "src/bench/bench.hpp")
    declared = set(re.findall(r"int\s+(run_[a-z0-9_]+)\s*\(", bench_hpp))
    main_cpp = read(ROOT / "src/bench/bench_main.cpp")
    dispatched = set(re.findall(r"return\s+(run_[a-z0-9_]+)\(", main_cpp))

    for fn in sorted(declared - dispatched):
        # run_e6 and run_e7 are dispatched but not in "all"; that is deliberate
        # and is not what this checks. What it checks is that nothing declared is
        # unreachable.
        c.fail(f"bench.hpp declares {fn} but bench_main.cpp never dispatches it")
    for fn in sorted(dispatched - declared):
        c.fail(f"bench_main.cpp dispatches {fn}, which bench.hpp does not declare")

    defined = set()
    for src in (ROOT / "src/bench").glob("*.cpp"):
        defined |= set(re.findall(r"^int\s+(run_[a-z0-9_]+)\s*\(",
                                  read(src), re.MULTILINE))
    for fn in sorted(declared - defined):
        c.fail(f"{fn} is declared but not defined in any src/bench/*.cpp")


def check_fragments(c: Checker) -> None:
    merger = read(ROOT / "tools/merge_results.py")
    m = re.search(r"EXPECTED\s*=\s*\{(.*?)\}", merger, re.S)
    if not m:
        c.fail("tools/merge_results.py has no EXPECTED table")
        return
    expected = dict(re.findall(r'"([^"]+\.json)":\s*"([^"]+)"', m.group(1)))

    # Who writes each fragment: the C++ experiments, the CUDA program, the
    # Python admission model, and the shell scripts that pipe into
    # tools/emit_fragment.py.
    produced: dict[str, str] = {}
    for src in list((ROOT / "src").rglob("*.cpp")) + list((ROOT / "src").rglob("*.cu")):
        text = read(src)
        for frag in re.findall(r'"/?((?:e\d+[a-z0-9_]*|controls)\.json)"', text):
            produced.setdefault(frag, str(src.relative_to(ROOT)))
        for frag in re.findall(r'outdir\s*\+\s*"/([a-z0-9_]+\.json)"', text):
            produced[frag] = str(src.relative_to(ROOT))
        for frag in re.findall(r'write_unavailable\(ctx,\s*"([a-z0-9_]+\.json)"', text):
            produced[frag] = str(src.relative_to(ROOT))
    # Only names that look like a fragment. Matching every `*.json` string would
    # pick up filenames from docstrings and example command lines, and a checker
    # that reports those as problems teaches the reader to ignore it.
    frag_re = re.compile(r'((?:e\d+[a-z0-9_]*|controls|environment)\.json)')
    for src in list((ROOT / "python").rglob("*.py")) + list((ROOT / "cloud").rglob("*.sh")):
        text = read(src)
        for frag in frag_re.findall(text):
            produced.setdefault(frag, str(src.relative_to(ROOT)))

    for frag in sorted(expected):
        if frag not in produced and frag != "environment.json":
            c.fail(f"merge_results.py expects {frag}, but nothing writes it")
    for frag, where in sorted(produced.items()):
        if frag in ("results.json", "environment.json"):
            continue
        if frag not in expected:
            c.fail(f"{where} writes {frag}, which merge_results.py does not expect")


def check_macros(c: Checker) -> None:
    report_py = read(ROOT / "python/lr/report.py")
    # Macro names as they are constructed: literal names passed to m.num / m.pct
    # / m.text / m.set, plus the per-cipher family built from a tag.
    literal = set(re.findall(r'm\.(?:num|pct|text|set)\(\s*"([A-Za-z]+)"', report_py))
    # Names built from an f-string, e.g. f"fitA{tag}{suffix}" or
    # f"virt{name}Cycles". Each becomes a regex in which the placeholders match
    # any run of letters, so a macro assembled at run time is still recognised.
    templated = []
    for pattern in re.findall(r'\(\s*f"([A-Za-z{}_]+)"', report_py):
        if "{" not in pattern:
            continue
        rx = re.sub(r"\{[a-z_]+\}", "[A-Za-z]+", pattern)
        templated.append(re.compile(r"^" + rx + r"$"))

    used: set[str] = set()
    for tex in list((ROOT / "report").glob("*.tex")) + \
               list((ROOT / "report/sections").glob("*.tex")):
        used |= set(re.findall(r"\\([a-z][A-Za-z]*)\b", read(tex)))

    # LaTeX and package control sequences that are not ours.
    builtin = {
        "documentclass", "input", "usepackage", "graphicspath", "settopmatter",
        "renewcommand", "newcommand", "setcopyright", "begin", "end", "title",
        "subtitle", "author", "affiliation", "institution", "country", "keywords",
        "maketitle", "balance", "bibliographystyle", "bibliography", "section",
        "paragraph", "subsection", "label", "ref", "cite", "caption", "centering",
        "footnotesize", "small", "texttt", "emph", "textbf", "textemdash",
        "includegraphics", "linewidth", "columnwidth", "item", "itemize", "quad",
        "qquad", "frac", "rightarrow", "text", "footnotetextcopyrightpermission",
        "lrfigure", "fbox", "parbox", "toprule", "midrule", "bottomrule",
        "multicolumn", "textbackslash", "textasciitilde", "textasciicircum",
        "protect", "hbox", "vspace", "hspace", "noindent", "leq", "geq", "times",
        "mathrm", "left", "right", "sum", "cdot", "rho", "alpha", "beta",
        "lambda", "mu", "sigma", "approx", "ldots", "dots", "footnote",
        "columnwidth", "textwidth", "linewidth", "url", "href", "verb",
    }
    ours_used = {u for u in used if u not in builtin}

    for name in sorted(ours_used):
        if name in literal:
            continue
        if any(rx.match(name) for rx in templated):
            continue
        c.fail(f"report uses \\{name}, which python/lr/report.py never defines")

    for name in sorted(literal):
        if name not in used:
            c.note(f"macro \\{name} is defined but never used in the report")


def check_figures(c: Checker) -> None:
    figures_py = read(ROOT / "python/lr/figures.py")
    produced = set(re.findall(r'_save\(fig,\s*outdir,\s*"([a-z_]+)"\)', figures_py))
    included: set[str] = set()
    for tex in (ROOT / "report/sections").glob("*.tex"):
        included |= set(re.findall(r"\\lrfigure\{([a-z_]+)\}", read(tex)))
        included |= set(re.findall(r"\\includegraphics\[[^\]]*\]\{([a-z_]+)\}",
                                   read(tex)))
    for name in sorted(included - produced):
        c.fail(f"report includes figure '{name}', which figures.py cannot draw")
    for name in sorted(produced - included):
        c.note(f"figures.py draws '{name}', which the report does not include")


def check_protocol_constants(c: Checker) -> None:
    worker = read(ROOT / "include/lr/worker.hpp")
    hdr = re.search(r"LR_HDR\s*=\s*sizeof\(WireHdr\)", worker)
    if not hdr:
        c.fail("include/lr/worker.hpp no longer derives LR_HDR from sizeof(WireHdr)")
    m = re.search(r"LR_MAX_PAYLOAD\s*=\s*(\d+)\s*-\s*LR_HDR\s*-\s*LR_TAG", worker)
    if not m:
        c.fail("LR_MAX_PAYLOAD is no longer derived from the MTU, the header and "
               "the tag; a literal there would drift from the wire format")
        return
    udp_mtu = int(m.group(1))
    if udp_mtu != 1472:
        c.fail(f"LR_MAX_PAYLOAD is derived from {udp_mtu}, not 1472 "
               "(1500 MTU minus 20 IPv4 minus 8 UDP)")

    # The self-test must assert the same numbers, or a change to the header could
    # pass the tests.
    selftest = read(ROOT / "src/apps/lr_selftest.cpp")
    if "LR_HDR == 24" not in selftest:
        c.fail("lr_selftest.cpp no longer asserts the header size")
    if "1472 - LR_HDR - LR_TAG" not in selftest:
        c.fail("lr_selftest.cpp no longer asserts the maximum payload derivation")

    # And the documentation must not carry a stale copy.
    # The documentation must not carry a stale copy of the constants. A line
    # that explicitly says it is describing the previous format is exempt: a
    # schema document should be able to say what changed, and forbidding that
    # would push the migration note out of the one place it belongs.
    # Exemption is by paragraph rather than by line, because prose wraps: the
    # sentence that says "version 1" and the sentence that gives version 1's
    # numbers are usually not on the same line.
    schema = read(ROOT / "docs/schema.md")
    line_of_para: list[int] = []
    paragraphs: list[str] = []
    current: list[str] = []
    start = 1
    for lineno, line in enumerate(schema.splitlines() + [""], 1):
        if line.strip():
            if not current:
                start = lineno
            current.append(line)
        elif current:
            paragraphs.append("\n".join(current))
            line_of_para.append(start)
            current = []
    for para, lineno in zip(paragraphs, line_of_para):
        low = para.lower()
        if "version 1" in low or "previous wire format" in low:
            continue
        for literal in ("28-byte header", "1428"):
            if literal in para:
                c.fail(f"docs/schema.md:{lineno} mentions '{literal}', which "
                       "belongs to the previous wire format; say so in the same "
                       "paragraph if the mention is deliberate")


def check_ciphers(c: Checker) -> None:
    """Every cipher the sweep declares must be constructible by the factory."""
    bench = read(ROOT / "src/bench/bench.cpp")
    m = re.search(r"kCiphers\s*=\s*\{(.*?)\};", bench, re.S)
    if not m:
        c.fail("src/bench/bench.cpp no longer declares kCiphers")
        return
    swept = set(re.findall(r'"([a-z0-9\-]+)"', m.group(1)))
    factory = read(ROOT / "src/core/aead_factory.cpp")
    for name in sorted(swept):
        if f'"{name}"' not in factory:
            c.fail(f"E1 sweeps cipher '{name}', which aead_factory.cpp does not build")
    tags = read(ROOT / "python/lr/report.py")
    for name in sorted(swept):
        if f'"{name}"' not in tags:
            c.fail(f"cipher '{name}' has no LaTeX macro tag in report.py, so its "
                   "macro name would contain digits and TeX would reject it")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", action="store_true", help="only print problems")
    args = ap.parse_args()

    c = Checker()
    check_experiments(c)
    check_fragments(c)
    check_macros(c)
    check_figures(c)
    check_protocol_constants(c)
    check_ciphers(c)

    if not args.quiet:
        for n in c.notes:
            print(f"note: {n}")
    for p in c.problems:
        print(f"PROBLEM: {p}", file=sys.stderr)

    if c.problems:
        print(f"\n{len(c.problems)} wiring problem(s).", file=sys.stderr)
        return 1
    print(f"wiring ok ({len(c.notes)} note(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
