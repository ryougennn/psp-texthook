/*
 * thkpopup.prx, the dictionary popup: L+R in-game shows the last captured
 * line over the game, darkened, with a dictionary entry for the selected
 * word.
 *
 * The core loads this module into the volatile memory when L+R is pressed
 * (whatever the Wi-Fi switch says), hides every button from the game, and
 * shows the popup's frames instead of the game's (thkPopupShow). Its
 * thread runs the popup once and says when it's done (thkPopupShow(NULL));
 * the core then unloads it and releases the memory.
 *
 * The line comes from the core, which captures the text: the newest one
 * the game's filters leave something of.
 *
 * Everything it needs is allocated from the volatile memory when it opens
 * and freed when it closes: three 272 KB frames (the darkened game frame
 * and two to draw into, alternately), the font's tables and glyph cache
 * (about 250 KB) and each dictionary's key filter (up to 256 KB).
 */

#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspdmac.h>
#include <psputility_sysparam.h>

#include "popup.h"
#include "mini.h"
#include "thk_abi.h"

PSP_MODULE_INFO("thkpopup", 0, 1, 0);

#define POPUP_PRIORITY  0x30
#define POPUP_STACK     0x4000

#define REPEAT_DELAY    (350 * 1000)
#define REPEAT_EVERY    (80 * 1000)

#define COMBO           (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)
#define DIRECTIONS      (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)

const char th_log_name[] = "popup";

static SceUID       thread_id;
static volatile int stopping;
static th_popup     pop;
static u32          close_button;   /* the system's cancel button */

/* --- memory -------------------------------------------------------------- */

static SceUID block = -1;
static u16    *bg, *frames[2];
static int    back;             /* the frame to draw into next */

static int alloc_frames(void)
{
    block = sceKernelAllocPartitionMemory(TH_PARTITION, "thk_popup", PSP_SMEM_High,
                                          3 * THK_POPUP_FB_SIZE + 64, NULL);
    if (block < 0) {
        u32 mem[2] = { 0, 0 };

        thkMemInfo(TH_PARTITION, mem);
        th_log("popup: no memory for the frames (%08x); %lu KB free, largest %lu KB",
               block, mem[0] / 1024, mem[1] / 1024);
        return 0;
    }

    /* the display wants its frames 64-byte aligned */
    u32 base = ((u32)sceKernelGetBlockHeadAddr(block) + 63) & ~63u;

    frames[0] = (u16 *)base;
    frames[1] = (u16 *)(base + THK_POPUP_FB_SIZE);
    bg        = (u16 *)(base + 2 * THK_POPUP_FB_SIZE);
    back      = 0;
    return 1;
}

static void free_frames(void)
{
    if (block >= 0)
        sceKernelFreePartitionMemory(block);
    block = -1;
}

/* --- showing ------------------------------------------------------------- */

static void present(void)
{
    u16 *fb = frames[back];

    th_draw_popup(fb, bg, &pop);
    sceKernelDcacheWritebackRange(fb, THK_POPUP_FB_SIZE);
    thkPopupShow(fb);
    back ^= 1;
}

/*
 * The game's last frame, darkened. The game keeps running and redrawing
 * (CLANNAD alternates two buffers), so the frame it shows must be copied
 * before it starts drawing into it again, about a frame later: right after
 * a vblank, the buffer asked for only then (the game may have flipped
 * while we waited), in one DMA into the two draw frames (together a 32 bpp
 * frame), then darkened from the copy. Through the CPU it took 58 ms
 * uncached and 15 ms cached, and both still caught it mid-redraw.
 *
 * The draw frames' range is cleared from the data cache before the DMA (so
 * no stale line is written back over it) and after (so none is read). The
 * first DMA version, without either, went grey after a second.
 */
#define RAW_MAX (2 * THK_POPUP_FB_SIZE)

static void make_background(void)
{
    u32 f[3];
    u32 *raw = (u32 *)frames[0];

    sceKernelDcacheWritebackInvalidateRange(raw, RAW_MAX);
    sceDisplayWaitVblankStart();

    u32 t0 = sceKernelGetSystemTimeLow();

    if (thkPopupFrame(f) != THK_OK || f[1] < 480 || f[1] > THK_POPUP_STRIDE || f[2] > 3) {
        th_log("popup: no game frame to show behind it");
        th_draw_background(bg, NULL, 0, 0);
        return;
    }

    int bpp  = f[2] == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
    u32 size = 272 * f[1] * bpp;                /* at most RAW_MAX */
    u32 src  = f[0] & 0x0FFFFFFF;
    int r    = sceDmacMemcpy(raw, (void *)src, size);

    if (r < 0) {
        /* CPU fallback, through the cache */
        sceKernelDcacheWritebackInvalidateRange((void *)src, size);
        for (u32 i = 0; i < size / 4; i++)
            raw[i] = ((const u32 *)src)[i];
    } else {
        sceKernelDcacheInvalidateRange(raw, size);
    }

    u32 t1 = sceKernelGetSystemTimeLow();
    th_draw_background(bg, raw, f[1], f[2]);
    th_log("popup: game frame %08lx, width %lu, format %lu: %s in %lu us, darkened in %lu us",
           f[0], f[1], f[2], r < 0 ? "copied by the CPU" : "copied by DMA", t1 - t0,
           sceKernelGetSystemTimeLow() - t1);
    if (r < 0)
        th_log("popup: sceDmacMemcpy failed (%08x)", r);
}

/*
 * The newest captured line the game's filters leave something of, as code
 * points; 0 if there is none yet.
 */
static int load_line(void)
{
    static char line[TH_LINE_MAX];
    char        id[16];
    u32         range[2];

    pop.n = 0;
    if (thkGetTitleId(id, sizeof(id)) == 0)
        th_current_game = th_find_game(id);
    if (thkEventRange(range) < 0 || !range[1])
        return 0;

    for (u32 seq = range[1]; seq >= range[0] && seq; seq--) {
        int hook;

        if (th_event_line(seq, line, sizeof(line), &hook) <= 0)
            continue;

        const char *s = line;
        u32        cp;

        while ((cp = th_next_utf8(&s)) && pop.n < TH_POPUP_TEXT)
            if (cp >= 0x20)
                pop.text[pop.n++] = cp;
        if (pop.n)
            break;
    }
    return pop.n;
}

/* --- input --------------------------------------------------------------- */

static void select_token(int cur)
{
    pop.cur    = cur;
    pop.entry  = 0;
    pop.scroll = 0;
    th_dict_select(&pop);
}

/* Returns 0 when the popup should close. */
static int handle(u32 pressed, u32 held, int have_dict)
{
    if (((held & COMBO) == COMBO && (pressed & COMBO)) || (pressed & close_button))
        return 0;
    if (!pop.ntok)
        return 1;

    int changed = 0;

    if ((pressed & PSP_CTRL_LEFT) && pop.cur > 0) {
        pop.cur--;
        changed = 1;
    }
    if ((pressed & PSP_CTRL_RIGHT) && pop.cur < pop.ntok - 1) {
        pop.cur++;
        changed = 1;
    }
    if (changed) {
        if (have_dict)
            select_token(pop.cur);
        present();
        return 1;
    }

    th_token *t = &pop.tok[pop.cur];

    if (!(held & PSP_CTRL_RTRIGGER) && (pressed & PSP_CTRL_LTRIGGER) && pop.entry > 0) {
        pop.entry--;
        changed = 1;
    }
    if (!(held & PSP_CTRL_LTRIGGER) && (pressed & PSP_CTRL_RTRIGGER) &&
        pop.entry < t->nrefs - 1) {
        pop.entry++;
        changed = 1;
    }
    if (changed) {
        pop.scroll = 0;
        th_dict_select(&pop);
        present();
        return 1;
    }

    int max_scroll = pop.content_h - th_draw_content_height();
    int scroll     = pop.scroll;

    if (pressed & PSP_CTRL_DOWN)
        scroll += TH_LINE_H;
    if (pressed & PSP_CTRL_UP)
        scroll -= TH_LINE_H;
    if (scroll > max_scroll)
        scroll = max_scroll;
    if (scroll < 0)
        scroll = 0;
    if (scroll != pop.scroll) {
        pop.scroll = scroll;
        present();
    }
    return 1;
}

/* Until closed: buttons, with the directions repeating while held. */
static void input_loop(int have_dict)
{
    SceCtrlData pad;
    u32         prev, since = 0, last_repeat = 0;

    sceCtrlPeekBufferPositive(&pad, 1);
    prev = pad.Buttons;     /* the L+R that opened it doesn't count */

    while (!stopping) {
        sceDisplayWaitVblankStart();
        sceCtrlPeekBufferPositive(&pad, 1);

        u32 held    = pad.Buttons;
        u32 pressed = held & ~prev;
        u32 now     = sceKernelGetSystemTimeLow();

        if (pressed & DIRECTIONS) {
            since       = now;
            last_repeat = now;
        } else if ((held & DIRECTIONS) && now - since > REPEAT_DELAY &&
                   now - last_repeat > REPEAT_EVERY) {
            pressed    |= held & DIRECTIONS;
            last_repeat = now;
        }
        prev = held;

        if (!handle(pressed, held, have_dict))
            break;
    }
}

/* --- the popup ----------------------------------------------------------- */

static void run(void)
{
    u32 t0 = sceKernelGetSystemTimeLow();

    if (!load_line()) {
        thkOverlayShow("Dictionary: no text captured yet", 3000, 0);
        goto out;
    }
    if (!alloc_frames()) {
        thkOverlayShow("Dictionary: not enough memory", 3000, 0);
        goto out;
    }
    if (th_pgf_open(TH_FONT_PATH) < 0) {
        thkOverlayShow("Dictionary: can't read the system font", 3000, 0);
        goto out;
    }

    /* close with the cancel button: × where ○ confirms (Japan), else ○ */
    int swap = 1;

    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &swap);
    close_button   = swap == 0 ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE;
    pop.close_mark = swap == 0 ? 0xD7 : 0x25CB;     /* × or ○ */

    int have_dict = th_dict_open() == 0;

    make_background();
    memset(pop.tok, 0, sizeof(pop.tok));
    pop.ntok   = 0;
    pop.cur    = 0;
    pop.entry  = 0;
    pop.scroll = 0;
    pop.status = have_dict ? "Looking up..."
                           : "No dictionaries in SEPLUGINS/texthook/dicts";
    present();

    /* split the line into words, then select the first one in the dictionary */
    th_dict_tokenize(&pop);
    if (have_dict) {
        int first = 0;

        while (first < pop.ntok - 1 && !pop.tok[first].nrefs)
            first++;
        if (pop.ntok)
            select_token(first);
        else
            pop.status = "No Japanese in this line";
    }
    u32 mem[2] = { 0, 0 };

    thkMemInfo(TH_PARTITION, mem);
    th_log("popup: %d characters, %d words, open in %lu ms; %lu KB free (largest %lu KB)",
           pop.n, pop.ntok, (sceKernelGetSystemTimeLow() - t0) / 1000, mem[0] / 1024,
           mem[1] / 1024);
    present();

    input_loop(have_dict);

out:
    /* the screen back to the game before the frames it shows are freed */
    th_log("popup: closing");
    thkPopupShow(NULL);

    /*
     * Nothing of ours may stay in the data cache unwritten: once the
     * memory is released, a late write-back would land on whatever the
     * system (or the next popup) put there.
     */
    sceKernelDcacheWritebackInvalidateAll();
    th_dict_close();
    th_pgf_close();
    th_text_free();
    free_frames();
}

/* Runs the popup once; its thkPopupShow(NULL) tells the core to unload this module. */
static int popup_thread(SceSize args, void *argp)
{
    run();
    th_log("popup: closed, everything freed");
    return 0;
}

/* --- module start and stop ----------------------------------------------- */

int module_start(SceSize args, void *argp)
{
    int api = thkGetApiVersion();
    if (api != THK_API_VERSION) {
        th_log("thkcore missing or wrong version (%08x)", api);
        thkPopupShow(NULL);
        return 0;
    }

    SceKernelThreadOptParam opt;

    memset(&opt, 0, sizeof(opt));
    opt.size      = sizeof(opt);
    opt.stackMpid = TH_PARTITION;

    thread_id = sceKernelCreateThread("thkpopup", popup_thread, POPUP_PRIORITY,
                                      POPUP_STACK, 0, &opt);
    if (thread_id < 0 || sceKernelStartThread(thread_id, 0, NULL) < 0) {
        th_log("popup: could not start its thread (%08x)", thread_id);
        thkPopupShow(NULL);
    }
    return 0;
}

/*
 * The core unloads the module once the popup is done, or at once for sleep
 * and quitting: then the thread closes the popup, freeing everything.
 */
int module_stop(SceSize args, void *argp)
{
    if (thread_id <= 0)
        return 0;
    stopping = 1;

    SceUInt timeout = 2 * 1000 * 1000;
    int     r       = sceKernelWaitThreadEnd(thread_id, &timeout);

    if (r < 0) {
        th_log("popup thread didn't stop (%08x)", r);
        sceKernelTerminateDeleteThread(thread_id);
    } else {
        sceKernelDeleteThread(thread_id);
    }
    return 0;
}
