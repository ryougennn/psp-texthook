/*
 * Text capture in the core: the game's hook script runs here, and the
 * strings it emits are kept for the user modules to read.
 *
 * The scripts (games/) are compiled into the core, so capture needs
 * nothing in the volatile memory and works whatever the Wi-Fi switch says.
 * A hooked instruction still has to jump somewhere the game's user-mode
 * thread can run, so each hook gets a small thunk, and all share one entry
 * routine (hook_entry.S), in a block of user memory (288 bytes plus 60
 * per hook) taken from the game's partition once, when the hooks are
 * installed:
 *
 *   game:  addr+0: j thunk         thunk: addiu sp, sp, -FRAME_SIZE
 *          addr+4: nop                    sw    at, R(1)(sp)
 *                                         sw    ra, R(31)(sp)
 *                                         lui   at, 0
 *                                         jal   entry
 *                                         ori   at, at, index
 *                                         lw    ra, R(31)(sp)
 *                                         lw    at, R(1)(sp)
 *                                         addiu sp, sp, FRAME_SIZE
 *                                         <tail: THK_TAIL_WORDS filled by
 *                                          hook_install(), see hook.c>
 *
 * The entry saves every register into a th_ctx on the game's stack and
 * makes the syscall for thkHookEntry, which runs the script's function in
 * kernel mode on the game's own thread. Hence the rules for hook functions:
 * no logging, no blocking, no floats, little stack (this is the thread's
 * kernel stack).
 *
 * Captured strings go into a small history (EVENT_BYTES, raw, as emitted)
 * with sequence numbers, overwritten oldest first. Readers never remove
 * anything: texthook.prx keeps its place, the popup takes the newest.
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman.h>
#include <pspsysmem_kernel.h>
#include <systemctrl.h>

#include "core.h"
#include "hook_frame.h"
#include "mini.h"
#include "texthook.h"

_Static_assert(sizeof(th_ctx) == CTX_SIZE, "th_ctx does not match hook_frame.h");

#define THUNK_TAIL          9
#define THUNK_WORDS         (THUNK_TAIL + THK_TAIL_WORDS)
#define EVENT_BYTES         8192
#define EVENT_SLOTS         32
#define TITLE_TRIES         600             /* the manager's 50 ms ticks: 30 s */

/* Kernel RAM, cached: where the core's own (and so the scripts') buffers are. */
#define KERNEL_RAM_START    0x88000000u
#define KERNEL_RAM_END      0x88400000u

/* hook_entry.S, the code copied into user memory */
extern u32 thk_entry_start[], thk_entry_syscall[], thk_entry_end[];

/* the games registry the Makefile generates */
extern const th_game *const th_games[];

static const th_game *game;

/* --- the event history --------------------------------------------------- */

typedef struct {
    u32  seq;                           /* 0 = empty or overwritten */
    u16  off, len;
    u8   hook, enc;
} event_slot;

static u8         store[EVENT_BYTES];
static event_slot slots[EVENT_SLOTS];
static u32        newest, write_at;

/* Hook context. Interrupts stay off while the store is inconsistent. */
static void push(const th_ctx *c, int enc, const void *src, u32 len)
{
    if (len > TH_MAX_TEXT)
        len = TH_MAX_TEXT;

    int intr = sceKernelCpuSuspendIntr();

    if (write_at + len > EVENT_BYTES)
        write_at = 0;

    /*
     * Whatever the new string covers is gone, and so is anything older,
     * though it may lie elsewhere in the store: readers see one unbroken
     * run of numbers.
     */
    u32 cut = 0;

    for (int i = 0; i < EVENT_SLOTS; i++) {
        event_slot *s = &slots[i];

        if (s->seq > cut && s->off < write_at + len && write_at < (u32)s->off + s->len)
            cut = s->seq;
    }
    for (int i = 0; cut && i < EVENT_SLOTS; i++)
        if (slots[i].seq <= cut)
            slots[i].seq = 0;

    event_slot *s = &slots[++newest % EVENT_SLOTS];

    s->seq  = newest;
    s->off  = write_at;
    s->len  = len;
    s->hook = c->hook;
    s->enc  = enc;
    memcpy(store + write_at, src, len);
    write_at += len;

    sceKernelCpuResumeIntr(intr);
}

/* A caller's buffer: word-aligned, in user RAM. */
static int user_buffer(const void *p, u32 len)
{
    return !((u32)p & 3) && user_range((u32)p, len);
}

int thkEventRange(u32 *out)
{
    int k1 = pspSdkSetK1(0);
    int ret = THK_ERR_ARGS;

    if (user_buffer(out, 8)) {
        int intr = sceKernelCpuSuspendIntr();
        u32 oldest = 0;

        for (int i = 0; i < EVENT_SLOTS; i++)
            if (slots[i].seq && (!oldest || slots[i].seq < oldest))
                oldest = slots[i].seq;
        out[0] = oldest;
        out[1] = oldest ? newest : 0;
        sceKernelCpuResumeIntr(intr);
        ret = THK_OK;
    }

    pspSdkSetK1(k1);
    return ret;
}

int thkEventGet(u32 seq, thk_event *out, int size)
{
    int k1 = pspSdkSetK1(0);
    int ret = THK_ERR_ARGS;

    if (size >= (int)sizeof(thk_event) && user_buffer(out, size)) {
        int        intr = sceKernelCpuSuspendIntr();
        event_slot *s   = &slots[seq % EVENT_SLOTS];

        if (seq && s->seq == seq) {
            u32 len = s->len;

            if (len > size - sizeof(thk_event))
                len = size - sizeof(thk_event);
            out->len  = len;
            out->hook = s->hook;
            out->enc  = s->enc;
            memcpy(out->data, store + s->off, len);
            ret = sizeof(thk_event) + len;
        } else {
            ret = THK_ERR_GONE;
        }
        sceKernelCpuResumeIntr(intr);
    }

    pspSdkSetK1(k1);
    return ret;
}

/* --- reading game memory (hook context) ---------------------------------- */

int th_valid(u32 addr, u32 len)
{
    return user_range(addr & 0x3FFFFFFF, len);     /* the uncached mirror too */
}

u8 th_read8(u32 addr)
{
    return th_valid(addr, 1) ? *(volatile u8 *)addr : 0;
}

u16 th_read16(u32 addr)
{
    return th_valid(addr, 2) && !(addr & 1) ? *(volatile u16 *)addr : 0;
}

u32 th_read32(u32 addr)
{
    return th_valid(addr, 4) && !(addr & 3) ? *(volatile u32 *)addr : 0;
}

/* --- emitting text (hook context) ---------------------------------------- */

/*
 * Where a string may be read from, and how far: game memory, or kernel
 * memory, where a script's own buffers now are (SNOW and Amagami build the
 * line in a static buffer and emit that).
 */
static u32 room_at(u32 addr)
{
    u32 a = addr & 0x3FFFFFFF;

    if (a >= THK_USER_RAM_START && a < THK_USER_RAM_END)
        return THK_USER_RAM_END - a;
    if (addr >= KERNEL_RAM_START && addr < KERNEL_RAM_END)
        return KERNEL_RAM_END - addr;
    return 0;
}

/*
 * Length in bytes of a NUL-terminated string of `unit`-byte code units,
 * scanning at most `max_bytes` and never past the end of its memory.
 */
static u32 scan_len(u32 addr, u32 unit, u32 max_bytes)
{
    u32 room = room_at(addr);

    if (room < unit)
        return 0;
    if (max_bytes > room)
        max_bytes = room;

    const u8 *p = (const u8 *)addr;
    u32 n = 0;

    while (n + unit <= max_bytes) {
        if (p[n] == 0 && (unit == 1 || p[n + 1] == 0))
            break;
        n += unit;
    }

    return n;
}

static u32 clamp_max(int max, u32 unit)
{
    if (max <= 0 || (u32)max * unit > TH_MAX_TEXT)
        return TH_MAX_TEXT / unit * unit;
    return (u32)max * unit;
}

void th_emit_utf16(th_ctx *c, u32 addr, int max)
{
    u32 len = scan_len(addr, 2, clamp_max(max, 2));

    if (len)
        push(c, THK_ENC_UTF16LE, (const void *)addr, len);
}

void th_emit_utf16_len(th_ctx *c, u32 addr, int units)
{
    u32 len = clamp_max(units, 2);

    if (units > 0 && room_at(addr) >= len)
        push(c, THK_ENC_UTF16LE, (const void *)addr, len);
}

void th_emit_utf8(th_ctx *c, u32 addr, int max)
{
    u32 len = scan_len(addr, 1, clamp_max(max, 1));

    if (len)
        push(c, THK_ENC_UTF8, (const void *)addr, len);
}

void th_emit_sjis(th_ctx *c, u32 addr, int max)
{
    u32 len = scan_len(addr, 1, clamp_max(max, 1));

    if (len)
        push(c, THK_ENC_SJIS, (const void *)addr, len);
}

void th_emit_text(th_ctx *c, const char *utf8, int len)
{
    if (len < 0)
        len = strlen(utf8);
    if (len)
        push(c, THK_ENC_UTF8, utf8, len);
}

/* --- the hook entry ------------------------------------------------------ */

/*
 * The syscall from the entry routine, on the game's thread, with the
 * game's registers in ctx (on the game's stack).
 */
int thkHookEntry(u32 index, void *ctx)
{
    th_ctx *c = ctx;

    if (!game || index >= (u32)game->nhooks || !user_buffer(c, sizeof(*c)))
        return 0;

    c->hook = index;
    game->hooks[index].fn(c);
    return 0;
}

/* --- installing ---------------------------------------------------------- */

static const th_game *find_game(const char *id)
{
    for (int i = 0; th_games[i]; i++)
        for (const char *const *p = th_games[i]->ids; *p; p++)
            if (!strcmp(*p, id))
                return th_games[i];
    return NULL;
}

/*
 * Copies the entry routine and builds the thunks in a fresh block of user
 * memory. Returns the block's address, or 0.
 */
static u32 *build_code(const th_game *g)
{
    int syscall = sctrlKernelQuerySystemCall((void *)thkHookEntry);

    if (syscall < 0) {
        klog("capture: no syscall for thkHookEntry (%08x); no text capture", syscall);
        return NULL;
    }

    int entry_words = thk_entry_end - thk_entry_start;
    u32 size        = (entry_words + g->nhooks * THUNK_WORDS) * 4;

    SceUID block = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "thk_hooks",
                                                 PSP_SMEM_High, size, NULL);
    if (block < 0) {
        klog("capture: no user memory for the hook code (%lu bytes, %08x)", size, block);
        return NULL;
    }

    u32 *code  = sceKernelGetBlockHeadAddr(block);
    u32 *entry = code;

    memcpy(entry, thk_entry_start, entry_words * 4);
    entry[thk_entry_syscall - thk_entry_start] = (u32)syscall << 6 | 0x0C;

    for (int i = 0; i < g->nhooks; i++) {
        u32 *t = code + entry_words + i * THUNK_WORDS;

        t[0] = 0x27BD0000 | (u16)-FRAME_SIZE;      /* addiu sp, sp, -FRAME_SIZE */
        t[1] = 0xAFA10000 | R(1);                  /* sw    at, R(1)(sp)       */
        t[2] = 0xAFBF0000 | R(31);                 /* sw    ra, R(31)(sp)      */
        t[3] = 0x3C010000;                         /* lui   at, 0              */
        t[4] = mips_jump(MIPS_JAL, (u32)entry);    /* jal   entry              */
        t[5] = 0x34210000 | i;                     /* ori   at, at, index      */
        t[6] = 0x8FBF0000 | R(31);                 /* lw    ra, R(31)(sp)      */
        t[7] = 0x8FA10000 | R(1);                  /* lw    at, R(1)(sp)       */
        t[8] = 0x27BD0000 | FRAME_SIZE;            /* addiu sp, sp, FRAME_SIZE */
    }
    sctrlFlushCache();

    klog("capture: hook code at %p, %lu bytes (syscall %x)", code, size, syscall);
    return code;
}

/*
 * The manager calls this every tick once the game has started, until it
 * returns 1: finds the game's script and installs its hooks. The title ID
 * can take a moment to appear, so it retries for a while.
 */
int capture_start(void)
{
    static int tries;
    char       id[16];

    if (title_id(id, sizeof(id)) < 0) {
        if (++tries < TITLE_TRIES)
            return 0;
        klog("capture: no title id after 30 s");
        return 1;
    }

    const th_game *g = find_game(id);
    if (!g) {
        klog("capture: %s has no hook script", id);
        return 1;
    }
    if (g->nhooks > MAX_HOOKS) {
        klog("capture: %s has %d hooks, only %d supported", g->title, g->nhooks, MAX_HOOKS);
        return 1;
    }
    klog("capture: %s: %s, %d hook(s)", id, g->title, g->nhooks);

    u32 *code = build_code(g);
    if (!code)
        return 1;

    int entry_words = thk_entry_end - thk_entry_start;

    game = g;
    for (int i = 0; i < g->nhooks; i++) {
        const th_hook *h = &g->hooks[i];
        u32           *t = code + entry_words + i * THUNK_WORDS;
        int           r  = hook_install(h->addr, (u32)t, t + THUNK_TAIL, h->expect);

        klog("hook %d %s @ %08lx: %s (%08x)", i, h->name, h->addr,
             r == THK_OK ? "installed" : r == THK_PENDING ? "pending" : "FAILED", r);
        if (r != THK_OK && r != THK_PENDING) {
            char msg[THK_OVERLAY_MAX_TEXT];

            mini_snprintf(msg, sizeof(msg), "hook %08lx failed (%08x)", h->addr, r);
            overlay_message(msg, 10000);
        }
    }
    return 1;
}
