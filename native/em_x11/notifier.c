/*
 * Browser-friendly Tcl notifier for em-x11.
 *
 * == Why this file exists ==
 *
 * The browser has no kernel-level blocking I/O. Wasm runs on the same
 * thread as the browser UI -- there is no `select()` that can block the
 * C stack while the browser processes input events. The JS event loop
 * is the master; C code must run-to-completion within each rAF-driven
 * pump tick.
 *
 * In a real Unix X+Tk client, Tcl's standard notifier calls `select()`
 * on all registered fds (including ConnectionNumber(dpy) -- the X
 * socket) with a timeout equal to the next timer deadline. This works
 * because the kernel puts the process to sleep while the X server sends
 * data.
 *
 * In the browser, two architectures are possible:
 *
 *   A. Host-driven pump (this file):
 *      JS rAF -> Tcl_DoOneEvent(TCL_DONT_WAIT) -> notifier drains
 *      handlers -> return. Tcl timers bridged to JS host via setTimer.
 *      Zero CPU when idle. Sync EM_JS bridge calls preserved.
 *
 *   B. C-driven loop with JSPI select():
 *      Tcl's event loop calls select() which emscripten_sleep()s.
 *      Would let the standard Unix notifier run unmodified. Costs:
 *      every bridge call must return a Promise, breaking the sync
 *      runTcl entry point. Nested event loops (tkwait, update)
 *      would work, but the trade-off isn't worth it for the main
 *      rAF-driven pump path.
 *
 * We use A for the rAF-driven pump (zero CPU when idle, sync EM_JS
 * bridges preserved) and keep B available for blocking calls that
 * go through emscripten_sleep via JSPI (poll/select override in
 * poll.c, XNextEvent/XIfEvent/XPeekEvent in event_queue.c).
 *
 * The dual-path design (self-pipe + select() for Xt apps, custom
 * notifier for Tk apps) is necessary because Xt and Tk have different
 * event loop drivers. The self-pipe path works for Xt-based programs
 * (xeyes, xcalc, glxgears) unmodified -- emscripten's libc select()
 * monitors pipe fds correctly. Tk replaces Xt's event loop with Tcl's
 * notifier, so it needs this adapter.
 *
 * == ABI ==
 *
 * Tcl_SetNotifier is the official, documented, stable extension API
 * that Tcl provides for embedding in foreign event loops. The ABI has
 * been stable since Tcl 8.0 (1997):
 *
 *   setTimerProc      -- Tcl tells us "next deadline is N ms away".
 *                        We forward to the JS host so its event loop
 *                        can schedule setTimeout(N) for the next pump
 *                        tick (mirrors setting a select() timeout).
 *
 *   alertNotifierProc -- "Wake the event loop now", analogous to
 *                        writing to a self-pipe to break select().
 *                        We forward to the host.
 *
 *   waitForEventProc  -- Drains all registered file handlers. In the
 *                        polling path (timePtr={0,0}) used by the JS
 *                        pump, drains without yielding. In the block
 *                        path, yields via emscripten_sleep(1).
 *
 *   createFileHandler / deleteFileHandler -- Track fds registered by
 *                        Tk (typically just ConnectionNumber(dpy), the
 *                        self-pipe read end). waitForEvent drains them
 *                        regardless of fd readiness because there's no
 *                        real socket to test.
 *
 * The JS host owns the actual pump scheduling: it receives setTimer /
 * alert signals and either schedules setTimeout(N) for the next timer
 * or wakes a paused pump. Idle = zero scheduled work, equivalent to a
 * Linux X client at select() with no fds ready and no timer due.
 *
 * Forward-declare the Tcl notifier ABI so em-x11 doesn't need tcl.h
 * on its include path. Struct layout / function pointer signatures
 * are stable across Tcl 8.6.x.
 */

#include <emscripten.h>
#include <limits.h>
#include <stdio.h>

/* Signal delivery at cooperative yield points. */
extern void em_x11_deliver_pending_signals(void);

typedef void* ClientData;
typedef struct Tcl_Time {
  long sec;
  long usec;
} Tcl_Time;
typedef void Tcl_FileProc(ClientData clientData, int mask);
typedef struct Tcl_NotifierProcs {
  void (*setTimerProc)(const Tcl_Time* timePtr);
  int (*waitForEventProc)(const Tcl_Time* timePtr);
  void (*createFileHandlerProc)(int fd,
                                int mask,
                                Tcl_FileProc* proc,
                                ClientData cd);
  void (*deleteFileHandlerProc)(int fd);
  void* (*initNotifierProc)(void);
  void (*finalizeNotifierProc)(ClientData cd);
  void (*alertNotifierProc)(ClientData cd);
  void (*serviceModeHookProc)(int mode);
} Tcl_NotifierProcs;

extern void Tcl_SetNotifier(Tcl_NotifierProcs* procs);

#define TCL_READABLE (1 << 1)

/* A real Unix client registers one fd per Display + one per XtAppAddInput
 * call. With 8 we covered the self-pipe + a handful of extras. Raised to
 * 32 because Tk's idle/signal/timer plumbing can register several fds per
 * Tcl interpreter, and pyodide-tk may host multiple interpreters in one
 * wasm process. */
#define MAX_FILE_HANDLERS 32
typedef struct {
  int fd;
  int mask;
  Tcl_FileProc* proc;
  ClientData cd;
  int in_use;
} FileHandler;
static FileHandler g_handlers[MAX_FILE_HANDLERS];

static void
track_CreateFileHandler(int fd, int mask, Tcl_FileProc* proc, ClientData cd) {
  for (int i = 0; i < MAX_FILE_HANDLERS; i++) {
    if (g_handlers[i].in_use && g_handlers[i].fd == fd) {
      g_handlers[i].mask = mask;
      g_handlers[i].proc = proc;
      g_handlers[i].cd = cd;
      return;
    }
  }
  for (int i = 0; i < MAX_FILE_HANDLERS; i++) {
    if (!g_handlers[i].in_use) {
      g_handlers[i].in_use = 1;
      g_handlers[i].fd = fd;
      g_handlers[i].mask = mask;
      g_handlers[i].proc = proc;
      g_handlers[i].cd = cd;
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
 * through Module['emX11Host'], matching the pattern in bridges.c.
 *
 * setTimer(ms) — schedule a wake at +ms relative; ms<0 clears any
 *                pending wake (analogous to passing NULL timeout to
 *                select()).
 * alert()      — wake the host pump ASAP (analogous to writing to
 *                a self-pipe to break select()). */
EM_JS(void, em_x11_js_notifier_set_timer, (int ms), {
  var host = Module['emX11Host'];
  if (host && host.onTclSetTimer)
    host.onTclSetTimer(ms);
});

EM_JS(void, em_x11_js_notifier_alert, (void), {
  var host = Module['emX11Host'];
  if (host && host.onTclAlertNotifier)
    host.onTclAlertNotifier();
});

static void real_SetTimer(const Tcl_Time* t) {
  if (t == NULL) {
    em_x11_js_notifier_set_timer(-1);
    return;
  }
  long ms = t->sec * 1000L + t->usec / 1000L;
  if (ms < 0)
    ms = 0;
  if (ms > INT_MAX)
    ms = INT_MAX;
  em_x11_js_notifier_set_timer((int)ms);
}

static void real_AlertNotifier(ClientData cd) {
  (void)cd;
  em_x11_js_notifier_alert();
}
static void* nop_InitNotifier(void) { return (void*)1; }
static void nop_FinalizeNotifier(ClientData cd) { (void)cd; }
static void nop_ServiceModeHook(int mode) { (void)mode; }

static int yield_WaitForEvent(const Tcl_Time* timePtr) {
  /* Poll path (timePtr={0,0}) is what Tcl_DoOneEvent(TCL_DONT_WAIT)
   * takes; the JS host pump drives that, so we must NOT yield to JS
   * here -- a re-entrant pump tick would corrupt state. Just drain.
   * Block path (timePtr==NULL) suspends via emscripten_sleep/JSPI
   * so the browser stays responsive; not used by the event-driven
   * pump but safe if something calls Tcl_DoOneEvent without
   * TCL_DONT_WAIT. */
  int polling = (timePtr && timePtr->sec == 0 && timePtr->usec == 0);
  if (!polling) {
    emscripten_sleep(1);
    em_x11_deliver_pending_signals();
  }
  for (int i = 0; i < MAX_FILE_HANDLERS; i++) {
    if (g_handlers[i].in_use && (g_handlers[i].mask & TCL_READABLE)) {
      g_handlers[i].proc(g_handlers[i].cd, TCL_READABLE);
    }
  }
  return 0;
}

EMSCRIPTEN_KEEPALIVE
void em_x11_install_browser_notifier(void) {
  static Tcl_NotifierProcs procs;
  procs.setTimerProc = real_SetTimer;
  procs.waitForEventProc = yield_WaitForEvent;
  procs.createFileHandlerProc = track_CreateFileHandler;
  procs.deleteFileHandlerProc = track_DeleteFileHandler;
  procs.initNotifierProc = nop_InitNotifier;
  procs.finalizeNotifierProc = nop_FinalizeNotifier;
  procs.alertNotifierProc = real_AlertNotifier;
  procs.serviceModeHookProc = nop_ServiceModeHook;
  Tcl_SetNotifier(&procs);
}
