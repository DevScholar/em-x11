/*
 * Emscripten --js-library: GC drawing bridges. Mirrors xserver/dix/gc.c's
 * core protocol drawing requests (PolyFillRectangle, PolyLine, PolyArc,
 * PolyFillArc, FillPoly, PolyPoint, ClearArea). The C side has already
 * resolved the GC's foreground / line width into the `color` / `lineWidth`
 * arguments; Host dispatches to compositor (windows) or pixmap ctx
 * depending on the drawable's identity.
 */

addToLibrary({
  // XClearWindow / XClearArea entry point. Lets the compositor decide
  // whether to paint with background_pixel (solid) or background_pixmap
  // (tiled) so the C side doesn't have to branch.
  emx11_js_clear_area: function (id, x, y, w, h) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onClearArea(id, x, y, w, h);
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
});
