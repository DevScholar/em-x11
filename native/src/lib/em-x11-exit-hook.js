/* em-x11 exit hook — post-js that hooks Module.onExit so the host
 * tears down owned windows and pushes DestroyNotify to cross-conn parents
 * (twm) before the wasm module unwinds.
 *
 * In JSPI builds, Emscripten instantiates the wasm module before post-js
 * runs, so wasmImports._exit/exit are already captured by the wasm instance
 * and cannot be patched.  The JS-library _exit override is also dead —
 * Emscripten 5.0.3 hardcodes `var _exit = exitJS;` after library insertion,
 * overwriting whatever the library defined.  The C-side --wrap=_exit
 * (fork.c) is never reached because C exit() calls wasmImports.exit
 * directly, never touching the C _exit() symbol.
 *
 * The only interceptable point in the JSPI exit chain is Module.onExit,
 * which _proc_exit calls before throwing ExitStatus — provided
 * noExitRuntime is false (set by EmX11Host.init()). */

(function() {
  Module['onExit'] = function(code) {
    if (typeof EmX11Host !== 'undefined' && EmX11Host.connId !== 0) {
      var h = EmX11Host.get();
      if (h) {
        h.closeDisplay(EmX11Host.connId);
        EmX11Host.connId = 0;
      }
    }
  };
})();
