"""Figures.

House rules, applied by build() rather than remembered per figure:

  vector output only, so the report never carries a resampled raster;
  no colour carries information on its own, because a double-column paper is
  read in greyscale as often as not — marker shape and line style carry it too;
  every axis says what it is and in what unit;
  intervals are drawn wherever the harness measured them, and where a point was
  excluded by the stability gate it is drawn hollow rather than omitted, so a
  reader can see what was left out.

Every figure returns the caption facts it depends on, so that the numbers quoted
in the text under a figure come from the same computation that drew it.
"""
from __future__ import annotations

import math
import pathlib
from typing import Any

import matplotlib
matplotlib.use("Agg")            # no display on a measurement host
import matplotlib.pyplot as plt
import numpy as np

from . import model

# Single-column width of the ACM sigconf template, in inches.
COL_W = 3.33
GOLDEN = 1.618

CIPHER_STYLE = {
    "aes-256-gcm":            dict(marker="o", ls="-",  color="#1f4e79"),
    "chacha20-poly1305":      dict(marker="s", ls="--", color="#7f3f00"),
    "lr-aes-256-gcm":         dict(marker="^", ls="-.", color="#2e6f40"),
    "lr-chacha20-poly1305":   dict(marker="v", ls=":",  color="#7d2e68"),
    "null":                   dict(marker="x", ls="-",  color="#555555"),
}

BACKEND_STYLE = {
    "posix":        dict(marker="o", ls="-",  color="#1f4e79"),
    "mmsg":         dict(marker="s", ls="--", color="#7f3f00"),
    "uring":        dict(marker="^", ls="-.", color="#2e6f40"),
    "uring-sqpoll": dict(marker="v", ls=":",  color="#7d2e68"),
}


def _setup():
    plt.rcParams.update({
        "font.size": 7,
        "axes.labelsize": 7,
        "axes.titlesize": 7,
        "legend.fontsize": 6,
        "xtick.labelsize": 6,
        "ytick.labelsize": 6,
        "axes.grid": True,
        "grid.alpha": 0.25,
        "grid.linewidth": 0.4,
        "lines.linewidth": 1.0,
        "lines.markersize": 3.0,
        "figure.dpi": 200,
        "savefig.bbox": "tight",
        "savefig.pad_inches": 0.01,
        "pdf.fonttype": 42,      # embed TrueType, not Type 3: some venues reject Type 3
        "ps.fonttype": 42,
    })


def _save(fig, outdir: pathlib.Path, name: str) -> str:
    outdir.mkdir(parents=True, exist_ok=True)
    path = outdir / f"{name}.pdf"
    fig.savefig(path)
    plt.close(fig)
    return str(path)


def _style(table: dict, key: str) -> dict:
    return dict(table.get(key, dict(marker=".", ls="-", color="#333333")))


# ---------------------------------------------------------------------------

def fig_cost_model(data: dict, outdir: pathlib.Path) -> dict:
    """Cycles per packet against payload size, with the fitted lines."""
    e1 = data.get("e1", {})
    pts = e1.get("points", [])
    if not pts:
        return {}

    fig, axes = plt.subplots(1, 2, figsize=(2 * COL_W, COL_W / GOLDEN * 1.15),
                             sharex=True)
    facts: dict[str, Any] = {"fits": {}}

    for ax, hw in zip(axes, [True, False]):
        ciphers = sorted({p["cipher"] for p in pts if bool(p["hw_crypto"]) == hw})
        if not ciphers:
            ax.set_visible(False)
            continue
        for cipher in ciphers:
            sel = [p for p in pts
                   if p["cipher"] == cipher and bool(p["hw_crypto"]) == hw and p.get("n")]
            if not sel:
                continue
            sel.sort(key=lambda p: p["payload_bytes"])
            xs = np.array([p["payload_bytes"] for p in sel], dtype=float)
            ys = np.array([p["cycles_median"] for p in sel], dtype=float)
            lo = np.array([p["cycles_ci_lo"] for p in sel], dtype=float)
            hi = np.array([p["cycles_ci_hi"] for p in sel], dtype=float)
            stable = np.array([bool(p.get("stable", True)) for p in sel])

            st = _style(CIPHER_STYLE, cipher)
            ax.errorbar(xs[stable], ys[stable],
                        yerr=[ys[stable] - lo[stable], hi[stable] - ys[stable]],
                        ls="none", marker=st["marker"], color=st["color"],
                        capsize=1.2, elinewidth=0.5, label=cipher)
            if (~stable).any():
                # Excluded from the fit, but drawn: a reader should be able to
                # see how far the discarded points sat from the line.
                ax.plot(xs[~stable], ys[~stable], ls="none", marker=st["marker"],
                        mfc="none", color=st["color"], alpha=0.6)

            f = model.fit_from_points(pts, cipher, hw)
            if f.ok:
                gx = np.linspace(0, max(xs) * 1.02, 64)
                ax.plot(gx, f.a + f.b * gx, ls=st["ls"], color=st["color"], lw=0.8)
                facts["fits"][f"{cipher}|{'hw' if hw else 'masked'}"] = {
                    "a": f.a, "b": f.b, "s_star": f.s_star,
                    "r_squared": f.r_squared, "n": f.n,
                }
        ax.set_xlabel("payload, bytes")
        ax.set_title("hardware crypto available" if hw else "hardware crypto masked")
        ax.set_ylim(bottom=0)
    axes[0].set_ylabel("cycles per packet")
    if any(ax.get_visible() for ax in axes):
        axes[0].legend(frameon=False, loc="upper left")
    facts["path"] = _save(fig, outdir, "cost_model")
    return facts


def fig_crossover(data: dict, outdir: pathlib.Path) -> dict:
    """Where the fixed and per-byte halves of the cost are equal."""
    e1 = data.get("e1", {})
    pts = e1.get("points", [])
    if not pts:
        return {}
    rows = []
    for cipher in sorted({p["cipher"] for p in pts}):
        for hw in sorted({bool(p["hw_crypto"]) for p in pts}, reverse=True):
            f = model.fit_from_points(pts, cipher, hw)
            if f.ok:
                rows.append((f"{cipher}\n{'hw' if hw else 'masked'}", f))
    if not rows:
        return {}

    fig, ax = plt.subplots(figsize=(2 * COL_W, COL_W / GOLDEN))
    ys = np.arange(len(rows))
    vals = [r[1].s_star for r in rows]
    err_lo = [max(0.0, r[1].s_star - r[1].s_star_ci[0]) for r in rows]
    err_hi = [max(0.0, r[1].s_star_ci[1] - r[1].s_star) for r in rows]
    ax.barh(ys, vals, xerr=[err_lo, err_hi], color="#c9d6e3", edgecolor="#1f4e79",
            height=0.6, error_kw=dict(elinewidth=0.6, capsize=1.5, ecolor="#333333"))
    ax.set_yticks(ys)
    ax.set_yticklabels([r[0] for r in rows])
    ax.invert_yaxis()
    ax.set_xlabel(r"crossover payload $s^{*} = a/b$, bytes")

    # The largest payload that fits an Ethernet MTU. A crossover beyond it means
    # the fixed cost dominates at every size this data plane can carry, which is
    # a conclusion and not a footnote.
    mtu_payload = data.get("environment", {}).get("protocol", {}) \
                      .get("max_payload_bytes", 1432)
    ax.axvline(mtu_payload, color="#7f3f00", ls="--", lw=0.8)
    ax.annotate(f"max payload {mtu_payload} B", xy=(mtu_payload, len(rows) - 0.5),
                xytext=(4, 0), textcoords="offset points", fontsize=6,
                color="#7f3f00", va="bottom")
    return {"path": _save(fig, outdir, "crossover"),
            "beyond_mtu": [r[0].replace("\n", " ") for r in rows
                           if r[1].s_star > mtu_payload]}


def fig_iopath(data: dict, outdir: pathlib.Path) -> dict:
    """Per-packet cost and syscalls against batch depth, one line per backend."""
    e3 = data.get("e3", {})
    pts = [p for p in e3.get("points", []) if p.get("ok")]
    if not pts:
        return {}
    payload = max(p["payload_bytes"] for p in pts)
    pts = [p for p in pts if p["payload_bytes"] == payload]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(2 * COL_W, COL_W / GOLDEN * 1.05))
    for backend in sorted({p["backend"] for p in pts}):
        sel = sorted([p for p in pts if p["backend"] == backend],
                     key=lambda p: p["batch"])
        st = _style(BACKEND_STYLE, backend)
        xs = [p["batch"] for p in sel]
        ax1.plot(xs, [p["cycles_median"] for p in sel], label=backend, **st)
        ax2.plot(xs, [p["syscalls_per_packet"] for p in sel], **st)
    for ax in (ax1, ax2):
        ax.set_xscale("log", base=2)
        ax.set_xlabel("batch depth, packets per call")
    ax1.set_ylabel("cycles per packet")
    ax2.set_ylabel("system calls per packet")
    ax1.set_ylim(bottom=0)
    ax2.set_ylim(bottom=0)
    ax1.legend(frameon=False)
    ax1.set_title(f"payload {payload} B")
    return {"path": _save(fig, outdir, "iopath"), "payload": payload}


def fig_scaling(data: dict, outdir: pathlib.Path) -> dict:
    """Strong and weak scaling of the shard pool."""
    e2 = data.get("e2", {})
    pts = [p for p in e2.get("points", []) if p.get("ok")]
    if not pts:
        return {}
    pts.sort(key=lambda p: p["shards"])
    n = [p["shards"] for p in pts]

    fig, ax = plt.subplots(figsize=(COL_W, COL_W / GOLDEN))
    ax.plot(n, [p["strong_speedup"] for p in pts], marker="o", color="#1f4e79",
            label="strong speedup")
    ax.plot(n, [p["weak_efficiency"] for p in pts], marker="s", ls="--",
            color="#7f3f00", label="weak efficiency")
    ax.plot(n, n, ls=":", color="#999999", lw=0.8, label="ideal")
    ax.set_xlabel("shards")
    ax.set_ylabel("speedup / efficiency")
    ax.legend(frameon=False)

    serial = model.amdahl_serial_fraction(
        n, [p["strong_speedup"] for p in pts])
    return {"path": _save(fig, outdir, "scaling"),
            "max_shards": max(n),
            "serial_fraction": serial,
            "single_core": len(n) < 2}


def fig_latency(data: dict, outdir: pathlib.Path) -> dict:
    """Tail latency against offered load."""
    e5 = data.get("e5", {})
    pts = [p for p in e5.get("points", []) if p.get("ok")]
    if not pts:
        return {}
    pts.sort(key=lambda p: p["offered_pps"])
    x = [p["offered_fraction"] * 100 for p in pts]

    fig, ax = plt.subplots(figsize=(COL_W, COL_W / GOLDEN))
    for key, label, st in [("p50_ns", "p50", dict(marker="o", ls="-", color="#1f4e79")),
                           ("p99_ns", "p99", dict(marker="s", ls="--", color="#7f3f00")),
                           ("p999_ns", "p99.9", dict(marker="^", ls="-.", color="#7d2e68"))]:
        ax.plot(x, [p[key] / 1000.0 for p in pts], label=label, **st)
    ax.set_yscale("log")
    ax.set_xlabel("offered load, % of measured capacity")
    ax.set_ylabel("latency, microseconds")
    ax.legend(frameon=False)
    if e5.get("knee_fraction"):
        ax.axvline(e5["knee_fraction"] * 100, color="#2e6f40", ls=":", lw=0.8)
    return {"path": _save(fig, outdir, "latency"),
            "interpretable": e5.get("interpretable", True),
            "topology": e5.get("topology", "unknown")}


def fig_memroof(data: dict, outdir: pathlib.Path) -> dict:
    """Achieved bandwidth against working-set size."""
    e4 = data.get("e4", {})
    pts = e4.get("points", [])
    if not pts:
        return {}
    fig, ax = plt.subplots(figsize=(COL_W, COL_W / GOLDEN))
    xs = [p["per_array_bytes"] / 1024.0 for p in pts]
    ax.plot(xs, [p["gbps"] for p in pts], marker="o", color="#1f4e79")
    ax.set_xscale("log", base=2)
    ax.set_xlabel("working set per array, KiB")
    ax.set_ylabel("triad bandwidth, GB/s")
    ax.set_ylim(bottom=0)
    return {"path": _save(fig, outdir, "memroof"),
            "sustained_gbps": e4.get("stream_bandwidth_gbps", 0.0)}


def fig_baselines(data: dict, outdir: pathlib.Path) -> dict:
    """This data plane beside the independent references."""
    e6 = data.get("e6", {})
    net = data.get("e6_network", {})
    rows: list[tuple[str, float]] = []
    for entry in e6.get("linerate", []):
        if entry.get("cycles_per_byte"):
            rows.append((f"linerate {entry['cipher']}", entry["cycles_per_byte"] / 2.0))
    for entry in e6.get("openssl_speed", []):
        if entry.get("ok") and entry.get("cycles_per_byte"):
            rows.append((f"openssl speed {entry['cipher']}", entry["cycles_per_byte"]))
    if not rows:
        return {}

    fig, ax = plt.subplots(figsize=(2 * COL_W, COL_W / GOLDEN * 0.8))
    ys = np.arange(len(rows))
    ax.barh(ys, [r[1] for r in rows], color="#c9d6e3", edgecolor="#1f4e79", height=0.6)
    ax.set_yticks(ys)
    ax.set_yticklabels([r[0] for r in rows])
    ax.invert_yaxis()
    ax.set_xlabel("cycles per payload byte, per pass through the cipher")
    facts = {"path": _save(fig, outdir, "baselines"), "rows": len(rows)}
    for key in ("iperf3_gbps", "wireguard_gbps", "linerate_gbps"):
        if key in net:
            facts[key] = net[key]
    return facts


def fig_virtualization(data: dict, outdir: pathlib.Path) -> dict:
    """Per-packet cost across isolation tiers, and along a service chain."""
    e7 = data.get("e7", {})
    tiers = [t for t in e7.get("tiers", []) if t.get("ok")]
    chain = [c for c in e7.get("chain", []) if c.get("ok")]
    if not tiers and not chain:
        return {}

    ncols = 2 if chain else 1
    fig, axes = plt.subplots(1, ncols, figsize=(ncols * COL_W, COL_W / GOLDEN))
    axes = np.atleast_1d(axes)

    if tiers:
        ax = axes[0]
        labels = [t["tier"] for t in tiers]
        vals = [t["cycles_median"] for t in tiers]
        ax.bar(np.arange(len(vals)), vals, color="#c9d6e3", edgecolor="#1f4e79",
               width=0.6)
        ax.set_xticks(np.arange(len(labels)))
        ax.set_xticklabels(labels, rotation=20, ha="right")
        ax.set_ylabel("cycles per packet")
        ax.set_title("isolation tier")
    if chain:
        ax = axes[-1]
        ax.plot([c["chain_length"] for c in chain],
                [c["cycles_median"] for c in chain], marker="o", color="#1f4e79")
        ax.set_xlabel("functions in the chain")
        ax.set_ylabel("cycles per packet")
        ax.set_title("service chaining")
    return {"path": _save(fig, outdir, "virtualization"),
            "tiers": len(tiers), "chain_points": len(chain)}


def fig_gpu(data: dict, outdir: pathlib.Path) -> dict:
    """Where a GPU offload's fixed cost stops dominating, and what the bus costs."""
    e8 = data.get("e8", {})
    pts = e8.get("points", [])
    if not pts or not e8.get("available"):
        return {}
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(2 * COL_W, COL_W / GOLDEN))
    xs = [p["packets"] for p in pts]
    ax1.plot(xs, [p["ns_per_packet"] for p in pts], marker="o", color="#1f4e79")
    ax1.set_xscale("log", base=2)
    ax1.set_yscale("log")
    ax1.set_xlabel("batch, packets")
    ax1.set_ylabel("nanoseconds per packet")
    ax2.stackplot(xs,
                  [p["h2d_ns"] / max(p["total_ns"], 1e-9) for p in pts],
                  [p["kernel_ns"] / max(p["total_ns"], 1e-9) for p in pts],
                  [p["d2h_ns"] / max(p["total_ns"], 1e-9) for p in pts],
                  labels=["host to device", "kernel", "device to host"],
                  colors=["#c9d6e3", "#f0d9b5", "#cfe3c9"])
    ax2.set_xscale("log", base=2)
    ax2.set_xlabel("batch, packets")
    ax2.set_ylabel("share of total time")
    ax2.set_ylim(0, 1)
    ax2.legend(frameon=False, loc="lower right")
    return {"path": _save(fig, outdir, "gpu"),
            "worthwhile_batch": e8.get("worthwhile_batch_packets", 0)}


def fig_admission(data: dict, outdir: pathlib.Path) -> dict:
    """Predicted against observed tail latency for the admission model."""
    e9 = data.get("e9", {})
    if not e9.get("available"):
        return {}
    obs = e9.get("observed_p99_ns", [])
    pred = e9.get("predicted_p99_ns", [])
    if not obs or not pred:
        return {}
    fig, ax = plt.subplots(figsize=(COL_W, COL_W / GOLDEN))
    ax.plot(np.array(obs) / 1000.0, np.array(pred) / 1000.0, ls="none",
            marker="o", ms=2.5, color="#1f4e79", alpha=0.7)
    lim = max(max(obs), max(pred)) / 1000.0
    ax.plot([0, lim], [0, lim], ls=":", color="#999999", lw=0.8)
    ax.set_xlabel(r"observed p99, microseconds")
    ax.set_ylabel(r"predicted p99, microseconds")
    ax.set_xscale("log")
    ax.set_yscale("log")
    return {"path": _save(fig, outdir, "admission"),
            "r2": e9.get("test_r2", 0.0)}


ALL_FIGURES = [
    ("cost_model", fig_cost_model),
    ("crossover", fig_crossover),
    ("iopath", fig_iopath),
    ("scaling", fig_scaling),
    ("latency", fig_latency),
    ("memroof", fig_memroof),
    ("baselines", fig_baselines),
    ("virtualization", fig_virtualization),
    ("gpu", fig_gpu),
    ("admission", fig_admission),
]


def build(data: dict, outdir: str | pathlib.Path) -> dict:
    """Draw every figure the dataset supports; report what it did not support."""
    _setup()
    out = pathlib.Path(outdir)
    facts: dict[str, Any] = {}
    skipped = []
    for name, fn in ALL_FIGURES:
        try:
            result = fn(data, out)
        except Exception as exc:                      # noqa: BLE001
            # A figure that cannot be drawn must not take the report down with
            # it: the reason is recorded and the section renders without it.
            skipped.append(f"{name}: {type(exc).__name__}: {exc}")
            continue
        if result:
            facts[name] = result
        else:
            skipped.append(f"{name}: no data in this dataset")
    facts["__skipped__"] = skipped
    return facts
