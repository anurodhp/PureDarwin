/*
 * xf86-video-puredarwingop: minimal Xorg video driver for PureDarwin's
 * IOGOPFramebuffer.
 *
 * This is a busless (no PCI/platform bus) framebuffer driver. It uses PDGOP
 * (the userspace IOKit client) to open the IOGOPFramebuffer user client, read
 * the current mode geometry, and map the linear VRAM aperture. That mapping is
 * handed straight to the fb layer via fbScreenInit, so all of X's rendering
 * lands in real GOP video memory - the same buffer the fbtri triangle demo
 * draws into, now driven by a full Xorg server.
 *
 * Modeled on the stock xf86-video-dummy driver (which is likewise busless),
 * with the malloc'd shadow framebuffer replaced by the PDGOP VRAM mapping.
 */
#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86str.h"
#include "xf86Module.h"
#include "fb.h"
#include "damage.h"
#include "damagestr.h"
#include "micmap.h"
#include "mipointer.h"
#include "colormapst.h"
#include "xf86cmap.h"

#include <PDGOP.h>
#include <pthread/qos.h>

#define PDGOP_NAME        "puredarwingop"
#define PDGOP_DRIVER_NAME "puredarwingop"
#define PDGOP_VERSION     1000
#define PDGOP_MAJOR       1
#define PDGOP_MINOR       0
#define PDGOP_PATCH       0

/* Per-screen private state. */
typedef struct {
    PDGOPFramebuffer fb;          /* live IOGOPFramebuffer connection + mapping */
    Bool             fbOpen;
    uint32_t         cacheMode;   /* PDGOP_MAP_* the VRAM is mapped with */
    Bool             useShadow;   /* Option "ShadowFB" */
    int              updateDelay; /* Option "ShadowUpdateDelay", ms */
    Bool             stats;       /* Option "ShadowStats" */
    CARD32           firstDamage; /* when the oldest un-copied damage arrived */
    CARD32           lastDamage;  /* when damage last arrived */
    Bool             pending;     /* damage waiting for the update delay */
    CARD32           statsStart;
    unsigned long    statUpdates, statBoxes, statBytes;
    double           statSeconds;
    void            *shadow;      /* cached-RAM render target, blitted to VRAM */
    DamagePtr        damage;      /* tracks which parts of it need pushing out */
    ScreenBlockHandlerProcPtr BlockHandler;
    CreateScreenResourcesProcPtr CreateScreenResources;
    CloseScreenProcPtr CloseScreen;
    OptionInfoPtr    Options;
} PDGOPRec, *PDGOPPtr;

static PDGOPPtr
PDGOPGetRec(ScrnInfoPtr pScrn)
{
    if (pScrn->driverPrivate == NULL) {
        pScrn->driverPrivate = xnfcalloc(sizeof(PDGOPRec), 1);
    }
    return (PDGOPPtr)pScrn->driverPrivate;
}

static void
PDGOPFreeRec(ScrnInfoPtr pScrn)
{
    PDGOPPtr p = pScrn->driverPrivate;

    if (p == NULL) {
        return;
    }
    if (p->fbOpen) {
        PDGOPClose(&p->fb);
        p->fbOpen = FALSE;
    }
    free(p->Options);
    free(p);
    pScrn->driverPrivate = NULL;
}

/* --- forward decls --- */
static const OptionInfoRec *PDGOPAvailableOptions(int chipid, int busid);
static void PDGOPIdentify(int flags);
static Bool PDGOPProbe(DriverPtr drv, int flags);
static Bool PDGOPPreInit(ScrnInfoPtr pScrn, int flags);
static Bool PDGOPScreenInit(ScreenPtr pScreen, int argc, char **argv);
static Bool PDGOPCreateScreenResources(ScreenPtr pScreen);
static Bool PDGOPEnterVT(ScrnInfoPtr pScrn);
static void PDGOPLeaveVT(ScrnInfoPtr pScrn);
static Bool PDGOPSwitchMode(ScrnInfoPtr pScrn, DisplayModePtr mode);
static void PDGOPAdjustFrame(ScrnInfoPtr pScrn, int x, int y);
static Bool PDGOPCloseScreen(ScreenPtr pScreen);
static ModeStatus PDGOPValidMode(ScrnInfoPtr pScrn, DisplayModePtr mode,
                                 Bool verbose, int flags);

typedef enum {
    OPTION_SHADOW_FB,
    OPTION_VRAM_CACHE_MODE,
    OPTION_SHADOW_UPDATE_DELAY,
    OPTION_SHADOW_STATS,
} PDGOPOpts;

/*
 * "ShadowFB" (bool, default on): render into a cached RAM copy of the
 *     screen and copy damaged areas to VRAM from the block handler. Off
 *     renders straight into the VRAM mapping (every fb read is then an
 *     uncached VRAM read).
 * "VRAMCacheMode" (string, default "writecombine"): cache attribute of the
 *     user VRAM mapping: "writecombine" (Normal non-cacheable, stores are
 *     gathered), "posted" (Device-nGnRE), "inhibit" (Device-nGnRnE), or
 *     "default" (whatever the kernel picks; Device-nGnRnE for the Pi 3's
 *     VideoCore framebuffer, the behaviour before DAR-460).
 * "ShadowUpdateDelay" (integer ms, default 0): hold shadow-to-VRAM copies
 *     until damage has been quiet for half this long, or this long since
 *     the first undrawn damage, so a burst of draws (an expose followed by
 *     the client's redraw, a window move) is copied once. 0 copies at
 *     every block handler, as before.
 * "ShadowStats" (bool, default off): log copy counts and time every 10 s.
 */
static const OptionInfoRec PDGOPOptions[] = {
    { OPTION_SHADOW_FB,           "ShadowFB",          OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_VRAM_CACHE_MODE,     "VRAMCacheMode",     OPTV_STRING,  {0}, FALSE },
    { OPTION_SHADOW_UPDATE_DELAY, "ShadowUpdateDelay", OPTV_INTEGER, {0}, FALSE },
    { OPTION_SHADOW_STATS,        "ShadowStats",       OPTV_BOOLEAN, {0}, FALSE },
    { -1, NULL, OPTV_NONE, {0}, FALSE }
};

static const char *
PDGOPCacheModeName(uint32_t mode)
{
    switch (mode) {
    case PDGOP_MAP_DEFAULT_CACHE: return "default";
    case PDGOP_MAP_INHIBIT_CACHE: return "inhibit";
    case PDGOP_MAP_WRITE_COMBINE: return "writecombine";
    case PDGOP_MAP_POSTED_WRITE:  return "posted";
    default:                      return "?";
    }
}

/* Open the framebuffer with the configured cache mode; if the kernel will
 * not map VRAM that way, fall back to its default mapping. */
static kern_return_t
PDGOPOpenConfigured(ScrnInfoPtr pScrn, PDGOPPtr p)
{
    kern_return_t kr = PDGOPOpenWithCacheMode(&p->fb, p->cacheMode);

    if (kr != KERN_SUCCESS && p->cacheMode != PDGOP_MAP_DEFAULT_CACHE) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "mapping VRAM %s failed at %s (0x%x); using the default mapping\n",
                   PDGOPCacheModeName(p->cacheMode), PDGOPLastErrorStage(), kr);
        p->cacheMode = PDGOP_MAP_DEFAULT_CACHE;
        kr = PDGOPOpenWithCacheMode(&p->fb, p->cacheMode);
    }
    if (kr == KERN_SUCCESS) {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "VRAM mapped %s (map options 0x%x) at 0x%llx, size 0x%llx\n",
                   PDGOPCacheModeName(p->cacheMode), p->fb.mapOptions,
                   (unsigned long long)p->fb.address,
                   (unsigned long long)p->fb.size);
    }
    return kr;
}

/* Field order per DriverRec (xf86str.h): driverVersion, driverName, Identify,
 * Probe, AvailableOptions, module, refCount, driverFunc, supported_devices,
 * PciProbe, platformProbe. Busless driver: no PCI/platform probe. */
_X_EXPORT DriverRec PUREDARWINGOP = {
    PDGOP_VERSION,
    PDGOP_DRIVER_NAME,
    PDGOPIdentify,
    PDGOPProbe,
    PDGOPAvailableOptions,
    NULL,   /* module */
    0,      /* refCount */
    NULL,   /* driverFunc */
    NULL,   /* supported_devices */
    NULL,   /* PciProbe */
    NULL    /* platformProbe */
};

/* Chipset "PCI" id table is irrelevant (busless); a single named chipset. */
static SymTabRec PDGOPChipsets[] = {
    { 0, "puredarwingop" },
    { -1, NULL }
};

/* --- module glue --- */
static MODULESETUPPROTO(PDGOPSetup);

static XF86ModuleVersionInfo PDGOPVersRec = {
    PDGOP_DRIVER_NAME,
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PDGOP_MAJOR, PDGOP_MINOR, PDGOP_PATCH,
    ABI_CLASS_VIDEODRV,
    ABI_VIDEODRV_VERSION,
    MOD_CLASS_VIDEODRV,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData puredarwingopModuleData = {
    &PDGOPVersRec,
    PDGOPSetup,
    NULL
};

static void *
PDGOPSetup(void *module, void *opts, int *errmaj, int *errmin)
{
    static Bool initialized = FALSE;

    (void)opts;
    (void)errmin;
    if (!initialized) {
        initialized = TRUE;
        /* DAR-439: module setup runs on Xorg's main thread, the one thread
         * that dispatches requests and draws, so raise it to USER_INTERACTIVE
         * here (Window Maker does the same in its main()). Under the launchd
         * session job (ProcessType=Interactive, a DAEMON_INTERACTIVE task)
         * XNU squashes that to USER_INITIATED, base priority 31 -> 37
         * (osfmk/kern/task_policy.c:866-868 and thread_policy.c:1554-1556 in
         * xnu-7195). Raising only one of Xorg and the window manager
         * measured worse than neither. Threads created later inherit it. The
         * return value is ignored: on failure the priority stays as it was. */
        {
            int qrc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            qos_class_t qc = QOS_CLASS_UNSPECIFIED;
            int qrel = 0;

            (void)pthread_get_qos_class_np(pthread_self(), &qc, &qrel);
            xf86Msg(X_INFO, "puredarwingop: main thread QoS set: rc=%d, class now 0x%x\n",
                qrc, (unsigned)qc);
        }
        /* Flags 0, NOT HaveDriverFuncs: that flag promises DriverRec.driverFunc
         * is valid, and this driver does not implement one (it is busless - no
         * GET_REQUIRED_HW_INTERFACES etc). Claiming it left Xorg calling through
         * a NULL driverFunc. */
        xf86AddDriver(&PUREDARWINGOP, module, 0);
        return (void *)1;
    }
    if (errmaj) {
        *errmaj = LDR_ONCEONLY;
    }
    return NULL;
}

static const OptionInfoRec *
PDGOPAvailableOptions(int chipid, int busid)
{
    (void)chipid;
    (void)busid;
    return PDGOPOptions;
}

static void
PDGOPIdentify(int flags)
{
    (void)flags;
    xf86PrintChipsets(PDGOP_NAME, "Driver for PureDarwin IOGOPFramebuffer",
                      PDGOPChipsets);
}

static Bool
PDGOPProbe(DriverPtr drv, int flags)
{
    GDevPtr *devSections;
    int      numDevSections;
    int      i;
    Bool     foundScreen = FALSE;

    xf86Msg(X_INFO, "puredarwingop: Probe(flags=0x%x)\n", flags);

    if (flags & PROBE_DETECT) {
        return FALSE;
    }

    numDevSections = xf86MatchDevice(PDGOP_DRIVER_NAME, &devSections);
    xf86Msg(X_INFO, "puredarwingop: xf86MatchDevice -> %d section(s)\n",
            numDevSections);
    if (numDevSections <= 0) {
        return FALSE;
    }

    for (i = 0; i < numDevSections; i++) {
        ScrnInfoPtr pScrn;
        int entity;

        /* Busless: claim a slot with no bus backing. */
        entity = xf86ClaimNoSlot(drv, 0, devSections[i], TRUE);
        pScrn = xf86AllocateScreen(drv, 0);
        if (pScrn == NULL) {
            continue;
        }
        xf86AddEntityToScreen(pScrn, entity);

        pScrn->driverVersion = PDGOP_VERSION;
        pScrn->driverName    = PDGOP_DRIVER_NAME;
        pScrn->name          = PDGOP_NAME;
        pScrn->Probe         = PDGOPProbe;
        pScrn->PreInit       = PDGOPPreInit;
        pScrn->ScreenInit    = PDGOPScreenInit;
        pScrn->SwitchMode    = PDGOPSwitchMode;
        pScrn->AdjustFrame   = PDGOPAdjustFrame;
        pScrn->EnterVT       = PDGOPEnterVT;
        pScrn->LeaveVT       = PDGOPLeaveVT;
        pScrn->ValidMode     = PDGOPValidMode;

        foundScreen = TRUE;
    }

    free(devSections);
    xf86Msg(X_INFO, "puredarwingop: Probe -> %s\n",
            foundScreen ? "found screen" : "no screen");
    return foundScreen;
}

static Bool
PDGOPPreInit(ScrnInfoPtr pScrn, int flags)
{
    PDGOPPtr      p;
    kern_return_t kr;
    DisplayModePtr mode;
    rgb            defaultWeight = { 0, 0, 0 };
    Gamma          zeros = { 0.0, 0.0, 0.0 };

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "PreInit(flags=0x%x, numEntities=%d)\n",
               flags, pScrn->numEntities);

    if (flags & PROBE_DETECT) {
        return FALSE;
    }
    if (pScrn->numEntities != 1) {
        return FALSE;
    }

    p = PDGOPGetRec(pScrn);

    /* Options first: the VRAM cache mode is needed to open the framebuffer.
     * xf86CollectOptions() cannot run yet: it dereferences pScrn->display
     * (xserver hw/xfree86/common/xf86Option.c:109), which xf86SetDepthBpp()
     * sets only after the framebuffer's bpp is known. So read the Device
     * section's options directly here; xf86CollectOptions() runs at its
     * usual place below and the list is processed again there (that marks
     * the options used and adds any Screen/Display-level ones). */
    pScrn->monitor = pScrn->confScreen->monitor;
    p->Options = malloc(sizeof(PDGOPOptions));
    if (p->Options == NULL) {
        return FALSE;
    }
    memcpy(p->Options, PDGOPOptions, sizeof(PDGOPOptions));
    {
        GDevPtr dev = xf86GetDevFromEntity(pScrn->entityList[0],
                                           pScrn->entityInstanceList[0]);
        if (dev != NULL && dev->options != NULL) {
            xf86ProcessOptions(pScrn->scrnIndex, dev->options, p->Options);
        }
    }

    p->cacheMode = PDGOP_MAP_WRITE_COMBINE;
    {
        const char *s = xf86GetOptValString(p->Options, OPTION_VRAM_CACHE_MODE);
        if (s != NULL) {
            if (!xf86NameCmp(s, "writecombine") || !xf86NameCmp(s, "wc")) {
                p->cacheMode = PDGOP_MAP_WRITE_COMBINE;
            } else if (!xf86NameCmp(s, "default")) {
                p->cacheMode = PDGOP_MAP_DEFAULT_CACHE;
            } else if (!xf86NameCmp(s, "inhibit") || !xf86NameCmp(s, "uncached")) {
                p->cacheMode = PDGOP_MAP_INHIBIT_CACHE;
            } else if (!xf86NameCmp(s, "posted")) {
                p->cacheMode = PDGOP_MAP_POSTED_WRITE;
            } else {
                xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                           "unknown VRAMCacheMode \"%s\"; using writecombine\n", s);
            }
        }
    }
    p->useShadow = xf86ReturnOptValBool(p->Options, OPTION_SHADOW_FB, TRUE);
    p->updateDelay = 0;
    if (xf86GetOptValInteger(p->Options, OPTION_SHADOW_UPDATE_DELAY, &p->updateDelay)) {
        if (p->updateDelay < 0) p->updateDelay = 0;
        if (p->updateDelay > 200) p->updateDelay = 200;
    }
    p->stats = xf86ReturnOptValBool(p->Options, OPTION_SHADOW_STATS, FALSE);
    xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
               "ShadowFB %s, ShadowUpdateDelay %d ms, VRAMCacheMode %s%s\n",
               p->useShadow ? "on" : "off", p->updateDelay,
               PDGOPCacheModeName(p->cacheMode), p->stats ? ", ShadowStats on" : "");

    /* Open the IOGOPFramebuffer user client and read its geometry now, so mode
     * setup below reflects the real GOP resolution rather than a guess. */
    kr = PDGOPOpenConfigured(pScrn, p);
    if (kr != KERN_SUCCESS) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "PDGOPOpen failed at %s: 0x%x\n",
                   PDGOPLastErrorStage(), kr);
        return FALSE;
    }
    p->fbOpen = TRUE;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "IOGOPFramebuffer: %ux%u, %u bpp, stride %u\n",
               p->fb.width, p->fb.height, p->fb.bpp, p->fb.stride);

    /* GOP is 32bpp BGRA/XRGB; advertise depth 24 in a 32-bit framebuffer. */
    if (!xf86SetDepthBpp(pScrn, 24, 0, p->fb.bpp,
                         Support32bppFb)) {
        return FALSE;
    }
    xf86PrintDepthBpp(pScrn);

    if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight)) {
        return FALSE;
    }
    if (!xf86SetDefaultVisual(pScrn, -1)) {
        return FALSE;
    }

    pScrn->progClock = TRUE;
    pScrn->rgbBits   = 8;
    pScrn->chipset   = PDGOP_DRIVER_NAME;
    pScrn->videoRam  = (int)(p->fb.size / 1024);

    xf86CollectOptions(pScrn, NULL);
    xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, p->Options);

    /* Build a single mode matching the live GOP resolution. */
    mode = xnfcalloc(sizeof(DisplayModeRec), 1);
    mode->name        = "GOPCurrent";
    mode->type        = M_T_DRIVER | M_T_PREFERRED;
    mode->HDisplay    = p->fb.width;
    mode->HSyncStart  = p->fb.width;
    mode->HSyncEnd    = p->fb.width;
    mode->HTotal      = p->fb.width;
    mode->VDisplay    = p->fb.height;
    mode->VSyncStart  = p->fb.height;
    mode->VSyncEnd    = p->fb.height;
    mode->VTotal      = p->fb.height;
    mode->Clock       = p->fb.width * p->fb.height * 60 / 1000;
    mode->CrtcHDisplay = mode->HDisplay;
    mode->CrtcVDisplay = mode->VDisplay;
    mode->next = mode;
    mode->prev = mode;

    pScrn->modes       = mode;
    pScrn->currentMode = mode;
    pScrn->virtualX    = p->fb.width;
    pScrn->virtualY    = p->fb.height;
    pScrn->displayWidth = p->fb.stride / (p->fb.bpp / 8);

    /* Physical size unknown; use 96 DPI. */
    pScrn->xDpi = 96;
    pScrn->yDpi = 96;

    xf86SetGamma(pScrn, zeros);

    if (!xf86LoadSubModule(pScrn, "fb")) {
        return FALSE;
    }

    return TRUE;
}

typedef uint32_t PDGOPVec __attribute__((vector_size(16), aligned(16)));

/* One scanline run, shadow -> VRAM: 4-byte stores up to 16-byte alignment,
 * then 64 bytes per iteration as four aligned 16-byte stores, then a 4-byte
 * tail. Every store is aligned to its size, so this is also safe on a
 * Device-memory mapping (VRAMCacheMode "default"/"inhibit"/"posted"), where
 * an unaligned access faults; and on a write-combine mapping the aligned
 * 16-byte stores fill whole write-buffer lines. Pixels are 32-bit, so dst,
 * src and bytes are multiples of 4.
 *
 * The 16-byte loads use __builtin_memcpy, not memcpy: the X server and its
 * modules are built with -fno-builtin (iokit build_xserver.sh), which turned
 * each memcpy(&a, src, 16) into a real call through a stack temporary --
 * about 216,000 calls per 1280x720 update. Measured on a Pi 3: the update
 * took ~32 ms (~105 MB/s, ShadowStats) while the same loop with the loads
 * inlined writes the VRAM at ~1.1 GB/s (3.35 ms per frame, fb_bench).
 * __builtin_memcpy is expanded inline whatever -fno-builtin says. */
static void
PDGOPCopyRun(CARD8 *dst, const CARD8 *src, size_t bytes)
{
    while (bytes >= 4 && ((uintptr_t)dst & 15)) {
        *(volatile uint32_t *)dst = *(const uint32_t *)src;
        dst += 4; src += 4; bytes -= 4;
    }
    while (bytes >= 64) {
        PDGOPVec a, b, c, d;
        __builtin_memcpy(&a, src, 16);
        __builtin_memcpy(&b, src + 16, 16);
        __builtin_memcpy(&c, src + 32, 16);
        __builtin_memcpy(&d, src + 48, 16);
        ((volatile PDGOPVec *)dst)[0] = a;
        ((volatile PDGOPVec *)dst)[1] = b;
        ((volatile PDGOPVec *)dst)[2] = c;
        ((volatile PDGOPVec *)dst)[3] = d;
        dst += 64; src += 64; bytes -= 64;
    }
    while (bytes >= 4) {
        *(volatile uint32_t *)dst = *(const uint32_t *)src;
        dst += 4; src += 4; bytes -= 4;
    }
}

/* Copy the damaged rectangles from the cached-RAM shadow into the VRAM
 * aperture. Both are linear and share the framebuffer's real stride
 * (pScrn->displayWidth = bytesPerRow / 4, which can exceed the width), so
 * each rectangle is a run of per-scanline copies. Returns bytes copied. */
static size_t
PDGOPBlitDamage(ScrnInfoPtr pScrn, RegionPtr region)
{
    PDGOPPtr p = PDGOPGetRec(pScrn);
    int      nbox;
    BoxPtr   box;
    int      bpp = pScrn->bitsPerPixel >> 3;
    size_t   stride = (size_t)pScrn->displayWidth * (size_t)bpp;
    size_t   copied = 0;

    if (region == NULL || p->shadow == NULL) {
        return 0;
    }

    nbox = RegionNumRects(region);
    box  = RegionRects(region);
    p->statBoxes += (unsigned long)nbox;

    for (; nbox--; box++) {
        int y1 = box->y1 < 0 ? 0 : box->y1;
        int y2 = box->y2 > pScrn->virtualY ? pScrn->virtualY : box->y2;
        int x1 = box->x1 < 0 ? 0 : box->x1;
        int x2 = box->x2 > pScrn->virtualX ? pScrn->virtualX : box->x2;
        size_t width;

        if (x2 <= x1 || y2 <= y1) {
            continue;
        }
        width = (size_t)(x2 - x1) * (size_t)bpp;

        for (int y = y1; y < y2; y++) {
            size_t off = (size_t)y * stride + (size_t)x1 * (size_t)bpp;
            PDGOPCopyRun((CARD8 *)(uintptr_t)p->fb.address + off,
                         (const CARD8 *)p->shadow + off, width);
        }
        copied += width * (size_t)(y2 - y1);
    }
    return copied;
}

/* Damage reporting only records the region; the copy happens once per dispatch
 * cycle from the block handler, so a burst of small draws costs one blit. */
static void
PDGOPDamageReport(DamagePtr damage, RegionPtr region, void *closure)
{
    PDGOPPtr p = closure;
    CARD32   now;

    (void)damage;
    (void)region;
    if (p == NULL || p->updateDelay <= 0) {
        return;
    }
    now = GetTimeInMillis();
    if (!p->pending) {
        p->pending = TRUE;
        p->firstDamage = now;
    }
    p->lastDamage = now;
}

static void
PDGOPBlockHandler(ScreenPtr pScreen, void *timeout)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    PDGOPPtr    p     = PDGOPGetRec(pScrn);
    RegionPtr   region;

    pScreen->BlockHandler = p->BlockHandler;
    (*pScreen->BlockHandler)(pScreen, timeout);
    p->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = PDGOPBlockHandler;

    if (p->damage == NULL) {
        return;
    }
    region = DamageRegion(p->damage);
    if (RegionNotEmpty(region)) {
        CARD32 now = GetTimeInMillis();

        if (p->updateDelay > 0 && p->pending) {
            /* Wait for a quiet gap of updateDelay/2, but never hold damage
             * longer than updateDelay in total. */
            int quiet = p->updateDelay / 2;
            int sinceLast = (int)(now - p->lastDamage);
            int sinceFirst = (int)(now - p->firstDamage);

            if (sinceLast < quiet && sinceFirst < p->updateDelay) {
                int wait = quiet - sinceLast;
                if (p->updateDelay - sinceFirst < wait) {
                    wait = p->updateDelay - sinceFirst;
                }
                AdjustWaitForDelay(timeout, wait);
                return;
            }
        }
        {
            CARD64 t0 = p->stats ? GetTimeInMicros() : 0;
            size_t bytes = PDGOPBlitDamage(pScrn, region);

            if (p->stats) {
                p->statSeconds += (double)(GetTimeInMicros() - t0) / 1e6;
                p->statBytes += bytes;
                p->statUpdates++;
            }
        }
        /* The mapped pages are the VirtIO resource backing, but the host
         * scanout only sees them after an explicit transfer/flush. PDGOP
         * makes this a no-op for any other framebuffer service. */
        (void)PDGOPPresent(&p->fb, 0, 0,
                           (uint32_t)pScrn->virtualX,
                           (uint32_t)pScrn->virtualY);
        DamageEmpty(p->damage);
        p->pending = FALSE;

        if (p->stats) {
            if (p->statsStart == 0) {
                p->statsStart = now;
            } else if ((int)(now - p->statsStart) >= 10000) {
                xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                           "ShadowStats: %lu updates, %lu boxes, %lu KB copied in %.1f ms over %.1f s\n",
                           p->statUpdates, p->statBoxes, p->statBytes / 1024,
                           p->statSeconds * 1e3, (now - p->statsStart) / 1000.0);
                p->statUpdates = p->statBoxes = p->statBytes = 0;
                p->statSeconds = 0;
                p->statsStart = now;
            }
        }
    }
}

static Bool
PDGOPCreateScreenResources(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    PDGOPPtr    p     = PDGOPGetRec(pScrn);
    Bool        ret;

    pScreen->CreateScreenResources = p->CreateScreenResources;
    ret = (*pScreen->CreateScreenResources)(pScreen);
    p->CreateScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = PDGOPCreateScreenResources;

    if (!ret) {
        return FALSE;
    }

    if (!p->useShadow) {
        return TRUE;
    }

    /* Raw reports (every damaging op) feed the update-delay timestamps;
     * the region still accumulates (miext/damage/damage.c,
     * DamageReportRawRegion: RegionUnion then the callback). */
    p->damage = DamageCreate(PDGOPDamageReport, NULL,
                             p->updateDelay > 0 ? DamageReportRawRegion : DamageReportNonEmpty,
                             TRUE, pScreen, p);
    if (p->damage == NULL) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "DamageCreate failed\n");
        return FALSE;
    }
    DamageRegister(&(*pScreen->GetScreenPixmap)(pScreen)->drawable, p->damage);
    DamageSetReportAfterOp(p->damage, TRUE);

    return TRUE;
}

static Bool
PDGOPScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    PDGOPPtr    p = PDGOPGetRec(pScrn);
    void       *fbstart;

    (void)argc;
    (void)argv;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "ScreenInit: fbOpen=%d vram=0x%llx size=0x%llx %dx%d stride=%d bpp=%d\n",
               (int)p->fbOpen, (unsigned long long)p->fb.address,
               (unsigned long long)p->fb.size, pScrn->virtualX, pScrn->virtualY,
               pScrn->displayWidth, pScrn->bitsPerPixel);

    /*
     * PreInit opens the framebuffer once per server run, but CloseScreen
     * closes it at the end of every server generation (so the kernel
     * console gets the display back between generations). When the last
     * client disconnects the server regenerates and calls ScreenInit again
     * without PreInit, so reopen here if a previous CloseScreen closed it.
     */
    if (!p->fbOpen) {
        kern_return_t kr = PDGOPOpenConfigured(pScrn, p);
        if (kr != KERN_SUCCESS) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "ScreenInit: PDGOPOpen failed at %s: 0x%x\n",
                       PDGOPLastErrorStage(), kr);
            return FALSE;
        }
        p->fbOpen = TRUE;
    }
    if (p->fb.address == 0) {
        return FALSE;
    }

    /* Clear VRAM to black before X takes over. */
    memset((void *)(uintptr_t)p->fb.address, 0, (size_t)p->fb.size);

    if (p->useShadow) {
        p->shadow = calloc(1, (size_t)p->fb.size);
        if (p->shadow == NULL) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "failed to allocate %llu-byte shadow framebuffer\n",
                       (unsigned long long)p->fb.size);
            return FALSE;
        }
        fbstart = p->shadow;
    } else {
        /* No shadow: fb renders straight into the VRAM mapping. */
        fbstart = (void *)(uintptr_t)p->fb.address;
    }
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "rendering into %s\n",
               p->useShadow ? "a shadow framebuffer in RAM" : "VRAM directly");

    miClearVisualTypes();
    if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth),
                          pScrn->rgbBits, pScrn->defaultVisual)) {
        return FALSE;
    }
    if (!miSetPixmapDepths()) {
        return FALSE;
    }

    if (!fbScreenInit(pScreen, fbstart,
                      pScrn->virtualX, pScrn->virtualY,
                      pScrn->xDpi, pScrn->yDpi,
                      pScrn->displayWidth, pScrn->bitsPerPixel)) {
        return FALSE;
    }

    /* Fix up RGB ordering. */
    {
        VisualPtr visual = pScreen->visuals + pScreen->numVisuals;
        while (--visual >= pScreen->visuals) {
            if ((visual->class | DynamicClass) == DirectColor) {
                visual->offsetRed   = pScrn->offset.red;
                visual->offsetGreen = pScrn->offset.green;
                visual->offsetBlue  = pScrn->offset.blue;
                visual->redMask     = pScrn->mask.red;
                visual->greenMask   = pScrn->mask.green;
                visual->blueMask    = pScrn->mask.blue;
            }
        }
    }

    if (!fbPictureInit(pScreen, NULL, 0)) {
        return FALSE;
    }

    xf86SetBlackWhitePixels(pScreen);

    /* Software cursor - no hardware cursor on GOP. */
    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

    if (!miCreateDefColormap(pScreen)) {
        return FALSE;
    }

    xf86SetBackingStore(pScreen);

    /* Registers the Damage extension's private keys on this screen. Without it
     * DamageRegister() faults looking up a private that was never allocated. */
    if (!DamageSetup(pScreen)) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "DamageSetup failed\n");
        return FALSE;
    }

    /* The screen pixmap does not exist yet - CreateScreenResources builds it
     * after ScreenInit returns - so damage cannot be attached here. */
    p->CreateScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = PDGOPCreateScreenResources;

    if (p->useShadow) {
        p->BlockHandler = pScreen->BlockHandler;
        pScreen->BlockHandler = PDGOPBlockHandler;
    }

    /* Wrap CloseScreen so we tear down the PDGOP mapping. */
    p->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = PDGOPCloseScreen;

    return TRUE;
}

static Bool
PDGOPCloseScreen(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    PDGOPPtr    p = PDGOPGetRec(pScrn);

    if (p->damage != NULL) {
        DamageUnregister(p->damage);
        DamageDestroy(p->damage);
        p->damage = NULL;
    }
    if (p->BlockHandler != NULL) {
        pScreen->BlockHandler = p->BlockHandler;
        p->BlockHandler = NULL;
    }
    if (p->shadow != NULL) {
        free(p->shadow);
        p->shadow = NULL;
    }
    if (p->fbOpen) {
        PDGOPClose(&p->fb);
        p->fbOpen = FALSE;
    }
    pScreen->CloseScreen = p->CloseScreen;
    return (*pScreen->CloseScreen)(pScreen);
}

static Bool
PDGOPEnterVT(ScrnInfoPtr pScrn)
{
    (void)pScrn;
    return TRUE;
}

static void
PDGOPLeaveVT(ScrnInfoPtr pScrn)
{
    (void)pScrn;
}

static Bool
PDGOPSwitchMode(ScrnInfoPtr pScrn, DisplayModePtr mode)
{
    (void)pScrn;
    (void)mode;
    /* Only one (native) mode; nothing to switch. */
    return TRUE;
}

static void
PDGOPAdjustFrame(ScrnInfoPtr pScrn, int x, int y)
{
    (void)pScrn;
    (void)x;
    (void)y;
    /* No panning. */
}

static ModeStatus
PDGOPValidMode(ScrnInfoPtr pScrn, DisplayModePtr mode, Bool verbose, int flags)
{
    (void)pScrn;
    (void)mode;
    (void)verbose;
    (void)flags;
    return MODE_OK;
}
