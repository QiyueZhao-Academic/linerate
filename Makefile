# Convenience targets. Everything here is a thin wrapper around setup.sh, run.sh
# and the tools, so nothing is only reachable through make.

SHELL := /bin/bash
BUILD := build
JOBS  ?= $(shell (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 2)

.PHONY: all build test quick run full figures check clean distclean doctor help

help:
	@echo "make build      configure and compile"
	@echo "make test       build, then run the self-test"
	@echo "make quick      a two-minute run that exercises every path"
	@echo "make run        the core experiments"
	@echo "make figures    the admission model and the figures from an existing dataset"
	@echo "make check      the wiring checks"
	@echo "make doctor     what this host can and cannot measure"
	@echo "make clean      remove the build"
	@echo "make distclean  remove the build and the results"

all: build

build:
	@bash setup.sh

test: build
	@./$(BUILD)/lr_selftest

quick:
	@bash run.sh --quick

run:
	@bash run.sh

full:
	@bash run.sh --full

figures:
	@python3 python/make_figures.py

check:
	@python3 tools/check_wiring.py

doctor:
	@python3 tools/doctor.py

clean:
	@rm -rf $(BUILD)
	@echo "removed $(BUILD)"

distclean: clean
	@rm -rf results
	@echo "removed results"
