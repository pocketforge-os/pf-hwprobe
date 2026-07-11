/*
 * widget_actuators.h — C6 (tsp-fr2n.6): first OUTPUT capability + first honored
 * preference for pf-hwprobe. Split from main.c so the actuator surface stays a
 * small, disjoint footprint alongside the C3/C4 input widgets.
 *
 * Adds two data-driven things, both descriptor-only:
 *
 *   (1) RUMBLE, via the E2 facade — `pf_rumble_pulse(session, ms)`. On a523 (the
 *       descriptor with the [[actuators]] kind="rumble" row) it FIRES; on a133
 *       (no rumble row) it returns NOOP_ABSENT; with the `hapticsEnabled`
 *       preference off it returns NOOP_SUPPRESSED. The app makes ONE
 *       unconditional call — the primitive maps descriptor + preference to the
 *       three-way status. No app-side conditional on `hapticsEnabled`.
 *
 *   (2) LEDs, as a descriptor-count widget — reads the `[[actuators]]`
 *       kind="led_array" row's `count` field (a133 = 23, a523 = 17; the per-
 *       device LED-controller IC is descriptor data the app never names) and
 *       renders that many cells along the canvas's bottom edge. The a133/a523
 *       delta is a DATA change: same C, different descriptor row.
 *
 * SIM CONTRACT (matches sim/control/broker_stub.py RUMBLE_* strings):
 *   `actuators_rumble_status_name(pf_rumble_pulse(...))` -> "fired" /
 *   "noop-absent" / "noop-suppressed" / "err". Same strings the sim's Python
 *   `dev.acquire_rumble().pulse(...)` returns, so the C6 verify script asserts
 *   ONE taxonomy across both routes.
 */
#ifndef PF_HWPROBE_WIDGET_ACTUATORS_H
#define PF_HWPROBE_WIDGET_ACTUATORS_H

#include <SDL3/SDL.h>
#include <pocketforge.h>

/* Parsed actuator descriptor state. Populated by actuators_parse_descriptor;
 * missing rows / missing descriptor leave fields zeroed (graceful omission). */
struct actuator_state {
    int has_rumble;   /* 1 iff a [[actuators]] block with kind="rumble" exists */
    int led_count;    /* count from [[actuators]] kind="led_array" (0 iff absent) */
};

/* Parse `<io_dir>/capabilities.toml` for [[actuators]] rows into `st`. */
void actuators_parse_descriptor(struct actuator_state *st, const char *io_dir);

/* PF_RUMBLE_* -> stable status name; unknown values -> "err". */
const char *actuators_rumble_status_name(int status);

/* Draw `st->led_count` LED cells along the canvas bottom edge (row-major from
 * bottom-left, wrapping if needed). No-op iff count==0. Colors and geometry
 * are self-contained so the verify script can count cells purely from the PPM
 * — see LED_ROW_Y_FROM_BOTTOM / LED_CELL_W / LED_CELL_GAP in the .c. */
void widget_led_grid_render(SDL_Renderer *r, const struct actuator_state *st,
                            int canvas_w, int canvas_h);

#endif /* PF_HWPROBE_WIDGET_ACTUATORS_H */
