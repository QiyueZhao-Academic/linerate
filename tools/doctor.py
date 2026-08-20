#!/usr/bin/env python3
"""Report what this machine can and cannot measure, and why.

Run before anything else. The point is not to pass or fail but to say, for each
capability the study depends on, whether it is present and what its absence
costs — so that an operator knows in advance which experiments will record
themselves as unavailable rather than discovering it after an hour of sweeping.

    tools/doctor.py            human-readable
    tools/doctor.py --json     machine-readable, for the Mac driver
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys

OK, WARN, BAD = "ok", "warn", "missing"


class Report:
    def __init__(self) -> None:
        self.items: list[dict] = []

    def add(self, name: str, status: str, detail: str, consequence: str = "") -> None:
        self.items.append({"name": name, "status": status,
                           "detail": detail, "consequence": consequence})

    def worst(self) -> str:
        if any(i["status"] == BAD for i in self.items):
            return BAD
        if any(i["status"] == WARN for i in self.items):
            return WARN
        return OK


def _read(path: str) -> str:
    try:
        with open(path) as fh:
            return fh.read().strip()
    except OSError:
        return ""


def _cpuinfo_flags() -> set[str]:
    text = _read("/proc/cpuinfo")
    for line in text.splitlines():
        if line.startswith("flags") or line.startswith("Features"):
            return set(line.split(":", 1)[1].split())
    return set()


def check_toolchain(r: Report) -> None:
    for tool, why in [("cmake", "the build"),
                      ("make", "the build"),
                      ("git", "recording which commit produced a dataset")]:
        path = shutil.which(tool)
        r.add(tool, OK if path else BAD, path or "not found",
              "" if path else f"required for {why}")
    cxx = shutil.which("g++") or shutil.which("clang++")
    r.add("C++ compiler", OK if cxx else BAD, cxx or "not found",
          "" if cxx else "nothing can be built")
    for tool, consequence in [
        ("latexmk", "the report cannot be typeset; the dataset is still produced"),
        ("mpicxx", "the cross-node scaling experiment is skipped"),
        ("nvcc", "the GPU experiment is skipped"),
        ("iperf3", "the network baseline is skipped"),
        ("wg", "the WireGuard baseline is skipped"),
        ("docker", "the container isolation tier is skipped"),
        ("runsc", "the gVisor isolation tier is skipped"),
        ("tc", "network conditions cannot be injected; E5 runs clean only"),
    ]:
        path = shutil.which(tool)
        r.add(tool, OK if path else WARN, path or "not found",
              "" if path else consequence)


def check_openssl(r: Report) -> None:
    exe = shutil.which("openssl")
    if not exe:
        r.add("openssl", WARN, "not found",
              "the independent per-byte reference in E6 is skipped")
        return
    try:
        out = subprocess.run([exe, "version"], capture_output=True, text=True,
                             timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        out = ""
    major_ok = out.startswith("OpenSSL 3")
    r.add("openssl", OK if major_ok else WARN, out or "unknown",
          "" if major_ok else "this project targets OpenSSL 3; "
                              "older releases expose a different EVP surface")
    # Headers, which is what the build actually needs.
    have_hdr = any(os.path.exists(p) for p in
                   ("/usr/include/openssl/evp.h",
                    "/usr/local/include/openssl/evp.h",
                    "/opt/homebrew/opt/openssl@3/include/openssl/evp.h",
                    "/usr/local/opt/openssl@3/include/openssl/evp.h"))
    r.add("openssl headers", OK if have_hdr else BAD,
          "found" if have_hdr else "not found",
          "" if have_hdr else "install libssl-dev, or brew install openssl@3")


def check_cpu(r: Report) -> None:
    flags = _cpuinfo_flags()
    arch = platform.machine()
    r.add("architecture", OK, arch)

    if arch in ("x86_64", "amd64"):
        want = {"aes": "hardware AES", "pclmulqdq": "the GHASH accelerator",
                "avx2": "the vectorised ChaCha20 path"}
        for flag, what in want.items():
            present = flag in flags
            r.add(f"cpu:{flag}", OK if present else WARN,
                  "present" if present else "absent",
                  "" if present else f"{what} is unavailable; the masked arm of "
                                     "E1 has nothing to contrast against")
        inv = "constant_tsc" in flags and "nonstop_tsc" in flags
        r.add("invariant counter", OK if inv else BAD,
              "constant_tsc and nonstop_tsc" if inv else "not advertised",
              "" if inv else "cycle counts cannot be trusted on this host")
    elif arch in ("arm64", "aarch64"):
        r.add("cpu:crypto", OK if ("aes" in flags or "pmull" in flags) else WARN,
              "ARMv8 crypto extensions" if "aes" in flags else "not advertised")
    else:
        r.add("cpu", WARN, f"unrecognised architecture {arch}",
              "the study has been validated on x86-64 and aarch64 only")

    cores = os.cpu_count() or 0
    r.add("logical CPUs", OK if cores >= 2 else WARN, str(cores),
          "" if cores >= 2 else
          "with one usable core the load generator competes with the data plane, "
          "so the latency experiment measures scheduling rather than queueing")


def check_kernel(r: Report) -> None:
    if platform.system() != "Linux":
        r.add("host class", WARN, platform.system(),
              "this host is 'development': it builds, self-tests and typesets, "
              "but the harness will refuse to write a measurement from it")
        return
    cmdline = _read("/proc/cmdline")
    for key, what in [("isolcpus", "data-plane cores are not isolated"),
                      ("nohz_full", "the tick is not suppressed on the data-plane cores"),
                      ("rcu_nocbs", "RCU callbacks still run on the data-plane cores")]:
        present = key in cmdline
        r.add(f"kernel:{key}", OK if present else WARN,
              "set" if present else "not set",
              "" if present else f"{what}; the host is 'constrained' rather than "
                                 "'measurement' and its datasets say so")
    gov = _read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
    r.add("cpufreq governor", OK if gov == "performance" else WARN,
          gov or "unavailable",
          "" if gov == "performance" else
          "a scaling governor moves the clock under the measurement; the counter "
          "is invariant so cycle counts stay meaningful, but wall-clock rates move")
    boost = _read("/sys/devices/system/cpu/intel_pstate/no_turbo")
    if boost:
        r.add("turbo", OK if boost == "1" else WARN,
              "disabled" if boost == "1" else "enabled",
              "" if boost == "1" else "turbo makes the first replicate of a point "
                                      "faster than the rest")
    # io_uring, which E3's most interesting row depends on.
    disabled = _read("/proc/sys/kernel/io_uring_disabled")
    if disabled and disabled != "0":
        r.add("io_uring", BAD, f"io_uring_disabled={disabled}",
              "the kernel-bypass bound in E3 cannot be measured on this host")
    else:
        r.add("io_uring", OK, "permitted" if disabled == "0" else "not restricted")


def check_python(r: Report) -> None:
    r.add("python", OK, sys.version.split()[0])
    for mod, consequence in [("numpy", "no analysis at all"),
                             ("matplotlib", "no figures"),
                             ("scipy", "nothing: scipy is optional here"),
                             ("sklearn", "the admission-control experiment is skipped")]:
        try:
            __import__(mod)
            r.add(f"python:{mod}", OK, "importable")
        except ImportError:
            r.add(f"python:{mod}", WARN if mod in ("scipy", "sklearn") else BAD,
                  "not installed", consequence)


def _ifaces_from_sysfs() -> list[tuple[str, str]]:
    """The same enumeration the harness does, without iproute2.

    lr_core reads interfaces with getifaddrs(3), which needs nothing installed.
    Doing it here through `ip` alone meant that on a host without iproute2 this
    script reported no interfaces while the harness on the same host reported
    eth0 — two tools in one repository disagreeing about the machine, which is
    the drift everything else here is arranged to prevent.
    """
    import fcntl
    import socket
    import struct

    SIOCGIFADDR = 0x8915
    found: list[tuple[str, str]] = []
    try:
        names = sorted(os.listdir("/sys/class/net"))
    except OSError:
        return found
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for name in names:
            if name == "lo":
                continue
            try:
                packed = struct.pack("256s", name[:15].encode())
                addr = socket.inet_ntoa(
                    fcntl.ioctl(sock.fileno(), SIOCGIFADDR, packed)[20:24])
            except OSError:
                continue          # no IPv4 address on this interface
            found.append((name, addr))
    finally:
        sock.close()
    return found


def check_network(r: Report) -> None:
    if platform.system() != "Linux":
        return
    ifaces: list[tuple[str, str]] = []
    try:
        out = subprocess.run(["ip", "-o", "-4", "addr", "show"],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        out = ""
    for line in out.splitlines():
        parts = line.split()
        if len(parts) > 3 and parts[1] != "lo":
            ifaces.append((parts[1], parts[3].split("/")[0]))
    if not ifaces:
        ifaces = _ifaces_from_sysfs()
    if not ifaces:
        r.add("interface", WARN, "only loopback",
              "the wire experiments cannot run; everything falls back to loopback "
              "and records that it did")
        return
    name, addr = ifaces[0]
    driver = ""
    try:
        driver = os.path.basename(os.readlink(f"/sys/class/net/{name}/device/driver"))
    except OSError:
        driver = "unknown"
    r.add("interface", OK, f"{name} {addr} ({driver})",
          "" if driver == "gve" else
          f"driver is {driver}; the two-VM setup this study describes uses gVNIC "
          "(gve), and a different driver changes the fixed per-packet cost")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    r = Report()
    check_toolchain(r)
    check_openssl(r)
    check_cpu(r)
    check_kernel(r)
    check_python(r)
    check_network(r)

    if args.json:
        print(json.dumps({"status": r.worst(), "items": r.items}, indent=2))
        return 0

    mark = {OK: "  ok  ", WARN: " warn ", BAD: "MISSING"}
    width = max(len(i["name"]) for i in r.items)
    for i in r.items:
        print(f"[{mark[i['status']]}] {i['name']:<{width}}  {i['detail']}")
        if i["consequence"]:
            print(f"{'':<{width + 11}}{i['consequence']}")
    print()
    verdict = {
        OK: "This host can run the full study.",
        WARN: "This host can measure. Some experiments will record themselves as "
              "unavailable, with the reasons above.",
        BAD: "Something required is missing. Fix the MISSING lines before "
             "running the sweep.",
    }[r.worst()]
    print(verdict)
    return 0 if r.worst() != BAD else 1


if __name__ == "__main__":
    sys.exit(main())
