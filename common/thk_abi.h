/*
 * Contract between the kernel core (thkcore.prx) and the user modules
 * (texthook.prx, the network; thkpopup.prx, the dictionary popup). All
 * include this; keep it in sync with core/exports.exp.
 */

#ifndef THK_ABI_H
#define THK_ABI_H

#include <psptypes.h>

#define THK_API_VERSION     10

/*
 * User-accessible RAM: the volatile memory (partition 5, 0x08400000, where
 * the user modules run) and the user partition (the game).
 * The PSP-1000 has 32 MB, so user RAM ends at 0x0A000000.
 */
#define THK_USER_RAM_START  0x08400000u
#define THK_USER_RAM_END    0x0A000000u

/* Hook states and results. */
#define THK_OK              0
#define THK_PENDING         1           /* target not loaded yet; patched at module start */
#define THK_ERR_ARGS        0x80EE0001  /* pointer outside user RAM or misaligned */
#define THK_ERR_FULL        0x80EE0002  /* too many hooks */
#define THK_ERR_DELAY_SLOT  0x80EE0003  /* target is in a branch delay slot */
#define THK_ERR_BRANCH      0x80EE0004  /* target+4 is a branch/jump */
#define THK_ERR_REACH       0x80EE0005  /* thunk not in target's 256 MB j-region */
#define THK_ERR_EXPECT      0x80EE0006  /* instruction (or word) at target != expected */
#define THK_ERR_STOPPING    0x80EE0008  /* thkNetModules: texthook.prx is being stopped */
#define THK_ERR_GONE        0x80EE0009  /* thkEventGet: overwritten, or not captured yet */

/* --- captured text ------------------------------------------------------- */

/*
 * The core runs the game's hooks (the scripts in games/ are compiled into
 * it) and keeps the most recent captured strings, raw, numbered 1, 2, 3,
 * ... Readers keep their own place: texthook.prx reads each one once, the
 * popup the newest.
 */
#define THK_ENC_UTF8        0           /* also th_emit_text() */
#define THK_ENC_UTF16LE     1
#define THK_ENC_SJIS        2

typedef struct thk_event {
    u16  len;           /* bytes in data */
    u8   hook;          /* index in the game's hook list */
    u8   enc;           /* THK_ENC_* */
    u8   data[];
} thk_event;

/* out[0] = the oldest event still kept, out[1] = the newest (0, 0: none yet). */
int thkEventRange(u32 *out);

/*
 * Copies event `seq` (header and data, at most `size` bytes) to out.
 * Returns the bytes copied, or THK_ERR_GONE.
 */
int thkEventGet(u32 seq, thk_event *out, int size);

/*
 * Final state of hook `addr` (THK_OK, THK_PENDING or THK_ERR_*), or
 * THK_ERR_ARGS if the game has no such hook.
 */
int thkGetHookState(u32 addr);

/*
 * Not for calling: the hook stubs reach the core through this export's
 * syscall (core/capture.c).
 */
int thkHookEntry(u32 index, void *ctx);

/* --- the running game ---------------------------------------------------- */

/* Copies the running title ID (e.g. "ULJM05338") into out. Returns 0 or < 0. */
int thkGetTitleId(char *out, int size);

/* --- overlay ------------------------------------------------------------- */

#define THK_OVERLAY_MAX_TEXT    56  /* chars; all fit across the screen (8 px each) */
#define THK_OVERLAY_QR          1   /* also show the text as a QR code */

#define THK_CORNER_TOP_LEFT     0
#define THK_CORNER_TOP_RIGHT    1
#define THK_CORNER_BOTTOM_LEFT  2
#define THK_CORNER_BOTTOM_RIGHT 3

/*
 * Show `msg` (ASCII; anything else becomes '?') for `ms` milliseconds,
 * replacing any current message.
 */
int thkOverlayShow(const char *msg, int ms, int flags);

/* Enable/disable the overlay and pick its corner (THK_CORNER_*). */
int thkOverlayConfig(int enable, int corner);

/*
 * Temporarily stop drawing (e.g. while taking a screenshot). Returns
 * whether a message was visible before the call.
 */
int thkOverlayHide(int hide);

/* --- dictionary popup ---------------------------------------------------- */

/*
 * L+R in-game makes the core load thkpopup.prx into the volatile memory
 * (locking it first, unless texthook.prx already holds it). While it is
 * loaded the game reads no buttons; the popup's own thread does.
 */
#define THK_POPUP_STRIDE    512                         /* pixels per row */
#define THK_POPUP_FB_SIZE   (THK_POPUP_STRIDE * 272 * 2) /* 480x272, RGB565 */

/*
 * fb (64-byte aligned, THK_POPUP_FB_SIZE bytes in user RAM, written back
 * from the cache): show it instead of the game's frames. NULL: the popup
 * is done; back to the game's frames, the game gets buttons again once all
 * are released, and the core unloads thkpopup.prx.
 */
int thkPopupShow(void *fb);

/*
 * The frame the game last showed, for the popup's background: out[0] =
 * address, out[1] = buffer width in pixels, out[2] = pixel format
 * (PSP_DISPLAY_PIXEL_FORMAT_*). THK_ERR_ARGS if there is none yet.
 */
int thkPopupFrame(u32 *out);

/* --- network modules and memory ------------------------------------------ */

/*
 * Load the network modules (ifhandle, pspnet, inet, apctl, resolver) with
 * their code and data in `partition`, which must be 5 (the volatile memory,
 * held by the core while texthook.prx is loaded). 0 stops and unloads them
 * (ifhandle stays).
 *
 * Returns 0 or the first load/start error; on error nothing is left loaded
 * but ifhandle. Returns THK_ERR_STOPPING, having loaded nothing, when the
 * core is stopping texthook.prx; unloading (0) is then skipped and left to
 * the core.
 */
int thkNetModules(int partition);

/*
 * Free memory of `partition` (2 or 5): out[0] = total free bytes,
 * out[1] = largest free block. Returns THK_OK or THK_ERR_ARGS.
 */
int thkMemInfo(int partition, u32 *out);

/* --- version ------------------------------------------------------------- */

/*
 * Returns THK_API_VERSION. Used by the user module to confirm the core
 * is loaded (unresolved imports return 0x8002013A instead).
 */
int thkGetApiVersion(void);

#endif /* THK_ABI_H */
