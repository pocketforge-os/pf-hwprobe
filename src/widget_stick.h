/*
 * widget_stick.h — kind=stick directional dot overlay (C4, tsp-fr2n.4).
 *
 * On TOP of C2's uniform active -> lit_overlay + red-wash rect render for kind=stick,
 * C4 adds a directional dot: a bright yellow filled circle at the normalized (nx, ny)
 * offset within the stick's canvas rect. It is drawn ONLY when the stick is active
 * (past deadzone) so the E7 rest assertion (centre NOT is_red) is preserved on the
 * dpad+stick regions, AND the E7 stick-deflect assertion (centre is_red) is preserved
 * because the wash paints the centre red before the dot lands at the offset EDGE of
 * the rect — the 3x3 centre sample never lands in the dot. See src/main.c
 * render_frame() for the wash + this call ordering.
 *
 * Non-red on purpose: the dot colour (255,220,40) fails the is_red predicate
 * (g <= 90 required), so even in the impossible corner where a future test moved a
 * dot over the centre sample the isolation would degrade to "not red" rather than
 * silently spoofing red on a rest-centred stick.
 */
#pragma once

#include <SDL3/SDL.h>

void widget_stick_render_dot(SDL_Renderer *r,
                             int rect_x, int rect_y, int rect_w, int rect_h,
                             double nx, double ny);
