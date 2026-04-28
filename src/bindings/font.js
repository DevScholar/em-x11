/*
 * Emscripten --js-library: text and colour bridges. ImageText / PolyText
 * rendering plus font-metrics queries (XQueryFont, XTextWidth) and named
 * colour resolution (XParseColor's bare-name path). All three share a
 * lazily-created OffscreenCanvas 2D context for measurement -- it sits on
 * globalThis.__emx11_measureCtx__ so the cost amortises across calls.
 */

addToLibrary({
  emx11_js_draw_string: function (id, x, y, fontPtr, textPtr, length, fg, bg, imageMode) {
    if (!globalThis.__EMX11__) return;
    var font = fontPtr !== 0 ? UTF8ToString(fontPtr) : '13px monospace';
    // X strings are not NUL-terminated; pass explicit length.
    var text = length > 0 && textPtr !== 0 ? UTF8ToString(textPtr, length) : '';
    globalThis.__EMX11__.onDrawString(id, x, y, font, text, fg, bg, imageMode);
  },

  // Font measurement. Runs in a lazily-created offscreen 2D context so we
  // don't depend on a DOM canvas being mounted (helps headless tests too).
  // Writes directly into C buffers via HEAP32. willReadFrequently:true is
  // important here: emx11_js_parse_color shares this context and does
  // getImageData per colour, which without the hint forces a GPU->CPU
  // readback every call and makes Chrome log a performance warning.
  emx11_js_measure_font: function (fontPtr, ascentPtr, descentPtr, maxWidthPtr, widthsPtr) {
    var g = globalThis;
    if (!g.__emx11_measureCtx__) {
      var c =
        typeof OffscreenCanvas !== 'undefined'
          ? new OffscreenCanvas(1, 1)
          : typeof document !== 'undefined'
            ? document.createElement('canvas')
            : null;
      g.__emx11_measureCtx__ = c
        ? c.getContext('2d', { willReadFrequently: true })
        : null;
    }
    var ctx = g.__emx11_measureCtx__;
    var fallbackWidth = 8;
    var fallbackAscent = 10;
    var fallbackDescent = 3;

    if (!ctx) {
      HEAP32[ascentPtr >> 2] = fallbackAscent;
      HEAP32[descentPtr >> 2] = fallbackDescent;
      HEAP32[maxWidthPtr >> 2] = fallbackWidth;
      for (var i = 0; i < 95; i++) HEAP32[(widthsPtr >> 2) + i] = fallbackWidth;
      return;
    }

    var css = UTF8ToString(fontPtr);
    ctx.font = css;
    // Use a representative pair of glyphs for the font-level bbox.
    var refMetrics = ctx.measureText('Mg');
    var ascent =
      Math.ceil(
        refMetrics.fontBoundingBoxAscent ?? refMetrics.actualBoundingBoxAscent ?? fallbackAscent,
      ) || fallbackAscent;
    var descent =
      Math.ceil(
        refMetrics.fontBoundingBoxDescent ??
          refMetrics.actualBoundingBoxDescent ??
          fallbackDescent,
      ) || fallbackDescent;
    HEAP32[ascentPtr >> 2] = ascent;
    HEAP32[descentPtr >> 2] = descent;

    var maxW = 0;
    var base = widthsPtr >> 2;
    for (var j = 0; j < 95; j++) {
      var ch = String.fromCharCode(32 + j);
      var w = Math.ceil(ctx.measureText(ch).width) || fallbackWidth;
      if (w > maxW) maxW = w;
      HEAP32[base + j] = w;
    }
    HEAP32[maxWidthPtr >> 2] = maxW;
  },

  // Measure the pixel-advance of an arbitrary string in the given CSS
  // font. Handles Unicode, proportional fonts, ligatures -- whatever
  // canvas.fillText would produce, measureText agrees with.
  emx11_js_measure_string: function (fontPtr, textPtr, length) {
    if (length <= 0 || textPtr === 0) return 0;
    var g = globalThis;
    if (!g.__emx11_measureCtx__) {
      var c =
        typeof OffscreenCanvas !== 'undefined'
          ? new OffscreenCanvas(1, 1)
          : typeof document !== 'undefined'
            ? document.createElement('canvas')
            : null;
      g.__emx11_measureCtx__ = c
        ? c.getContext('2d', { willReadFrequently: true })
        : null;
    }
    var ctx = g.__emx11_measureCtx__;
    if (!ctx) return length * 8; // fallback
    ctx.font = fontPtr !== 0 ? UTF8ToString(fontPtr) : '13px monospace';
    var text = UTF8ToString(textPtr, length);
    return Math.ceil(ctx.measureText(text).width);
  },

  // Named-colour parse. XParseColor already handles "#RRGGBB" and
  // "rgb:R/G/B" in C; bare names (the ~600 entries of X11's rgb.txt --
  // "slategrey", "gray85", "rebeccapurple", etc.) we delegate to the
  // browser. CSS3's <named-color> set is literally the X11 table with
  // a few additions, so the browser's parser is authoritative. Returns
  // 1 on success with 16-bit R/G/B written to the caller's pointers
  // (matching XColor's field precision); 0 for invalid names.
  emx11_js_parse_color: function (namePtr, rPtr, gPtr, bPtr) {
    if (namePtr === 0) return 0;
    var name = UTF8ToString(namePtr);
    // Fast reject: non-colour values (currentcolor, transparent, bad
    // syntax) that fillStyle would silently ignore.
    if (typeof CSS !== 'undefined' && CSS.supports &&
        !CSS.supports('color', name)) {
      return 0;
    }
    var g = globalThis;
    if (!g.__emx11_measureCtx__) {
      var c =
        typeof OffscreenCanvas !== 'undefined'
          ? new OffscreenCanvas(1, 1)
          : typeof document !== 'undefined'
            ? document.createElement('canvas')
            : null;
      g.__emx11_measureCtx__ = c
        ? c.getContext('2d', { willReadFrequently: true })
        : null;
    }
    var ctx = g.__emx11_measureCtx__;
    if (!ctx) return 0;
    // Fallback validity check for browsers without CSS.supports:
    // pick a sentinel, read it back, then attempt the caller's name;
    // fillStyle silently retains the prior value for invalid inputs.
    ctx.fillStyle = '#010203';
    var sentinel = ctx.fillStyle;
    ctx.fillStyle = name;
    if (ctx.fillStyle === sentinel) return 0;
    ctx.clearRect(0, 0, 1, 1);
    ctx.fillRect(0, 0, 1, 1);
    var p = ctx.getImageData(0, 0, 1, 1).data;
    // Scale 8-bit to 16-bit the same way XParseColor's hex paths do.
    HEAPU16[rPtr >> 1] = (p[0] * 0x101) & 0xFFFF;
    HEAPU16[gPtr >> 1] = (p[1] * 0x101) & 0xFFFF;
    HEAPU16[bPtr >> 1] = (p[2] * 0x101) & 0xFFFF;
    return 1;
  },
});
