/*
 * Glyphs from the PSP's own Japanese font, flash0:/font/jpn0.pgf (FTT
 * NewRodin Pro DB, 4 bpp antialiased, about 19x20 px), for the popup.
 *
 * Read directly rather than through sceLibFont: no module to load into a
 * game that may already have its own copy, and no NIDs or structures to
 * get exactly right. The format is the one PPSSPP and JPCSP document:
 * a header, bit-packed tables (code point -> glyph index -> glyph offset),
 * then the glyphs, each a bit-packed header and nibble RLE pixels.
 *
 * The tables (146 KB for jpn0) and a cache of decoded glyphs share one
 * block in the volatile memory, held only while the popup is open; glyphs
 * are read from flash0 as they are first drawn.
 *
 * Popup thread only.
 */

#include <pspkernel.h>
#include <pspsysmem.h>

#include "popup.h"
#include "mini.h"

#define PGF_HEADER_SIZE 392
#define GLYPH_READ      512         /* bytes read per glyph: header and pixels */
#define CACHE_SLOTS     256
#define CACHE_HASH      512         /* code point -> slot, open addressing */

/* Glyph header flags */
#define BMP_H_ROWS      1
#define BMP_V_ROWS      2
#define BMP_DIRECTION   3
#define DIM_INDEX       0x04
#define XADJ_INDEX      0x08
#define YADJ_INDEX      0x10
#define ADV_INDEX       0x20

static SceUID   fd = -1, block = -1;
static const u8 *charmap, *charptr;
static const u32 *adv_table;
static int      map_len, map_bpe, ptr_len, ptr_bpe, first_glyph, adv_len;
static int      max_w, max_h, ascent;
static u32      data_off;

/* The cache: slots of max_w * max_h pixels, and a hash of code points. */
static th_glyph *slots;
static u8       *pixels;
static u16      hash[CACHE_HASH];   /* slot + 1; 0 = empty */
static int      used;

/* --- bits ---------------------------------------------------------------- */

/* PGF tables are little-endian bit streams, least significant bit first. */
static u32 bits(const u8 *buf, u32 pos, int n)
{
    u32 v = 0;

    for (int i = 0; i < n; i++, pos++)
        v |= (u32)((buf[pos >> 3] >> (pos & 7)) & 1) << i;
    return v;
}

static int sbits7(const u8 *buf, u32 *pos)
{
    int v = bits(buf, *pos, 7);

    *pos += 7;
    return v >= 64 ? v - 128 : v;
}

static u32 le32(const u8 *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

/* --- open and close ------------------------------------------------------ */

int th_pgf_open(const char *path)
{
    static u8 h[PGF_HEADER_SIZE];

    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        th_log("font: can't open %s (%08x)", path, fd);
        return fd;
    }
    if (sceIoRead(fd, h, sizeof(h)) != sizeof(h) || memcmp(h + 4, "PGF0", 4) ||
        le32(h + 8) != 2) {
        th_log("font: %s isn't a revision 2 PGF font", path);
        th_pgf_close();
        return -1;
    }

    map_len     = le32(h + 16);
    ptr_len     = le32(h + 20);
    map_bpe     = le32(h + 24);
    ptr_bpe     = le32(h + 28);
    first_glyph = h[182] | h[183] << 8;
    ascent      = (int)le32(h + 212) / 64;
    max_w       = h[252] | h[253] << 8;
    max_h       = h[254] | h[255] << 8;

    int dim = h[258], xadj = h[259], yadj = h[260];
    int shadow_len = le32(h + 364), shadow_bpe = le32(h + 368);

    adv_len = h[261];
    if (map_bpe < 1 || map_bpe > 24 || ptr_bpe < 1 || ptr_bpe > 24 || max_w < 1 ||
        max_w > 64 || max_h < 1 || max_h > 64) {
        th_log("font: unexpected table sizes in %s", path);
        th_pgf_close();
        return -1;
    }

    /* The tables: dimension, x and y adjust, advance; shadow map; char map; char pointers. */
    u32 adv_off = PGF_HEADER_SIZE + (dim + xadj + yadj) * 8;
    u32 map_off = adv_off + adv_len * 8 + (((u32)shadow_len * shadow_bpe + 31) & ~31u) / 8;
    u32 ptr_off = map_off + (((u32)map_len * map_bpe + 31) & ~31u) / 8;

    data_off = ptr_off + (((u32)ptr_len * ptr_bpe + 31) & ~31u) / 8;

    u32 table_size = (data_off + 3) & ~3u;
    u32 slot_size  = (max_w * max_h + 3) & ~3u;
    u32 size       = table_size + CACHE_SLOTS * (sizeof(th_glyph) + slot_size);

    block = sceKernelAllocPartitionMemory(TH_PARTITION, "thk_font", PSP_SMEM_High, size, NULL);
    if (block < 0) {
        th_log("font: no memory for %lu bytes (%08x)", size, block);
        th_pgf_close();
        return -1;
    }

    u8 *base = sceKernelGetBlockHeadAddr(block);
    int n;

    if (sceIoLseek32(fd, 0, PSP_SEEK_SET) != 0 ||
        (n = sceIoRead(fd, base, data_off)) != (int)data_off) {
        th_log("font: reading the tables of %s failed", path);
        th_pgf_close();
        return -1;
    }

    adv_table = (const u32 *)(base + adv_off);
    charmap   = base + map_off;
    charptr   = base + ptr_off;
    slots     = (th_glyph *)(base + table_size);
    pixels    = (u8 *)(slots + CACHE_SLOTS);
    memset(hash, 0, sizeof(hash));
    used = 0;
    return 0;
}

/* Partition blocks outlive the module: always free them. */
void th_pgf_close(void)
{
    if (block >= 0)
        sceKernelFreePartitionMemory(block);
    if (fd >= 0)
        sceIoClose(fd);
    block = -1;
    fd    = -1;
    slots = NULL;
}

int th_pgf_ascent(void)
{
    return ascent;
}

/* --- glyphs -------------------------------------------------------------- */

/* Reads and decodes one glyph into g (pixels already pointing at its slot). */
static int load(u32 cp, th_glyph *g)
{
    static u8 buf[GLYPH_READ];

    if (cp < (u32)first_glyph || cp - first_glyph >= (u32)map_len)
        return 0;

    u32 index = bits(charmap, (cp - first_glyph) * map_bpe, map_bpe);
    if (index >= (u32)ptr_len)
        return 0;

    u32 off = data_off + bits(charptr, index * ptr_bpe, ptr_bpe) * 4;
    int n   = -1;

    if (sceIoLseek32(fd, off, PSP_SEEK_SET) == (int)off)
        n = sceIoRead(fd, buf, sizeof(buf));
    if (n < 16)
        return 0;

    u32 pos = 14;           /* skip the record size */

    g->w    = bits(buf, pos, 7), pos += 7;
    g->h    = bits(buf, pos, 7), pos += 7;
    g->left = sbits7(buf, &pos);
    g->top  = sbits7(buf, &pos);

    int flags = bits(buf, pos, 6);

    pos += 6 + 2 + 2 + 3 + 9;                       /* flags, shadow flags and id */
    pos += flags & DIM_INDEX  ? 8 : 64;
    pos += flags & XADJ_INDEX ? 8 : 64;
    pos += flags & YADJ_INDEX ? 8 : 64;
    if (flags & ADV_INDEX) {
        int a = bits(buf, pos, 8);

        g->advance = a < adv_len ? (int)adv_table[2 * a] : 0;
        pos += 8;
    } else {
        g->advance = bits(buf, pos, 32);
        pos += 64;
    }

    int dir = flags & BMP_DIRECTION;

    if (g->w > max_w || g->h > max_h || (dir != BMP_H_ROWS && dir != BMP_V_ROWS))
        g->w = g->h = 0;

    /* Nibble RLE: < 8 = repeat the next nibble n + 1 times, else 16 - n literal nibbles. */
    int total = g->w * g->h, i = 0;
    u32 end   = (u32)n * 8;

    while (i < total && pos + 4 <= end) {
        int code  = bits(buf, pos, 4);
        int count = code < 8 ? code + 1 : 16 - code;
        int value = 0;

        pos += 4;
        if (code < 8)
            value = bits(buf, pos, 4), pos += 4;
        for (int k = 0; k < count && i < total && pos <= end; k++, i++) {
            if (code >= 8)
                value = bits(buf, pos, 4), pos += 4;
            if (dir == BMP_H_ROWS)
                g->alpha[i] = value;
            else
                g->alpha[(i % g->h) * g->w + i / g->h] = value;
        }
    }
    for (; i < total; i++)
        g->alpha[dir == BMP_H_ROWS ? i : (i % g->h) * g->w + i / g->h] = 0;
    return 1;
}

const th_glyph *th_pgf_glyph(u32 cp)
{
    if (!slots)
        return NULL;

    u32 h = (cp * 2654435761u) >> 23;               /* 9 bits: CACHE_HASH */

    for (;; h = (h + 1) & (CACHE_HASH - 1)) {
        if (!hash[h])
            break;
        if (slots[hash[h] - 1].cp == cp)
            return slots[hash[h] - 1].w || slots[hash[h] - 1].advance ? &slots[hash[h] - 1]
                                                                      : NULL;
    }

    /* Full: start over. A screen needs far fewer than CACHE_SLOTS glyphs. */
    if (used == CACHE_SLOTS) {
        memset(hash, 0, sizeof(hash));
        used = 0;
        h    = (cp * 2654435761u) >> 23;
    }

    th_glyph *g = &slots[used];

    memset(g, 0, sizeof(*g));
    g->cp    = cp;
    g->alpha = pixels + used * ((max_w * max_h + 3) & ~3);
    hash[h]  = ++used;

    if (!load(cp, g)) {
        g->w = g->h = g->advance = 0;
        return NULL;
    }
    return g;
}
