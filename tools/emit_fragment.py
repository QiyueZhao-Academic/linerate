#!/usr/bin/env python3
"""Write a results fragment from a shell script.

The C++ experiments write their own fragments with the environment block
attached. The experiments that are shell scripts — the network baselines, the
virtualisation tiers, the service chain — need the same shape, and duplicating
the environment block in shell would guarantee the two drift apart.

So: run `lr_bench env` once at the start of a session to produce
results/raw/environment.json, then pipe a flat JSON body into this, and it
attaches that environment and writes the fragment.

    echo '{"available": true, "iperf3_mbps": 9412}' \\
        | tools/emit_fragment.py --key e6_network --out results/raw/e6_network_baselines.json

An unavailable result is written the same way and is not an error:

    tools/emit_fragment.py --key e8 --out results/raw/e8_gpu.json \\
        --unavailable "no CUDA toolkit on this host"
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key", required=True, help="top-level key, e.g. e6_network")
    ap.add_argument("--out", required=True, help="fragment path to write")
    ap.add_argument("--env", default="results/raw/environment.json",
                    help="file holding the environment block")
    ap.add_argument("--unavailable", metavar="REASON",
                    help="write an unavailable fragment with this reason")
    args = ap.parse_args()

    if args.unavailable:
        body = {"available": False, "reason": args.unavailable}
    else:
        text = sys.stdin.read().strip()
        if not text:
            print("nothing on stdin and no --unavailable given", file=sys.stderr)
            return 64
        try:
            body = json.loads(text)
        except json.JSONDecodeError as exc:
            print(f"body is not valid JSON: {exc}", file=sys.stderr)
            print(text[:400], file=sys.stderr)
            return 65
        body.setdefault("available", True)

    env = None
    env_path = pathlib.Path(args.env)
    if env_path.exists():
        try:
            env = json.loads(env_path.read_text()).get("environment")
        except (OSError, json.JSONDecodeError):
            env = None
    if env is None:
        # Better to say so in the data than to write a fragment that claims an
        # environment it does not have.
        env = {"schema_version": 2,
               "note": f"environment block unavailable; {args.env} was missing "
                       f"or unreadable when this fragment was written"}

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    with tmp.open("w") as fh:
        json.dump({"environment": env, args.key: body}, fh, indent=2)
        fh.write("\n")
    tmp.replace(out)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
