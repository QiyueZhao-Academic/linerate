#!/usr/bin/env bash
# setup.sh — configure and build, on either kind of host.
#
# Works on macOS and on Linux. On macOS it produces a build that self-tests and
# typesets; it will not measure, because the harness classifies macOS as a
# development host and refuses to. That refusal is the point: an unpinnable
# thread on a machine with a non-invariant counter can produce a number, and a
# number produced that way is worse than no number at all.
#
# Downloads and any environment the build needs go into ../linerate-env, beside
# the repository rather than inside it, so a clean checkout stays clean.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
ENVDIR="${LR_ENV_DIR:-$(dirname "$ROOT")/linerate-env}"
JOBS="${LR_JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

say "==> host"
UNAME="$(uname -s)"
say "    $UNAME $(uname -m)"

command -v cmake >/dev/null 2>&1 || die "cmake is not installed.
  macOS:  brew install cmake
  Ubuntu: sudo apt-get install cmake"

# OpenSSL 3 is the only hard dependency. macOS ships LibreSSL headers under the
# same name, which compile and then behave differently, so the Homebrew prefix is
# located explicitly rather than left to the linker.
if [ "$UNAME" = "Darwin" ]; then
  if command -v brew >/dev/null 2>&1; then
    SSL="$(brew --prefix openssl@3 2>/dev/null || true)"
    if [ -z "$SSL" ] || [ ! -d "$SSL" ]; then
      say "    installing openssl@3"
      brew install openssl@3
      SSL="$(brew --prefix openssl@3)"
    fi
    export LR_OPENSSL_ROOT="$SSL"
    say "    openssl at $SSL"
  else
    die "Homebrew is not installed, and macOS's own OpenSSL headers are LibreSSL.
  Install Homebrew from https://brew.sh, then: brew install openssl@3"
  fi
else
  [ -f /usr/include/openssl/evp.h ] || [ -f /usr/local/include/openssl/evp.h ] || \
    die "OpenSSL 3 headers not found. Ubuntu: sudo apt-get install libssl-dev"
fi

mkdir -p "$ENVDIR"
say "    environment directory $ENVDIR"

say "==> configure"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release \
  | sed -n '/^-- linerate/,$p' | sed 's/^-- /    /'

say "==> build"
cmake --build "$ROOT/build" -j "$JOBS" 2>&1 | tail -3 | sed 's/^/    /'

say "==> self-test"
# Correctness before anything else. Every cipher against its published vectors,
# the two implementations written here against OpenSSL byte for byte, the replay
# window, the rekey schedule, the handshake, every transport, and the clock.
if ! "$ROOT/build/lr_selftest"; then
  die "the self-test failed. Nothing measured from this build would mean anything."
fi

say ""
say "Built. Next:"
if [ "$UNAME" = "Darwin" ]; then
  say "  ./mac/linerate up        create two VMs and build there"
  say ""
  say "  This Mac will not measure: the harness classifies it as a development"
  say "  host. It builds, drives the VMs, and typesets the report."
else
  say "  ./run.sh --quick         a two-minute smoke run"
  say "  ./run.sh                 the full sweep"
  say "  python3 tools/doctor.py  what this host can and cannot measure"
fi
