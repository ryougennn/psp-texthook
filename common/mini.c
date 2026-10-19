/* The freestanding libc subset declared in mini.h. */

#include "mini.h"

/* --- memory -------------------------------------------------------------- */

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    while (n--)
        d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;

    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

/* --- numbers ------------------------------------------------------------- */

int abs(int v)
{
    return v < 0 ? -v : v;
}

long labs(long v)
{
    return v < 0 ? -v : v;
}

/* --- strings ------------------------------------------------------------- */

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p)
        p++;
    return p - s;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            break;
    }
    return 0;
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (; *hay; hay++)
        if (!strncmp(hay, needle, n))
            return (char *)hay;
    return n ? NULL : (char *)hay;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = lower((unsigned char)*a) - lower((unsigned char)*b);
        if (d || !*a)
            return d;
    }
    return 0;
}

int atoi_simple(const char *s)
{
    int v = 0, neg = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-')
        neg = 1, s++;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

char *strncpy0(char *dst, const char *src, size_t size)
{
    size_t i = 0;

    if (!size)
        return dst;
    for (; i + 1 < size && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
    return dst;
}

/* --- formatting ---------------------------------------------------------- */

/* Output state: writes past `size` are counted but dropped. */
struct out {
    char    *buf;
    size_t  size, len;
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void put_num(struct out *o, unsigned int v, unsigned base, int upper,
                    int neg, int width, char pad)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg)
        width--;
    if (neg && pad == '0')
        put(o, '-');
    for (; width > n; width--)
        put(o, pad);
    if (neg && pad != '0')
        put(o, '-');
    while (n)
        put(o, tmp[--n]);
}

int mini_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;

        /* flags, width, length */
        char pad   = ' ';
        int  width = 0;

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l')     /* long is 32-bit here; accepted for u32 */
            fmt++;

        /* conversion */

        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            put_num(&o, v < 0 ? -(unsigned)v : (unsigned)v, 10, 0, v < 0, width, pad);
            break;
        }
        case 'u':
            put_num(&o, va_arg(ap, unsigned), 10, 0, 0, width, pad);
            break;
        case 'x':
        case 'X':
            put_num(&o, va_arg(ap, unsigned), 16, *fmt == 'X', 0, width, pad);
            break;
        case 'p':
            put(&o, '0');
            put(&o, 'x');
            put_num(&o, (unsigned)va_arg(ap, void *), 16, 0, 0, 8, '0');
            break;
        case 'c':
            put(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int n = strlen(s);
            for (; width > n; width--)
                put(&o, ' ');
            while (*s)
                put(&o, *s++);
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case 0:
            fmt--;
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
            break;
        }
    }

    if (size)
        buf[o.len < size ? o.len : size - 1] = 0;
    return o.len;
}

int mini_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int n = mini_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
