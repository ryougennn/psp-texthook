/*
 * Only the JPEG writer of stb_image_write: everything is static, so the
 * other writers (and their malloc/libm calls) are dropped. Ours, not part
 * of the vendored code.
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x) ((void)0)
#include "stb_image_write.h"
#include "stb_jpeg.h"

int stb_jpeg_write(void (*write)(void *ctx, void *data, int size), void *ctx,
                   int w, int h, int comp, const void *pixels, int quality)
{
    return stbi_write_jpg_to_func(write, ctx, w, h, comp, pixels, quality);
}
