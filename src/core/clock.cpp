// clock.cpp — tick frequency discovery and calibration.
#include "lr/clock.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <time.h>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#endif

namespace lr::clk {
namespace {

struct Cal {
    uint64_t    tick_hz = 0;
    uint64_t    cpu_hz  = 0;
    bool        invariant = false;
    std::string source;
};

// Calibrate against CLOCK_MONOTONIC_RAW, which is not slewed by NTP. The
// measurement is repeated and the median taken, so that a single descheduling
// event during the window cannot bias the frequency estimate.
uint64_t calibrate_ticks(double window_s, int reps) {
    std::vector<double> est;
    est.reserve(reps);
    for (int i = 0; i < reps; ++i) {
        timespec t0{}, t1{};
        clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
        uint64_t c0 = ticks();
        double target = t0.tv_sec + t0.tv_nsec * 1e-9 + window_s;
        double now;
        do {
            clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
            now = t1.tv_sec + t1.tv_nsec * 1e-9;
        } while (now < target);
        uint64_t c1 = ticks();
        double elapsed = now - (t0.tv_sec + t0.tv_nsec * 1e-9);
        if (elapsed > 0) est.push_back(double(c1 - c0) / elapsed);
    }
    if (est.empty()) return 0;
    std::sort(est.begin(), est.end());
    return (uint64_t)est[est.size() / 2];
}

bool has_flag(const char* flag) {
#if defined(__linux__)
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("flags", 0) == 0 && line.find(flag) != std::string::npos) return true;
    return false;
#else
    (void)flag;
    return false;
#endif
}

// The nominal core clock, used only to express ticks as reference cycles.
uint64_t read_nominal_cpu_hz() {
#if defined(__linux__)
    // cpuinfo_max_freq is in kHz and reflects the platform's advertised ceiling.
    std::ifstream mf("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (mf) {
        unsigned long long khz = 0;
        mf >> khz;
        if (khz > 0) return khz * 1000ULL;
    }
    // In a guest, cpufreq is usually absent. Fall back to the frequency embedded
    // in the model-name string, which is the nominal (non-turbo) clock.
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) != 0) continue;
        auto at = line.rfind('@');
        if (at == std::string::npos) break;
        double ghz = atof(line.c_str() + at + 1);
        if (ghz > 0.1) return (uint64_t)(ghz * 1e9);
    }
    // Last resort: the tick rate itself. On x86 the TSC runs at the nominal
    // clock, so this makes the scale factor exactly 1 and says so in source().
    return 0;
#elif defined(__APPLE__)
    uint64_t v = 0; size_t len = sizeof(v);
    if (sysctlbyname("hw.tbfrequency", &v, &len, nullptr, 0) == 0) {
        // Apple silicon does not publish a core clock; the P-core nominal
        // frequency is not exposed by sysctl. Report 0 and let source() say so.
    }
    return 0;
#else
    return 0;
#endif
}

const Cal& cal() {
    static Cal c = [] {
        Cal x;
        std::ostringstream src;
#if defined(__aarch64__)
        uint64_t frq = 0;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(frq));
        if (frq > 0) {
            x.tick_hz = frq;
            src << "cntfrq_el0";
        }
#endif
        if (x.tick_hz == 0) {
            x.tick_hz = calibrate_ticks(0.05, 5);
            src << "calibrated-vs-CLOCK_MONOTONIC_RAW";
        }
#if defined(__x86_64__)
        x.invariant = has_flag("constant_tsc") && has_flag("nonstop_tsc");
#if defined(__APPLE__)
        x.invariant = true;
#endif
#else
        x.invariant = true;   // CNTVCT_EL0 is architecturally invariant
#endif
        x.cpu_hz = read_nominal_cpu_hz();
        if (x.cpu_hz == 0) {
            x.cpu_hz = x.tick_hz;
            src << ";cycles=ticks(nominal clock not exposed)";
        } else {
            src << ";cycles=ticks*" << (double(x.cpu_hz) / double(x.tick_hz));
        }
        x.source = src.str();
        return x;
    }();
    return c;
}

} // namespace

uint64_t tick_hz()        { return cal().tick_hz; }
uint64_t nominal_cpu_hz() { return cal().cpu_hz; }
bool     invariant()      { return cal().invariant; }
std::string source()      { return cal().source; }

double cycles_per_tick() {
    const Cal& c = cal();
    if (c.tick_hz == 0) return 1.0;
    return double(c.cpu_hz) / double(c.tick_hz);
}

} // namespace lr::clk
