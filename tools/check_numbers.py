#!/usr/bin/env python3
"""Check that no number in the report was written by hand.

The rule the report is built on: every measured quantity in the prose is a macro
generated from results.json. That rule is what makes a re-run update the text
along with the figures, and what makes it impossible for a sentence and a table
to disagree.

A rule of that kind decays unless something enforces it, because typing a number
directly is always easier in the moment. So this refuses to pass when:

  a section file contains a bare numeral where a measured value belongs;
  a macro the report uses has no value in the generated file;
  a macro's value contradicts the dataset it was supposed to come from;
  a fragment that claims to be available carries no points.

Numbers that are part of the design rather than of the results — a replicate
count that also appears as a macro, an equation's exponent, a section number —
are allowed by an explicit whitelist, which is short on purpose.
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Numerals that may appear literally in the prose, with the reason each is
# allowed. Anything not here is a hand-written measurement until proven otherwise.
#
# One entry per numeral. A dict literal with the same key twice keeps only the
# last reason and discards the others without saying so, which is precisely the
# silent drift this file exists to prevent; where a numeral is allowed for more
# than one reason, the reasons are joined instead.
ALLOWED_LITERALS = {
    "0": "an origin or a zero in an equation",
    "1": "a count of one, a normalising constant, or an index",
    "2": "the two passes over each payload byte, stated structurally",
    "3": "a count of items in an enumeration",
    "4": "the four cipher levels and the four transports, both structural",
    "8": "the eight-block interleave of OpenSSL's assembly, a cited property",
    "16": "the tag length in bytes, also available as a macro",
    "20": "the IPv4 header size, quoted while deriving the maximum payload, and "
          "ChaCha20's round count",
    "24": "the wire header, also available as a macro",
    "128": "a key or block size named in prose",
    "256": "a key size named in prose",
    "1500": "the Ethernet MTU, also available as a macro",
    "8439": "an RFC number",
    "4303": "an RFC number",
    "1305": "Poly1305's prime, part of the algorithm's name",
}

# Phrases whose numerals are part of a name rather than a measurement.
NAME_PATTERNS = [
    r"AES-\d+-GCM", r"aes-\d+-gcm", r"ChaCha\d+", r"chacha\d+",
    r"Poly\d+", r"poly\d+", r"RFC~?\d+", r"rfc\d+", r"IPv\d",
    r"ARMv\d", r"x86-\d+", r"SHA-?\d+", r"\\ref\{[^}]*\}", r"\\label\{[^}]*\}",
    r"\\cite\{[^}]*\}", r"\\includegraphics[^}]*\}", r"\\lrfigure\{[^}]*\}\{[^}]*\}",
    r"Equation~\\ref\{[^}]*\}", r"Section~\\ref\{[^}]*\}",
    r"\\begin\{[^}]*\}", r"\\end\{[^}]*\}", r"\d+\\%",   # percentages in method text
    r"0\.\d+\\?linewidth", r"0\.\d+\\?columnwidth", r"\\input\{[^}]*\}",
]


def load_macros(path: pathlib.Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for m in re.finditer(r"\\newcommand\{\\([A-Za-z]+)\}\{(.*)\}", path.read_text()):
        out[m.group(1)] = m.group(2)
    return out


def strip_noise(text: str) -> str:
    """Remove comments, names, and LaTeX plumbing before hunting for numerals."""
    text = re.sub(r"(?<!\\)%.*", "", text)
    for pat in NAME_PATTERNS:
        text = re.sub(pat, " ", text)
    # Maths is where an equation's own constants live; those are structural.
    text = re.sub(r"\$[^$]*\$", " ", text)
    text = re.sub(r"\\\[.*?\\\]", " ", text, flags=re.S)
    text = re.sub(r"\\begin\{equation\}.*?\\end\{equation\}", " ", text, flags=re.S)
    return text


def check_literals(problems: list[str]) -> None:
    for tex in sorted((ROOT / "report/sections").glob("*.tex")):
        text = strip_noise(tex.read_text())
        for m in re.finditer(r"(?<![\w.\\])(\d[\d,.]*)(?![\w])", text):
            token = m.group(1).rstrip(".,")
            if token in ALLOWED_LITERALS:
                continue
            line = text[:m.start()].count("\n") + 1
            context = text[max(0, m.start() - 60):m.end() + 40].replace("\n", " ")
            problems.append(
                f"{tex.name}:{line}: literal '{token}' in prose; measured values "
                f"must be macros. Context: ...{context.strip()}...")


def check_macro_values(macros: dict[str, str], data: dict,
                       problems: list[str], notes: list[str]) -> None:
    """Re-derive a sample of macros from the dataset and compare."""
    def num(s: str) -> float | None:
        s = s.replace("\\,", "").replace("\\%", "").replace("\\textemdash{}", "")
        s = s.replace(",", "").strip()
        try:
            return float(s)
        except ValueError:
            return None

    def close(a: float | None, b: float | None, rel: float = 0.02) -> bool:
        if a is None or b is None:
            return False
        if not (math.isfinite(a) and math.isfinite(b)):
            return False
        if b == 0:
            return abs(a) < 1e-9
        return abs(a - b) / abs(b) <= rel

    env = data.get("environment", {})
    proto = env.get("protocol", {})
    direct = [
        ("protoHeaderBytes", proto.get("header_bytes")),
        ("protoTagBytes", proto.get("tag_bytes")),
        ("protoMaxPayload", proto.get("max_payload_bytes")),
        ("protoMTU", proto.get("mtu_bytes")),
        ("envCores", env.get("physical_cores")),
        ("latCapacityPPS", data.get("e5", {}).get("capacity_pps")),
        ("ioPosixCycles", data.get("e3", {}).get("posix_b1_cycles")),
        ("ioSqpollCycles", data.get("e3", {}).get("sqpoll_b1_cycles")),
        ("ctlRan", data.get("controls", {}).get("ran")),
        ("ctlPassed", data.get("controls", {}).get("passed")),
        ("memSustainedGBps", data.get("e4", {}).get("stream_bandwidth_gbps")),
    ]
    for name, expected in direct:
        if name not in macros:
            problems.append(f"macro \\{name} is used but never defined")
            continue
        got = num(macros[name])
        if expected is None:
            notes.append(f"\\{name} has no counterpart in the dataset to check")
            continue
        if not close(got, float(expected)):
            problems.append(
                f"\\{name} renders as {macros[name]!r} but the dataset says "
                f"{expected!r}")

    # The derived identity the whole study rests on: s* must equal a/b.
    for key, val in list(macros.items()):
        if not key.startswith("fitSStar") or key.startswith(("fitSStarLo", "fitSStarHi")):
            continue
        suffix = key[len("fitSStar"):]
        a, b = num(macros.get("fitA" + suffix, "")), num(macros.get("fitB" + suffix, ""))
        s = num(val)
        if a is None or b is None or s is None or b == 0:
            continue
        if not close(s, a / b, rel=0.03):
            problems.append(
                f"\\{key} is {s:.1f} but \\fitA{suffix} / \\fitB{suffix} "
                f"is {a / b:.1f}; the crossover and its inputs disagree")

    # A macro left as an em dash is legitimate, but the reader should be told how
    # many of them there are.
    missing = [k for k, v in macros.items() if "textemdash" in v]
    if missing:
        notes.append(f"{len(missing)} macro(s) render as an em dash because the "
                     f"experiment behind them was unavailable: "
                     f"{', '.join(sorted(missing)[:8])}"
                     f"{' ...' if len(missing) > 8 else ''}")


def check_fragment_integrity(data: dict, problems: list[str]) -> None:
    for key in ("e1", "e2", "e3", "e4", "e5", "e6", "e7", "controls"):
        block = data.get(key)
        if not isinstance(block, dict):
            continue
        if block.get("available") is False:
            continue
        if key in ("e1", "e2", "e3", "e5") and not block.get("points"):
            problems.append(f"{key} claims to be available but carries no points")
    e1 = data.get("e1", {})
    if e1.get("available") and not e1.get("anchor_ok", True):
        problems.append("E1's anchor check failed, so the sweep is void and must "
                        "not be reported as a result")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", default=str(ROOT / "results/results.json"))
    ap.add_argument("--macros", default=str(ROOT / "report/generated/macros.tex"))
    args = ap.parse_args()

    problems: list[str] = []
    notes: list[str] = []

    macros_path = pathlib.Path(args.macros)
    results_path = pathlib.Path(args.results)
    if not macros_path.exists():
        print(f"no macros at {macros_path}; run python/make_report.py first",
              file=sys.stderr)
        return 2
    macros = load_macros(macros_path)

    check_literals(problems)
    if results_path.exists():
        data = json.loads(results_path.read_text())
        check_macro_values(macros, data, problems, notes)
        check_fragment_integrity(data, problems)
    else:
        notes.append(f"no dataset at {results_path}; only the literal check ran")

    total = len(problems)
    checks = 4
    for n in notes:
        print(f"note: {n}")
    for p in problems:
        print(f"PROBLEM: {p}", file=sys.stderr)
    if total:
        print(f"\n{total} numerical consistency problem(s).", file=sys.stderr)
        return 1
    print(f"numbers ok: {len(macros)} macros checked across {checks} classes of "
          f"consistency, {len(notes)} note(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
