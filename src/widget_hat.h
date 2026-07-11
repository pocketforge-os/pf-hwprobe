/*
 * widget_hat.h — kind=hat 8-way segment highlight (C4, tsp-fr2n.4).
 *
 * On TOP of C2's uniform active -> lit_overlay + red-wash rect render for kind=hat,
 * C4 adds a directional segment: the dpad rect is subdivided into a 3x3 grid and the
 * cell at (hx+1, hy+1) is highlighted bright yellow, where (hx, hy) each in {-1, 0, 1}
 * come from the hat's normalized axis values. The centre cell (0, 0) never lights —
 * the rest state carries no overlay. The 8 non-centre cells span the 8 dpad directions.
 *
 * E7 preservation: the highlighted cell for any of the 8 directions never overlaps
 * the rect's geometric centre (the sample point framebuffer_region().is_red() uses
 * — a 3x3 average at (x + w/2, y + h/2), which sits in the centre column AND centre
 * row of the 3x3 grid). The wash under paints the centre red on any deflection, and
 * this overlay only paints the direction cell — no confounding.
 */
#pragma once

#include <SDL3/SDL.h>

void widget_hat_render_segment(SDL_Renderer *r,
                               int rect_x, int rect_y, int rect_w, int rect_h,
                               int hx, int hy);
