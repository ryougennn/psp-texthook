/*
 * String helpers for hook filters. All work in place on NUL-terminated
 * UTF-8; matching is byte-wise, which is safe for UTF-8 needles.
 */

#include "texthook.h"
#include "mini.h"

int th_remove(char *s, const char *needle)
{
    int  n = strlen(needle), count = 0;
    char *r = s, *w = s;

    if (!n)
        return 0;

    while (*r) {
        if (!strncmp(r, needle, n)) {
            r += n;
            count++;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;

    return count;
}

int th_replace(char *s, int size, const char *from, const char *to)
{
    int nf = strlen(from), nt = strlen(to), len = strlen(s), count = 0;

    if (!nf)
        return 0;

    for (char *p = s; *p;) {
        if (strncmp(p, from, nf)) {
            p++;
            continue;
        }
        if (len - nf + nt >= size)
            break;      /* no room; leave the rest untouched */

        memmove(p + nt, p + nf, len - (p - s) - nf + 1);
        memcpy(p, to, nt);
        len += nt - nf;
        p   += nt;
        count++;
    }

    return count;
}

int th_remove_between(char *s, const char *open, const char *close)
{
    int  no = strlen(open), nc = strlen(close), count = 0;
    char *r = s, *w = s;

    if (!no || !nc)
        return 0;

    while (*r) {
        if (!strncmp(r, open, no)) {
            const char *end = r + no;

            while (*end && strncmp(end, close, nc))
                end++;
            if (*end) {
                r = (char *)end + nc;
                count++;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = 0;

    return count;
}

static int space_at(const char *p)
{
    if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        return 1;
    if (!strncmp(p, "\xE3\x80\x80", 3))     /* U+3000 IDEOGRAPHIC SPACE */
        return 3;
    return 0;
}

int th_trim(char *s)
{
    int  count = 0, n;
    char *p = s;

    /* leading: move the rest down */
    while ((n = space_at(p)))
        p += n, count++;
    int len = strlen(p);
    memmove(s, p, len + 1);

    /* trailing: cut off */
    for (;;) {
        if (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                    s[len - 1] == '\r' || s[len - 1] == '\n')) {
            s[--len] = 0;
        } else if (len >= 3 && !strncmp(s + len - 3, "\xE3\x80\x80", 3)) {
            len -= 3;
            s[len] = 0;
        } else {
            break;
        }
        count++;
    }

    return count;
}
