/*
 * On-screen overlay: a short ASCII message (and optionally a QR code)
 * drawn into each frame just before it is shown.
 *
 * The game's sceDisplaySetFrameBuf syscall is redirected here. At that
 * point the frame is finished but not yet displayed, so drawing into it
 * shows up without flicker. When no message is active the cost is one
 * comparison per frame.
 *
 * The box is opaque rather than a dimmed copy of what's behind it: a game
 * that re-flips a buffer without redrawing it would otherwise get darker
 * every frame.
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <pspdisplay.h>
#include <pspintrman.h>
#include <systemctrl.h>

#include "core.h"
#include "font8x8.h"
#include "mini.h"
#include "qrcodegen.h"

#define SCREEN_W        480
#define SCREEN_H        272

#define TEXT_SCALE      1
#define MARGIN          4
#define PAD             3

#define QR_SCALE        4
#define QR_QUIET        4       /* modules of white border required around a QR code */
#define QR_MAX_VERSION  4

#define NID_sceDisplaySetFrameBuf 0x289D82FE

static int (*orig_set_frame_buf)(void *top, int width, int fmt, int sync);

static char text[THK_OVERLAY_MAX_TEXT];
static u32  until;              /* sceKernelGetSystemTimeLow() deadline; 0 = nothing to show */
static int  hidden, enabled = 1, corner = THK_CORNER_TOP_LEFT;

/* The game's last frame, and the popup's frame shown instead of it (popup.c). */
static void *game_top, *popup_fb;
static int  game_width, game_fmt;

static u8   qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
static u8   qr_temp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
static int  qr_size;            /* modules per side, 0 = no QR code */

/* --- pixel helpers ------------------------------------------------------- */

typedef struct {
    u8  *base;                  /* uncached framebuffer address */
    int stride, bpp;
    u32 white, black, panel;
} surface;

static void fill(const surface *s, int x, int y, int w, int h, u32 color)
{
    if (x < 0)
        w += x, x = 0;
    if (y < 0)
        h += y, y = 0;
    if (x + w > SCREEN_W)
        w = SCREEN_W - x;
    if (y + h > SCREEN_H)
        h = SCREEN_H - y;

    for (int j = 0; j < h; j++) {
        u8 *row = s->base + ((y + j) * s->stride + x) * s->bpp;

        if (s->bpp == 4)
            for (int i = 0; i < w; i++)
                ((u32 *)row)[i] = color;
        else
            for (int i = 0; i < w; i++)
                ((u16 *)row)[i] = color;
    }
}

static void draw_text(const surface *s, int x, int y)
{
    for (const char *p = text; *p; p++, x += 8 * TEXT_SCALE) {
        int c = (u8)*p;
        const u8 *glyph = font8x8[(c >= 32 && c < 127 ? c : '?') - 32];

        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (glyph[gy] & (0x80 >> gx))
                    fill(s, x + gx * TEXT_SCALE, y + gy * TEXT_SCALE,
                         TEXT_SCALE, TEXT_SCALE, s->white);
    }
}

static void draw_qr(const surface *s, int x, int y)
{
    int side = (qr_size + 2 * QR_QUIET) * QR_SCALE;

    fill(s, x, y, side, side, s->white);
    for (int my = 0; my < qr_size; my++)
        for (int mx = 0; mx < qr_size; mx++)
            if (qrcodegen_getModule(qr, mx, my))
                fill(s, x + (mx + QR_QUIET) * QR_SCALE, y + (my + QR_QUIET) * QR_SCALE,
                     QR_SCALE, QR_SCALE, s->black);
}

static int init_surface(surface *s, void *top, int width, int fmt)
{
    u32 a       = (u32)top & 0x0FFFFFFF;
    int in_vram = a >= 0x04000000 && a < 0x04200000;
    int in_ram  = user_range(a, 0);

    if (!top || (!in_vram && !in_ram) || width < SCREEN_W || fmt < 0 || fmt > 3)
        return 0;

    s->base   = (u8 *)(a | 0x40000000);    /* uncached: the display reads memory directly */
    s->stride = width;
    s->bpp    = fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;

    switch (fmt) {
    case PSP_DISPLAY_PIXEL_FORMAT_565:
        s->white = 0xFFFF,     s->black = 0x0000,     s->panel = 0x2104;
        break;
    case PSP_DISPLAY_PIXEL_FORMAT_5551:
        s->white = 0xFFFF,     s->black = 0x8000,     s->panel = 0x9084;
        break;
    case PSP_DISPLAY_PIXEL_FORMAT_4444:
        s->white = 0xFFFF,     s->black = 0xF000,     s->panel = 0xF222;
        break;
    default:
        s->white = 0xFFFFFFFF, s->black = 0xFF000000, s->panel = 0xFF202020;
        break;
    }
    return 1;
}

static void draw_overlay(void *top, int width, int fmt)
{
    surface s;

    if (!init_surface(&s, top, width, fmt))
        return;

    /* size */
    int text_w  = strlen(text) * 8 * TEXT_SCALE + 2 * PAD;
    int text_h  = 8 * TEXT_SCALE + 2 * PAD;
    int qr_side = qr_size ? (qr_size + 2 * QR_QUIET) * QR_SCALE : 0;
    int total_h = text_h + (qr_side ? MARGIN + qr_side : 0);

    /* position */
    int left = corner == THK_CORNER_TOP_LEFT || corner == THK_CORNER_BOTTOM_LEFT;
    int topc = corner == THK_CORNER_TOP_LEFT || corner == THK_CORNER_TOP_RIGHT;
    int y    = topc ? MARGIN : SCREEN_H - MARGIN - total_h;
    int tx   = left ? MARGIN : SCREEN_W - MARGIN - text_w;
    int qx   = left ? MARGIN : SCREEN_W - MARGIN - qr_side;

    fill(&s, tx, y, text_w, text_h, s.panel);
    draw_text(&s, tx + PAD, y + PAD);
    if (qr_side)
        draw_qr(&s, qx, y + text_h + MARGIN);
}

static int set_frame_buf_hook(void *top, int width, int fmt, int sync)
{
    if (top) {
        game_top   = top;
        game_width = width;
        game_fmt   = fmt;
    }
    if (popup_fb)
        return orig_set_frame_buf(popup_fb, THK_POPUP_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565, sync);

    if (until && !hidden && enabled) {
        if ((int)(until - sceKernelGetSystemTimeLow()) > 0)
            draw_overlay(top, width, fmt);
        else
            until = 0;
    }

    return orig_set_frame_buf(top, width, fmt, sync);
}

/* --- API ----------------------------------------------------------------- */

void overlay_init(void)
{
    u32 f = sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", NID_sceDisplaySetFrameBuf);

    if (!f) {
        klog("overlay: sceDisplaySetFrameBuf not found; overlay disabled");
        return;
    }
    orig_set_frame_buf = (void *)f;
    sctrlHENPatchSyscall((void *)f, set_frame_buf_hook);
    sctrlFlushCache();

    klog("overlay: hooked sceDisplaySetFrameBuf at %08lx", f);
}

/* `copy` is already ASCII-only and NUL-terminated (n characters). */
static void show(const char *copy, int n, int ms, int flags)
{
    int size = 0;

    if ((flags & THK_OVERLAY_QR) &&
        qrcodegen_encodeText(copy, qr_temp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                             QR_MAX_VERSION, qrcodegen_Mask_AUTO, true))
        size = qrcodegen_getSize(qr);

    int intr = sceKernelCpuSuspendIntr();
    memcpy(text, copy, n + 1);
    qr_size = size;
    until   = (sceKernelGetSystemTimeLow() + (u32)ms * 1000) | 1;
    sceKernelCpuResumeIntr(intr);
}

/* For the core's own messages (kernel strings). */
void overlay_message(const char *msg, int ms)
{
    char copy[THK_OVERLAY_MAX_TEXT];
    int  n = 0;

    for (; n < THK_OVERLAY_MAX_TEXT - 1 && msg[n]; n++)
        copy[n] = msg[n];
    copy[n] = 0;

    show(copy, n, ms, 0);
}

int thkOverlayShow(const char *msg, int ms, int flags)
{
    int  k1  = pspSdkSetK1(0);
    char copy[THK_OVERLAY_MAX_TEXT];
    int  ret = 0;

    if (!user_range((u32)msg, 1)) {
        ret = THK_ERR_ARGS;
        goto out;
    }

    int n = 0;
    for (; n < THK_OVERLAY_MAX_TEXT - 1 && user_range((u32)msg + n, 1) && msg[n]; n++)
        copy[n] = (u8)msg[n] < 127 && (u8)msg[n] >= 32 ? msg[n] : '?';
    copy[n] = 0;

    show(copy, n, ms, flags);

out:
    pspSdkSetK1(k1);
    return ret;
}

int thkOverlayConfig(int enable, int where)
{
    enabled = enable;

    if (where >= THK_CORNER_TOP_LEFT && where <= THK_CORNER_BOTTOM_RIGHT)
        corner = where;
    return 0;
}

int thkOverlayHide(int hide)
{
    int was_visible = until && !hidden && enabled;

    hidden = hide;
    return was_visible;
}

/*
 * Show the popup's frame (THK_POPUP_STRIDE, RGB565) instead of the game's,
 * or the game's again (NULL). Switched at once rather than at the game's
 * next frame: a visual novel waiting for a button may not send one.
 */
int overlay_popup(void *fb)
{
    int k1 = pspSdkSetK1(0);

    popup_fb = fb;
    if (fb)
        orig_set_frame_buf(fb, THK_POPUP_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565,
                           PSP_DISPLAY_SETBUF_NEXTFRAME);
    else if (game_top)
        orig_set_frame_buf(game_top, game_width, game_fmt, PSP_DISPLAY_SETBUF_NEXTFRAME);

    pspSdkSetK1(k1);
    return 0;
}

/* The frame the game last showed: address, buffer width, pixel format. */
int overlay_game_frame(u32 *out)
{
    if (!game_top)
        return THK_ERR_ARGS;
    out[0] = (u32)game_top;
    out[1] = game_width;
    out[2] = game_fmt;
    return THK_OK;
}
