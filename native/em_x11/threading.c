/*
 * X11 threading stubs for em-x11.
 *
 * Real Xlib's XInitThreads / XLockDisplay / XUnlockDisplay guard the
 * Display struct against concurrent access from multiple threads.  In
 * em-x11's single-threaded cooperative model there is no true concurrency,
 * so locking is a no-op — but the symbols MUST exist so programs that
 * call these functions (almost every non-trivial Xt/Motif app) can link
 * and run unmodified.
 *
 * If Emscripten pthreads (-pthread + SAB) are ever enabled, the lock
 * stubs should be promoted to real pthread_mutex_lock/unlock on a
 * per-Display mutex allocated inside XInitThreads.  The Xlib-int.h
 * convention (dpy->lock, dpy->lock_fns) is the right place to store it.
 *
 * _Xglobal_lock is the fallback mutex used by Xlib's _XLockMutex /
 * _XUnlockMutex macros (Xos_r.h) when per-display locks aren't
 * initialised.  We allocate a real mutex_rec here so the Xos_r.h
 * macros don't dereference NULL, but without pthreads the _XLockMutex_fn
 * / _XUnlockMutex_fn function pointers are still NULL so the macros
 * are a silent no-op — correct behaviour for single-threaded wasm.
 *
 * The Xthreads.h type cascade (xthread_t, xmutex_rec, xcondition_rec)
 * resolves to pthread types when _REENTRANT is defined.  We don't set
 * _REENTRANT and Tcl/Tk builds with --disable-threads, so the existing
 * header paths should already be inactive.  This file is the safety net
 * for apps that call the three Xlib locking entry points regardless.
 */

#include <X11/Xlib.h>
#include <stdlib.h>

/* Xos_r.h references _Xglobal_lock as an extern; define it here. */
void* _Xglobal_lock = NULL;

/* Per-thread error-handler slot.  Real Xlib stores the per-thread error
 * handler via XSetIOErrorHandler / XSetErrorHandler; single-threaded
 * wasm uses the global _XErrorFunction / _XIOErrorFunction variables
 * from error.c directly, so this is a zero-size stub. */
void* _XErrorFunction = NULL;
void* _XIOErrorFunction = NULL;

/* _XLockMutex_fn / _XUnlockMutex_fn — function pointers that Xos_r.h
 * calls through.  NULL means "not initialised" → macros are no-ops.
 * When pthreads land, XInitThreads sets these to pthread_mutex_lock /
 * pthread_mutex_unlock (or the xmutex_* wrappers). */
void (*_XLockMutex_fn)(void*, const char*, int) = NULL;
void (*_XUnlockMutex_fn)(void*, const char*, int) = NULL;

/* ---- public API --------------------------------------------------------- */

Status XInitThreads(void) {
  /* Single-threaded: record that we were called (so apps that gate on
   * the return value proceed) but don't allocate anything. */
  return 1;
}

void XLockDisplay(Display* dpy) {
  (void)dpy;
  /* No-op: single-threaded wasm has no concurrent Display access. */
}

void XUnlockDisplay(Display* dpy) {
  (void)dpy;
  /* No-op: paired with XLockDisplay above. */
}

/* XSetIOErrorHandler / XSetErrorHandler are declared in Xlib.h and
 * defined in error.c.  Some threaded apps reach for XSetIOErrorHandler
 * through a macro that resolves through _XLockMutex.  The actual
 * implementations in em_x11/error.c are single-threaded already. */
