/*
 * Emscripten --js-library: core lifecycle bridges (init / open_display /
 * close_display / get_root_window / flush / pointer_xy / get_window_attrs).
 *
 * Each function bridges a C call into the globalThis.__EMX11__ host object
 * installed by src/host/index.ts. Installing the host must happen BEFORE
 * the wasm module starts, i.e. before awaiting the Emscripten factory.
 *
 * Syntax note: this file is consumed by the Emscripten linker, not Vite.
 * It uses the pre-ESM addToLibrary dialect (no imports, no TS).
 */

addToLibrary({
  emx11_js_init: function (screenWidth, screenHeight) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onInit(screenWidth, screenHeight);
  },

  // Connection setup. Host returns a connection id (used to route events
  // back to this Module once we go multi-client) and an XID range
  // carved out per x11protocol.txt §869/§935 -- the client ORs its
  // counter into `base`, limited to bits in `mask`, and every allocated
  // XID is globally unique.
  emx11_js_open_display: function (connIdPtr, basePtr, maskPtr) {
    if (!globalThis.__EMX11__) {
      // No Host installed: hand back a harmless single-client range so
      // test harnesses / headless runs still work.
      HEAP32[connIdPtr >> 2] = 0;
      HEAPU32[basePtr >> 2] = 0;
      HEAPU32[maskPtr >> 2] = 0x001FFFFF;
      return;
    }
    var info = globalThis.__EMX11__.openDisplay();
    HEAP32[connIdPtr >> 2] = info.connId | 0;
    HEAPU32[basePtr >> 2] = info.xidBase >>> 0;
    HEAPU32[maskPtr >> 2] = info.xidMask >>> 0;
  },

  emx11_js_close_display: function (connId) {
    globalThis.__EMX11__ && globalThis.__EMX11__.closeDisplay(connId);
  },

  // Shared root window. Every client's XOpenDisplay learns the root's
  // XID this way rather than minting its own: one root, one compositor
  // entry, one weave. Returns a 32-bit XID (always Host-reserved low
  // range, top 3 bits zero per x11protocol.txt §935).
  emx11_js_get_root_window: function () {
    if (!globalThis.__EMX11__) return 0;
    return globalThis.__EMX11__.getRootWindow() >>> 0;
  },

  emx11_js_flush: function () {
    globalThis.__EMX11__ && globalThis.__EMX11__.onFlush();
  },

  // Last-known pointer position (root-relative canvas CSS pixels).
  // XQueryPointer polls this; the host maintains it off a canvas
  // mousemove listener that runs regardless of which window is under
  // the cursor.
  emx11_js_pointer_xy: function (xPtr, yPtr) {
    if (!globalThis.__EMX11__) {
      HEAP32[xPtr >> 2] = 0;
      HEAP32[yPtr >> 2] = 0;
      return;
    }
    var pt = globalThis.__EMX11__.getPointerXY();
    HEAP32[xPtr >> 2] = pt.x | 0;
    HEAP32[yPtr >> 2] = pt.y | 0;
  },

  // Cross-connection XGetWindowAttributes fallback. C calls this when
  // its local EmxWindow table has no shadow for the queried XID (twm
  // asking for xeyes's shell, canonical WM case). Host returns the
  // authoritative compositor record.
  // Layout of the 8-int buffer at outPtr:
  //   [0] found (0/1)  [1] x       [2] y
  //   [3] width        [4] height  [5] mapped (0/1)
  //   [6] override_redirect (0/1)  [7] border_width
  emx11_js_get_window_attrs: function (id, outPtr) {
    var base = outPtr >> 2;
    if (!globalThis.__EMX11__) {
      HEAP32[base] = 0;
      return;
    }
    var a = globalThis.__EMX11__.getWindowAttrs(id >>> 0);
    if (!a) {
      HEAP32[base] = 0;
      return;
    }
    HEAP32[base + 0] = 1;
    HEAP32[base + 1] = a.x | 0;
    HEAP32[base + 2] = a.y | 0;
    HEAP32[base + 3] = a.width | 0;
    HEAP32[base + 4] = a.height | 0;
    HEAP32[base + 5] = a.mapped ? 1 : 0;
    HEAP32[base + 6] = a.overrideRedirect ? 1 : 0;
    HEAP32[base + 7] = a.borderWidth | 0;
  },

  // Cross-connection absolute-origin lookup. C-side window_abs_origin
  // uses this when its local parent chain ends in a None (because the
  // ancestor is owned by another connection -- e.g. a twm frame under
  // which xcalc was reparented). Returns 3 ints:
  //   [0] found (0/1)  [1] ax  [2] ay
  // ax/ay are the cumulative root-relative origin from the Host's
  // authoritative tree. See Host.getWindowAbsOrigin for the why.
  emx11_js_get_window_abs_origin: function (id, outPtr) {
    var base = outPtr >> 2;
    if (!globalThis.__EMX11__) {
      HEAP32[base] = 0;
      return;
    }
    var o = globalThis.__EMX11__.getWindowAbsOrigin(id >>> 0);
    if (!o) {
      HEAP32[base] = 0;
      return;
    }
    HEAP32[base + 0] = 1;
    HEAP32[base + 1] = o.ax | 0;
    HEAP32[base + 2] = o.ay | 0;
  },
});
