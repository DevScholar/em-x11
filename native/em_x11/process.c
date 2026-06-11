/*
 * Weak overrides for the exec*() family.
 *
 * Emscripten's libc ships execvp / execv / execvpe as stubs that
 * return -1 / ENOSYS: there's no kernel under wasm to swap an
 * address-space image. Linked into a demo wasm that opts in (only
 * twm right now), this TU replaces the stubs with a host-side
 * kill+respawn shim: the wasm tells the JS host "this connection
 * wants to re-exec with these argv", then exits cleanly. The host's
 * ProcessImpl observes the request via the onExecSelf bridge and
 * launches a fresh Module instance with the same glue URL.
 *
 * That gives twm's F_RESTART menu the visual semantics it expects --
 * the previous Reborder() pass's XConfigure on managed clients lines
 * up with a real process replacement, instead of accumulating an
 * unmatched title_height shift on every click.
 *
 * Emscripten's libc defines these symbols as weak stubs, so a plain
 * strong definition here wins at link time without a `__attribute__
 * ((weak))` annotation.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

extern int em_x11_current_conn_id(void);

/* Defined in bridges.c. Hands argv across to the host. */
extern void em_x11_js_exec_self(int conn_id, int argv_ptrs, int argc);

/* vfork state (see fork.c).  When execvp is called from a vfork child,
 * we must clear the vfork-in-progress flag so exit() kills the wasm
 * module instead of longjmp'ing back to the parent. */
extern void em_x11_vfork_clear(void);

static int do_exec(char* const argv[]) {
  int argc = 0;
  while (argv && argv[argc])
    argc++;
  /* Clear vfork state BEFORE exit(): we want the wasm to actually
   * terminate so the host can respawn with the new argv.  If we
   * longjmp back to the vfork parent, the exec-self message would
   * still be pending and the host would respawn a second process
   * on top of the resumed parent. */
  em_x11_vfork_clear();
  em_x11_js_exec_self(em_x11_current_conn_id(), (int)(intptr_t)argv, argc);
  /* Exit cleanly so the host's exit hook fires; ProcessImpl sees
   * the pending exec request and respawns. We don't return -- on a
   * successful real exec(), control transfers to the new image, so
   * callers don't expect us to come back either. */
  exit(0);
  return -1;
}

int execvp(const char* file, char* const argv[]) {
  (void)file;
  return do_exec(argv);
}

int execv(const char* path, char* const argv[]) {
  (void)path;
  return do_exec(argv);
}

int execvpe(const char* file, char* const argv[], char* const envp[]) {
  (void)file;
  (void)envp;
  return do_exec(argv);
}
