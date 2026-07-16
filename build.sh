#!/usr/bin/env bash
# build.sh — reproducible aarch64 build of pf-hwprobe, driven from committed pins.
#
# The pipeline (all steps invoked from THIS script; no manual manual):
#   1. Clone the pinned runtime (E2) + sim (E5).
#   2. Cross-build libpocketforge.a for aarch64 via a stock rust:1.77-bookworm
#      container (--target aarch64-unknown-linux-gnu, staticlib output).
#   3. Cross-build the sim's sdl3-render static libSDL3.a for aarch64 (matches the
#      hwprobe-lite build in sim/Dockerfile: VIDEO+RENDER on, GPU/wayland/x11 off).
#   4. Cross-compile pf-hwprobe.arm64 (Makefile), statically linking both.
#
# Outputs land in ./build/. No global tool installs required beyond `docker` and
# the standard aarch64-linux-gnu cross-gcc (or docker containers for those too if
# you set BUILD_MODE=docker-only). Reproducibility caveats: the SDL3 build resolves
# `apt install cmake ninja-build` from live bookworm suite — same residual gap the
# sim's own Dockerfile calls out; hardened when we snapshot-pin bookworm.
#
# Usage:
#   ./build.sh                # build target: aarch64 (the sim/device arch)
#   ./build.sh --force-refetch   # blow away caches under .cache/
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# shellcheck disable=SC1091
source "$HERE/pins.env"

CACHE_DIR="${CACHE_DIR:-$HERE/.cache}"
BUILD_DIR="${BUILD_DIR:-$HERE/build}"
DEPS_DIR="$BUILD_DIR/deps"
RUNTIME_SRC="$CACHE_DIR/runtime"
SIM_SRC="$CACHE_DIR/sim"

force_refetch=0
for arg in "$@"; do
    case "$arg" in
        --force-refetch) force_refetch=1 ;;
        -h|--help)
            sed -n '2,20p' "$0"; exit 0 ;;
    esac
done

mkdir -p "$CACHE_DIR" "$BUILD_DIR" "$DEPS_DIR"

banner() { printf '\n==== %s ====\n' "$*"; }

clone_pin() {  # $1=repo $2=commit $3=dest
    local repo="$1" commit="$2" dest="$3"
    if [ "$force_refetch" = 1 ]; then rm -rf "$dest"; fi
    if [ ! -d "$dest/.git" ]; then
        banner "clone $(basename "$dest") @ $commit"
        git clone --quiet "$repo" "$dest"
    fi
    (cd "$dest" && git fetch --quiet --tags origin && git -c advice.detachedHead=false checkout --quiet "$commit")
    (cd "$dest" && git rev-parse HEAD)
}

# ------------------------------------------------------------------ 1. sources
clone_pin "$RUNTIME_REPO" "$RUNTIME_COMMIT" "$RUNTIME_SRC" > "$BUILD_DIR/runtime.sha"
clone_pin "$SIM_REPO"     "$SIM_COMMIT"     "$SIM_SRC"     > "$BUILD_DIR/sim.sha"

# platform is consumed READ-ONLY at run time by ci/run-under-sim.py to stage
# capabilities.toml into outdir; pin it here so build + run share one source of truth.
PLATFORM_SRC="$CACHE_DIR/platform"
clone_pin "$PLATFORM_REPO" "$PLATFORM_COMMIT" "$PLATFORM_SRC" > "$BUILD_DIR/platform.sha"

# ------------------------------------------------------------------ 2. libpocketforge.a (cross-Rust)
LIBPF_A="$DEPS_DIR/libpocketforge.a"
if [ ! -f "$LIBPF_A" ] || [ "$force_refetch" = 1 ]; then
    banner "cross-build libpocketforge.a (aarch64-unknown-linux-gnu, staticlib)"
    # HOST_UID/HOST_GID + the final chown: the container runs as root (apt/rustup
    # need it), so anything it writes to /out lands root-owned on the HOST bind
    # mount — which wedged the build-lab runner workdir (actions/checkout EACCES
    # rmdir on stale root-owned deps, bead tsp-u3e4). Hand ownership back before
    # the container exits so ./build.sh never leaves files its caller can't delete.
    docker run --rm \
        -v "$RUNTIME_SRC":/src:ro \
        -v "$DEPS_DIR":/out \
        -e CARGO_HOME=/tmp/cargo \
        -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
        "$RUST_IMAGE" bash -euxc '
            mkdir -p /work && cp -a /src/. /work/
            cd /work
            apt-get update -qq
            apt-get install -y -qq --no-install-recommends gcc-aarch64-linux-gnu libc6-dev-arm64-cross >/dev/null
            rustup target add aarch64-unknown-linux-gnu >/dev/null
            mkdir -p .cargo
            cat > .cargo/config.toml <<EOF
[target.aarch64-unknown-linux-gnu]
linker = "aarch64-linux-gnu-gcc"
EOF
            cargo build --release -p libpocketforge --target aarch64-unknown-linux-gnu
            cp target/aarch64-unknown-linux-gnu/release/libpocketforge.a /out/libpocketforge.a
            aarch64-linux-gnu-nm --defined-only /out/libpocketforge.a | grep -E "\\bT pf_" | wc -l
            chown -R "$HOST_UID:$HOST_GID" /out
        '
    echo "libpocketforge.a: $(file "$LIBPF_A")"
fi

# Publish the header alongside the archive so the Makefile has one -I root.
mkdir -p "$DEPS_DIR/include"
cp -f "$RUNTIME_SRC/include/pocketforge.h" "$DEPS_DIR/include/pocketforge.h"

# ------------------------------------------------------------------ 3. libSDL3.a (arm64 render static)
SDL3_A="$DEPS_DIR/libSDL3.a"
SDL3_INC="$DEPS_DIR/sdl3-include"
if [ ! -f "$SDL3_A" ] || [ "$force_refetch" = 1 ]; then
    banner "cross-build SDL3 arm64 static (sim/fb build recipe)"
    # Reuse the sim's build-sdl3-render.sh — it clones the pinned SDL3, configures
    # video+render+software with every GPU backend off, produces libSDL3.a. We run
    # it in the toolchain container so apt/cmake/cross-gcc are hermetic.
    # Same ownership hand-back as the rust container above (bead tsp-u3e4):
    # /out is a host bind mount and this container runs as root.
    docker run --rm \
        -v "$SIM_SRC":/sim:ro \
        -v "$DEPS_DIR":/out \
        -w /work \
        -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
        debian:bookworm bash -euxc '
            apt-get update -qq
            apt-get install -y -qq --no-install-recommends \
                git ca-certificates cmake ninja-build gcc g++ \
                gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libc6-dev-arm64-cross \
                file >/dev/null
            cp -a /sim/sdl3 sdl3
            cp -a /sim/fb   fb
            OUT=/work/sdl3-render SRC=/work/sdl3-src bash fb/build-sdl3-render.sh
            cp /work/sdl3-render/arm64/lib/libSDL3.a /out/libSDL3.a
            mkdir -p /out/sdl3-include
            cp -a /work/sdl3-render/arm64/include/. /out/sdl3-include/
            chown -R "$HOST_UID:$HOST_GID" /out
        '
    echo "libSDL3.a: $(file "$SDL3_A")"
fi

# ------------------------------------------------------------------ 4. pf-hwprobe.arm64
banner "cross-compile pf-hwprobe (aarch64-linux-gnu-gcc)"
export CC=aarch64-linux-gnu-gcc
export SDL3_INCLUDE="$SDL3_INC"
export PF_INCLUDE="$DEPS_DIR/include"
export SDL3_STATIC="$SDL3_A"
export PF_STATIC="$LIBPF_A"
export OUT_DIR="$BUILD_DIR"
export BIN_NAME="pf-hwprobe.arm64"
make -f Makefile all
file "$BUILD_DIR/pf-hwprobe.arm64"

banner "build ok"
printf '   binary: %s\n' "$BUILD_DIR/pf-hwprobe.arm64"
printf '   libpocketforge.a runtime commit: %s\n' "$(cat "$BUILD_DIR/runtime.sha")"
printf '   sim commit for SDL3: %s\n' "$(cat "$BUILD_DIR/sim.sha")"
