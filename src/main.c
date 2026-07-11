/*
 * pf-hwprobe — the PocketForge hardware hello-world diagnostic (C1 skeleton).
 *
 * The first first-party non-Steam-Link PocketForge app: a full-screen SDL3 diagnostic
 * whose main loop is PURELY data-bound to a device capability descriptor. For each
 * `[[inputs]]` row it renders one widget on a fixed 1280x720 canvas and lights it on
 * press. The a133 (base Pro) and a523 (Pro S) differ only by DATA — same binary, same
 * source, different descriptor.
 *
 * C1 SCOPE (this file, bead tsp-fr2n.1): the skeleton — descriptor wiring + main loop +
 * stub-rect widgets + the sim's FIFO handshake. The real button/trigger/hat/stick body
 * rendering lands in C2 (tsp-fr2n.2) after the tsp-osr renderer recipe is pinned.
 *
 * LINKS (per the epic decisions):
 *   * SDL3 via `SDL3_DYNAMIC_API` — this binary is built against SDL3's public headers
 *     and static-linked with a stock SDL3 for the sim proof; SDL3's built-in DYNAPI
 *     jump table lets `SDL_DYNAMIC_API=/…/libSDL3-pocketforge.so.0` swap in the on-
 *     device sunxifb SDL3 unchanged, which is exactly the Steam-Link pattern. NOT
 *     LD_PRELOAD.
 *   * `libpocketforge` (E2 C ABI, frozen v1) via `pf_connect_descriptor()` — proves
 *     the app talks to the PocketForge runtime facade, not raw /dev/ nodes. C1 opens
 *     the session, reports the wire version, and closes it cleanly; C2–C6 layer
 *     `pf_acquire` / `pf_rumble_pulse` / IIO reads on top.
 *
 * SIM CONTRACT (E5 / matches `sim/control/hwprobe-lite.c`):
 *   Usage:     pf-hwprobe <io-dir>
 *   Reads:     <io-dir>/layout.txt        canvas + evdev nodes + control rects
 *              <io-dir>/capabilities.toml  descriptor consumed via pf_connect_descriptor
 *   FIFOs:     <io-dir>/req  (host -> app), <io-dir>/resp (app -> host)
 *   Protocol:  emit "ready" on start; on "snap <ppm-path>" drain events + render +
 *              write PPM + reply "ok" (or "err"); on "quit" reply "bye" and exit;
 *              unknown lines reply "err".
 *
 * DEVICE-FREE + GREP-CLEAN: this source names ZERO per-device evdev symbol
 * (no face-button / home-key / trigger-axis / IMU / rumble / LED-controller
 * strings) and ZERO per-device literal — every widget, every code, every rect
 * enters through `layout.txt`, which is descriptor-derived. The only kernel-ABI
 * constants baked here are the three generic evdev event-type numbers
 * (SYN / KEY / ABS), because their numeric values are frozen and the runtime
 * uses them for every device. See README.md for the enforcing grep test.
 */

#include <SDL3/SDL.h>
#include <pocketforge.h>

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
    char             name[MAX_NAME];
    enum widget_kind kind;
    int              x, y, w, h;
    int              n_bindings;
    struct binding   bindings[MAX_BINDINGS];
};

struct app {
    int             canvas_w, canvas_h;
    int             n_controls;
    struct control  controls[MAX_CONTROLS];
    int             n_nodes;
    int             node_fd[MAX_NODES];
    PfSession      *session;   /* E2 handle; may be NULL if no descriptor found */
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
            /* rotation token intentionally ignored: at C1 we render in canvas space. */
        } else if (!strcmp(head, "node")) {
            char *p = next_tok(&save);
            if (!p || app->n_nodes >= MAX_NODES) continue;
            int fd = open(p, O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                log_line("pf-hwprobe: node open %s failed: %s", p, strerror(errno));
                fclose(f);
                return -1;
            }
            app->node_fd[app->n_nodes++] = fd;
            log_line("pf-hwprobe: opened evdev node %s (fd %d)", p, fd);
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
    for (int n = 0; n < app->n_nodes; n++) {
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

static int control_is_active(const struct control *c) {
    for (int j = 0; j < c->n_bindings; j++) {
        const struct binding *b = &c->bindings[j];
        if (b->type == EV_TYPE_KEY) {
            if (b->value) return 1;
        } else if (b->type == EV_TYPE_ABS) {
            /* deadzone: 25% of range past the resting centre */
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

/* C1 stub-rect widget: a filled rectangle whose colour reports control state.
 * Idle = grey, active = red, trigger = grey track + red proportional fill. C2
 * replaces this with the button-light / slider-with-marker / hat-cross / stick-
 * calibration body renders. */
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

static void render_frame(SDL_Renderer *r, const struct app *app) {
    fill_rect(r, 0, 0, app->canvas_w, app->canvas_h, 24, 24, 24);
    for (int i = 0; i < app->n_controls; i++) {
        render_stub_widget(r, &app->controls[i]);
    }
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

/* ------------------------------------------------------------------ pf runtime seam
 *
 * C1 responsibility: prove the app talks to the PocketForge runtime facade. We
 * try `pf_connect_descriptor(<io-dir>/capabilities.toml)` first (explicit path,
 * matches the sim's descriptor bind) and fall back to `pf_connect()` (env-driven,
 * matches the on-device supervisor→app handoff). Either way we log the wire
 * version so the sim transcript captures a truthful E2 heartbeat.
 */
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

/* ------------------------------------------------------------------ tsp-osr recipe pin
 *
 * The E6 epic decision R-E fix (device-free root-cause of tsp-osr): a WINDOW
 * created WITHOUT SDL_WINDOW_OPENGL plus SDL_CreateRenderer(win,"software")
 * MUST succeed. C2 pins the real renderer recipe on the on-panel path; here we
 * only log the sim-side equivalent so the C1 transcript records the intent.
 */
static void probe_software_recipe(int w, int h) {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_Window *win = SDL_CreateWindow("pf-hwprobe-c1-recipe-probe", w, h, 0);
    if (!win) {
        log_line("pf-hwprobe: recipe probe skipped (SDL_CreateWindow: %s)", SDL_GetError());
        return;
    }
    SDL_Renderer *r = SDL_CreateRenderer(win, "software");
    if (!r) {
        log_line("pf-hwprobe: recipe probe FAILED renderer NULL (%s)", SDL_GetError());
        SDL_DestroyWindow(win);
        return;
    }
    const char *name = SDL_GetRendererName(r);
    log_line("pf-hwprobe: recipe probe ok -> renderer '%s'", name ? name : "?");
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

    /* SDL video: dummy driver — no window system needed under the sim. */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_line("pf-hwprobe: SDL_Init(VIDEO) failed (%s) — surface path still works",
                 SDL_GetError());
    }
    probe_software_recipe(app.canvas_w, app.canvas_h);

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

    /* Descriptor-driven E2 seam. */
    connect_runtime(&app, io_dir);

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
        } else if (!strncmp(line, "quit", 4)) {
            fifo_reply(resp_fd, "bye");
            break;
        } else if (line[0]) {
            /* Unknown verb (e.g. "imu" — arrives with C5). Reply "err" per contract. */
            fifo_reply(resp_fd, "err");
        }
    }

    free(rgb_scratch);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surf);
    if (fb_fd >= 0) { munmap(fb_mem, fb_bytes); close(fb_fd); } else { free(fb_mem); }
    SDL_Quit();
    if (app.session) pf_free(app.session);
    for (int i = 0; i < app.n_nodes; i++) close(app.node_fd[i]);
    log_line("pf-hwprobe: exit clean");
    return 0;
}
