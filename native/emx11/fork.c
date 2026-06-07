/*
 * fork/vfork/posix_spawn for em-x11's cooperative model.
 *
 * == Why this file exists ==
 *
 * Emscripten's libc stubs fork() to return -1/ENOSYS and has no
 * posix_spawn().  Many real-world programs (shells, script interpreters,
 * init systems) depend on fork+exec or posix_spawn to create child
 * processes.  This file provides the best possible approximations
 * within the browser/wasm sandbox.
 *
 * == fork() ==
 *
 * True fork() requires copy-on-write duplication of the entire wasm
 * linear memory + stack + heap + all handles (DOM, canvas, WebGL
 * contexts), which is impossible in the browser.  We return -1/ENOSYS
 * with a clear errno so callers can fall back to vfork() or posix_spawn().
 *
 * == vfork() ==
 *
 * POSIX vfork() semantics: the parent is SUSPENDED until the child
 * either calls exec*() or _exit().  The child borrows the parent's
 * address space.  We implement this with setjmp/longjmp:
 *
 *   1. vfork() calls setjmp() to save the parent's resume point.
 *   2. setjmp returns 0 → child path.  Child runs in the same wasm
 *      context with vfork_in_progress=1.
 *   3a. Child calls exec*() → process.c's execvp override fires:
 *       emx11_js_exec_self tells the JS host to kill+respawn this
 *       connection.  vfork state is cleared first so exit() kills
 *       the wasm module cleanly.
 *   3b. Child calls _exit() → __wrap__exit detects vfork_in_progress
 *       and longjmps back to vfork()'s setjmp, which returns the
 *       exit status.  Parent resumes.
 *
 * The linker needs: -Wl,--wrap=_exit
 * (so __wrap__exit in this file intercepts _exit calls from vfork children)
 *
 * == posix_spawn() ==
 *
 * posix_spawn() is the modern, race-free replacement for fork+exec.
 * It maps directly to the em-x11 model: create a new wasm Module
 * instance.  We provide:
 *
 *   posix_spawn()    — spawn with explicit path
 *   posix_spawnp()   — spawn with PATH search (delegated to JS host)
 *
 * Both take a posix_spawn_file_actions_t and posix_spawnattr_t for
 * API compatibility; we only honour the argv/envp subset for now.
 */

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <emscripten.h>

/* ---- vfork ------------------------------------------------------------- */

static jmp_buf vfork_jmp;
static volatile int g_vfork_active = 0;

pid_t vfork(void) {
  if (g_vfork_active) {
    errno = EAGAIN;
    return -1;
  }

  int r = setjmp(vfork_jmp);
  if (r == 0) {
    g_vfork_active = 1;
    return 0; /* child path */
  }

  /* Parent path — child called _exit(r).  r is at least 1 because
   * longjmp(jmp, 0) is impossible (setjmp would return 1).  Map
   * back to the exit status for waitpid() compatibility. */
  g_vfork_active = 0;
  return 1; /* fake child pid — positive to distinguish from error */
}

/* Clear vfork state before exec().  Called from process.c's do_exec()
 * so exit() kills the wasm module instead of longjmp'ing back. */
void emx11_vfork_clear(void) { g_vfork_active = 0; }

int emx11_vfork_active(void) { return g_vfork_active; }

/* Intercept _exit when called from a vfork child.
 *
 * Emscripten's libc _exit is a strong symbol, so we need
 * -Wl,--wrap=_exit on the final link line.  This TU provides
 * __wrap__exit; the linker redirects all _exit calls here and
 * provides __real__exit for the normal path. */

/* Forward-declared: the linker supplies this. */
extern void __real__exit(int status);

void __wrap__exit(int status) {
  if (g_vfork_active) {
    g_vfork_active = 0;
    /* longjmp back to vfork(), which returns `status ? status : 1`.
     * If status is 0 we pass 1 so setjmp can distinguish it from
     * the initial 0 return. */
    longjmp(vfork_jmp, status ? status : 1);
    /* unreachable */
  }
  __real__exit(status);
}

/* fork() — not possible in wasm.  Return a clear error so callers
 * that check errno can fall back gracefully. */
pid_t fork(void) {
  errno = ENOSYS;
  return -1;
}

/* ---- posix_spawn ------------------------------------------------------- */

EM_JS(
  int,
  emx11_js_posix_spawn,
  (int pathPtr, int pathLen, int argvPtrs, int argc, int envpPtrs, int envc),
  {
    /* Decode path. */
    var path = pathPtr ? UTF8ToString(pathPtr, pathLen) : '';

    /* Decode argv: array of char* in HEAP32. */
    var args = [];
    if (argvPtrs != 0 && argc > 0) {
      var base = argvPtrs >> 2;
      for (var i = 0; i < argc; i++) {
        var p = HEAPU32[base + i] >>> 0;
        args.push(p == 0 ? '' : UTF8ToString(p));
      }
    }

    /* Decode envp: same layout. */
    var env = [];
    if (envpPtrs != 0 && envc > 0) {
      var ebase = envpPtrs >> 2;
      for (var i = 0; i < envc; i++) {
        var p = HEAPU32[ebase + i] >>> 0;
        env.push(p == 0 ? '' : UTF8ToString(p));
      }
    }

    var host = Module['emx11Host'];
    if (!host || !host.onPosixSpawn) {
      errno = 38; /* ENOSYS */
      return -1;
    }

    /* The host spawns a new wasm module and returns its connId as pid.
     * This is synchronous from the caller's perspective: the host queues
     * the spawn and returns the pid immediately; the child boots async. */
    try {
      return host.onPosixSpawn(path, args, env) | 0;
    } catch (e) {
      console.error('[emx11] posix_spawn failed:', e);
      errno = 38; /* ENOSYS */
      return -1;
    }
  });

static int count_strings(char* const* arr) {
  int n = 0;
  if (arr)
    while (arr[n])
      n++;
  return n;
}

int posix_spawn(pid_t* pid,
                const char* path,
                const posix_spawn_file_actions_t* file_actions,
                const posix_spawnattr_t* attrp,
                char* const argv[],
                char* const envp[]) {
  (void)file_actions;
  (void)attrp;

  if (!pid || !path || !argv) {
    errno = EFAULT;
    return -1;
  }

  int argc = count_strings(argv);
  int envc = count_strings(envp);

  /* Build argv pointer array for the bridge.  argv[0] is the program
   * name by POSIX convention. */
  int result = emx11_js_posix_spawn((int)(intptr_t)path,
                                    (int)strlen(path),
                                    (int)(intptr_t)argv,
                                    argc,
                                    (int)(intptr_t)envp,
                                    envc);

  if (result < 0)
    return -1;

  *pid = result;
  return 0;
}

int posix_spawnp(pid_t* pid,
                 const char* file,
                 const posix_spawn_file_actions_t* file_actions,
                 const posix_spawnattr_t* attrp,
                 char* const argv[],
                 char* const envp[]) {
  /* PATH search is handled on the JS host side — we pass the bare
   * file name and the host resolves it against its binary registry.
   * For now, delegate to posix_spawn (same bridge). */
  return posix_spawn(pid, file, file_actions, attrp, argv, envp);
}
