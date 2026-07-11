/*
 * widget_stick.c — kind=stick directional dot overlay (C4, tsp-fr2n.4).
 * See widget_stick.h for the contract.
 */
#include "widget_stick.h"

#include <math.h>

static double clamp_unit(double v) {
    if (v < -1.0) return -1.0;
    if (v >  1.0) return  1.0;
    return v;
}

void widget_stick_render_dot(SDL_Renderer *r,
                             int rect_x, int rect_y, int rect_w, int rect_h,
                             double nx, double ny) {
    if (rect_w <= 0 || rect_h <= 0) return;
    nx = clamp_unit(nx);
    ny = clamp_unit(ny);

    /* Dot radius: 1/6 of the smaller rect dim, clamped 4..12 px so it stays visible on
     * the 116x116 stick rects while never dominating a smaller widget. */
    int dim = rect_w < rect_h ? rect_w : rect_h;
    int radius = dim / 6;
    if (radius < 4)  radius = 4;
    if (radius > 12) radius = 12;

    /* Keep the dot fully inside the rect at extremes (1.0), inset by radius + 2 for a
     * one-pixel gap of the underlying wash at the boundary. */
    int inset = radius + 2;
    int half_x = rect_w / 2 - inset;
    int half_y = rect_h / 2 - inset;
    if (half_x < 0) half_x = 0;
    if (half_y < 0) half_y = 0;

    int cx = rect_x + rect_w / 2 + (int)floor(nx * half_x + 0.5);
    int cy = rect_y + rect_h / 2 + (int)floor(ny * half_y + 0.5);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    /* Filled disc via horizontal scanlines — SDL3's 2D renderer has no circle primitive,
     * and doing this in software here keeps the widget self-contained. */
    SDL_SetRenderDrawColor(r, 255, 220, 40, 255);
    for (int dy = -radius; dy <= radius; dy++) {
        int xspan = (int)floor(sqrt((double)(radius * radius - dy * dy)) + 0.5);
        SDL_FRect line = { (float)(cx - xspan), (float)(cy + dy),
                            (float)(2 * xspan + 1), 1.0f };
        SDL_RenderFillRect(r, &line);
    }
    /* Dark yellow ring for edge contrast against the red wash. */
    SDL_SetRenderDrawColor(r, 128, 100, 20, 255);
    SDL_FRect ring = { (float)(cx - radius - 1), (float)(cy - radius - 1),
                        (float)(2 * radius + 2), (float)(2 * radius + 2) };
    SDL_RenderRect(r, &ring);
}
