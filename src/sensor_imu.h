/*
 * sensor_imu.h — C5 (tsp-fr2n.5): the sensor path + tilt-bubble widget.
 *
 * The FIRST sensor capability in pf-hwprobe. Everything here is descriptor-driven and
 * routed through the E2 facade — the app never scans /dev with ambient authority:
 *
 *   1. Parse the descriptor's ``[[sensors]]`` rows out of ``capabilities.toml`` — a
 *      row whose ``kind`` names accel/gyro (``accel``, ``gyro``, ``accel+gyro``, ``imu``)
 *      is the IMU. NO row => the app is on a descriptor with no IMU — the OMISSION
 *      path both shipping devices ride today (a133 has no sensors; the a523 imu is
 *      DT-present but driver-UNBOUND per SPIKE-0 so its row is omitted from the
 *      descriptor). This is the graceful missing-hardware degradation (never a crash).
 *
 *   2. Authorize via ``pf_acquire(session, "imu")`` — same shape as C2's input path.
 *
 *   3. Read the CHIP-frame raws out of the IIO sysfs tree at ``PF_IIO_ROOT`` (default
 *      ``/sys/bus/iio/devices``, indistinguishable from a real qmi8658 — sim binds
 *      the E5 ``iio_synth.IIOSynth``-materialized tree there). Multiply the raws by
 *      ``in_accel_scale`` / ``in_anglvel_scale`` to get chip-frame SI; apply the
 *      DESCRIPTOR's ``mount_matrix`` (device = M . chip) to reach the DEVICE frame.
 *      This is the whole point of ``in_*_mount_matrix`` — the driver reports chip-
 *      frame, userspace transforms.
 *
 *   4. On the sim's ``imu <name>`` FIFO verb, reply with the device-frame accel+gyro
 *      in milli-SI integers ("imu <name> ax_mm/s2 ay_mm/s2 az_mm/s2 gx_mrad/s
 *      gy_mrad/s gz_mrad/s") — the shape ``control_surface.Device.read_imu`` parses
 *      byte-for-byte (see the pinned sim rev in pins.env). No IMU row => "imu-absent".
 *
 *   5. On snap, render the tilt-bubble widget: a circular level with a dot displaced
 *      by (device-frame ax, ay). The rect for the bubble comes from
 *      ``[skin.parts].<sensor.id>`` — a descriptor with no such row simply never
 *      draws it. The synthetic test descriptor carries one; NEITHER shipping
 *      descriptor does => omission on both, zero source change vs the synthetic run.
 *
 * DEVICE-FREE. The whole path is proven under the E5 sim's virtual IIO. The on-panel
 * proof + real-silicon EVIOCGABS ground truth ride the C8 hardware gate (owner-return).
 */
#ifndef PF_HWPROBE_SENSOR_IMU_H
#define PF_HWPROBE_SENSOR_IMU_H

#include <SDL3/SDL.h>
#include <pocketforge.h>

#include <stddef.h>

/* Parallel bound to main.c's MAX_NAME — kept in sync by hand (small, stable). */
#define PF_IMU_MAX_NAME 40
#define PF_IMU_MAX_PATH 512

struct sensor_imu {
    /* -- descriptor-derived (parse_from_descriptor) --------------------------- */
    int  present;                       /* an accel or gyro row exists */
    int  has_accel;                     /* row.kind contains "accel" or "imu" */
    int  has_gyro;                      /* row.kind contains "gyro" or "imu" */
    char id[PF_IMU_MAX_NAME];           /* row.id — e.g. "imu" */
    char kind[PF_IMU_MAX_NAME];         /* row.kind — e.g. "accel+gyro" */
    char iio_device[PF_IMU_MAX_NAME];   /* row.iio_device (name attr) — e.g. "qmi8658" */
    char ui[PF_IMU_MAX_NAME];           /* row.ui — e.g. "tilt_bubble" (drives render) */
    double mount[3][3];                 /* row.mount_matrix (identity if omitted) */
    int  have_mount;                    /* 1 iff mount_matrix was in the descriptor */

    /* -- E2 facade authorization (acquire_from_facade) ------------------------ */
    int  authorized;                    /* pf_acquire(session, "imu") == PF_OK */

    /* -- skin_part rect for the tilt-bubble (resolve_skin_rect) --------------- */
    int  skin_x, skin_y, skin_w, skin_h;  /* skin-space rect ([skin.parts].<id>) */
    int  canvas_x, canvas_y, canvas_w, canvas_h;  /* canvas-space rect (after fit) */
    int  have_canvas_rect;              /* set by resolve_skin_rect */

    /* -- last IIO read (refresh_from_iio) ------------------------------------- */
    double last_accel_dev[3];           /* device-frame m/s^2 */
    double last_gyro_dev[3];            /* device-frame rad/s */
    int    last_read_ok;                /* 1 iff the read chain succeeded */
    double accel_scale;                 /* metres/s^2 per LSB (from in_accel_scale) */
    double gyro_scale;                  /* rad/s per LSB (from in_anglvel_scale) */
    int    have_accel_scale;
    int    have_gyro_scale;
};

/* Zero the struct + set mount_matrix to identity. Idempotent. */
void sensor_imu_init(struct sensor_imu *imu);

/* Parse the descriptor at ``toml_path`` for the first ``[[sensors]]`` row whose kind
 * names an accel or gyro. On no match, ``imu->present == 0`` (the omission path).
 * A missing/unreadable descriptor leaves ``imu`` in its initial state and is not fatal
 * — the caller has already logged the missing descriptor via connect_runtime(). */
void sensor_imu_parse_from_descriptor(struct sensor_imu *imu, const char *toml_path);

/* Route the acquire decision through the E2 facade — same shape as acquire_input().
 * A NULL session (no facade) leaves ``authorized`` at 0; the FIFO handler treats that
 * as a graceful "no authorization; report absent" so descriptor-only runs still boot. */
void sensor_imu_acquire_from_facade(struct sensor_imu *imu, PfSession *session);

/* Resolve the sensor's canvas rect: look up ``[skin.parts].<imu->id>`` from the parsed
 * skin_parts table, then apply the same fit_s/fit_ox/fit_oy the body uses. When there
 * is no matching skin_part (both shipping descriptors), this is a no-op and the render
 * function draws nothing — the honest omission (no fabricated widget). ``fit_s``/etc.
 * come from the app's main() context; the caller passes them in explicitly so this TU
 * has no coupling to main.c's ``struct app``.
 *
 * ``skin_parts`` / ``n_skin_parts`` describe the parsed [skin.parts] table — one flat
 * name+rect entry per part. */
struct pf_imu_skin_part_view {
    const char *name;
    int x, y, w, h;
};

void sensor_imu_resolve_skin_rect(struct sensor_imu *imu,
                                  const struct pf_imu_skin_part_view *parts,
                                  int n_parts,
                                  double fit_s, double fit_ox, double fit_oy);

/* Read the IIO sysfs tree (default root ``/sys/bus/iio/devices``, overridable via
 * ``PF_IIO_ROOT`` env — that is the sim's "native launcher" hook). Reads
 * ``iio:device0/name`` to verify the device matches the descriptor's iio_device (soft
 * match: warn on mismatch, never fail), the ``in_*_scale`` factors, and the
 * ``in_accel_[xyz]_raw`` / ``in_anglvel_[xyz]_raw`` sample. On success, populates
 * ``last_accel_dev`` / ``last_gyro_dev`` (device-frame SI) by applying the descriptor's
 * mount_matrix to the chip-frame reading. Returns ``last_read_ok`` (1 on success). */
int sensor_imu_refresh_from_iio(struct sensor_imu *imu);

/* Handle the ``imu <name>`` FIFO verb from the E5 sim control surface. Refreshes
 * IIO first, then writes the reply to ``resp_fd``:
 *   * NO descriptor row / not authorized / read failed  ->  "imu-absent"
 *   * success                                            ->  "imu <name> ax ay az gx gy gz"
 *     where the six trailing values are milli-SI integers (mm/s^2, mrad/s) — the exact
 *     shape ``control_surface.Device.read_imu`` parses. */
void sensor_imu_reply_verb(struct sensor_imu *imu, int resp_fd);

/* Render the tilt-bubble on ``r`` at the resolved canvas rect. No-op when
 * ``imu->present == 0``, ``imu->have_canvas_rect == 0``, or the last read failed —
 * that is the honest omission path (both shipping descriptors today). Refreshes the
 * IIO tree first so the bubble tracks the pose the sim just injected. */
void sensor_imu_render(SDL_Renderer *r, struct sensor_imu *imu);

#endif /* PF_HWPROBE_SENSOR_IMU_H */
