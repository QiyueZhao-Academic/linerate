// clock.hpp — cycle-accurate timing with an explicit unit contract.
//
// The whole study is expressed in *reference cycles*. A reference cycle is one
// tick of the platform's invariant timestamp counter, rescaled to the nominal
// core clock:
//
//     cycles = ticks * (nominal_cpu_hz / tick_hz)
//
// On x86-64 the TSC advances at the nominal core frequency, so the scale factor
// is 1 by construction. On AArch64 the userspace counter is CNTVCT_EL0, which
// runs at CNTFRQ_EL0 (typically 24-100 MHz), so the factor is large and the
// counter resolution is coarse; every measurement therefore times a *window* of
// many packets and divides, never a single packet.
//
// The unit matters for absolute numbers only. The crossover payload s* = a/b is
// a ratio of two quantities in the same unit and is invariant to the scale
// factor, which is why it survives an unpinnable turbo clock (see docs/methodology.md).
#pragma once
#include <cstdint>
#include <string>

#if defined(__x86_64__) || defined(_M_X64)
// Included at file scope: an include inside a namespace would pull every
// intrinsic declaration into it and break anything else that uses them.
#include <x86intrin.h>
#endif

namespace lr::clk {

#if defined(__x86_64__) || defined(_M_X64)
// rdtscp already waits for prior instructions to retire; the trailing lfence
// stops later loads from being hoisted above the read.
static inline uint64_t ticks() {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}
#elif defined(__aarch64__)
static inline uint64_t ticks() {
    uint64_t t;
    // isb orders the counter read against surrounding instructions.
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(t)::"memory");
    return t;
}
#else
#error "linerate requires x86-64 or AArch64 for cycle-level measurement"
#endif

// Frequency of the raw tick counter, in Hz. Read from the architecture where it
// is authoritative (CNTFRQ_EL0), otherwise calibrated against CLOCK_MONOTONIC_RAW.
uint64_t tick_hz();

// Nominal core clock in Hz, as advertised by the OS. Used only to convert ticks
// to reference cycles; never assumed to be the *actual* clock during a run.
uint64_t nominal_cpu_hz();

// nominal_cpu_hz() / tick_hz().
double cycles_per_tick();

// True when the counter is guaranteed not to vary with P-state or halt in idle.
// x86: constant_tsc && nonstop_tsc. AArch64: architecturally guaranteed.
bool invariant();

// Human-readable description of how tick_hz() was obtained, recorded in the
// environment block so a number stays interpretable a year later.
std::string source();

inline double ticks_to_cycles(uint64_t t) { return double(t) * cycles_per_tick(); }
inline double ticks_to_ns(uint64_t t) { return double(t) * 1e9 / double(tick_hz()); }
inline uint64_t ns_to_ticks(double ns) { return uint64_t(ns * double(tick_hz()) / 1e9); }

// Busy-wait until the counter reaches `deadline`. Used by the open-loop
// generator, where sleeping would quantise the send schedule to the timer tick.
inline void spin_until(uint64_t deadline) {
    while (ticks() < deadline) {
#if defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#endif
    }
}

} // namespace lr::clk
