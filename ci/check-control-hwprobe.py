#!/usr/bin/env python3
"""check-control-hwprobe.py — pf-hwprobe's E5-sim + E7-CI matrix runner (bead tsp-fr2n.7).

Drives the **generic, descriptor-driven regression matrix** that already lives in
``pocketforge-os/sim`` (``sim/control/check-control.py``, tsp-an4.5) against the
**real** pf-hwprobe binary — NOT the sim's reference ``hwprobe-lite`` payload.
That matrix is the honest owner-approved contract: for every ``[[inputs]]`` row
inject the event -> assert the control lit; for every trigger sweep min->max ->
assert the marker tracks; for every ABSENT control assert typed hardware-absent
(never a crash). Two descriptors (a133, a523) = two matrix rows with ZERO
app/test-code difference — the a133/a523 delta enters purely through the
descriptor (E1's ``capabilities.toml``).

Why this exists as a THIN wrapper (and not a fork of check-control.py):
    * ``sim/control/check-control.py`` already implements the matrix + the FIFO
      handshake driver (``sim/control/control_surface.Device``). The pf-hwprobe
      binary implements the SAME FIFO protocol (``ready``/``snap <ppm>``/``quit``,
      matches ``sim/control/hwprobe-lite.c``), so it is a drop-in replacement for
      the reference payload once ``APP_ARM64`` points at pf-hwprobe.
    * We drive **qemu launcher ONLY** for now — the sim's parity check needs an
      x86 native build too, and pf-hwprobe currently only cross-builds arm64.
      An x86 build is a straightforward follow-up (tracked below); until it
      lands, the parity leg is SKIPPED loudly, NEVER silently.
    * The R-G bootstrap holds: pf-hwprobe's C1 skeleton already renders red on
      active (``src/main.c`` render_stub_widget: 220,30,30 when
      ``control_is_active``, and grey+red proportional fill for triggers), so
      the ``is_red()`` + slider fraction assertions PASS against C1 today.
      C2..C6 replace the stub bodies with proper button-light /
      slider-with-marker / hat-cross / stick-calibration widgets — the same
      red-on-press semantics carry through, so this matrix stays green as they
      land. (A C2 that regresses red-on-active would fail this gate, which is
      the point.)

Wired into E7's CI caller as an ADVISORY smoke gate — see
``.github/workflows/pf-hwprobe-smoke.yml``. Blocking promotion follows the same
posture as E5's ``sim-gate.yml`` (advisory -> blocking once both descriptors
stay green + stable).

USAGE
-----
::

    ./ci/check-control-hwprobe.py \
        --binary   ./build/pf-hwprobe.arm64 \
        --sim      ./.cache/sim \
        --platform ./.cache/platform \
        --qemu-tsp /home/mm/qemu-tsp/build/qemu-tsp/qemu-aarch64 \
        --rootfs   /home/mm/sim-build/harness/rootfs-arm64 \
        --outdir   ./evidence/matrix \
        [--devices a133 a523]

Exit 0 = matrix PASS on every requested descriptor. Same exit-code contract as
``check-control.py``.

Follow-up (out of scope for tsp-fr2n.7):
    * ADD an x86 native build to ``build.sh`` (mirror the arm64 recipe with
      ``x86_64-linux-gnu-gcc`` + ``x86`` SDL3 build) and drop
      ``--only-launcher qemu`` — the parity check restores.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def _resolve_sim_check_control(sim_dir: Path) -> Path:
    """Return the sim's check-control.py path; explode loudly if the layout drifts."""
    p = sim_dir / "control" / "check-control.py"
    if not p.is_file():
        raise SystemExit(
            f"[check-control-hwprobe] no {p} — is --sim pointing at the sim checkout root? "
            f"(expected <sim>/control/check-control.py)"
        )
    return p


def _stage_descriptors(platform_dir: Path, devices: list[str]) -> None:
    """Sanity-check that every requested device has a descriptor under
    <platform>/devices/<id>/capabilities.toml before we launch the sim."""
    missing = []
    for dev in devices:
        p = platform_dir / "devices" / dev / "capabilities.toml"
        if not p.is_file():
            missing.append(str(p))
    if missing:
        raise SystemExit(
            "[check-control-hwprobe] missing descriptor(s): " + ", ".join(missing)
            + "  (is --platform pointing at the platform checkout root?)"
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary",   required=True, type=Path,
                    help="pf-hwprobe.arm64 (the real app under test)")
    ap.add_argument("--sim",      required=True, type=Path,
                    help="sim checkout root (has control/check-control.py)")
    ap.add_argument("--platform", required=True, type=Path,
                    help="platform checkout root (has devices/<id>/capabilities.toml)")
    ap.add_argument("--qemu-tsp", required=True, type=Path,
                    help="qemu-tsp aarch64-linux-user binary")
    ap.add_argument("--rootfs",   required=True, type=Path,
                    help="arm64 bookworm rootfs (bwrap-bind target)")
    ap.add_argument("--outdir",   required=True, type=Path,
                    help="evidence root (frames PPM+PNG land here)")
    ap.add_argument("--devices",  nargs="+", default=["a133", "a523"],
                    help="descriptor ids to sweep (default: a133 a523)")
    ap.add_argument("--label", default="pf-hwprobe",
                    help="label stamped in the transcript header (default: pf-hwprobe)")
    args = ap.parse_args()

    for label, path in (("binary", args.binary), ("sim", args.sim),
                        ("platform", args.platform),
                        ("qemu-tsp", args.qemu_tsp), ("rootfs", args.rootfs)):
        if not path.exists():
            raise SystemExit(f"[check-control-hwprobe] --{label} does not exist: {path}")

    check_control = _resolve_sim_check_control(args.sim)
    _stage_descriptors(args.platform, args.devices)
    args.outdir.mkdir(parents=True, exist_ok=True)

    env = dict(os.environ,
               PLATFORM=str(args.platform),
               APP_ARM64=str(args.binary),
               QEMU_TSP=str(args.qemu_tsp),
               ROOTFS=str(args.rootfs))
    # APP_X86 is intentionally not set — --only-launcher qemu skips the native leg
    # (see the wrapper's module docstring). Passing APP_X86 through would enable
    # parity checks against a hwprobe-lite.x86 the sim baked, which is NOT the
    # pf-hwprobe payload; the parity check is meaningless there.
    env.pop("APP_X86", None)

    cmd = [
        sys.executable, str(check_control),
        "--only-launcher", "qemu",
        "--outdir-base", str(args.outdir),
        "--label", args.label,
        *args.devices,
    ]
    print(f"[check-control-hwprobe] $ {' '.join(cmd)}")
    print(f"[check-control-hwprobe]   APP_ARM64={args.binary}")
    print(f"[check-control-hwprobe]   PLATFORM={args.platform}")
    print(f"[check-control-hwprobe]   devices={args.devices}")
    rc = subprocess.call(cmd, env=env)
    if rc == 0:
        print(f"[check-control-hwprobe] MATRIX PASS ({', '.join(args.devices)}) "
              f"evidence under {args.outdir}")
    else:
        print(f"[check-control-hwprobe] MATRIX FAIL (rc={rc}); evidence under {args.outdir}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
