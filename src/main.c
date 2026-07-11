/*
 * pf-hwprobe — the PocketForge hardware hello-world diagnostic.
 *
 * The first first-party non-Steam-Link PocketForge app: a full-screen SDL3 diagnostic
 * whose main loop is PURELY data-bound to a device capability descriptor. For each
 * `[[inputs]]` row it renders one control on a fixed 1280x720 canvas and lights it on
 * press. The a133 (base Pro) and a523 (Pro S) differ only by DATA — same binary, same
 * source, different descriptor.
 *
 * C1 SCOPE (tsp-fr2n.1, merged): the skeleton — descriptor wiring + main loop + stub-
 * rect widgets + the sim's FIFO handshake.
 *
 * C2 SCOPE (this bead, tsp-fr2n.2): the REAL body render + the button binding for BOTH
 * devices, plus PINNING the tsp-osr renderer recipe. On top of the C1 skeleton this
 * adds:
 *   * SKIN RENDER — draws `skin.body` (the device's traced body PNG) into the landscape
 *     canvas and, on press, composites the `skin.lit_body` overlay sprite for that
 *     control's `[skin.parts].<id>` rect (the control goes RED on press). This is the
 *     AVD clickable-skin model, applied UNIFORMLY from `control_is_active()` — so
 *     buttons, the a523-only Home key, and the a523-only L3/R3 stick-clicks all light
 *     from descriptor rows with ZERO per-device code (C3/C4 layer the richer trigger/
 *     stick/hat widgets on top of this foundation).
 *   * INPUT VIA THE FACADE — input is acquired THROUGH the E2 capability facade
 *     (`pf_acquire("input")`), never by scanning /dev with ambient authority. See the
 *     "input capability acquisition seam" block below for the authorization gate, the
 *     frozen-v1 C-ABI fd gap this uncovered, and the swappable read-source seam.
 *   * tsp-osr RECIPE — see `pin_tsp_osr_recipe()`.
 *
 * LINKS (per the epic decisions):
 *   * SDL3 via `SDL3_DYNAMIC_API` — built against SDL3's public headers, static-linked
 *     with a stock SDL3 for the sim proof; the DYNAPI jump table lets
 *     `SDL_DYNAMIC_API=/…/libSDL3-pocketforge.so.0` swap in the on-device sunxifb SDL3
 *     unchanged (the Steam-Link pattern). NOT LD_PRELOAD.
 *   * `libpocketforge` (E2 C ABI, frozen v1) via `pf_connect_descriptor()` — proves the
 *     app talks to the PocketForge runtime facade, not raw /dev/ nodes.
 *
 * SIM CONTRACT (E5 / matches `sim/control/hwprobe-lite.c`):
 *   Usage:     pf-hwprobe <io-dir>
 *   Reads:     <io-dir>/layout.txt        canvas + evdev nodes + control rects (canvas space)
 *              <io-dir>/capabilities.toml  descriptor (E2 seam + [skin]/[skin.parts])
 *              <io-dir>/<skin.body>        the body PNG (staged by ci/run-under-sim.py)
 *   FIFOs:     <io-dir>/req  (host -> app), <io-dir>/resp (app -> host)
 *   Protocol:  emit "ready" on start; on "snap <ppm-path>" drain events + render + write
 *              PPM + reply "ok" (or "err"); on "quit" reply "bye" and exit; else "err".
 *
 * DEVICE-FREE + GREP-CLEAN: this source names ZERO per-device evdev symbol (no
 * face-button / home-key / trigger-axis / IMU / rumble / LED-controller strings) and
 * ZERO per-device literal — every widget, code, rect, and skin part enters through
 * layout.txt + the descriptor's [skin] tables. The only kernel-ABI constants baked here
 * are the three generic evdev event-type numbers (SYN / KEY / ABS). See README.md for
 * the enforcing grep test. (The PNG decoder is vendored under third_party/, outside the
 * grep scope, behind the img_decode seam.)
 */

#include <SDL3/SDL.h>
#include <pocketforge.h>

#include "img_decode.h"
#include "widget_actuators.h"     /* C6 (tsp-fr2n.6): rumble handle + LED-grid widget */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

/* Generic evdev event-type numbers (kernel ABI, device-independent). */
enum { EV_TYPE_SYN = 0, EV_TYPE_KEY = 1, EV_TYPE_ABS = 3 };

/* 64-bit kernel input_event, packed <l l H H i> = 24 bytes. Header-free so this TU
 * never drags in <linux/input-event-codes.h> and its per-device symbols. */
struct evdev_event {
    long           sec;
    long           usec;
    unsigned short type;
    unsigned short code;
    int            value;
};

/* Widget kinds emitted by layout.py from the descriptor's `input.kind`. Names in
 * layout.txt: "button" | "trigger" | "hat" | "stick". */
enum widget_kind { WK_BUTTON = 0, WK_TRIGGER, WK_HAT, WK_STICK };

/* A single (ev_type, code) binding + its live value. */
struct binding {
    int   type;
    int   code;
    int   vmin;
    int   vmax;
    int   value;
    char  role;   /* 'k' digital, 't' trigger axis, 'x'/'y' stick/hat axis */
};

/* Fixed upper bounds — every real descriptor stays comfortably below these. */
#define MAX_BINDINGS  4
#define MAX_CONTROLS  40
#define MAX_NODES     4
#define MAX_NAME      40
#define PATH_BUF      512

struct control {
    char             name[MAX_NAME];   /* == the skin_part key from layout.txt */
    enum widget_kind kind;
    int              x, y, w, h;       /* canvas-space rect (layout.txt; the test samples this) */
    int              sx, sy, sw, sh;   /* skin-image-space rect ([skin.parts]; 0 => none) */
    int              n_bindings;
    struct binding   bindings[MAX_BINDINGS];
};

/* A named rect from the descriptor's [skin.parts] table (skin-image pixel space). */
struct skin_part {
    char name[MAX_NAME];
    int  x, y, w, h;
};

struct app {
    int             canvas_w, canvas_h;
    int             n_controls;
    struct control  controls[MAX_CONTROLS];

    /* input: node PATHS come from layout.txt (platform-provided); fds are opened only
     * AFTER pf_acquire("input") authorizes — see the acquisition seam below. */
    int             n_nodes;
    char            node_path[MAX_NODES][PATH_BUF];
    int             n_fds;
    int             node_fd[MAX_NODES];
    int             input_authorized;

    /* skin (the AVD clickable-skin body render) */
    int             n_skin_parts;
    struct skin_part skin_parts[MAX_CONTROLS];
    char            skin_body[PATH_BUF];
    char            skin_lit[PATH_BUF];
    SDL_Texture    *tex_body;
    SDL_Texture    *tex_lit;
    int             body_w, body_h;
    double          fit_s, fit_ox, fit_oy;   /* skin-space -> canvas-space transform */
    int             have_skin;               /* both textures + fit ready */

    PfSession      *session;                 /* E2 handle; may be NULL if no descriptor */

    /* Actuator descriptor state (C6 / tsp-fr2n.6): the a133/a523 delta for the LED
     * count + rumble presence enters here as PURE DATA — no per-device code. */
    struct actuator_state actuators;
};

/* ------------------------------------------------------------------ evidence log */

static void log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ layout.txt parse
 *
 * Line grammar (whitespace-separated), one per line:
 *   canvas <W> <H> <rotation>
 *   node   <path>
 *   ctl    <name> <kind> <x> <y> <w> <h> <n_bindings> [<evtype> <code> <min> <max> <role>]*
 *
 * kind   ∈ {button,trigger,hat,stick}
 * evtype ∈ {1 (EV_KEY), 3 (EV_ABS)}
 * role   ∈ {k,t,x,y}
 */

static char *next_tok(char **cursor) {
    return strtok_r(NULL, " \t\n", cursor);
}

static int parse_widget_kind(const char *tok) {
    if (!tok) return WK_BUTTON;
    if (!strcmp(tok, "trigger")) return WK_TRIGGER;
    if (!strcmp(tok, "hat"))     return WK_HAT;
    if (!strcmp(tok, "stick"))   return WK_STICK;
    return WK_BUTTON;
}

static int parse_layout(struct app *app, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        log_line("pf-hwprobe: layout open %s failed: %s", path, strerror(errno));
        return -1;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *save = NULL;
        char *head = strtok_r(line, " \t\n", &save);
        if (!head || head[0] == '#') continue;

        if (!strcmp(head, "canvas")) {
            char *w = next_tok(&save), *h = next_tok(&save);
            if (w) app->canvas_w = atoi(w);
            if (h) app->canvas_h = atoi(h);
            /* rotation token intentionally ignored: at C1/C2 we render in canvas space. */
        } else if (!strcmp(head, "node")) {
            char *p = next_tok(&save);
            if (!p || app->n_nodes >= MAX_NODES) continue;
            /* record the PATH only — the fd is opened later, AFTER the facade
             * authorizes input (see acquire_input); the app never scans /dev. */
            snprintf(app->node_path[app->n_nodes++], PATH_BUF, "%s", p);
        } else if (!strcmp(head, "ctl")) {
            if (app->n_controls >= MAX_CONTROLS) continue;
            struct control *c = &app->controls[app->n_controls];
            memset(c, 0, sizeof *c);
            char *name = next_tok(&save);
            char *kind = next_tok(&save);
            char *x = next_tok(&save), *y = next_tok(&save);
            char *w = next_tok(&save), *h = next_tok(&save);
            char *nc_s = next_tok(&save);
            if (!name || !x || !y || !w || !h || !nc_s) continue;
            snprintf(c->name, sizeof c->name, "%s", name);
            c->kind = parse_widget_kind(kind);
            c->x = atoi(x); c->y = atoi(y); c->w = atoi(w); c->h = atoi(h);
            int nc = atoi(nc_s);
            if (nc > MAX_BINDINGS) nc = MAX_BINDINGS;
            for (int i = 0; i < nc; i++) {
                struct binding *b = &c->bindings[i];
                char *t = next_tok(&save);
                char *cd = next_tok(&save);
                char *lo = next_tok(&save);
                char *hi = next_tok(&save);
                char *ro = next_tok(&save);
                if (!t || !cd || !lo || !hi) break;
                b->type  = atoi(t);
                b->code  = atoi(cd);
                b->vmin  = atoi(lo);
                b->vmax  = atoi(hi);
                b->role  = ro ? ro[0] : '?';
                /* Rest state: triggers at min; stick/hat axes centred; digital at 0. */
                b->value = (b->type == EV_TYPE_ABS && c->kind != WK_TRIGGER)
                              ? (b->vmin + b->vmax) / 2
                          : (b->type == EV_TYPE_ABS) ? b->vmin
                                                     : 0;
                c->n_bindings++;
            }
            app->n_controls++;
        }
    }
    fclose(f);
    log_line("pf-hwprobe: layout parsed: canvas %dx%d, %d controls, %d nodes",
             app->canvas_w, app->canvas_h, app->n_controls, app->n_nodes);
    return 0;
}

/* ------------------------------------------------------------------ [skin] parse
 *
 * A minimal, targeted reader for the descriptor's clickable-skin tables — NOT a
 * general TOML parser. It extracts exactly:
 *   [skin]        body = "…"   lit_body = "…"
 *   [skin.parts]  <name> = { x = N, y = N, w = N, h = N }
 * The app renders the body PNG + the per-part lit overlay from these; parsing the
 * SAME descriptor the sim + test consume keeps "one descriptor, three consumers"
 * honest and works on-device too (no sim-only side channel). Skin part NAMES
 * (btn_south, dpad, …) are generic — not the per-device evdev symbols the grep test
 * forbids.
 */
static void strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int parse_skin(struct app *app, const char *io_dir) {
    char path[PATH_BUF];
    snprintf(path, sizeof path, "%s/capabilities.toml", io_dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        log_line("pf-hwprobe: skin parse: no descriptor at %s (%s)", path, strerror(errno));
        return -1;
    }
    enum { SEC_OTHER, SEC_SKIN, SEC_PARTS } sec = SEC_OTHER;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        /* left-trim */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        if (*p == '[') {
            if (!strncmp(p, "[skin.parts]", 12))      sec = SEC_PARTS;
            else if (!strncmp(p, "[skin]", 6))         sec = SEC_SKIN;
            else                                        sec = SEC_OTHER;
            continue;
        }
        if (sec == SEC_SKIN) {
            char key[MAX_NAME], val[PATH_BUF];
            if (sscanf(p, "%39s = \"%511[^\"]\"", key, val) == 2) {
                if (!strcmp(key, "body"))          snprintf(app->skin_body, PATH_BUF, "%s", val);
                else if (!strcmp(key, "lit_body")) snprintf(app->skin_lit,  PATH_BUF, "%s", val);
            }
        } else if (sec == SEC_PARTS) {
            char name[MAX_NAME];
            char *eq = strchr(p, '=');
            char *brace = strchr(p, '{');
            if (!eq || !brace || app->n_skin_parts >= MAX_CONTROLS) continue;
            if (sscanf(p, " %39[^= \t]", name) != 1) continue;
            int x, y, w, h;
            if (sscanf(brace, "{ x = %d , y = %d , w = %d , h = %d", &x, &y, &w, &h) == 4) {
                struct skin_part *sp = &app->skin_parts[app->n_skin_parts++];
                snprintf(sp->name, sizeof sp->name, "%s", name);
                sp->x = x; sp->y = y; sp->w = w; sp->h = h;
            }
        }
    }
    fclose(f);
    log_line("pf-hwprobe: skin parsed: body='%s' lit='%s' parts=%d",
             app->skin_body, app->skin_lit, app->n_skin_parts);
    return 0;
}

/* Attach each control's skin-space rect by matching its skin_part name. Controls with
 * no matching [skin.parts] entry keep sw==0 and simply don't composite an overlay. */
static void resolve_skin_rects(struct app *app) {
    for (int i = 0; i < app->n_controls; i++) {
        struct control *c = &app->controls[i];
        for (int j = 0; j < app->n_skin_parts; j++) {
            if (!strcmp(c->name, app->skin_parts[j].name)) {
                c->sx = app->skin_parts[j].x; c->sy = app->skin_parts[j].y;
                c->sw = app->skin_parts[j].w; c->sh = app->skin_parts[j].h;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ evdev drain */

static void apply_event(struct app *app, const struct evdev_event *e) {
    if (e->type == EV_TYPE_SYN) return;
    for (int i = 0; i < app->n_controls; i++) {
        struct control *c = &app->controls[i];
        for (int j = 0; j < c->n_bindings; j++) {
            struct binding *b = &c->bindings[j];
            if (b->type == e->type && b->code == e->code) {
                b->value = e->value;
            }
        }
    }
}

static void drain_nodes(struct app *app) {
    struct evdev_event ev;
    for (int n = 0; n < app->n_fds; n++) {
        int fd = app->node_fd[n];
        while (1) {
            ssize_t r = read(fd, &ev, sizeof ev);
            if (r == (ssize_t)sizeof ev) {
                apply_event(app, &ev);
            } else {
                break;   /* EAGAIN / short read: no more pending events. */
            }
        }
    }
}

/* ------------------------------------------------------------------ activity state */

static double trigger_fraction(const struct control *c);   /* fwd decl */

static int control_is_active(const struct control *c) {
    /* Triggers REST at their axis MIN (not centre) and travel toward MAX, so a
     * centre-deadzone test would read them active at rest. Judge them by travel. */
    if (c->kind == WK_TRIGGER) {
        return trigger_fraction(c) > 0.5;
    }
    for (int j = 0; j < c->n_bindings; j++) {
        const struct binding *b = &c->bindings[j];
        if (b->type == EV_TYPE_KEY) {
            if (b->value) return 1;
        } else if (b->type == EV_TYPE_ABS) {
            /* stick/hat axes rest CENTRED: active past a 25%-of-range deadzone. */
            double centre = (b->vmin + b->vmax) / 2.0;
            double span   = (double)(b->vmax - b->vmin);
            double thr    = fabs(span) * 0.25;
            if (fabs((double)b->value - centre) > thr) return 1;
        }
    }
    return 0;
}

static double trigger_fraction(const struct control *c) {
    if (c->n_bindings == 0) return 0.0;
    const struct binding *b = &c->bindings[0];
    if (b->vmax == b->vmin) return 0.0;
    double f = (double)(b->value - b->vmin) / (double)(b->vmax - b->vmin);
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return f;
}

/* ------------------------------------------------------------------ input capability acquisition seam
 *
 * CAPABILITY MODEL (the epic invariant): the app obtains input THROUGH the E2 facade,
 * never by scanning /dev with ambient authority — that is the whole reason pf-hwprobe
 * exists as the capability-layer dog-food. Two parts, and one is currently a runtime gap:
 *
 *   (1) AUTHORIZATION — pf_acquire(session, "input") must return PF_OK. This routes the
 *       acquire DECISION through the facade (the v0 InProcessBackend under the sim; the
 *       pf-broker on-device). Done in acquire_input().
 *
 *   (2) THE EVENT FD — the frozen v1 C ABI (libpocketforge) exposes NO fd-returning input
 *       export: pf_acquire returns a STATUS code only, and the SCM_RIGHTS grabbed-uinput-fd
 *       handoff lives in pf-input-broker's Rust WIRE path, not in the C header. So a C app
 *       cannot receive the event fd through the facade today. Under the SIM we therefore
 *       read the DESCRIPTOR-MATCHED node whose PATH the platform provides via layout.txt
 *       (written by the sim's control surface — NEVER discovered by the app). This is a
 *       SIM-ONLY interim, ratified by the E6 coordinator (tsp-fr2n-coord, 2026-07-11).
 *
 * input_fd_for() is the SINGLE SWAPPABLE SEAM. The sim-impl opens the platform-provided
 * node path; the on-device impl (C8, tsp-fr2n.8) will instead call the new frozen-v1-
 * ADDITIVE runtime export `int pf_acquire_input_fd(PfSession*)` once E2 adds it — filed as
 * a child of tsp-e1b (E2), which this app is the first C consumer to need. Swapping the
 * read-source here is a zero-app-logic change. See the bead + PR for the full write-up.
 */
static int input_fd_for(struct app *app, const char *platform_node_path) {
    /* SIM-ONLY interim: the node PATH is platform-provided (layout.txt), not app-scanned.
     * DEVICE (C8/tsp-fr2n.8): replace this body with pf_acquire_input_fd(app->session)
     * once the E2/tsp-e1b fd export lands — no other app change. */
    (void)app;
    return open(platform_node_path, O_RDONLY | O_NONBLOCK);
}

static void acquire_input(struct app *app) {
    if (app->session) {
        int st = pf_acquire(app->session, "input");
        app->input_authorized = (st == PF_OK);
        if (app->input_authorized) {
            log_line("pf-hwprobe: input capability acquired via facade (pf_acquire -> PF_OK)");
        } else {
            log_line("pf-hwprobe: pf_acquire(\"input\") -> %s (%d) — NOT authorized, no input read",
                     pf_strerror(st), st);
            return;   /* graceful: unauthorized => no ambient node access */
        }
    } else {
        /* No facade session (descriptor-only degraded run): the platform-provided node
         * path is still the only source, and there is no facade to authorize against. */
        app->input_authorized = 1;
        log_line("pf-hwprobe: no facade session; reading platform node paths (descriptor-only)");
    }
    for (int i = 0; i < app->n_nodes; i++) {
        int fd = input_fd_for(app, app->node_path[i]);
        if (fd < 0) {
            log_line("pf-hwprobe: input node open %s failed: %s",
                     app->node_path[i], strerror(errno));
            continue;
        }
        app->node_fd[app->n_fds++] = fd;
        log_line("pf-hwprobe: input fd %d from platform node %s", fd, app->node_path[i]);
    }
}

/* ------------------------------------------------------------------ software fb + SDL */

static int make_framebuffer(size_t bytes, void **mem) {
    int fd = -1;
#ifdef SYS_memfd_create
    fd = (int)syscall(SYS_memfd_create, "pf-hwprobe-fb", 0u);
#endif
    if (fd >= 0 && ftruncate(fd, (off_t)bytes) != 0) {
        close(fd);
        fd = -1;
    }
    if (fd >= 0) {
        *mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (*mem == MAP_FAILED) {
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0) {
        *mem = calloc(1, bytes);
        return -1;
    }
    return fd;
}

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h,
                      int cr, int cg, int cb) {
    SDL_SetRenderDrawColor(r, (Uint8)cr, (Uint8)cg, (Uint8)cb, 255);
    SDL_FRect fr = { (float)x, (float)y, (float)w, (float)h };
    SDL_RenderFillRect(r, &fr);
}

/* C1 stub-rect fallback: used ONLY when the skin PNGs are unavailable (e.g. a bare run
 * with no staged assets) so the app still renders + the binding still proves out. */
static void render_stub_widget(SDL_Renderer *r, const struct control *c) {
    if (c->kind == WK_TRIGGER) {
        fill_rect(r, c->x, c->y, c->w, c->h, 70, 70, 70);
        int fw = (int)((double)c->w * trigger_fraction(c) + 0.5);
        if (fw > 0) fill_rect(r, c->x, c->y, fw, c->h, 220, 30, 30);
        return;
    }
    int on = control_is_active(c);
    fill_rect(r, c->x, c->y, c->w, c->h,
              on ? 220 : 70,
              on ? 30  : 70,
              on ? 30  : 70);
}

/* ------------------------------------------------------------------ skin textures + fit */

static void resolve_asset_path(const char *io_dir, const char *rel, char *out, size_t cap) {
    if (rel[0] == '/') snprintf(out, cap, "%s", rel);
    else               snprintf(out, cap, "%s/%s", io_dir, rel);
}

static SDL_Texture *load_texture_png(SDL_Renderer *r, const char *path, int *w, int *h) {
    int iw = 0, ih = 0;
    unsigned char *px = img_load_rgba(path, &iw, &ih);
    if (!px) {
        log_line("pf-hwprobe: skin PNG decode failed: %s", path);
        return NULL;
    }
    SDL_Surface *surf = SDL_CreateSurfaceFrom(iw, ih, SDL_PIXELFORMAT_RGBA32, px, iw * 4);
    if (!surf) {
        log_line("pf-hwprobe: SDL_CreateSurfaceFrom(skin) failed: %s", SDL_GetError());
        img_free(px);
        return NULL;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);   /* copies pixels */
    SDL_DestroySurface(surf);
    img_free(px);
    if (!tex) {
        log_line("pf-hwprobe: SDL_CreateTextureFromSurface(skin) failed: %s", SDL_GetError());
        return NULL;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);   /* opaque copy — art is fully opaque */
    if (w) *w = iw;
    if (h) *h = ih;
    return tex;
}

/* Recover the skin-space -> canvas-space transform (uniform scale s + offset ox,oy) by
 * least-squares over the controls that carry BOTH a skin rect (from [skin.parts]) and a
 * canvas rect (from layout.txt). This RECOVERS layout.py's own aspect-preserving fit from
 * its output — no duplication of the fit constant, no drift. It positions ONLY the body
 * background; every lit overlay is composited at the exact layout.txt canvas rect (the
 * same rect the test samples), so overlays never depend on this recovery being pixel-exact.
 */
static int recover_fit(struct app *app) {
    double sx_sum = 0, cx_sum = 0, sy_sum = 0, cy_sum = 0;
    int n = 0;
    for (int i = 0; i < app->n_controls; i++) {
        const struct control *c = &app->controls[i];
        if (c->sw <= 0 || c->sh <= 0 || c->w <= 0 || c->h <= 0) continue;
        sx_sum += c->sx + c->sw / 2.0; cx_sum += c->x + c->w / 2.0;
        sy_sum += c->sy + c->sh / 2.0; cy_sum += c->y + c->h / 2.0;
        n++;
    }
    if (n < 2) return -1;
    double sx_bar = sx_sum / n, cx_bar = cx_sum / n;
    double sy_bar = sy_sum / n, cy_bar = cy_sum / n;
    double numx = 0, denx = 0, numy = 0, deny = 0;
    for (int i = 0; i < app->n_controls; i++) {
        const struct control *c = &app->controls[i];
        if (c->sw <= 0 || c->sh <= 0 || c->w <= 0 || c->h <= 0) continue;
        double sxc = c->sx + c->sw / 2.0, cxc = c->x + c->w / 2.0;
        double syc = c->sy + c->sh / 2.0, cyc = c->y + c->h / 2.0;
        numx += (sxc - sx_bar) * (cxc - cx_bar); denx += (sxc - sx_bar) * (sxc - sx_bar);
        numy += (syc - sy_bar) * (cyc - cy_bar); deny += (syc - sy_bar) * (syc - sy_bar);
    }
    if (denx <= 0.0 || deny <= 0.0) return -1;
    double s = 0.5 * (numx / denx + numy / deny);   /* aspect-preserving: sx==sy in theory */
    if (!(s > 0.0)) return -1;
    app->fit_s  = s;
    app->fit_ox = cx_bar - s * sx_bar;
    app->fit_oy = cy_bar - s * sy_bar;
    return 0;
}

static void load_skin(struct app *app, SDL_Renderer *r, const char *io_dir) {
    if (app->skin_body[0] == '\0' || app->skin_lit[0] == '\0') {
        log_line("pf-hwprobe: descriptor declares no skin body/lit — stub render");
        return;
    }
    char bpath[PATH_BUF], lpath[PATH_BUF];
    resolve_asset_path(io_dir, app->skin_body, bpath, sizeof bpath);
    resolve_asset_path(io_dir, app->skin_lit,  lpath, sizeof lpath);
    int bw = 0, bh = 0, lw = 0, lh = 0;
    app->tex_body = load_texture_png(r, bpath, &bw, &bh);
    app->tex_lit  = load_texture_png(r, lpath, &lw, &lh);
    if (!app->tex_body || !app->tex_lit) {
        log_line("pf-hwprobe: skin textures unavailable (%s / %s) — stub render", bpath, lpath);
        return;
    }
    app->body_w = bw; app->body_h = bh;
    if (recover_fit(app) != 0) {
        log_line("pf-hwprobe: skin fit recovery failed (too few skin rects) — stub render");
        return;
    }
    app->have_skin = 1;
    log_line("pf-hwprobe: skin ready: body %dx%d, fit s=%.4f ox=%.1f oy=%.1f",
             bw, bh, app->fit_s, app->fit_ox, app->fit_oy);
}

/* Active red-highlight alpha (0..255). The lit SPRITE gives the fidelity; this translucent
 * red wash over the active control guarantees it reads unambiguously RED at its reference
 * point. WHY it's needed: the traced face-button lit art carries a DARK letter glyph
 * (A/B/X/Y) at the button's exact geometric CENTRE — which is precisely where the headline
 * `framebuffer_region(id).is_red()` contract samples (a 3x3 average). The button FACE is
 * bright red (214,64,64) but the glyph stroke through the centre drags the average below the
 * red threshold (r>=150) for the letter-bearing faces (east/west/north). A 0.5 red wash lifts
 * the centre average back above threshold on every control while leaving the lit art visible.
 * Sticks/dpad/shoulders/system keys have no centred glyph and pass on the sprite alone. */
#define ACTIVE_TINT_A 128

/* The C2 render: draw the device body, then for every ACTIVE control composite the lit
 * overlay at its canvas rect + a red active-highlight. The uniform "active -> lit" rule
 * lights buttons, the a523 Home key, and the a523 L3/R3 stick-clicks straight from
 * descriptor rows with ZERO per-device code. (C3/C4 layer proportional trigger + directional
 * stick/hat widgets on top of this foundation.) */
static void render_frame(SDL_Renderer *r, const struct app *app) {
    fill_rect(r, 0, 0, app->canvas_w, app->canvas_h, 24, 24, 24);
    if (app->have_skin) {
        SDL_FRect body_dst = {
            (float)app->fit_ox, (float)app->fit_oy,
            (float)(app->body_w * app->fit_s), (float)(app->body_h * app->fit_s)
        };
        SDL_RenderTexture(r, app->tex_body, NULL, &body_dst);
        for (int i = 0; i < app->n_controls; i++) {
            const struct control *c = &app->controls[i];
            if (c->sw <= 0 || c->sh <= 0) continue;
            if (c->kind == WK_TRIGGER) {
                /* Triggers render PROPORTIONALLY: the lit strip fills left-to-right to the
                 * axis fraction, so the E7 slider assertion (fraction ≈ value, monotonic)
                 * holds. C3 layers the slider-with-marker refinement on top. */
                double f = trigger_fraction(c);
                if (f <= 0.001) continue;
                float fw_src = (float)(c->sw * f);
                float fw_dst = (float)(c->w  * f);
                if (fw_dst < 1.0f) continue;
                SDL_FRect src = { (float)c->sx, (float)c->sy, fw_src, (float)c->sh };
                SDL_FRect dst = { (float)c->x,  (float)c->y,  fw_dst, (float)c->h  };
                SDL_RenderTexture(r, app->tex_lit, &src, &dst);
                continue;
            }
            if (!control_is_active(c)) continue;
            SDL_FRect src = { (float)c->sx, (float)c->sy, (float)c->sw, (float)c->sh };
            SDL_FRect dst = { (float)c->x,  (float)c->y,  (float)c->w,  (float)c->h  };
            SDL_RenderTexture(r, app->tex_lit, &src, &dst);
            /* red active-highlight wash (see ACTIVE_TINT_A) */
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, 214, 64, 64, ACTIVE_TINT_A);
            SDL_RenderFillRect(r, &dst);
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        }
    } else {
        for (int i = 0; i < app->n_controls; i++) {
            render_stub_widget(r, &app->controls[i]);
        }
    }
    /* C6: LED-grid widget — cells reflect the descriptor `[[actuators]]` count
     * (a133 = 23, a523 = 17; the per-device controller IC is descriptor data the
     * app never names). Rendered on top of the body/lit pass so it's visible
     * whether or not the skin loaded. */
    widget_led_grid_render(r, &app->actuators, app->canvas_w, app->canvas_h);
    SDL_RenderPresent(r);
}

static void dump_ppm_from_argb(const void *argb, unsigned char *rgb_scratch,
                               int w, int h, const char *path) {
    const uint32_t *px = (const uint32_t *)argb;
    for (int i = 0; i < w * h; i++) {
        rgb_scratch[i * 3 + 0] = (unsigned char)((px[i] >> 16) & 0xff);
        rgb_scratch[i * 3 + 1] = (unsigned char)((px[i] >>  8) & 0xff);
        rgb_scratch[i * 3 + 2] = (unsigned char)((px[i]      ) & 0xff);
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        log_line("pf-hwprobe: ppm open %s failed: %s", path, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb_scratch, 1, (size_t)w * h * 3, f);
    fclose(f);
}

/* ------------------------------------------------------------------ pf runtime seam */
static void connect_runtime(struct app *app, const char *io_dir) {
    char desc_path[PATH_BUF];
    snprintf(desc_path, sizeof desc_path, "%s/capabilities.toml", io_dir);
    struct stat st;
    if (stat(desc_path, &st) == 0) {
        app->session = pf_connect_descriptor(desc_path);
        if (app->session) {
            log_line("pf-hwprobe: pf_connect_descriptor(%s) ok, wire v%u",
                     desc_path, pf_wire_version());
            return;
        }
        log_line("pf-hwprobe: pf_connect_descriptor(%s) failed, falling back to env",
                 desc_path);
    } else {
        log_line("pf-hwprobe: no descriptor at %s (%s), trying env pf_connect()",
                 desc_path, strerror(errno));
    }
    app->session = pf_connect();
    if (app->session) {
        log_line("pf-hwprobe: pf_connect() (env) ok, wire v%u", pf_wire_version());
    } else {
        log_line("pf-hwprobe: pf_connect() returned NULL — running descriptor-only");
    }
}

/* ------------------------------------------------------------------ FIFO line I/O */

static int fifo_readline(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r <= 0) return -1;
        if (ch == '\n') break;
        buf[n++] = ch;
    }
    buf[n] = '\0';
    return (int)n;
}

static void fifo_reply(int fd, const char *s) {
    (void)!write(fd, s, strlen(s));
    (void)!write(fd, "\n", 1);
}

/* ------------------------------------------------------------------ tsp-osr renderer recipe (PINNED)
 *
 * tsp-osr = the SDL3 sunxifb RENDER segfault: a window created WITHOUT SDL_WINDOW_OPENGL
 * plus SDL_CreateRenderer() dereferences NULL inside PowerVR's libIMGegl.so at NULL+0x8,
 * because the backend left the window's EGL surface as EGL_NO_SURFACE. On sunxifb the SDL
 * software renderer is NOT compiled in and there are no fb hooks, so EVERY SDL_Renderer on
 * that backend IS the GLES2-over-PowerVR path — you cannot sidestep it by asking for a
 * software renderer on-device (that is only how the SIM renders, on a GPU-less host).
 *
 * THE PINNED RECIPE (device-free root-cause; the recipe every later PocketForge app reuses):
 *
 *   A. OWNED-SOURCE FIX (epic decision R-E "Fix A", landed in this bead): patch
 *      libsdl3-sunxifb so SUNXIFB_CreateWindow ALWAYS creates the EGL surface (self-loading
 *      EGL first for a non-OPENGL window, since SDL core only loads it when the app sets the
 *      OPENGL flag). With Fix A, SDL_CreateWindow(no flag) + SDL_CreateRenderer() no longer
 *      NULL-derefs. See libsdl3-sunxifb sdl3/src/video/sunxifb/SDL_sunxifbvideo.c.
 *
 *   B. APP-SIDE BELT-AND-SUSPENDERS (the on-device window recipe): create the window WITH
 *      SDL_WINDOW_OPENGL, which makes SDL core load EGL and the backend create the surface
 *      even on an UNPATCHED lib; then SDL_CreateRenderer(window, NULL) selects GLES2 and
 *      binds that valid surface. Honest on sunxifb, where the renderer IS GLES2 anyway (R-E).
 *
 *   C. THE SIM PATH (this binary, off-hardware): there is no panel/GL — we render with
 *      SDL_CreateSoftwareRenderer() on an off-screen surface (NO window, NO GL), which is
 *      inherently tsp-osr-safe. That is what main() below uses for the CI/sim proof.
 *
 * On-panel proof on real PowerVR silicon (that A+B actually present a frame and the NULL
 * deref is gone) is the C8 hardware gate (tsp-fr2n.8), owner-return-gated. This function
 * exercises recipe B off-hardware as a smoke check (best-effort; logged for the transcript).
 */
static void pin_tsp_osr_recipe(int w, int h) {
    /* Recipe B, exercised with the sim's software driver so it runs on a GPU-less host:
     * window WITHOUT relying on a NULL EGL surface, then SDL_CreateRenderer(). On-device
     * this same shape uses SDL_WINDOW_OPENGL + the GLES2 renderer (see the comment above). */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_Window *win = SDL_CreateWindow("pf-hwprobe-tsp-osr-recipe", w, h, 0);
    if (!win) {
        log_line("pf-hwprobe: tsp-osr recipe probe skipped (SDL_CreateWindow: %s)", SDL_GetError());
        return;
    }
    SDL_Renderer *r = SDL_CreateRenderer(win, NULL);   /* NULL => auto-select; must not NULL-deref */
    if (!r) {
        log_line("pf-hwprobe: tsp-osr recipe probe FAILED renderer NULL (%s)", SDL_GetError());
        SDL_DestroyWindow(win);
        return;
    }
    const char *name = SDL_GetRendererName(r);
    log_line("pf-hwprobe: tsp-osr recipe OK -> window(no NULL-EGL) + SDL_CreateRenderer -> '%s'",
             name ? name : "?");
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: pf-hwprobe <io-dir>\n");
        return 2;
    }
    const char *io_dir = argv[1];

    struct app app;
    memset(&app, 0, sizeof app);
    app.canvas_w = 1280;
    app.canvas_h = 720;

    char layout_p[PATH_BUF], req_p[PATH_BUF], resp_p[PATH_BUF];
    snprintf(layout_p, sizeof layout_p, "%s/layout.txt", io_dir);
    snprintf(req_p,    sizeof req_p,    "%s/req",        io_dir);
    snprintf(resp_p,   sizeof resp_p,   "%s/resp",       io_dir);

    if (parse_layout(&app, layout_p) != 0) return 3;
    parse_skin(&app, io_dir);
    resolve_skin_rects(&app);
    /* C6: parse [[actuators]] rows from the SAME descriptor for the LED count + rumble
     * presence. Missing rows leave app.actuators zeroed (a133's honest omission path). */
    actuators_parse_descriptor(&app.actuators, io_dir);
    log_line("pf-hwprobe: actuators: rumble=%s leds.count=%d",
             app.actuators.has_rumble ? "present" : "absent",
             app.actuators.led_count);

    /* SDL video: dummy driver — no window system needed under the sim. */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_line("pf-hwprobe: SDL_Init(VIDEO) failed (%s) — surface path still works",
                 SDL_GetError());
    }
    pin_tsp_osr_recipe(app.canvas_w, app.canvas_h);

    /* Off-screen ARGB framebuffer + software renderer (tsp-osr-safe: no window/GL). */
    size_t fb_bytes = (size_t)app.canvas_w * app.canvas_h * 4;
    void  *fb_mem   = NULL;
    int    fb_fd    = make_framebuffer(fb_bytes, &fb_mem);
    log_line("pf-hwprobe: fb %s %dx%d (%zu bytes)",
             fb_fd >= 0 ? "memfd" : "anon-buffer", app.canvas_w, app.canvas_h, fb_bytes);

    SDL_Surface *surf = SDL_CreateSurfaceFrom(app.canvas_w, app.canvas_h,
                                              SDL_PIXELFORMAT_XRGB8888,
                                              fb_mem, app.canvas_w * 4);
    if (!surf) {
        log_line("pf-hwprobe: SDL_CreateSurfaceFrom failed: %s", SDL_GetError());
        return 3;
    }
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(surf);
    if (!r) {
        log_line("pf-hwprobe: SDL_CreateSoftwareRenderer failed: %s", SDL_GetError());
        return 3;
    }

    /* Descriptor-driven E2 seam, then acquire input THROUGH the facade + load the skin. */
    connect_runtime(&app, io_dir);
    acquire_input(&app);
    load_skin(&app, r, io_dir);

    /* C6: exercise the FIRST OUTPUT CAPABILITY through the facade at startup, so the
     * descriptor-derived rumble shape shows up in the transcript on every run. The app
     * makes ONE unconditional pf_rumble_pulse() call — the primitive maps descriptor
     * presence + the `hapticsEnabled` preference to the three-way status. There is NO
     * app-side check of the preference (that would double-implement the E4 gate). */
    if (app.session) {
        int rst = pf_rumble_pulse(app.session, 40);
        log_line("pf-hwprobe: rumble startup pulse(40ms) -> %s (PF_RUMBLE=%d)",
                 actuators_rumble_status_name(rst), rst);
    }

    /* FIFO handshake — O_RDWR so open never blocks; the host owns the peer end. */
    int req_fd  = open(req_p,  O_RDWR);
    int resp_fd = open(resp_p, O_RDWR);
    if (req_fd < 0 || resp_fd < 0) {
        log_line("pf-hwprobe: FIFO open failed: %s", strerror(errno));
        return 4;
    }

    unsigned char *rgb_scratch = malloc((size_t)app.canvas_w * app.canvas_h * 3);
    if (!rgb_scratch) {
        log_line("pf-hwprobe: rgb scratch alloc failed");
        return 5;
    }

    fifo_reply(resp_fd, "ready");
    log_line("pf-hwprobe: ready — awaiting commands on %s", req_p);

    char line[PATH_BUF];
    while (fifo_readline(req_fd, line, sizeof line) >= 0) {
        if (!strncmp(line, "snap", 4)) {
            const char *path = line + 4;
            while (*path == ' ') path++;
            if (!*path) { fifo_reply(resp_fd, "err"); continue; }
            drain_nodes(&app);
            render_frame(r, &app);
            dump_ppm_from_argb(fb_mem, rgb_scratch, app.canvas_w, app.canvas_h, path);
            fifo_reply(resp_fd, "ok");
        } else if (!strncmp(line, "rumble", 6)) {
            /* C6 (tsp-fr2n.6): host-driven pulse through the C ABI. Same three-way
             * status the sim's Python broker_stub returns from `dev.acquire_rumble()
             * .pulse(...)` — verified by ci/c6-verify.py in both directions. */
            const char *arg = line + 6;
            while (*arg == ' ') arg++;
            uint32_t ms = (uint32_t)atoi(*arg ? arg : "40");
            if (app.session) {
                int rst = pf_rumble_pulse(app.session, ms);
                fifo_reply(resp_fd, actuators_rumble_status_name(rst));
            } else {
                fifo_reply(resp_fd, "err");
            }
        } else if (!strncmp(line, "quit", 4)) {
            fifo_reply(resp_fd, "bye");
            break;
        } else if (line[0]) {
            /* Unknown verb (e.g. "imu" — arrives with C5). Reply "err" per contract. */
            fifo_reply(resp_fd, "err");
        }
    }

    free(rgb_scratch);
    if (app.tex_body) SDL_DestroyTexture(app.tex_body);
    if (app.tex_lit)  SDL_DestroyTexture(app.tex_lit);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surf);
    if (fb_fd >= 0) { munmap(fb_mem, fb_bytes); close(fb_fd); } else { free(fb_mem); }
    SDL_Quit();
    if (app.session) pf_free(app.session);
    for (int i = 0; i < app.n_fds; i++) close(app.node_fd[i]);
    log_line("pf-hwprobe: exit clean");
    return 0;
}
