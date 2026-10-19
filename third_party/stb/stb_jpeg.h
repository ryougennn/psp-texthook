/* JPEG writer from stb_image_write (see README.md). */

#ifndef STB_JPEG_H
#define STB_JPEG_H

/*
 * Encodes w x h pixels of `comp` bytes each (3 = RGB, 4 = RGBA, alpha
 * ignored), rows packed, top-down. Output goes to `write` in pieces.
 * quality 1..100. Returns 1 on success. Uses floats: don't call from hook
 * context.
 */
int stb_jpeg_write(void (*write)(void *ctx, void *data, int size), void *ctx,
                   int w, int h, int comp, const void *pixels, int quality);

#endif /* STB_JPEG_H */
