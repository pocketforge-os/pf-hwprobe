#!/usr/bin/env bash
# workflow-host-route-test.sh — static host-route guard for this repo's CI workflows.
#
# Incident tsp-ihne1.56 (bead tsp-ihne1.56.5): a sim harness reached the RUNNER HOST's input
# stack because CI passed the host kernel input node into the container (--device), bound the
# host event directory (-v /dev/input), and widened the device cgroup for char major 13.
# uinput is host-global: a container with the node reaches every seat on the machine, so
# those flags are deleted, not guarded. check-control now rides the sim's per-run private
# FIFO control surface (sim control/control_surface.py, FifoSynth) — the same route the
# sim repo's own headless-suite job uses — and needs no host kernel input node at all.
#
# This test inspects the COMPLETE generated docker argv of every `docker run` in
# .github/workflows (joining backslash continuations first, then shlex-tokenizing — a raw
# line regex would miss split-argument spellings) and fails on:
#   host_uinput_device       any argv token carrying the host uinput node path
#   host_input_bind          any argv token carrying the host input event dir (bind/mount)
#   input_major_wildcard     any --device-cgroup-rule widening char major 13
#   host_display_projection  DISPLAY/XAUTHORITY/WAYLAND_DISPLAY env projection or
#                            X11/wayland socket binds into the container
#   host_uinput_permission   runner-side module load, `sg input`, chmod/chgrp/install on
#                            the node, modules-load.d persistence, host input automation
#   unparseable_command      a docker command the tokenizer cannot split (fail closed)
# Full mode additionally asserts the POSITIVE controls: the smoke gate still runs — both
# check-control invocations (BLOCKING + ADVISORY) are still present, each with the nesting
# caps, --network none, the APP_ARM64 env projection and the read-only build bind.
# Migration must not delete the suite.
#
# Usage:
#   tests/workflow-host-route-test.sh --self-test   # seeded reds + greens, one invocation
#   tests/workflow-host-route-test.sh               # scan .github/workflows
#
# Env: PF_HOST_ROUTE_ROOT (repo root override), PF_HOST_ROUTE_PYTHON (interpreter override).
# Stdlib-only Python (the runner lane is not required to carry PyYAML).
set -euo pipefail

repo="${PF_HOST_ROUTE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

PY="${PF_HOST_ROUTE_PYTHON:-}"
if [ -z "$PY" ]; then
  for c in python3.12 python3.11 python3; do
    if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
  done
fi
[ -n "$PY" ] || { echo "FATAL: no python3 interpreter found (host-route guard)" >&2; exit 3; }

exec "$PY" - "$repo/.github/workflows" "${1:-}" <<'PY'
import os
import re
import shlex
import sys

workflows_dir = sys.argv[1]
mode = sys.argv[2] if len(sys.argv) > 2 else ""

# ---------------------------------------------------------------- rule patterns
# Applied to the shlex tokens of a joined `docker run` logical line.
TOKEN_RULES = [
    ("host_uinput_device", re.compile(r"/dev/uinput")),
    ("host_input_bind", re.compile(r"/dev/input(?![A-Za-z0-9_.-])")),
    ("host_display_projection", re.compile(r"/tmp/\.X11-unix")),
    ("host_display_projection", re.compile(r"\.Xauthority")),
    ("host_display_projection", re.compile(r"(?:^|[/,=])wayland-\d")),
    ("host_display_projection", re.compile(r"/run/user/\d+/wayland")),
    ("host_display_projection", re.compile(r"(?:^|[,\s=])(?:DISPLAY|WAYLAND_DISPLAY|XAUTHORITY)(?:=|$)")),
]
# Flags whose VALUE is the next token (GNU long `--flag=value` handled inline).
PAIR_FLAGS = {
    "--device", "--device-cgroup-rule", "-v", "--volume", "--mount",
    "-e", "--env",
}
CGROUP_MAJOR13 = re.compile(r"^c\s+13\s*:")

# Text-level rules applied to every joined logical line (docker or not).
TEXT_RULES = [
    ("host_uinput_permission", re.compile(r"\bmodprobe\s+(?:-\S+\s+)*uinput\b")),
    ("host_uinput_permission", re.compile(r"\bsg\s+input\b")),
    ("host_uinput_permission", re.compile(r"\b(?:chmod|chgrp|chown|setfacl|install)\b[^|;&\n]*/dev/(?:uinput|input)")),
    ("host_uinput_permission", re.compile(r"modules-load\.d/uinput")),
    ("host_input_tool", re.compile(r"\b(?:xdotool|xclip|xsel|wl-copy|wl-paste|xhost)\b")),
]

DOCKER_RUN = re.compile(r"\bdocker\s+run\b")


def logical_lines(text):
    """Yield (start_lineno_1based, joined_line) with backslash continuations joined."""
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        start = i + 1
        joined = lines[i]
        while joined.rstrip().endswith("\\") and i + 1 < len(lines):
            joined = joined.rstrip()[:-1] + " " + lines[i + 1].strip()
            i += 1
        yield start, joined
        i += 1


def scan_text(text):
    """Return findings: list of (rule, lineno, detail)."""
    findings = []
    for lineno, line in logical_lines(text):
        for rule, pat in TEXT_RULES:
            m = pat.search(line)
            if m:
                findings.append((rule, lineno, m.group(0)))
        if not DOCKER_RUN.search(line):
            continue
        try:
            tokens = shlex.split(line, comments=False)
        except ValueError as exc:
            findings.append(("unparseable_command", lineno, str(exc)))
            continue
        i = 0
        n = len(tokens)
        while i < n:
            tok = tokens[i]
            for rule, pat in TOKEN_RULES:
                if pat.search(tok):
                    findings.append((rule, lineno, tok))
            if tok == "--device-cgroup-rule":
                if i + 1 >= n:
                    findings.append(("unparseable_command", lineno, "truncated --device-cgroup-rule"))
                elif CGROUP_MAJOR13.match(tokens[i + 1]):
                    findings.append(("input_major_wildcard", lineno, tokens[i + 1]))
            elif tok.startswith("--device-cgroup-rule="):
                value = tok.split("=", 1)[1]
                if CGROUP_MAJOR13.match(value):
                    findings.append(("input_major_wildcard", lineno, value))
            if tok in PAIR_FLAGS and i + 1 >= n:
                findings.append(("unparseable_command", lineno, f"truncated {tok}"))
            i += 1
    return findings


def docker_run_commands(text):
    """Return the shlex token lists of every joined `docker run` logical line."""
    cmds = []
    for _lineno, line in logical_lines(text):
        if not DOCKER_RUN.search(line):
            continue
        try:
            cmds.append(shlex.split(line, comments=False))
        except ValueError:
            cmds.append(None)  # unparseable: scan_text already reported it
    return cmds


def positive_controls_hwprobe(text):
    """The migration keeps the smoke gate: BOTH check-control invocations (blocking +
    advisory) still run, each keeping the nesting caps, --network none, the APP_ARM64
    projection and the read-only build bind. Returns failure strings."""
    problems = []
    check_runs = 0
    for tokens in docker_run_commands(text):
        if tokens is None:
            continue
        if not any(re.fullmatch(r"check-control", t) for t in tokens):
            continue
        check_runs += 1
        if "--cap-add" not in tokens or "SYS_ADMIN" not in tokens:
            problems.append("positive-control: a check-control run lost the nesting caps (SYS_ADMIN)")
        if "--network" not in tokens or "none" not in tokens:
            problems.append("positive-control: a check-control run lost --network none")
        if not any(t.startswith("APP_ARM64=") for t in tokens):
            problems.append("positive-control: a check-control run lost the APP_ARM64 projection")
        if not any(re.search(r"/pf-hwprobe-build", t) for t in tokens):
            problems.append("positive-control: a check-control run lost the build bind")
        if not any(t == "qemu" for t in tokens):
            problems.append("positive-control: a check-control run lost --only-launcher qemu")
    if check_runs < 2:
        problems.append(
            f"positive-control: {check_runs} check-control `docker run` command(s); "
            "expected >= 2 (blocking + advisory)")
    return problems


# ---------------------------------------------------------------- self-test
SELF_TEST_FIXTURES = [
    # (name, text, expect_rule_or_None) — reds must produce the named rule; greens none.
    ("red_device_split_continuation",
     "run: |\n  docker run --rm --device \\\n    /dev/uinput \"$IMG\" check-control a133\n",
     "host_uinput_device"),
    ("red_device_equals_form",
     "run: docker run --rm --device=/dev/uinput img cmd\n",
     "host_uinput_device"),
    ("red_input_bind_v",
     "run: docker run --rm -v /dev/input:/dev/input img cmd\n",
     "host_input_bind"),
    ("red_input_mount_long",
     "run: docker run --rm --mount type=bind,src=/dev/input,dst=/dev/input img cmd\n",
     "host_input_bind"),
    ("red_cgroup_major13",
     'run: docker run --rm --device-cgroup-rule "c 13:* rmw" img cmd\n',
     "input_major_wildcard"),
    ("red_cgroup_major13_spacing",
     "run: docker run --rm --device-cgroup-rule 'c  13:*  rmw' img cmd\n",
     "input_major_wildcard"),
    ("red_display_env",
     'run: docker run --rm -e DISPLAY="$DISPLAY" img cmd\n',
     "host_display_projection"),
    ("red_wayland_env",
     "run: docker run --rm -e WAYLAND_DISPLAY=wayland-0 img cmd\n",
     "host_display_projection"),
    ("red_x11_socket_bind",
     "run: docker run --rm -v /tmp/.X11-unix:/tmp/.X11-unix img cmd\n",
     "host_display_projection"),
    ("red_xauthority_bind",
     "run: docker run --rm -v /home/mm/.Xauthority:/root/.Xauthority:ro img cmd\n",
     "host_display_projection"),
    ("red_wayland_socket_bind",
     "run: docker run --rm -v /run/user/1000/wayland-0:/tmp/wayland-0 img cmd\n",
     "host_display_projection"),
    ("red_modprobe",
     "run: sudo modprobe uinput || true\n",
     "host_uinput_permission"),
    ("red_modprobe_in_stale_comment",
     "# `sudo modprobe uinput` can never work — the module is guaranteed host-side\n",
     "host_uinput_permission"),
    ("red_sg_input",
     'run: sg input -c "./ci/check-control-hwprobe.py --device a133"\n',
     "host_uinput_permission"),
    ("red_chmod_node",
     "run: sudo chmod 0666 /dev/uinput\n",
     "host_uinput_permission"),
    ("red_modules_load_persistence",
     "run: echo uinput | sudo tee /etc/modules-load.d/uinput.conf\n",
     "host_uinput_permission"),
    ("red_xdotool",
     "run: xdotool key --clearmodifiers space\n",
     "host_input_tool"),
    ("red_unparseable_fail_closed",
     "run: docker run --rm --device 'unbalanced img cmd\n",
     "unparseable_command"),
    ("green_migrated_argv",
     "run: |\n"
     "  docker run --rm --network none \\\n"
     "    --cap-add SYS_ADMIN --security-opt apparmor=unconfined --security-opt seccomp=unconfined \\\n"
     '    -v "$PWD/build:/pf-hwprobe-build:ro" \\\n'
     "    -e APP_ARM64=/pf-hwprobe-build/pf-hwprobe.arm64 \\\n"
     '    "$SIM_IMG" check-control --only-launcher qemu --label pf-hwprobe $BLOCKING\n',
     None),
    ("green_doc_only_prose",
     "# input rides the sim's private per-run FIFOs; the host kernel node is never opened,\n"
     "# bound, or module-loaded, and no seat variable is projected (incident tsp-ihne1.56.5).\n",
     None),
    ("green_caps_helper",
     'run: caps() { docker run --rm "$SIM_IMG" shell -lc "python3 /opt/pf/platform/core/caps.py $*" 2>/dev/null; }\n',
     None),
    ("green_workspace_chown_reclaim",
     'run: docker run --rm -v "$PWD:/w" debian:bookworm chown -R "$(id -u):$(id -g)" /w || true\n',
     None),
]


def run_self_test():
    failures = []
    for name, text, expect in SELF_TEST_FIXTURES:
        found = scan_text(text)
        rules = {rule for rule, _ln, _d in found}
        if expect is None:
            if found:
                failures.append(f"{name}: expected PASS, got findings {sorted(rules)}")
            else:
                print(f"self-test {name}: PASS (green control clean)")
        else:
            if expect in rules:
                print(f"self-test {name}: PASS (caught {expect})")
            else:
                failures.append(f"{name}: expected rule {expect}, got {sorted(rules) or 'no findings'}")
    if failures:
        for f in failures:
            print(f"host_route_guard=FAIL reason=self_test path=<fixture> line=0 rule=self_test detail={f}")
        print(f"SELF-TEST FAIL ({len(failures)} fixture(s) misbehaved)")
        return 1
    print(f"SELF-TEST PASS ({len(SELF_TEST_FIXTURES)} fixtures: "
          f"{sum(1 for _n, _t, e in SELF_TEST_FIXTURES if e)} reds caught, "
          f"{sum(1 for _n, _t, e in SELF_TEST_FIXTURES if e is None)} greens clean)")
    return 0


# ---------------------------------------------------------------- full scan
def run_full_scan():
    if not os.path.isdir(workflows_dir):
        print(f"host_route_guard=FAIL reason=missing_workflows_dir path={workflows_dir} line=0 rule=meta")
        return 1
    names = sorted(n for n in os.listdir(workflows_dir) if n.endswith((".yml", ".yaml")))
    if not names:
        print(f"host_route_guard=FAIL reason=no_workflow_files path={workflows_dir} line=0 rule=meta")
        return 1
    failures = 0
    for name in names:
        path = os.path.join(workflows_dir, name)
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        findings = scan_text(text)
        for rule, lineno, detail in findings:
            failures += 1
            print(f"host_route_guard=FAIL reason={rule} path={path} line={lineno} "
                  f"rule={rule} detail={detail!r}")
        if not findings:
            print(f"host_route_guard=PASS path={path} (no host input/display route)")
        if name == "hwprobe-smoke.yml":
            problems = positive_controls_hwprobe(text)
            for problem in problems:
                failures += 1
                print(f"host_route_guard=FAIL reason=positive_control path={path} line=0 "
                      f"rule=positive_control detail={problem!r}")
            if not problems:
                print(f"host_route_guard=PASS path={path} positive-controls "
                      "(blocking+advisory check-control runs keep SYS_ADMIN, --network none, "
                      "APP_ARM64, the build bind and --only-launcher qemu)")
    if failures:
        print(f"FAIL: {failures} host-route finding(s)")
        return 1
    print(f"PASS: {len(names)} workflow(s) carry no host input/display route")
    return 0


if mode == "--self-test":
    sys.exit(run_self_test())
elif mode == "":
    sys.exit(run_full_scan())
else:
    print(f"usage: {sys.argv[0]} [--self-test]", file=sys.stderr)
    sys.exit(2)
PY
