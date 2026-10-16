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
#   host_uinput_device       any argv token carrying the host uinput node path, or a shell
#                            assignment whose value is that path
#   host_input_bind          any argv token carrying the host input event dir (bind/mount),
#                            or an assignment whose value is that path
#   input_major_wildcard     any --device-cgroup-rule that can reach an input or uinput
#                            node: char major 13, the uinput misc node (c 10:223), or a
#                            wildcard grant (c/a/b with major or minor `*`)
#   host_device_wildcard     --privileged (grants every host device, including input nodes)
#   host_path_bind           a STATIC bind source that is (or is an ancestor of) a
#                            protected host path: /, /dev, /dev/input, /dev/uinput, /etc,
#                            /home, /root, /run, /run/user[/<id>], /tmp, /tmp/.X11-unix
#   host_env_file            any --env-file (contents are unprovable from the workflow)
#   host_display_projection  DISPLAY/XAUTHORITY/WAYLAND_DISPLAY env projection, X11/wayland
#                            socket binds, or assignments carrying those paths
#   host_uinput_permission   runner-side module load, `sg input`, chmod/chgrp/install on
#                            the node, modules-load.d persistence, assignments carrying
#                            host input tool names, host input automation tools
#   dynamic_route            FAIL CLOSED on shell indirection: a dynamic value ($var,
#                            ${arr[@]}, `cmd`) in a sensitive docker option position
#                            (--device, --device-cgroup-rule, --cap-add, -v/--volume/
#                            --mount, -e/--env), a dynamic modprobe/sg/permission-command
#                            argument, when the runtime value cannot be proven safe. The
#                            only dynamic bind source allowed is the workspace itself
#                            (anchored at a runner workspace root: $GITHUB_WORKSPACE / ${GITHUB_WORKSPACE} / $RUNNER_TEMP / ${RUNNER_TEMP} / ${{ github.workspace }} / ${{ runner.temp }}, exact or as a path prefix; $PWD and $HOME are NOT workspace anchors). shlex does NOT
#                            expand variables, so `node=/dev/uinput; docker run --device
#                            "$node"` is invisible to a literal scan — hence this rule.
#                            WHOLE-ARGV expansions: an UNQUOTED $VAR, ${VAR}, $(cmd) or
#                            `cmd` word anywhere in the Docker OPTION region (any token
#                            before the image operand, including the image slot and
#                            consumed flag values) word-splits at runtime and can inject
#                            ANY option (--privileged, --device, binds, env) past the
#                            static scan -> denied. QUOTED expansions ("$FLAGS",
#                            "${FLAGS}", "$(route_args)", "`route_args`") do not word-
#                            split, but the resulting single word can still BE a
#                            complete Docker option (--privileged, --device=/dev/
#                            uinput) or an option-shaped image, so every quoted
#                            expansion in the option region (including the image-slot
#                            candidate) is denied too. No text scan can prove a
#                            variable's runtime value (printf -v, read, eval/indirect,
#                            a sourced script or a $GITHUB_ENV write all mutate it
#                            outside the NAME=value grammar), so the ONLY accepted
#                            dynamic image operand is the word right after pflag's
#                            `--` end-of-options delimiter: docker parses that word
#                            unconditionally as the image reference, so an option-
#                            shaped runtime value fails as an invalid image instead of
#                            becoming a Docker option. An option-shaped word directly
#                            after `--`, or a `--` with no following word, fails closed.
#                            Expansions AFTER the image operand (the
#                            steps.matrix.outputs.* device list) are container
#                            arguments, inert for Docker option parsing. ATTACHED short-option spellings
#                            (`-v/dev:/dev`, `-eDISPLAY`, pflag-combined
#                            `-tv/dev:/dev`, `-v=/dev:/dev`) are split and face the
#                            SAME deny model as the spaced spelling.
#   unparseable_command      a docker command the tokenizer cannot split, or whose shell
#                            word walk and shlex tokenization disagree (fail closed)
# Full mode additionally asserts the POSITIVE controls on hwprobe-smoke.yml: the smoke gate
# still runs — EXACTLY one BLOCKING and one ADVISORY check-control `docker run` (classified
# by their steps.matrix.outputs.{blocking,advisory} device-list argument). Each requirement
# is validated IN THE ARGV REGION WHERE ITS CONSUMER INTERPRETS IT: `--cap-add SYS_ADMIN`,
# `--network none`, the exact `-e APP_ARM64=/pf-hwprobe-build/pf-hwprobe.arm64`, the exact
# read-only `-v $GITHUB_WORKSPACE/build:/pf-hwprobe-build:ro` and `--rm` must be Docker OPTIONS before
# the image operand (the same strings appended after the image are inert application
# arguments and do NOT satisfy the control), and `--only-launcher qemu` must be a pair
# AFTER check-control in the container-argument region. Migration must not delete or
# degrade the suite.
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

# Sensitive docker options: their VALUE decides whether a host device, path or seat
# variable reaches the container. A dynamic (shell-expanded) value cannot be proven
# safe statically -> dynamic_route, fail closed (design-plan §3 pattern family).
SENSITIVE_VALUE_FLAGS = {
    "--device", "--device-cgroup-rule", "--cap-add",
    "-v", "--volume", "--mount", "-e", "--env",
}
BIND_FLAGS = {"-v", "--volume", "--mount"}
# Docker short options accept ATTACHED values (`-v/dev:/dev` == `-v /dev:/dev`,
# `-eDISPLAY` == `-e DISPLAY`). They must be split before the deny model runs.
SHORT_VALUE_FLAGS = ("-v", "-e")
# Amended canonical policy (runtime host-route-policy v1, input-coord 2026-10-16):
# a dynamic bind source is allowed only when anchored at a runner workspace root.
# $PWD, $HOME, unanchored variables and whole-argv expansions stay red.
WORKSPACE_PREFIXES = (
    "$GITHUB_WORKSPACE/", "${GITHUB_WORKSPACE}/",
    "$RUNNER_TEMP/", "${RUNNER_TEMP}/",
    "${{ github.workspace }}/", "${{ runner.temp }}/",
)
WORKSPACE_EXACT = (
    "$GITHUB_WORKSPACE", "${GITHUB_WORKSPACE}",
    "$RUNNER_TEMP", "${RUNNER_TEMP}",
    "${{ github.workspace }}", "${{ runner.temp }}",
)
ARRAY_EXPANSION = re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*\[[@*]\]\}")
DYNAMIC = re.compile(r"[$`]")

# Static bind sources that are (or contain, or sit above) host device/seat paths.
PROTECTED_BIND_SOURCES = {
    "/", "/dev", "/dev/input", "/dev/uinput", "/etc", "/home", "/root",
    "/run", "/run/user", "/tmp", "/tmp/.X11-unix",
}
RUN_USER_RE = re.compile(r"^/run/user(?:/\d+)?(?:/.*)?$")

CGROUP_RULE = re.compile(r"^([abc]) (\*|\d+):(\*|\d+)(?: [a-z]+)?$")

# Text-level rules applied to every joined logical line (docker or not).
TEXT_RULES = [
    ("host_uinput_permission", re.compile(r"\bmodprobe\s+(?:-\S+\s+)*uinput\b")),
    ("host_uinput_permission", re.compile(r"\bsg\s+input\b")),
    ("host_uinput_permission", re.compile(r"\b(?:chmod|chgrp|chown|setfacl|install)\b[^|;&\n]*/dev/(?:uinput|input)")),
    ("host_uinput_permission", re.compile(r"modules-load\.d/uinput")),
    ("host_input_tool", re.compile(r"\b(?:xdotool|xclip|xsel|wl-copy|wl-paste|xhost)\b")),
    # Fail closed on dynamic module/group arguments: `mod=uinput; modprobe "$mod"`.
    ("dynamic_route", re.compile(r"\bmodprobe\b[^|;&\n]*[$`]")),
    ("dynamic_route", re.compile(r"\bsg\s+[^|;&\n]*[$`]")),
]

# Assignments whose VALUE names a protected path/tool: the literal-scan evasion
# `node=/dev/uinput; docker run --device "$node"` plants the literal HERE.
ASSIGN_RULE = re.compile(
    r"""(?:^|[\s;|&({])(?:export\s+)?[A-Za-z_][A-Za-z0-9_]*=["']?"""
    r"(/dev/uinput\b|/dev/input\b|/tmp/\.X11-unix\b|/run/user/\d+/(?:wayland|bus)\S*"
    r"|xdotool\b|xclip\b|xsel\b|wl-copy\b|wl-paste\b|xhost\b|uinput\b)"
)
ASSIGN_VALUE_RULE = [
    (re.compile(r"^/dev/uinput\b"), "host_uinput_device"),
    (re.compile(r"^/dev/input\b"), "host_input_bind"),
    (re.compile(r"^/tmp/\.X11-unix\b|^/run/user/"), "host_display_projection"),
    (re.compile(r"^(?:xdotool|xclip|xsel|wl-copy|wl-paste|xhost)\b"), "host_input_tool"),
    (re.compile(r"^uinput\b"), "host_uinput_permission"),
]

# Permission commands with dynamic arguments hide the target: `n=/dev/uinput; chmod "$n"`.
# The ownership-reclaim idiom ($(id -u)/$(id -g)) is the only provably-safe dynamic here.
PERM_CMD = re.compile(r"\b(?:chmod|chgrp|chown|setfacl|install)\b([^|;&\n]*)")
OWNERSHIP_IDIOMS = (
    "$(id -u):$(id -g)", "$(id -un):$(id -gn)", "$(id -u)", "$(id -g)",
    "$(id -un)", "$(id -gn)",
)

DOCKER_RUN = re.compile(r"\bdocker\s+run\b")

# Docker flags that never take a value. Any OTHER unknown `-flag` consumes the
# following non-dash word as its value (fail closed: a mis-parsed value could
# otherwise be mistaken for the image operand and end option parsing too early).
DOCKER_BOOL_FLAGS = {
    "--rm", "--privileged", "--init", "--read-only", "--detach", "-d",
    "--interactive", "-i", "--tty", "-t", "-it", "--publish-all", "-P",
    "--no-healthcheck", "--sig-proxy",
}

# Single-char boolean shorthands pflag may combine ahead of an attached value
# (`-tv/dev:/dev` == `-t -v /dev:/dev`).
SHORT_BOOL_FLAGS = frozenset(
    f for f in DOCKER_BOOL_FLAGS if len(f) == 2 and not f.startswith("--")
)

# Quoting prevents word splitting but NOT the single resulting word from being a
# complete Docker option (`FLAGS=--privileged; docker run "$FLAGS" img` runs
# privileged) or an option-shaped image. No scan of the workflow TEXT can prove a
# variable's runtime value: channels outside the NAME=value grammar (printf -v,
# read, eval/indirect, a sourced script, a $GITHUB_ENV write) mutate it after any
# regex-visible assignment. Every dynamic (quoted or unquoted) expansion in the
# option region therefore fails closed. The ONLY accepted dynamic image operand is
# a word right after pflag's `--` end-of-options delimiter: docker parses the word
# after `--` unconditionally as the image reference, so an option-shaped runtime
# value fails as an invalid image instead of becoming a Docker option.
DYNAMIC_IMAGE_HINT = ("a dynamic image operand is only allowed right after the "
                      "`--` end-of-options delimiter")


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


def raw_word_flags(line):
    """One state per whitespace-separated shell word: "unquoted" when the word
    carries a `$` or backtick OUTSIDE any quotes (word-splits at runtime, so a
    single token like $FLAGS or $(route_args) can inject WHOLE docker options);
    "quoted" when it carries expansions only inside double quotes (stays one
    word, but that word can still evaluate to a complete option or an
    option-shaped image); else "static" (single-quoted or plain text). Aligned
    1:1 with the shlex.split() tokens of the same line (callers must verify the
    lengths)."""
    flags = []
    have_word = False
    unquoted_dyn = False
    quoted_dyn = False

    def state():
        return "unquoted" if unquoted_dyn else "quoted" if quoted_dyn else "static"

    in_single = in_double = esc = False
    for ch in line:
        if esc:
            esc = False
            continue
        if in_single:
            if ch == "'":
                in_single = False
            continue
        if in_double:
            if ch == "\\":
                esc = True
            elif ch == '"':
                in_double = False
            elif ch in "$`":
                quoted_dyn = True
            continue
        if ch == "\\":
            esc = True
            continue
        if ch == "'":
            have_word = True
            in_single = True
            continue
        if ch == '"':
            have_word = True
            in_double = True
            continue
        if ch.isspace():
            if have_word:
                flags.append(state())
            have_word = False
            unquoted_dyn = quoted_dyn = False
            continue
        have_word = True
        if ch in "$`":
            unquoted_dyn = True
    if have_word:
        flags.append(state())
    return flags


def attached_short_pair(tok):
    """Split docker's attached-value short-option spelling into (flag, value);
    None when tok is not one. `-v/dev:/dev` -> ("-v", "/dev:/dev"); pflag also
    accepts `-v=/dev:/dev` and combined shorthands (`-tv/dev:/dev`)."""
    if not tok.startswith("-") or tok.startswith("--"):
        return None
    body = tok[1:]
    while body:
        flag = "-" + body[0]
        if flag in SHORT_VALUE_FLAGS:
            if len(body) < 2:
                return None
            value = body[1:]
            if value.startswith("="):
                value = value[1:]
            return flag, value
        if flag in SHORT_BOOL_FLAGS:
            body = body[1:]
            continue
        return None
    return None


def docker_regions(tokens):
    """Split one `docker run` token list into (option_tokens, image_idx, app_tokens).
    The image is the first positional operand after `docker run`; everything after it
    is the container command + arguments (inert as far as Docker option parsing goes).
    image_idx is None when no positional operand exists."""
    try:
        start = tokens.index("run", tokens.index("docker"))
    except ValueError:
        return tokens, None, []
    i = start + 1
    n = len(tokens)
    while i < n:
        tok = tokens[i]
        if tok == "--":
            # pflag end-of-options delimiter: the next word is unconditionally
            # the image operand.
            if i + 1 < n:
                return tokens[:i], i + 1, tokens[i + 2:]
            return tokens, None, []
        if tok.startswith("-"):
            if tok in DOCKER_BOOL_FLAGS or "=" in tok or attached_short_pair(tok):
                i += 1  # self-contained (boolean, inline `=`, or attached value)
            elif i + 1 < n and not tokens[i + 1].startswith("-"):
                i += 2  # flag value (sensitive or unknown)
            else:
                i += 1
            continue
        return tokens[:i], i, tokens[i + 1:]
    return tokens, None, []


def bind_source_denied(src):
    """True when a STATIC bind source exposes protected host device/seat paths."""
    if src in PROTECTED_BIND_SOURCES:
        return True
    if RUN_USER_RE.match(src):
        return True
    norm = src.rstrip("/")
    if not norm:  # src == "/" (or "//")
        return True
    return any(p.startswith(norm + "/") for p in PROTECTED_BIND_SOURCES)


def workspace_bind_ok(src):
    return src in WORKSPACE_EXACT or src.startswith(WORKSPACE_PREFIXES)


def cgroup_rule_denied(value):
    """True when a --device-cgroup-rule value can reach an input/uinput node,
    including wildcard grants. Unparseable rules fail closed (denied)."""
    norm = " ".join(value.split())
    m = CGROUP_RULE.match(norm)
    if not m:
        return True
    _typ, major, minor = m.groups()
    if major in ("*", "13"):
        return True
    if major == "10" and minor in ("*", "223"):
        return True
    return False


def mount_src_of(value):
    """Extract the src= field of a --mount value (empty string when absent)."""
    for field in value.split(","):
        if field.startswith("src=") or field.startswith("source="):
            return field.split("=", 1)[1]
    return ""


def check_sensitive_value(flag, value, lineno, findings):
    """Deny model for one docker option value (both `--flag value` and `--flag=value`)."""
    if flag == "--device-cgroup-rule":
        if DYNAMIC.search(value):
            findings.append(("dynamic_route", lineno, f"{flag} {value!r}"))
        elif cgroup_rule_denied(value):
            findings.append(("input_major_wildcard", lineno, value))
        return
    if flag == "--device":
        if DYNAMIC.search(value):
            findings.append(("dynamic_route", lineno, f"{flag} {value!r}"))
        return
    if flag == "--cap-add":
        if DYNAMIC.search(value):
            findings.append(("dynamic_route", lineno, f"{flag} {value!r}"))
        return
    if flag in BIND_FLAGS:
        src = mount_src_of(value) if flag == "--mount" else value.split(":", 1)[0]
        if DYNAMIC.search(value):
            if not workspace_bind_ok(src):
                findings.append(("dynamic_route", lineno, f"{flag} {value!r}"))
            return
        if bind_source_denied(src):
            findings.append(("host_path_bind", lineno, value))
        return
    if flag in ("-e", "--env"):
        if DYNAMIC.search(value):
            findings.append(("dynamic_route", lineno, f"{flag} {value!r}"))
        # A static bare `-e NAME` passes the host env var through; a display
        # variable NAME is already caught by TOKEN_RULES on the literal token.
        return


def token_findings(tok, lineno):
    """Literal-token denials for one argv token (used for plain tokens AND for
    option values consumed positionally, so `--device /dev/uinput` is never skipped)."""
    out = []
    for rule, pat in TOKEN_RULES:
        if pat.search(tok):
            out.append((rule, lineno, tok))
    if ARRAY_EXPANSION.search(tok):
        out.append(("dynamic_route", lineno,
                    f"unprovable array expansion in docker argv: {tok}"))
    return out


def scan_text(text):
    """Return findings: list of (rule, lineno, detail)."""
    findings = []
    for lineno, line in logical_lines(text):
        for rule, pat in TEXT_RULES:
            m = pat.search(line)
            if m:
                findings.append((rule, lineno, m.group(0)))
        m = ASSIGN_RULE.search(line)
        if m:
            value = m.group(1)
            for pat, rule in ASSIGN_VALUE_RULE:
                if pat.match(value):
                    findings.append((rule, lineno, m.group(0).strip()))
                    break
        for pm in PERM_CMD.finditer(line):
            rest = pm.group(1)
            for idiom in OWNERSHIP_IDIOMS:
                rest = rest.replace(idiom, "")
            if DYNAMIC.search(rest):
                findings.append(("dynamic_route", lineno,
                                 "permission command with dynamic argument: "
                                 + pm.group(0).strip()[:60]))
        if not DOCKER_RUN.search(line):
            continue
        try:
            tokens = shlex.split(line, comments=False)
        except ValueError as exc:
            findings.append(("unparseable_command", lineno, str(exc)))
            continue
        raws = raw_word_flags(line)
        if len(raws) != len(tokens):
            # The quote-aware word walk and shlex disagree -> the argv cannot be
            # proven safe statically. Fail closed.
            findings.append(("unparseable_command", lineno,
                             "word/token misalignment in docker run line"))
            continue
        # The Docker option region starts AFTER the `docker run` tokens; a shell
        # prefix on the same logical line (`for suite ...; do`, `if`, `caps() {`)
        # is not part of the argv and must not be mistaken for the image operand.
        try:
            run_idx = tokens.index("run", tokens.index("docker"))
        except ValueError:
            run_idx = None
        i = 0
        n = len(tokens)
        image_seen = False
        while i < n:
            tok = tokens[i]
            findings.extend(token_findings(tok, lineno))
            if run_idx is None or i <= run_idx:
                i += 1
                continue
            if tok == "--privileged":
                findings.append(("host_device_wildcard", lineno, tok))
            if tok == "--env-file" or tok.startswith("--env-file="):
                findings.append(("host_env_file", lineno, tok))
            apair = attached_short_pair(tok)
            if apair is not None:
                # Attached short-option spelling (`-v/dev:/dev`, `-eDISPLAY`):
                # split, then run the SAME deny model as the spaced spelling.
                flag, value = apair
                findings.extend(token_findings(value, lineno))
                check_sensitive_value(flag, value, lineno, findings)
                if not image_seen and raws[i] == "unquoted":
                    findings.append(("dynamic_route", lineno,
                                     "unquoted expansion attached to "
                                     f"{flag} can word-split into extra docker "
                                     f"options: {tok}"))
                i += 1
                continue
            consumed = False
            for flag in SENSITIVE_VALUE_FLAGS:
                if tok == flag:
                    if i + 1 >= n:
                        findings.append(("unparseable_command", lineno, f"truncated {tok}"))
                    else:
                        value = tokens[i + 1]
                        # A consumed option value must still face the literal-token
                        # denials, else `--device /dev/uinput` hides in the value slot.
                        findings.extend(token_findings(value, lineno))
                        check_sensitive_value(flag, value, lineno, findings)
                        if not image_seen and raws[i + 1] == "unquoted":
                            findings.append(("dynamic_route", lineno,
                                             "unquoted expansion as the value of "
                                             f"{tok} can word-split into extra "
                                             f"docker options: {value}"))
                        i += 1
                        consumed = True
                    break
                if tok.startswith(flag + "="):
                    check_sensitive_value(flag, tok.split("=", 1)[1], lineno, findings)
                    consumed = True  # inline form carries its own value
                    break
            if consumed:
                i += 1
                continue
            if not image_seen:
                # Everything before the first positional operand is the Docker
                # OPTION region. An unquoted expansion here ($FLAGS, ${FLAGS},
                # $(cmd), `cmd`) word-splits at runtime and can inject ANY option
                # (--privileged, --device, binds, env) past this static scan.
                if tok == "--":
                    # pflag's end-of-options delimiter: the NEXT word is parsed
                    # unconditionally as the image operand, so a dynamic image
                    # reference can no longer be interpreted as a Docker option.
                    if i + 1 >= n:
                        findings.append(("unparseable_command", lineno,
                                         "truncated `--` delimiter: no image operand "
                                         "follows"))
                    else:
                        nxt = tokens[i + 1]
                        findings.extend(token_findings(nxt, lineno))
                        if nxt.startswith("-"):
                            findings.append(("dynamic_route", lineno,
                                             "option-shaped word follows the `--` "
                                             "delimiter (misplaced delimiter or an "
                                             f"option smuggled into the image slot): "
                                             f"{nxt}"))
                        else:
                            image_seen = True  # the delimiter proves the boundary
                        i += 1
                elif raws[i] == "unquoted":
                    findings.append(("dynamic_route", lineno,
                                     "unquoted shell expansion in the docker option "
                                     "region can inject arbitrary options at runtime: "
                                     f"{tok}"))
                elif tok.startswith("-"):
                    if raws[i] == "quoted":
                        findings.append(("dynamic_route", lineno,
                                         "quoted expansion as a docker flag name can "
                                         "evaluate to an arbitrary option at runtime: "
                                         f"{tok}"))
                    elif ("=" not in tok and tok not in DOCKER_BOOL_FLAGS
                          and i + 1 < n and not tokens[i + 1].startswith("-")):
                        # Unknown non-boolean flag: its value is the next non-dash
                        # word. The flag is not in the audited sensitive set, so an
                        # unresolved value cannot be proven safe -> fail closed.
                        findings.extend(token_findings(tokens[i + 1], lineno))
                        if raws[i + 1] != "static":
                            findings.append(("dynamic_route", lineno,
                                             "unresolved expansion as the value of "
                                             f"unknown docker flag {tok} cannot be "
                                             f"proven safe: {tokens[i + 1]}"))
                        i += 1
                elif raws[i] != "static":
                    # Quoting prevents word splitting but NOT the single word from
                    # evaluating to a complete option (--privileged) or an
                    # option-shaped image, and no text scan can prove a variable's
                    # runtime value. Fail closed; the `--` delimiter is the only
                    # accepted route for a dynamic image operand.
                    findings.append(("dynamic_route", lineno,
                                     "quoted shell expansion in the docker option "
                                     "region can evaluate to a complete docker "
                                     f"option at runtime; {DYNAMIC_IMAGE_HINT}: "
                                     f"{tok}"))
                else:
                    image_seen = True  # first static positional operand = the image
            i += 1
    return findings


def pair_ok(tokens, flag, value):
    """True when `value` is the option value of `flag` (adjacent or --flag=value),
    not merely present somewhere in the argv."""
    for i, tok in enumerate(tokens):
        if tok == flag and i + 1 < len(tokens) and tokens[i + 1] == value:
            return True
        if tok == f"{flag}={value}":
            return True
    return False


BLOCKING_RE = re.compile(r"steps\.matrix\.outputs\.blocking")
ADVISORY_RE = re.compile(r"steps\.matrix\.outputs\.advisory")
APP_ARM64_ENV = "APP_ARM64=/pf-hwprobe-build/pf-hwprobe.arm64"
BUILD_BIND = "$GITHUB_WORKSPACE/build:/pf-hwprobe-build:ro"


def positive_controls_hwprobe(text):
    """The migration keeps the smoke gate: EXACTLY one BLOCKING and one ADVISORY
    check-control `docker run`. Each requirement is validated IN THE ARGV REGION
    WHERE ITS CONSUMER INTERPRETS IT: the five Docker-side requirements (--rm,
    --network none, --cap-add SYS_ADMIN, the exact -e APP_ARM64 projection, the
    exact read-only -v build bind) must be option/value pairs BEFORE the image
    operand — the same strings appended after the image are inert application
    arguments — and `--only-launcher qemu` must be a pair AFTER the check-control
    command in the container-argument region. Returns failure strings."""
    problems = []
    blocking_runs = 0
    advisory_runs = 0
    for _lineno, line in logical_lines(text):
        if not DOCKER_RUN.search(line):
            continue
        try:
            tokens = shlex.split(line, comments=False)
        except ValueError:
            continue  # scan_text already reported it as unparseable_command
        if "check-control" not in tokens:
            continue
        if BLOCKING_RE.search(line):
            blocking_runs += 1
        elif ADVISORY_RE.search(line):
            advisory_runs += 1
        else:
            problems.append("positive-control: a check-control run carries neither "
                            "steps.matrix.outputs.blocking nor .advisory (unclassified)")
        opts, image_idx, app = docker_regions(tokens)
        if image_idx is None:
            problems.append("positive-control: a check-control run has no image "
                            "operand; the Docker option region cannot be proven")
            continue
        if not pair_ok(opts, "--cap-add", "SYS_ADMIN"):
            problems.append("positive-control: a check-control run lost the paired "
                            "`--cap-add SYS_ADMIN` nesting cap as a Docker option "
                            "before the image")
        if not pair_ok(opts, "--network", "none"):
            problems.append("positive-control: a check-control run lost the paired "
                            "`--network none` isolation as a Docker option before "
                            "the image")
        if not pair_ok(opts, "-e", APP_ARM64_ENV):
            problems.append("positive-control: a check-control run lost the exact "
                            f"`-e {APP_ARM64_ENV}` projection as a Docker option "
                            "before the image")
        if not pair_ok(opts, "-v", BUILD_BIND):
            problems.append("positive-control: a check-control run lost the exact "
                            f"read-only bind `-v {BUILD_BIND}` as a Docker option "
                            "before the image")
        if "--rm" not in opts:
            problems.append("positive-control: a check-control run lost --rm from "
                            "the Docker option region")
        if "check-control" not in app:
            problems.append("positive-control: check-control is not in the "
                            "container-argument region (it must be the command "
                            "the image runs)")
            continue
        after_cc = app[app.index("check-control"):]
        if not pair_ok(after_cc, "--only-launcher", "qemu"):
            problems.append("positive-control: a check-control run lost the paired "
                            "`--only-launcher qemu` selection after check-control")
    if blocking_runs != 1:
        problems.append(f"positive-control: {blocking_runs} BLOCKING check-control run(s); "
                        "expected exactly 1")
    if advisory_runs != 1:
        problems.append(f"positive-control: {advisory_runs} ADVISORY check-control run(s); "
                        "expected exactly 1")
    return problems


# ---------------------------------------------------------------- self-test
# Scan fixtures: (name, text, expected_rules) — reds must produce EVERY named rule;
# greens (empty set) must produce no findings at all.
SELF_TEST_FIXTURES = [
    ("red_device_split_continuation",
     "run: |\n  docker run --rm --device \\\n    /dev/uinput \"$IMG\" check-control a133\n",
     {"host_uinput_device"}),
    ("red_device_equals_form",
     "run: docker run --rm --device=/dev/uinput img cmd\n",
     {"host_uinput_device"}),
    ("red_input_bind_v",
     "run: docker run --rm -v /dev/input:/dev/input img cmd\n",
     {"host_input_bind"}),
    ("red_input_mount_long",
     "run: docker run --rm --mount type=bind,src=/dev/input,dst=/dev/input img cmd\n",
     {"host_input_bind"}),
    ("red_cgroup_major13",
     'run: docker run --rm --device-cgroup-rule "c 13:* rmw" img cmd\n',
     {"input_major_wildcard"}),
    ("red_cgroup_major13_spacing",
     "run: docker run --rm --device-cgroup-rule 'c  13:*  rmw' img cmd\n",
     {"input_major_wildcard"}),
    ("red_cgroup_misc_uinput_10_223",
     'run: docker run --rm --device-cgroup-rule "c 10:223 rmw" img cmd\n',
     {"input_major_wildcard"}),
    ("red_cgroup_wildcard_major",
     'run: docker run --rm --device-cgroup-rule "c *:* rmw" img cmd\n',
     {"input_major_wildcard"}),
    ("red_cgroup_wildcard_all",
     'run: docker run --rm --device-cgroup-rule "a *:* rmw" img cmd\n',
     {"input_major_wildcard"}),
    ("red_privileged",
     "run: docker run --rm --privileged img cmd\n",
     {"host_device_wildcard"}),
    ("red_parent_bind_dev",
     "run: docker run --rm -v /dev:/dev img cmd\n",
     {"host_path_bind"}),
    ("red_parent_bind_tmp",
     "run: docker run --rm -v /tmp:/tmp img cmd\n",
     {"host_path_bind"}),
    ("red_parent_bind_run_user",
     "run: docker run --rm -v /run/user/1000:/run/user/1000 img cmd\n",
     {"host_path_bind"}),
    ("red_env_file",
     "run: docker run --rm --env-file /etc/host.env img cmd\n",
     {"host_env_file"}),
    ("red_display_env",
     'run: docker run --rm -e DISPLAY="$DISPLAY" img cmd\n',
     {"host_display_projection"}),
    ("red_wayland_env",
     "run: docker run --rm -e WAYLAND_DISPLAY=wayland-0 img cmd\n",
     {"host_display_projection"}),
    ("red_x11_socket_bind",
     "run: docker run --rm -v /tmp/.X11-unix:/tmp/.X11-unix img cmd\n",
     {"host_display_projection"}),
    ("red_xauthority_bind",
     "run: docker run --rm -v /home/mm/.Xauthority:/root/.Xauthority:ro img cmd\n",
     {"host_display_projection"}),
    ("red_wayland_socket_bind",
     "run: docker run --rm -v /run/user/1000/wayland-0:/tmp/wayland-0 img cmd\n",
     {"host_display_projection"}),
    ("red_modprobe",
     "run: sudo modprobe uinput || true\n",
     {"host_uinput_permission"}),
    ("red_modprobe_in_stale_comment",
     "# `sudo modprobe uinput` can never work — the module is guaranteed host-side\n",
     {"host_uinput_permission"}),
    ("red_sg_input",
     'run: sg input -c "./ci/check-control-hwprobe.py --device a133"\n',
     {"host_uinput_permission"}),
    ("red_chmod_node",
     "run: sudo chmod 0666 /dev/uinput\n",
     {"host_uinput_permission"}),
    ("red_modules_load_persistence",
     "run: echo uinput | sudo tee /etc/modules-load.d/uinput.conf\n",
     {"host_uinput_permission"}),
    ("red_xdotool",
     "run: xdotool key --clearmodifiers space\n",
     {"host_input_tool"}),
    ("red_unparseable_fail_closed",
     "run: docker run --rm --device 'unbalanced img cmd\n",
     {"unparseable_command"}),
    # ---- shell-expansion / indirection reds: every protected class via a variable
    ("red_dynamic_device_via_var",
     'run: |\n  node=/dev/uinput\n  docker run --rm --device "$node" "$IMG" cmd\n',
     {"host_uinput_device", "dynamic_route"}),
    ("red_dynamic_input_bind_via_var",
     'run: |\n  dst=/dev/input\n  docker run --rm -v "$dst:/x" "$IMG" cmd\n',
     {"host_input_bind", "dynamic_route"}),
    ("red_dynamic_cgroup_rule_via_var",
     'run: |\n  rule="c 13:* rmw"\n  docker run --rm --device-cgroup-rule "$rule" "$IMG" cmd\n',
     {"dynamic_route"}),
    ("red_dynamic_display_env_via_var",
     'run: |\n  d=DISPLAY\n  docker run --rm -e "$d=:0" "$IMG" cmd\n',
     {"dynamic_route"}),
    ("red_dynamic_x11_socket_via_var",
     'run: |\n  XSOCK=/tmp/.X11-unix\n  docker run --rm -v "$XSOCK:/tmp/.X11-unix" "$IMG" cmd\n',
     {"host_display_projection", "dynamic_route"}),
    ("red_dynamic_modprobe_via_var",
     'run: modprobe "$mod" || true\n',
     {"dynamic_route"}),
    ("red_dynamic_chmod_via_var",
     'run: |\n  n=/dev/uinput\n  chmod 0666 "$n"\n',
     {"host_uinput_device", "dynamic_route"}),
    ("red_dynamic_tool_via_assign",
     'run: |\n  t=xdotool\n  "$t" key space\n',
     {"host_input_tool"}),
    ("red_array_indirection",
     'run: |\n  CAPS=(--device /dev/uinput)\n  docker run --rm "${CAPS[@]}" "$IMG" cmd\n',
     {"dynamic_route"}),
    ("red_dynamic_bind_non_workspace",
     'run: |\n  SRC=/opt/pf\n  docker run --rm -v "$SRC/data:/data" "$IMG" cmd\n',
     {"dynamic_route"}),
    # ---- whole-argv expansion reds: an UNQUOTED expansion in the Docker option
    # region word-splits at runtime and can inject any option past the static scan.
    ("red_scalar_expansion_option_region",
     'run: |\n  FLAGS=\'--privileged --device /dev/uinput\'\n'
     '  docker run --rm $FLAGS "$IMG" cmd\n',
     {"dynamic_route"}),
    ("red_braced_scalar_expansion_option_region",
     'run: docker run --rm ${FLAGS} "$IMG" cmd\n',
     {"dynamic_route"}),
    ("red_cmdsub_option_region",
     'run: docker run --rm $(route_args) "$IMG" cmd\n',
     {"dynamic_route"}),
    ("red_backtick_option_region",
     'run: docker run --rm `route_args` "$IMG" cmd\n',
     {"dynamic_route"}),
    ("red_unquoted_dynamic_image_slot",
     "run: docker run --rm $IMG_VAR cmd\n",
     {"dynamic_route"}),
    ("red_unquoted_expansion_as_unknown_flag_value",
     'run: docker run --rm --security-opt $OPT "$IMG" cmd\n',
     {"dynamic_route"}),
    # ---- attached-value short-option reds: docker accepts `-vVALUE` / `-eVALUE`
    # as equivalent to the spaced spelling; they must face the same deny model.
    ("red_attached_short_bind_parent_dev",
     "run: docker run --rm -v/dev:/dev alpine true\n",
     {"host_path_bind"}),
    ("red_attached_short_bind_run_user",
     "run: docker run --rm -v/run/user/1000:/run/user/1000 alpine true\n",
     {"host_path_bind"}),
    ("red_attached_short_bind_combined_bools",
     "run: docker run -it -tv/tmp:/tmp alpine true\n",
     {"host_path_bind"}),
    ("red_attached_short_env_display_passthrough",
     "run: docker run --rm -eDISPLAY alpine true\n",
     {"host_display_projection"}),
    ("red_attached_short_env_display_assign",
     "run: docker run --rm -eWAYLAND_DISPLAY=wayland-0 alpine true\n",
     {"host_display_projection"}),
    # ---- quoted expansions in the option region: quoting prevents word
    # splitting but NOT the single word from evaluating to a complete option
    # (--privileged, --device=/dev/uinput) or an option-shaped image.
    ("red_quoted_scalar_option_region",
     'run: |\n  FLAGS=--privileged\n  docker run --rm "$FLAGS" pocketforge-sim:x check-control\n',
     {"dynamic_route"}),
    ("red_quoted_braced_option_region",
     'run: docker run --rm "${FLAGS}" pocketforge-sim:x cmd\n',
     {"dynamic_route"}),
    ("red_quoted_cmdsub_option_region",
     'run: docker run --rm "$(route_args)" pocketforge-sim:x cmd\n',
     {"dynamic_route"}),
    ("red_quoted_backtick_option_region",
     'run: docker run --rm "`route_args`" pocketforge-sim:x cmd\n',
     {"dynamic_route"}),
    ("red_quoted_image_slot_unproven_var",
     'run: docker run --rm --network none "$SOME_IMG" check-control a133\n',
     {"dynamic_route"}),
    ("red_quoted_image_var_unsafe_value",
     'run: |\n  SIM_IMG=--device=/dev/uinput\n  docker run --rm "$SIM_IMG" cmd\n',
     {"dynamic_route"}),
    ("red_quoted_flag_name_option_region",
     'run: docker run --rm "--$OPT" pocketforge-sim:x cmd\n',
     {"dynamic_route"}),
    ("red_image_var_unsafe_gh_context",
     'env:\n  SIM_IMG: pocketforge-${{ github.event.pull_request.title }}\n'
     'run: docker run --rm "$SIM_IMG" cmd\n',
     {"dynamic_route"}),
    # ---- mutation channels: a regex-visible SAFE assignment proves nothing about
    # the runtime value; channels outside the NAME=value grammar (printf -v, read,
    # eval/indirect, a sourced script, a $GITHUB_ENV write) mutate it invisibly.
    # A dynamic image operand is therefore only allowed right after `--`.
    ("red_image_var_printf_v_mutation",
     'run: |\n  SIM_IMG=pocketforge-sim:hwprobe-smoke-0\n'
     "  printf -v SIM_IMG %s '--privileged'\n"
     '  docker run --rm "$SIM_IMG" shell\n',
     {"dynamic_route"}),
    ("red_image_var_read_mutation",
     'run: |\n  SIM_IMG=pocketforge-sim:hwprobe-smoke-0\n'
     "  read -r SIM_IMG <<< '--device=/dev/uinput'\n"
     '  docker run --rm "$SIM_IMG" shell\n',
     {"dynamic_route"}),
    ("red_image_var_eval_mutation",
     'run: |\n  SIM_IMG=pocketforge-sim:hwprobe-smoke-0\n'
     '  name=SIM_IMG; eval "$name=--privileged"\n'
     '  docker run --rm "$SIM_IMG" shell\n',
     {"dynamic_route"}),
    ("red_image_var_sourced_mutation",
     'run: |\n  SIM_IMG=pocketforge-sim:hwprobe-smoke-0\n'
     '  . ./mutate-image-var.sh\n'
     '  docker run --rm "$SIM_IMG" shell\n',
     {"dynamic_route"}),
    ("red_image_var_github_env_write",
     'env:\n  SIM_IMG: pocketforge-sim:hwprobe-smoke-${{ github.run_id }}\n'
     'run: |\n  echo \'SIM_IMG=--privileged\' >> "$GITHUB_ENV"\n'
     '  docker run --rm "$SIM_IMG" shell\n',
     {"dynamic_route"}),
    # `--` present but an OPTION-SHAPED word follows it: misplaced delimiter or a
    # smuggled option in the image slot -> fail closed.
    ("red_option_after_delimiter",
     'run: docker run --rm -- --oops pocketforge-sim:x shell\n',
     {"dynamic_route"}),
    ("red_delimiter_without_image",
     'run: docker run --rm --\n',
     {"unparseable_command"}),
    # ---- greens
    ("green_quoted_image_after_delimiter",
     'env:\n  SIM_IMG: pocketforge-sim:hwprobe-smoke-${{ github.run_id }}\n'
     'run: docker run --rm --network none -- "$SIM_IMG" check-control --only-launcher qemu\n',
     set()),
    ("green_migrated_argv",
     "run: |\n"
     "  SIM_IMG=pocketforge-sim:hwprobe-smoke-0\n"
     "  docker run --rm --network none \\\n"
     "    --cap-add SYS_ADMIN --security-opt apparmor=unconfined --security-opt seccomp=unconfined \\\n"
     '    -v "$GITHUB_WORKSPACE/build:/pf-hwprobe-build:ro" \\\n'
     "    -e APP_ARM64=/pf-hwprobe-build/pf-hwprobe.arm64 \\\n"
     '    -- "$SIM_IMG" check-control --only-launcher qemu --label pf-hwprobe $BLOCKING\n',
     set()),
    ("green_doc_only_prose",
     "# input rides the sim's private per-run FIFOs; the host kernel node is never opened,\n"
     "# bound, or module-loaded, and no seat variable is projected (incident tsp-ihne1.56.5).\n",
     set()),
    ("green_caps_helper",
     'run: |\n  SIM_IMG=pocketforge-sim:hwprobe-smoke-0\n'
     '  caps() { docker run --rm -- "$SIM_IMG" shell -lc "python3 /opt/pf/platform/core/caps.py $*" 2>/dev/null; }\n',
     set()),
    ("green_workspace_chown_reclaim",
     'run: docker run --rm -v "$GITHUB_WORKSPACE:/w" debian:bookworm chown -R "$(id -u):$(id -g)" /w || true\n',
     set()),
    ("green_workspace_child_binds",
     'run: |\n  SIM_IMG=pocketforge-sim:hwprobe-smoke-0\n'
     '  docker run --rm --network none -v "$GITHUB_WORKSPACE/build:/app-build:ro" '
     '-v "$GITHUB_WORKSPACE/evidence:/evidence" -- "$SIM_IMG" shell -lc \'python3 /app-ci/run.py --platform "$PLATFORM"\'\n',
     set()),
    ("green_attached_workspace_bind",
     'run: |\n  SIM_IMG: pocketforge-sim:hwprobe-smoke-7\n'
     '  docker run --rm "-v$GITHUB_WORKSPACE/build:/app-build:ro" -- "$SIM_IMG" shell -lc "python3 /app-ci/run.py"\n',
     set()),
    ("green_static_benign_bind",
     "run: docker run --rm -v /opt/pf/platform:/platform:ro img cmd\n",
     set()),
    ("green_cgroup_benign_rule",
     'run: docker run --rm --device-cgroup-rule "c 1:3 rw" img cmd\n',
     set()),
    # $PWD/$HOME bind sources: under the amended canonical policy a dynamic bind
    # source must be anchored at a runner workspace root ($GITHUB_WORKSPACE,
    # ${GITHUB_WORKSPACE}, $RUNNER_TEMP, ${RUNNER_TEMP}, ${{ github.workspace }},
    # ${{ runner.temp }}); $PWD and $HOME stay red.
    ("red_pwd_bind_not_workspace_anchored",
     'run: docker run --rm -v "$PWD/build:/app-build:ro" -- "$SIM_IMG" shell\n',
     {"dynamic_route"}),
    ("red_home_bind_not_workspace_anchored",
     'run: docker run --rm -v "$HOME/cache:/cache" -- "$SIM_IMG" shell\n',
     {"dynamic_route"}),
]


def _run_block(devices_expr):
    return (
        "  docker run --rm --network none \\\n"
        "    --cap-add SYS_ADMIN --security-opt apparmor=unconfined --security-opt seccomp=unconfined \\\n"
        '    -v "$GITHUB_WORKSPACE/build:/pf-hwprobe-build:ro" \\\n'
        "    -e APP_ARM64=/pf-hwprobe-build/pf-hwprobe.arm64 \\\n"
        '    -- "$SIM_IMG" check-control --only-launcher qemu --label pf-hwprobe '
        + devices_expr + "\n"
    )


CONTROL_GREEN_TEXT = (
    "run: |\n"
    + _run_block("${{ steps.matrix.outputs.blocking }}")
    + "  if " + _run_block("${{ steps.matrix.outputs.advisory }}").lstrip()[:-1] + "; then\n"
    + "    echo advisory-ok\n  fi\n"
)

# Positive-control fixtures: (name, text, expect_problems). Each red degrades exactly
# one enumerated check while keeping every OTHER value pair intact, so a failing
# fixture proves that check (not a collateral one) rejects the degradation.
CONTROL_FIXTURES = [
    ("ctrl_green_blocking_and_advisory", CONTROL_GREEN_TEXT, False),
    # --cap-add present but NOT paired with SYS_ADMIN: a free `SYS_ADMIN` token rides
    # along in the argv (free token presence would accept this).
    ("ctrl_red_cap_unpaired",
     CONTROL_GREEN_TEXT.replace("--cap-add SYS_ADMIN", "--cap-add NET_ADMIN")
     .replace("--label pf-hwprobe", "--label SYS_ADMIN"),
     True),
    # --network present but not `none`; a free `none` token rides along.
    ("ctrl_red_network_unpaired",
     CONTROL_GREEN_TEXT.replace("--network none", "--network host")
     .replace("--label pf-hwprobe", "--label none"),
     True),
    # APP_ARM64 projection with the WRONG value (any-value acceptance was the defect).
    ("ctrl_red_env_wrong_value",
     CONTROL_GREEN_TEXT.replace(APP_ARM64_ENV, "APP_ARM64=/elsewhere/some-app.arm64"),
     True),
    # Build bind present but NOT read-only (the `:ro` suffix was the enumerated defect).
    ("ctrl_red_bind_not_readonly",
     CONTROL_GREEN_TEXT.replace(BUILD_BIND, "$GITHUB_WORKSPACE/build:/pf-hwprobe-build"),
     True),
    # --only-launcher NOT paired with qemu; a free `qemu` token rides along.
    ("ctrl_red_launcher_unpaired",
     CONTROL_GREEN_TEXT.replace("--only-launcher qemu", "--only-launcher native")
     .replace("--label pf-hwprobe", "--label qemu"),
     True),
    ("ctrl_red_rm_missing",
     CONTROL_GREEN_TEXT.replace("docker run --rm", "docker run"),
     True),
    # Both runs classified blocking ("two copies of one path still pass" was the defect).
    ("ctrl_red_advisory_missing",
     CONTROL_GREEN_TEXT.replace("steps.matrix.outputs.advisory",
                                "steps.matrix.outputs.blocking"),
     True),
    # The advisory invocation deleted outright.
    ("ctrl_red_single_run",
     "run: |\n" + _run_block("${{ steps.matrix.outputs.blocking }}"),
     True),
    # ---- boundary spoofs: each Docker-side requirement relocated AFTER the image
    # operand, where it is an inert application argument. Region-blind token
    # presence would accept every one of these.
    ("ctrl_red_cap_spoofed_after_image",
     CONTROL_GREEN_TEXT.replace("--cap-add SYS_ADMIN --security-opt", "--security-opt")
     .replace("check-control --only-launcher",
              "check-control --cap-add SYS_ADMIN --only-launcher"),
     True),
    ("ctrl_red_network_spoofed_after_image",
     CONTROL_GREEN_TEXT.replace("--rm --network none", "--rm")
     .replace("check-control --only-launcher",
              "check-control --network none --only-launcher"),
     True),
    ("ctrl_red_rm_spoofed_after_image",
     CONTROL_GREEN_TEXT.replace("docker run --rm", "docker run")
     .replace("check-control --only-launcher", "check-control --rm --only-launcher"),
     True),
    ("ctrl_red_env_spoofed_after_image",
     CONTROL_GREEN_TEXT.replace("    -e " + APP_ARM64_ENV + " \\\n", "")
     .replace("check-control --only-launcher",
              "check-control -e " + APP_ARM64_ENV + " --only-launcher"),
     True),
    ("ctrl_red_bind_spoofed_after_image",
     CONTROL_GREEN_TEXT.replace('    -v "' + BUILD_BIND + '" \\\n', "")
     .replace("check-control --only-launcher",
              'check-control -v "' + BUILD_BIND + '" --only-launcher'),
     True),
    # --only-launcher qemu relocated BEFORE the image: a Docker option region pair
    # is not the check-control application argument the gate needs.
    ("ctrl_red_launcher_spoofed_before_image",
     CONTROL_GREEN_TEXT.replace("--rm --network none", "--rm --network none --only-launcher qemu")
     .replace("check-control --only-launcher qemu --label", "check-control --label"),
     True),
]


def run_self_test():
    failures = []
    for name, text, expect in SELF_TEST_FIXTURES:
        found = scan_text(text)
        rules = {rule for rule, _ln, _d in found}
        if not expect:
            if found:
                failures.append(f"{name}: expected PASS, got findings {sorted(rules)}")
            else:
                print(f"self-test {name}: PASS (green control clean)")
        else:
            missing = expect - rules
            if not missing:
                print(f"self-test {name}: PASS (caught {sorted(expect)})")
            else:
                failures.append(f"{name}: expected rules {sorted(expect)}, "
                                f"missing {sorted(missing)}, got {sorted(rules) or 'no findings'}")
    for name, text, expect_problems in CONTROL_FIXTURES:
        problems = positive_controls_hwprobe(text)
        if expect_problems:
            if problems:
                print(f"self-test {name}: PASS (controls rejected: {problems[0][:70]}...)")
            else:
                failures.append(f"{name}: expected positive-control FAILURE, got none")
        else:
            if problems:
                failures.append(f"{name}: expected positive-control PASS, got {problems}")
            else:
                print(f"self-test {name}: PASS (positive controls clean)")
    if failures:
        for f in failures:
            print(f"host_route_guard=FAIL reason=self_test path=<fixture> line=0 rule=self_test detail={f}")
        print(f"SELF-TEST FAIL ({len(failures)} fixture(s) misbehaved)")
        return 1
    reds = sum(1 for _n, _t, e in SELF_TEST_FIXTURES if e)
    greens = sum(1 for _n, _t, e in SELF_TEST_FIXTURES if not e)
    print(f"SELF-TEST PASS ({len(SELF_TEST_FIXTURES) + len(CONTROL_FIXTURES)} fixtures: "
          f"{reds} scan reds caught, {greens} scan greens clean, "
          f"{len(CONTROL_FIXTURES)} positive-control fixtures)")
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
                      "(exactly one blocking + one advisory check-control run, each keeping "
                      "paired --cap-add SYS_ADMIN, --network none, the exact APP_ARM64 env, "
                      "the :ro build bind, --only-launcher qemu and --rm)")
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
