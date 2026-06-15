/**
 * @license
 * em-x11 Emscripten JS library.
 *
 * Provides the JS-side bridge functions that the C code in
 * native/em_x11/bridges.c calls. In the static-link path (-sUSE_EM_X11)
 * these override the EM_JS bodies in bridges.c; in the SIDE_MODULE path
 * (Pyodide dlopen) the EM_JS bodies provide the implementations and
 * this library is not used.
 *
 * Internal state lives under $EmX11Host (closure-private, NOT on Module).
 * User-configurable knobs are flat Module properties:
 *
 *   Module['emX11Host']           — pre-created Host instance (layers 2/3)
 *   Module['emX11ClipboardBytes'] — Uint8Array, staged before paste events
 *   Module['emX11NoAutoStart']    — skip auto-init (advanced users only)
 *
 * The Host (set via Module['emX11Host'] or by createEmX11()) must provide
 * the EmX11Host interface methods that each bridge function below
 * dispatches into.
 */

var LibraryEmX11 = {
  $EmX11Host__internal: true,
  $EmX11Host__postset: 'EmX11Host.init();',
  $EmX11Host: {
    bridge: null,
    /** Emscripten Module reference, captured during init() when it is
     *  guaranteed to be live. Bridge functions must use this.module and
     *  NEVER bare `Module` — in some Emscripten build modes (MODULARIZE +
     *  EXPORT_ES6 with certain optimiser passes) bare `Module` inside a
     *  library function body resolves to undefined at call time even
     *  though bracket notation inside __postset sees it. */
    module: null,
    caches: {},
    debug: {
      traceHit: false,
      traceHitNext: false,
      traceMotion: false,
      traceButton: false,
      tracePaint: false,
      traceCBtn: false,
      traceCMot: false,
      traceMove: false,
      traceQp: false,
    },

    init: function() {
      // Capture Module now — it IS live inside __postset evaluation.
      this.module = Module;
      // If the user pre-installed a Host via Module['emX11Host'], use it.
      if (Module['emX11Host']) {
        this.bridge = Module['emX11Host'];
        // Mirror debug flags from the installed Host if it provides them.
        // The TypeScript Host also writes its _debug onto Module['emX11Debug']
        // so DevTools can toggle flags without reaching into the closure.
        if (Module['emX11Debug']) this.debug = Module['emX11Debug'];
        return;
      }
      // Layer 1 (zero JS): if no Host was pre-installed and auto-start is
      // not disabled, the default host creator runs.  This path is exercised
      // when the user compiles with -sUSE_EM_X11 and writes zero JS glue.
      if (!Module['emX11NoAutoStart']) {
        if (typeof EmX11DefaultHost !== 'undefined') {
          this.bridge = EmX11DefaultHost.create(Module);
          // EmX11DefaultHost.create() calls attachToBridge() which sets
          // Module['emX11Debug']; sync it into the closure so $EmX11Host
          // stays consistent.
          if (Module['emX11Debug']) this.debug = Module['emX11Debug'];
        }
      }
    },

    /** Safe accessor – returns the bridge or null.  Every bridge function
     *  below calls this so a missing Host is a silent no-op rather than a
     *  TypeError on undefined. */
    get: function() { return this.bridge; },
  },

  /* ---- clipboard -------------------------------------------------------- */

  em_x11_js_clipboard_read_begin__sig: 'i',
  em_x11_js_clipboard_read_begin: function() {
    var bytes = Module['emX11ClipboardBytes'];
    if (!bytes) return -1;
    return bytes.length | 0;
  },

  em_x11_js_clipboard_read_fetch__sig: 'iiii',
  em_x11_js_clipboard_read_fetch: function(dstPtr, capacity) {
    var bytes = Module['emX11ClipboardBytes'];
    if (!bytes) return 0;
    var n = Math.min(bytes.length, capacity) | 0;
    HEAPU8.set(bytes.subarray(0, n), dstPtr);
    Module['emX11ClipboardBytes'] = null;
    return n;
  },

  em_x11_js_clipboard_write_utf8__sig: 'vii',
  em_x11_js_clipboard_write_utf8: function(dataPtr, len) {
    var b2 = HEAPU8.subarray(dataPtr, dataPtr + len);
    var copy = new Uint8Array(b2);
    var B = EmX11Host.get();
    if (B && typeof B.clipboardWriteRemote === 'function') {
      B.clipboardWriteRemote(copy);
      return;
    }
    if (typeof navigator === 'undefined' ||
        !navigator.clipboard ||
        !navigator.clipboard.writeText) {
      warnOnce('[em-x11] clipboard write: API unavailable');
      return;
    }
    var text = new TextDecoder('utf-8').decode(copy);
    navigator.clipboard.writeText(text).catch(function(e) {
      warnOnce('[em-x11] clipboard write failed: ' + e);
    });
  },

  /* ---- core lifecycle --------------------------------------------------- */

  em_x11_js_init__sig: 'vii',
  em_x11_js_init: function(screenWidth, screenHeight) {
    var h = EmX11Host.get();
    if (h) h.onInit(screenWidth, screenHeight);
  },

  em_x11_js_open_display__sig: 'viii',
  em_x11_js_open_display: function(connIdPtr, basePtr, maskPtr) {
    var h = EmX11Host.get();
    if (!h) {
      HEAP32[connIdPtr >> 2] = 0;
      HEAPU32[basePtr >> 2] = 0;
      HEAPU32[maskPtr >> 2] = 0x001FFFFF;
      return;
    }
    var info = h.openDisplay(EmX11Host.module);
    HEAP32[connIdPtr >> 2] = info.connId | 0;
    HEAPU32[basePtr >> 2] = info.xidBase >>> 0;
    HEAPU32[maskPtr >> 2] = info.xidMask >>> 0;
  },

  em_x11_js_close_display__sig: 'vi',
  em_x11_js_close_display: function(connId) {
    var h = EmX11Host.get();
    if (h) h.closeDisplay(connId);
  },

  em_x11_js_get_root_window__sig: 'i',
  em_x11_js_get_root_window: function() {
    var h = EmX11Host.get();
    if (!h) return 0;
    return h.getRootWindow() >>> 0;
  },

  em_x11_js_flush__sig: 'v',
  em_x11_js_flush: function() {
    var h = EmX11Host.get();
    if (h) h.onFlush();
  },

  /* ---- pointer ---------------------------------------------------------- */

  em_x11_js_pointer_xy__sig: 'vii',
  em_x11_js_pointer_xy: function(xPtr, yPtr) {
    var h = EmX11Host.get();
    if (!h) {
      HEAP32[xPtr >> 2] = 0;
      HEAP32[yPtr >> 2] = 0;
      return;
    }
    var pt = h.getPointerXY();
    HEAP32[xPtr >> 2] = pt.x | 0;
    HEAP32[yPtr >> 2] = pt.y | 0;
  },

  /* ---- window attributes ------------------------------------------------ */

  em_x11_js_get_window_attrs__sig: 'viii',
  em_x11_js_get_window_attrs: function(id, outPtr) {
    var out = outPtr >> 2;
    var h = EmX11Host.get();
    if (!h) { HEAP32[out + 0] = 0; return; }
    var a = h.getWindowAttrs(id >>> 0);
    if (!a) { HEAP32[out + 0] = 0; return; }
    HEAP32[out + 0] = 1;
    HEAP32[out + 1] = a.x | 0;
    HEAP32[out + 2] = a.y | 0;
    HEAP32[out + 3] = a.width | 0;
    HEAP32[out + 4] = a.height | 0;
    HEAP32[out + 5] = a.mapped ? 1 : 0;
    HEAP32[out + 6] = a.overrideRedirect ? 1 : 0;
    HEAP32[out + 7] = a.borderWidth | 0;
  },

  em_x11_js_get_window_abs_origin__sig: 'viii',
  em_x11_js_get_window_abs_origin: function(id, outPtr) {
    var out = outPtr >> 2;
    var h = EmX11Host.get();
    if (!h) { HEAP32[out + 0] = 0; return; }
    var o = h.getWindowAbsOrigin(id >>> 0);
    if (!o) { HEAP32[out + 0] = 0; return; }
    HEAP32[out + 0] = 1;
    HEAP32[out + 1] = o.ax | 0;
    HEAP32[out + 2] = o.ay | 0;
  },

  /* ---- shape ------------------------------------------------------------ */

  em_x11_js_get_window_shape_count__sig: 'ii',
  em_x11_js_get_window_shape_count: function(id) {
    var h = EmX11Host.get();
    if (!h) return -1;
    var rects = h.getWindowShape(id >>> 0);
    if (rects === null || rects === undefined) return -1;
    EmX11Host.caches.shapeStash = { id: id >>> 0, rects: rects };
    return rects.length | 0;
  },

  em_x11_js_get_window_shape_rects__sig: 'iiii',
  em_x11_js_get_window_shape_rects: function(id, dstPtr, capacity) {
    var stashed = EmX11Host.caches.shapeStash;
    var rects = (stashed && stashed.id === (id >>> 0)) ? stashed.rects : null;
    EmX11Host.caches.shapeStash = null;
    if (!rects) {
      var h = EmX11Host.get();
      if (!h) return 0;
      rects = h.getWindowShape(id >>> 0);
      if (!rects) return 0;
    }
    var n = Math.min(rects.length | 0, capacity | 0);
    var base = dstPtr >> 2;
    for (var i = 0; i < n; i++) {
      var r = rects[i];
      HEAP32[base + i * 4 + 0] = r.x | 0;
      HEAP32[base + i * 4 + 1] = r.y | 0;
      HEAP32[base + i * 4 + 2] = r.w | 0;
      HEAP32[base + i * 4 + 3] = r.h | 0;
    }
    return n;
  },

  /* ---- XQueryTree cross-conn -------------------------------------------- */

  em_x11_js_get_window_children_count__sig: 'ii',
  em_x11_js_get_window_children_count: function(parent) {
    var h = EmX11Host.get();
    if (!h) return 0;
    var kids = h.getWindowChildren(parent >>> 0);
    if (!kids) return 0;
    EmX11Host.caches.childrenStash = { parent: parent >>> 0, kids: kids };
    return kids.length | 0;
  },

  em_x11_js_get_window_children__sig: 'iiii',
  em_x11_js_get_window_children: function(parent, dstPtr, capacity) {
    var stashed = EmX11Host.caches.childrenStash;
    var kids = (stashed && stashed.parent === (parent >>> 0)) ? stashed.kids : null;
    EmX11Host.caches.childrenStash = null;
    if (!kids) {
      var h = EmX11Host.get();
      if (!h) return 0;
      kids = h.getWindowChildren(parent >>> 0);
      if (!kids) return 0;
    }
    var n = Math.min(kids.length | 0, capacity | 0);
    var base = dstPtr >> 2;
    for (var i = 0; i < n; i++) HEAPU32[base + i] = kids[i] >>> 0;
    return n;
  },

  /* ---- passive grabs ---------------------------------------------------- */

  em_x11_js_grab_button__sig: 'viiiiiiiii',
  em_x11_js_grab_button: function(window, button, modifiers,
                                  owner_events, event_mask,
                                  pointer_mode, keyboard_mode,
                                  confine_to, cursor) {
    var h = EmX11Host.get();
    if (!h) return;
    h.onGrabButton(
      window >>> 0, button >>> 0, modifiers >>> 0,
      owner_events !== 0, event_mask >>> 0,
      pointer_mode | 0, keyboard_mode | 0,
      confine_to >>> 0, cursor >>> 0);
  },

  em_x11_js_ungrab_button__sig: 'viii',
  em_x11_js_ungrab_button: function(window, button, modifiers) {
    var h = EmX11Host.get();
    if (h) h.onUngrabButton(window >>> 0, button >>> 0, modifiers >>> 0);
  },

  /* ---- active pointer grabs --------------------------------------------- */

  em_x11_js_grab_pointer__sig: 'viii',
  em_x11_js_grab_pointer: function(conn_id, window, owner_events) {
    var h = EmX11Host.get();
    if (h) h.onGrabPointer(conn_id >>> 0, window >>> 0, owner_events !== 0);
  },

  em_x11_js_ungrab_pointer__sig: 'v',
  em_x11_js_ungrab_pointer: function() {
    var h = EmX11Host.get();
    if (h) h.onUngrabPointer();
  },

  /* ---- deferred pointer-window repoll ----------------------------------- */

  em_x11_js_schedule_repoll__sig: 'vi',
  em_x11_js_schedule_repoll: function(conn_id) {
    var h = EmX11Host.get();
    if (h) h.onScheduleRepoll(conn_id >>> 0);
  },

  /* ---- input focus ------------------------------------------------------ */

  em_x11_js_set_input_focus__sig: 'vi',
  em_x11_js_set_input_focus: function(window) {
    var h = EmX11Host.get();
    if (h && h.onSetInputFocus) h.onSetInputFocus(window >>> 0);
  },

  /* ---- XIM -------------------------------------------------------------- */

  em_x11_js_xim_set_focus__sig: 'vi',
  em_x11_js_xim_set_focus: function(window) {
    var h = EmX11Host.get();
    if (h && h.onXimSetFocus) h.onXimSetFocus(window >>> 0);
  },

  em_x11_js_xim_clear_focus__sig: 'v',
  em_x11_js_xim_clear_focus: function() {
    var h = EmX11Host.get();
    if (h && h.onXimClearFocus) h.onXimClearFocus();
  },

  em_x11_js_xim_set_spot__sig: 'viii',
  em_x11_js_xim_set_spot: function(window, x, y) {
    var h = EmX11Host.get();
    if (h && h.onXimSetSpot) h.onXimSetSpot(window >>> 0, x | 0, y | 0);
  },

  /* ---- exec self (twm F_RESTART) ---------------------------------------- */

  em_x11_js_exec_self__sig: 'viii',
  em_x11_js_exec_self: function(conn_id, argv_ptrs, argc) {
    var args = [];
    if (argv_ptrs !== 0 && argc > 0) {
      var base = argv_ptrs >> 2;
      for (var i = 0; i < argc; i++) {
        var p = HEAPU32[base + i] >>> 0;
        args.push(p === 0 ? '' : UTF8ToString(p));
      }
    }
    var h = EmX11Host.get();
    if (h && h.onExecSelf) h.onExecSelf(conn_id | 0, args);
  },

  /* ---- atom ------------------------------------------------------------- */

  em_x11_js_intern_atom__sig: 'iii',
  em_x11_js_intern_atom: function(namePtr, onlyIfExists) {
    if (namePtr === 0) return 0;
    var name = UTF8ToString(namePtr);
    var h = EmX11Host.get();
    if (!h) return 0;
    return h.internAtom(name, onlyIfExists !== 0) >>> 0;
  },

  em_x11_js_get_atom_name__sig: 'ii',
  em_x11_js_get_atom_name: function(atom) {
    var h = EmX11Host.get();
    if (!h) return 0;
    var name = h.getAtomName(atom >>> 0);
    if (name === null) return 0;
    return stringToNewUTF8(name);
  },

  /* ---- drawing ---------------------------------------------------------- */

  em_x11_js_clear_area__sig: 'viiiii',
  em_x11_js_clear_area: function(id, x, y, w, h) {
    var h = EmX11Host.get();
    if (h) h.onClearArea(id, x, y, w, h);
  },

  em_x11_js_fill_rect__sig: 'viiiiii',
  em_x11_js_fill_rect: function(id, x, y, w, h, color) {
    var h = EmX11Host.get();
    if (h) h.onFillRect(id, x, y, w, h, color);
  },

  em_x11_js_fill_stippled_rect__sig: 'viiiiiiiiiii',
  em_x11_js_fill_stippled_rect: function(dstId, x, y, w, h, fg, bg, stippleId, tsX, tsY, opaque) {
    var h = EmX11Host.get();
    if (h) h.onFillStippledRect(dstId, x, y, w, h, fg, bg, stippleId, tsX, tsY, opaque);
  },

  em_x11_js_draw_line__sig: 'viiiiiii',
  em_x11_js_draw_line: function(id, x1, y1, x2, y2, color, lineWidth) {
    var h = EmX11Host.get();
    if (h) h.onDrawLine(id, x1, y1, x2, y2, color, lineWidth);
  },

  em_x11_js_draw_arc__sig: 'viiiiiiiii',
  em_x11_js_draw_arc: function(id, x, y, w, h, angle1, angle2, color, lineWidth) {
    var h = EmX11Host.get();
    if (h) h.onDrawArc(id, x, y, w, h, angle1, angle2, color, lineWidth);
  },

  em_x11_js_fill_arc__sig: 'viiiiiiii',
  em_x11_js_fill_arc: function(id, x, y, w, h, angle1, angle2, color) {
    var h = EmX11Host.get();
    if (h) h.onFillArc(id, x, y, w, h, angle1, angle2, color);
  },

  em_x11_js_fill_polygon__sig: 'viiiiiii',
  em_x11_js_fill_polygon: function(id, ptsPtr, count, shape, mode, color) {
    var pts = [];
    if (count > 0 && ptsPtr !== 0) {
      var base = ptsPtr >> 2;
      for (var i = 0; i < count; i++) {
        pts.push({ x: HEAP32[base + i * 2], y: HEAP32[base + i * 2 + 1] });
      }
    }
    var h = EmX11Host.get();
    if (h) h.onFillPolygon(id, pts, shape, mode, color);
  },

  em_x11_js_draw_points__sig: 'viiiiii',
  em_x11_js_draw_points: function(id, ptsPtr, count, mode, color) {
    var pts = [];
    if (count > 0 && ptsPtr !== 0) {
      var base = ptsPtr >> 2;
      for (var i = 0; i < count; i++) {
        pts.push({ x: HEAP32[base + i * 2], y: HEAP32[base + i * 2 + 1] });
      }
    }
    var h = EmX11Host.get();
    if (h) h.onDrawPoints(id, pts, mode, color);
  },

  /* ---- font / text ------------------------------------------------------ */

  em_x11_js_draw_string__sig: 'viiiiiiiiii',
  em_x11_js_draw_string: function(id, x, y, fontPtr, textPtr, length, fg, bg, imageMode) {
    var font = fontPtr !== 0 ? UTF8ToString(fontPtr) : '13px monospace';
    var text = length > 0 && textPtr !== 0 ? UTF8ToString(textPtr, length) : '';
    var h = EmX11Host.get();
    if (h) h.onDrawString(id, x, y, font, text, fg, bg, imageMode);
  },

  em_x11_js_draw_string_latin1__sig: 'viiiiiiiiii',
  em_x11_js_draw_string_latin1: function(id, x, y, fontPtr, textPtr, length, fg, bg, imageMode) {
    var font = fontPtr !== 0 ? UTF8ToString(fontPtr) : '13px monospace';
    var text = '';
    if (length > 0 && textPtr !== 0) {
      var u8 = HEAPU8;
      for (var i = 0; i < length; i++) text += String.fromCharCode(u8[textPtr + i]);
    }
    var h = EmX11Host.get();
    if (h) h.onDrawString(id, x, y, font, text, fg, bg, imageMode);
  },

  /* ---- font measurement ------------------------------------------------- */

  em_x11_js_measure_font__sig: 'viiiii',
  em_x11_js_measure_font: function(fontPtr, ascentPtr, descentPtr, maxWidthPtr, widthsPtr) {
    var C = EmX11Host.caches;
    if (C.measureCtx === undefined) {
      var c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1)
            : typeof document !== 'undefined' ? document.createElement('canvas') : null;
      C.measureCtx = c ? c.getContext('2d', { willReadFrequently: true }) : null;
    }
    if (!C.fontCache) C.fontCache = new Map();
    var ctx = C.measureCtx;
    var fallbackWidth = 8, fallbackAscent = 10, fallbackDescent = 3;

    if (!ctx) {
      HEAP32[ascentPtr >> 2] = fallbackAscent;
      HEAP32[descentPtr >> 2] = fallbackDescent;
      HEAP32[maxWidthPtr >> 2] = fallbackWidth;
      for (var i = 0; i < 95; i++) HEAP32[(widthsPtr >> 2) + i] = fallbackWidth;
      return;
    }

    var css = UTF8ToString(fontPtr);
    var entry = C.fontCache.get(css);
    if (entry) {
      HEAP32[ascentPtr >> 2] = entry.ascent;
      HEAP32[descentPtr >> 2] = entry.descent;
      HEAP32[maxWidthPtr >> 2] = entry.maxW;
      var bbase = widthsPtr >> 2;
      for (var k = 0; k < 95; k++) HEAP32[bbase + k] = entry.widths[k];
      return;
    }
    ctx.font = css;
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

    var widths = new Int32Array(95);
    var maxW = 0;
    var base = widthsPtr >> 2;
    for (var j = 0; j < 95; j++) {
      var ch = String.fromCharCode(32 + j);
      var w = Math.ceil(ctx.measureText(ch).width) || fallbackWidth;
      if (w > maxW) maxW = w;
      widths[j] = w;
      HEAP32[base + j] = w;
    }
    HEAP32[maxWidthPtr >> 2] = maxW;
    C.fontCache.set(css, { ascent: ascent, descent: descent, maxW: maxW, widths: widths });
  },

  em_x11_js_measure_string__sig: 'iiii',
  em_x11_js_measure_string: function(fontPtr, textPtr, length) {
    if (length <= 0 || textPtr === 0) return 0;
    var C = EmX11Host.caches;
    if (C.measureCtx === undefined) {
      var c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1)
            : typeof document !== 'undefined' ? document.createElement('canvas') : null;
      C.measureCtx = c ? c.getContext('2d', { willReadFrequently: true }) : null;
    }
    if (!C.textCache) C.textCache = new Map();
    var ctx = C.measureCtx;
    if (!ctx) return length * 8;
    var css = fontPtr !== 0 ? UTF8ToString(fontPtr) : '13px monospace';
    var text = UTF8ToString(textPtr, length);
    var key = css + '' + text;
    var cache = C.textCache;
    var hit = cache.get(key);
    if (hit !== undefined) return hit;
    ctx.font = css;
    var w = Math.ceil(ctx.measureText(text).width);
    if (cache.size >= 8192) cache.clear();
    cache.set(key, w);
    return w;
  },

  em_x11_js_measure_string_latin1__sig: 'iiii',
  em_x11_js_measure_string_latin1: function(fontPtr, textPtr, length) {
    if (length <= 0 || textPtr === 0) return 0;
    var C = EmX11Host.caches;
    if (C.measureCtx === undefined) {
      var c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1)
            : typeof document !== 'undefined' ? document.createElement('canvas') : null;
      C.measureCtx = c ? c.getContext('2d', { willReadFrequently: true }) : null;
    }
    if (!C.textCache) C.textCache = new Map();
    var ctx = C.measureCtx;
    if (!ctx) return length * 8;
    var css = fontPtr !== 0 ? UTF8ToString(fontPtr) : '13px monospace';
    var text = '';
    if (textPtr !== 0) {
      var u8 = HEAPU8;
      for (var i = 0; i < length; i++) text += String.fromCharCode(u8[textPtr + i]);
    }
    var key = css + '' + text;
    var cache = C.textCache;
    var hit = cache.get(key);
    if (hit !== undefined) return hit;
    ctx.font = css;
    var w = Math.ceil(ctx.measureText(text).width);
    if (cache.size >= 8192) cache.clear();
    cache.set(key, w);
    return w;
  },

  /* ---- color parsing ---------------------------------------------------- */

  em_x11_js_parse_color__sig: 'iiiii',
  em_x11_js_parse_color: function(namePtr, rPtr, gPtr, bPtr) {
    if (namePtr === 0) return 0;
    var name = UTF8ToString(namePtr);
    if (typeof CSS !== 'undefined' && CSS.supports &&
        !CSS.supports('color', name)) {
      return 0;
    }
    var C = EmX11Host.caches;
    if (C.measureCtx === undefined) {
      var c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1)
            : typeof document !== 'undefined' ? document.createElement('canvas') : null;
      C.measureCtx = c ? c.getContext('2d', { willReadFrequently: true }) : null;
    }
    var ctx = C.measureCtx;
    if (!ctx) return 0;
    ctx.fillStyle = '#010203';
    var sentinel = ctx.fillStyle;
    ctx.fillStyle = name;
    if (ctx.fillStyle === sentinel) return 0;
    ctx.clearRect(0, 0, 1, 1);
    ctx.fillRect(0, 0, 1, 1);
    var p = ctx.getImageData(0, 0, 1, 1).data;
    HEAPU16[rPtr >> 1] = (p[0] * 0x101) & 0xFFFF;
    HEAPU16[gPtr >> 1] = (p[1] * 0x101) & 0xFFFF;
    HEAPU16[bPtr >> 1] = (p[2] * 0x101) & 0xFFFF;
    return 1;
  },

  /* ---- pixmap ----------------------------------------------------------- */

  em_x11_js_pixmap_create__sig: 'viiii',
  em_x11_js_pixmap_create: function(id, width, height, depth) {
    var h = EmX11Host.get();
    if (h) h.onPixmapCreate(id, width, height, depth);
  },

  em_x11_js_pixmap_destroy__sig: 'vi',
  em_x11_js_pixmap_destroy: function(id) {
    var h = EmX11Host.get();
    if (h) h.onPixmapDestroy(id);
  },

  em_x11_js_shape_combine_mask__sig: 'viiiii',
  em_x11_js_shape_combine_mask: function(destId, srcId, xOff, yOff, op) {
    var h = EmX11Host.get();
    if (h) h.onShapeCombineMask(destId, srcId, xOff, yOff, op);
  },

  em_x11_js_shape_select_input__sig: 'viii',
  em_x11_js_shape_select_input: function(connId, window, mask) {
    var h = EmX11Host.get();
    if (h) h.onShapeSelectInput(connId, window, mask);
  },

  em_x11_js_copy_area__sig: 'viiiiiiii',
  em_x11_js_copy_area: function(srcId, dstId, srcX, srcY, w, h, dstX, dstY) {
    var h = EmX11Host.get();
    if (h) h.onCopyArea(srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY);
  },

  em_x11_js_copy_plane__sig: 'viiiiiiiiiiii',
  em_x11_js_copy_plane: function(srcId, dstId, srcX, srcY, w, h, dstX, dstY, plane, fg, bg, applyBg) {
    var h = EmX11Host.get();
    if (h) h.onCopyPlane(srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY, plane >>> 0, fg >>> 0, bg >>> 0, applyBg !== 0);
  },

  em_x11_js_put_image__sig: 'viiiiiiiiiiii',
  em_x11_js_put_image: function(dstId, dstX, dstY, w, h, format, depth, bytesPerLine, dataPtr, dataLen, fg, bg) {
    var data = dataLen > 0 && dataPtr !== 0
      ? HEAPU8.slice(dataPtr, dataPtr + dataLen)
      : new Uint8Array(0);
    var h = EmX11Host.get();
    if (h) h.onPutImage(dstId >>> 0, dstX, dstY, w, h, format, depth, bytesPerLine, data, fg >>> 0, bg >>> 0);
  },

  /* ---- property --------------------------------------------------------- */

  em_x11_js_change_property__sig: 'iiiiiiii',
  em_x11_js_change_property: function(w, atom, type, format, mode, dataPtr, nelements) {
    var unit = format === 8 ? 1 : format === 16 ? 2 : format === 32 ? 4 : 0;
    if (unit === 0) return 0;
    var bytes = unit * (nelements | 0);
    var data = bytes > 0
      ? HEAPU8.slice(dataPtr, dataPtr + bytes)
      : new Uint8Array(0);
    var h = EmX11Host.get();
    if (!h) return -1;
    var ok = h.changeProperty(w >>> 0, atom >>> 0, type >>> 0, format | 0, mode | 0, data);
    return ok ? 1 : 0;
  },

  em_x11_js_get_property_meta__sig: 'viiiiii',
  em_x11_js_get_property_meta: function(w, atom, reqType, longOffset, longLength, metaPtr) {
    var base = metaPtr >> 2;
    for (var i = 0; i < 8; i++) HEAP32[base + i] = 0;
    var h = EmX11Host.get();
    if (!h) return;
    var r2 = h.peekProperty(w >>> 0, atom >>> 0, reqType >>> 0,
                            longOffset | 0, longLength | 0, false);
    if (r2 === null) return;
    HEAP32[base + 6] = 1;
    HEAP32[base + 0] = r2.found ? 1 : 0;
    HEAP32[base + 1] = r2.type | 0;
    HEAP32[base + 2] = r2.format | 0;
    HEAP32[base + 3] = r2.nitems | 0;
    HEAP32[base + 4] = r2.bytesAfter | 0;
    HEAP32[base + 5] = r2.data.length | 0;
    EmX11Host.caches.propStash = {
      w: w >>> 0, atom: atom >>> 0, reqType: reqType >>> 0,
      longOffset: longOffset | 0, longLength: longLength | 0,
      data: r2.data,
    };
  },

  em_x11_js_get_property_data__sig: 'viiiiiiii',
  em_x11_js_get_property_data: function(w, atom, reqType, longOffset, longLength, deleteFlag, dstPtr, capacity) {
    var stash = EmX11Host.caches.propStash;
    EmX11Host.caches.propStash = null;
    var data = (stash &&
                stash.w === (w >>> 0) &&
                stash.atom === (atom >>> 0) &&
                stash.reqType === (reqType >>> 0) &&
                stash.longOffset === (longOffset | 0) &&
                stash.longLength === (longLength | 0))
      ? stash.data : null;
    if (data && data.length > 0) {
      var n = Math.min(data.length, capacity | 0);
      HEAPU8.set(data.subarray ? data.subarray(0, n) : data.slice(0, n), dstPtr);
      if (deleteFlag !== 0) {
        var h2 = EmX11Host.get();
        if (h2) h2.deleteProperty(w >>> 0, atom >>> 0);
      }
      return;
    }
    var h3 = EmX11Host.get();
    if (!h3) return;
    var r3 = h3.peekProperty(w >>> 0, atom >>> 0, reqType >>> 0,
                              longOffset | 0, longLength | 0, deleteFlag !== 0);
    if (!r3 || !r3.found || r3.data.length === 0) return;
    var n3 = Math.min(r3.data.length, capacity | 0);
    HEAPU8.set(r3.data.subarray(0, n3), dstPtr);
  },

  em_x11_js_delete_property__sig: 'vii',
  em_x11_js_delete_property: function(w, atom) {
    var h = EmX11Host.get();
    if (h) h.deleteProperty(w >>> 0, atom >>> 0);
  },

  em_x11_js_list_properties_count__sig: 'ii',
  em_x11_js_list_properties_count: function(w) {
    var h = EmX11Host.get();
    if (!h) return 0;
    return h.listProperties(w >>> 0).length;
  },

  em_x11_js_list_properties_fetch__sig: 'iiii',
  em_x11_js_list_properties_fetch: function(w, dstPtr, capacity) {
    var h = EmX11Host.get();
    if (!h) return 0;
    var atoms2 = h.listProperties(w >>> 0);
    var n2 = Math.min(atoms2.length, capacity | 0);
    var base2 = dstPtr >> 2;
    for (var j = 0; j < n2; j++) HEAPU32[base2 + j] = atoms2[j] >>> 0;
    return n2;
  },

  /* ---- window management ------------------------------------------------ */

  em_x11_js_window_create__sig: 'viiiiiiiiiii',
  em_x11_js_window_create: function(connId, id, parent, x, y, w, h, borderWidth, borderPixel, bgType, bgValue) {
    var h = EmX11Host.get();
    if (h) h.onWindowCreate(connId, id, parent, x, y, w, h, borderWidth, borderPixel, bgType, bgValue);
  },

  em_x11_js_window_set_border__sig: 'viii',
  em_x11_js_window_set_border: function(id, borderWidth, borderPixel) {
    var h = EmX11Host.get();
    if (h) h.onWindowSetBorder(id, borderWidth, borderPixel);
  },

  em_x11_js_window_set_bg__sig: 'viii',
  em_x11_js_window_set_bg: function(id, bgType, bgValue) {
    var h = EmX11Host.get();
    if (h) h.onWindowSetBg(id, bgType, bgValue);
  },

  em_x11_js_window_configure__sig: 'viiiiii',
  em_x11_js_window_configure: function(connId, id, x, y, w, h) {
    var h = EmX11Host.get();
    if (h) h.onWindowConfigure(connId, id, x, y, w, h);
  },

  em_x11_js_window_set_bit_gravity__sig: 'vii',
  em_x11_js_window_set_bit_gravity: function(id, gravity) {
    var h = EmX11Host.get();
    if (h) h.onWindowSetBitGravity(id, gravity);
  },

  em_x11_js_window_map__sig: 'vii',
  em_x11_js_window_map: function(connId, id) {
    var h = EmX11Host.get();
    if (h) h.onWindowMap(connId, id);
  },

  em_x11_js_window_unmap__sig: 'vii',
  em_x11_js_window_unmap: function(connId, id) {
    var h = EmX11Host.get();
    if (h) h.onWindowUnmap(connId, id);
  },

  em_x11_js_window_destroy__sig: 'vi',
  em_x11_js_window_destroy: function(id) {
    var h = EmX11Host.get();
    if (h) h.onWindowDestroy(id);
  },

  em_x11_js_window_raise__sig: 'vi',
  em_x11_js_window_raise: function(id) {
    var h = EmX11Host.get();
    if (h) h.onWindowRaise(id);
  },

  em_x11_js_window_lower__sig: 'vi',
  em_x11_js_window_lower: function(id) {
    var h = EmX11Host.get();
    if (h) h.onWindowLower(id);
  },

  em_x11_js_select_input__sig: 'viii',
  em_x11_js_select_input: function(connId, id, mask) {
    var h = EmX11Host.get();
    if (h) h.onSelectInput(connId, id, mask >>> 0);
  },

  em_x11_js_set_override_redirect__sig: 'vii',
  em_x11_js_set_override_redirect: function(id, flag) {
    var h = EmX11Host.get();
    if (h) h.onSetOverrideRedirect(id, flag !== 0);
  },

  em_x11_js_reparent_window__sig: 'viiii',
  em_x11_js_reparent_window: function(id, parent, x, y) {
    var h = EmX11Host.get();
    if (h) h.onReparentWindow(id, parent, x, y);
  },

  em_x11_js_window_set_bg_pixmap__sig: 'vii',
  em_x11_js_window_set_bg_pixmap: function(id, pmId) {
    var h = EmX11Host.get();
    if (h) h.onWindowSetBgPixmap(id, pmId);
  },

  em_x11_js_window_set_cursor__sig: 'vii',
  em_x11_js_window_set_cursor: function(id, cursor) {
    var h = EmX11Host.get();
    if (h) h.onWindowSetCursor(id, cursor);
  },

  em_x11_js_set_grab_cursor__sig: 'vi',
  em_x11_js_set_grab_cursor: function(cursor) {
    var h = EmX11Host.get();
    if (h) h.onSetGrabCursor(cursor);
  },

  em_x11_js_window_shape__sig: 'viii',
  em_x11_js_window_shape: function(id, rectsPtr, count) {
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
    var h = EmX11Host.get();
    if (h) h.onWindowShape(id, rects);
  },

  /* ---- GLX -------------------------------------------------------------- */

  em_x11_js_glx_create_context__sig: 'iiiii',
  em_x11_js_glx_create_context: function(width, height, outTargetIdPtr, outTargetIdLen) {
    var h = EmX11Host.get();
    if (!h || !h.glx) return 0;
    var info = h.glx.createContext(width | 0, height | 0);
    if (!info) return 0;
    EmX11Host.module.specialHTMLTargets[info.targetId] = info.canvas;
    stringToUTF8(info.targetId, outTargetIdPtr, outTargetIdLen);
    return info.id | 0;
  },

  em_x11_js_glx_legacy_init_once__sig: 'v',
  em_x11_js_glx_legacy_init_once: function() {
    if (typeof GLImmediate === 'undefined' || GLImmediate.initted) return;
    if (typeof Browser === 'undefined') return;
    Browser.useWebGL = true;
    if (Browser.moduleContextCreatedCallbacks) {
      Browser.moduleContextCreatedCallbacks.forEach(function(cb) { cb(); });
    }
  },

  em_x11_js_glx_destroy_context__sig: 'vi',
  em_x11_js_glx_destroy_context: function(id) {
    var h = EmX11Host.get();
    if (!h || !h.glx) return;
    var targetId = h.glx.targetIdOf(id | 0);
    if (targetId && EmX11Host.module.specialHTMLTargets) {
      delete EmX11Host.module.specialHTMLTargets[targetId];
    }
    h.glx.destroyContext(id | 0);
  },

  em_x11_js_glx_swap_buffers__sig: 'vii',
  em_x11_js_glx_swap_buffers: function(id, drawable) {
    var h = EmX11Host.get();
    if (!h || !h.glx) return;
    h.glx.swapBuffers(id | 0, drawable >>> 0);
  },

  em_x11_js_glx_resize__sig: 'viii',
  em_x11_js_glx_resize: function(id, width, height) {
    var h = EmX11Host.get();
    if (!h || !h.glx) return;
    h.glx.resize(id | 0, width | 0, height | 0);
  },
};

autoAddDeps(LibraryEmX11, '$EmX11Host');
addToLibrary(LibraryEmX11);
