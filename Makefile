# ParallelCrack build file.
#
# The build follows the COMP 380 Lab 4 convention on the Lotus cluster:
#   - compile with the MPI wrapper (mpicc)
#   - OpenMPI (openmpi5) is loaded by default on Lotus
#   - `module load OpenSSL/3` puts openssl/*.h on CPATH and libcrypto on
#     LIBRARY_PATH, so a plain -lcrypto links with no explicit -I/-L
#   - strict warnings as errors, debug symbols, -O3, and SSE4.2 on x86
#   - -rdynamic for readable backtraces (a GNU/Linux link flag)
#
# On Lotus:
#       module load OpenSSL/3
#       make
#
# On macOS (Homebrew, Apple Silicon) for local development, point the build at
# the keg-only OpenSSL. The x86-only and Linux-only flags are dropped
# automatically so the same `make` works here too:
#       make OPENSSL_PREFIX=$(brew --prefix openssl@3)

MPICC ?= mpicc

# -msse4.2 is an x86 instruction-set flag (as in the Lab 4 Makefile). It only
# applies on x86_64 hosts such as Lotus and is omitted elsewhere (e.g. Apple
# Silicon) so the same `make` still builds locally.
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
	ARCHFLAGS := -msse4.2
else
	ARCHFLAGS :=
endif

# -rdynamic (as in the Lab 4 Makefile) is a GNU/Linux link flag for readable
# backtraces; it is only added on Linux hosts like Lotus.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
	RDYNAMIC := -rdynamic
else
	RDYNAMIC :=
endif

# Warnings match the Lab 4 Makefile (-Wall -Wextra -Werror) plus -Wpedantic kept
# from ParallelCrack. The code builds cleanly under all of them.
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Werror
OPT     := -g -O3
CFLAGS  ?= $(CSTD) $(WARN) $(OPT) $(ARCHFLAGS)

# On Lotus, OpenSSL/3 makes -lcrypto resolve with no -I/-L. On macOS, pass
# OPENSSL_PREFIX to point at a non-default (Homebrew) OpenSSL.
OPENSSL_PREFIX ?=
ifneq ($(OPENSSL_PREFIX),)
	CPPFLAGS += -I$(OPENSSL_PREFIX)/include
	LDFLAGS  += -L$(OPENSSL_PREFIX)/lib
endif

CPPFLAGS += -Iinclude
LDFLAGS  += $(RDYNAMIC)
LDLIBS   += -lcrypto -lm

TARGET  := parallelcrack
BUILD   := build

# Every .c under src/ (including src/strategies/) is part of the program.
SRCS := $(wildcard src/*.c) $(wildcard src/strategies/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Launcher used by the integration tests. Override on a cluster, e.g. MPIRUN=srun.
MPIRUN ?= mpirun --oversubscribe

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(MPICC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

# Unit-test binary: links only the pieces it needs (no MPI, but built with mpicc
# so it picks up the same OpenSSL flags).
$(BUILD)/test_search: tests/test_search.c src/search.c src/crypto.c
	@mkdir -p $(BUILD)
	$(MPICC) $(CFLAGS) $(CPPFLAGS) -o $@ \
		tests/test_search.c src/search.c src/crypto.c $(LDFLAGS) $(LDLIBS)

# Run unit tests then integration tests.
test: all $(BUILD)/test_search
	@echo "=== unit tests ==="
	@$(BUILD)/test_search
	@echo
	@echo "=== integration tests ==="
	@MPIRUN="$(MPIRUN)" PROG=./$(TARGET) bash tests/run_tests.sh

# Compile each source to build/..., mirroring the src/ subdirectory layout.
# -MMD -MP writes header dependency files so edits to headers trigger rebuilds.
$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(MPICC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD) $(TARGET)

-include $(DEPS)
