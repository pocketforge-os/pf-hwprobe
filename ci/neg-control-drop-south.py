#!/usr/bin/env python3
"""neg-control-drop-south.py — infra-113 B1 NEGATIVE CONTROL (tsp-65jc.3). DO NOT MERGE.

Deletes exactly ONE descriptor-backed assertion input — the a133 `[[inputs]]` row with
`id = "south"` (the A button, EV_KEY/BTN_A) — from a `capabilities.toml`, to PROVE the
hwprobe-smoke CI gate goes RED on descriptor-level breakage. With the "south" row gone,
check-control's HEADLINE assertion (`dev.press("south")`) can no longer bind, so the gate
FAILS — which is the whole point of the negative control.

Runs inside the cloned sim image build (see ci/neg-control-break-a133-south.sh, which stages
this file and injects the invoking RUN into the sim's Dockerfile). This file exists only on a
throwaway branch whose PR closes UNMERGED; the recorded RED run URL is the deliverable.
Usage: neg-control-drop-south.py <capabilities.toml>
"""
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()

# Match the whole array-of-tables entry: the `[[inputs]]` header, its `id = "south"` line,
# and every following field line up to (not including) the next `[[` table header.
block = re.compile(r'\[\[inputs\]\]\nid = "south"\n(?:(?!\[\[)[^\n]*\n)*', re.MULTILINE)
new_text, n = block.subn("", text, count=1)
if n != 1:
    sys.exit(f"NEGATIVE CONTROL FAILED: expected to drop exactly 1 a133 'south' row, dropped {n}")

path.write_text(new_text)
print(f"NEGATIVE CONTROL: dropped a133 [[inputs]] id=south from {path}")
