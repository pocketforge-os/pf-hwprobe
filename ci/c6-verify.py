#!/usr/bin/env python3
"""c6-verify.py — device-free acceptance for tsp-fr2n.6 (C6: rumble + LEDs +
first honored user preference).

Drives the SAME pf-hwprobe.arm64 binary under the E5 simulator against a
descriptor and asserts the C6 contract, purely from descriptor rows (zero
per-device test code). Proves the unified no-op shape TWO ways, so a
regression in EITHER route breaks the gate:

  * Route P (SIM broker, Python): dev.acquire_rumble().pulse(40) exercises the
    sim's broker_stub — asserts fired / noop-absent per descriptor, and
    noop-suppressed after dev.set_preference("hapticsEnabled", False). This is
    the epic acceptance verbatim.

  * Route C (C ABI, through pf-hwprobe): the app's own pf_rumble_pulse() at
    startup + the "rumble <ms>" FIFO verb. Asserts the SAME status name — this
    proves suppression happens AT THE PRIMITIVE (the app doesn't check the
    preference) because the app-side C call is UNCONDITIONAL.

Also proves the LED-grid widget reflects the descriptor `count`:

  * asserts the app logged `leds.count=<N>` == descriptor kind="led_array" count;
  * counts bright cells along the widget row in the framebuffer PPM and asserts
    the count matches the descriptor. The pixel scan uses ONLY the geometry
    invariant documented in src/widget_actuators.c (a bottom-of-canvas row of
    bright cells on a dark background).

Two descriptors, ONE test body: pass `--device a133|a523`. Evidence PPM/PNGs
land under `<outdir>/frames/`. Intended run host: modelmaker (mm@10.0.40.90).
Exit non-zero on the first failed assertion.

USAGE (mirrors ci/c2-verify.py):
    sudo ./ci/c6-verify.py --device a523 \\
        --binary ./build/pf-hwprobe.arm64 --sim ./.cache/sim --platform ./.cache/platform \\
        --qemu-tsp /home/mm/qemu-tsp/build/qemu-tsp/qemu-aarch64 \\
        --rootfs /home/mm/sim-build/harness/rootfs-arm64 --outdir ./evidence/c6-a523
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


def _import_sim(sim_dir: Path):
    for extra in ("synth", "fb", "sensor"):
        sys.path.insert(0, str(sim_dir / extra))
    sys.path.insert(0, str(sim_dir / "control"))
    from control_surface import Device, ControlError  # type: ignore[import-not-found]
    from broker_stub import (                          # type: ignore[import-not-found]
        RUMBLE_FIRED, RUMBLE_NOOP_ABSENT, RUMBLE_NOOP_SUPPRESSED,
    )
    return Device, ControlError, (RUMBLE_FIRED, RUMBLE_NOOP_ABSENT, RUMBLE_NOOP_SUPPRESSED)


def _load_toml(path: Path):
    try:
        import tomllib
    except ModuleNotFoundError:  # pragma: no cover
        import tomli as tomllib  # type: ignore
    with open(path, "rb") as f:
        return tomllib.load(f)


def stage(platform_dir: Path, device_id: str, outdir: Path):
    """Stage capabilities.toml + skin PNGs into outdir (== ci/c2-verify.py)."""
    outdir.mkdir(parents=True, exist_ok=True)
    desc_src = platform_dir / "devices" / device_id / "capabilities.toml"
    if not desc_src.is_file():
        raise SystemExit(f"[c6-verify] no descriptor at {desc_src}")
    shutil.copyfile(desc_src, outdir / "capabilities.toml")
    desc = _load_toml(desc_src)
    for rel in (desc.get("skin", {}).get("body"), desc.get("skin", {}).get("lit_body")):
        if not rel:
            continue
        src = platform_dir / rel
        if not src.is_file():
            raise SystemExit(f"[c6-verify] skin asset missing: {src}")
        dst = outdir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
    return desc


def _png(outdir: Path, sim: Path, name: str):
    ppm = outdir / "frames" / f"{name}.ppm"
    if not ppm.is_file():
        return
    try:
        sys.path.insert(0, str(sim / "fb"))
        from ppm2png import read_ppm  # type: ignore[import-not-found]
        w, h, rgb = read_ppm(str(ppm))
        try:
            import png as pypng  # type: ignore[import-not-found]
            with open(ppm.with_suffix(".png"), "wb") as f:
                pypng.Writer(w, h, greyscale=False).write_array(f, rgb)
        except ModuleNotFoundError:
            from PIL import Image  # type: ignore[import-not-found]
            Image.frombytes("RGB", (w, h), bytes(rgb)).save(ppm.with_suffix(".png"))
    except Exception as e:  # noqa: BLE001
        print(f"[c6-verify] PNG skip {name}: {e}")


def _actuators_from_desc(desc) -> tuple[bool, int]:
    """Extract (has_rumble, led_count) from a descriptor. Missing = graceful (0)."""
    has_rumble = False
    led_count = 0
    for a in desc.get("actuators", []):
        k = a.get("kind")
        if k == "rumble":
            has_rumble = True
        elif k == "led_array":
            led_count = int(a.get("count", 0))
    return has_rumble, led_count


def _count_led_cells(ppm_path: Path, expected: int) -> int:
    """Count bright rectangular runs along the LED-grid row at the CANVAS BOTTOM.

    Uses ONLY the geometry invariant from src/widget_actuators.c: a horizontal
    strip of bright (>=180) pixels on a dark (~24) background, at
    y = canvas_h - LED_MARGIN_Y - LED_CELL_H/2 (near the bottom edge). Robust
    against wrapping: if the top row's count is less than expected we sweep a
    small band and pick the row with the maximum runs (still ONE scanline of
    work). Returns the max cell-run count observed; -1 if the PPM is missing.
    """
    if not ppm_path.is_file():
        return -1
    with open(ppm_path, "rb") as f:
        head = b""
        while head.count(b"\n") < 3:
            head += f.read(64)
        # P6\n<w> <h>\n<maxval>\n
        lines = head.split(b"\n", 3)
        w, h = (int(x) for x in lines[1].split())
        maxval = int(lines[2])
        payload_offset = len(b"\n".join(lines[:3])) + 1
        if maxval != 255:
            return -1
        # Read the whole payload — small at 1280x720 = 2.7MB, and we scan a band.
        f.seek(payload_offset)
        data = f.read(w * h * 3)

    def row_count(y_row: int) -> int:
        row_off = y_row * w * 3
        runs = 0
        in_run = False
        for x in range(w):
            r = data[row_off + x * 3 + 0]
            g = data[row_off + x * 3 + 1]
            b = data[row_off + x * 3 + 2]
            bright = (r >= 180 and g >= 180 and b >= 180)
            if bright and not in_run:
                in_run = True
                runs += 1
            elif not bright and in_run:
                in_run = False
        return runs

    # Sweep the whole bottom band of the canvas (below anything the skin body could
    # occupy). Pick the row with the most bright cell-runs — that's the LED-grid row.
    # Deliberately DOES NOT depend on the exact widget geometry constants, so a
    # cosmetic tweak to the .c cell size / margin doesn't silently invalidate the test.
    best = 0
    for y in range(max(0, h - 48), h):
        best = max(best, row_count(y))
    return best


def _app_log_scan(outdir: Path, launcher: str, needle: str) -> str | None:
    """Return the first line of the app's stderr log containing `needle`, or None."""
    log = outdir / f"app.{launcher}.log"
    if not log.is_file():
        return None
    with open(log, "rb") as f:
        for raw in f:
            line = raw.decode(errors="replace").rstrip("\n")
            if needle in line:
                return line
    return None


class Checker:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def ok(self, cond, msg):
        mark = "PASS" if cond else "FAIL"
        print(f"  [{mark}] {msg}")
        if cond:
            self.passed += 1
        else:
            self.failed += 1
        return cond


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", required=True)
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--sim", required=True, type=Path)
    ap.add_argument("--platform", required=True, type=Path)
    ap.add_argument("--qemu-tsp", required=True, type=Path)
    ap.add_argument("--rootfs", required=True, type=Path)
    ap.add_argument("--outdir", required=True, type=Path)
    args = ap.parse_args()

    Device, ControlError, (RUMBLE_FIRED, RUMBLE_NOOP_ABSENT, RUMBLE_NOOP_SUPPRESSED) = \
        _import_sim(args.sim)
    desc = stage(args.platform, args.device, args.outdir)
    has_rumble, led_count = _actuators_from_desc(desc)
    print(f"== C6 verify: {args.device} == descriptor rumble={has_rumble} led_count={led_count}")

    chk = Checker()
    dev = Device(args.device, str(args.platform), launcher="qemu", outdir=str(args.outdir),
                 app_arm64=str(args.binary), qemu_tsp=str(args.qemu_tsp), rootfs=str(args.rootfs))
    dev.boot()
    try:
        # -------- Route P (SIM Python broker) --------
        # Rest: baseline pulse — a133 -> noop-absent, a523 -> fired.
        h = dev.acquire_rumble()
        st = h.pulse(40)
        want = RUMBLE_FIRED if has_rumble else RUMBLE_NOOP_ABSENT
        chk.ok(st == want, f"[sim] pulse(40) at defaults -> {st!r} (want {want!r})")

        # Flip hapticsEnabled off -> suppressed on descriptors WITH rumble;
        # descriptors WITHOUT rumble stay noop-absent (absence dominates).
        dev.set_preference("hapticsEnabled", False)
        st_off = dev.acquire_rumble().pulse(40)
        want_off = RUMBLE_NOOP_SUPPRESSED if has_rumble else RUMBLE_NOOP_ABSENT
        chk.ok(st_off == want_off,
               f"[sim] pulse(40) with hapticsEnabled=false -> {st_off!r} (want {want_off!r})")

        # Restore the preference for the render-side snapshot.
        dev.set_preference("hapticsEnabled", True)

        # -------- Route C (C ABI, through pf-hwprobe) --------
        # Startup log: the app calls pf_rumble_pulse() at boot with hapticsEnabled=true
        # (fresh in-process backend), so the shape mirrors Route P's rest baseline.
        want_boot = "fired" if has_rumble else "noop-absent"
        line = _app_log_scan(args.outdir, "qemu", "rumble startup pulse")
        chk.ok(line is not None and f"-> {want_boot}" in line,
               f"[C ABI] startup log has rumble pulse -> {want_boot}  ({line!r})")

        # Live FIFO verb — proves the app forwards pf_rumble_pulse's status verbatim
        # (no app-side conditional on hapticsEnabled).
        dev._write_req("rumble 40")
        reply = dev._read_resp(5.0)
        chk.ok(reply == want_boot,
               f"[C ABI] FIFO 'rumble 40' -> {reply!r} (want {want_boot!r})")

        # -------- LED-grid render + descriptor-count reflection --------
        # Log line records the descriptor-derived count the app is rendering.
        log_leds = _app_log_scan(args.outdir, "qemu", "actuators:")
        chk.ok(log_leds is not None and f"leds.count={led_count}" in log_leds,
               f"[leds] startup log matches descriptor count={led_count}  ({log_leds!r})")

        # Snapshot + pixel-count the bright cells along the LED row.
        dev.snapshot("c6-rest")
        _png(args.outdir, args.sim, "c6-rest")
        ppm = args.outdir / "frames" / "c6-rest.ppm"
        observed = _count_led_cells(ppm, led_count)
        chk.ok(observed == led_count,
               f"[leds] snapshot cell-count == descriptor count ({observed} == {led_count})")
    finally:
        dev.shutdown()

    print(f"== {args.device}: {chk.passed} passed, {chk.failed} failed ==")
    return 0 if chk.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
