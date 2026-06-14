# em-x11 libc overrides

em-x11 replaces a subset of Emscripten's libc with its own implementations
suited to the browser/wasm sandbox. Two strategies are used:

- **Strong symbol**: a function with the same name is defined in `libX11.a`.
  User `.o`/`.a` files are searched before libc, so the override wins at link
  time with no extra flags.
- **Linker wrap**: the final link line needs `-Wl,--wrap=<name>`. Every call
  site is redirected to `__wrap_<name>`; the original libc function remains
  reachable as `__real_<name>`.

Both wraps are baked into the `em_x11_finalize_demo` macro in
[cmake/em_x11_demo.cmake](../cmake/em_x11_demo.cmake), so downstream demos
get them automatically.

### Three consumer profiles

| Consumer | Link style | `--wrap=read` | `--wrap=_exit` | Overrides active |
|---|---|---|---|---|
| em-x11 demos (twm, xcalc, glxgears) | `em_x11_finalize_demo` via port script | yes | yes | all (poll + signals + process + fork) |
| tcldide-runtime-tk | `--use-port` via CMake | no | no | poll + signals + threading; process/fork/wrap dead code |
| tcldide-runtime-base | `tcl-poll.c` (trimmed copy) | no | no | poll/select only (no Display fd, no signals, no pushback) |
| pyodide-tk side modules | `--whole-archive` relink into .so | no | no | poll + signals + threading; process/fork/wrap dead code |

The `--wrap` flags are **only** passed by `em_x11_finalize_demo`.  In the
tcldide and pyodide-tk paths, `__wrap_read` and `__wrap__exit` are dead code
— they exist in the archive but are never called because no call site is
redirected to them.

---

## poll / select ([poll.c](../native/em_x11/poll.c)) — strong symbols

**Necessity**: core-essential for all consumers. Every Tcl/Tk event loop calls
`select()`; without these overrides it never yields to the browser and
busy-loops. tcldide-runtime-base uses a trimmed copy
([tcl-poll.c](../../tcldide/runtime/tcl-poll.c)) without Display-fd
handling, pushback buffer, or signal delivery. tcldide-runtime-tk and
pyodide-tk get the full version via libX11.a / libX11.so.

Emscripten's `poll`/`select` lower to `__syscall_poll`, which only does a
non-blocking fd-readiness check. The em-x11 versions use JSPI
`emscripten_sleep()` to yield to the browser event loop, providing real
blocking semantics.

| Function | Strategy | Consumers | Notes |
|---|---|---|---|
| `poll` | strong | all | Adaptive polling intervals (1/2/5/10 ms), supports infinite timeout |
| `ppoll` | strong | all | Same + `struct timespec` timeout + `sigmask` parameter compat |
| `select` | strong | all | Converts fd_sets → pollfds, delegates to `poll()`, converts back |
| `pselect` | strong | all | Same + `struct timespec` timeout + `sigmask` parameter compat |
| `__wrap_read` | **wrap** | em-x11 demos only | Drains the 1-byte pushback buffer that poll's FIONREAD fallback consumed |

`__wrap_read` is only active when `-Wl,--wrap=read` is on the link line,
which only `em_x11_finalize_demo` injects. Even then, it only triggers for
non-Display fds — Display fds go through the non-destructive ring-buffer
path. In the tcldide and pyodide-tk paths, `__wrap_read` is dead code.

**Blocking-poll gate**: `g_in_blocking_poll` prevents the outer rAF tick from
stealing events that an inner event loop (tkwait/vwait) is waiting for.
Used by tcldide-runtime-tk's `tick()` via `em_x11_is_blocking_in_poll()`.

**Display fd registry**: `em_x11_poll_register_display_fd()` maps self-pipe
read ends to `Display*`. poll checks the ring buffer directly
(`event_head != event_tail`) rather than reading the pipe, preserving the
Linux poll() pure-observer contract.

---

## Signals ([signal.c](../native/em_x11/signal.c)) — strong symbols

**Necessity**: core-essential. `em_x11_deliver_pending_signals()` is called
from `poll.c`, `event_queue.c`, and `notifier.c` at every cooperative yield
point. Without it those call sites would not compile. The `signal`/`sigaction`
overrides replace Emscripten's own signal machinery so the handler table is
owned by em-x11 and `em_x11_deliver_pending_signals` can dispatch correctly.

The browser has no kernel signal delivery, and wasm instructions cannot be
interrupted asynchronously. em-x11 delivers signals at cooperative yield
points (every `emscripten_sleep` return, every rAF tick return).

| Function | Strategy | Consumers | Notes |
|---|---|---|---|
| `signal` | strong | all | Own handler table; does not touch Emscripten's internal signal machinery |
| `sigaction` | strong | all | Same, honors `SA_RESETHAND` |
| `raise` | strong | all | Sets pending flag + synchronous delivery |
| `kill` | strong | all (cross-proc: twm only) | Same process → `raise()`; cross-process → forwarded to JS host via `onSignalDeliver` |
| `sigprocmask` | strong | all | API-compat no-op; signals are never truly blocked |
| `sigemptyset` | strong | all | |
| `sigfillset` | strong | all | |
| `sigaddset` | strong | all | |
| `sigdelset` | strong | all | |
| `sigismember` | strong | all | |
| `em_x11_deliver_pending_signals` | strong (internal) | all | Called after every yield in poll.c, event_queue.c, notifier.c |
| `em_x11_signal_set_pending` | strong (internal) | all | Called by `raise()`, ccall'd by JS host via `onSignalDeliver` |
| `em_x11_signal_on_child_exit` | EMSCRIPTEN_KEEPALIVE | **uncalled** | Entry point for JS host; no current caller |
| `em_x11_signal_on_alarm` | EMSCRIPTEN_KEEPALIVE | **uncalled** | Entry point for JS host; no current caller |
| `em_x11_signal_on_interrupt` | EMSCRIPTEN_KEEPALIVE | **uncalled** | Entry point for JS host; no current caller |
| `em_x11_signal_on_terminate` | EMSCRIPTEN_KEEPALIVE | **uncalled** | Entry point for JS host; no current caller |
| `em_x11_signal_on_pipe` | EMSCRIPTEN_KEEPALIVE | **uncalled** | Entry point for JS host; no current caller |

The five `em_x11_signal_on_*` EMSCRIPTEN_KEEPALIVE functions are not
currently called by any JS code.  The only cross-process signal path in use is
the host calling `em_x11_signal_set_pending` directly via ccall (see
`onSignalDeliver` in [host/index.ts](../src/host/index.ts)).  The wrappers
exist as documented API surface — the host *can* call them, and they're small
enough (~2 bytes each) that removing them isn't worth the risk of breaking a
future host implementation that might use them.

**Supported signals**: `SIGALRM`, `SIGCHLD`, `SIGINT`, `SIGPIPE`, `SIGTERM`,
`SIGUSR1`, `SIGUSR2`

**Delivery points**: `em_x11_deliver_pending_signals()` is called after every
yield return in poll.c, notifier.c, and event_queue.c. Re-entrant delivery
is prevented by the `g_in_delivery` guard.

---

## Process management ([fork.c](../native/em_x11/fork.c) + [process.c](../native/em_x11/process.c)) — strong symbols + wrap

**Necessity**: conditional — only twm (F_RESTART menu) and mwm use the full
vfork+exec pipeline.  In the tcldide and pyodide-tk paths, `process.c` and
large parts of `fork.c` are dead code.  They remain in `libX11.a` because the
archive is shared across all consumers; the linker only pulls in object files
that resolve referenced symbols.

Emscripten's libc `fork` is a stub that returns `-1/ENOSYS`; its `exec*`
functions are *weak* stubs returning `-1/ENOSYS`.  em-x11 provides strong
definitions that replace the weak stubs when linked.

| Function | Strategy | File | Consumers | Notes |
|---|---|---|---|---|
| `fork` | strong | fork.c | _redundant_ | Returns `-1/ENOSYS` — **identical to Emscripten's own stub**. No value added. |
| `vfork` | strong | fork.c | twm only | setjmp/longjmp simulation: child borrows parent address space until `exec` or `_exit` |
| `posix_spawn` | strong | fork.c | twm (menu spawn) | JS host creates a new wasm Module instance |
| `posix_spawnp` | strong | fork.c | twm (menu spawn) | Same, PATH search handled by JS host |
| `execvp` | strong | process.c | twm, mwm | Tells JS host to kill+respawn the current connection |
| `execv` | strong | process.c | twm, mwm | Same |
| `execvpe` | strong | process.c | twm, mwm | Same |
| `__wrap__exit` | **wrap** | fork.c | twm only | vfork child calling `_exit` longjmps back to parent instead of killing wasm |

**vfork flow** (only twm F_RESTART exercises this):
1. `vfork()` calls `setjmp()` to save the parent resume point.
2. `setjmp` returns 0 → child path, `g_vfork_active=1`.
3. Child calls `exec*()` → process.c clears vfork state then exits; JS host respawns.
4. Child calls `_exit()` → `__wrap__exit` detects `g_vfork_active`, longjmps back
   to vfork's setjmp.
5. `setjmp` returns non-zero → parent resumes.

`__wrap__exit` is only active when `-Wl,--wrap=_exit` is on the link line,
which only `em_x11_finalize_demo` injects.  In the tcldide and pyodide-tk
paths, `__wrap__exit` is dead code — no call site is redirected to it.

**Current consumers**: twm (F_RESTART menu uses `execvp` + `vfork`; menu
launches use `posix_spawn`), mwm (WmFunction.c uses `execvp`).

---

## GL display-list compat ([gl_displaylist.c](../examples/glxgears/gl_displaylist.c)) — app-level wrap

**Necessity**: conditional — only the glxgears demo links this file. Not part of
`libX11.a`. Emscripten's `LEGACY_GL_EMULATION` omits display lists, so this
file fills the gap with a record-and-replay scheme using `--wrap`.

| Function | Strategy |
|---|---|
| `__wrap_glBegin` | wrap |
| `__wrap_glEnd` | wrap |
| `__wrap_glVertex3f` | wrap |
| `__wrap_glNormal3f` | wrap |
| `__wrap_glShadeModel` | wrap |
| `__wrap_glMaterialfv` | wrap |

During recording (`glNewList`–`glEndList`), calls append to a per-list
command buffer. Replay and direct calls share the same exec path, including
the `AMBIENT_AND_DIFFUSE` split and deferred-`glNormal3f` workarounds.

---

## Master table

Necessity key: **E** = essential (all consumers), **D** = demo-only (only
em-x11 demos via `em_x11_finalize_demo`), **T** = twm/mwm only,
**U** = currently uncalled, **R** = redundant with Emscripten libc.

| Category | Function | Mechanism | File | Necessity |
|---|---|---|---|---|
| I/O | `poll` | strong | poll.c | E |
| I/O | `ppoll` | strong | poll.c | E |
| I/O | `select` | strong | poll.c | E |
| I/O | `pselect` | strong | poll.c | E |
| I/O | `read` | `--wrap=read` | poll.c | D |
| Signal | `signal` | strong | signal.c | E |
| Signal | `sigaction` | strong | signal.c | E |
| Signal | `raise` | strong | signal.c | E |
| Signal | `kill` | strong | signal.c | E |
| Signal | `sigprocmask` | strong | signal.c | E |
| Signal | `sigemptyset` | strong | signal.c | E |
| Signal | `sigfillset` | strong | signal.c | E |
| Signal | `sigaddset` | strong | signal.c | E |
| Signal | `sigdelset` | strong | signal.c | E |
| Signal | `sigismember` | strong | signal.c | E |
| Signal | `em_x11_deliver_pending_signals` | strong (internal) | signal.c | E |
| Signal | `em_x11_signal_set_pending` | strong (internal) | signal.c | E |
| Signal | `em_x11_signal_on_*` (5×) | EMSCRIPTEN_KEEPALIVE | signal.c | U |
| Process | `fork` | strong | fork.c | R |
| Process | `vfork` | strong | fork.c | T |
| Process | `posix_spawn` | strong | fork.c | T |
| Process | `posix_spawnp` | strong | fork.c | T |
| Process | `_exit` | `--wrap=_exit` | fork.c | D, T |
| Process | `execvp` | strong | process.c | T |
| Process | `execv` | strong | process.c | T |
| Process | `execvpe` | strong | process.c | T |
| GL | `glBegin`/`glEnd`/… | `--wrap=` (app) | gl_displaylist.c | glxgears only |

### Necessity summary

- **E (essential)**: Remove these and Tcl/Tk event loops break — busy-loops,
  signals don't dispatch, or the build fails on undefined symbols.
- **D (demo-only)**: Only active when `--wrap` flags are set, which only
  `em_x11_finalize_demo` does. Dead code in tcldide and pyodide-tk paths.
- **T (twm/mwm)**: Only twm's F_RESTART menu and mwm's restart path need these.
- **U (uncalled)**: EMSCRIPTEN_KEEPALIVE entry points with no current JS
  caller. Small enough to keep as documented API surface.
- **R (redundant)**: `fork()` in fork.c returns -1/ENOSYS — identical to
  Emscripten's own stub. Provides zero additional value.
