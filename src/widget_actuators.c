/*
 * widget_actuators.c — see widget_actuators.h.
 *
 * The descriptor read is a targeted single-pass state machine over the SAME
 * `capabilities.toml` main.c's parse_skin() already reads — not a general TOML
 * parser. It only looks at `[[actuators]]` blocks and extracts `kind = "..."`
 * plus `count = N`. Every other section is ignored. This mirrors the E1 rule
 * that missing hardware is ROW OMISSION, so the a133 (no rumble row) simply
 * leaves has_rumble=0 with zero code branching.
 */
#include "widget_actuators.h"

#include <stdio.h>
#include <string.h>

/* Grid geometry — kept in the .c so the verify script only depends on the
 * ONE cell-detection invariant (see the header). At the a133's count=23 the
 * grid fits in one row of a 1280-wide canvas; wrapping is defensive against a
 * future descriptor with a much larger count. */
#define LED_CELL_W             18
#define LED_CELL_H             12
#define LED_CELL_GAP            4
#define LED_MARGIN_X           16
#define LED_MARGIN_Y           16
#define LED_OFF_R              96
#define LED_OFF_G              96
#define LED_OFF_B              96
#define LED_ON_R              200
#define LED_ON_G              200
#define LED_ON_B              200

void actuators_parse_descriptor(struct actuator_state *st, const char *io_dir) {
    st->has_rumble = 0;
    st->led_count  = 0;
    if (!io_dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/capabilities.toml", io_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;

    int  in_act    = 0;
    char cur_kind[32] = {0};
    int  cur_count    = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        if (*p == '[') {
            /* Section boundary: flush any pending [[actuators]] before advancing. */
            if (in_act) {
                if (!strcmp(cur_kind, "rumble"))    st->has_rumble = 1;
                if (!strcmp(cur_kind, "led_array")) st->led_count  = cur_count;
            }
            in_act = !strncmp(p, "[[actuators]]", 13);
            cur_kind[0] = '\0';
            cur_count   = 0;
            continue;
        }
        if (!in_act) continue;

        int ival;
        if (sscanf(p, "kind = \"%31[^\"]\"", cur_kind) == 1) {
            /* stored directly into cur_kind (bounded); no scratch buffer needed. */
        } else if (sscanf(p, "count = %d", &ival) == 1) {
            cur_count = ival;
        }
    }
    /* Flush the trailing block (EOF terminates the last [[actuators]]). */
    if (in_act) {
        if (!strcmp(cur_kind, "rumble"))    st->has_rumble = 1;
        if (!strcmp(cur_kind, "led_array")) st->led_count  = cur_count;
    }
    fclose(f);
}

const char *actuators_rumble_status_name(int status) {
    switch (status) {
        case PF_RUMBLE_FIRED:           return "fired";
        case PF_RUMBLE_NOOP_ABSENT:     return "noop-absent";
        case PF_RUMBLE_NOOP_SUPPRESSED: return "noop-suppressed";
        default:                        return "err";
    }
}

void widget_led_grid_render(SDL_Renderer *r, const struct actuator_state *st,
                            int canvas_w, int canvas_h) {
    if (!st || st->led_count <= 0) return;
    int stride = LED_CELL_W + LED_CELL_GAP;
    int cells_per_row = (canvas_w - 2 * LED_MARGIN_X + LED_CELL_GAP) / stride;
    if (cells_per_row < 1) cells_per_row = 1;
    for (int i = 0; i < st->led_count; i++) {
        int col = i % cells_per_row;
        int row = i / cells_per_row;
        int x = LED_MARGIN_X + col * stride;
        int y = canvas_h - LED_MARGIN_Y - (row + 1) * (LED_CELL_H + LED_CELL_GAP);
        SDL_SetRenderDrawColor(r, LED_OFF_R, LED_OFF_G, LED_OFF_B, 255);
        SDL_FRect fr = { (float)x, (float)y, (float)LED_CELL_W, (float)LED_CELL_H };
        SDL_RenderFillRect(r, &fr);
        /* Bright inner rectangle so a naive PPM scanline detects one bright run
         * per cell; the verify script counts those runs against descriptor count. */
        SDL_SetRenderDrawColor(r, LED_ON_R, LED_ON_G, LED_ON_B, 255);
        SDL_FRect inner = { (float)(x + 4), (float)(y + 3),
                            (float)(LED_CELL_W - 8), (float)(LED_CELL_H - 6) };
        SDL_RenderFillRect(r, &inner);
    }
}
