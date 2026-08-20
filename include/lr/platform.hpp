// platform.hpp — capability detection and the host classification that the
// measurement discipline depends on.
//
// The rule the project enforces in code: a host that cannot hold a measurement
// still is not allowed to write one. Rather than a single global switch, each
// experiment declares the tier it needs, and the harness refuses to run above
// the tier of the host it is on.
//
//   development  anything that cannot time reliably: macOS, a non-invariant
//                counter, no hard thread affinity. Builds, self-tests, analyses
//                and typesets — never measures.
//   constrained  Linux with an invariant counter and hard affinity, but without
//                kernel isolation (isolcpus / nohz_full / rcu_nocbs) or with
//                fewer than two usable cores. Measures, and every dataset it
//                writes carries the caveats that apply to it.
//   measurement  constrained, plus kernel isolation on the data-plane cores,
//                homogeneous cores, and at least two of them.
//
// Separately from the tier, the harness records the *isolation tier* it is
// running inside — the VM's own kernel, a namespace, a runc container, or a
// gVisor sandbox — because E7 varies exactly that and a number that does not
// say which sandbox produced it is not comparable with one that does.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lr::plat {

enum class HostClass { Development = 0, Constrained = 1, Measurement = 2 };

const char* host_class_name(HostClass c);
HostClass   host_class_from_name(const std::string& s);

struct Caps {
    std::string os;              // "Linux 6.8.0-31-generic" / "Darwin 25.6.0"
    std::string arch;            // "x86_64" / "aarch64"
    std::string cpu_model;
    int      logical_cpus   = 0;
    int      physical_cores = 0;
    bool     smt_enabled    = false;
    bool     homogeneous    = true;   // false when P-cores and E-cores coexist
    bool     invariant_tsc  = false;
    bool     hard_affinity  = false;  // sched_setaffinity is enforced, not advisory
    bool     batched_io     = false;  // recvmmsg / sendmmsg present
    bool     uring_io       = false;  // io_uring accepted a ring
    bool     uring_sqpoll   = false;  // io_uring accepted IORING_SETUP_SQPOLL
    bool     isolcpus       = false;
    bool     nohz_full      = false;
    bool     rcu_nocbs      = false;
    bool     hw_aes         = false;  // AES-NI, or ARMv8 crypto extensions
    bool     hw_clmul       = false;  // PCLMULQDQ / PMULL, the GHASH accelerator
    bool     hw_avx2        = false;
    bool     openmp         = false;
    std::string kernel_cmdline;
    std::string governor       = "unknown";
    std::string turbo_control  = "unknown";
    std::string irqbalance     = "unknown";
    std::string isolation_tier = "host";   // host | netns | container | gvisor
    std::string nic_driver     = "unknown";// gve (gVNIC), virtio_net, veth, lo
    std::string nic_name       = "";
    std::string nic_addr       = "";
    uint64_t tsc_hz         = 0;
    uint64_t nominal_cpu_hz = 0;
    std::string clock_source;
    std::string openssl_version;
    std::vector<std::string> cpu_flags;   // only the flags this study depends on

    HostClass klass() const;
    // Human-readable reasons the host did not reach Measurement, e.g.
    // "kernel isolation absent (isolcpus)". Empty on a measurement host.
    std::vector<std::string> caveats() const;
};

const Caps& caps();

// Pin the calling thread to one logical CPU. Returns false when the platform
// offers only an advisory hint (macOS), in which case the caller must not
// produce a measurement that assumes pinning held.
bool pin_to_cpu(int cpu);

// The primary non-loopback interface and its address, discovered once. Empty
// when the host has none, which is how the harness knows it cannot run a wire
// experiment.
bool primary_interface(std::string* name, std::string* addr, std::string* driver);

// The OpenSSL capability-mask environment variable that clears hardware AES and
// the carry-less multiply used by GHASH, for this architecture. Empty when the
// architecture has no such lever.
//   x86-64  OPENSSL_ia32cap  clears AES-NI, PCLMULQDQ, VAES, VPCLMULQDQ
//   aarch64 OPENSSL_armcap   clears ARMV8_AES and ARMV8_PMULL, keeps NEON
const char* hw_crypto_mask_var();
const char* hw_crypto_mask_value();

// Whether the *process* should take its hardware crypto paths. The OpenSSL mask
// is read by libcrypto at load time; the implementations written in this
// repository dispatch themselves, so they read the same variable and honour it.
// Without this the masked half of E1 would compare OpenSSL's software path
// against our hardware path, which measures nothing.
bool hw_crypto_masked();

// Measured bulk cost of AES-256-GCM in cycles per byte, probed once at first
// use. Hardware AES lands far below one cycle per byte; the software fallback
// lands above ten. This is how the harness confirms that a capability mask
// actually reached libcrypto, rather than assuming it did.
double openssl_aes_cycles_per_byte();
bool   openssl_hw_aes_active();

} // namespace lr::plat
