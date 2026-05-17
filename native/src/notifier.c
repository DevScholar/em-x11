/*
 * Browser-friendly Tcl notifier for em-x11.
 *
 * Stock Unix notifier calls select() on the X "fd" — meaningless under
 * em-x11 since there is no real socket; Tk's DisplayFileProc never
 * fires and X events queued by em-x11 sit forever. We replace the
 * notifier with one that honors the standardised Tcl_SetNotifier ABI:
 *
 *   setTimerProc      — Tcl tells us "next deadline is N ms away".
 *                       We forward this to the JS host so its event
 *                       loop can schedule a wake at exactly that
 *                       deadline (mirrors how a real X client sets a
 *                       select() timeout).
 *   alertNotifierProc — Standardised "wake the event loop" primitive,
 *                       analogous to writing to a self-pipe fd that
 *                       breaks select(). We forward to the host.
 *   createFileHandler — Tracked; waitForEvent drains every registered
 *                       handler regardless of fd readiness, because
 *                       there's no real fd to test.
 *
 * The JS host (em-x11's Host) owns the actual pump scheduling: it
 * receives setTimer / alert signals and either schedules a real
 * setTimeout(N) for the next timer or wakes a paused pump. Idle =
 * zero scheduled work, exactly as a Linux X client at select() with
 * no fds ready and no timer due.
 *
 * Forward-declare the Tcl notifier ABI so em-x11 doesn't need tcl.h
 * on its include path. Struct layout / function pointer signatures
 * are stable across Tcl 8.6.x.
 */

#include <stdio.h>
#include <limits.h>
#include <emscripten.h>

typedef void *ClientData;
typedef struct Tcl_Time { long sec; long usec; } Tcl_Time;
typedef void Tcl_FileProc(ClientData clientData, int mask);
typedef struct Tcl_NotifierProcs {
    void  (*setTimerProc)(const Tcl_Time *timePtr);
    int   (*waitForEventProc)(const Tcl_Time *timePtr);
    void  (*createFileHandlerProc)(int fd, int mask, Tcl_FileProc *proc, ClientData cd);
    void  (*deleteFileHandlerProc)(int fd);
    void *(*initNotifierProc)(void);
    void  (*finalizeNotifierProc)(ClientData cd);
    void  (*alertNotifierProc)(ClientData cd);
    void  (*serviceModeHookProc)(int mode);
} Tcl_NotifierProcs;

extern void Tcl_SetNotifier(Tcl_NotifierProcs *procs);

#define TCL_READABLE (1<<1)

#define MAX_FILE_HANDLERS 8
typedef struct {
    int fd;
    int mask;
    Tcl_FileProc *proc;
    ClientData cd;
    int in_use;
} FileHandler;
static FileHandler g_handlers[MAX_FILE_HANDLERS];

static void track_CreateFileHandler(int fd, int mask, Tcl_FileProc *proc, ClientData cd) {
    for (int i = 0; i < MAX_FILE_HANDLERS; i++) {
        if (g_handlers[i].in_use && g_handlers[i].fd == fd) {
            g_handlers[i].mask = mask;
            g_handlers[i].proc = proc;
            g_handlers[i].cd   = cd;
            return;
        }
    }
    for (int i = 0; i < MAX_FILE_HANDLERS; i++) {
        if (!g_handlers[i].in_use) {
            g_handlers[i].in_use = 1;
            g_handlers[i].fd   = fd;
            g_handlers[i].mask = mask;
            g_handlers[i].proc = proc;
            g_handlers[i].cd   = cd;
            return;
        }
    }
    fprintf(stderr, "em-x11: file handler table full (fd=%d dropped)\n", fd);
}

static void track_DeleteFileHandler(int fd) {
    for (int i = 0; i < MAX_FILE_HANDLERS; i++) {
        if (g_handlers[i].in_use && g_handlers[i].fd == fd) {
            g_handlers[i].in_use = 0;
            return;
        }
    }
}

/* Bridges to the JS host. The host registers two callbacks via
 * Host.installEventLoopWake(); these EM_JS bodies dispatch to them
 * through globalThis.emX11._bridge, matching the pattern in bridges.c.
 *
 * setTimer(ms) — schedule a wake at +ms relative; ms<0 clears any
 *                pending wake (analogous to passing NULL timeout to
 *                select()).
 * alert()      — wake the host pump ASAP (analogous to writing to
 *                a self-pipe to break select()). */
EM_JS(void, emx11_js_notifier_set_timer, (int ms), {
    var b = globalThis.emX11 && globalThis.emX11._bridge;
    if (b && b.onTclSetTimer) b.onTclSetTimer(ms);
});

EM_JS(void, emx11_js_notifier_alert, (void), {
    var b = globalThis.emX11 && globalThis.emX11._bridge;
    if (b && b.onTclAlertNotifier) b.onTclAlertNotifier();
});

static void real_SetTimer(const Tcl_Time *t) {
    if (t == NULL) {
        emx11_js_notifier_set_timer(-1);
        return;
    }
    long ms = t->sec * 1000L + t->usec / 1000L;
    if (ms < 0) ms = 0;
    if (ms > INT_MAX) ms = INT_MAX;
    emx11_js_notifier_set_timer((int)ms);
}

static void  real_AlertNotifier(ClientData cd)      { (void)cd; emx11_js_notifier_alert(); }
static void *nop_InitNotifier(void)                 { return (void *)1; }
static void  nop_FinalizeNotifier(ClientData cd)    { (void)cd; }
static void  nop_ServiceModeHook(int mode)          { (void)mode; }

static int yield_WaitForEvent(const Tcl_Time *timePtr) {
    /* Poll path (timePtr={0,0}) is what Tcl_DoOneEvent(TCL_DONT_WAIT)
     * takes; the JS host pump drives that, so we must NOT yield to JS
     * here -- a re-entrant pump tick would corrupt state. Just drain.
     * Block path (timePtr==NULL) yields with emscripten_sleep so the
     * browser stays responsive; not used by the event-driven pump but
     * safe if something does call Tcl_DoOneEvent without TCL_DONT_WAIT. */
    int polling = (timePtr && timePtr->sec == 0 && timePtr->usec == 0);
    if (!polling) {
        emscripten_sleep(1);
    }
    for (int i = 0; i < MAX_FILE_HANDLERS; i++) {
        if (g_handlers[i].in_use && (g_handlers[i].mask & TCL_READABLE)) {
            g_handlers[i].proc(g_handlers[i].cd, TCL_READABLE);
        }
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void emx11_install_browser_notifier(void) {
    static Tcl_NotifierProcs procs;
    procs.setTimerProc           = real_SetTimer;
    procs.waitForEventProc       = yield_WaitForEvent;
    procs.createFileHandlerProc  = track_CreateFileHandler;
    procs.deleteFileHandlerProc  = track_DeleteFileHandler;
    procs.initNotifierProc       = nop_InitNotifier;
    procs.finalizeNotifierProc   = nop_FinalizeNotifier;
    procs.alertNotifierProc      = real_AlertNotifier;
    procs.serviceModeHookProc    = nop_ServiceModeHook;
    Tcl_SetNotifier(&procs);
}
