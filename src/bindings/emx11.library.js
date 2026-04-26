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

  emx11_js_window_create: function (connId, id, parent, x, y, w, h, background) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onWindowCreate(connId, id, parent, x, y, w, h, background);
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

  emx11_js_flush: function () {
    globalThis.__EMX11__ && globalThis.__EMX11__.onFlush();
  },

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

  // Property storage lives Host-side (dix/property.c layout: keyed by
  // XID + atom, cross-client readable). Four bridges mirror the four
  // Xlib entry points we support.

  // XChangeProperty. mode: 0=Replace, 1=Prepend, 2=Append (X.h).
  // Returns 1 on success, 0 on BadMatch (format/type mismatch with
  // existing entry under Prepend/Append), -1 on BadWindow (unknown XID).
  emx11_js_change_property: function (w, atom, type, format, mode, dataPtr, nelements) {
    if (!globalThis.__EMX11__) return -1;
    var unit = format === 8 ? 1 : format === 16 ? 2 : format === 32 ? 4 : 0;
    if (unit === 0) return 0;
    var bytes = unit * (nelements | 0);
    var data = bytes > 0
      ? HEAPU8.slice(dataPtr, dataPtr + bytes)
      : new Uint8Array(0);
    var ok = globalThis.__EMX11__.changeProperty(
      w >>> 0, atom >>> 0, type >>> 0, format | 0, mode | 0, data,
    );
    return ok ? 1 : 0;
  },

  // XGetWindowProperty -- two-call pattern so C can allocate the
  // return buffer itself. First call returns meta; second call fills
  // a caller-provided byte buffer. delete_flag is honoured on the
  // second call (when bytesAfter == 0 and longOffset == 0).
  //
  // metaPtr is an int[8]:
  //   [0] found (0/1)       [1] actual_type   [2] actual_format
  //   [3] nitems_returned   [4] bytes_after   [5] data_bytes
  //   [6] valid (1 if window exists, 0 = BadWindow)
  //   [7] reserved
  // When valid=0 the caller returns BadWindow. When valid=1 && found=0,
  // the atom isn't set on that window (Success with type=None).
  emx11_js_get_property_meta: function (w, atom, reqType, longOffset, longLength, metaPtr) {
    var base = metaPtr >> 2;
    for (var i = 0; i < 8; i++) HEAP32[base + i] = 0;
    if (!globalThis.__EMX11__) return;
    var r = globalThis.__EMX11__.peekProperty(
      w >>> 0, atom >>> 0, reqType >>> 0,
      longOffset | 0, longLength | 0, false,
    );
    if (r === null) {
      /* BadWindow */
      HEAP32[base + 6] = 0;
      return;
    }
    HEAP32[base + 6] = 1;
    HEAP32[base + 0] = r.found ? 1 : 0;
    HEAP32[base + 1] = r.type | 0;
    HEAP32[base + 2] = r.format | 0;
    HEAP32[base + 3] = r.nitems | 0;
    HEAP32[base + 4] = r.bytesAfter | 0;
    HEAP32[base + 5] = r.data.length | 0;
  },

  // Second half of the two-call. Writes up to `capacity` bytes of the
  // sliced property data into dstPtr, and deletes the entry if
  // deleteFlag and the slice covers the whole entry (matches
  // dix/property.c ProcGetProperty semantics).
  emx11_js_get_property_data: function (w, atom, reqType, longOffset, longLength, deleteFlag, dstPtr, capacity) {
    if (!globalThis.__EMX11__) return;
    var r = globalThis.__EMX11__.peekProperty(
      w >>> 0, atom >>> 0, reqType >>> 0,
      longOffset | 0, longLength | 0, deleteFlag !== 0,
    );
    if (!r || !r.found || r.data.length === 0) return;
    var n = Math.min(r.data.length, capacity | 0);
    HEAPU8.set(r.data.subarray(0, n), dstPtr);
  },

  emx11_js_delete_property: function (w, atom) {
    globalThis.__EMX11__ && globalThis.__EMX11__.deleteProperty(w >>> 0, atom >>> 0);
  },

  // Returns property count; caller then calls fetch with a buffer
  // sized accordingly.
  emx11_js_list_properties_count: function (w) {
    if (!globalThis.__EMX11__) return 0;
    return globalThis.__EMX11__.listProperties(w >>> 0).length;
  },

  emx11_js_list_properties_fetch: function (w, dstPtr, capacity) {
    if (!globalThis.__EMX11__) return 0;
    var atoms = globalThis.__EMX11__.listProperties(w >>> 0);
    var n = Math.min(atoms.length, capacity | 0);
    var base = dstPtr >> 2;
    for (var i = 0; i < n; i++) HEAPU32[base + i] = atoms[i] >>> 0;
    return n;
  },

  // Pixmap lifecycle: each allocation gets its own OffscreenCanvas.
  // Drawing calls that target this id land on the pixmap's ctx via
  // the same emx11_js_fill_rect / fill_arc bindings (the host
  // dispatches).
  emx11_js_pixmap_create: function (id, width, height, depth) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onPixmapCreate(id, width, height, depth);
  },

  emx11_js_pixmap_destroy: function (id) {
    globalThis.__EMX11__ && globalThis.__EMX11__.onPixmapDestroy(id);
  },

  // XShapeCombineMask(dest_window, src_pixmap): read the pixmap's
  // ImageData, convert opaque cells into row strips, and install the
  // resulting region on the destination window.
  emx11_js_shape_combine_mask: function (destId, srcId, xOff, yOff, op) {
    globalThis.__EMX11__ &&
      globalThis.__EMX11__.onShapeCombineMask(destId, srcId, xOff, yOff, op);
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
