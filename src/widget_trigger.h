/*
 * widget_trigger.h — the C3 slider-with-marker widget for kind=trigger inputs.
 *
 * Owned by tsp-fr2n.3 (E6 / pf-hwprobe). Extracted out of src/main.c so main.c's
 * dispatch stays tiny and sibling widgets (button — inline in main.c today;
 * widget_stick, widget_hat — C4; widget_sensor_imu — C5; widget_actuators — C6)
 * can land in parallel with trivial rebase.
 *
 * The widget renders TWO things per trigger:
 *
 *   1. PROPORTIONAL FILL inside the trigger's [skin.parts].trig_l/trig_r canvas
 *      rect (fraction f of the rect's width goes red). This is the C7 gate
 *      contract: the sim's ``Slider.fraction()`` samples the middle row of that
 *      rect and asserts ``fraction ≈ f`` (tol 0.06) for f ∈ {0, 0.25, 0.5, 0.75,
 *      1.0} — the "sweep min->max tracks monotonically" acceptance of infra-105
 *      §THE TASK item 3.
 *
 *   2. A SLIDER MARKER drawn ABOVE the rect (ui="slider_above") at horizontal
 *      position x + f*w — the "slider MARKER" refinement C3 layers on top of C2's
 *      body render. This is the visible tell that the widget has been informed
 *      by the descriptor's normalized axis value.
 *
 * `sem_binary` is a hint carried from the descriptor's actuator-semantics field
 * (schema `semantics` — tsp-v19s, merged 2026-07-11). When 1, the underlying
 * actuator is BINARY (endpoint-only 0<->255 on real silicon; the sim tracks the
 * injected fraction). Rendering stays IDENTICAL either way — data-bound to `f`
 * — but two small endpoint ticks are drawn above the rect as an HONEST
 * presentation hint (a reviewer of the PNG can see this is a binary-on-analog
 * actuator without reading the descriptor). This satisfies the DoR "present them
 * HONESTLY" requirement without special-casing any device.
 *
 * The widget is deliberately grep-clean: no per-device evdev symbol and no
 * per-device literal — every value enters through the caller's rect + fraction
 * args (which came from layout.txt, itself derived from the descriptor). See
 * README.md for the grep-test invariant.
 */
#ifndef PF_HWPROBE_WIDGET_TRIGGER_H
#define PF_HWPROBE_WIDGET_TRIGGER_H

#include <SDL3/SDL.h>

/* Render one trigger widget.
 *
 *   r         : SDL renderer (software-backed under the sim).
 *   tex_lit   : the descriptor's lit_body sprite sheet; NULL for stub runs.
 *   x,y,w,h   : the trigger's CANVAS rect (from layout.txt, mirrors [skin.parts]).
 *   sx,sy,sw,sh: the trigger's SKIN-SPACE rect (from [skin.parts]); sw==0 => no
 *               source rect available, fall back to a flat red fill.
 *   f         : 0.0..1.0 axis fraction (normalized by the caller via the
 *               descriptor's range min/max — clamped again defensively here).
 *   sem_binary: 0 for analog semantics; 1 for binary-on-analog semantics.
 */
void wt_trigger_render(SDL_Renderer *r,
                       SDL_Texture  *tex_lit,
                       int x, int y, int w, int h,
                       int sx, int sy, int sw, int sh,
                       double f,
                       int sem_binary);

#endif /* PF_HWPROBE_WIDGET_TRIGGER_H */
