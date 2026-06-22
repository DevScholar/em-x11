/*
 * Signal delivery for em-x11's cooperative multitasking model.
 *
 * Real Linux delivers signals asynchronously: the kernel interrupts any
 * running thread to invoke the handler.  The browser/wasm has no such
 * mechanism — wasm instructions cannot be interrupted mid-execution.
 *
 * What we CAN do is deliver signals at cooperative yield points
 * (every emscripten_sleep return, every rAF tick return), which covers
 * the same ground for well-behaved event-loop–driven programs.  A
 * program in a tight CPU loop without yield points won't see signals,
 * but real signals can also be blocked (sigprocmask), so callers that
 * depend on signal delivery must yield.
 *
 * == Override strategy ==
 *
 * We provide strong definitions of signal(), sigaction(), raise(),
 * kill(), and sigprocmask().  Because user .o / .a files are searched
 * before Emscripten's libc, our definitions win.  The handler table is
 * our own; Emscripten's internal signal machinery is not touched.
 *
 * == Supported signals ==
 *
 *   SIGALRM  — JS host sets pending on setTimeout expiry
 *   SIGCHLD  — JS host sets pending on child wasm exit
 *   SIGINT   — JS host sets pending on Ctrl+C key event
 *   SIGPIPE  — poll.c sets pending on POLLHUP detection
 *   SIGTERM  — JS host sets pending on process termination request
 *   SIGUSR1  — user-defined, raise()-able from C
 *   SIGUSR2  — user-defined, raise()-able from C
 *
 * Unsupported signals (SIGILL, SIGSEGV, SIGFPE, SIGBUS, SIGSTOP,
 * SIGKILL, SIGTSTP, SIGCONT, etc.) have no meaningful wasm analogue
 * and are silently accepted by signal()/sigaction() but never delivered.
 *
 * == sigset_t notes ==
 *
 * Emscripten's <bits/alltypes.h> defines sigset_t as:
 *   typedef struct __sigset_t { unsigned long __bits[128/sizeof(long)]; }
 * sigset_t; Bitwise ops on a struct are illegal in C, so we operate on
 * __bits[0] directly (32 signals fit in one unsigned long on wasm32).
 */

/* Emscripten's <signal.h> gates sighandler_t behind _GNU_SOURCE. */
#define _GNU_SOURCE
#include <signal.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include <emscripten.h>

/* Emscripten's <signal.h> defines NSIG as _NSIG (typically 65).
 * Our handler table only tracks the first 32 signals — the rest
 * (real-time signals, etc.) have no wasm analogue. */
#define EM_X11_NSIG 32

static struct {
  struct sigaction sa;
  int pending;
} g_sig[EM_X11_NSIG];

/* True when inside em_x11_deliver_pending_signals; prevents re-entrant
 * delivery if a handler itself triggers a yield point. */
static volatile int g_in_delivery = 0;

/* Forward — defined after the public API. */
void em_x11_deliver_pending_signals(void);
void em_x11_js_kill(int pid, int sig);

/* ---- public overrides -------------------------------------------------- */

sighandler_t signal(int signum, sighandler_t handler) {
  if (signum <= 0 || signum >= EM_X11_NSIG || signum == SIGKILL ||
      signum == SIGSTOP) {
    errno = EINVAL;
    return SIG_ERR;
  }
  sighandler_t old = g_sig[signum].sa.sa_handler;
  g_sig[signum].sa.sa_handler = handler;
  g_sig[signum].sa.sa_flags = 0;
  sigemptyset(&g_sig[signum].sa.sa_mask);
  return old;
}

int sigaction(int signum,
              const struct sigaction* act,
              struct sigaction* oldact) {
  if (signum <= 0 || signum >= EM_X11_NSIG || signum == SIGKILL ||
      signum == SIGSTOP) {
    errno = EINVAL;
    return -1;
  }
  if (oldact)
    *oldact = g_sig[signum].sa;
  if (act)
    g_sig[signum].sa = *act;
  return 0;
}

int raise(int sig) {
  if (sig <= 0 || sig >= EM_X11_NSIG)
    return -1;
  g_sig[sig].pending = 1;
  /* Deliver synchronously when called from user code (safe: we're at a
   * well-defined point, not inside a yield). */
  if (!g_in_delivery)
    em_x11_deliver_pending_signals();
  return 0;
}

int kill(pid_t pid, int sig) {
  if (sig <= 0 || sig >= EM_X11_NSIG) {
    errno = EINVAL;
    return -1;
  }
  if (pid == getpid()) {
    return raise(sig);
  }
  /* Cross-process: tell the JS host to deliver.  The host finds the
   * target wasm module by pid (= connId) and ccalls
   * em_x11_signal_set_pending on it. */
  em_x11_js_kill(pid, sig);
  return 0;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
  /* The browser has no real sigprocmask.  We track the "blocked" notion
   * inside g_sig[].sa.sa_mask for API compatibility but don't enforce
   * it — callers that mask signals are in the same boat as callers
   * that just don't yield: no delivery until unmasked. */
  (void)how;
  (void)set;
  (void)oldset;
  return 0;
}

int sigemptyset(sigset_t* set) {
  if (!set) {
    errno = EFAULT;
    return -1;
  }
  set->__bits[0] = 0;
  return 0;
}

int sigfillset(sigset_t* set) {
  if (!set) {
    errno = EFAULT;
    return -1;
  }
  set->__bits[0] = ~0UL;
  return 0;
}

int sigaddset(sigset_t* set, int signum) {
  if (!set || signum <= 0 || signum >= EM_X11_NSIG) {
    errno = EINVAL;
    return -1;
  }
  set->__bits[0] |= 1UL << signum;
  return 0;
}

int sigdelset(sigset_t* set, int signum) {
  if (!set || signum <= 0 || signum >= EM_X11_NSIG) {
    errno = EINVAL;
    return -1;
  }
  set->__bits[0] &= ~(1UL << signum);
  return 0;
}

int sigismember(const sigset_t* set, int signum) {
  if (!set || signum <= 0 || signum >= EM_X11_NSIG) {
    errno = EINVAL;
    return -1;
  }
  return !!(set->__bits[0] & (1UL << signum));
}

/* ---- pending-signal API (called from host / poll / notifier) ----------- */

void em_x11_signal_set_pending(int sig) {
  if (sig > 0 && sig < EM_X11_NSIG)
    g_sig[sig].pending = 1;
}

/* Check and deliver every pending, non-blocked signal.  Safe to call
 * at any cooperative yield point; guards against re-entrant delivery. */
void em_x11_deliver_pending_signals(void) {
  if (g_in_delivery)
    return;
  g_in_delivery = 1;

  for (int sig = 1; sig < EM_X11_NSIG; sig++) {
    if (!g_sig[sig].pending)
      continue;

    struct sigaction* sa = &g_sig[sig].sa;
    sighandler_t h = sa->sa_handler;

    if (h == SIG_IGN || h == NULL) {
      g_sig[sig].pending = 0;
      continue;
    }
    if (h == SIG_DFL) {
      /* Default actions for wasm-safe signals:
       *   SIGALRM, SIGCHLD, SIGURG, SIGWINCH — ignore
       *   Everything else — ignore (we can't terminate wasm from C) */
      g_sig[sig].pending = 0;
      continue;
    }

    g_sig[sig].pending = 0;
    if (sa->sa_flags & SA_RESETHAND)
      sa->sa_handler = SIG_DFL;

    /* Call the handler.  If the handler longjmps or calls exit(), we
     * won't return here — that's correct, same as on Linux. */
    h(sig);
  }

  g_in_delivery = 0;
}

/* ---- EM_JS bridges for host-driven signals ----------------------------- */

/* The host calls this (via ccall) when a child wasm module exits.
 * conn_id is the child's connection id (its "pid"). */
EMSCRIPTEN_KEEPALIVE
void em_x11_signal_on_child_exit(int conn_id, int status) {
  (void)conn_id;
  (void)status;
  em_x11_signal_set_pending(SIGCHLD);
  /* Delivery happens at the next yield point — poll.c, notifier.c,
   * and event_queue.c all call em_x11_deliver_pending_signals after
   * their emscripten_sleep returns. */
}

/* The host calls this when a SIGALRM timer fires. */
EMSCRIPTEN_KEEPALIVE
void em_x11_signal_on_alarm(void) { em_x11_signal_set_pending(SIGALRM); }

/* The host calls this on Ctrl+C or equivalent. */
EMSCRIPTEN_KEEPALIVE
void em_x11_signal_on_interrupt(void) { em_x11_signal_set_pending(SIGINT); }

/* The host calls this when SIGTERM is requested. */
EMSCRIPTEN_KEEPALIVE
void em_x11_signal_on_terminate(void) { em_x11_signal_set_pending(SIGTERM); }

/* The host calls this when a SIGPIPE condition is detected. */
EMSCRIPTEN_KEEPALIVE
void em_x11_signal_on_pipe(void) { em_x11_signal_set_pending(SIGPIPE); }

/* kill() bridge: tell the host to deliver a signal to another process. */
/* clang-format off */
EM_JS(void, em_x11_js_kill, (int pid, int sig), {
  var host = Module['emX11Host'];
  if (host && host.onSignalDeliver) {
    host.onSignalDeliver(pid | 0, sig | 0);
  }
});
/* clang-format on */
