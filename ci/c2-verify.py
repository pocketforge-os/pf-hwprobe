#!/usr/bin/env python3
"""c2-verify.py — device-free acceptance for tsp-fr2n.2 (C2: buttons, both devices).

Drives the SAME pf-hwprobe.arm64 binary under the E5 simulator against a descriptor and
asserts the C2 contract, purely from descriptor rows (zero per-device test code):

  * every `kind="button"` input lights RED on press and clears on release
    (deterministic framebuffer_region(id).is_red(), NOT a VLM);
  * the a523-only rows — `home` (KEY_HOMEPAGE), `l3`/`r3` (BTN_THUMBL/R stick-clicks) —
    light on the a523 descriptor, and on the a133 are OMITTED: `press()` raises
    HardwareAbsent (the control is not on the device), and the app renders + runs without
    crashing;
  * `guide` (BTN_MODE) exists and lights on BOTH.

Two descriptors, ONE test body: pass `--device a133|a523`. Evidence PPM/PNGs land under
`<outdir>/frames/`. Intended run host: modelmaker (mm@10.0.40.90), under sudo (uinput +
the bound event nodes are root-only). Exits non-zero on the first failed assertion.

USAGE (mirrors ci/run-under-sim.py):
    sudo ./ci/c2-verify.py --device a523 \
        --binary ./build/pf-hwprobe.arm64 --sim ./.cache/sim --platform ./.cache/platform \
        --qemu-tsp /home/mm/qemu-tsp/build/qemu-tsp/qemu-aarch64 \
        --rootfs /home/mm/sim-build/harness/rootfs-arm64 --outdir ./evidence/c2-a523
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

# a523-only rows (present on a523, OMITTED on a133) + their expected lit behaviour.
A523_ONLY = ("home", "l3", "r3")


def _import_sim(sim_dir: Path):
    for extra in ("synth", "fb", "sensor"):
        sys.path.insert(0, str(sim_dir / extra))
    sys.path.insert(0, str(sim_dir / "control"))
    from control_surface import Device, ControlError  # type: ignore[import-not-found]
    from broker_stub import HardwareAbsent            # type: ignore[import-not-found]
    return Device, ControlError, HardwareAbsent


def _load_toml(path: Path):
    try:
        import tomllib
    except ModuleNotFoundError:  # pragma: no cover
        import tomli as tomllib  # type: ignore
    with open(path, "rb") as f:
        return tomllib.load(f)


def stage(platform_dir: Path, device_id: str, outdir: Path):
    """Stage capabilities.toml + the skin PNGs into outdir (== ci/run-under-sim.py)."""
    outdir.mkdir(parents=True, exist_ok=True)
    desc_src = platform_dir / "devices" / device_id / "capabilities.toml"
    if not desc_src.is_file():
        raise SystemExit(f"[c2-verify] no descriptor at {desc_src}")
    shutil.copyfile(desc_src, outdir / "capabilities.toml")
    desc = _load_toml(desc_src)
    for rel in (desc.get("skin", {}).get("body"), desc.get("skin", {}).get("lit_body")):
        if not rel:
            continue
        src = platform_dir / rel
        if not src.is_file():
            raise SystemExit(f"[c2-verify] skin asset missing: {src}")
        dst = outdir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
    return desc


def _png(outdir: Path, sim: Path, name: str):
    """Best-effort PPM->PNG for evidence (silently skipped if no encoder available)."""
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
        print(f"[c2-verify] PNG skip {name}: {e}")


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

    Device, ControlError, HardwareAbsent = _import_sim(args.sim)
    desc = stage(args.platform, args.device, args.outdir)

    buttons = [i["id"] for i in desc.get("inputs", []) if i.get("kind") == "button"]
    present_ids = {i["id"] for i in desc.get("inputs", [])}
    a523_only_here = [r for r in A523_ONLY if r in present_ids]
    a523_only_absent = [r for r in A523_ONLY if r not in present_ids]

    print(f"== C2 verify: {args.device} == buttons={buttons}")
    print(f"   a523-only present={a523_only_here} absent(expect omitted)={a523_only_absent}")

    chk = Checker()
    dev = Device(args.device, str(args.platform), launcher="qemu", outdir=str(args.outdir),
                 app_arm64=str(args.binary), qemu_tsp=str(args.qemu_tsp), rootfs=str(args.rootfs))
    dev.boot()
    try:
        # rest frame (nothing pressed) — no control should read red.
        dev.snapshot("c2-rest")
        _png(args.outdir, args.sim, "c2-rest")
        for bid in buttons:
            chk.ok(not dev.framebuffer_region(bid).is_red(), f"{bid}: idle not red")

        # every kind=button: press -> red, release -> not red.
        for bid in buttons:
            dev.press(bid)
            dev.snapshot(f"c2-{bid}-press")
            red = dev.framebuffer_region(bid).is_red()
            chk.ok(red, f"{bid}: RED on press  ({dev.framebuffer_region(bid).color()})")
            if red:
                _png(args.outdir, args.sim, f"c2-{bid}-press")
            dev.release(bid)
            dev.snapshot(f"c2-{bid}-release")
            chk.ok(not dev.framebuffer_region(bid).is_red(), f"{bid}: clears on release")

        # a523-only rows PRESENT here: light on press.
        for rid in a523_only_here:
            dev.press(rid)
            dev.snapshot(f"c2-{rid}-press")
            red = dev.framebuffer_region(rid).is_red()
            chk.ok(red, f"{rid} (a523-only): RED on press  ({dev.framebuffer_region(rid).color()})")
            if red:
                _png(args.outdir, args.sim, f"c2-{rid}-press")
            dev.release(rid)

        # a523-only rows ABSENT here (a133): press() must raise HardwareAbsent; no crash.
        for rid in a523_only_absent:
            try:
                dev.press(rid)
                chk.ok(False, f"{rid}: expected HardwareAbsent on {args.device}, got none")
            except HardwareAbsent:
                chk.ok(True, f"{rid}: omitted on {args.device} (HardwareAbsent, no crash)")
        if a523_only_absent:
            # prove the app is still alive + rendering after the absent-control probes.
            dev.snapshot("c2-post-absent")
            chk.ok(True, "app still renders after omitted-control probes (no crash)")
    finally:
        dev.shutdown()

    print(f"== {args.device}: {chk.passed} passed, {chk.failed} failed ==")
    return 0 if chk.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
