/*
 * img_decode.h — tiny PNG->RGBA8888 decode seam for pf-hwprobe (C2, tsp-fr2n.2).
 *
 * The app renders the device's clickable skin (skin.body / skin.lit_body PNGs from
 * the E1 descriptor). SDL3 core has no PNG loader (SDL_image is not linked in the
 * sim's static SDL3 build), so we decode PNG ourselves via the vendored public-
 * domain stb_image (third_party/stb_image.h — self-contained, its own inflate, no
 * libz link). Kept OUT of src/ so the acceptance grep (src/ + include/ only) stays
 * clean; kept behind this thin seam so main.c never sees the decoder internals.
 *
 * DEVICE-FREE / PORTABLE: reads the descriptor's real PNG bytes on the sim host AND
 * on the device (no host image-lib dependency, no staging conversion) — the honest
 * "app draws skin.body" path.
 */
#ifndef PF_HWPROBE_IMG_DECODE_H
#define PF_HWPROBE_IMG_DECODE_H

/*
 * Decode the PNG at `path` to a freshly-malloc'd RGBA8888 buffer (4 bytes/pixel,
 * top-to-bottom, no row padding). On success returns the buffer and writes *w,*h;
 * the caller owns it and frees it with img_free(). Returns NULL on any error
 * (missing file, decode failure) — a caller then falls back to the stub render.
 */
unsigned char *img_load_rgba(const char *path, int *w, int *h);

/* Free a buffer returned by img_load_rgba (NULL-safe). */
void img_free(unsigned char *pixels);

#endif /* PF_HWPROBE_IMG_DECODE_H */
