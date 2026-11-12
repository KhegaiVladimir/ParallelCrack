# ParallelCrack build file.
#
# Requirements:
#   - An MPI C compiler (mpicc). Open MPI and MPICH both work.
#   - OpenSSL 3 development headers and libcrypto.
#
# On Lotus:
#       module load OpenSSL/3
#       make
#   OpenSSL/3 puts openssl/*.h on the include path and libcrypto on the library
#   path, so no explicit -I/-L is needed there.
#
# On macOS with Homebrew (OpenSSL is "keg-only", so point the build at it):
#       make OPENSSL_PREFIX=$(brew --prefix openssl@3)

MPICC ?= mpicc

# Warnings are intentionally strict: the assignment and good practice both ask
# for clean builds under -Wall -Wextra -Wpedantic.
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic
OPT     := -O2
CFLAGS  ?= $(CSTD) $(WARN) $(OPT)

# Let the caller point us at a non-default OpenSSL (needed on macOS/Homebrew).
OPENSSL_PREFIX ?=
ifneq ($(OPENSSL_PREFIX),)
	CPPFLAGS += -I$(OPENSSL_PREFIX)/include
	LDFLAGS  += -L$(OPENSSL_PREFIX)/lib
endif

CPPFLAGS += -Iinclude
LDLIBS   += -lcrypto -lm

TARGET  := parallelcrack
BUILD   := build

# Every .c under src/ (including src/strategies/) is part of the program.
SRCS := $(wildcard src/*.c) $(wildcard src/strategies/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(MPICC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

# Compile each source to build/..., mirroring the src/ subdirectory layout.
# -MMD -MP writes header dependency files so edits to headers trigger rebuilds.
$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(MPICC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD) $(TARGET)

-include $(DEPS)
