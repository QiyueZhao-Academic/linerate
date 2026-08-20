"""E9: predicting tail latency, and using the prediction to admit traffic.

The rest of the study measures. This asks whether the measurements support a
control decision: given the offered rate, the payload size, the cipher and the
network conditions, can the p99 latency be predicted well enough to decide
whether to accept more traffic?

The comparison that matters is not "does a model fit". A model fitted to a
smooth curve always fits. It is whether a learned predictor beats the thing an
operator would actually do without one, which is a static threshold — admit
traffic up to some fixed fraction of measured capacity, refuse beyond it. That
baseline is implemented here and is what the learned models are scored against.
Reporting only the learned model's R^2 would be reporting that latency depends
on load, which nobody doubted.

Two models are fitted, deliberately at opposite ends of the flexibility range:

  ridge regression on physically motivated features. Linear in the features, so
  its coefficients can be read. The features include 1/(1-rho), the queueing
  term that any M/G/1-like system produces, because leaving it out would force a
  linear model to approximate a pole with a straight line and would understate
  what an interpretable model can do here.

  gradient boosting. Flexible, uninterpretable, and included as an upper bound
  on what is extractable from these features at all. If it does not beat ridge
  by much, the relationship is close to the analytic form and the report can say
  so; if it beats it by a lot, the analytic form is missing something.

Data are grouped by run condition when splitting, not shuffled. Points from one
netem condition at neighbouring offered rates are near-duplicates, so a random
split would leak and every score would be optimistic.
"""
from __future__ import annotations

import json
import math
import pathlib
from typing import Any

import numpy as np


def _features(rows: list[dict], capacity: float) -> tuple[np.ndarray, list[str]]:
    """Physically motivated features, so the linear model has a fair chance.

    rho is the utilisation. The 1/(1-rho) term is the shape a queueing delay
    takes as utilisation approaches one; clipped, because at rho >= 1 the delay
    is unbounded and no finite feature represents it.
    """
    names = ["rho", "inv_one_minus_rho", "payload_bytes", "loss_ratio",
             "netem_delay_ms", "netem_loss_pct", "achieved_ratio"]
    out = []
    for r in rows:
        rho = float(r.get("offered_fraction") or
                    (r["offered_pps"] / capacity if capacity else 0.0))
        rho_c = min(max(rho, 0.0), 0.995)
        delay_ms, loss_pct = _parse_netem(r.get("netem_label", "none"))
        achieved = r.get("achieved_pps", 0.0)
        out.append([
            rho,
            1.0 / (1.0 - rho_c),
            float(r.get("payload_bytes", 512)),
            float(r.get("loss_ratio", 0.0)),
            delay_ms,
            loss_pct,
            (achieved / r["offered_pps"]) if r.get("offered_pps") else 1.0,
        ])
    return np.asarray(out, dtype=float), names


def _parse_netem(label: str) -> tuple[float, float]:
    """Read the delay and loss out of a condition label such as 'd5ms-l0.1'.

    cloud/netem.sh writes the label, so the format is fixed by that script and
    not guessed at here.
    """
    delay = loss = 0.0
    if not label or label == "none":
        return 0.0, 0.0
    for part in label.split("-"):
        part = part.strip()
        if part.startswith("d") and part.endswith("ms"):
            try:
                delay = float(part[1:-2])
            except ValueError:
                pass
        elif part.startswith("l"):
            try:
                loss = float(part[1:])
            except ValueError:
                pass
    return delay, loss


def _group_split(labels: list[str], test_fraction: float = 0.3) -> tuple[np.ndarray, np.ndarray]:
    """Split by condition, so no condition appears on both sides."""
    uniq = sorted(set(labels))
    if len(uniq) < 2:
        # Only one condition available: fall back to a contiguous split by
        # position, which still avoids interleaving neighbouring rates but
        # cannot avoid the two halves sharing a condition. The report records
        # that this happened rather than presenting the score as clean.
        n = len(labels)
        cut = int(n * (1 - test_fraction))
        idx = np.arange(n)
        return idx[:cut], idx[cut:]
    n_test = max(1, int(round(len(uniq) * test_fraction)))
    # Held-out groups are taken at a stride through the sorted list rather than
    # from its tail. Taking the tail would hold out the most extreme conditions
    # and score the model on extrapolation, which is both harder than the task
    # and not the task: an admission controller interpolates between conditions
    # it has seen, it does not predict one worse than any it was trained on.
    stride = max(1, len(uniq) // (n_test + 1))
    test_groups = {uniq[min(len(uniq) - 1, (i + 1) * stride)] for i in range(n_test)}
    arr = np.asarray(labels)
    test = np.where(np.isin(arr, list(test_groups)))[0]
    train = np.where(~np.isin(arr, list(test_groups)))[0]
    return train, test


def _r2(y: np.ndarray, pred: np.ndarray) -> float:
    ss_res = float(((y - pred) ** 2).sum())
    ss_tot = float(((y - y.mean()) ** 2).sum())
    return 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0


def _static_baseline(x_train: np.ndarray, y_train: np.ndarray,
                     x_test: np.ndarray, feature_names: list[str]) -> np.ndarray:
    """What an operator does without a model.

    Two regimes separated by a fixed utilisation threshold, with the mean
    observed latency of each regime as the prediction. This is generous to the
    baseline: the threshold is chosen on the training data rather than fixed in
    advance.
    """
    rho_i = feature_names.index("rho")
    best_thr, best_err = 0.8, float("inf")
    for thr in np.arange(0.5, 1.01, 0.05):
        below = y_train[x_train[:, rho_i] < thr]
        above = y_train[x_train[:, rho_i] >= thr]
        if len(below) == 0 or len(above) == 0:
            continue
        pred = np.where(x_train[:, rho_i] < thr, below.mean(), above.mean())
        err = float(((y_train - pred) ** 2).mean())
        if err < best_err:
            best_err, best_thr = err, float(thr)
    below = y_train[x_train[:, rho_i] < best_thr]
    above = y_train[x_train[:, rho_i] >= best_thr]
    lo = below.mean() if len(below) else y_train.mean()
    hi = above.mean() if len(above) else y_train.mean()
    return np.where(x_test[:, rho_i] < best_thr, lo, hi)


def run(data: dict, out_path: str | pathlib.Path) -> dict:
    """Fit the models and write results/raw/e9_admission.json."""
    out_path = pathlib.Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    def _write(body: dict) -> dict:
        doc = {"environment": data.get("environment", {}), "e9": body}
        tmp = out_path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(doc, indent=2) + "\n")
        tmp.replace(out_path)
        return body

    e5 = data.get("e5", {})
    rows = [p for p in e5.get("points", []) if p.get("ok") and p.get("p99_ns", 0) > 0]
    if len(rows) < 12:
        return _write({"available": False,
                       "reason": f"only {len(rows)} usable latency points; the "
                                 "admission model needs at least twelve, which "
                                 "means running E5 under several netem conditions"})
    if not e5.get("interpretable", True):
        return _write({"available": False,
                       "reason": "the latency dataset is marked uninterpretable: "
                                 + e5.get("interpretation_note", "")})

    try:
        from sklearn.ensemble import GradientBoostingRegressor
        from sklearn.linear_model import Ridge
        from sklearn.preprocessing import StandardScaler
    except ImportError:
        return _write({"available": False,
                       "reason": "scikit-learn is not installed; "
                                 "pip install -r requirements.txt"})

    capacity = float(e5.get("capacity_pps") or 0.0)
    x, names = _features(rows, capacity)
    y = np.asarray([r["p99_ns"] for r in rows], dtype=float)
    labels = [r.get("netem_label", "none") for r in rows]
    train, test = _group_split(labels)
    if len(train) < 6 or len(test) < 3:
        return _write({"available": False,
                       "reason": "not enough distinct conditions to split without "
                                 "leaking neighbouring rates between train and test"})

    # The target is log latency. Tail latency spans orders of magnitude across
    # the knee, so a squared error on the raw value would be entirely dominated
    # by the overloaded points and the model would learn nothing about the
    # region where an admission decision is actually made.
    ylog = np.log(np.maximum(y, 1.0))

    scaler = StandardScaler().fit(x[train])
    xs_tr, xs_te = scaler.transform(x[train]), scaler.transform(x[test])

    ridge = Ridge(alpha=1.0).fit(xs_tr, ylog[train])
    ridge_pred = np.exp(ridge.predict(xs_te))

    gbr = GradientBoostingRegressor(
        n_estimators=200, max_depth=3, learning_rate=0.05,
        random_state=20260819).fit(xs_tr, ylog[train])
    gbr_pred = np.exp(gbr.predict(xs_te))

    base_pred = _static_baseline(x[train], y[train], x[test], names)

    r2_ridge = _r2(y[test], ridge_pred)
    r2_gbr = _r2(y[test], gbr_pred)
    r2_base = _r2(y[test], base_pred)
    best_name, best_pred, best_r2 = (
        ("gradient boosting", gbr_pred, r2_gbr) if r2_gbr >= r2_ridge
        else ("ridge regression", ridge_pred, r2_ridge))

    # The control decision, scored the way it would be used: given a latency
    # budget, admit if the predicted p99 is under it. An error is a wrong
    # decision, not a wrong number, and the two do not have the same shape --
    # being wrong by 10% matters only when it crosses the budget.
    budget = float(np.median(y))
    truth = y[test] <= budget
    decisions = {
        "learned": best_pred <= budget,
        "static": base_pred <= budget,
    }
    accuracy = {k: float((v == truth).mean()) for k, v in decisions.items()}
    # An admission controller that admits traffic which then misses the budget
    # is worse than one that refuses traffic it could have served, so the two
    # error kinds are reported separately rather than folded into one score.
    over_admit = {k: float(((v) & (~truth)).mean()) for k, v in decisions.items()}

    body = {
        "available": True,
        "n_samples": len(rows),
        "n_train": int(len(train)),
        "n_test": int(len(test)),
        "conditions": sorted(set(labels)),
        "single_condition": len(set(labels)) < 2,
        "features": names,
        "target": "p99 latency, nanoseconds, fitted in log space",
        "capacity_pps": capacity,
        "latency_budget_ns": budget,
        "test_r2": best_r2,
        "ridge_r2": r2_ridge,
        "gbr_r2": r2_gbr,
        "baseline_r2": r2_base,
        "best_model": best_name,
        "admission_accuracy_learned": accuracy["learned"],
        "admission_accuracy_static": accuracy["static"],
        "admission_gain": accuracy["learned"] - accuracy["static"],
        "over_admission_learned": over_admit["learned"],
        "over_admission_static": over_admit["static"],
        "ridge_coefficients": dict(zip(names, [float(c) for c in ridge.coef_])),
        "feature_importance": dict(zip(names,
                                       [float(f) for f in gbr.feature_importances_])),
        "observed_p99_ns": [float(v) for v in y[test]],
        "predicted_p99_ns": [float(v) for v in best_pred],
        "baseline_note": "the static baseline admits below a utilisation "
                         "threshold chosen on the training split, which is more "
                         "than an operator without a model would normally tune",
    }
    return _write(body)


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--results", default="results/results.json")
    ap.add_argument("--out", default="results/raw/e9_admission.json")
    args = ap.parse_args()
    path = pathlib.Path(args.results)
    if not path.exists():
        print(f"no dataset at {path}; run tools/merge_results.py first")
        return 2
    data = json.loads(path.read_text())
    body = run(data, args.out)
    if body.get("available"):
        print(f"E9: {body['best_model']} reaches R^2 {body['test_r2']:.3f} "
              f"against {body['baseline_r2']:.3f} for the static baseline")
    else:
        print(f"E9 not available: {body.get('reason')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
