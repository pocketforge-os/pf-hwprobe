#!/usr/bin/env python3
"""check-imu.py — C5 (tsp-fr2n.5) sensor + tilt-bubble matrix.

Complements ``ci/check-control-hwprobe.py`` (the E7 regression matrix, which asserts
the input/render/capability matrix on both shipping descriptors) by proving the
CAPABILITY the shipping devices happen not to advertise: sensor→tilt-bubble.

Two legs, SAME ARM64 BINARY, ZERO source change between them:

  * SYNTHETIC leg (``tests/fixtures/platform-synth/devices/synth_imu``):
    a fixture descriptor with an ``[[sensors]]`` imu row + a non-identity
    ``mount_matrix`` (90° rotation about Z) + ``[skin.parts].imu``. The sim's virtual
    IIO (``sim.sensor.iio_synth.IIOSynth``) inverse-mounts the physical model's device-
    frame accel into a chip-frame raw and writes it to the synthesized sysfs tree; the
    app reads the raw, applies THIS descriptor's mount_matrix, and reports the device
    frame back through the ``imu <name>`` FIFO verb. If the app skipped the mount step
    the reply would be rotated 90° — the assertions below catch it. And the ``snap``
    frame shows a bubble displaced from centre in the ``[skin.parts].imu`` rect.

  * OMISSION leg (``<platform>/devices/a133`` + ``<platform>/devices/a523``):
    NEITHER shipping descriptor carries an ``[[sensors]]`` row (a133 has none; a523's
    qmi8658 is DT-present but driver-UNBOUND per SPIKE-0). ``dev.broker.is_present("imu")``
    returns False, the ``imu`` FIFO verb replies ``imu-absent``, and the snap frame
    shows NO bubble in the (absent) rect. NEVER a crash.

USAGE
-----
::

    ./ci/check-imu.py \\
        --binary   ./build/pf-hwprobe.arm64 \\
        --sim      ./.cache/sim \\
        --platform ./.cache/platform \\
        --qemu-tsp /opt/pf/qemu-tsp/qemu-aarch64 \\
        --rootfs   /opt/pf/rootfs-arm64 \\
        --outdir   ./evidence/c5-imu

Exit 0 = both legs PASS. Exit non-zero on any assertion failure.
"""
from __future__ import annotations

import argparse
import math
import os
import shutil
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
FIXTURE = REPO / "tests" / "fixtures" / "platform-synth"


def _import_sim(sim_dir: Path):
    """Wire sim's Python path the way sim itself does."""
    for extra in ("synth", "fb", "sensor"):
        sys.path.insert(0, str(sim_dir / extra))
    sys.path.insert(0, str(sim_dir / "control"))
    from control_surface import Device, ControlError, HardwareAbsent   # noqa: E402
    return Device, ControlError, HardwareAbsent


def _read_ppm(path: Path):
    """Parse a P6 PPM (matches sim/fb/ppm2png.read_ppm's shape). Returns (w, h, rgb)."""
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P6":
            raise SystemExit(f"[check-imu] not a P6 PPM: {path}")
        # skip comments
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = (int(x) for x in line.strip().split())
        maxv = int(f.readline().strip())
        if maxv != 255:
            raise SystemExit(f"[check-imu] unexpected PPM maxval {maxv}")
        rgb = f.read(w * h * 3)
    return w, h, rgb


class Checker:
    def __init__(self, tag):
        self.tag = tag
        self.fails = []

    def chk(self, cond, msg):
        line = ("  ok  " if cond else "FAIL  ") + f"[{self.tag}] {msg}"
        print(line)
        if not cond:
            self.fails.append(msg)
        return cond


# ---------- SYNTHETIC LEG ---------------------------------------------------

def _stage_synth_skins(fixture_dir: Path):
    """Assert the fixture's skin PNGs actually landed (they are versioned copies of the
    a133 skin so run-under-sim's stage_skin machinery is exercised identically)."""
    body = fixture_dir / "skins" / "synth_imu" / "body.png"
    litb = fixture_dir / "skins" / "synth_imu" / "body_lit.png"
    for p in (body, litb):
        if not p.is_file():
            raise SystemExit(f"[check-imu] fixture skin missing: {p}")


def _find_red_dot_center(rgb, w, h, rect):
    """Return (cx, cy, n_red) — centre of gravity of RED pixels inside ``rect``.
    A pixel is "red" iff r >= 150 and g <= 90 and b <= 90 (same threshold as the sim's
    Region.is_red). Returns (None, None, 0) when no red pixel is found."""
    x0, y0, rw, rh = rect
    x1, y1 = x0 + rw, y0 + rh
    sx = sy = n = 0
    for yy in range(max(0, y0), min(h, y1)):
        row = yy * w
        for xx in range(max(0, x0), min(w, x1)):
            o = (row + xx) * 3
            if rgb[o] >= 150 and rgb[o + 1] <= 90 and rgb[o + 2] <= 90:
                sx += xx; sy += yy; n += 1
    if n == 0:
        return None, None, 0
    return sx / n, sy / n, n


def _imu_skin_rect(desc):
    """Read the fit-transformed canvas rect for [skin.parts].imu. We recompute the SAME
    fit the sim's layout.py uses so this test never depends on a running app to tell it
    where the bubble is. The recovery mirrors main.c's recover_fit (aspect-preserving fit
    of the used skin_parts' bounding box)."""
    parts = desc.get("skin", {}).get("parts", {})
    imu = parts.get("imu")
    if not imu:
        return None
    canvas = desc["screens"][0]["render_canvas"]
    W, H = int(canvas["w"]), int(canvas["h"])
    # bounding box of ALL used skin_parts (the same fit main.c recovers)
    used = list(parts.values())
    minx = min(p["x"] for p in used); miny = min(p["y"] for p in used)
    maxx = max(p["x"] + p["w"] for p in used); maxy = max(p["y"] + p["h"] for p in used)
    bw, bh = maxx - minx, maxy - miny
    s = min(W / bw, H / bh) * 0.94
    ox = (W - bw * s) / 2.0 - minx * s
    oy = (H - bh * s) / 2.0 - miny * s
    return (int(round(imu["x"] * s + ox)),
            int(round(imu["y"] * s + oy)),
            max(1, int(round(imu["w"] * s))),
            max(1, int(round(imu["h"] * s))))


def _rect_center(rect):
    x, y, w, h = rect
    return x + w / 2.0, y + h / 2.0


def _stage_descriptor_and_skin(platform_dir: Path, device_id: str, outdir: Path,
                               desc: dict) -> None:
    """Copy ``<platform>/devices/<id>/capabilities.toml`` + its skin PNGs into ``outdir``
    so the app reads them via ``<io-dir>/capabilities.toml`` and the fit-recovered skin.
    Same shape as ``ci/run-under-sim.py``'s ``stage_descriptor`` + ``stage_skin`` — factored
    here so this test does not require running the sim wrapper first."""
    src = platform_dir / "devices" / device_id / "capabilities.toml"
    outdir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, outdir / "capabilities.toml")
    skin = desc.get("skin", {})
    for rel in (skin.get("body"), skin.get("lit_body")):
        if not rel:
            continue
        s = platform_dir / rel
        if not s.is_file():
            raise SystemExit(f"[check-imu] skin asset missing: {s}")
        d = outdir / rel
        d.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(s, d)


def _load_desc(desc_path: Path):
    """Load a capabilities.toml (matches sim's load_descriptor). Uses the stdlib
    tomllib when available (py3.11+), tomli as a fallback for older CI images."""
    try:
        import tomllib
        with open(desc_path, "rb") as f:
            return tomllib.load(f)
    except ModuleNotFoundError:
        import tomli
        with open(desc_path, "rb") as f:
            return tomli.load(f)


def run_synth_leg(args, Device, ControlError, HardwareAbsent) -> Checker:
    c = Checker("synth_imu")
    _stage_synth_skins(FIXTURE)
    desc = _load_desc(FIXTURE / "devices" / "synth_imu" / "capabilities.toml")
    c.chk(bool(desc.get("sensors")), "fixture descriptor has [[sensors]] row")
    imu_rect = _imu_skin_rect(desc)
    c.chk(imu_rect is not None, "fixture descriptor has [skin.parts].imu rect")

    outdir = args.outdir / "synth_imu"
    outdir.mkdir(parents=True, exist_ok=True)
    # Stage the synthetic descriptor + its skin the same way run-under-sim.py stages a
    # real device — main.c reads capabilities.toml out of the outdir bind for skin/sensor.
    _stage_descriptor_and_skin(FIXTURE, "synth_imu", outdir, desc)

    dev = Device("synth_imu", str(FIXTURE),
                 launcher="qemu",
                 outdir=str(outdir),
                 app_arm64=str(args.binary),
                 qemu_tsp=str(args.qemu_tsp),
                 rootfs=str(args.rootfs))
    dev.boot()
    try:
        c.chk(dev.broker.is_present("imu"), "broker reports imu present on synth fixture")

        # 1. REST pose: bubble draws at centre, dot ≈ centre of [skin.parts].imu rect.
        dev.set_pose(yaw=0.0, pitch=0.0, roll=0.0)
        rest = dev.snapshot("rest")
        rest_ppm = outdir / "frames" / f"{rest}.ppm"
        w, h, rgb = _read_ppm(rest_ppm)
        rest_cx, rest_cy, rest_n = _find_red_dot_center(rgb, w, h, imu_rect)
        c.chk(rest_n > 0, "rest: red bubble dot exists inside imu rect")
        cx, cy = _rect_center(imu_rect)
        if rest_cx is not None:
            dr = math.hypot(rest_cx - cx, rest_cy - cy)
            c.chk(dr < min(imu_rect[2], imu_rect[3]) * 0.15,
                  f"rest: bubble centred (offset={dr:.1f}px)")

        # 2. IMU FIFO read matches physical model + mount_matrix APPLIED at rest.
        rest_reading = dev.read_imu()
        c.chk(rest_reading is not None, "rest: read_imu() returns a reading (not absent)")
        if rest_reading is not None:
            ax, ay, az = rest_reading["accel"]
            c.chk(abs(ax) < 0.2 and abs(ay) < 0.2 and abs(az - 9.80665) < 0.2,
                  f"rest: accel≈(0,0,g) — got ({ax:.3f}, {ay:.3f}, {az:.3f})")

        # 3. TILTED pose: pitch=15°, roll=10° — device-frame accel that the physical
        #    model would compute (broker.set_pose returns it via read_imu). The mount
        #    matrix is a 90° rotation about Z; if the app skipped the mount transform
        #    the reply's ax/ay would be swapped/sign-flipped and this fails.
        pitch_deg, roll_deg = 15.0, 10.0
        dev.set_pose(pitch=pitch_deg, roll=roll_deg)
        # Expected DEVICE-frame accel from physical_model (see sim/sensor/physical_model.py):
        #   accel_x = -g * sin(roll)
        #   accel_y =  g * cos(roll) * sin(pitch)
        #   accel_z =  g * cos(roll) * cos(pitch)
        g = 9.80665
        pr, rr = math.radians(pitch_deg), math.radians(roll_deg)
        exp_ax = -g * math.sin(rr)
        exp_ay =  g * math.cos(rr) * math.sin(pr)
        exp_az =  g * math.cos(rr) * math.cos(pr)
        tilted_reading = dev.read_imu()
        c.chk(tilted_reading is not None, "tilted: read_imu() returns a reading")
        if tilted_reading is not None:
            ax, ay, az = tilted_reading["accel"]
            # Tight tolerance: within 100 mm/s^2 (~1% of g) accounts for milli-SI rounding
            # + int16 quantization from the IIO synth. Mount-matrix violation would drift
            # by ~1.7 m/s^2 in ax or ay — well outside this envelope.
            tol = 0.10
            c.chk(abs(ax - exp_ax) < tol,
                  f"tilted: ax={ax:.3f} vs expected {exp_ax:.3f} (tol {tol})")
            c.chk(abs(ay - exp_ay) < tol,
                  f"tilted: ay={ay:.3f} vs expected {exp_ay:.3f} (tol {tol})")
            c.chk(abs(az - exp_az) < tol,
                  f"tilted: az={az:.3f} vs expected {exp_az:.3f} (tol {tol})")

            # Anti-check: WITHOUT the mount_matrix, the reply would be chip-frame; for
            # a 90° rotation about Z that's ax_wrong = -exp_ay, ay_wrong = exp_ax. The
            # actual reply should NOT look like the chip-frame — proves the app applied
            # the descriptor mount_matrix (not just re-echoed the sysfs raws).
            chip_ax_wrong, chip_ay_wrong = -exp_ay, exp_ax
            drift_if_missed = math.hypot(ax - chip_ax_wrong, ay - chip_ay_wrong)
            c.chk(drift_if_missed > 1.0,
                  f"tilted: reply is device-frame, not chip-frame "
                  f"(drift-from-chip={drift_if_missed:.3f} m/s^2 > 1.0)")

        # 4. Snap the tilted frame + assert the bubble dot MOVED (roll shifts it in x,
        #    pitch shifts it in y — see sensor_imu.c's render mapping).
        tilted = dev.snapshot("tilted")
        w2, h2, rgb2 = _read_ppm(outdir / "frames" / f"{tilted}.ppm")
        t_cx, t_cy, t_n = _find_red_dot_center(rgb2, w2, h2, imu_rect)
        c.chk(t_n > 0, "tilted: red bubble dot still inside imu rect")
        if rest_cx is not None and t_cx is not None:
            shift = math.hypot(t_cx - rest_cx, t_cy - rest_cy)
            c.chk(shift > min(imu_rect[2], imu_rect[3]) * 0.03,
                  f"tilted: bubble moved from rest (shift={shift:.1f}px)")
    finally:
        dev.shutdown()
    return c


# ---------- OMISSION LEG ----------------------------------------------------

def run_omission_leg(args, device_id, Device, ControlError, HardwareAbsent) -> Checker:
    c = Checker(f"omission-{device_id}")
    outdir = args.outdir / device_id
    outdir.mkdir(parents=True, exist_ok=True)
    # Stage the shipping descriptor + its skin the same way run-under-sim.py does; without
    # this the app runs "descriptor-only" and the omission read short-circuits to absent
    # by *file absence* — a weaker proof. Staging exercises the real parse path against
    # both shipping descriptors (they contain no [[sensors]] rows -> honest omission).
    shipping_desc = _load_desc(args.platform / "devices" / device_id / "capabilities.toml")
    _stage_descriptor_and_skin(args.platform, device_id, outdir, shipping_desc)

    dev = Device(device_id, str(args.platform),
                 launcher="qemu",
                 outdir=str(outdir),
                 app_arm64=str(args.binary),
                 qemu_tsp=str(args.qemu_tsp),
                 rootfs=str(args.rootfs))
    dev.boot()
    try:
        c.chk(not dev.broker.is_present("imu"),
              f"{device_id}: broker reports imu absent (no [[sensors]] row)")
        c.chk(dev.assert_capability_absent("imu"),
              f"{device_id}: assert_capability_absent(\"imu\") passes")

        # read_imu FIFO must reply "imu-absent" — no crash, no fabricated numbers.
        reading = dev.read_imu()
        c.chk(reading is None,
              f"{device_id}: read_imu() replies imu-absent (typed hardware-absent)")

        # set_pose on absent hardware raises HardwareAbsent — the typed no-op, not a crash.
        try:
            dev.set_pose(pitch=15.0)
            c.chk(False, f"{device_id}: set_pose should raise HardwareAbsent")
        except HardwareAbsent:
            c.chk(True, f"{device_id}: set_pose raises HardwareAbsent (typed no-op)")

        # A snapshot still WORKS (the app doesn't crash on absent sensor) — the
        # existing E7 matrix (check-control-hwprobe.py) proves the input matrix here;
        # C5's only visual claim is "no bubble", trivially satisfied by no bubble rect.
        dev.snapshot("rest")
    finally:
        dev.shutdown()
    return c


# ---------- MAIN ------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary",   required=True, type=Path)
    ap.add_argument("--sim",      required=True, type=Path)
    ap.add_argument("--platform", required=True, type=Path,
                    help="platform checkout root — shipping descriptors come from here")
    ap.add_argument("--qemu-tsp", required=True, type=Path)
    ap.add_argument("--rootfs",   required=True, type=Path)
    ap.add_argument("--outdir",   required=True, type=Path)
    ap.add_argument("--omission-devices", nargs="+", default=["a133", "a523"],
                    help="shipping descriptors to sweep for the omission leg")
    args = ap.parse_args()

    for label, path in (("binary", args.binary), ("sim", args.sim),
                        ("platform", args.platform),
                        ("qemu-tsp", args.qemu_tsp), ("rootfs", args.rootfs)):
        if not path.exists():
            raise SystemExit(f"[check-imu] --{label} does not exist: {path}")

    args.outdir.mkdir(parents=True, exist_ok=True)
    Device, ControlError, HardwareAbsent = _import_sim(args.sim)

    all_fails = []
    print("\n############## SYNTHETIC LEG (sensor + tilt-bubble) ##############\n")
    synth = run_synth_leg(args, Device, ControlError, HardwareAbsent)
    all_fails.extend(synth.fails)

    for dev_id in args.omission_devices:
        print(f"\n############## OMISSION LEG ({dev_id}) ##############\n")
        om = run_omission_leg(args, dev_id, Device, ControlError, HardwareAbsent)
        all_fails.extend(om.fails)

    print()
    if all_fails:
        print(f"CHECK-IMU FAIL — {len(all_fails)} assertion(s) failed")
        return 1
    print("CHECK-IMU PASS — sensor + tilt-bubble on synthetic descriptor + "
          f"omission proof on {', '.join(args.omission_devices)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
