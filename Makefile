# pf-hwprobe — thin cross-build Makefile.
#
# The heavy lifting (Rust cross-build of libpocketforge.a, cloning the pinned sim
# for SDL3, cloning platform for descriptors) lives in build.sh; this Makefile is
# a pure C compile step, driven by build.sh with the header/lib paths + arch
# variables already resolved. Run `make -C /path/to/pf-hwprobe help` for detail.

# --- INPUTS (all overridable from build.sh --------------------------------------
# Cross-compiler triple (default = aarch64 static ELF for the sim harness).
CC             ?= aarch64-linux-gnu-gcc
STRIP          ?= aarch64-linux-gnu-strip

# Include roots.
SDL3_INCLUDE   ?= /opt/pf/sdl3-render/arm64/include
PF_INCLUDE     ?= /opt/pf/runtime/include

# Static archives to link.
SDL3_STATIC    ?= /opt/pf/sdl3-render/arm64/lib/libSDL3.a
PF_STATIC      ?= /opt/pf/libpocketforge.a

# Output.
OUT_DIR        ?= build
BIN_NAME       ?= pf-hwprobe

# --- FLAGS ---------------------------------------------------------------------
# -ffp-contract=off matches the sim's app builds — keeps native==qemu float bits
# identical. -static gives a self-contained ELF the harness can bwrap over any
# bookworm-arm64 rootfs without libSDL3.so resolution (SDL3_DYNAMIC_API still
# engages: on the device the DYNAPI env swaps libSDL3-pocketforge.so.0 in).
CFLAGS         ?= -O2 -Wall -Wextra -Wpedantic -std=gnu11 -ffp-contract=off
# third_party/ carries the vendored public-domain stb_image (PNG decode); it lives
# OUTSIDE src/ so the acceptance grep (src/ + include/) stays clean.
CFLAGS         += -I$(SDL3_INCLUDE) -I$(PF_INCLUDE) -Isrc -Ithird_party
LDFLAGS        ?= -static
LIBS           ?= $(SDL3_STATIC) $(PF_STATIC) -lm -ldl -lpthread -lrt

SRC            := src/main.c src/img_decode.c src/widget_actuators.c src/sensor_imu.c
BIN            := $(OUT_DIR)/$(BIN_NAME)

.PHONY: all clean help
all: $(BIN)

$(BIN): $(SRC) | $(OUT_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LIBS)
	@file $@ || true

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

clean:
	rm -rf $(OUT_DIR)

help:
	@echo "pf-hwprobe cross-build (call via ../build.sh; this Makefile is not user-facing)."
	@echo "Overridable vars: CC STRIP SDL3_INCLUDE PF_INCLUDE SDL3_STATIC PF_STATIC OUT_DIR BIN_NAME"
