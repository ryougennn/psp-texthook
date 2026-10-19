/*
 * The game's script, as the user modules see it: its hooks' names and
 * filters (the hook functions themselves run in the core). Shared by
 * texthook.prx and thkpopup.prx.
 */

#include <pspkernel.h>

#include "mini.h"
#include "shared.h"

const th_game *th_current_game;

const th_game *th_find_game(const char *id)
{
    for (int i = 0; th_games[i]; i++)
        for (const char *const *p = th_games[i]->ids; *p; p++)
            if (!strcmp(*p, id))
                return th_games[i];
    return NULL;
}

int th_event_line(u32 seq, char *out, int size, int *hook)
{
    static th_event ev;

    int r = thkEventGet(seq, (thk_event *)&ev, sizeof(ev));
    if (r < 0)
        return r;

    *hook = ev.hook;
    th_to_utf8(&ev, out, size);
    if (th_current_game && ev.hook < th_current_game->nhooks &&
        th_current_game->hooks[ev.hook].filter)
        th_current_game->hooks[ev.hook].filter(out, size);
    return strlen(out);
}
