#!/usr/bin/env python3
"""run-under-sim.py — headless sim drive of pf-hwprobe (C1 acceptance evidence).

Runs the SAME pf-hwprobe.arm64 binary under the E5 simulator harness
(``sim/harness/run-in-harness.sh``: qemu-tsp + bubblewrap, no crun) against a
descriptor, wires up the FIFO handshake via ``sim.control.control_surface.Device``,
and captures one framebuffer PPM per descriptor. This is the "runs UNMODIFIED for
BOTH descriptors" proof — we invoke it twice (a133, a523) with different values of
``--device`` and assert the a133/a523 delta comes from the descriptor alone.

Not a sim-repo modification: this wrapper only CONSUMES ``control_surface.Device``
(read-only per the E6 epic ruling) and stages ONE extra input under ``outdir`` —
the copy of ``platform/devices/<id>/capabilities.toml`` the app reads through
``pf_connect_descriptor`` — before ``.boot()``. If the sim later exposes a first-
party descriptor bind, this wrapper collapses to a straight ``Device(...).boot()``.

USAGE
-----
::

    ./ci/run-under-sim.py \\
        --device a133 \\
        --binary   ./build/pf-hwprobe.arm64 \\
        --sim      ./.cache/sim \\
        --platform ./.cache/platform \\
        --qemu-tsp /opt/pf/qemu-tsp/qemu-aarch64 \\
        --rootfs   /opt/pf/rootfs-arm64 \\
        --outdir   ./evidence/a133

The script exits non-zero (with a printed diagnostic) on any handshake failure or
snapshot capture failure. Intended run host: modelmaker (mm@10.0.40.90).
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def _import_sim(sim_dir: Path):
    """Wire up sim's Python path exactly the way sim itself does (../synth, ../fb,
    ../sensor, and the ``control`` package)."""
    control = sim_dir / "control"
    for extra in ("synth", "fb", "sensor"):
        sys.path.insert(0, str(sim_dir / extra))
    sys.path.insert(0, str(control))
    # noqa import order: injections must precede the actual imports.
    from control_surface import Device, ControlError  # type: ignore[import-not-found]
    return Device, ControlError


def stage_descriptor(platform_dir: Path, device_id: str, outdir: Path) -> Path:
    """Copy ``platform/devices/<id>/capabilities.toml`` into ``outdir`` so the app
    finds it via ``<io-dir>/capabilities.toml`` when the harness binds ``outdir`` as
    ``/out``. Never re-authors the descriptor — plain file copy."""
    src = platform_dir / "devices" / device_id / "capabilities.toml"
    if not src.is_file():
        raise SystemExit(f"[run-under-sim] no descriptor at {src}")
    outdir.mkdir(parents=True, exist_ok=True)
    dst = outdir / "capabilities.toml"
    shutil.copyfile(src, dst)
    return dst


def stage_skin(platform_dir: Path, device_id: str, outdir: Path) -> None:
    """Stage the descriptor's skin PNGs (``skin.body`` + ``skin.lit_body``) into
    ``outdir`` preserving their descriptor-relative paths, so the app resolves them at
    ``<io-dir>/<skin.body>`` under the bwrap ``/out`` bind (the harness binds ``outdir``
    only, not the platform ``skins/`` tree). C2 (tsp-fr2n.2) render dependency. Plain
    file copies of the platform's real PNGs — never re-authored or converted."""
    try:
        import tomllib  # py3.11+
        _load = lambda p: tomllib.load(open(p, "rb"))  # noqa: E731
    except ModuleNotFoundError:  # pragma: no cover
        import tomli as tomllib  # type: ignore
        _load = lambda p: tomllib.load(open(p, "rb"))  # noqa: E731
    desc = _load(platform_dir / "devices" / device_id / "capabilities.toml")
    skin = desc.get("skin", {})
    rels = [skin.get("body"), skin.get("lit_body")]
    for rel in rels:
        if not rel:
            continue
        src = platform_dir / rel
        if not src.is_file():
            raise SystemExit(f"[run-under-sim] skin asset missing: {src}")
        dst = outdir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        print(f"[run-under-sim] staged skin {rel}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device",   required=True, help="a133 | a523 | ...")
    ap.add_argument("--binary",   required=True, type=Path)
    ap.add_argument("--sim",      required=True, type=Path)
    ap.add_argument("--platform", required=True, type=Path)
    ap.add_argument("--qemu-tsp", required=True, type=Path)
    ap.add_argument("--rootfs",   required=True, type=Path)
    ap.add_argument("--outdir",   required=True, type=Path)
    ap.add_argument("--snap-name", default="c1-skeleton",
                    help="snapshot name (PPM basename under outdir/frames)")
    args = ap.parse_args()

    for label, path in (("binary", args.binary), ("sim", args.sim),
                        ("platform", args.platform),
                        ("qemu-tsp", args.qemu_tsp), ("rootfs", args.rootfs)):
        if not path.exists():
            raise SystemExit(f"[run-under-sim] --{label} does not exist: {path}")

    Device, _ = _import_sim(args.sim)
    stage_descriptor(args.platform, args.device, args.outdir)
    stage_skin(args.platform, args.device, args.outdir)

    dev = Device(args.device, str(args.platform),
                 launcher="qemu", outdir=str(args.outdir),
                 app_arm64=str(args.binary),
                 qemu_tsp=str(args.qemu_tsp), rootfs=str(args.rootfs))
    dev.boot()
    try:
        # C1 acceptance frame: rest state, no injections, one snapshot.
        name = dev.snapshot(args.snap_name)
        ppm = args.outdir / "frames" / f"{name}.ppm"
        if not ppm.is_file():
            raise SystemExit(f"[run-under-sim] snapshot missing: {ppm}")
        # Convert PPM -> PNG via the sim's own utility if available (evidence
        # attachment prefers PNG). Silently skip if the module is unavailable.
        try:
            sys.path.insert(0, str(args.sim / "fb"))
            from ppm2png import read_ppm  # type: ignore[import-not-found]
            w, h, rgb = read_ppm(str(ppm))
            png = ppm.with_suffix(".png")
            try:
                import png as pypng  # type: ignore[import-not-found]
                writer = pypng.Writer(w, h, greyscale=False)
                with open(png, "wb") as f:
                    writer.write_array(f, rgb)
            except ModuleNotFoundError:
                # pypng absent — use Pillow if present, else leave PPM only.
                try:
                    from PIL import Image  # type: ignore[import-not-found]
                    img = Image.frombytes("RGB", (w, h), bytes(rgb))
                    img.save(png)
                except ModuleNotFoundError:
                    png = None
            if png:
                print(f"[run-under-sim] wrote {png}")
        except Exception as e:  # noqa: BLE001 — evidence gen is best-effort
            print(f"[run-under-sim] PNG conversion skipped: {e}")

        print(f"[run-under-sim] {args.device}: OK  ppm={ppm}  "
              f"controls={len(dev.groups)}  canvas={dev.canvas}")
        return 0
    finally:
        dev.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
