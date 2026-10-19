/*
 * The core's half of the dictionary popup: the buttons and the screen.
 *
 * The game's controller reads (sceCtrl Peek/Read Buffer Positive/Negative,
 * Peek/Read Latch) are redirected here. L+R pressed together asks the
 * manager to load thkpopup.prx (main.c). From then until the popup closes
 * the game reads no buttons at all, and after it closes none until every
 * button is released, so the button that closed it doesn't also advance
 * the text. The popup thread's own reads pass straight through.
 *
 * Visual novels use L and R alone too (CLANNAD: back and skip), so the
 * first trigger of a would-be L+R mustn't reach the game before the second
 * is down: a trigger pressed alone is held back. Let go within HOLD_BACK,
 * and the game gets the tap then (replayed for TAP_REPLAY); keep holding,
 * and it gets the press after HOLD_BACK; press the other, and it's L+R.
 *
 * The screen is overlay.c's: while the popup is open, the game's frames
 * are swapped for the popup's own (thkPopupShow), and the game keeps
 * drawing into its buffers unseen.
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <pspctrl.h>
#include <systemctrl.h>

#include "core.h"
#include "mini.h"

#define NID_PeekBufferPositive  0x3A622550
#define NID_PeekBufferNegative  0xC152080A
#define NID_ReadBufferPositive  0x1F803938
#define NID_ReadBufferNegative  0x60B81F86
#define NID_PeekLatch           0xB1D0E5CD
#define NID_ReadLatch           0x0B588501

/* The buttons a game can read; HOME, HOLD, the Wi-Fi switch etc. pass. */
#define GAME_BUTTONS    0xF3F9
#define COMBO           (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)
#define HOLD_BACK       (1000 * 1000)   /* a trigger held alone reaches the game after this */
#define TAP_REPLAY      (100 * 1000)    /* a held-back tap is replayed this long: ~6 frames */
#define TAP_GAP         (50 * 1000)     /* and that long after the last, so two stay two */

enum { MASK_OFF, MASK_ON, MASK_UNTIL_RELEASE };

static int (*orig_peek_pos)(SceCtrlData *pad, int count);
static int (*orig_peek_neg)(SceCtrlData *pad, int count);
static int (*orig_read_pos)(SceCtrlData *pad, int count);
static int (*orig_read_neg)(SceCtrlData *pad, int count);
static int (*orig_peek_latch)(SceCtrlLatch *latch);
static int (*orig_read_latch)(SceCtrlLatch *latch);

static volatile SceUID popup_thread;    /* the thread showing the popup; its reads pass */
static volatile int    mask, combo_held, shown, requested, finished;

/* Each trigger alone: L, R */
static const u32 trigger[2] = { PSP_CTRL_LTRIGGER, PSP_CTRL_RTRIGGER };
static u32       held_since[2], replay_until[2];   /* sceKernelGetSystemTimeLow() | 1; 0 = no */
static u8        taps[2];                           /* held-back taps still to replay */
static u32       triggers_down;                     /* as of the last read */
static u32       latch_made;                        /* replayed taps, for latch readers */

/* What the game gets of the triggers alone: these hidden, these added. */
static u32 hide, add;

/* --- buttons ------------------------------------------------------------- */

/* Holds back a trigger pressed alone, and replays it as a tap if let go (see above). */
static void hold_back(u32 pressed, int combo)
{
    u32 now = sceKernelGetSystemTimeLow();

    hide = add = 0;
    for (int i = 0; i < 2; i++) {
        u32 t    = trigger[i];
        int down = (pressed & t) != 0;

        if (combo) {
            held_since[i] = taps[i] = 0;
            continue;
        }
        if (down && !(triggers_down & t) && !(pressed & trigger[!i]))
            held_since[i] = now | 1;

        if (held_since[i]) {
            if (!down) {
                held_since[i] = 0;
                taps[i]++;
            } else if ((int)(now - held_since[i]) >= HOLD_BACK) {
                held_since[i] = 0;
            } else {
                hide |= t;
            }
        }

        /* taps are replayed one at a time, with a gap between them */
        if (replay_until[i] && (int)(replay_until[i] + TAP_GAP - now) <= 0)
            replay_until[i] = 0;
        if (!replay_until[i] && taps[i]) {
            taps[i]--;
            replay_until[i] = (now + TAP_REPLAY) | 1;
        }
        if (replay_until[i] && (int)(replay_until[i] - now) > 0)
            add |= t;
    }
    triggers_down = pressed & COMBO;
}

/* Sees what the game read: opens the popup on L+R, ends a mask on release. */
static void watch(u32 pressed)
{
    int combo = (pressed & COMBO) == COMBO;

    if (combo && !combo_held && mask == MASK_OFF) {
        mask      = MASK_ON;
        requested = 1;
    }
    combo_held = combo;
    hold_back(pressed, combo);

    if (mask == MASK_UNTIL_RELEASE && !(pressed & GAME_BUTTONS))
        mask = MASK_OFF;
}

static int filter_buffer(SceCtrlData *pad, int r, int negative)
{
    if (r <= 0 || sceKernelGetThreadId() == popup_thread)
        return r;

    u32 last = pad[r - 1].Buttons;
    watch(negative ? ~last : last);

    for (int i = 0; i < r; i++) {
        if (mask != MASK_OFF) {
            if (negative)
                pad[i].Buttons |= GAME_BUTTONS;
            else
                pad[i].Buttons &= ~GAME_BUTTONS;
            pad[i].Lx = pad[i].Ly = 128;
        } else if (negative) {
            pad[i].Buttons = (pad[i].Buttons | hide) & ~add;
        } else {
            pad[i].Buttons = (pad[i].Buttons & ~hide) | add;
        }
    }
    return r;
}

static int filter_latch(SceCtrlLatch *l, int r)
{
    if (r < 0 || sceKernelGetThreadId() == popup_thread)
        return r;

    watch(l->uiPress);
    if (mask != MASK_OFF) {
        l->uiMake    &= ~GAME_BUTTONS;
        l->uiBreak   &= ~GAME_BUTTONS;
        l->uiPress   &= ~GAME_BUTTONS;
        l->uiRelease |= GAME_BUTTONS;
        return r;
    }

    /* a replayed tap: made on its first read, broken on the first read after */
    l->uiMake    = (l->uiMake & ~hide) | (add & ~latch_made);
    l->uiPress   = (l->uiPress & ~hide) | add;
    l->uiRelease = (l->uiRelease | hide) & ~add;
    l->uiBreak   = (l->uiBreak & ~(hide | add)) | (latch_made & ~add);
    latch_made   = add;
    return r;
}

static int peek_pos_hook(SceCtrlData *pad, int count)
{
    return filter_buffer(pad, orig_peek_pos(pad, count), 0);
}

static int peek_neg_hook(SceCtrlData *pad, int count)
{
    return filter_buffer(pad, orig_peek_neg(pad, count), 1);
}

static int read_pos_hook(SceCtrlData *pad, int count)
{
    return filter_buffer(pad, orig_read_pos(pad, count), 0);
}

static int read_neg_hook(SceCtrlData *pad, int count)
{
    return filter_buffer(pad, orig_read_neg(pad, count), 1);
}

static int peek_latch_hook(SceCtrlLatch *latch)
{
    return filter_latch(latch, orig_peek_latch(latch));
}

static int read_latch_hook(SceCtrlLatch *latch)
{
    return filter_latch(latch, orig_read_latch(latch));
}

void popup_init(void)
{
    static const struct {
        u32  nid;
        void **orig;
        void *hook;
    } calls[] = {
        { NID_PeekBufferPositive, (void **)&orig_peek_pos,   peek_pos_hook },
        { NID_PeekBufferNegative, (void **)&orig_peek_neg,   peek_neg_hook },
        { NID_ReadBufferPositive, (void **)&orig_read_pos,   read_pos_hook },
        { NID_ReadBufferNegative, (void **)&orig_read_neg,   read_neg_hook },
        { NID_PeekLatch,          (void **)&orig_peek_latch, peek_latch_hook },
        { NID_ReadLatch,          (void **)&orig_read_latch, read_latch_hook },
    };
    int hooked = 0;

    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        u32 f = sctrlHENFindFunction("sceController_Service", "sceCtrl", calls[i].nid);

        if (!f) {
            klog("popup: sceCtrl function %08lx not found", calls[i].nid);
            continue;
        }
        *calls[i].orig = (void *)f;
        sctrlHENPatchSyscall((void *)f, calls[i].hook);
        hooked++;
    }
    sctrlFlushCache();
    klog("popup: %d controller calls hooked", hooked);
}

/* The popup is gone (unloaded, or never started). */
void popup_reset(void)
{
    popup_thread = 0;
    if (shown) {
        overlay_popup(NULL);
        shown = 0;
    }
    if (mask != MASK_OFF)
        mask = MASK_UNTIL_RELEASE;
}

int popup_requested(void)
{
    int r = requested;

    requested = 0;
    return r;
}

int popup_finished(void)
{
    int r = finished;

    finished = 0;
    return r;
}

/* --- API ----------------------------------------------------------------- */

int thkPopupShow(void *fb)
{
    u32 a = (u32)fb;

    if (fb && ((a & 63) || !user_range(a, THK_POPUP_FB_SIZE)))
        return THK_ERR_ARGS;

    if (fb) {
        popup_thread = sceKernelGetThreadId();
        mask         = MASK_ON;
        shown        = 1;
        overlay_popup(fb);
    } else {
        popup_reset();
        finished = 1;
    }
    return THK_OK;
}

int thkPopupFrame(u32 *out)
{
    int k1  = pspSdkSetK1(0);
    u32 a   = (u32)out;
    int ret = THK_ERR_ARGS;

    if (!(a & 3) && user_range(a, 3 * sizeof(u32)))
        ret = overlay_game_frame(out);

    pspSdkSetK1(k1);
    return ret;
}
