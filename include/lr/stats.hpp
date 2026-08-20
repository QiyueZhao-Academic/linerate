// stats.hpp — the small statistics the harness needs at collection time.
//
// Model fitting lives in Python (python/lr/model.py). What lives here is only
// what must happen while the data is still in the process that produced it:
// robust central tendency, a spread that does not assume normality, and the
// stability gate that decides whether a point is admissible.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lr::stats {

struct Summary {
    double median   = 0;
    double ci_lo    = 0;   // bootstrap 95% CI on the median
    double ci_hi    = 0;
    double cv       = 0;   // coefficient of variation, the stability gate input
    double mean     = 0;
    double min      = 0;
    double max      = 0;
    int    n        = 0;
    bool   stable   = true;
};

double percentile(std::vector<double> v, double p);   // p in [0,100]
double median(std::vector<double> v);
double mean(const std::vector<double>& v);
double stddev(const std::vector<double>& v);

// Percentile-method bootstrap on the median. `iters` resamples with replacement;
// 2000 is the default and is plenty for a 95% interval on 11 replicates.
void bootstrap_ci(const std::vector<double>& v, double conf, int iters,
                  uint64_t seed, double* lo, double* hi);

// Summarise replicates and apply the stability gate. A point whose coefficient
// of variation exceeds `cv_threshold` is marked unstable. It is still recorded:
// an excluded point that appears in the report as excluded is a result, a
// quietly dropped one is not.
Summary summarise(const std::vector<double>& v, double cv_threshold, uint64_t seed);

// Fixed-bucket latency histogram in nanoseconds, log-spaced so that p99.9 is
// resolved without storing every sample. Bucket edges are exported with the
// data so a percentile can be re-derived from the raw histogram.
class LatencyHist {
public:
    LatencyHist();
    void   add(double ns);
    double quantile(double q) const;   // q in [0,1]
    uint64_t count() const { return n_; }
    const std::vector<uint64_t>& buckets() const { return b_; }
    static const std::vector<double>& edges();
private:
    std::vector<uint64_t> b_;
    uint64_t n_ = 0;
};

} // namespace lr::stats
