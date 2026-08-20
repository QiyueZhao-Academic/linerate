"""Fitting the cost model, and the diagnostics that decide whether to believe it.

The model is

    C(s) = a + b*s

with a the cycles that do not depend on payload size and b the cycles per
payload byte. The quantity the study is about is their ratio,

    s* = a / b

the payload at which the two halves are equal. Below s* a packet costs mostly
what it costs to *be* a packet; above it, mostly what it costs to be that many
bytes. s* is a ratio of two quantities that scale together with the clock, so it
is invariant to clock speed — which is what makes it comparable across machines
in a way that neither a nor b is.

Three things here are deliberate.

Weighted least squares. Each point carries a bootstrap confidence interval from
the harness, and those intervals are not equal: small payloads are noisier
because the fixed cost dominates and the fixed cost is where scheduling noise
lands. Fitting unweighted would let the noisiest points pull the intercept,
which is the parameter the whole study turns on.

Confidence interval on s* by propagation, not by dividing intervals. a and b
come from the same fit and are strongly anticorrelated: a fit that raises the
intercept lowers the slope. Dividing the interval of a by the interval of b
ignores that and produces an interval several times too wide. The covariance
matrix of the fit is used instead.

Residual diagnostics that can reject the model. A linear model fitted to
anything at all produces an a and a b. Structure left in the residuals is how a
reader finds out that the relationship was not linear, so the curvature test is
reported next to every fit rather than only when it fails.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Sequence

import numpy as np


@dataclass
class Fit:
    a: float = 0.0                  # fixed cycles per packet
    b: float = 0.0                  # cycles per payload byte
    a_se: float = 0.0
    b_se: float = 0.0
    a_ci: tuple[float, float] = (0.0, 0.0)
    b_ci: tuple[float, float] = (0.0, 0.0)
    cov_ab: float = 0.0
    s_star: float = 0.0             # crossover payload, bytes
    s_star_ci: tuple[float, float] = (0.0, 0.0)
    r_squared: float = 0.0
    rmse: float = 0.0
    n: int = 0
    residuals: list[float] = field(default_factory=list)
    curvature_p: float = 1.0        # p-value of a quadratic term
    curvature_note: str = ""
    ok: bool = False
    note: str = ""


def _t_crit(dof: int) -> float:
    """Two-sided 95% critical value of Student's t.

    Table lookup with a normal tail, so the module does not need SciPy just for
    one constant. The fits here have between 5 and 30 degrees of freedom, where
    the difference from the normal value still matters.
    """
    table = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447,
             7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179,
             13: 2.160, 14: 2.145, 15: 2.131, 16: 2.120, 17: 2.110, 18: 2.101,
             19: 2.093, 20: 2.086, 25: 2.060, 30: 2.042, 40: 2.021, 60: 2.000,
             120: 1.980}
    if dof <= 0:
        return float("inf")
    if dof in table:
        return table[dof]
    keys = sorted(table)
    if dof > keys[-1]:
        return 1.960
    lo = max(k for k in keys if k < dof)
    hi = min(k for k in keys if k > dof)
    f = (dof - lo) / (hi - lo)
    return table[lo] + f * (table[hi] - table[lo])


def fit_cost_model(sizes: Sequence[float],
                   cycles: Sequence[float],
                   weights: Sequence[float] | None = None) -> Fit:
    """Fit C(s) = a + b*s and everything needed to judge the fit."""
    x = np.asarray(sizes, dtype=float)
    y = np.asarray(cycles, dtype=float)
    f = Fit(n=len(x))

    keep = np.isfinite(x) & np.isfinite(y) & (y > 0)
    x, y = x[keep], y[keep]
    if weights is not None:
        w = np.asarray(weights, dtype=float)[keep]
        w = np.where(np.isfinite(w) & (w > 0), w, 0.0)
        if not np.any(w > 0):
            w = np.ones_like(x)
    else:
        w = np.ones_like(x)
    f.n = len(x)

    if f.n < 3:
        f.note = "fewer than three usable points"
        return f
    if len(np.unique(x)) < 2:
        f.note = "all points share one payload size, so the slope is undetermined"
        return f

    # Weighted least squares in closed form. Explicit rather than via lstsq,
    # because the covariance between a and b is needed below and getting it out
    # of the normal equations directly is clearer than reconstructing it.
    sw = w.sum()
    sx = (w * x).sum()
    sy = (w * y).sum()
    sxx = (w * x * x).sum()
    sxy = (w * x * y).sum()
    det = sw * sxx - sx * sx
    if det == 0:
        f.note = "design matrix is singular"
        return f

    b = (sw * sxy - sx * sy) / det
    a = (sy - b * sx) / sw
    f.a, f.b = float(a), float(b)

    resid = y - (a + b * x)
    f.residuals = [float(r) for r in resid]
    dof = f.n - 2
    if dof <= 0:
        f.note = "no degrees of freedom left after the fit"
        return f

    # Residual variance, scaled by the weights, then the parameter covariance.
    s2 = float((w * resid ** 2).sum() / dof)
    var_a = s2 * sxx / det
    var_b = s2 * sw / det
    cov_ab = -s2 * sx / det
    f.a_se = math.sqrt(max(var_a, 0.0))
    f.b_se = math.sqrt(max(var_b, 0.0))
    f.cov_ab = float(cov_ab)

    t = _t_crit(dof)
    f.a_ci = (f.a - t * f.a_se, f.a + t * f.a_se)
    f.b_ci = (f.b - t * f.b_se, f.b + t * f.b_se)

    ybar = float((w * y).sum() / sw)
    ss_tot = float((w * (y - ybar) ** 2).sum())
    ss_res = float((w * resid ** 2).sum())
    f.r_squared = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0
    f.rmse = math.sqrt(ss_res / f.n)

    # The crossover, and its interval by first-order propagation on the ratio.
    # The covariance term is the whole point: a and b are anticorrelated, so
    # ignoring it inflates the interval, often by a factor of three or more.
    if f.b > 0:
        f.s_star = f.a / f.b
        rel = (var_a / f.a ** 2 + var_b / f.b ** 2 - 2 * cov_ab / (f.a * f.b)) \
            if f.a != 0 else float("inf")
        se = abs(f.s_star) * math.sqrt(max(rel, 0.0))
        f.s_star_ci = (f.s_star - t * se, f.s_star + t * se)
    else:
        f.note = "non-positive slope, so the crossover is not defined"
        return f

    # Curvature: refit with a quadratic term and test whether it is needed. An F
    # test of the nested models, reported as a p-value so a reader can see how
    # close the call was rather than only a verdict.
    try:
        design2 = np.vstack([np.ones_like(x), x, x * x]).T
        wsqrt = np.sqrt(w)
        beta2, *_ = np.linalg.lstsq(design2 * wsqrt[:, None], y * wsqrt, rcond=None)
        resid2 = y - design2 @ beta2
        ss_res2 = float((w * resid2 ** 2).sum())
        dof2 = f.n - 3
        if dof2 > 0 and ss_res2 > 0:
            f_stat = ((ss_res - ss_res2) / 1.0) / (ss_res2 / dof2)
            f.curvature_p = _f_test_p(f_stat, 1, dof2)
            if f.curvature_p < 0.01:
                f.curvature_note = (
                    "a quadratic term is significant at the 1% level, so the "
                    "linear model is an approximation here and a and b should "
                    "be read as fitted over this payload range rather than as "
                    "properties of the code")
            else:
                f.curvature_note = "no significant curvature; the linear model holds"
    except np.linalg.LinAlgError:
        f.curvature_note = "curvature test could not be evaluated"

    f.ok = True
    return f


def _f_test_p(f_stat: float, d1: int, d2: int) -> float:
    """Upper tail of the F distribution, via the regularised incomplete beta.

    Written out rather than imported so that the analysis runs with numpy alone.
    SciPy is used elsewhere in the pipeline where it is worth the dependency;
    here it would be a heavy import for one tail probability.
    """
    if f_stat <= 0 or d1 <= 0 or d2 <= 0:
        return 1.0
    x = d2 / (d2 + d1 * f_stat)
    return _betainc(d2 / 2.0, d1 / 2.0, x)


def _betainc(a: float, b: float, x: float) -> float:
    """Regularised incomplete beta function I_x(a, b), by continued fraction."""
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    lbeta = (math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
             + a * math.log(x) + b * math.log(1 - x))
    if x < (a + 1) / (a + b + 2):
        return math.exp(lbeta) * _betacf(a, b, x) / a
    return 1.0 - math.exp(lbeta) * _betacf(b, a, 1 - x) / b


def _betacf(a: float, b: float, x: float, itmax: int = 200, eps: float = 3e-12) -> float:
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < 1e-30:
        d = 1e-30
    d = 1.0 / d
    h = d
    for m in range(1, itmax + 1):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < 1e-30:
            d = 1e-30
        c = 1.0 + aa / c
        if abs(c) < 1e-30:
            c = 1e-30
        d = 1.0 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < 1e-30:
            d = 1e-30
        c = 1.0 + aa / c
        if abs(c) < 1e-30:
            c = 1e-30
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < eps:
            break
    return h


def fit_from_points(points: list[dict], cipher: str, hw_crypto: bool,
                    require_stable: bool = True) -> Fit:
    """Fit one (cipher, hardware-crypto) cell of the E1 grid.

    Unstable points are excluded by default and the exclusion is recorded in the
    fit's note, because a point dropped without a trace is a point a reader
    cannot audit.
    """
    sizes, cycles, weights = [], [], []
    dropped = 0
    for p in points:
        if p.get("cipher") != cipher or bool(p.get("hw_crypto")) != hw_crypto:
            continue
        if not p.get("n"):
            continue
        if require_stable and not p.get("stable", True):
            dropped += 1
            continue
        med = float(p.get("cycles_median", 0.0))
        if med <= 0:
            continue
        lo = float(p.get("cycles_ci_lo", med))
        hi = float(p.get("cycles_ci_hi", med))
        half = max((hi - lo) / 2.0, 1e-9)
        sizes.append(float(p["payload_bytes"]))
        cycles.append(med)
        # Inverse-variance weighting, using the bootstrap interval as the
        # standard error. A point measured five times more precisely deserves
        # twenty-five times the influence, and that is what this gives it.
        weights.append(1.0 / (half ** 2))

    f = fit_cost_model(sizes, cycles, weights)
    if dropped:
        f.note = (f.note + "; " if f.note else "") + \
            f"{dropped} point(s) excluded for exceeding the stability threshold"
    return f


def amdahl_serial_fraction(shards: Sequence[int], speedup: Sequence[float]) -> float:
    """Least-squares serial fraction from a strong-scaling curve.

    Amdahl's law as speedup S(n) = 1 / (f + (1-f)/n) rearranges to a linear
    relationship in 1/n, which is fitted here rather than read off the widest
    point alone.
    """
    n = np.asarray(shards, dtype=float)
    s = np.asarray(speedup, dtype=float)
    keep = (n > 0) & (s > 0)
    n, s = n[keep], s[keep]
    if len(n) < 2:
        return 0.0
    # 1/S = f + (1-f)/n  ->  1/S = f*(1 - 1/n) + 1/n
    y = 1.0 / s - 1.0 / n
    xx = 1.0 - 1.0 / n
    denom = float((xx * xx).sum())
    if denom == 0:
        return 0.0
    return float(max(0.0, min(1.0, (xx * y).sum() / denom)))
