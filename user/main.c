/*
 * texthook.prx: the network half of the texthooker.
 *
 * Loaded by the core into the volatile memory when the Wi-Fi switch goes on
 * in-game, and stopped and unloaded when it goes off (or for sleep and
 * quitting). The core captures the text (its capture.c runs the game's
 * script); this module's worker thread turns what it captures into UTF-8
 * lines with the script's filters and serves them over Wi-Fi (net.c).
 */

#include <pspkernel.h>
#include <pspsysmem.h>

#include "internal.h"
#include "mini.h"
#include "thk_abi.h"

PSP_MODULE_INFO("texthook", 0, 1, 0);

const char th_log_name[] = "user";
char       th_title_id[16];

/* --- finding the game ---------------------------------------------------- */

/* "ULJM05338", from the core (the kernel's game info). */
static int detect_title(char *out, int size, int verbose)
{
    int r = thkGetTitleId(out, size);

    if (r >= 0 && out[0])
        return 0;
    if (verbose)
        th_log("title id: not available yet (%08x)", r);
    return -1;
}

/*
 * Title detection can fail if it runs before the game's info is set up,
 * so retry for a while before giving up.
 */
static const th_game *wait_for_game(void)
{
    for (int attempt = 0; attempt < 300; attempt++) {
        if (detect_title(th_title_id, sizeof(th_title_id), attempt == 0 || attempt == 299) == 0) {
            const th_game *g = th_find_game(th_title_id);

            if (g)
                th_log("%s: %s, %d hook(s) (attempt %d)",
                       th_title_id, g->title, g->nhooks, attempt);
            else
                th_log("%s: no hooks for this title", th_title_id);
            return g;
        }
        sceKernelDelayThread(100 * 1000);
    }

    th_log("could not determine title ID after 30 s");
    return NULL;
}

/* --- the worker ---------------------------------------------------------- */

/*
 * Turns the core's captured events into lines: decode, run the hook's
 * filter, drop repeats, then log and publish them for the HTTP server.
 * Starts after the newest event when the module loads: text read while
 * the switch was off isn't replayed to the page and the reading session.
 */
#define REPEAT_HOOKS    16          /* hooks whose last line is remembered */

static u32 next_seq, events_missed;

u32 th_events_dropped(void)
{
    return events_missed;
}

static void process_events(void)
{
    static char line[TH_LINE_MAX];
    static u32  last_hash[REPEAT_HOOKS];
    static u32  reported;
    static int  started;
    u32         range[2];

    if (thkEventRange(range) < 0)
        return;
    if (!started) {
        next_seq = range[1] + 1;
        started  = 1;
    }
    if (range[1] && next_seq < range[0]) {
        events_missed += range[0] - next_seq;
        next_seq = range[0];
    }

    for (; range[1] && next_seq <= range[1]; next_seq++) {
        int hook;
        int n = th_event_line(next_seq, line, sizeof(line), &hook);

        if (n < 0)
            events_missed++;
        if (n <= 0 || hook >= th_current_game->nhooks)
            continue;

        /* drop an exact repeat of this hook's previous line */
        u32 hash = th_hash(line, n);
        if (hook < REPEAT_HOOKS) {
            if (last_hash[hook] == hash)
                continue;
            last_hash[hook] = hash;
        }

        th_log("text[%s] %s", th_current_game->hooks[hook].name, line);
        th_lines_add(hook, line);
    }

    if (events_missed != reported) {
        th_log("%lu captured strings were overwritten before they were read", events_missed);
        reported = events_missed;
    }
}

/*
 * Logs free user memory whenever it has moved by 32 KB or more, checked
 * once a second: shows what a game allocates after boot, and for a game
 * that stalls, whether it ran short.
 */
static void watch_memory(void)
{
    static u32 last_check, last_free;
    u32 now = sceKernelGetSystemTimeLow();

    if (last_check && (u32)(now - last_check) < 1000 * 1000)
        return;
    last_check = now;

    u32 free = sceKernelTotalFreeMemSize();
    if (last_free && free + 32 * 1024 > last_free && free < last_free + 32 * 1024)
        return;
    last_free = free;
    th_log("mem: free %lu KB (largest %lu KB)", free / 1024,
           (u32)sceKernelMaxFreeMemSize() / 1024);
}

static SceUID       worker_id;
static volatile int stopping;

static int worker(SceSize args, void *argp)
{
    th_config_load();
    th_session_load();
    const th_game *g = wait_for_game();

    th_current_game = g;    /* its hooks run in the core; here, its filters */

    th_net_start();

    while (!stopping) {
        if (g)
            process_events();
        watch_memory();
        if (th_session_tick())
            th_http_session_event("pause");
        th_net_poll();          /* also paces this loop (~20 ms) */
    }

    th_log("stopping");
    th_net_stop(0);             /* the core unloads the network modules */
    th_text_free();
    return 0;                   /* module_stop deletes the thread */
}

/* --- module start and stop ----------------------------------------------- */

int module_start(SceSize args, void *argp)
{
    th_log("texthook starting");

    int api = thkGetApiVersion();
    if (api != THK_API_VERSION) {
        th_log("thkcore missing or wrong version (%08x); load thkcore.prx first", api);
        return 0;
    }
    th_log("loaded at %p", module_start);

    /* The stack comes from the volatile memory too. */
    SceKernelThreadOptParam opt;

    memset(&opt, 0, sizeof(opt));
    opt.size      = sizeof(opt);
    opt.stackMpid = TH_PARTITION;

    worker_id = sceKernelCreateThread("texthook_worker", worker, 0x60, 0x8000, 0, &opt);
    if (worker_id < 0) {
        th_log("could not create worker thread (%08x)", worker_id);
        return 0;
    }

    sceKernelStartThread(worker_id, 0, NULL);
    return 0;
}

/*
 * The core stops this module when the switch goes off, and for sleep and
 * quitting: the worker shuts the network down and exits first.
 */
int module_stop(SceSize args, void *argp)
{
    if (worker_id <= 0)
        return 0;
    stopping = 1;

    SceUInt timeout = 3 * 1000 * 1000;
    int     r       = sceKernelWaitThreadEnd(worker_id, &timeout);

    if (r < 0) {
        th_log("worker didn't stop (%08x)", r);
        sceKernelTerminateDeleteThread(worker_id);
    } else {
        sceKernelDeleteThread(worker_id);
    }

    return 0;
}
