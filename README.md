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

> Status: scaffolding. Application source lands via E6 child beads `tsp-fr2n.1` (skeleton) onward.
> Licensing: TBD (owner to confirm the org convention before first release).
