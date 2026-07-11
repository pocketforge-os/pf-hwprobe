# pf-hwprobe

**PocketForge's gamepad-native hardware hello-world diagnostic** — the first first-party
app that is *not* Steam Link, and the first real consumer of the PocketForge capability layer.

It draws a virtual representation of the **exact device being held**, lights each control
**red on press**, shows a **slider-with-marker** above each analog/variable trigger tracking
travel, a **tilt-bubble** for the accelerometer, and visualizes every input / sensor /
actuator (sticks, dpad, LED rings, rumble). It ships for **both** the TrimUI Smart Pro
(`a133`) and Smart Pro S (`a523`) from day one with **zero per-device code** — it renders and
binds **purely from the E1 `capabilities.toml` descriptor**, so adding a device is a *data*
change, not a code change.

## Architecture in one line
`capabilities.toml` (E1 descriptor) → the E2 capability facade (`libpocketforge` C ABI /
`pocketforge` Rust crate) → SDL3 (`libSDL3-pocketforge.so.0`, bound via `SDL3_DYNAMIC_API`)
→ fb0. The app never opens `/dev/*` with ambient authority and never names a device-specific
evdev code or IIO device in source.

## Where this lives in the plan
- **Epic:** E6 / `tsp-fr2n` (beads); kickoff & design: `mission-control/.planning/infra/infra-105-hardware-hello-world-app.md`.
- **Consumes (read-only):** E1 descriptors (`pocketforge-os/platform` `devices/<id>/capabilities.toml`),
  E2 runtime (`pocketforge-os/runtime`), and develops off-hardware against the E5 simulator
  (`pocketforge-os/sim`).
- **Ships:** first-party **in the image** (no `[fetch]`), as a built-in OCI/app bundle.

## Contributing
Branch → PR → merge (no direct pushes to `main`); every non-draft PR carries the three
checklist sections (Summary / Test plan / Related PRs) and is gated by `pf-pr-review`.
Develop and demo each capability against the E5 simulator before the hardware gate.

## C1 skeleton (this bead — `tsp-fr2n.1`)

The current source is the **descriptor-wiring skeleton**: `src/main.c` opens a
software-render off-screen framebuffer, connects the E2 runtime via
`pf_connect_descriptor()`, reads the sim's descriptor-computed `layout.txt`,
renders **one stub rect per control group** (a133 gets 12 groups; a523 gets 13 —
Home is the delta, entirely from the descriptor), and speaks the E5 sim's
`ready` / `snap <ppm>` / `quit` FIFO protocol.

## C2 — body render + buttons, both devices + tsp-osr recipe (`tsp-fr2n.2`)

On top of the C1 skeleton, C2 adds the **real clickable-skin render** and the button
binding for BOTH devices, and PINS the tsp-osr renderer recipe:

- **Skin render** — draws `skin.body` (the traced device body PNG) into the landscape
  canvas and, on press, composites the `skin.lit_body` overlay sprite for the pressed
  control's `[skin.parts].<id>` rect (a translucent red active-highlight guarantees the
  control reads unambiguously RED at the reference point the headline `is_red()` contract
  samples, since the face-button glyphs are dark at that exact centre). The uniform
  `active -> lit overlay` rule lights buttons, the a523 Home key, and the a523 L3/R3
  stick-clicks straight from descriptor rows with ZERO per-device code. `render_frame()`
  falls back to the C1 stub rects when the skin PNGs are unavailable. PNG decode is the
  vendored public-domain **stb_image** under `third_party/` (self-contained, no libz),
  behind the `src/img_decode.*` seam.
- **Input via the facade** — input is acquired THROUGH the E2 facade
  (`pf_acquire("input")` — the authorization gate) and read from a single swappable seam
  `input_fd_for()`. NOTE: the frozen v1 C ABI has no fd-returning input export, so under
  the sim the seam reads the platform-provided `layout.txt` node path (never app-scanned);
  the on-device facade fd export is tracked as **`tsp-e1b.10`** (E2) and swapped in at C8.
- **tsp-osr recipe** — `pin_tsp_osr_recipe()` documents + smoke-checks the safe
  renderer-creation recipe; the owned-source Fix A lands in `libsdl3-sunxifb`. Recorded on
  the `tsp-osr` bead. On-panel proof on real PowerVR is the C8 hardware gate.

C3/C4 layer the richer proportional-trigger + directional stick/hat widgets on top of
this foundation. Verify with `ci/c2-verify.py --device {a133,a523}` (below).

The **later body widgets** (slider-with-marker, hat cross, stick calibration box,
tilt-bubble, LED grid) build on this render in C3–C6.

### Grep-clean promise

The source names ZERO per-device evdev symbol (no `BTN_*` / `KEY_*` / `ABS_*`)
and ZERO per-device string (no `qmi8658` / `pwm-vibrator` / `sunxi_led` / etc.)
The vendored PNG decoder lives under `third_party/` (outside the `src/` + `include/`
grep scope) behind the `img_decode` seam, so the acceptance grep stays clean.
Every code, every rect, every widget kind enters through `layout.txt`, which is
derived from `capabilities.toml`. The only ABI constants baked in are the three
generic evdev event-type numbers (`SYN=0` / `KEY=1` / `ABS=3`).

## Build (`./build.sh`)

Reproducible aarch64 cross-build, driven from pinned refs in [`pins.env`](pins.env):

1. Clone `runtime` (E2) + `sim` (E5) at their pinned commits (cached under
   `.cache/`).
2. Cross-build `libpocketforge.a` for `aarch64-unknown-linux-gnu` inside a stock
   `rust:1.77-bookworm` container (staticlib output from the ABI-frozen v1
   crate).
3. Cross-build the sim's `sdl3-render` static `libSDL3.a` for aarch64 inside a
   `debian:bookworm` toolchain container (matches the same recipe the sim's
   `hwprobe-lite.c` builds against — video + software renderer on, all GPU /
   X11 / Wayland / audio backends off).
4. Cross-compile `build/pf-hwprobe.arm64` via `aarch64-linux-gnu-gcc`, static-
   linking both archives (the [`Makefile`](Makefile) is the compile step).

Run host: **modelmaker** (`mm@10.0.40.90`; per the epic ruling this bead is
device-free). The build takes ~5–10 min cold and ~30 s warm.

## Verify (device-free, under the E5 sim)

### Rest-state boot proof (`ci/run-under-sim.py`)

The C1 skeleton's proof: drive the SAME binary against BOTH descriptors — the
a133/a523 delta comes purely from the loaded `capabilities.toml`:

```sh
./ci/run-under-sim.py \
    --device a133 \
    --binary   ./build/pf-hwprobe.arm64 \
    --sim      ./.cache/sim \
    --platform ./.cache/platform \
    --qemu-tsp /home/mm/qemu-tsp/build/qemu-tsp/qemu-aarch64 \
    --rootfs   /home/mm/sim-build/harness/rootfs-arm64 \
    --outdir   ./evidence/a133
# ... and again with --device a523 --outdir ./evidence/a523
```

The wrapper stages `platform/devices/<id>/capabilities.toml` into `--outdir`
before the sim harness bwrap-binds it as `/out`, so the app finds the descriptor
at `/out/capabilities.toml` and passes it to `pf_connect_descriptor()`. Evidence
lands under `evidence/<device>/frames/*.{ppm,png}`.

### Full descriptor-driven matrix (`ci/check-control-hwprobe.py`, C7)

The E7 CI smoke gate — drives the honest, descriptor-generic regression matrix
from `sim/control/check-control.py` against pf-hwprobe (NOT the reference
`hwprobe-lite`). For BOTH descriptors, with ZERO app/test-code difference:

* every `[[inputs]]` row: inject the event → assert the control lit
  (`Region.is_red()`);
* every trigger: sweep `min..max` → assert the marker tracks (`Slider.at(f)`);
* every ABSENT control (a133 home/l3/r3; both-device IMU): assert typed
  `hardware-absent`, NEVER a crash;
* framebuffer → PPM → deterministic sampling is the authoritative readback
  (per the `tsp-visual-inspection` hallucination caveat); the reviewer script
  `ci/screen-reviewer-readback.py` is a complementary text-only visual check.

```sh
./ci/check-control-hwprobe.py \
    --binary   ./build/pf-hwprobe.arm64 \
    --sim      ./.cache/sim \
    --platform ./.cache/platform \
    --qemu-tsp /home/mm/qemu-tsp/build/qemu-tsp/qemu-aarch64 \
    --rootfs   /home/mm/sim-build/harness/rootfs-arm64 \
    --outdir   ./evidence/matrix
```

Exit 0 = matrix PASS on every requested descriptor. Native-vs-qemu byte
parity is SKIPPED for now — pf-hwprobe currently only cross-builds arm64; the
transcript prints `parity SKIPPED` rather than faking a green tick (adding an
x86 build step to `build.sh` restores the parity leg).

The matching CI caller is `.github/workflows/hwprobe-smoke.yml`. It builds
pf-hwprobe.arm64, builds the sim's pinned `pocketforge-sim` container, then
runs the matrix inside it with `pf-hwprobe.arm64` bound at `APP_ARM64`.
Advisory-first (same posture as `pocketforge-os/sim`'s `sim-gate.yml`).

### Grep test (part of acceptance)

```sh
grep -REn 'BTN_|KEY_|ABS_[A-Z]|qmi8658|mmc5603|pwm-vibrator|sunxi_led|ledc|KEY_HOMEPAGE' src/ include/ 2>/dev/null || echo "clean"
```

Expected: `clean`. (Bare `EV_KEY=1` / `EV_ABS=3` / `EV_SYN=0` are generic
kernel-ABI type numbers and are named as local enum members
(`EV_TYPE_KEY` / `EV_TYPE_ABS` / `EV_TYPE_SYN`), not the underscored
per-device symbols the grep pattern looks for.)

## Licensing

MIT — matches the sibling `runtime`, `sim`, and `platform` repos. See
[`LICENSE`](LICENSE).
