/*
 * Emscripten --js-library: implementations of the emx11_js_* extern symbols
 * declared in native/src/emx11_internal.h.
 *
 * Each function bridges a C call into the globalThis.__EMX11__ host object
 * installed by src/runtime/host.ts. Installing the host must happen BEFORE
 * the wasm module starts, i.e. before awaiting the Emscripten factory.
 *
 * Syntax note: this file is consumed by the Emscripten linker, not Vite.
 * It uses the pre-ESM addToLibrary dialect (no imports, no TS).
 */

addToLibrary({
  emx11_js_init: function (screenWidth, screenHeight) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onInit(screenWidth, screenHeight);
  },

  emx11_js_window_create: function (id, x, y, w, h, background) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onWindowCreate(id, x, y, w, h, background);
  },

  emx11_js_window_map: function (id) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowMap(id);
  },

  emx11_js_window_unmap: function (id) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowUnmap(id);
  },

  emx11_js_window_destroy: function (id) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowDestroy(id);
  },

  emx11_js_fill_rect: function (id, x, y, w, h, color) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onFillRect(id, x, y, w, h, color);
  },

  emx11_js_draw_line: function (id, x1, y1, x2, y2, color, lineWidth) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onDrawLine(id, x1, y1, x2, y2, color, lineWidth);
  },

  emx11_js_draw_arc: function (id, x, y, w, h, angle1, angle2, color, lineWidth) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onDrawArc(id, x, y, w, h, angle1, angle2, color, lineWidth);
  },

  emx11_js_fill_arc: function (id, x, y, w, h, angle1, angle2, color) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onFillArc(id, x, y, w, h, angle1, angle2, color);
  },

  emx11_js_fill_polygon: function (id, ptsPtr, count, shape, mode, color) {
    if (!globalThis.__EMX11__) return;
    var pts = [];
    if (count > 0 && ptsPtr !== 0) {
      var base = ptsPtr >> 2;
      for (var i = 0; i < count; i++) {
        pts.push({ x: HEAP32[base + i * 2], y: HEAP32[base + i * 2 + 1] });
      }
    }
    globalThis.__EMX11__.onFillPolygon(id, pts, shape, mode, color);
  },

  emx11_js_draw_points: function (id, ptsPtr, count, mode, color) {
    if (!globalThis.__EMX11__) return;
    var pts = [];
    if (count > 0 && ptsPtr !== 0) {
      var base = ptsPtr >> 2;
      for (var i = 0; i < count; i++) {
        pts.push({ x: HEAP32[base + i * 2], y: HEAP32[base + i * 2 + 1] });
      }
    }
    globalThis.__EMX11__.onDrawPoints(id, pts, mode, color);
  },

  emx11_js_flush: function () {
    globalThis.__EMX11__ && globalThis.__EMX11__.onFlush();
  },

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
