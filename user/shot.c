/*
 * Screenshots: one whole frame, copied at once, then sent as JPEG (or BMP).
 *
 * The buffers live in the volatile memory with the rest of the module and
 * stay allocated once used, until the module stops: head room for the HTTP
 * and BMP headers, room for a full 512 x 272 frame at 32 bpp (544 KB), then
 * head room and room for the JPEG (256 KB). Right after a vblank, one DMA copies whichever
 * buffer is on screen (about 5 ms); the rows are then packed to 480 px in
 * place. The picture is a single frame, and the overlay is only hidden for
 * that frame.
 *
 * JPEG (the default): a 480x272 frame comes to tens of KB instead of
 * 261 KB, and over the PSP's Wi-Fi (20-400 KB/s, varying a lot) fewer bytes
 * is what matters. Encoding takes a while, so it runs on its own thread
 * below the worker's priority; the worker keeps serving text and sends the
 * result, with an exact Content-Length, when it's done.
 *
 * BMP (?format=bmp, /screenshot.bmp): top-down with BI_BITFIELDS masks
 * describing the pixels as sent, so 16-bit games need no per-pixel work.
 * 8888 is converted to 565 unless ?full=1.
 *
 * Sending happens from th_http_poll() (shot_poll) as the socket allows, so
 * text keeps flowing.
 */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdmac.h>
#include <pspsysmem.h>
#include <pspnet_inet.h>

#include "http.h"
#include "mini.h"
#include "stb_jpeg.h"

#define SHOT_W          480
#define SHOT_H          272
#define SHOT_MAX_STRIDE 512
#define SHOT_HEAD_ROOM  512     /* HTTP + BMP headers, ahead of the pixels */
#define SHOT_FRAME      (SHOT_MAX_STRIDE * SHOT_H * 4)

#define JPEG_HEAD_ROOM  256     /* HTTP header, ahead of the JPEG */
#define JPEG_MAX        (256 * 1024)
#define JPEG_QUALITY    90

#define TX_STALL_LIMIT  (20 * SEC)      /* give up after this long without progress */

enum { CAPTURE_BMP, CAPTURE_BMP_FULL, CAPTURE_RGBA };

static SceUID shot_block;
static u8     *shot_buf;        /* head room + frame */
static u8     *jpeg_buf;        /* head room + JPEG */

/* The encoder thread's job. Written by it until `done`, then by the worker. */
static struct {
    SceUID       thread;        /* > 0 while it exists */
    int          quality, len, overflow, ok;
    u32          ms;
    volatile int done;
} enc;

int shot_busy(void)
{
    if (enc.thread > 0)
        return 1;

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].tx_shot)
            return 1;

    return 0;
}

/* --- copying the frame --------------------------------------------------- */

/* 16-bit pixel -> 8888 (R in the low byte), for the encoder. */
static u32 expand16(u32 p, int fmt)
{
    u32 r, g, b;

    switch (fmt) {
    case PSP_DISPLAY_PIXEL_FORMAT_565:
        r = p & 0x1F,             g = (p >> 5) & 0x3F,       b = (p >> 11) & 0x1F;
        r = (r << 3) | (r >> 2),  g = (g << 2) | (g >> 4),   b = (b << 3) | (b >> 2);
        break;
    case PSP_DISPLAY_PIXEL_FORMAT_5551:
        r = p & 0x1F,             g = (p >> 5) & 0x1F,       b = (p >> 10) & 0x1F;
        r = (r << 3) | (r >> 2),  g = (g << 3) | (g >> 2),   b = (b << 3) | (b >> 2);
        break;
    default:    /* 4444 */
        r = (p & 0xF) * 17,       g = ((p >> 4) & 0xF) * 17, b = ((p >> 8) & 0xF) * 17;
        break;
    }

    return r | (g << 8) | (b << 16);
}

/*
 * Copies the frame on screen into `pixels` and packs it to SHOT_W, as
 * `mode` asks: CAPTURE_BMP converts 8888 to 565, CAPTURE_BMP_FULL keeps
 * the game's format, CAPTURE_RGBA makes everything 8888. Returns the bytes
 * per pixel of the result (0 on failure) and the game's format.
 */
static int capture_frame(u8 *pixels, int mode, int *fmt_out)
{
    void *top;
    int  stride, fmt;

    /* Keep the overlay out of the picture: it's drawn into the frame. */
    int hidden = thkOverlayHide(1);
    for (int i = 0; i < (hidden ? 3 : 1); i++)
        sceDisplayWaitVblankStart();    /* the displayed buffer just became stable */

    int r = sceDisplayGetFrameBuf(&top, &stride, &fmt, 0);
    if (r < 0 || !top || stride < SHOT_W || stride > SHOT_MAX_STRIDE || fmt < 0 || fmt > 3) {
        thkOverlayHide(0);
        th_log("screenshot: unusable display mode (%08x, buffer %p, stride %d, format %d)",
               r, top, stride, fmt);
        return 0;
    }

    /* Copy the whole frame at once. */
    int bpp = fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
    int raw = SHOT_H * stride * bpp;
    const u8 *src = (const u8 *)((u32)top & 0x0FFFFFFF);

    sceKernelDcacheWritebackInvalidateRange(pixels, raw);
    if (sceDmacMemcpy(pixels, src, raw) < 0) {
        /* CPU fallback: whole words through the uncached VRAM mirror. */
        const u32 *s32 = (const u32 *)((u32)src | 0x40000000);

        for (int i = 0; i < raw / 4; i++)
            ((u32 *)pixels)[i] = s32[i];
    }
    thkOverlayHide(0);
    *fmt_out = fmt;

    /*
     * Pack rows to SHOT_W pixels (and 8888 -> 565). Writes never overtake
     * reads, so this works in place.
     */
    int to565   = bpp == 4 && mode == CAPTURE_BMP;
    int out_bpp = to565 ? 2 : bpp;

    for (int y = 0; y < SHOT_H; y++) {
        const u8 *in  = pixels + y * stride * bpp;
        u8       *out = pixels + y * SHOT_W * out_bpp;

        if (to565) {
            const u32 *s32 = (const u32 *)in;
            u16 *d16 = (u16 *)out;

            for (int x = 0; x < SHOT_W; x++) {
                u32 p = s32[x];

                d16[x] = ((p >> 3) & 0x1F) | (((p >> 10) & 0x3F) << 5) | (((p >> 19) & 0x1F) << 11);
            }
        } else if (out != in) {
            memmove(out, in, SHOT_W * bpp);
        }
    }

    if (mode == CAPTURE_RGBA && bpp == 2) {
        /* 16 -> 32 bits in place, from the end: writes stay ahead of reads. */
        const u16 *s16 = (const u16 *)pixels;
        u32 *d32 = (u32 *)pixels;

        for (int i = SHOT_W * SHOT_H - 1; i >= 0; i--)
            d32[i] = expand16(s16[i], fmt);
        out_bpp = 4;
    }

    return out_bpp;
}

static int alloc_shot_buffers(void)
{
    if (shot_buf)
        return 1;

    int size = SHOT_HEAD_ROOM + SHOT_FRAME + JPEG_HEAD_ROOM + JPEG_MAX;

    shot_block = sceKernelAllocPartitionMemory(TH_PARTITION, "thk_screenshot", PSP_SMEM_High,
                                               size, NULL);
    if (shot_block < 0) {
        u32 vol[2] = { 0, 0 };

        thkMemInfo(TH_PARTITION, vol);
        th_log("screenshot: need %d bytes, largest free block %lu (%08x)", size, vol[1],
               shot_block);
        return 0;
    }

    shot_buf = sceKernelGetBlockHeadAddr(shot_block);
    jpeg_buf = shot_buf + SHOT_HEAD_ROOM + SHOT_FRAME;
    return 1;
}

/* --- the JPEG encoder ---------------------------------------------------- */

static void jpeg_out(void *ctx, void *data, int size)
{
    if (enc.len + size > JPEG_MAX) {
        enc.overflow = 1;
        return;
    }
    memcpy(jpeg_buf + JPEG_HEAD_ROOM + enc.len, data, size);
    enc.len += size;
}

static int encoder(SceSize args, void *argp)
{
    u32 t = now();

    enc.ok = stb_jpeg_write(jpeg_out, NULL, SHOT_W, SHOT_H, 4, shot_buf + SHOT_HEAD_ROOM,
                            enc.quality) && !enc.overflow;
    enc.ms   = (now() - t) / 1000;
    enc.done = 1;
    return 0;
}

/*
 * Waits for the encoder (it reads and writes the buffers) and deletes it.
 * Called once it's done, and before the module goes away.
 */
static void reap_encoder(void)
{
    if (enc.thread <= 0)
        return;

    SceUInt timeout = 2 * 1000 * 1000;
    if (sceKernelWaitThreadEnd(enc.thread, &timeout) < 0) {
        th_log("screenshot: encoder didn't finish; stopping it");
        sceKernelTerminateDeleteThread(enc.thread);
    } else {
        sceKernelDeleteThread(enc.thread);
    }
    enc.thread = 0;
}

/* --- sending ------------------------------------------------------------- */

static void put_le(u8 *p, u32 v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = v >> (8 * i);
}

static void start_sending(client *c, u8 *body, int body_len, const char *type)
{
    char http[256];

    int hl = mini_snprintf(http, sizeof(http),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n", type, body_len);
    u8 *head = body - hl;

    memcpy(head, http, hl);

    c->tx_data  = head;
    c->tx_len   = c->tx_total    = hl + body_len;
    c->tx_off   = c->tx_sent     = 0;
    c->tx_start = c->tx_progress = now();
}

/* The encoder is done: send the JPEG to whoever asked, if still there. */
static void finish_jpeg(void)
{
    reap_encoder();

    client *c = NULL;
    for (int i = 0; i < MAX_CLIENTS && !c; i++)
        if (clients[i].fd >= 0 && clients[i].tx_encoding)
            c = &clients[i];

    if (!enc.ok)
        th_log("screenshot: JPEG encoding failed%s", enc.overflow ? " (over 256 KB)" : "");
    if (!c)
        return;

    c->tx_encoding = 0;
    if (!enc.ok) {
        send_error(c->fd, "503 Service Unavailable");
        close_client(c);
        return;
    }

    start_sending(c, jpeg_buf + JPEG_HEAD_ROOM, enc.len, "image/jpeg");
    th_log("screenshot: JPEG quality %d, %d bytes, encoded in %lu ms", enc.quality, enc.len,
           enc.ms);
}

void shot_start(client *c, const char *query, int bmp)
{
    static const u32 masks[4][3] = {
        [PSP_DISPLAY_PIXEL_FORMAT_565]  = { 0x001F,     0x07E0,     0xF800     },
        [PSP_DISPLAY_PIXEL_FORMAT_5551] = { 0x001F,     0x03E0,     0x7C00     },
        [PSP_DISPLAY_PIXEL_FORMAT_4444] = { 0x000F,     0x00F0,     0x0F00     },
        [PSP_DISPLAY_PIXEL_FORMAT_8888] = { 0x000000FF, 0x0000FF00, 0x00FF0000 },
    };

    if (query && strstr(query, "format=bmp"))
        bmp = 1;

    if (shot_busy() || !alloc_shot_buffers()) {
        send_error(c->fd, "503 Service Unavailable");
        close_client(c);
        return;
    }

    u8  *pixels = shot_buf + SHOT_HEAD_ROOM;
    int full    = query && strstr(query, "full=1");
    int fmt;
    int out_bpp = capture_frame(pixels, !bmp ? CAPTURE_RGBA : full ? CAPTURE_BMP_FULL : CAPTURE_BMP,
                                &fmt);
    if (!out_bpp) {
        send_error(c->fd, "503 Service Unavailable");
        close_client(c);
        return;
    }

    if (!bmp) {
        const char *q = query ? strstr(query, "q=") : NULL;
        int quality   = q ? atoi_simple(q + 2) : JPEG_QUALITY;

        memset(&enc, 0, sizeof(enc));
        enc.quality = quality >= 1 && quality <= 100 ? quality : JPEG_QUALITY;

        SceKernelThreadOptParam opt;

        memset(&opt, 0, sizeof(opt));
        opt.size      = sizeof(opt);
        opt.stackMpid = TH_PARTITION;

        /* Below the worker (0x60), so text and sending come first. */
        enc.thread = sceKernelCreateThread("texthook_jpeg", encoder, 0x70, 0x4000, 0, &opt);
        if (enc.thread < 0 || sceKernelStartThread(enc.thread, 0, NULL) < 0) {
            th_log("screenshot: encoder thread failed (%08x)", enc.thread);
            if (enc.thread > 0)
                sceKernelDeleteThread(enc.thread);
            enc.thread = 0;
            send_error(c->fd, "503 Service Unavailable");
            close_client(c);
            return;
        }
        c->tx_shot = 1;
        c->tx_encoding = 1;
        broadcast_screenshot("{\"state\":\"start\"}");
        return;
    }

    int out_fmt = out_bpp == 2 && fmt == PSP_DISPLAY_PIXEL_FORMAT_8888
                ? PSP_DISPLAY_PIXEL_FORMAT_565 : fmt;
    int size    = SHOT_W * SHOT_H * out_bpp;

    /* BMP header just before the pixels; the HTTP header goes before it. */
    enum { BMP_HDR = 14 + 40 + 12 };
    u8 *bmp_hdr = pixels - BMP_HDR;

    memset(bmp_hdr, 0, BMP_HDR);
    bmp_hdr[0] = 'B', bmp_hdr[1] = 'M';
    put_le(bmp_hdr +  2, BMP_HDR + size, 4);    /* file size */
    put_le(bmp_hdr + 10, BMP_HDR, 4);           /* pixel data offset */
    put_le(bmp_hdr + 14, 40, 4);                /* BITMAPINFOHEADER */
    put_le(bmp_hdr + 18, SHOT_W, 4);            /* width */
    put_le(bmp_hdr + 22, (u32)-SHOT_H, 4);      /* negative height: top-down rows */
    put_le(bmp_hdr + 26, 1, 2);                 /* planes */
    put_le(bmp_hdr + 28, out_bpp * 8, 2);       /* bits per pixel */
    put_le(bmp_hdr + 30, 3, 4);                 /* BI_BITFIELDS */
    put_le(bmp_hdr + 34, size, 4);              /* image size */
    for (int i = 0; i < 3; i++)
        put_le(bmp_hdr + 54 + 4 * i, masks[out_fmt][i], 4);   /* channel masks */

    c->tx_shot = 1;
    start_sending(c, bmp_hdr, BMP_HDR + size, "image/bmp");
    broadcast_screenshot("{\"state\":\"start\"}");
    th_log("screenshot: BMP, format %d -> %d bpp, %d bytes", fmt, out_bpp * 8, c->tx_total);
}

/* Sends what the socket will take right now. */
static void pump_tx(client *c)
{
    while (c->tx_off < c->tx_len) {
        int n = (int)sceNetInetSend(c->fd, c->tx_data + c->tx_off, c->tx_len - c->tx_off, 0);

        if (n > 0) {
            c->tx_off     += n;
            c->tx_sent    += n;
            c->tx_progress = now();
            continue;
        }
        if (n < 0 && would_block()) {
            if ((u32)(now() - c->tx_progress) > TX_STALL_LIMIT) {
                th_log("screenshot: aborted, stalled for 20 s at %d/%d bytes",
                       c->tx_sent, c->tx_total);
                close_client(c);
            }
            return;
        }
        th_log("screenshot: aborted at %d/%d bytes (errno %d)",
               c->tx_sent, c->tx_total, sceNetInetGetErrno());
        close_client(c);
        return;
    }

    u32 ms = (now() - c->tx_start) / 1000;
    th_log("screenshot: sent %d bytes in %lu ms (%lu KB/s)",
           c->tx_total, ms, ms ? (u32)c->tx_total / ms : 0);
    close_client(c);
}

/* --- the worker's side --------------------------------------------------- */

void shot_poll(void)
{
    if (enc.thread > 0 && enc.done)
        finish_jpeg();
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].tx_shot && !clients[i].tx_encoding)
            pump_tx(&clients[i]);
}

/*
 * The module is stopping: the encoder's code and the buffers go with it.
 * Partition blocks outlive the module: kept, each stop leaked 800 KB of
 * the volatile memory, until screenshots failed (Amagami, 2026-10-09).
 */
void th_shot_free(void)
{
    reap_encoder();
    if (shot_buf) {
        sceKernelFreePartitionMemory(shot_block);
        shot_buf = jpeg_buf = NULL;
    }
}
