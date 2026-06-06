/*
 * Strong-symbol overrides for poll() and select().
 *
 * Emscripten's libc poll/select lower to __syscall_poll.  In the browser,
 * there is no kernel to block on — the JS event loop is the master.  We
 * replace poll() and select() with versions that:
 *
 *   timeout == 0          — non-blocking fd readiness check, returns
 *                           immediately.  Used by TCL_DONT_WAIT paths
 *                           (rAF tick, post-eval drain).
 *
 *   timeout > 0  or  NULL — blocking with deadline or indefinite.  We
 *                           poll fds at adaptive intervals, yielding to
 *                           the browser via emscripten_sleep() between
 *                           checks.  JSPI suspends the wasm call across
 *                           each sleep, so DOM/X11 events arrive freely
 *                           during the block.  The JS caller receives a
 *                           Promise when the wasm suspends — callers
 *                           must await it.
 *
 * This follows the same strong-symbol pattern as process.c (execvp).
 *
 *   emx11_event_queue_push → write(wakeup_fd) → pipe becomes readable
 *                            → poll/select returns → file handler runs
 *                            → XEventsQueued drains pipe → processes events
 */

#include <emscripten.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <unistd.h>

/* -- fd readiness helpers ------------------------------------------------
 *
 * The self-pipe read end (dpy->fd) is O_NONBLOCK (set in display.c).  We
 * probe every fd in the poll set by attempting a non-blocking read of up
 * to 64 bytes.  If read() returns > 0, the fd is readable.  The consumed
 * byte(s) are wakeup bytes written by emx11_event_queue_push; consuming
 * them here is harmless — XEventsQueued drains the pipe again and reads
 * the real events from the ring buffer. */

static int fd_is_readable(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;
  if (!(flags & O_NONBLOCK))
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  char buf[64];
  ssize_t n = read(fd, buf, sizeof(buf));
  if (n > 0)
    return 1;
  if (n == 0)
    return 1; /* EOF — peer closed */
  if (errno == EAGAIN || errno == EWOULDBLOCK)
    return 0; /* no data available */
  return -1;  /* real error (EBADF etc.) */
}

static int fd_is_writable(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return (flags != -1) ? 1 : -1;
}

/* -- non-blocking pass --------------------------------------------------- */

static int poll_check(struct pollfd* fds, nfds_t nfds) {
  int ready = 0;
  for (nfds_t i = 0; i < nfds; i++) {
    fds[i].revents = 0;
    if (fds[i].fd < 0)
      continue;
    if ((fds[i].events & POLLIN) && fd_is_readable(fds[i].fd) > 0) {
      fds[i].revents |= POLLIN;
      ready++;
    }
    if ((fds[i].events & POLLOUT) && fd_is_writable(fds[i].fd) > 0) {
      fds[i].revents |= POLLOUT;
      ready++;
    }
  }
  return ready;
}

/* -- blocking-poll gate --------------------------------------------------
 *
 * While poll() is blocked in emscripten_sleep, the rAF tick (tick() in
 * tcldide-runtime.c) must NOT process Tcl events — otherwise it steals
 * events that the inner event loop (tkwait/vwait) is waiting for.
 * tick() checks this flag and returns immediately when set. */

static int g_in_blocking_poll = 0;

int emx11_is_blocking_in_poll(void) { return g_in_blocking_poll; }

/* -- async sleep via JSPI ------------------------------------------------- */

static void async_sleep(unsigned int ms) {
  /* emscripten_sleep suspends the wasm call via JSPI and yields to the
   * browser for ms milliseconds.  During the yield, the browser event
   * loop runs freely — DOM/X11 events arrive, rAF ticks fire
   * (emscripten_set_main_loop → tick() → Tcl_DoOneEvent), and the
   * self-pipe becomes readable.  When the sleep expires, execution
   * resumes after this call.
   *
   * From the C caller's perspective this is synchronous — poll() just
   * blocks for a while.  From the JS caller's perspective, the wasm
   * export returns a Promise when it suspends, so the JS side must
   * await it. */
  emscripten_sleep(ms);
}

/* -- poll() — blocking with async sleep ----------------------------------- */

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  int ready = poll_check(fds, nfds);
  if (ready > 0 || timeout == 0)
    return ready;

  /* Blocking path.  timeout > 0 → deadline; timeout < 0 → indefinite.
   * We yield to the browser via emscripten_sleep():
   *   - JSPI suspends the wasm call, browser event loop runs freely,
   *     DOM/X11 events arrive, rAF tick drains Tcl events.
   *   - When sleep expires, execution resumes, we re-check fds.
   *
   * Interval adapts to the deadline to balance latency and wasm↔JS
   * boundary-crossing overhead. */
  double deadline = (timeout > 0) ? emscripten_get_now() + (double)timeout : 0;

  for (;;) {
    unsigned int sleep_ms;
    if (timeout > 0) {
      double remaining = deadline - emscripten_get_now();
      if (remaining <= 0)
        return 0;
      if (remaining > 100)
        sleep_ms = 50;
      else if (remaining > 10)
        sleep_ms = 10;
      else
        sleep_ms = 1;
    } else {
      /* Infinite wait — sleep 10 ms between checks so we don't
       * flood the server with 1000 req/s per tkwait call. */
      sleep_ms = 10;
    }
    g_in_blocking_poll = 1;
    async_sleep(sleep_ms);
    g_in_blocking_poll = 0;

    ready = poll_check(fds, nfds);
    if (ready > 0)
      return ready;
    if (timeout > 0 && emscripten_get_now() >= deadline)
      return 0;
  }
}

/* -- select() — delegate to poll() with real timeout --------------------- */

int select(int nfds,
           fd_set* readfds,
           fd_set* writefds,
           fd_set* exceptfds,
           struct timeval* timeout) {
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

  int poll_timeout;
  if (!timeout) {
    poll_timeout = -1;
  } else if (timeout->tv_sec == 0 && timeout->tv_usec == 0) {
    poll_timeout = 0;
  } else {
    poll_timeout = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    if (poll_timeout < 1)
      poll_timeout = 1;
  }

  int ret = poll(pfds, pidx, poll_timeout);
  if (ret <= 0)
    return ret;

  /* Convert poll results back to fd_sets. */
  int count = 0;
  for (nfds_t i = 0; i < pidx; i++) {
    if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
      if (readfds) {
        FD_SET(pfds[i].fd, readfds);
        count++;
      }
    }
    if (pfds[i].revents & POLLOUT) {
      if (writefds) {
        FD_SET(pfds[i].fd, writefds);
        count++;
      }
    }
    if (pfds[i].revents & POLLPRI) {
      if (exceptfds) {
        FD_SET(pfds[i].fd, exceptfds);
        count++;
      }
    }
  }
  return count;
}
