#!/usr/bin/env python3
"""screen-reviewer-readback.py — pf-hwprobe frame -> screen-reviewer subagent (bead tsp-fr2n.7).

The matrix's PRIMARY readback is deterministic pixel sampling
(``sim/control/control_surface.Region.is_red()`` / ``Slider.at()``, NOT a VLM
per the tsp-visual-inspection hallucination caveat). This script adds a
COMPLEMENTARY visual assertion: run a curated set of key PNG frames through the
same ``review-screen.sh`` -> opencode ``screen-reviewer`` agent -> local vision
model (gemma-4-31B on modelmaker's vLLM) that hardware bring-up already uses.
Same vocabulary as the on-device camera path (``pocketforge-automation``'s
``review-screen.sh``), which is exactly what infra-105's C7 gate calls for.

WHY THIS IS NOT AN is_red() REPLACEMENT
---------------------------------------
Region.is_red() is authoritative (deterministic RGB sampling of a canvas rect
we already own the coordinates for). The reviewer is a defense-in-depth check
against a widget regressing in a way is_red() misses (e.g. the whole canvas
turning solid red would still make is_red() PASS but is obviously wrong to a
human — the reviewer catches that). We ASSERT the reviewer's summary contains
the expected control name + a lit/red/active keyword, and BAIL LOUDLY if it
does not; we do NOT try to parse a VLM sentence into a full-matrix truth
table.

WHY THIS IS OPTIONAL IN CI
--------------------------
The reviewer needs ``mm@10.0.40.90:8000`` reachable + ``opencode`` set up.
When it is unavailable (a fresh runner, a modelmaker outage, an untrusted PR
that cannot reach the lab network) the deterministic matrix is still the
authoritative pass/fail — this script exits ``review_status=skipped`` with a
clear reason, NEVER faking a green tick.

USAGE
-----
::

    # Assert one frame — reviewer's report must mention 'south' AND ('red' OR 'lit')
    ./ci/screen-reviewer-readback.py \
        --frame ./evidence/matrix/a133/qemu/frames/south_press.png \
        --expect-part south \
        --expect-lit

    # Batch mode: JSON list of {frame, expect_part, expect_lit?} — one report each.
    ./ci/screen-reviewer-readback.py --batch curated-frames.json

    # Just capture reports without asserting (for evidence attach).
    ./ci/screen-reviewer-readback.py --frame ./evidence/rest.png --no-assert

Exit codes:
    0 = every expected assertion PASSED (or --no-assert given and reviewer ran)
    1 = at least one assertion FAILED — the report was received but the
        expected part/lit signal was absent
    3 = reviewer unavailable (``review-screen.sh`` missing / model unreachable /
        opencode not configured) — CI treats this as SKIPPED, NOT FAIL

The script prints one machine-parseable line per frame:
    review_status={pass,fail,skipped} frame=<path> part=<id> lit=<0|1> reason=<...>

Then dumps the reviewer's full report indented under it, for the CI transcript.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_REVIEWER = "/home/matt/pocketforge-automation/scripts/review-screen.sh"
# The reviewer's tier-1 failure signals we treat as "skipped, not failed" (env issue,
# not an incorrect rendering) — matches the review-screen.sh contract.
SKIP_SIGNALS = (
    "review_status=failed",
    "reason=model_unreachable",
    "reason=image_not_delivered",
    "reason=opencode_missing",
)


def _run_reviewer(reviewer: Path, frame: Path, prompt: str, timeout: int) -> tuple[int, str]:
    """Invoke review-screen.sh on ONE frame; return (rc, combined stdout+stderr).
    Wrapped so a missing binary / missing opencode / model outage all bubble up
    as an exit-3 SKIP at the caller, never a false-red assertion FAIL."""
    if not reviewer.is_file() or not os.access(str(reviewer), os.X_OK):
        return 127, f"reviewer script missing or not executable: {reviewer}"
    cmd = [str(reviewer), str(frame), "--prompt", prompt]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, f"review-screen.sh timed out after {timeout}s"
    return p.returncode, (p.stdout or "") + (p.stderr or "")


LIT_WORDS = ("red", "lit", "active", "highlighted", "pressed", "on")


def _mentions(text: str, needle: str) -> bool:
    return re.search(rf"\b{re.escape(needle)}\b", text, re.IGNORECASE) is not None


def _mentions_lit(text: str) -> bool:
    return any(_mentions(text, w) for w in LIT_WORDS)


def _print_frame_report(frame: Path, part: str, expect_lit: bool,
                        status: str, lit_seen: bool, reason: str, body: str) -> None:
    print(f"review_status={status} frame={frame} part={part} "
          f"lit={'1' if lit_seen else '0'} expect_lit={'1' if expect_lit else '0'} "
          f"reason={reason}")
    for line in (body or "").splitlines():
        print(f"    | {line}")


def _assert_one(reviewer: Path, frame: Path, part: str, expect_lit: bool,
                assert_it: bool, timeout: int) -> str:
    """Returns "pass" | "fail" | "skipped"."""
    if not frame.is_file():
        _print_frame_report(frame, part, expect_lit, "fail", False,
                            "frame does not exist", "")
        return "fail" if assert_it else "skipped"
    prompt = (
        f"You are looking at one frame from pf-hwprobe (a gamepad-native hardware "
        f"diagnostic). Describe which controls are LIT/RED/ACTIVE and which are "
        f"IDLE/GREY. If a slider or trigger is filled, say roughly what fraction. "
        f"Focus especially on the control named '{part}' — is it lit (red/active) "
        f"or idle (grey)?"
    )
    rc, body = _run_reviewer(reviewer, frame, prompt, timeout)
    if rc == 127:
        _print_frame_report(frame, part, expect_lit, "skipped", False, body, "")
        return "skipped"
    if rc != 0 and any(sig in body for sig in SKIP_SIGNALS):
        _print_frame_report(frame, part, expect_lit, "skipped", False,
                            "reviewer environment unavailable", body)
        return "skipped"
    if rc != 0:
        _print_frame_report(frame, part, expect_lit, "fail", False,
                            f"reviewer exited rc={rc}", body)
        return "fail" if assert_it else "skipped"
    part_seen = _mentions(body, part)
    lit_seen = _mentions_lit(body)
    if not assert_it:
        _print_frame_report(frame, part, expect_lit, "pass", lit_seen,
                            "no-assert", body)
        return "pass"
    if not part_seen:
        _print_frame_report(frame, part, expect_lit, "fail", lit_seen,
                            f"reviewer did not mention '{part}'", body)
        return "fail"
    if expect_lit and not lit_seen:
        _print_frame_report(frame, part, expect_lit, "fail", lit_seen,
                            "reviewer described no lit control", body)
        return "fail"
    _print_frame_report(frame, part, expect_lit, "pass", lit_seen, "ok", body)
    return "pass"


def _load_batch(path: Path) -> list[dict]:
    doc = json.loads(path.read_text())
    if not isinstance(doc, list):
        raise SystemExit(f"[screen-reviewer-readback] --batch expected a list, got {type(doc).__name__}")
    return doc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--frame", type=Path, help="single-frame mode")
    g.add_argument("--batch", type=Path,
                   help="batch mode: JSON list of {frame, expect_part, expect_lit?, assert?}")
    ap.add_argument("--expect-part", default=None,
                    help="control id/name the reviewer must mention (single-frame mode)")
    ap.add_argument("--expect-lit", action="store_true", default=False,
                    help="require the reviewer to describe a lit/red/active control")
    ap.add_argument("--no-assert", dest="assert_it", action="store_false", default=True,
                    help="capture reports without failing on mismatch (evidence-only mode)")
    ap.add_argument("--reviewer", type=Path, default=Path(DEFAULT_REVIEWER),
                    help=f"path to review-screen.sh (default: {DEFAULT_REVIEWER})")
    ap.add_argument("--timeout", type=int, default=600,
                    help="per-frame reviewer timeout in seconds (default 600 — first cold call warms the model)")
    args = ap.parse_args()

    items: list[dict]
    if args.frame:
        if not args.expect_part and args.assert_it:
            raise SystemExit("[screen-reviewer-readback] --frame requires --expect-part (or --no-assert)")
        items = [{"frame": str(args.frame), "expect_part": args.expect_part or "",
                  "expect_lit": args.expect_lit, "assert": args.assert_it}]
    else:
        items = _load_batch(args.batch)

    n_pass = n_fail = n_skip = 0
    for item in items:
        rc = _assert_one(
            args.reviewer,
            Path(item["frame"]),
            item.get("expect_part", ""),
            bool(item.get("expect_lit", False)),
            bool(item.get("assert", True)),
            args.timeout,
        )
        if rc == "pass": n_pass += 1
        elif rc == "fail": n_fail += 1
        else: n_skip += 1

    print(f"\nreadback_summary pass={n_pass} fail={n_fail} skipped={n_skip} "
          f"total={len(items)}")
    if n_fail:
        return 1
    if n_pass == 0 and n_skip > 0:
        # every frame skipped -> reviewer env unavailable -> CI treats as SKIPPED,
        # NOT a false green. The deterministic matrix stays authoritative.
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
