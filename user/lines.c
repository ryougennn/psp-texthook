/*
 * Recent processed lines, numbered for SSE event IDs. Only the worker
 * thread touches this, so there is no locking.
 */

#include "internal.h"
#include "mini.h"

#define HISTORY 16

static th_line lines[HISTORY];
static u32     latest;

void th_lines_add(int hook, const char *text)
{
    th_line *l = &lines[++latest % HISTORY];

    l->seq  = latest;
    l->hook = hook;
    strncpy0(l->text, text, sizeof(l->text));
}

const th_line *th_lines_get(u32 seq)
{
    if (!seq || seq > latest)
        return NULL;

    const th_line *l = &lines[seq % HISTORY];
    return l->seq == seq ? l : NULL;
}

u32 th_lines_latest(void)
{
    return latest;
}

u32 th_lines_oldest(void)
{
    return latest > HISTORY ? latest - HISTORY + 1 : (latest ? 1 : 0);
}
