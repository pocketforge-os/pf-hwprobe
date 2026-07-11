/*
 * widget_hat.c — kind=hat 8-way segment highlight (C4, tsp-fr2n.4).
 * See widget_hat.h for the contract.
 */
#include "widget_hat.h"

void widget_hat_render_segment(SDL_Renderer *r,
                               int rect_x, int rect_y, int rect_w, int rect_h,
                               int hx, int hy) {
    if (rect_w <= 0 || rect_h <= 0) return;
    /* Rest: no overlay. Together with the caller's C2 wash-only-when-active rule this
     * keeps the rest state visually inert (dpad rect the body PNG's neutral background). */
    if (hx == 0 && hy == 0) return;
    if (hx < -1) hx = -1;
    if (hx >  1) hx =  1;
    if (hy < -1) hy = -1;
    if (hy >  1) hy =  1;

    int cw = rect_w / 3;
    int ch = rect_h / 3;
    if (cw <= 0 || ch <= 0) return;

    int cell_x = rect_x + (hx + 1) * cw;
    int cell_y = rect_y + (hy + 1) * ch;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 255, 220, 40, 255);
    SDL_FRect box = { (float)cell_x, (float)cell_y, (float)cw, (float)ch };
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 128, 100, 20, 255);
    SDL_RenderRect(r, &box);
}
