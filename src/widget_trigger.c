/*
 * widget_trigger.c — implementation of the C3 slider-with-marker widget.
 * See widget_trigger.h for the shape + why.
 */
#include "widget_trigger.h"

/* Small helpers — kept local so this TU compiles without leaning on main.c. */
static void wt_fill(SDL_Renderer *r, int x, int y, int w, int h,
                    int cr, int cg, int cb) {
    SDL_SetRenderDrawColor(r, (Uint8)cr, (Uint8)cg, (Uint8)cb, 255);
    SDL_FRect fr = { (float)x, (float)y, (float)w, (float)h };
    SDL_RenderFillRect(r, &fr);
}

/* The "hot" red used to light active controls (matches main.c ACTIVE_TINT / lit
 * artwork palette — 214,64,64 → passes the sim's Region.is_red() (r>=150) AND
 * Slider.fraction() (r>=150 && g<=90 && b<=90) predicates). */
enum { WT_RED_R = 214, WT_RED_G = 64, WT_RED_B = 64 };

/* Ticks + marker are LIGHT GREY (never red) so they cannot mistakenly bump the
 * Slider.fraction() red-count when drawn above/around the rect. */
enum { WT_MARK_R = 220, WT_MARK_G = 220, WT_MARK_B = 220 };

void wt_trigger_render(SDL_Renderer *r,
                       SDL_Texture  *tex_lit,
                       int x, int y, int w, int h,
                       int sx, int sy, int sw, int sh,
                       double f,
                       int sem_binary) {
    if (!r || w <= 0 || h <= 0) return;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;

    /* ---- (1) PROPORTIONAL FILL — the C7 slider-fraction contract ------------
     *
     * Fill fraction f of the rect's width with the "hot red" the sim's
     * Slider.fraction() counts. Prefer the descriptor's lit_body sprite when
     * one is loaded (keeps the widget visually consistent with C2's button-lit
     * on skin runs); fall back to a solid red fill for stub / no-skin runs. */
    if (f > 0.001) {
        int fw = (int)((double)w * f + 0.5);
        if (fw >= 1) {
            if (tex_lit && sw > 0 && sh > 0) {
                float fw_src = (float)((double)sw * f);
                SDL_FRect src = { (float)sx, (float)sy, fw_src, (float)sh };
                SDL_FRect dst = { (float)x,  (float)y,  (float)fw, (float)h  };
                SDL_RenderTexture(r, tex_lit, &src, &dst);
            } else {
                wt_fill(r, x, y, fw, h, WT_RED_R, WT_RED_G, WT_RED_B);
            }
        }
    }

    /* ---- (2) SLIDER MARKER above the rect — ui="slider_above" ---------------
     *
     * A small marker at (x + f*w, y - offset) points down at the current
     * fractional position, drawn OUTSIDE the trig rect (smaller y) so it never
     * bleeds into the Slider.fraction() middle-row sample. GREY, not red — the
     * red-count predicate cannot see it either. Approximated as a stack of
     * decreasing-width fills (SDL_Render is fill-only in the sim toolchain). */
    const int marker_h    = 5;                              /* triangle height */
    const int marker_gap  = 1;                              /* px above the rect */
    const int marker_hw   = 3;                              /* half-width at base */
    int mx = x + (int)((double)w * f + 0.5);
    if (mx < x)         mx = x;                             /* clamp so it stays over the rect */
    if (mx > x + w - 1) mx = x + w - 1;
    int base_y = y - marker_gap - 1;                        /* base row, just above the rect */
    for (int i = 0; i < marker_h; i++) {
        int py = base_y - i;
        if (py < 0) break;
        int hw = marker_hw - (i * marker_hw + (marker_h / 2)) / marker_h;
        if (hw < 0) hw = 0;
        int px = mx - hw;
        int pw = 2 * hw + 1;
        if (px < 0) { pw += px; px = 0; }                   /* keep on-canvas */
        if (pw > 0) wt_fill(r, px, py, pw, 1, WT_MARK_R, WT_MARK_G, WT_MARK_B);
    }

    /* ---- (3) BINARY-SEMANTICS HONESTY TICKS ---------------------------------
     *
     * When the actuator is binary-on-analog (tsp-v19s: the trigger's underlying
     * switch reports endpoint-only 0<->255 on the analog wire), draw two small
     * ticks flanking the ends of the rect, above the trig band — a subtle
     * visual "endpoints only" hint that a reviewer of the snapshot PNG can read
     * without cross-referencing the descriptor. The widget still tracks the
     * injected fraction faithfully (the sim sweep proof); on real silicon the
     * raw axis values are already 0 or full-range so the marker naturally
     * snaps 0<->1. */
    if (sem_binary) {
        int tick_y = y - marker_gap - marker_h - 2;
        if (tick_y < 0) tick_y = 0;
        wt_fill(r, x,           tick_y, 2, 2, WT_MARK_R, WT_MARK_G, WT_MARK_B);
        wt_fill(r, x + w - 2,   tick_y, 2, 2, WT_MARK_R, WT_MARK_G, WT_MARK_B);
    }
}
