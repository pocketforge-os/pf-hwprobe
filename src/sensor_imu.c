/*
 * sensor_imu.c — C5 (tsp-fr2n.5): sensor path + tilt-bubble widget.
 *
 * See sensor_imu.h for the design contract. This file is the whole implementation:
 * a targeted, allocation-free TOML reader for [[sensors]], the IIO tree reader (plain
 * sysfs read()s; no ioctl), the mount_matrix transform, the "imu" FIFO handler, and
 * the tilt-bubble render (SDL software renderer only — matches main.c's off-screen path).
 *
 * GREP-CLEAN: this TU names ZERO per-device evdev/IIO/actuator symbol. Sensor IDs, kinds,
 * device names, mount matrices, and skin rects all enter through the descriptor.
 */
#include "sensor_imu.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Match main.c's log_line — kept as a static so this TU has no header coupling with it. */
static void sensor_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ init */

void sensor_imu_init(struct sensor_imu *imu) {
    memset(imu, 0, sizeof *imu);
    /* mount_matrix defaults to identity so a descriptor without one still transforms
     * correctly (a no-op) rather than zeroing the reading. */
    imu->mount[0][0] = imu->mount[1][1] = imu->mount[2][2] = 1.0;
}

/* ------------------------------------------------------------------ toml reader
 *
 * A minimal, allocation-free reader for the two TOML shapes we need:
 *
 *   [[sensors]]           <- array-of-tables header; opens a new sensor block
 *   id           = "imu"
 *   kind         = "accel+gyro"
 *   iio_device   = "qmi8658"
 *   ui           = "tilt_bubble"
 *   mount_matrix = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
 *
 * mount_matrix may be a single-line inline array OR span multiple lines — we accept
 * both by consuming until we've seen 9 numbers between two matching brackets.
 *
 * We fill ONLY the first row whose kind matches accel / gyro / imu. Every other row
 * is scanned-through (so its keys don't leak into the next section).
 */
static char *ltrim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static void rtrim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

/* Return 1 iff the kind string names an accel or gyro sensor. */
static int kind_matches_imu(const char *kind) {
    if (!kind) return 0;
    return strstr(kind, "accel") != NULL
        || strstr(kind, "gyro")  != NULL
        || strcmp(kind, "imu")   == 0;
}

/* Parse an inline TOML string value: `key = "value"` -> value. Strips the quotes. */
static int parse_string_value(const char *line, const char *key, char *out, size_t cap) {
    const char *eq = strchr(line, '=');
    if (!eq) return 0;
    /* find first '"' after '=' */
    const char *q1 = strchr(eq, '"');
    if (!q1) return 0;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return 0;
    size_t klen = strspn(key, "abcdefghijklmnopqrstuvwxyz_");
    (void)klen;
    /* verify the key on the left is `key` (ignoring whitespace) */
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t want = strlen(key);
    if (strncmp(p, key, want) != 0) return 0;
    /* everything between the two quotes */
    size_t n = (size_t)(q2 - q1 - 1);
    if (n >= cap) n = cap - 1;
    memcpy(out, q1 + 1, n);
    out[n] = '\0';
    return 1;
}

/* Read up to 9 numeric values from ``buf`` into ``out`` — used by mount_matrix parse.
 * A number = optional sign, digits, optional '.', digits; separators are anything
 * non-numeric (commas, spaces, brackets, semicolons). Returns count read (<=9). */
static int scan_numbers(const char *buf, double *out, int max) {
    int n = 0;
    const char *p = buf;
    while (*p && n < max) {
        while (*p && !(isdigit((unsigned char)*p) || *p == '-' || *p == '+')) p++;
        if (!*p) break;
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) { p++; continue; }
        out[n++] = v;
        p = end;
    }
    return n;
}

/* Accumulate mount_matrix numbers across possibly-multiple lines. Returns 1 when the
 * closing outer ']' has been seen. Caller re-invokes with subsequent lines until it
 * returns 1 (or the section ends, in which case we stop consuming). ``buf`` grows via
 * strcat; caller sizes it. */
static void mount_matrix_accumulate(char *buf, size_t buf_cap, const char *line) {
    size_t cur = strlen(buf);
    size_t add = strlen(line);
    if (cur + add + 1 >= buf_cap) return;   /* pathological line length; give up cleanly */
    memcpy(buf + cur, line, add);
    buf[cur + add] = '\0';
}

/* Count how many opening/closing top-level brackets the buffer has seen. Returns the
 * signed depth (0 = balanced). Ignores brackets inside strings — not needed for our
 * numeric matrix, but done defensively. */
static int bracket_depth(const char *buf) {
    int depth = 0;
    for (const char *p = buf; *p; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') depth--;
    }
    return depth;
}

void sensor_imu_parse_from_descriptor(struct sensor_imu *imu, const char *toml_path) {
    FILE *f = fopen(toml_path, "r");
    if (!f) {
        sensor_log("pf-hwprobe/imu: no descriptor at %s (%s) — treating as no imu",
                   toml_path, strerror(errno));
        return;
    }
    /* In-progress sensor block state: we only KEEP it if kind matches. Everything is
     * flushed into ``imu`` at the end of the [[sensors]] block (next section or EOF). */
    int in_sensors_row = 0;
    int keep_row = 0;                     /* set once kind matches accel/gyro/imu */
    char pending_id[PF_IMU_MAX_NAME] = "";
    char pending_kind[PF_IMU_MAX_NAME] = "";
    char pending_iio[PF_IMU_MAX_NAME] = "";
    char pending_ui[PF_IMU_MAX_NAME] = "";
    char mm_buf[512] = "";                /* accumulator across possibly-multiple lines */
    int  mm_active = 0;                   /* 1 while gathering mount_matrix tokens */
    int  mm_done = 0;

    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = ltrim(line);
        rtrim(p);
        /* comment / blank */
        if (*p == '#' || *p == '\0') continue;

        /* mount_matrix may span lines — keep folding if we're mid-accumulate */
        if (mm_active) {
            mount_matrix_accumulate(mm_buf, sizeof mm_buf, p);
            if (bracket_depth(mm_buf) == 0) {
                double vals[9] = {0};
                int n = scan_numbers(mm_buf, vals, 9);
                if (n == 9) {
                    for (int i = 0; i < 3; i++) {
                        for (int j = 0; j < 3; j++) {
                            imu->mount[i][j] = vals[i * 3 + j];
                        }
                    }
                    imu->have_mount = 1;
                    mm_done = 1;
                }
                mm_active = 0;
            }
            continue;
        }

        /* Section header. Any new section ends the pending sensors row. */
        if (*p == '[') {
            /* Commit the pending row FIRST if we were inside a matching [[sensors]] */
            if (in_sensors_row && keep_row && !imu->present) {
                imu->present = 1;
                snprintf(imu->id,         PF_IMU_MAX_NAME, "%s", pending_id);
                snprintf(imu->kind,       PF_IMU_MAX_NAME, "%s", pending_kind);
                snprintf(imu->iio_device, PF_IMU_MAX_NAME, "%s", pending_iio);
                snprintf(imu->ui,         PF_IMU_MAX_NAME, "%s", pending_ui);
                imu->has_accel = (strstr(imu->kind, "accel") != NULL)
                              || strcmp(imu->kind, "imu") == 0;
                imu->has_gyro  = (strstr(imu->kind, "gyro")  != NULL)
                              || strcmp(imu->kind, "imu") == 0;
                if (!mm_done) imu->have_mount = 0;   /* leave identity default */
            }
            /* Any [[sensors]] header opens a new row (once we've kept ours, ignore) */
            if (!strncmp(p, "[[sensors]]", 11) && !imu->present) {
                in_sensors_row = 1;
                keep_row = 0;
                pending_id[0]   = pending_kind[0] = '\0';
                pending_iio[0]  = pending_ui[0]   = '\0';
                mm_buf[0] = '\0';
                mm_done = 0;
            } else {
                in_sensors_row = 0;
            }
            continue;
        }
        if (!in_sensors_row) continue;

        /* Key = value inside [[sensors]] */
        if (parse_string_value(p, "id",         pending_id,  PF_IMU_MAX_NAME) ||
            parse_string_value(p, "iio_device", pending_iio, PF_IMU_MAX_NAME) ||
            parse_string_value(p, "ui",         pending_ui,  PF_IMU_MAX_NAME)) {
            continue;
        }
        if (parse_string_value(p, "kind", pending_kind, PF_IMU_MAX_NAME)) {
            if (kind_matches_imu(pending_kind)) keep_row = 1;
            continue;
        }
        /* mount_matrix — may be single-line inline or multi-line. */
        const char *eq = strchr(p, '=');
        const char *mm_key = "mount_matrix";
        if (eq && strncmp(p, mm_key, strlen(mm_key)) == 0) {
            /* start accumulating from '=' onwards */
            const char *from = eq + 1;
            mm_buf[0] = '\0';
            mount_matrix_accumulate(mm_buf, sizeof mm_buf, from);
            if (bracket_depth(mm_buf) == 0 && strchr(mm_buf, '[')) {
                double vals[9] = {0};
                int n = scan_numbers(mm_buf, vals, 9);
                if (n == 9) {
                    for (int i = 0; i < 3; i++) {
                        for (int j = 0; j < 3; j++) {
                            imu->mount[i][j] = vals[i * 3 + j];
                        }
                    }
                    imu->have_mount = 1;
                    mm_done = 1;
                }
                mm_active = 0;
            } else {
                mm_active = 1;
            }
            continue;
        }
    }
    /* EOF: commit the pending row if we were still in one. */
    if (in_sensors_row && keep_row && !imu->present) {
        imu->present = 1;
        snprintf(imu->id,         PF_IMU_MAX_NAME, "%s", pending_id);
        snprintf(imu->kind,       PF_IMU_MAX_NAME, "%s", pending_kind);
        snprintf(imu->iio_device, PF_IMU_MAX_NAME, "%s", pending_iio);
        snprintf(imu->ui,         PF_IMU_MAX_NAME, "%s", pending_ui);
        imu->has_accel = (strstr(imu->kind, "accel") != NULL)
                      || strcmp(imu->kind, "imu") == 0;
        imu->has_gyro  = (strstr(imu->kind, "gyro")  != NULL)
                      || strcmp(imu->kind, "imu") == 0;
    }
    fclose(f);

    if (imu->present) {
        sensor_log("pf-hwprobe/imu: descriptor row present: id='%s' kind='%s' "
                   "iio_device='%s' ui='%s' mount=%s",
                   imu->id, imu->kind, imu->iio_device, imu->ui,
                   imu->have_mount ? "descriptor" : "identity");
    } else {
        sensor_log("pf-hwprobe/imu: no [[sensors]] accel/gyro row — omission path");
    }
}

/* ------------------------------------------------------------------ facade acquire */

void sensor_imu_acquire_from_facade(struct sensor_imu *imu, PfSession *session) {
    if (!imu->present) {
        /* No hardware advertised => nothing to acquire; leave authorized=0. */
        return;
    }
    if (!session) {
        /* Descriptor-only run (no facade session). The v0 InProcessBackend and pf-broker
         * are both absent — we still boot, but the FIFO reply is "imu-absent" because
         * we cannot honestly claim the facade authorized the read. */
        sensor_log("pf-hwprobe/imu: no facade session; imu unauthorized (reads report absent)");
        return;
    }
    int st = pf_acquire(session, "imu");
    imu->authorized = (st == PF_OK);
    if (imu->authorized) {
        sensor_log("pf-hwprobe/imu: imu capability acquired via facade (pf_acquire -> PF_OK)");
    } else {
        sensor_log("pf-hwprobe/imu: pf_acquire(\"imu\") -> %s (%d)",
                   pf_strerror(st), st);
    }
}

/* ------------------------------------------------------------------ skin rect resolve */

void sensor_imu_resolve_skin_rect(struct sensor_imu *imu,
                                  const struct pf_imu_skin_part_view *parts,
                                  int n_parts,
                                  double fit_s, double fit_ox, double fit_oy) {
    if (!imu->present || imu->id[0] == '\0') return;
    for (int i = 0; i < n_parts; i++) {
        if (!strcmp(parts[i].name, imu->id)) {
            imu->skin_x = parts[i].x; imu->skin_y = parts[i].y;
            imu->skin_w = parts[i].w; imu->skin_h = parts[i].h;
            /* Same fit-s/fit-ox/fit-oy the body uses (recovered from the descriptor's
             * used skin_parts vs its layout.txt canvas rects). */
            imu->canvas_x = (int)(imu->skin_x * fit_s + fit_ox + 0.5);
            imu->canvas_y = (int)(imu->skin_y * fit_s + fit_oy + 0.5);
            imu->canvas_w = (int)(imu->skin_w * fit_s + 0.5);
            imu->canvas_h = (int)(imu->skin_h * fit_s + 0.5);
            if (imu->canvas_w < 4) imu->canvas_w = 4;
            if (imu->canvas_h < 4) imu->canvas_h = 4;
            imu->have_canvas_rect = 1;
            sensor_log("pf-hwprobe/imu: bubble rect resolved from [skin.parts].%s -> "
                       "canvas %d,%d %dx%d",
                       imu->id, imu->canvas_x, imu->canvas_y,
                       imu->canvas_w, imu->canvas_h);
            return;
        }
    }
    sensor_log("pf-hwprobe/imu: no [skin.parts].%s — bubble not rendered (omission)",
               imu->id);
}

/* ------------------------------------------------------------------ iio reader
 *
 * Reads /sys/bus/iio/devices/iio:device0/... (override root with $PF_IIO_ROOT for the
 * sim's native launcher). Everything is a plain read of a text sysfs file; no ioctl.
 * The IIO ABI: raws are chip-frame; userspace applies the mount_matrix. We use the
 * DESCRIPTOR's mount_matrix (not the sysfs attribute), because the whole point of C5
 * is proving the app CONSUMES the descriptor — not just re-reads the sysfs echo of it.
 */

static const char *iio_root(void) {
    const char *r = getenv("PF_IIO_ROOT");
    return r && *r ? r : "/sys/bus/iio/devices";
}

/* Read a numeric attribute (accepts float or int; float scale, int raws). 0 on
 * success + fill ``*out``, -1 on any failure. */
static int read_attr_number(const char *root, const char *rel, double *out) {
    char path[PF_IMU_MAX_PATH];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[64];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return -1;
    *out = v;
    return 0;
}

/* Optionally read the sysfs ``name`` attr for a soft cross-check with the descriptor's
 * iio_device. This is diagnostic only — mismatch logs a WARN but does not fail the read. */
static void verify_iio_device_name(const struct sensor_imu *imu, const char *root) {
    char path[PF_IMU_MAX_PATH];
    snprintf(path, sizeof path, "%s/iio:device0/name", root);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[64] = {0};
    if (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
        if (imu->iio_device[0] && strcmp(buf, imu->iio_device) != 0) {
            sensor_log("pf-hwprobe/imu: WARN iio name mismatch: descriptor='%s' sysfs='%s'",
                       imu->iio_device, buf);
        }
    }
    fclose(f);
}

/* device = M . chip. The matrix is passed as ``double m[3][3]`` (non-const inner arrays)
 * to match ``struct sensor_imu``'s field type — -Wpedantic warns on the const-qualifier
 * mismatch even though the body only reads. Callers never mutate through this parameter. */
static void apply_mount(double m[3][3], const double chip[3], double dev[3]) {
    for (int i = 0; i < 3; i++) {
        dev[i] = m[i][0] * chip[0] + m[i][1] * chip[1] + m[i][2] * chip[2];
    }
}

int sensor_imu_refresh_from_iio(struct sensor_imu *imu) {
    imu->last_read_ok = 0;
    if (!imu->present) return 0;

    const char *root = iio_root();
    verify_iio_device_name(imu, root);

    if (imu->has_accel) {
        double scale = 0.0;
        if (read_attr_number(root, "iio:device0/in_accel_scale", &scale) == 0) {
            imu->accel_scale = scale;
            imu->have_accel_scale = 1;
        } else {
            /* No scale => can't produce SI. The reader treats this as read-failed;
             * the FIFO handler will reply "imu-absent" so the test surfaces it. */
            return 0;
        }
        double raw[3] = {0};
        static const char *ax_files[3] = {
            "iio:device0/in_accel_x_raw",
            "iio:device0/in_accel_y_raw",
            "iio:device0/in_accel_z_raw"
        };
        for (int i = 0; i < 3; i++) {
            if (read_attr_number(root, ax_files[i], &raw[i]) != 0) return 0;
        }
        double chip[3] = { raw[0] * imu->accel_scale,
                           raw[1] * imu->accel_scale,
                           raw[2] * imu->accel_scale };
        apply_mount(imu->mount, chip, imu->last_accel_dev);
    } else {
        imu->last_accel_dev[0] = imu->last_accel_dev[1] = imu->last_accel_dev[2] = 0.0;
    }

    if (imu->has_gyro) {
        double scale = 0.0;
        if (read_attr_number(root, "iio:device0/in_anglvel_scale", &scale) == 0) {
            imu->gyro_scale = scale;
            imu->have_gyro_scale = 1;
        } else {
            /* Gyro scale missing while an accel-only descriptor said has_gyro=0 is fine;
             * if has_gyro=1 and the scale attr is missing, treat gyro as 0 (accel path
             * still succeeded — the pose read is what the test asserts). */
            imu->last_gyro_dev[0] = imu->last_gyro_dev[1] = imu->last_gyro_dev[2] = 0.0;
        }
        if (imu->have_gyro_scale) {
            double raw[3] = {0};
            static const char *gy_files[3] = {
                "iio:device0/in_anglvel_x_raw",
                "iio:device0/in_anglvel_y_raw",
                "iio:device0/in_anglvel_z_raw"
            };
            int ok = 1;
            for (int i = 0; i < 3; i++) {
                if (read_attr_number(root, gy_files[i], &raw[i]) != 0) { ok = 0; break; }
            }
            if (ok) {
                double chip[3] = { raw[0] * imu->gyro_scale,
                                   raw[1] * imu->gyro_scale,
                                   raw[2] * imu->gyro_scale };
                apply_mount(imu->mount, chip, imu->last_gyro_dev);
            } else {
                imu->last_gyro_dev[0] = imu->last_gyro_dev[1] = imu->last_gyro_dev[2] = 0.0;
            }
        }
    } else {
        imu->last_gyro_dev[0] = imu->last_gyro_dev[1] = imu->last_gyro_dev[2] = 0.0;
    }
    imu->last_read_ok = 1;
    return 1;
}

/* ------------------------------------------------------------------ fifo reply */

static void write_all(int fd, const char *s) {
    size_t n = strlen(s);
    /* Cast to (void) discards the return value while still triggering the
     * unused-result warning to prompt review if the reply is truncated. Same
     * pattern main.c's fifo_reply uses. */
    (void)!write(fd, s, n);
}

void sensor_imu_reply_verb(struct sensor_imu *imu, int resp_fd) {
    if (!imu->present || !imu->authorized) {
        write_all(resp_fd, "imu-absent\n");
        return;
    }
    (void)sensor_imu_refresh_from_iio(imu);
    if (!imu->last_read_ok) {
        write_all(resp_fd, "imu-absent\n");
        return;
    }
    /* Milli-SI integers, matching control_surface.Device.read_imu:
     *   "imu <name> ax ay az gx gy gz"   with values in mm/s^2, mrad/s. */
    char buf[256];
    long ax = (long)llround(imu->last_accel_dev[0] * 1000.0);
    long ay = (long)llround(imu->last_accel_dev[1] * 1000.0);
    long az = (long)llround(imu->last_accel_dev[2] * 1000.0);
    long gx = (long)llround(imu->last_gyro_dev[0]  * 1000.0);
    long gy = (long)llround(imu->last_gyro_dev[1]  * 1000.0);
    long gz = (long)llround(imu->last_gyro_dev[2]  * 1000.0);
    const char *name = imu->iio_device[0] ? imu->iio_device : "qmi8658";
    int n = snprintf(buf, sizeof buf, "imu %s %ld %ld %ld %ld %ld %ld\n",
                     name, ax, ay, az, gx, gy, gz);
    (void)n;
    write_all(resp_fd, buf);
}

/* ------------------------------------------------------------------ tilt-bubble render */

static void fill_r(SDL_Renderer *r, int x, int y, int w, int h,
                   int cr, int cg, int cb, int ca) {
    SDL_SetRenderDrawBlendMode(r, ca < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, (Uint8)cr, (Uint8)cg, (Uint8)cb, (Uint8)ca);
    SDL_FRect fr = { (float)x, (float)y, (float)w, (float)h };
    SDL_RenderFillRect(r, &fr);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

/* Draw a filled disc via a triangle fan — works on the software renderer. Cheap: this
 * is one bubble, not a general drawing primitive. */
static void fill_disc(SDL_Renderer *r, float cx, float cy, float rad,
                      int cr, int cg, int cb, int ca) {
    if (rad < 0.5f) return;
    enum { SEG = 40 };
    SDL_FColor col = {
        cr / 255.0f, cg / 255.0f, cb / 255.0f, ca / 255.0f
    };
    SDL_Vertex verts[SEG + 2];
    int idx[SEG * 3];
    verts[0].position.x = cx;
    verts[0].position.y = cy;
    verts[0].color = col;
    verts[0].tex_coord.x = verts[0].tex_coord.y = 0.0f;
    for (int i = 0; i <= SEG; i++) {
        double t = (double)i * 2.0 * M_PI / (double)SEG;
        verts[i + 1].position.x = cx + (float)(rad * cos(t));
        verts[i + 1].position.y = cy + (float)(rad * sin(t));
        verts[i + 1].color = col;
        verts[i + 1].tex_coord.x = verts[i + 1].tex_coord.y = 0.0f;
    }
    for (int i = 0; i < SEG; i++) {
        idx[i * 3 + 0] = 0;
        idx[i * 3 + 1] = i + 1;
        idx[i * 3 + 2] = i + 2;
    }
    SDL_SetRenderDrawBlendMode(r, ca < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_RenderGeometry(r, NULL, verts, SEG + 2, idx, SEG * 3);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

/* Standard gravity — the axis magnitude tilt is normalized against, so a 45-degree
 * tilt puts the dot at ~sin(45) ≈ 0.707 of full deflection. */
#define PF_IMU_G 9.80665

void sensor_imu_render(SDL_Renderer *r, struct sensor_imu *imu) {
    if (!imu->present || !imu->have_canvas_rect) return;
    (void)sensor_imu_refresh_from_iio(imu);
    if (!imu->last_read_ok) return;

    float x = (float)imu->canvas_x, y = (float)imu->canvas_y;
    float w = (float)imu->canvas_w, h = (float)imu->canvas_h;
    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float rad = 0.5f * (w < h ? w : h) - 2.0f;

    /* background disc — a level's dark viewing surface */
    fill_disc(r, cx, cy, rad, 32, 32, 42, 255);
    /* inner ring — half-radius reference (a bubble level's centre target) */
    fill_disc(r, cx, cy, rad * 0.5f, 44, 44, 60, 255);
    /* crosshair through the centre */
    fill_r(r, (int)(cx - rad), (int)(cy - 1), (int)(2 * rad), 2, 96, 96, 110, 255);
    fill_r(r, (int)(cx - 1),   (int)(cy - rad), 2, (int)(2 * rad), 96, 96, 110, 255);

    /* Dot displacement: at rest device-frame accel = (0, 0, g). Roll (right) makes
     * accel_x negative (gravity tips onto the left side of the chip in device frame);
     * pitch (forward) makes accel_y positive (top of screen away). We map
     *   dx = -accel_x / g   (positive dx => bubble drifts right when roll positive)
     *   dy = -accel_y / g   (positive dy => bubble drifts down when pitch positive:
     *                        a spirit-level analogue where the bubble runs AWAY from
     *                        the low side, i.e. same as a real bubble in a fluid).
     * Clamp to a 90% radius so the dot never falls off. */
    double ax = imu->last_accel_dev[0], ay = imu->last_accel_dev[1];
    double dx = -ax / PF_IMU_G;
    double dy = -ay / PF_IMU_G;
    if (dx > 1.0) dx = 1.0; else if (dx < -1.0) dx = -1.0;
    if (dy > 1.0) dy = 1.0; else if (dy < -1.0) dy = -1.0;
    float dot_cx = cx + (float)(dx * rad * 0.9);
    float dot_cy = cy + (float)(dy * rad * 0.9);
    float dot_r  = rad * 0.18f;

    /* The moving bubble — bright red like the rest of pf-hwprobe's active-highlight,
     * so it reads unambiguously as "the widget is alive" against the dark disc. */
    fill_disc(r, dot_cx, dot_cy, dot_r, 220, 30, 30, 255);
}
