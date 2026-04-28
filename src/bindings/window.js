/*
 * Emscripten --js-library: window lifecycle / structure bridges.
 * Mirrors xserver/dix/window.c request handlers (CreateWindow,
 * ConfigureWindow, MapWindow, UnmapWindow, DestroyWindow, ReparentWindow,
 * ChangeWindowAttributes) plus SelectInput and the Shape extension's
 * window region setter.
 */

addToLibrary({
  emx11_js_window_create: function (connId, id, parent, x, y, w, h, borderWidth, borderPixel, background) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onWindowCreate(connId, id, parent, x, y, w, h, borderWidth, borderPixel, background);
  },

  // Border-only update: width and/or pixel color. Geometry unchanged;
  // Host repaints the border ring.
  emx11_js_window_set_border: function (id, borderWidth, borderPixel) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onWindowSetBorder(id, borderWidth, borderPixel);
  },

  // Solid-background update (XSetWindowBackground, CWBackPixel). The
  // native side has already cleared any bound pixmap via window_set_bg_pixmap
  // if needed; here we update the solid colour so the next clearArea /
  // Expose paints with it.
  emx11_js_window_set_bg: function (id, background) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onWindowSetBg(id, background);
  },

  // Geometry-only update for an existing window. Unlike window_create
  // this preserves parent, shape, background_pixmap, and the Host-side
  // subscription table.
  emx11_js_window_configure: function (id, x, y, w, h) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowConfigure(id, x, y, w, h);
  },

  emx11_js_window_map: function (connId, id) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowMap(connId, id);
  },

  emx11_js_window_unmap: function (connId, id) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowUnmap(connId, id);
  },

  emx11_js_window_destroy: function (id) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowDestroy(id);
  },

  // Mirror XSelectInput into Host so it can track redirect / notify
  // holders (x11protocol.txt §1477: at most one SubstructureRedirect
  // claim per window). Host dedupes per (window, connection).
  emx11_js_select_input: function (connId, id, mask) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onSelectInput(connId, id, mask >>> 0);
  },

  // Set override_redirect on a window. Non-zero flag = True.
  emx11_js_set_override_redirect: function (id, flag) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onSetOverrideRedirect(id, flag !== 0);
  },

  // XReparentWindow: re-home a window under a new parent. Always
  // forwarded (cross-connection reparents are legal, e.g. twm takes
  // xeyes's shell as a child of its decoration frame).
  emx11_js_reparent_window: function (id, parent, x, y) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onReparentWindow(id, parent, x, y);
  },

  // Bind (or unbind, if pmId==0) a Pixmap as the window's tiled
  // background. The compositor will thereafter paint the window's
  // background using ctx.createPattern(pixmap.canvas, 'repeat') with
  // the tile origin aligned to the window's top-left.
  emx11_js_window_set_bg_pixmap: function (id, pmId) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onWindowSetBgPixmap(id, pmId);
  },

  // XShape extension: set the window's bounding region from a list of
  // axis-aligned rectangles (1px tall after host's row-RLE of a mask).
  emx11_js_window_shape: function (id, rectsPtr, count) {
    if (!globalThis.__EMX11__) return;
    var rects = [];
    if (count > 0 && rectsPtr !== 0) {
      var base = rectsPtr >> 2;
      for (var i = 0; i < count; i++) {
        rects.push({
          x: HEAP32[base + i * 4 + 0],
          y: HEAP32[base + i * 4 + 1],
          w: HEAP32[base + i * 4 + 2],
          h: HEAP32[base + i * 4 + 3],
        });
      }
    }
    globalThis.__EMX11__.onWindowShape(id, rects);
  },
});
