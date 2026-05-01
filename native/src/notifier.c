/*
 * Browser-friendly Tcl notifier for em-x11.
 *
 * Stock Unix notifier calls select() on the X "fd" — meaningless under
 * em-x11 since there is no real socket; Tk's DisplayFileProc never
 * fires and X events queued by em-x11 sit forever. We replace the
 * notifier so WaitForEvent unconditionally drains every registered
 * file handler. Same shape as wacl-tk's in-runtime notifier; this
 * version is the one used by pyodide-tk where the runtime is just
 * Pyodide + dlopen-ed side modules.
 *
 * Forward-declare the Tcl notifier ABI so em-x11 doesn't need tcl.h
 * on its include path. Struct layout / function pointer signatures
 * are stable across Tcl 8.6.x.
 */

#include <stdio.h>
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

static void  nop_SetTimer(const Tcl_Time *t)        { (void)t; }
static void *nop_InitNotifier(void)                 { return (void *)1; }
static void  nop_FinalizeNotifier(ClientData cd)    { (void)cd; }
static void  nop_AlertNotifier(ClientData cd)       { (void)cd; }
static void  nop_ServiceModeHook(int mode)          { (void)mode; }

static int yield_WaitForEvent(const Tcl_Time *timePtr) {
    /* Poll path (timePtr={0,0}) is what Tcl_DoOneEvent(TCL_DONT_WAIT)
     * takes; the JS RAF pump drives that, so we must NOT yield to JS
     * here -- a re-entrant pump tick would corrupt state. Just drain.
     * Block path (timePtr==NULL) yields with emscripten_sleep so the
     * browser stays responsive; not used by the RAF pump but safe if
     * something does call Tcl_DoOneEvent without TCL_DONT_WAIT. */
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
    procs.setTimerProc           = nop_SetTimer;
    procs.waitForEventProc       = yield_WaitForEvent;
    procs.createFileHandlerProc  = track_CreateFileHandler;
    procs.deleteFileHandlerProc  = track_DeleteFileHandler;
    procs.initNotifierProc       = nop_InitNotifier;
    procs.finalizeNotifierProc   = nop_FinalizeNotifier;
    procs.alertNotifierProc      = nop_AlertNotifier;
    procs.serviceModeHookProc    = nop_ServiceModeHook;
    Tcl_SetNotifier(&procs);
}
