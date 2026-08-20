// stats.cpp — robust summaries at collection time.
#include "lr/stats.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace lr::stats {

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    if (v.size() == 1) return v[0];
    // Linear interpolation between order statistics; the same definition as
    // numpy's default, so a percentile recomputed in Python matches this one.
    double idx = (p / 100.0) * (double)(v.size() - 1);
    size_t lo = (size_t)std::floor(idx), hi = (size_t)std::ceil(idx);
    double frac = idx - (double)lo;
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

double median(std::vector<double> v) { return percentile(std::move(v), 50.0); }

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0;
    double s = 0;
    for (double x : v) s += x;
    return s / (double)v.size();
}

double stddev(const std::vector<double>& v) {
    if (v.size() < 2) return 0;
    double m = mean(v), s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / (double)(v.size() - 1));
}

void bootstrap_ci(const std::vector<double>& v, double conf, int iters,
                  uint64_t seed, double* lo, double* hi) {
    if (v.empty()) { *lo = *hi = 0; return; }
    if (v.size() == 1) { *lo = *hi = v[0]; return; }
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<size_t> pick(0, v.size() - 1);
    std::vector<double> meds;
    meds.reserve(iters);
    std::vector<double> sample(v.size());
    for (int i = 0; i < iters; ++i) {
        for (size_t j = 0; j < v.size(); ++j) sample[j] = v[pick(rng)];
        meds.push_back(median(sample));
    }
    double alpha = (1.0 - conf) / 2.0;
    *lo = percentile(meds, alpha * 100.0);
    *hi = percentile(meds, (1.0 - alpha) * 100.0);
}

Summary summarise(const std::vector<double>& v, double cv_threshold, uint64_t seed) {
    Summary s;
    if (v.empty()) return s;
    s.n      = (int)v.size();
    s.median = median(v);
    s.mean   = mean(v);
    s.min    = *std::min_element(v.begin(), v.end());
    s.max    = *std::max_element(v.begin(), v.end());
    s.cv     = (s.mean != 0.0) ? stddev(v) / s.mean : 0.0;
    bootstrap_ci(v, 0.95, 2000, seed, &s.ci_lo, &s.ci_hi);
    s.stable = s.cv <= cv_threshold;
    return s;
}

// Log-spaced buckets from 100 ns to 1 s. 24 buckets per decade resolves a
// percentile to within about 10%, which is finer than the run-to-run spread of
// a tail percentile and far cheaper than storing every sample.
const std::vector<double>& LatencyHist::edges() {
    static const std::vector<double> e = [] {
        std::vector<double> v;
        const int per_decade = 24;
        const double lo = 1e2, hi = 1e9;
        int n = (int)std::lround(std::log10(hi / lo) * per_decade);
        v.reserve(n + 1);
        for (int i = 0; i <= n; ++i)
            v.push_back(lo * std::pow(10.0, (double)i / per_decade));
        return v;
    }();
    return e;
}

LatencyHist::LatencyHist() : b_(edges().size() + 1, 0) {}

void LatencyHist::add(double ns) {
    const auto& e = edges();
    // Below the first edge and above the last are kept in the sentinel buckets
    // so that count() always equals the number of samples added.
    size_t i = (size_t)(std::upper_bound(e.begin(), e.end(), ns) - e.begin());
    if (i >= b_.size()) i = b_.size() - 1;
    b_[i]++;
    n_++;
}

double LatencyHist::quantile(double q) const {
    if (n_ == 0) return 0;
    const auto& e = edges();
    uint64_t target = (uint64_t)std::ceil(q * (double)n_);
    if (target == 0) target = 1;
    uint64_t cum = 0;
    for (size_t i = 0; i < b_.size(); ++i) {
        cum += b_[i];
        if (cum >= target) {
            if (i == 0)             return e.front();
            if (i >= e.size())      return e.back();
            // Report the bucket's geometric midpoint: with log-spaced edges that
            // is the least biased single value for the interval.
            return std::sqrt(e[i - 1] * e[i]);
        }
    }
    return e.back();
}

} // namespace lr::stats
