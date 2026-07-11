#!/usr/bin/env python3
"""c4-verify.py — the C4 (tsp-fr2n.4) sticks+dpad bead-specific evidence run.

Rides on the same E5 sim + control_surface.Device the C7 matrix uses. For each
device (a133, a523):
  * 4 stick quadrants per stick (lstick + rstick), snap each -> assert the region
    is red (wash) AND the bright-yellow dot sits in the expected quadrant of the
    rect. The dot position is verified deterministically by scanning the rect's
    quadrant tiles for the presence of a yellow pixel (r>=200, g>=180, b<=80) —
    this proves the C4 widget is drawing at the normalized (nx, ny), not somewhere
    else in the rect.
  * 8 hat directions on dpad, snap each -> assert the region is red AND the
    corresponding 3x3-grid octant has a yellow highlight.
  * A rest-state re-check per device: the stick/dpad rect centre is NOT red,
    confirming no wash + no confounding yellow at centre (the E7 rest invariant).

Regression protection: this script does NOT rerun the full E7 matrix; the
canonical E7 matrix run (ci/check-control-hwprobe.py) is the authoritative gate
and must be green independently. This script's job is to prove the *new C4*
directional widget behavior beyond the "is_red at centre" bar.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path


def _import_sim(sim_dir: Path):
    for extra in ("synth", "fb", "sensor"):
        sys.path.insert(0, str(sim_dir / extra))
    sys.path.insert(0, str(sim_dir / "control"))
    from control_surface import Device  # type: ignore[import-not-found]
    from ppm2png import read_ppm  # type: ignore[import-not-found]
    return Device, read_ppm


def _stage(platform_dir: Path, device: str, outdir: Path) -> None:
    """Match run-under-sim.py: stage capabilities.toml AND the skin PNGs so the
    app's C2 have_skin path (which my C4 widget dispatch layers over) is exercised.
    check-control-hwprobe.py's E7 matrix uses stub renders and never staged either —
    that is deliberate for the base is_red contract but is NOT the honest C4 render
    path, so we stage explicitly here."""
    src = platform_dir / "devices" / device / "capabilities.toml"
    outdir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, outdir / "capabilities.toml")
    try:
        import tomllib
    except ModuleNotFoundError:
        import tomli as tomllib  # type: ignore
    with open(src, "rb") as f:
        desc = tomllib.load(f)
    for rel in (desc.get("skin", {}).get("body"), desc.get("skin", {}).get("lit_body")):
        if not rel:
            continue
        s = platform_dir / rel
        d = outdir / rel
        d.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(s, d)


def _yellow(rgb, w, h, x0, y0, x1, y1) -> bool:
    """True if any pixel in the [x0,x1) x [y0,y1) rect is bright yellow (widget colour)."""
    x0 = max(0, x0); y0 = max(0, y0)
    x1 = min(w, x1); y1 = min(h, y1)
    for yy in range(y0, y1):
        base = yy * w * 3
        for xx in range(x0, x1):
            o = base + xx * 3
            r, g, b = rgb[o], rgb[o + 1], rgb[o + 2]
            if r >= 200 and g >= 180 and b <= 80:
                return True
    return False


def _center_red(rgb, w, h, cx, cy) -> bool:
    """3x3 average at (cx, cy) — matches control_surface.Region.is_red()."""
    rs = gs = bs = n = 0
    for yy in range(max(0, cy - 1), min(h, cy + 2)):
        for xx in range(max(0, cx - 1), min(w, cx + 2)):
            o = (yy * w + xx) * 3
            rs += rgb[o]; gs += rgb[o + 1]; bs += rgb[o + 2]
            n += 1
    if n == 0:
        return False
    r, g, b = rs // n, gs // n, bs // n
    return r >= 150 and g <= 90 and b <= 90


def _rect_for(dev, skin_part):
    for g in dev.groups:
        if g.skin_part == skin_part:
            return g.canvas
    raise KeyError(skin_part)


STICK_QUADRANTS = [(-1.0, -1.0), (1.0, -1.0), (-1.0, 1.0), (1.0, 1.0)]
HAT_DIRS = [(-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1)]


def _run_for(device_id, platform_dir, sim_dir, qemu_tsp, rootfs, app_arm64, base_out):
    Device, read_ppm = _import_sim(sim_dir)
    outdir = base_out / device_id
    if outdir.exists():
        shutil.rmtree(outdir)
    _stage(platform_dir, device_id, outdir)
    dev = Device(device_id, str(platform_dir), launcher="qemu", outdir=str(outdir),
                 app_arm64=str(app_arm64), qemu_tsp=str(qemu_tsp), rootfs=str(rootfs))
    dev.boot()
    fails = []
    try:
        # Rest baseline.
        dev.snapshot("rest")
        rest_ppm = outdir / "frames" / "rest.ppm"
        w, h, rgb = read_ppm(str(rest_ppm))
        for sp in ("stick_l", "stick_r", "dpad"):
            try:
                rx, ry, rw, rh = _rect_for(dev, sp)
            except KeyError:
                continue
            if _center_red(rgb, w, h, rx + rw // 2, ry + rh // 2):
                fails.append(f"[{device_id}] rest: {sp} centre is RED (should be dark)")

        # Sticks: 4 quadrants each, per stick present on this descriptor.
        stick_ids = [i["id"] for i in dev.inputs() if i.get("kind") == "stick"]
        for iid in stick_ids:
            sp = next(i["skin_part"] for i in dev.inputs() if i["id"] == iid)
            rx, ry, rw, rh = _rect_for(dev, sp)
            cx0, cy0 = rx + rw // 2, ry + rh // 2
            for nx, ny in STICK_QUADRANTS:
                dev.set_stick(iid, nx, ny)
                name = f"{iid}_{'p' if nx > 0 else 'n'}x_{'p' if ny > 0 else 'n'}y"
                dev.snapshot(name)
                w, h, rgb = read_ppm(str(outdir / "frames" / f"{name}.ppm"))
                # E7 preserved: centre red.
                if not _center_red(rgb, w, h, cx0, cy0):
                    fails.append(f"[{device_id}] {iid} @({nx:+.1f},{ny:+.1f}) centre NOT red (wash regression)")
                # C4 dot: half of the rect containing (nx, ny) has a yellow pixel.
                # Split rect into 4 quadrants; the pointed-to quadrant should carry yellow.
                mx, my = rx + rw // 2, ry + rh // 2
                if nx > 0:
                    qx0, qx1 = mx, rx + rw
                else:
                    qx0, qx1 = rx, mx
                if ny > 0:
                    qy0, qy1 = my, ry + rh
                else:
                    qy0, qy1 = ry, my
                if not _yellow(rgb, w, h, qx0, qy0, qx1, qy1):
                    fails.append(f"[{device_id}] {iid} @({nx:+.1f},{ny:+.1f}) NO YELLOW dot in {'+X' if nx>0 else '-X'},{'+Y' if ny>0 else '-Y'} quadrant")
                # Wrong quadrant SHOULD be dot-free.
                if nx > 0:
                    wx0, wx1 = rx, mx
                else:
                    wx0, wx1 = mx, rx + rw
                if ny > 0:
                    wy0, wy1 = ry, my
                else:
                    wy0, wy1 = my, ry + rh
                if _yellow(rgb, w, h, wx0, wy0, wx1, wy1):
                    fails.append(f"[{device_id}] {iid} @({nx:+.1f},{ny:+.1f}) SPURIOUS yellow dot in opposite quadrant")
            # Return this stick to centre before moving to the next.
            dev.set_stick(iid, 0.0, 0.0)

        # dpad hat: 8 directions.
        hat_ids = [i["id"] for i in dev.inputs() if i.get("kind") == "hat"]
        for iid in hat_ids:
            sp = next(i["skin_part"] for i in dev.inputs() if i["id"] == iid)
            rx, ry, rw, rh = _rect_for(dev, sp)
            cx0, cy0 = rx + rw // 2, ry + rh // 2
            cw, ch = rw // 3, rh // 3
            for hx, hy in HAT_DIRS:
                dev.move_hat(iid, hx, hy)
                name = f"{iid}_{hx:+d}_{hy:+d}".replace("+", "p").replace("-", "n")
                dev.snapshot(name)
                w, h, rgb = read_ppm(str(outdir / "frames" / f"{name}.ppm"))
                if not _center_red(rgb, w, h, cx0, cy0):
                    fails.append(f"[{device_id}] dpad @({hx:+d},{hy:+d}) centre NOT red (wash regression)")
                # Expected cell rectangle for (hx, hy) — matches widget_hat's 3x3 grid.
                cell_x = rx + (hx + 1) * cw
                cell_y = ry + (hy + 1) * ch
                if not _yellow(rgb, w, h, cell_x, cell_y, cell_x + cw, cell_y + ch):
                    fails.append(f"[{device_id}] dpad @({hx:+d},{hy:+d}) NO YELLOW segment in expected cell")
                # Opposite cell should carry no yellow.
                ocell_x = rx + (-hx + 1) * cw
                ocell_y = ry + (-hy + 1) * ch
                if _yellow(rgb, w, h, ocell_x, ocell_y, ocell_x + cw, ocell_y + ch):
                    fails.append(f"[{device_id}] dpad @({hx:+d},{hy:+d}) SPURIOUS yellow in opposite cell")
            dev.move_hat(iid, 0, 0)
    finally:
        dev.shutdown()
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--sim", required=True, type=Path)
    ap.add_argument("--platform", required=True, type=Path)
    ap.add_argument("--qemu-tsp", required=True, type=Path)
    ap.add_argument("--rootfs", required=True, type=Path)
    ap.add_argument("--outdir", required=True, type=Path)
    ap.add_argument("--devices", nargs="+", default=["a133", "a523"])
    args = ap.parse_args()

    args.outdir.mkdir(parents=True, exist_ok=True)
    total_fails = []
    for dev_id in args.devices:
        print(f"\n==== C4 verify [{dev_id}] ====")
        fails = _run_for(dev_id, args.platform, args.sim, args.qemu_tsp,
                          args.rootfs, args.binary, args.outdir)
        if not fails:
            print(f"  ok  {dev_id}: ALL C4 stick+dpad checks passed")
        else:
            for f in fails:
                print(f"  FAIL  {f}")
            total_fails.extend(fails)
    if total_fails:
        print(f"\nC4 VERIFY FAIL ({len(total_fails)} failure(s))")
        return 1
    print("\nC4 VERIFY PASS (all devices)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
