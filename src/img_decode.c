/*
 * img_decode.c — the PNG decode implementation TU for pf-hwprobe (C2, tsp-fr2n.2).
 *
 * Encapsulates the vendored stb_image so main.c stays decoder-free and grep-clean.
 * stb_image is public-domain and self-contained (its own zlib inflate) — no libz
 * link, no host image library, works identically on the sim host and the device.
 */
#include "img_decode.h"

#include <stdlib.h>

/* stb_image emits benign -Wpedantic/-Wextra noise (trailing enum commas, sign
 * conversions); silence it only for this TU so the app build log stays clean. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG            /* the only format our skins use */
#define STBI_NO_STDIO           /* we feed bytes ourselves (below) — no fopen in stb */
#include "stb_image.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdio.h>

unsigned char *img_load_rgba(const char *path, int *w, int *h)
{
    if (!path || !w || !h) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    unsigned char *raw = (unsigned char *)malloc((size_t)sz);
    if (!raw) { fclose(f); return NULL; }
    size_t got = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(raw); return NULL; }

    int n = 0;
    /* force 4 channels (RGBA) so the SDL surface stride is always w*4. */
    unsigned char *pixels = stbi_load_from_memory(raw, (int)sz, w, h, &n, 4);
    free(raw);
    return pixels;   /* NULL on decode failure */
}

void img_free(unsigned char *pixels)
{
    if (pixels) {
        stbi_image_free(pixels);
    }
}
