#!/usr/bin/env bash
# neg-control-break-a133-south.sh — infra-113 B1 NEGATIVE CONTROL (tsp-65jc.3). DO NOT MERGE.
#
# hwprobe-smoke builds the pinned sim image (which bakes the platform a133 descriptor) and runs
# check-control against pf-hwprobe.arm64. To prove the gate goes RED on DESCRIPTOR-LEVEL breakage
# we cannot edit the pinned sim in-place — so this stages a descriptor drop into the cloned sim
# BEFORE `docker build`: it copies the drop helper into the sim clone's build context and injects a
# RUN into the sim's Dockerfile (right after the platform descriptors are COPYed in) that deletes
# the a133 "south" [[inputs]] row. With "south" gone, check-control's HEADLINE (dev.press("south"))
# can no longer bind and hwprobe-smoke goes RED. The RED run URL is the deliverable; PR closes UNMERGED.
#
# Arg 1: path to the sim clone (default .cache/sim-clone, matching hwprobe-smoke.yml).
set -euo pipefail
SIM_CLONE="${1:-.cache/sim-clone}"
HERE="$(cd "$(dirname "$0")" && pwd)"

cp "$HERE/neg-control-drop-south.py" "$SIM_CLONE/docker/neg-control-drop-south.py"

python3 - "$SIM_CLONE/Dockerfile" <<'PY'
import pathlib, sys
df = pathlib.Path(sys.argv[1])
lines = df.read_text().splitlines(keepends=True)
out, injected = [], False
for ln in lines:
    out.append(ln)
    if (not injected) and ln.lstrip().startswith("COPY --from=platform") and "/opt/pf/platform" in ln:
        out.append("RUN python3 /opt/sim/docker/neg-control-drop-south.py "
                   "/opt/pf/platform/devices/a133/capabilities.toml\n")
        injected = True
if not injected:
    sys.exit("anchor 'COPY --from=platform ... /opt/pf/platform' not found in sim Dockerfile — sim pin drift?")
df.write_text("".join(out))
print("patched cloned sim Dockerfile: negative-control drop of a133 south")
PY

echo "NEGATIVE CONTROL staged in $SIM_CLONE"
