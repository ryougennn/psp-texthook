/*
 * thkcore.prx: the kernel plugin, and the only module PLUGINS.TXT lists.
 *
 * It captures the text and does everything that needs kernel privileges:
 *
 *   main.c      logging, the title ID, the network modules, and the manager
 *               that loads the user modules into the volatile memory and
 *               gives it back for sleep and quitting
 *   hook.c      patching the game's code
 *   capture.c   running the game's hook script and keeping what it emits
 *   overlay.c   on-screen messages, and the popup's frames on screen
 *   popup.c     the buttons: L+R, and keeping them from the game
 *
 * It lives in kernel memory, so games keep all of their user memory but
 * for the few hundred bytes of hook code. The user modules run from the
 * 4 MB volatile memory (partition 5), loaded only when asked: texthook.prx
 * (the network) while the Wi-Fi switch is on, thkpopup.prx (the dictionary)
 * while L+R's popup is open.
 *
 * Never call into the module manager (load/start/stop) from the
 * module-start handler: the manager is busy starting that module and the
 * call deadlocks (black screen on boot, seen with CLANNAD).
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <pspintrman.h>
#include <psploadcore.h>
#include <pspmodulemgr.h>
#include <pspsysmem.h>
#include <pspsysmem_kernel.h>
#include <pspthreadman_kernel.h>
#include <systemctrl.h>

#include "core.h"
#include "mini.h"

PSP_MODULE_INFO("thkcore", 0x1007, 1, 0);

#define LOG_PATH            "ms0:/SEPLUGINS/texthook/log.txt"
#define USER_MODULE_PATH    "ms0:/SEPLUGINS/texthook/texthook.prx"
#define POPUP_MODULE_PATH   "ms0:/SEPLUGINS/texthook/thkpopup.prx"

/* Firmware calls the SDK has no prototypes for. */
int sceWlanGetSwitchState(void);
int sceKernelVolatileMemTryLock(int unk, void **ptr, int *size);
int sceKernelVolatileMemUnlock(int unk);
int scePowerRegisterCallback(int slot, SceUID cbid);
int scePowerUnregisterCallback(int slot);

/* --- logging ------------------------------------------------------------- */

void klog(const char *fmt, ...)
{
    char    buf[256];
    va_list ap;

    va_start(ap, fmt);
    u32 ms = sceKernelGetSystemTimeLow() / 1000;
    int n  = mini_snprintf(buf, sizeof(buf), "%5lu.%03lu [core] ", ms / 1000, ms % 1000);
    n     += mini_vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);

    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n++] = '\n';

    sceIoWrite(1, buf, n);

#ifdef TH_LOG_FILE
    /*
     * The core loads once per game boot and logs before anything else, so its
     * first write starts a fresh log; everything after appends. A crash drops
     * back to the XMB, where nothing loads, so its log survives to be read.
     */
    static int started;

    SceUID fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT |
                          (started ? PSP_O_APPEND : PSP_O_TRUNC), 0777);
    if (fd >= 0) {
        started = 1;
        sceIoWrite(fd, buf, n);
        sceIoClose(fd);
    }
#endif
}

/* --- watching modules start ---------------------------------------------- */

static u32 user_free_kb(void)
{
    return sceKernelPartitionTotalFreeMemSize(PSP_MEMORY_PARTITION_USER) / 1024;
}

static u32 user_largest_kb(void)
{
    return sceKernelPartitionMaxFreeMemSize(PSP_MEMORY_PARTITION_USER) / 1024;
}

static STMOD_HANDLER previous_handler;
static volatile u32  game_started_at;

static int on_module_start(SceModule *mod)
{
    u32 start = mod->text_addr, end = start + mod->text_size;

    /*
     * The module owns all its segments, not just the first: a hook may be
     * in a segment some executables load separately after the code.
     */
    for (int s = 0; s < mod->nsegment && s < 4; s++)
        if (mod->segmentaddr[s] + mod->segmentsize[s] > end)
            end = mod->segmentaddr[s] + mod->segmentsize[s];

    /*
     * With the user partition's free memory as it was before this module
     * started: a game that boots without the plugin but hangs with it is
     * usually one that wanted the memory the plugin took.
     */
    if (user_range(start, 0))
        klog("user module starting: %s at %08lx-%08lx, free %lu KB (largest %lu KB)",
             mod->modname, start, end, user_free_kb(), user_largest_kb());

    /*
     * The game's executable: the first user module at the usual load
     * address or above (sceKernelLibrary sits below it, and our own modules
     * load into volatile memory, lower still).
     */
    if (!game_started_at && start >= 0x08804000 && start < THK_USER_RAM_END)
        game_started_at = sceKernelGetSystemTimeLow() | 1;

    hook_module_started(mod->modname, start, end);
    return previous_handler ? previous_handler(mod) : 0;
}

/* --- the title ID -------------------------------------------------------- */

/* "ULJM-05338" or "ULJM05338" -> "ULJM05338". Returns length. */
static int normalize_title(const char *in, int inlen, char *out, int size)
{
    int n = 0;

    for (int i = 0; i < inlen && in[i] && in[i] != '|' && n + 1 < size; i++)
        if (in[i] != '-')
            out[n++] = in[i];
    out[n] = 0;
    return n;
}

int title_id(char *out, int size)
{
    char id[16];
    int  ret = -1;

    /*
     * The kernel's game info is the only source: it has answered on every
     * game so far, and the others (init PARAM.SFO, UMD_DATA.BIN) never did.
     */
    SceGameInfo *info = sceKernelGetGameInfo();
    if (info && normalize_title(info->title_id, sizeof(info->title_id), id, sizeof(id))) {
        ret = 0;
    } else {
        static int logged;      /* capture polls; log the failure once */

        if (!logged++)
            klog("no title id yet: game info %p", info);
    }

    if (ret == 0)
        strncpy0(out, id, size);
    return ret;
}

int thkGetTitleId(char *out, int size)
{
    int  k1 = pspSdkSetK1(0);
    char id[16];
    int  ret = title_id(id, sizeof(id));

    if (ret == 0 && size > 0 && user_range((u32)out, size))
        strncpy0(out, id, size);
    else if (ret == 0)
        ret = THK_ERR_ARGS;

    pspSdkSetK1(k1);
    return ret;
}

/* --- network modules ----------------------------------------------------- */

/*
 * The user module runs Wi-Fi from the 4 MB volatile memory (partition 5),
 * so a game keeps all of its user memory. sceUtilityLoadNetModule always
 * loads into the user partition, so the core loads the same files itself,
 * with a partition option only kernel callers get. ifhandle is a kernel
 * module (kernel memory); it loads once and stays, the others come and go
 * with the Wi-Fi switch.
 */

/*
 * Their module names, to find one left loaded (e.g. by a load that was
 * cut short) and unload it before loading it again: a second copy is
 * refused (80020139) until the game is restarted.
 */
static const char *const net_names[] = {
    "sceNet_Service",
    "sceNet_Library",
    "sceNetInet_Library",
    "sceNetApctl_Library",
    "sceNetResolver_Library",
};

/*
 * Loading and unloading go through the module manager, and so does
 * stopping texthook.prx, whose module_stop waits for the worker: if a stop
 * started while the worker was loading, each waited for the other and the
 * worker was terminated mid-load, leaving pspnet_inet loaded for good.
 * So stop_texthook raises net_stopping and waits for net_busy to clear;
 * a load claims net_busy only while no stop is pending, and gives up
 * between modules once one is.
 */
static volatile int net_busy, net_stopping;

static int net_claim(void)
{
    int intr = sceKernelCpuSuspendIntr();
    int ok = !net_stopping;

    if (ok)
        net_busy = 1;
    sceKernelCpuResumeIntr(intr);
    return ok;
}

static const char *const net_paths[] = {
    "flash0:/kd/ifhandle.prx",
    "flash0:/kd/pspnet.prx",
    "flash0:/kd/pspnet_inet.prx",
    "flash0:/kd/pspnet_apctl.prx",
    "flash0:/kd/pspnet_resolver.prx",
};

#define NET_MODULES (int)(sizeof(net_paths) / sizeof(net_paths[0]))

static SceUID net_ids[NET_MODULES];

static void net_unload(void)
{
    for (int i = NET_MODULES - 1; i >= 1; i--) {
        if (net_ids[i] <= 0)
            continue;

        int status;
        int r = sceKernelStopModule(net_ids[i], 0, NULL, &status, NULL);
        int u = sceKernelUnloadModule(net_ids[i]);

        if (r < 0 || u < 0)
            klog("net: unloading %s: stop %08x, unload %08x", net_paths[i], r, u);
        net_ids[i] = 0;
    }
}

int thkNetModules(int partition)
{
    int k1 = pspSdkSetK1(0);
    int ret = 0;

    if (partition != 0 && partition != 5) {
        ret = THK_ERR_ARGS;
    } else if (!net_claim()) {
        ret = THK_ERR_STOPPING;     /* the core unloads them once the module is gone */
    } else if (partition == 0) {
        net_unload();
        klog("net: modules unloaded");
        net_busy = 0;
    } else {
        SceKernelLMOption opt;

        memset(&opt, 0, sizeof(opt));
        opt.size     = sizeof(opt);
        opt.mpidtext = partition;
        opt.mpiddata = partition;
        opt.position = PSP_SMEM_Low;
        opt.access   = 1;

        for (int i = 0; i < NET_MODULES && ret >= 0; i++) {
            if (net_ids[i] > 0)
                continue;

            if (net_stopping) {
                klog("net: texthook.prx is being stopped; loading abandoned");
                ret = THK_ERR_STOPPING;
                break;
            }

            /* A copy left loaded by an earlier start: reuse ifhandle, unload the rest. */
            SceModule *old = sceKernelFindModuleByName(net_names[i]);
            if (old && i == 0) {
                net_ids[0] = old->modid;    /* ifhandle stays loaded anyway */
                continue;
            }
            if (old) {
                int status;
                int r = sceKernelStopModule(old->modid, 0, NULL, &status, NULL);
                int u = sceKernelUnloadModule(old->modid);

                klog("net: %s was left loaded; unloaded it first (stop %08x, unload %08x)",
                     net_names[i], r, u);
            }

            SceUID id = sceKernelLoadModule(net_paths[i], 0, i == 0 ? NULL : &opt);
            if (id < 0) {
                klog("net: loading %s failed (%08x)", net_paths[i], id);
                ret = id;
                break;
            }

            int status;
            int r = sceKernelStartModule(id, 0, NULL, &status, NULL);
            if (r < 0) {
                klog("net: starting %s failed (%08x)", net_paths[i], r);
                sceKernelUnloadModule(id);
                ret = r;
                break;
            }
            net_ids[i] = id;

            SceModule *m = sceKernelFindModuleByUID(id);
            if (m)
                klog("net: %s at %08lx", m->modname, (u32)m->text_addr);
        }

        if (ret < 0)
            net_unload();
        klog("net: partition %d now free %lu KB (largest %lu KB)", partition,
             (u32)sceKernelPartitionTotalFreeMemSize(partition) / 1024,
             (u32)sceKernelPartitionMaxFreeMemSize(partition) / 1024);
        net_busy = 0;
    }

    pspSdkSetK1(k1);
    return ret;
}

int thkMemInfo(int partition, u32 *out)
{
    int k1  = pspSdkSetK1(0);
    int ret = THK_ERR_ARGS;

    if ((partition == PSP_MEMORY_PARTITION_USER || partition == 5) &&
        user_range((u32)out, 2 * sizeof(u32)) && !((u32)out & 3)) {
        out[0] = sceKernelPartitionTotalFreeMemSize(partition);
        out[1] = sceKernelPartitionMaxFreeMemSize(partition);
        ret = THK_OK;
    }

    pspSdkSetK1(k1);
    return ret;
}

/* --- the manager: the user modules in the volatile memory ---------------- */

/*
 * The system needs the volatile memory to boot a game, sleep, quit and show
 * its save/load screens, and simply waits while anyone else holds it
 * (sleep stalled, quitting hung, CLANNAD black-screened). So the plugin
 * holds it only while the user asks for something that runs there:
 *
 *   - texthook.prx (the network) while the Wi-Fi switch is on. A switch
 *     already on when the game boots is ignored until it's flipped off and
 *     on, so the memory is never taken while the game is still loading, and
 *     it's stopped while the system needs the memory (see the dialogs
 *     section);
 *   - thkpopup.prx (the dictionary) while L+R's popup is open. If the
 *     system has the memory (a save screen), the popup says so instead.
 *
 * The memory is released when neither is loaded. Sleep and quitting
 * release it too, whatever the switch says; after waking, the switch
 * decides again.
 *
 * Everything happens on the manager thread; sleep, quit and L+R only post
 * a request. Quitting waits until the manager has released everything and
 * stopped for good.
 */
enum { M_BOOT, M_WAIT_FLIP, M_OFF, M_RUNNING, M_SLEEP };
enum { REQ_NONE, REQ_SLEEP, REQ_QUIT, REQ_DIALOG };

#define GAME_SETTLE     (2 * 1000 * 1000)   /* after the game's module starts */
#define WAKE_SETTLE     (3 * 1000 * 1000)
#define START_RETRY     (5 * 1000 * 1000)
#define DIALOG_GRACE    (3 * 1000 * 1000)

/* When the game last opened a system dialog. */
static volatile u32 dialog_at;
#define EXIT_WAIT_MS    3000

#define NID_sceKernelLoadExec           0xBD2F1094
#define NID_sceKernelExitGame           0x05572A5F
#define NID_sceKernelExitGameWithStatus 0x2AC9954B

#define POWER_SUSPENDING        0x00010000
#define POWER_RESUME_COMPLETE   0x00040000

static volatile int    release_req, woke, manager_done;
static int             power_slot = -1;
static int             vmem_held;
static volatile SceUID texthook_id, popup_id;

/* --- the volatile memory, and loading into it ---------------------------- */

static int vmem_take(void)
{
    if (vmem_held)
        return 0;

    void *base = NULL;
    int   size = 0;
    int   r    = sceKernelVolatileMemTryLock(0, &base, &size);

    if (r < 0)
        return r;
    vmem_held = 1;
    klog("volatile memory locked, %d KB at %p", size / 1024, base);
    return 0;
}

/* Releases it once nothing of ours is loaded there. */
static void vmem_drop(const char *why)
{
    if (!vmem_held || texthook_id > 0 || popup_id > 0)
        return;

    int r = sceKernelVolatileMemUnlock(0);

    vmem_held = 0;
    klog("%s: volatile memory released (%08x)", why, r);
}

/* Smaller start/stop stacks outside the volatile memory. */
static SceKernelSMOption *small_stack(SceKernelSMOption *sm, int partition)
{
    if (partition == 5)
        return NULL;

    memset(sm, 0, sizeof(*sm));
    sm->size      = sizeof(*sm);
    sm->mpidstack = partition;
    sm->stacksize = 0x4000;
    sm->priority  = 0x20;
    return sm;
}

/* Loads and starts a user module in `partition`. */
static SceUID load_module(const char *path, int partition)
{
    SceKernelLMOption opt;

    memset(&opt, 0, sizeof(opt));
    opt.size     = sizeof(opt);
    opt.mpidtext = partition;
    opt.mpiddata = partition;
    opt.position = PSP_SMEM_Low;
    opt.access   = 1;

    SceUID id = sceKernelLoadModule(path, 0, &opt);
    if (id < 0) {
        klog("loading %s failed (%08x)", path, id);
        return id;
    }

    SceKernelSMOption sm;
    int status;
    int r = sceKernelStartModule(id, 0, NULL, &status, small_stack(&sm, partition));
    if (r < 0) {
        klog("starting %s failed (%08x)", path, r);
        sceKernelUnloadModule(id);
        return r;
    }
    return id;
}

static void unload_module(SceUID id, const char *name, int partition)
{
    SceKernelSMOption sm;
    int status;
    int r = sceKernelStopModule(id, 0, NULL, &status, small_stack(&sm, partition));
    int u = sceKernelUnloadModule(id);

    if (r < 0 || u < 0)
        klog("%s: stop %08x, unload %08x", name, r, u);
}

/* --- texthook.prx: the network ------------------------------------------- */

static int start_texthook(void)
{
    static int waiting;     /* logged once per wait */
    int r = vmem_take();

    if (r < 0) {
        if (!waiting)
            klog("volatile memory is in use (%08x); retrying every 5 s", r);
        waiting = 1;
        return r;
    }
    waiting = 0;

    SceUID id = load_module(USER_MODULE_PATH, 5);
    if (id < 0) {
        vmem_drop("texthook.prx didn't start");
        return id;
    }
    texthook_id = id;
    return 0;
}

/* texthook.prx's next event, saved as it stopped (thkEventSaveNext). */
static u32 saved_next;
static int resume_after_dialog;

void thkEventSaveNext(u32 seq)
{
    saved_next = seq;
}

u32 thkEventResumeNext(void)
{
    u32 seq = resume_after_dialog ? saved_next : 0;

    resume_after_dialog = 0;
    return seq;
}

static void stop_texthook(const char *why)
{
    resume_after_dialog = 0;
    if (texthook_id <= 0)
        return;
    klog("%s: stopping texthook.prx", why);

    /* Let a network load in progress finish first (see net_busy). */
    net_stopping = 1;
    for (int ms = 0; net_busy && ms < 5000; ms += 10)
        sceKernelDelayThread(10 * 1000);
    if (net_busy)
        klog("%s: network modules still loading after 5 s; stopping anyway", why);

    unload_module(texthook_id, "texthook.prx", 5);
    net_unload();   /* texthook.prx can't: it's stopped from inside the module manager */
    net_stopping = 0;
    texthook_id  = 0;
    vmem_drop(why);
}

/* --- the popup's buffers ------------------------------------------------- */

/*
 * The popup's buffers come from the core (thkPopupAlloc): the volatile
 * memory, or, in games that keep it, the extra RAM of the PSP-2000 and
 * later, with the popup itself in the game's memory.
 */
#define XMEM_PARTITION      9
#define XMEM_SIZE           (3 << 20)
#define XMEM_CODE_ROOM      (112 * 1024)    /* the popup, and room left for the game */
#define POPUP_BLOCKS        8

static SceUID popup_blocks[POPUP_BLOCKS];   /* in the volatile memory */
static SceUID xmem_block = -1;
static u32    xmem_used;
static int    popup_partition = 5;          /* where thkpopup.prx and its stack go */
volatile u32  xmem_lo, xmem_hi;

static int xmem_open(void)
{
    if (sceKernelGetModel() == 0)
        return -1;      /* the PSP-1000 has no extra RAM */

    u32 room = sceKernelPartitionMaxFreeMemSize(PSP_MEMORY_PARTITION_USER);
    if (room < XMEM_CODE_ROOM) {
        klog("popup: only %lu KB left in the game's memory", room / 1024);
        return -1;
    }

    SceUID b = sceKernelAllocPartitionMemory(XMEM_PARTITION, "thk_xmem", PSP_SMEM_High,
                                             XMEM_SIZE + 64, NULL);
    if (b < 0) {
        klog("popup: no extra RAM (%08x)", b);
        return b;
    }

    u32 lo = ((u32)sceKernelGetBlockHeadAddr(b) + 63) & ~63u;

    xmem_block = b;
    xmem_used  = 0;
    xmem_lo    = lo;
    xmem_hi    = lo + XMEM_SIZE;
    popup_partition = PSP_MEMORY_PARTITION_USER;
    klog("popup: buffers in the extra RAM, %08lx-%08lx; code in the game's memory (%lu KB free)",
         lo, lo + XMEM_SIZE, room / 1024);
    return 0;
}

static void xmem_close(void)
{
    if (xmem_block < 0)
        return;

    xmem_lo = xmem_hi = 0;
    sceKernelFreePartitionMemory(xmem_block);
    xmem_block = -1;
    popup_partition = 5;
}

int thkPopupAlloc(u32 size, void **addr)
{
    int k1  = pspSdkSetK1(0);
    int ret = THK_ERR_ARGS;

    if (!user_range((u32)addr, sizeof(*addr)) || ((u32)addr & 3) || !size) {
        /* refused */
    } else if (xmem_block >= 0) {
        /* used up in order; freed when the popup closes */
        size = (size + 63) & ~63u;
        if (size > XMEM_SIZE - xmem_used) {
            klog("popup: extra RAM full (%lu + %lu KB)", xmem_used / 1024, size / 1024);
            ret = SCE_KERNEL_ERROR_NO_MEMORY;
        } else {
            *addr      = (void *)(xmem_lo + xmem_used);
            xmem_used += size;
            ret        = 1;
        }
    } else {
        int i = 0;

        while (i < POPUP_BLOCKS && popup_blocks[i] > 0)
            i++;
        if (i == POPUP_BLOCKS) {
            ret = SCE_KERNEL_ERROR_NO_MEMORY;
        } else {
            SceUID b = sceKernelAllocPartitionMemory(5, "thk_popup", PSP_SMEM_High, size, NULL);

            if (b >= 0) {
                popup_blocks[i] = b;
                *addr = sceKernelGetBlockHeadAddr(b);
            }
            ret = b;
        }
    }

    pspSdkSetK1(k1);
    return ret;
}

int thkPopupPartition(void)
{
    return popup_partition;
}

int thkPopupFree(int id)
{
    int k1 = pspSdkSetK1(0);

    for (int i = 0; id > 0 && i < POPUP_BLOCKS; i++)
        if (popup_blocks[i] == id) {
            sceKernelFreePartitionMemory(id);
            popup_blocks[i] = 0;
        }

    pspSdkSetK1(k1);
    return 0;
}

/* After the popup is unloaded: what it left, and the extra RAM. */
static void popup_free_all(void)
{
    for (int i = 0; i < POPUP_BLOCKS; i++)
        if (popup_blocks[i] > 0) {
            sceKernelFreePartitionMemory(popup_blocks[i]);
            popup_blocks[i] = 0;
        }
    xmem_close();
}

/* --- thkpopup.prx: the dictionary ---------------------------------------- */

static void start_popup(int state)
{
    if (state == M_BOOT || state == M_SLEEP) {
        popup_reset();
        return;
    }

    int r = vmem_take();
    if (r < 0 && xmem_open() < 0) {
        klog("popup: the system is using the volatile memory (%08x)", r);
        overlay_message("Dictionary: not now (the system is busy)", 3000);
        popup_reset();
        return;
    }

    SceUID id = load_module(POPUP_MODULE_PATH, popup_partition);
    if (id < 0) {
        overlay_message("Dictionary: thkpopup.prx didn't load", 3000);
        popup_reset();
        popup_free_all();
        vmem_drop("popup didn't start");
        return;
    }
    popup_id = id;
}

/* The popup closed itself (thkPopupShow(NULL)), or must close now. */
static void stop_popup(const char *why)
{
    if (popup_id <= 0)
        return;

    klog("%s: unloading thkpopup.prx", why);
    unload_module(popup_id, "thkpopup.prx", popup_partition);  /* module_stop waits for its thread */
    popup_id = 0;
    popup_reset();
    popup_free_all();
    klog("%s: thkpopup.prx unloaded", why);
    vmem_drop(why);
}

/* Threads waiting for the volatile memory. */
static int vmem_waiters(void)
{
    static SceUID sema = -1;
    SceKernelSemaInfo info;

    if (sema < 0) {
        static SceUID ids[128];
        int n = 0;

        if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Semaphore, ids, 128, &n) < 0)
            return 0;
        for (int i = 0; i < n && i < 128 && sema < 0; i++) {
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (sceKernelReferSemaStatus(ids[i], &info) >= 0 && !strcmp(info.name, "ScePowerVmem"))
                sema = ids[i];
        }
        if (sema < 0)
            return 0;
    }
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelReferSemaStatus(sema, &info) < 0) {
        sema = -1;
        return 0;
    }
    return info.numWaitThreads;
}

static int manager(SceSize args, void *argp)
{
    int state    = M_BOOT;
    int captured = 0;
    u32 woke_at  = 0, retry_at = 0;

    for (;;) {
        sceKernelDelayThread(50 * 1000);
        u32 now = sceKernelGetSystemTimeLow();

        /* Sleep and quit come first, whatever the state. */
        int req = release_req;
        if (!req && texthook_id > 0 && vmem_waiters() > 0) {
            klog("a system task is waiting for the volatile memory");
            dialog_at = now | 1;
            req = REQ_DIALOG;
        }
        if (req == REQ_DIALOG) {
            release_req = REQ_NONE;
            stop_texthook("the system needs the volatile memory");
            resume_after_dialog = 1;
            if (state == M_RUNNING) {
                overlay_message("Wi-Fi: paused for a system screen", 3000);
                state    = M_OFF;
                retry_at = now | 1;     /* back when the dialog has let go */
            }
            continue;
        }
        if (req) {
            release_req = REQ_NONE;
            stop_popup(req == REQ_QUIT ? "quitting" : "going to sleep");
            stop_texthook(req == REQ_QUIT ? "quitting" : "going to sleep");
            if (req == REQ_QUIT) {
                /*
                 * The game is going away: stop touching the system (the Wi-Fi
                 * driver is shut down during the quit, and this loop reads
                 * its switch every 50 ms), then let the quit go ahead.
                 */
                if (power_slot >= 0)
                    scePowerUnregisterCallback(power_slot);
                klog("manager stopped");
                manager_done = 1;
                return 0;
            }
            state = M_SLEEP;
            continue;
        }

        /* capture, once the game is under way */
        if (!captured && game_started_at && (u32)(now - game_started_at) >= GAME_SETTLE)
            captured = capture_start();

        /* the popup: L+R opens it, it closes itself */
        if (popup_finished())
            stop_popup("popup closed");
        if (popup_requested() && popup_id <= 0)
            start_popup(state);

        int sw = sceWlanGetSwitchState() > 0;

        switch (state) {
        case M_BOOT:
            if (!game_started_at || (u32)(now - game_started_at) < GAME_SETTLE)
                break;
            if (sw) {
                klog("Wi-Fi switch is on at boot; waiting for it to be flipped off and on");
                overlay_message("Wi-Fi: flip the switch off and on", 10000);
                state = M_WAIT_FLIP;
            } else {
                klog("Wi-Fi switch is off; waiting for it to be turned on");
                state = M_OFF;
            }
            break;

        case M_WAIT_FLIP:
            if (!sw) {
                klog("Wi-Fi switch turned off");
                state = M_OFF;
            }
            break;

        case M_OFF:
            if (!sw)
                resume_after_dialog = 0;    /* switched off meanwhile: start fresh */
            if (sw && (!retry_at || (u32)(now - retry_at) > START_RETRY) &&
                (!dialog_at || (u32)(now - dialog_at) > DIALOG_GRACE) && !vmem_waiters()) {
                if (!retry_at)
                    klog("Wi-Fi switch turned on");
                retry_at = 0;
                if (start_texthook() == 0)
                    state = M_RUNNING;
                else
                    retry_at = now | 1;
            }
            break;

        case M_RUNNING:
            if (!sw) {
                stop_texthook("Wi-Fi switch turned off");
                overlay_message("Wi-Fi off", 3000);
                state = M_OFF;
            }
            break;

        case M_SLEEP:
            if (woke) {
                woke = 0;
                woke_at = now | 1;
                klog("woke up");
            }
            if (woke_at && (u32)(now - woke_at) > WAKE_SETTLE) {
                woke_at = 0;
                state = M_OFF;  /* starts again if the switch is on */
            }
            break;
        }
    }
    return 0;
}

/* --- sleep --------------------------------------------------------------- */

/*
 * Sleep waits for the volatile memory, so "suspending" only posts the
 * request; the manager releases it and the sleep goes ahead.
 */
static int power_cb(int unk, int flags, void *arg)
{
    if (flags & POWER_SUSPENDING)
        release_req = REQ_SLEEP;
    if (flags & POWER_RESUME_COMPLETE)
        woke = 1;
    return 0;
}

static int power_thread(SceSize args, void *argp)
{
    SceUID cb = sceKernelCreateCallback("thkcore_power", power_cb, NULL);

    /*
     * "Any free slot" (-1) is refused from a kernel thread (80000004), so
     * try the slots from the top: games tend to use the low ones.
     */
    int r = scePowerRegisterCallback(-1, cb), slot = -1;
    for (int s = 15; r < 0 && s >= 0; s--)
        if ((r = scePowerRegisterCallback(s, cb)) >= 0)
            slot = s;
    power_slot = slot >= 0 ? slot : r;  /* r: the slot -1 got, if it worked */

    if (r < 0)
        klog("power callback not registered (%08x); sleep will wait for the Wi-Fi switch", r);
    else
        klog("power callback in slot %d", slot);

    sceKernelSleepThreadCB();
    return 0;
}

/* --- quitting ------------------------------------------------------------ */

/*
 * Quitting (HOME -> quit) or launching another executable needs the
 * volatile memory too. The core takes over the game's calls to leave and
 * waits, up to 3 s, for the manager to release it first. This runs inside
 * the game's syscall, where k1 still marks the caller as user mode: file
 * calls would refuse klog's kernel buffers, so k1 is switched for the wait
 * and restored before the original call checks the game's arguments.
 * The wait runs on a kernel stack of its own.
 */
#define EXIT_STACK_SIZE 0x4000
static int  (*orig_load_exec)(const char *file, void *param);
static void (*orig_exit_game)(void);
static void (*orig_exit_game_status)(int status);

static void release_on_own_stack(void *arg)
{
    const char *how = arg;

    if (!manager_done) {
        if (vmem_held)
            klog("%s: releasing the volatile memory first", how);
        release_req = REQ_QUIT;
        for (int ms = 0; !manager_done && ms < EXIT_WAIT_MS; ms += 20)
            sceKernelDelayThread(20 * 1000);
        if (!manager_done)
            klog("%s: manager still busy after %d ms; continuing anyway", how, EXIT_WAIT_MS);
    }

    klog("%s: passing the call on", how);
}

static void release_before_exit(const char *how)
{
    int k1 = pspSdkSetK1(0);

    sceKernelExtendKernelStack(EXIT_STACK_SIZE, release_on_own_stack, (void *)how);
    pspSdkSetK1(k1);
}

static int load_exec_hook(const char *file, void *param)
{
    release_before_exit("sceKernelLoadExec");
    return orig_load_exec(file, param);
}

static void exit_game_hook(void)
{
    release_before_exit("sceKernelExitGame");
    orig_exit_game();
}

static void exit_game_status_hook(int status)
{
    release_before_exit("sceKernelExitGameWithStatus");
    orig_exit_game_status(status);
}

static void hook_exit_calls(void)
{
    static const struct {
        u32         nid;
        void        **orig;
        void        *hook;
        const char  *name;
    } calls[] = {
        {
            .nid  = NID_sceKernelLoadExec,
            .orig = (void **)&orig_load_exec,
            .hook = load_exec_hook,
            .name = "sceKernelLoadExec",
        },
        {
            .nid  = NID_sceKernelExitGame,
            .orig = (void **)&orig_exit_game,
            .hook = exit_game_hook,
            .name = "sceKernelExitGame",
        },
        {
            .nid  = NID_sceKernelExitGameWithStatus,
            .orig = (void **)&orig_exit_game_status,
            .hook = exit_game_status_hook,
            .name = "sceKernelExitGameWithStatus",
        },
    };

    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        u32 f = sctrlHENFindFunction("sceLoadExec", "LoadExecForUser", calls[i].nid);

        if (!f) {
            klog("%s not found; quitting with Wi-Fi on will hang", calls[i].name);
            continue;
        }
        *calls[i].orig = (void *)f;
        sctrlHENPatchSyscall((void *)f, calls[i].hook);
    }

    sctrlFlushCache();
}

/* --- dialogs ------------------------------------------------------------- */

/*
 * The system's dialogs need the volatile memory, so the core stops
 * texthook.prx when the game starts one; the manager starts it again
 * afterwards.
 */
#define NID_sceUtilitySavedataInitStart     0x50C4CD57
#define NID_sceUtilityMsgDialogInitStart    0x2AD8E239
#define NID_sceUtilityOskInitStart          0xF6269B82
#define NID_sceUtilityNetconfInitStart      0x4DB1E739
#define NID_sceUtilityGameSharingInitStart  0xC492F751
#define NID_sceUtilityScreenshotInitStart   0x0251B134
#define NID_sceUtilityHtmlViewerInitStart   0xCDC3AA41
#define NID_sceUtilityInstallInitStart      0x1281DA8E
#define NID_sceUtilityGamedataInstallInitStart 0x32E32DCB
#define DIALOG_WAIT_MS  6000

static void dialog_on_own_stack(void *arg)
{
    if (manager_done || texthook_id <= 0 || release_req)
        return;
    klog("%s: the game wants the volatile memory", (const char *)arg);
    release_req = REQ_DIALOG;
    for (int ms = 0; texthook_id > 0 && ms < DIALOG_WAIT_MS; ms += 20)
        sceKernelDelayThread(20 * 1000);
    if (texthook_id > 0)
        klog("%s: texthook.prx still loaded after %d ms; continuing anyway",
             (const char *)arg, DIALOG_WAIT_MS);
}

static void release_before_dialog(const char *name)
{
    int k1 = pspSdkSetK1(0);

    dialog_at = sceKernelGetSystemTimeLow() | 1;
    if (texthook_id > 0)
        sceKernelExtendKernelStack(EXIT_STACK_SIZE, dialog_on_own_stack, (void *)name);
    pspSdkSetK1(k1);
}

#define DIALOG_HOOK(fn)                                         \
    static int (*orig_##fn)(void *params);                      \
    static int fn##_hook(void *params)                          \
    {                                                           \
        release_before_dialog(#fn);                             \
        return orig_##fn(params);                               \
    }

DIALOG_HOOK(sceUtilitySavedataInitStart)
DIALOG_HOOK(sceUtilityMsgDialogInitStart)
DIALOG_HOOK(sceUtilityOskInitStart)
DIALOG_HOOK(sceUtilityNetconfInitStart)
DIALOG_HOOK(sceUtilityGameSharingInitStart)
DIALOG_HOOK(sceUtilityScreenshotInitStart)
DIALOG_HOOK(sceUtilityHtmlViewerInitStart)
DIALOG_HOOK(sceUtilityInstallInitStart)
DIALOG_HOOK(sceUtilityGamedataInstallInitStart)

#define DIALOG(fn) { NID_##fn, (void **)&orig_##fn, fn##_hook, #fn }

static void hook_dialog_calls(void)
{
    static const struct {
        u32         nid;
        void        **orig;
        void        *hook;
        const char  *name;
    } calls[] = {
        DIALOG(sceUtilitySavedataInitStart),
        DIALOG(sceUtilityMsgDialogInitStart),
        DIALOG(sceUtilityOskInitStart),
        DIALOG(sceUtilityNetconfInitStart),
        DIALOG(sceUtilityGameSharingInitStart),
        DIALOG(sceUtilityScreenshotInitStart),
        DIALOG(sceUtilityHtmlViewerInitStart),
        DIALOG(sceUtilityInstallInitStart),
        DIALOG(sceUtilityGamedataInstallInitStart),
    };

    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        u32 f = sctrlHENFindFunction("sceUtility_Driver", "sceUtility", calls[i].nid);

        if (!f) {
            klog("%s not found; with Wi-Fi on, its dialog will wait", calls[i].name);
            continue;
        }
        *calls[i].orig = (void *)f;
        sctrlHENPatchSyscall((void *)f, calls[i].hook);
    }

    sctrlFlushCache();
}

/* --- module start -------------------------------------------------------- */

static void start_thread(const char *name, SceKernelThreadEntry fn, int prio)
{
    SceUID th = sceKernelCreateThread(name, fn, prio, 0x1000, 0, NULL);

    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0)
        klog("could not start thread %s (%08x)", name, th);
}

int thkGetApiVersion(void)
{
    return THK_API_VERSION;
}

int module_start(SceSize args, void *argp)
{
    previous_handler = sctrlHENSetStartModuleHandler(on_module_start);
    klog("thkcore up (api %d); kernel memory free %lu KB (largest %lu KB)", THK_API_VERSION,
         (u32)sceKernelPartitionTotalFreeMemSize(PSP_MEMORY_PARTITION_KERNEL) / 1024,
         (u32)sceKernelPartitionMaxFreeMemSize(PSP_MEMORY_PARTITION_KERNEL) / 1024);

    overlay_init();
    popup_init();
    hook_exit_calls();
    hook_dialog_calls();
    start_thread("thkcore_manager", manager, 0x20);
    start_thread("thkcore_power", power_thread, 0x11);
    return 0;
}
