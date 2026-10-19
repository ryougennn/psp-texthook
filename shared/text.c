/* Converting captured text to UTF-8. */

#include <pspkernel.h>

#include "mini.h"
#include "shared.h"

/* --- UTF-8 and UTF-16 ---------------------------------------------------- */

static int put_utf8(char *out, int pos, int size, u32 cp)
{
    char tmp[4];
    int  n;

    if (cp < 0x80) {
        tmp[0] = cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = 0xC0 | (cp >> 6);
        tmp[1] = 0x80 | (cp & 0x3F);
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = 0xE0 | (cp >> 12);
        tmp[1] = 0x80 | ((cp >> 6) & 0x3F);
        tmp[2] = 0x80 | (cp & 0x3F);
        n = 3;
    } else {
        tmp[0] = 0xF0 | (cp >> 18);
        tmp[1] = 0x80 | ((cp >> 12) & 0x3F);
        tmp[2] = 0x80 | ((cp >> 6) & 0x3F);
        tmp[3] = 0x80 | (cp & 0x3F);
        n = 4;
    }
    if (pos + n >= size)
        return pos; /* out of room: drop the code point, keep valid UTF-8 */
    memcpy(out + pos, tmp, n);
    return pos + n;
}

static int utf16le_to_utf8(const u8 *src, int bytes, char *out, int size)
{
    int pos = 0;

    for (int i = 0; i + 1 < bytes; i += 2) {
        u32 cp = src[i] | (src[i + 1] << 8);

        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < bytes) {
            u32 lo = src[i + 2] | (src[i + 3] << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        pos = put_utf8(out, pos, size, cp);
    }
    out[pos] = 0;
    return pos;
}

/* Copies UTF-8 through, replacing malformed sequences with U+FFFD. */
static int utf8_sanitize(const u8 *src, int bytes, char *out, int size)
{
    int pos = 0;

    for (int i = 0; i < bytes;) {
        u8  b  = src[i];
        int n  = b < 0x80             ? 1
               : (b & 0xE0) == 0xC0   ? 2
               : (b & 0xF0) == 0xE0   ? 3
               : (b & 0xF8) == 0xF0   ? 4
               :                        0;
        int ok = n && i + n <= bytes;

        for (int k = 1; ok && k < n; k++)
            ok = (src[i + k] & 0xC0) == 0x80;
        if (!ok) {
            pos = put_utf8(out, pos, size, 0xFFFD);
            i++;
            continue;
        }
        if (pos + n >= size)
            break;
        memcpy(out + pos, src + i, n);
        pos += n;
        i += n;
    }
    out[pos] = 0;
    return pos;
}

/* --- Shift-JIS ----------------------------------------------------------- */

/*
 * Shift-JIS (CP932). The double-byte table lives on the memory stick
 * (tools/gen_sjis_table.py) and is only loaded once a game actually emits
 * Shift-JIS, so UTF-16/UTF-8 games don't pay its 22 KB.
 */
#define SJIS_TABLE_PATH "ms0:/SEPLUGINS/texthook/sjis.tbl"
#define SJIS_TRAILS     189                     /* 0x40..0xFC */
#define SJIS_TABLE_SIZE (60 * SJIS_TRAILS * 2)

static SceUID    sjis_block;
static const u16 *sjis_table;
static int       sjis_state;    /* 0 = not tried, 1 = loaded, -1 = unavailable */

static int sjis_load(void)
{
    if (sjis_state)
        return sjis_state > 0;
    sjis_state = -1;

    SceUID fd = sceIoOpen(SJIS_TABLE_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        th_log("sjis: %s missing (%08x); Shift-JIS text will show as hex", SJIS_TABLE_PATH, fd);
        return 0;
    }
    SceUID block = sceKernelAllocPartitionMemory(TH_PARTITION, "thk_sjis",
                                                 PSP_SMEM_High, SJIS_TABLE_SIZE, NULL);
    if (block < 0) {
        th_log("sjis: no memory for the table (%08x)", block);
        sceIoClose(fd);
        return 0;
    }

    void *table = sceKernelGetBlockHeadAddr(block);
    int   n     = sceIoRead(fd, table, SJIS_TABLE_SIZE);

    sceIoClose(fd);
    if (n != SJIS_TABLE_SIZE) {
        th_log("sjis: table is %d bytes, expected %d", n, SJIS_TABLE_SIZE);
        sceKernelFreePartitionMemory(block);
        return 0;
    }

    sjis_block = block;
    sjis_table = table;
    sjis_state = 1;
    th_log("sjis: table loaded");
    return 1;
}

/*
 * Partition blocks outlive the module: one left allocated is lost to the
 * volatile memory until the PSP restarts.
 */
void th_text_free(void)
{
    if (sjis_state > 0)
        sceKernelFreePartitionMemory(sjis_block);
    sjis_table = NULL;
    sjis_state = 0;
}

static int sjis_lead(u8 b)
{
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

static int sjis_to_utf8(const u8 *src, int bytes, char *out, int size)
{
    int have_table = sjis_load();
    int pos        = 0;

    for (int i = 0; i < bytes; i++) {
        u8  b = src[i];
        u32 cp;

        if (b < 0x80) {
            cp = b;
        } else if (b >= 0xA1 && b <= 0xDF) {
            cp = 0xFF61 + (b - 0xA1);   /* half-width katakana */
        } else if (sjis_lead(b) && i + 1 < bytes && src[i + 1] >= 0x40 && src[i + 1] <= 0xFC) {
            u8 t = src[++i];

            if (!have_table) {
                if (pos + 9 < size)
                    pos += mini_snprintf(out + pos, size - pos, "<%02x%02x>", b, t);
                continue;
            }
            int lead = b < 0xA0 ? b - 0x81 : b - 0xE0 + 31;
            cp = sjis_table[lead * SJIS_TRAILS + (t - 0x40)];
        } else {
            cp = 0xFFFD;
        }
        pos = put_utf8(out, pos, size, cp);
    }
    out[pos] = 0;
    return pos;
}

/* --- entry points -------------------------------------------------------- */

int th_to_utf8(const th_event *ev, char *out, int outsize)
{
    switch (ev->enc) {
    case THK_ENC_UTF16LE:
        return utf16le_to_utf8(ev->data, ev->len, out, outsize);
    case THK_ENC_SJIS:
        return sjis_to_utf8(ev->data, ev->len, out, outsize);
    default:
        return utf8_sanitize(ev->data, ev->len, out, outsize);
    }
}

u32 th_hash(const char *s, int len)
{
    u32 h = 0x811C9DC5;     /* FNV-1a */

    while (len--) {
        h ^= (u8)*s++;
        h *= 0x01000193;
    }
    return h;
}

int th_put_utf8(char *out, u32 cp)
{
    char tmp[8];
    int  n = put_utf8(tmp, 0, sizeof(tmp), cp);

    memcpy(out, tmp, n);
    return n;
}

/* Malformed bytes decode as U+FFFD, one byte at a time. */
u32 th_next_utf8(const char **s)
{
    const u8 *p = (const u8 *)*s;
    u32      cp;
    int      n;

    if (!p[0])
        return 0;
    if (p[0] < 0x80)
        cp = p[0], n = 1;
    else if ((p[0] & 0xE0) == 0xC0)
        cp = p[0] & 0x1F, n = 2;
    else if ((p[0] & 0xF0) == 0xE0)
        cp = p[0] & 0x0F, n = 3;
    else if ((p[0] & 0xF8) == 0xF0)
        cp = p[0] & 0x07, n = 4;
    else
        cp = 0xFFFD, n = 1;

    for (int k = 1; k < n; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            *s += 1;
            return 0xFFFD;
        }
        cp = cp << 6 | (p[k] & 0x3F);
    }
    *s += n;
    return cp;
}
