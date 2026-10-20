.DEFAULT_GOAL := help

CC ?= cc ## C compiler.
CFLAGS ?= -O2 ## Extra compiler flags; required C11 warning flags are always added.
PYTHON ?= python3 ## Python interpreter for the metadata-only tests.
WARN_CFLAGS = -std=c11 -Werror=vla -pedantic -Wall -Wextra

.PHONY: help all check clean
help: ## Show available targets and configuration variables.
	@printf 'Usage: make TARGET [NAME=value ...]\n\nTargets:\n'
	@awk -F ':.*##[[:space:]]*' '/^[a-zA-Z0-9_.-]+:.*##/ { printf "  %-12s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@printf '\nVariables (override with NAME=value):\n'
	@awk -F '##[[:space:]]*' '/^[A-Z_]+[[:space:]]+\?=.*##/ { split($$1, v, /[[:space:]]*\?=[[:space:]]*/); sub(/[[:space:]]+$$/, "", v[2]); printf "  %-12s %s (default: %s)\n", v[1], $$2, v[2] }' $(MAKEFILE_LIST)

all: mac2ios ## Build the host metadata converter.

mac2ios: mac2ios.c ar.h macho.h ## Build the converter binary.
	$(CC) $(CFLAGS) $(WARN_CFLAGS) -o $@ mac2ios.c

check: mac2ios ## Check object conversion, version normalization and archive/payload preservation.
	$(PYTHON) tests/test_mac2ios.py ./mac2ios

clean: ## Remove the local tool binary.
	rm -f mac2ios
