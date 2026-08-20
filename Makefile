# Convenience targets. Everything here is a thin wrapper around setup.sh, run.sh
# and the tools, so nothing is only reachable through make.

SHELL := /bin/bash
BUILD := build
JOBS  ?= $(shell (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 2)

.PHONY: all build test quick run full report check clean distclean doctor help

help:
	@echo "make build      configure and compile"
	@echo "make test       build, then run the self-test"
	@echo "make quick      a two-minute run that exercises every path"
	@echo "make run        the core experiments"
	@echo "make report     figures, macros and the PDF from an existing dataset"
	@echo "make check      the wiring and numerical consistency checks"
	@echo "make doctor     what this host can and cannot measure"
	@echo "make clean      remove the build"
	@echo "make distclean  remove the build, the results and the generated report"

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

report:
	@python3 python/make_report.py

check:
	@python3 tools/check_wiring.py
	@python3 tools/check_numbers.py

doctor:
	@python3 tools/doctor.py

clean:
	@rm -rf $(BUILD)
	@echo "removed $(BUILD)"

distclean: clean
	@rm -rf results report/generated report/figures
	@rm -f report/main.pdf report/*.aux report/*.log report/*.out \
	       report/*.fls report/*.fdb_latexmk report/*.bbl report/*.blg
	@echo "removed results and the generated report"
