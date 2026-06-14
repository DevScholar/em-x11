/*
 * Strong-symbol overrides for poll(), select(), ppoll(), pselect().
 *
 * Emscripten's libc poll/select lower to __syscall_poll which does a
 * non-blocking fd-readiness check.  For the blocking case (timeout > 0
 * or NULL), the browser has no kernel to put the thread to sleep on —
 * the JS event loop is the master.  We replace poll/select with versions
 * that yield to the browser via emscripten_sleep() under JSPI,
 * suspending the wasm call so DOM/X11 events arrive freely.
 *
 * == Linux compatibility ==
 *
 * poll() is a PURE OBSERVER on Linux — it never consumes fd data.  We
 * preserve this by:
 *
 *   - Display fds (self-pipe read ends): checking the em_x11 ring buffer
 *     directly (dpy->event_head != dpy->event_tail), never reading the
 *     pipe.  The pipe bytes are wakeup signals; the ring buffer is the
 *     authoritative data store.
 *
 *   - Other fds: trying FIONREAD ioctl first (non-destructive, works
 *     for Emscripten pipes and sockets).  When FIONREAD is unavailable
 *     the fallback consumes 1 byte via a non-blocking read — that byte
 *     is stashed in a per-fd pushback buffer and returned to the real
 *     reader via __wrap_read() (see bottom of file), preserving the
 *     Linux poll() observer contract.
 *
 *   - POLLNVAL is set when fcntl(F_GETFL) returns -1 (EBADF).
 *   - POLLHUP is set when a non-blocking read returns EOF (peer closed).
 *   - POLLERR is set on real I/O errors from the probe read.
 *
 *   - ppoll() and pselect() accept the sigmask parameter for API
 *     compatibility; the mask is ignored by default.  Signal delivery
 *     is handled cooperatively via em_x11_deliver_pending_signals().
 *
 *   - Input validation sets errno (EFAULT, EINVAL) matching Linux.
 *
 * == Blocking architecture ==
 *
 * The blocking path polls fds at adaptive intervals via emscripten_sleep():
 *
 *     remaining > 50ms  → sleep 10ms   (100 Hz — imperceptible to humans)
 *     remaining >  5ms  → sleep  2ms   (500 Hz)
 *     remaining >  0ms  → sleep  1ms   (1000 Hz — tight polling)
 *     infinite           → sleep  5ms   (200 Hz — responsive enough)
 *
 * Each sleep unwinds the wasm stack via JSPI and yields to the browser
 * event loop.  DOM/X11 events arrive, the ring buffer fills, the self-
 * pipe receives wakeup bytes.  When the sleep expires, stack rewinds,
 * we re-probe all fds, and return immediately if any are ready.
 *
 * After each emscripten_sleep return we call em_x11_deliver_pending_signals()
 * so SIGALRM / SIGCHLD / SIGINT handlers fire at cooperative yield points.
 *
 * g_in_blocking_poll gates the rAF tick (tick() in tcldide-runtime.c)
 * so inner event loops (tkwait/vwait) don't have their events stolen
 * by the outer pump.
 *
 * This follows the same strong-symbol pattern as process.c (execvp).
 *
 *   em_x11_event_queue_push → write(wakeup_fd) → pipe becomes readable
 *                            → poll/select returns → file handler runs
 *                            → XEventsQueued drains pipe → processes events
 */

#include "em_x11_internal.h"

#include <emscripten.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---- pushback buffer for poll() destructive fallback ---------------------
 *
 * When FIONREAD ioctl is unsupported on a non-Display fd, poll() falls
 * back to a 1-byte non-blocking read() to determine readiness.  That
 * consumes data the real reader (e.g. a file-handler callback) would
 * expect to see, violating the Linux poll() contract that poll is a
 * PURE OBSERVER.
 *
 * We stash the consumed byte here and serve it from __wrap_read() (see
 * bottom of file) so the next read(fd) on that fd returns the byte
 * first, then falls through to the real read.  Capacity is 1 byte per
 * fd — multiple poll() passes between reads lose data, same as on Linux
 * where multiple threads racing on the same fd without synchronisation
 * produce undefined results.
 *
 * -Wl,--wrap=read is required on the final link line for the pushback
 * to work.  Without it, __wrap_read is dead code and the consumed byte
 * is silently lost (the pre-existing limitation).  Consumers that only
 * use Display fds (which go through the non-destructive ring-buffer
 * path) don't need the wrap. */

#define PEEKBUF_SIZE FD_SETSIZE

static char g_peek_byte[PEEKBUF_SIZE];
static int g_peek_valid[PEEKBUF_SIZE];

/* ---- Display fd registry ----------------------------------------------
 *
 * Maps self-pipe read-end fds to their Display*.  poll() uses this to
 * check the ring buffer (non-destructive) instead of reading from the
 * pipe (destructive).  Sized for the single-Display case today; grows
 * trivially when multi-Display lands. */

#define POLL_MAX_DISPLAYS 4

static Display* g_poll_fd_display[FD_SETSIZE];
static int g_poll_display_count;

void em_x11_poll_register_display_fd(int fd, Display* dpy) {
  if (fd >= 0 && fd < FD_SETSIZE && !g_poll_fd_display[fd]) {
    g_poll_fd_display[fd] = dpy;
    g_poll_display_count++;
  }
}

void em_x11_poll_unregister_display_fd(int fd) {
  if (fd >= 0 && fd < FD_SETSIZE && g_poll_fd_display[fd]) {
    g_poll_fd_display[fd] = NULL;
    g_poll_display_count--;
  }
}

static Display* poll_display_for_fd(int fd) {
  if (fd >= 0 && fd < FD_SETSIZE)
    return g_poll_fd_display[fd];
  return NULL;
}

/* ---- fd readiness helpers --------------------------------------------- */

/* Return values for fd_is_readable / fd_is_writable:
 *   >0  — ready (1 = data, 2 = EOF / peer closed)
 *    0  — not ready, no error
 *   -1  — EBADF (fd invalid) → POLLNVAL
 *   -2  — real I/O error → POLLERR */

static int fd_is_readable(int fd) {
  /* Display self-pipe: check ring buffer directly.  This is the
   * authoritative data store — the pipe bytes are just wakeups.
   * A Display fd never reaches EOF through the ring-buffer path
   * (the pipe is local and both ends are owned by the Display). */
  Display* dpy = poll_display_for_fd(fd);
  if (dpy)
    return (dpy->event_head != dpy->event_tail) ? 1 : 0;

  /* Validate the fd.  EBADF → POLLNVAL. */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;

  /* Non-destructive check: how many bytes are waiting? */
  int nbytes = 0;
  if (ioctl(fd, FIONREAD, &nbytes) == 0)
    return nbytes > 0 ? 1 : 0;

  /* FIONREAD failed — fall back to non-blocking peek read.
   * Save the consumed byte in the pushback buffer so the real
   * reader retrieves it via __wrap_read(). */
  int saved_flags = flags;
  if (!(flags & O_NONBLOCK))
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  char c;
  ssize_t n = read(fd, &c, 1);

  /* Restore original flags. */
  if (!(saved_flags & O_NONBLOCK))
    fcntl(fd, F_SETFL, saved_flags);

  if (n > 0) {
    if (fd >= 0 && fd < PEEKBUF_SIZE && !g_peek_valid[fd]) {
      g_peek_byte[fd] = c;
      g_peek_valid[fd] = 1;
    }
    return 1;
  }
  if (n == 0)
    return 2; /* EOF — peer closed → POLLIN | POLLHUP */
  if (errno == EAGAIN || errno == EWOULDBLOCK)
    return 0; /* no data, no error */
  return -2;  /* real error → POLLERR */
}

static int fd_is_writable(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1; /* EBADF → POLLNVAL */

  /* A fd is writable if it was opened with O_WRONLY or O_RDWR and
   * the write path is still live.  We can't detect a full pipe buffer
   * without attempting a write, which would be destructive.  Callers
   * that need strict POLLOUT semantics should use non-blocking writes
   * and handle EAGAIN at the application level — same as on Linux
   * where poll(POLLOUT) doesn't guarantee the next write won't block
   * if another thread consumes the buffer space between poll and write. */
  int accmode = flags & O_ACCMODE;
  return (accmode == O_WRONLY || accmode == O_RDWR) ? 1 : 0;
}

/* ---- signal checkpoint (declared in signal.c) ------------------------- */

extern void em_x11_deliver_pending_signals(void);

/* ---- non-blocking poll ------------------------------------------------ */

static int poll_check(struct pollfd* fds, nfds_t nfds) {
  int ready = 0;

  for (nfds_t i = 0; i < nfds; i++) {
    fds[i].revents = 0;

    if (fds[i].fd < 0)
      continue;

    int readable = -1;
    int writable = -1;
    Display* dpy = poll_display_for_fd(fds[i].fd);

    /* POLLIN / POLLPRI */
    if (fds[i].events & (POLLIN | POLLPRI)) {
      readable = fd_is_readable(fds[i].fd);

      if (readable == 1) {
        /* Data available, not EOF. */
        if (fds[i].events & POLLIN)
          fds[i].revents |= POLLIN;
        if (fds[i].events & POLLPRI)
          fds[i].revents |= POLLPRI;
        ready++;
      } else if (readable == 2) {
        /* EOF — peer closed.  Linux returns POLLIN | POLLHUP. */
        if (fds[i].events & POLLIN)
          fds[i].revents |= (POLLIN | POLLHUP);
        if (fds[i].events & POLLPRI)
          fds[i].revents |= POLLPRI;
        ready++;
      } else if (readable == 0) {
        /* No data.  For non-Display fds whose FIONREAD succeeded
         * (returning 0), the fd might be at EOF — a non-blocking
         * read is the only way to tell.  Display fds never reach
         * EOF through the ring-buffer path. */
        if (!dpy && (fds[i].events & POLLIN)) {
          int flags = fcntl(fds[i].fd, F_GETFL, 0);
          if (flags != -1) {
            if (!(flags & O_NONBLOCK))
              fcntl(fds[i].fd, F_SETFL, flags | O_NONBLOCK);
            char c;
            ssize_t n = read(fds[i].fd, &c, 1);
            if (n == 0) {
              fds[i].revents |= (POLLIN | POLLHUP);
              ready++;
            } else if (n > 0) {
              /* Stash the consumed byte. */
              int efd = fds[i].fd;
              if (efd >= 0 && efd < PEEKBUF_SIZE && !g_peek_valid[efd]) {
                g_peek_byte[efd] = c;
                g_peek_valid[efd] = 1;
              }
              fds[i].revents |= POLLIN;
              ready++;
            }
          }
        }
      } else if (readable == -1) {
        /* EBADF — fd is invalid. */
        fds[i].revents |= POLLNVAL;
        ready++;
        continue;
      } else {
        /* readable == -2: real I/O error. */
        fds[i].revents |= POLLERR;
        ready++;
      }
    }

    /* POLLOUT */
    if (fds[i].events & POLLOUT) {
      writable = fd_is_writable(fds[i].fd);
      if (writable > 0) {
        fds[i].revents |= POLLOUT;
        ready++;
      } else if (writable == -1) {
        fds[i].revents |= POLLNVAL;
        ready++;
      }
    }

    /* Stale POLLNVAL check: fd in set with no events requested but
     * invalid fd.  Linux sets POLLNVAL even when no events are
     * specified (the fd merely being in the poll set is enough). */
    if (!fds[i].revents) {
      int flags = fcntl(fds[i].fd, F_GETFL, 0);
      if (flags == -1) {
        fds[i].revents |= POLLNVAL;
        ready++;
      }
    }
  }

  return ready;
}

/* ---- blocking-poll gate -----------------------------------------------
 *
 * While poll() is blocked in emscripten_sleep, the rAF tick (tick() in
 * tcldide-runtime.c) must NOT process Tcl events — otherwise it steals
 * events that the inner event loop (tkwait/vwait) is waiting for.
 * tick() checks this flag and returns immediately when set.
 *
 * volatile suffices for JSPI's single-threaded wasm; there is no
 * concurrent C thread to race with.  If multi-threaded wasm ever lands
 * here, upgrade to _Atomic. */

static volatile int g_in_blocking_poll = 0;

int em_x11_is_blocking_in_poll(void) { return g_in_blocking_poll; }

/* ---- poll() -----------------------------------------------------------
 *
 * timeout == 0      → non-blocking: one poll_check, return immediately.
 * timeout >  0      → deadline in ms; poll at adaptive intervals.
 * timeout <  0 (-1) → infinite;  poll at 5 ms intervals. */

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  /* Validate inputs (mirrors Linux checks). */
  if (nfds > FD_SETSIZE) {
    errno = EINVAL;
    return -1;
  }
  if (nfds > 0 && !fds) {
    errno = EFAULT;
    return -1;
  }

  /* Non-blocking pass. */
  int ready = poll_check(fds, nfds);
  if (ready > 0 || timeout == 0)
    return ready;

  /* Blocking path.
   *
   * Interval adapts to the deadline to balance latency and wasm↔JS
   * boundary-crossing overhead.  Each emscripten_sleep suspends the
   * wasm call via JSPI, yielding to the browser event loop.  During
   * the yield, ccalls from JS (em_x11_event_queue_push, etc.) create
   * new wasm stacks and populate the ring buffer — they do not
   * interfere with the suspended poll() stack. */

  int infinite = (timeout < 0);
  double deadline = infinite ? 0 : emscripten_get_now() + (double)timeout;

  for (;;) {
    unsigned int sleep_ms;

    if (infinite) {
      sleep_ms = 5;
    } else {
      double remaining = deadline - emscripten_get_now();
      if (remaining <= 0)
        return 0;
      if (remaining > 50.0)
        sleep_ms = 10;
      else if (remaining > 5.0)
        sleep_ms = 2;
      else
        sleep_ms = 1;
    }

    g_in_blocking_poll = 1;
    emscripten_sleep(sleep_ms);
    g_in_blocking_poll = 0;

    /* Deliver any pending signals before re-checking fds.
     * The JS host may have set SIGCHLD (child exit), SIGALRM
     * (timer expiry), or SIGINT (Ctrl+C) during the yield. */
    em_x11_deliver_pending_signals();

    ready = poll_check(fds, nfds);
    if (ready > 0)
      return ready;
    if (!infinite && emscripten_get_now() >= deadline)
      return 0;
  }
}

/* ---- ppoll() ----------------------------------------------------------
 *
 * Identical to poll() except:
 *   - timeout is a struct timespec* (nanosecond precision) instead of int
 *   - sigmask is accepted for API compat; signal delivery is handled
 *     cooperatively via em_x11_deliver_pending_signals() in poll() */

int ppoll(struct pollfd* fds,
          nfds_t nfds,
          const struct timespec* tmo_p,
          const sigset_t* sigmask) {
  (void)sigmask;

  int timeout_ms;
  if (!tmo_p) {
    timeout_ms = -1;
  } else {
    long ms = tmo_p->tv_sec * 1000L + tmo_p->tv_nsec / 1000000L;
    /* Round up sub-millisecond remainders: {0, 500000} → 1 ms. */
    if (tmo_p->tv_nsec % 1000000L > 0)
      ms++;
    if (ms > (long)INT_MAX)
      ms = INT_MAX;
    timeout_ms = (int)ms;
  }

  return poll(fds, nfds, timeout_ms);
}

/* ---- select() ---------------------------------------------------------
 *
 * Converts fd_sets → pollfds, delegates to poll(), converts back.
 * Counts each ready fd per fd_set (a single fd ready in both readfds
 * and writefds counts as 2), matching Linux semantics. */

int select(int nfds,
           fd_set* readfds,
           fd_set* writefds,
           fd_set* exceptfds,
           struct timeval* timeout) {
  if (nfds < 0 || nfds > FD_SETSIZE) {
    errno = EINVAL;
    return -1;
  }

  /* Convert fd_sets → pollfds. */
  struct pollfd pfds[FD_SETSIZE];
  nfds_t pidx = 0;

  for (int fd = 0; fd < nfds && pidx < FD_SETSIZE; fd++) {
    short events = 0;
    if (readfds && FD_ISSET(fd, readfds))
      events |= POLLIN;
    if (writefds && FD_ISSET(fd, writefds))
      events |= POLLOUT;
    if (exceptfds && FD_ISSET(fd, exceptfds))
      events |= POLLPRI;
    if (events) {
      pfds[pidx].fd = fd;
      pfds[pidx].events = events;
      pfds[pidx].revents = 0;
      pidx++;
    }
  }

  /* Clear output fd_sets so we only set bits for ready fds. */
  if (readfds)
    FD_ZERO(readfds);
  if (writefds)
    FD_ZERO(writefds);
  if (exceptfds)
    FD_ZERO(exceptfds);

  /* Convert timeout.  Preserve sub-millisecond intent: {0, 500} →
   * 1 ms (rounding up) rather than 0 (which would mean non-blocking
   * and discard the caller's wait intent). */
  int poll_timeout;
  if (!timeout) {
    poll_timeout = -1;
  } else if (timeout->tv_sec == 0 && timeout->tv_usec == 0) {
    poll_timeout = 0;
  } else {
    poll_timeout = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    if (poll_timeout == 0)
      poll_timeout = 1; /* sub-ms: round up to 1 ms */
  }

  int ret = poll(pfds, pidx, poll_timeout);
  if (ret <= 0)
    return ret;

  /* Convert poll results back to fd_sets.  A single fd ready for both
   * read and write counts as 2, matching Linux select() semantics. */
  int count = 0;
  for (nfds_t i = 0; i < pidx; i++) {
    short rev = pfds[i].revents;
    if (rev & (POLLIN | POLLHUP | POLLERR)) {
      if (readfds) {
        FD_SET(pfds[i].fd, readfds);
        count++;
      }
    }
    if (rev & POLLOUT) {
      if (writefds) {
        FD_SET(pfds[i].fd, writefds);
        count++;
      }
    }
    if (rev & POLLPRI) {
      if (exceptfds) {
        FD_SET(pfds[i].fd, exceptfds);
        count++;
      }
    }
  }
  return count;
}

/* ---- pselect() --------------------------------------------------------
 *
 * Identical to select() except:
 *   - timeout is a struct timespec* (nanosecond precision)
 *   - sigmask is accepted for API compat; signal delivery is handled
 *     cooperatively via em_x11_deliver_pending_signals() in poll() */

int pselect(int nfds,
            fd_set* readfds,
            fd_set* writefds,
            fd_set* exceptfds,
            const struct timespec* timeout,
            const sigset_t* sigmask) {
  (void)sigmask;

  struct timeval tv;
  struct timeval* tvp = NULL;

  if (timeout) {
    tv.tv_sec = timeout->tv_sec;
    tv.tv_usec = timeout->tv_nsec / 1000;
    if (timeout->tv_nsec % 1000 > 0)
      tv.tv_usec++; /* round up sub-microsecond remainders */
    tvp = &tv;
  }

  return select(nfds, readfds, writefds, exceptfds, tvp);
}

/* ---- __wrap_read: pushback buffer drain ---------------------------------
 *
 * When FIONREAD is unsupported, poll()'s destructive fallback consumes
 * 1 byte to determine readiness.  We stash it in g_peek_byte[fd] and
 * serve it here, so the real reader sees every byte Linux would have
 * delivered — poll() is once again a pure observer.
 *
 * Link with -Wl,--wrap=read to activate.  Without the wrap, this
 * function is dead code and the old limitation (silently lost byte)
 * applies — fine for Display fds, which go through the non-destructive
 * ring-buffer path. */

#ifndef __wasm__
/* Host-side compilation: no-op, the real read() is the system call. */
#else

extern ssize_t __real_read(int fd, void* buf, size_t count);

ssize_t __wrap_read(int fd, void* buf, size_t count) {
  if (fd >= 0 && fd < PEEKBUF_SIZE && g_peek_valid[fd] && count > 0) {
    *(char*)buf = g_peek_byte[fd];
    g_peek_valid[fd] = 0;
    /* If more bytes were requested, chain to __real_read for the rest. */
    if (count > 1) {
      ssize_t tail = __real_read(fd, (char*)buf + 1, count - 1);
      return tail >= 0 ? 1 + tail : 1;
    }
    return 1;
  }
  return __real_read(fd, buf, count);
}

#endif

/* ---- __wrap_select / __wrap_poll — --wrap linker overrides ---------------
 *
 * Pyodide's main module (pyodide.asm.wasm) has its own strong _select /
 * _poll symbols from Emscripten's libc, which side modules cannot
 * override via {global: true}.  Libraries that call select()/poll()
 * thus reach Emscripten's non-blocking stubs instead of our JSPI
 * implementation.
 *
 * The workaround: link the *caller* (e.g. libtcl8.6.so) with
 * -Wl,--wrap=select -Wl,--wrap=poll.  wasm-ld rewrites every call to
 * `select` → `__wrap_select`, making it an import satisfied by the
 * functions below.  Our __wrap_* are thin trampolines that delegate to
 * the real JSPI-capable implementations in this file.
 *
 * Without the --wrap flag on the caller, these functions are dead code
 * and the original Pyodide-builtin select/poll are used instead. */

int __wrap_select(int nfds,
                  fd_set* readfds,
                  fd_set* writefds,
                  fd_set* exceptfds,
                  struct timeval* timeout) {
  return select(nfds, readfds, writefds, exceptfds, timeout);
}

int __wrap_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  return poll(fds, nfds, timeout);
}
