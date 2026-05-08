/*
 * em-x11 host bridges, embedded as EM_JS so libemx11 is self-contained
 * (embedded in a wasm custom section, readable by dlopen).
 *
 * Each bridge talks to a single EmX11 instance installed at
 * `globalThis.emX11` -- the wasm client and the host run in the
 * same JS context (main thread or, for pyodide-tk, the same Worker).
 *
 * Layout under `globalThis.emX11`:
 *   _bridge   -- the Host facade (onWindowCreate, onFillRect, ...)
 *                that every EM_JS body below dispatches into.
 *   _caches   -- shared scratchpads owned by the bridges themselves
 *                (measure ctx, font cache, text cache, property stash).
 *                Lazy-initialised on first use.
 *   _debug    -- runtime trace flags toggled from JS / DevTools
 *                (e.g. _debug.traceHit). The bridges don't touch them;
 *                hit-test.ts on the JS side does.
 *
 * Everything the C side ever reads sits under `globalThis.emX11`
 * with a leading underscore -- nothing scattered across separate
 * top-level globals. Public surface (em.fs, em.spawn, em.display,
 * em.debug) lives unprefixed on the same object.
 *
 * The earlier dual-mode design (multi-thread "channel" mode via
 * MessagePort RPC + SharedArrayBuffer hot reads, in addition to this
 * direct path) was removed at pre-alpha: it was never wired into a
 * demo, gave no measured perf benefit, and doubled the maintenance
 * cost of every bridge. If multi-client / OffscreenCanvas hosting
 * comes back later, it can be reintroduced from a smaller base; the
 * old design lives in git history.
 *
 * Every bridge is EM_JS (sync). The earlier set of EM_ASYNC_JS
 * declarations on the atom / property paths was a holdover from the
 * channel mode, where atom interning crossed a worker boundary and
 * had to unwind via Asyncify. In direct mode every JS body returns
 * sync, so EM_ASYNC_JS is pure overhead -- worse, it forces an
 * unwind/rewind on EVERY call, which makes user-side `wacl_eval`
 * always return a Promise (Asyncify suspends mid-eval), breaking
 * the sync runTcl entry point.
 */

#include <emscripten.h>

/* Link anchor: this TU only contains EM_JS data symbols, which the
 * archive linker drops unless a real ref pulls the .o in. display.c
 * calls this function so emcc's post-link pass sees the JS bodies. */
void emx11_bridges_link_anchor(void) {}

/* --- core ---------------------------------------------------------------- */

EM_JS(void, emx11_js_init, (int screenWidth, int screenHeight), {
    var e = globalThis.emX11; var h = e && e._bridge;
    if (h) h.onInit(screenWidth, screenHeight);
});

EM_JS(void, emx11_js_open_display, (int connIdPtr, int basePtr, int maskPtr), {
    var e = globalThis.emX11; var h = e && e._bridge;
    if (!h) {
        HEAP32[connIdPtr >> 2] = 0;
        HEAPU32[basePtr >> 2] = 0;
        HEAPU32[maskPtr >> 2] = 0x001FFFFF;
        return;
    }
    var info = h.openDisplay();
    HEAP32[connIdPtr >> 2] = info.connId | 0;
    HEAPU32[basePtr >> 2] = info.xidBase >>> 0;
    HEAPU32[maskPtr >> 2] = info.xidMask >>> 0;
});

EM_JS(void, emx11_js_close_display, (int connId), {
    var e = globalThis.emX11; var h = e && e._bridge;
    if (h) h.closeDisplay(connId);
});

EM_JS(unsigned int, emx11_js_get_root_window, (void), {
    var e = globalThis.emX11; var h = e && e._bridge;
    if (!h) return 0;
    return h.getRootWindow() >>> 0;
});

EM_JS(void, emx11_js_flush, (void), {
    var e = globalThis.emX11; var h = e && e._bridge;
    if (h) h.onFlush();
});

/* Hot read: XQueryPointer fires at 50ms cadence per xeyes + on every
 * pointer-related Xt dispatch. */
EM_JS(void, emx11_js_pointer_xy, (int xPtr, int yPtr), {
    var e = globalThis.emX11; var h = e && e._bridge;
    if (!h) {
        HEAP32[xPtr >> 2] = 0;
        HEAP32[yPtr >> 2] = 0;
        return;
    }
    var pt = h.getPointerXY();
    HEAP32[xPtr >> 2] = pt.x | 0;
    HEAP32[yPtr >> 2] = pt.y | 0;
});

/* Hot read: called on every motion/button hit-test + each client's
 * own XGetWindowAttributes. Output layout matches emx11_meta_layout.h
 * EMX11_WIN_ATTRS_* (0 PRESENT, 1 X, 2 Y, 3 W, 4 H, 5 MAPPED,
 * 6 OVERRIDE, 7 BORDER_WIDTH). */
EM_JS(void, emx11_js_get_window_attrs, (unsigned int id, int outPtr), {
    var out = outPtr >> 2;
    var e = globalThis.emX11; var h = e && e._bridge;
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
});

/* Hot read: output layout matches EMX11_ABS_ORIGIN_* (0 PRESENT,
 * 1 AX, 2 AY). */
EM_JS(void, emx11_js_get_window_abs_origin, (unsigned int id, int outPtr), {
    var out = outPtr >> 2;
    var e = globalThis.emX11; var h = e && e._bridge;
    if (!h) { HEAP32[out + 0] = 0; return; }
    var o = h.getWindowAbsOrigin(id >>> 0);
    if (!o) { HEAP32[out + 0] = 0; return; }
    HEAP32[out + 0] = 1;
    HEAP32[out + 1] = o.ax | 0;
    HEAP32[out + 2] = o.ay | 0;
});

/* Cross-conn shape lookup, two-call pattern (count, then fetch). Return
 * value on the count call:
 *    -1 -- window unknown to the host
 *     0 -- window known, but no bounding shape (rectangular)
 *    >0 -- count of XRectangles available
 * Used by twm's XShapeQueryExtents / XShapeCombineShape on a foreign
 * client (xeyes) -- without it, twm's frame stays rectangular and the
 * shape's hole doesn't pass clicks through. */
EM_JS(int, emx11_js_get_window_shape_count, (unsigned int id), {
    var e = globalThis.emX11; var h = e && e._bridge;
    if (!h) return -1;
    var rects = h.getWindowShape(id >>> 0);
    if (rects === null || rects === undefined) return -1;
    /* Stash for the immediate-following fetch so we don't re-query. */
    var caches = e._caches || (e._caches = {});
    caches.shapeStash = { id: id >>> 0, rects: rects };
    return rects.length | 0;
});

EM_JS(int, emx11_js_get_window_shape_rects, (unsigned int id, int dstPtr, int capacity), {
    var e = globalThis.emX11;
    var caches = e && e._caches;
    var stashed = caches && caches.shapeStash;
    var rects = (stashed && stashed.id === (id >>> 0)) ? stashed.rects : null;
    if (caches) caches.shapeStash = null;
    if (!rects) {
        var h = e && e._bridge;
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
});

/* --- passive grabs (XGrabButton / XUngrabButton) ------------------------- */

EM_JS(void, emx11_js_grab_button,
      (unsigned int window, unsigned int button, unsigned int modifiers,
       int owner_events, unsigned int event_mask,
       int pointer_mode, int keyboard_mode,
       unsigned int confine_to, unsigned int cursor),
      {
          var e = globalThis.emX11; var h = e && e._bridge;
          if (!h) return;
          h.onGrabButton(
              window >>> 0, button >>> 0, modifiers >>> 0,
              owner_events !== 0, event_mask >>> 0,
              pointer_mode | 0, keyboard_mode | 0,
              confine_to >>> 0, cursor >>> 0);
      });

EM_JS(void, emx11_js_ungrab_button,
      (unsigned int window, unsigned int button, unsigned int modifiers),
      {
          var e = globalThis.emX11; var h = e && e._bridge;
          if (h) h.onUngrabButton(window >>> 0, button >>> 0, modifiers >>> 0);
      });

/* --- atom ---------------------------------------------------------------- */

EM_JS(unsigned int, emx11_js_intern_atom, (int namePtr, int onlyIfExists), {
    if (namePtr === 0) return 0;
    var name = UTF8ToString(namePtr);
    var e = globalThis.emX11; var h = e && e._bridge;
    if (!h) return 0;
    return h.internAtom(name, onlyIfExists !== 0) >>> 0;
});

EM_JS(int, emx11_js_get_atom_name, (unsigned int atom), {
    var e = globalThis.emX11; var h = e && e._bridge;
    if (!h) return 0;
    var name = h.getAtomName(atom >>> 0);
    if (name === null) return 0;
    return stringToNewUTF8(name);
});

/* --- clipboard ----------------------------------------------------------- */

EM_JS(int, emx11_js_clipboard_read_begin, (void), {
    /* Async read still requires JSPI or Asyncify unwind; stubbed for
     * now. Tcl's selection layer treats -1 as "empty paste". */
    return -1;
});

EM_JS(int, emx11_js_clipboard_read_fetch, (int dstPtr, int capacity), {
    return 0;
});

EM_JS(void, emx11_js_clipboard_write_utf8, (int dataPtr, int len), {
    if (typeof navigator === 'undefined' ||
        !navigator.clipboard ||
        !navigator.clipboard.writeText) {
        console.warn('[emx11] clipboard write: API unavailable');
        return;
    }
    var b2 = HEAPU8.subarray(dataPtr, dataPtr + len);
    var copy = new Uint8Array(b2);
    var text = new TextDecoder('utf-8').decode(copy);
    navigator.clipboard.writeText(text).catch(function (e) {
        console.warn('[emx11] clipboard write failed:', e);
    });
});

/* --- draw ---------------------------------------------------------------- */

EM_JS(void, emx11_js_clear_area, (unsigned int id, int x, int y, int w, int h), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onClearArea(id, x, y, w, h);
});

EM_JS(void, emx11_js_fill_rect, (unsigned int id, int x, int y, int w, int h, unsigned int color), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onFillRect(id, x, y, w, h, color);
});

EM_JS(void, emx11_js_draw_line, (unsigned int id, int x1, int y1, int x2, int y2, unsigned int color, int lineWidth), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onDrawLine(id, x1, y1, x2, y2, color, lineWidth);
});

EM_JS(void, emx11_js_draw_arc, (unsigned int id, int x, int y, int w, int h, int angle1, int angle2, unsigned int color, int lineWidth), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onDrawArc(id, x, y, w, h, angle1, angle2, color, lineWidth);
});

EM_JS(void, emx11_js_fill_arc, (unsigned int id, int x, int y, int w, int h, int angle1, int angle2, unsigned int color), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onFillArc(id, x, y, w, h, angle1, angle2, color);
});

EM_JS(void, emx11_js_fill_polygon, (unsigned int id, int ptsPtr, int count, int shape, int mode, unsigned int color), {
    var pts = [];
    if (count > 0 && ptsPtr !== 0) {
        var base = ptsPtr >> 2;
        for (var i = 0; i < count; i++) {
            pts.push({ x: HEAP32[base + i * 2], y: HEAP32[base + i * 2 + 1] });
        }
    }
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onFillPolygon(id, pts, shape, mode, color);
});

EM_JS(void, emx11_js_draw_points, (unsigned int id, int ptsPtr, int count, int mode, unsigned int color), {
    var pts = [];
    if (count > 0 && ptsPtr !== 0) {
        var base = ptsPtr >> 2;
        for (var i = 0; i < count; i++) {
            pts.push({ x: HEAP32[base + i * 2], y: HEAP32[base + i * 2 + 1] });
        }
    }
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onDrawPoints(id, pts, mode, color);
});

/* --- font ---------------------------------------------------------------- */

EM_JS(void, emx11_js_draw_string, (unsigned int id, int x, int y, int fontPtr, int textPtr, int length, unsigned int fg, unsigned int bg, int imageMode), {
    var font = fontPtr !== 0 ? UTF8ToString(fontPtr) : '13px monospace';
    var text = length > 0 && textPtr !== 0 ? UTF8ToString(textPtr, length) : '';
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onDrawString(id, x, y, font, text, fg, bg, imageMode);
});

/* measure_font and measure_string are pure-JS measurements with no
 * shared state with the host bridge. Their scratchpads (one canvas
 * context, two LRU-ish maps) live under `globalThis.emX11._caches`
 * so every bridge-owned bit of state stays in one namespace. */
EM_JS(void, emx11_js_measure_font, (int fontPtr, int ascentPtr, int descentPtr, int maxWidthPtr, int widthsPtr), {
    var e = globalThis.emX11;
    var caches = e && (e._caches || (e._caches = {}));
    if (caches && caches.measureCtx === undefined) {
        var c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1)
              : typeof document !== 'undefined' ? document.createElement('canvas') : null;
        caches.measureCtx = c ? c.getContext('2d', { willReadFrequently: true }) : null;
    }
    if (caches && !caches.fontCache) caches.fontCache = new Map();
    var ctx = caches ? caches.measureCtx : null;
    var fallbackWidth = 8, fallbackAscent = 10, fallbackDescent = 3;

    if (!ctx) {
        HEAP32[ascentPtr >> 2] = fallbackAscent;
        HEAP32[descentPtr >> 2] = fallbackDescent;
        HEAP32[maxWidthPtr >> 2] = fallbackWidth;
        for (var i = 0; i < 95; i++) HEAP32[(widthsPtr >> 2) + i] = fallbackWidth;
        return;
    }

    var css = UTF8ToString(fontPtr);
    var entry = caches.fontCache.get(css);
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
    caches.fontCache.set(css, { ascent: ascent, descent: descent, maxW: maxW, widths: widths });
});

EM_JS(int, emx11_js_measure_string, (int fontPtr, int textPtr, int length), {
    if (length <= 0 || textPtr === 0) return 0;
    var e = globalThis.emX11;
    var caches = e && (e._caches || (e._caches = {}));
    if (caches && caches.measureCtx === undefined) {
        var c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1)
              : typeof document !== 'undefined' ? document.createElement('canvas') : null;
        caches.measureCtx = c ? c.getContext('2d', { willReadFrequently: true }) : null;
    }
    if (caches && !caches.textCache) caches.textCache = new Map();
    var ctx = caches ? caches.measureCtx : null;
    if (!ctx) return length * 8;
    var css = fontPtr !== 0 ? UTF8ToString(fontPtr) : '13px monospace';
    var text = UTF8ToString(textPtr, length);
    var key = css + '' + text;
    var cache = caches.textCache;
    var hit = cache.get(key);
    if (hit !== undefined) return hit;
    ctx.font = css;
    var w = Math.ceil(ctx.measureText(text).width);
    if (cache.size >= 8192) cache.clear();
    cache.set(key, w);
    return w;
});

EM_JS(int, emx11_js_parse_color, (int namePtr, int rPtr, int gPtr, int bPtr), {
    /* Pure-JS color parse; reuses the shared measureCtx under emX11._caches. */
    if (namePtr === 0) return 0;
    var name = UTF8ToString(namePtr);
    if (typeof CSS !== 'undefined' && CSS.supports &&
        !CSS.supports('color', name)) {
        return 0;
    }
    var e = globalThis.emX11;
    var caches = e && (e._caches || (e._caches = {}));
    if (caches && caches.measureCtx === undefined) {
        var c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1)
              : typeof document !== 'undefined' ? document.createElement('canvas') : null;
        caches.measureCtx = c ? c.getContext('2d', { willReadFrequently: true }) : null;
    }
    var ctx = caches ? caches.measureCtx : null;
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
});

/* --- pixmap -------------------------------------------------------------- */

EM_JS(void, emx11_js_pixmap_create, (unsigned int id, int width, int height, int depth), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onPixmapCreate(id, width, height, depth);
});

EM_JS(void, emx11_js_pixmap_destroy, (unsigned int id), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onPixmapDestroy(id);
});

EM_JS(void, emx11_js_shape_combine_mask, (unsigned int destId, unsigned int srcId, int xOff, int yOff, int op), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onShapeCombineMask(destId, srcId, xOff, yOff, op);
});

EM_JS(void, emx11_js_copy_area, (unsigned int srcId, unsigned int dstId, int srcX, int srcY, int w, int h, int dstX, int dstY), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onCopyArea(srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY);
});

EM_JS(void, emx11_js_copy_plane, (unsigned int srcId, unsigned int dstId, int srcX, int srcY, int w, int h, int dstX, int dstY, unsigned int plane, unsigned int fg, unsigned int bg, int applyBg), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onCopyPlane(srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY, plane >>> 0, fg >>> 0, bg >>> 0, applyBg !== 0);
});

EM_JS(void, emx11_js_put_image, (unsigned int dstId, int dstX, int dstY, int w, int h, int format, int depth, int bytesPerLine, int dataPtr, int dataLen, unsigned int fg, unsigned int bg), {
    var data = dataLen > 0 && dataPtr !== 0
        ? HEAPU8.slice(dataPtr, dataPtr + dataLen)
        : new Uint8Array(0);
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onPutImage(dstId >>> 0, dstX, dstY, w, h, format, depth, bytesPerLine, data, fg >>> 0, bg >>> 0);
});

/* --- property ------------------------------------------------------------ */

EM_JS(int, emx11_js_change_property, (unsigned int w, unsigned int atom, unsigned int type, int format, int mode, int dataPtr, int nelements), {
    var unit = format === 8 ? 1 : format === 16 ? 2 : format === 32 ? 4 : 0;
    if (unit === 0) return 0;
    var bytes = unit * (nelements | 0);
    var data = bytes > 0
        ? HEAPU8.slice(dataPtr, dataPtr + bytes)
        : new Uint8Array(0);
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (!Host) return -1;
    var ok = Host.changeProperty(w >>> 0, atom >>> 0, type >>> 0, format | 0, mode | 0, data);
    return ok ? 1 : 0;
});

EM_JS(void, emx11_js_get_property_meta, (unsigned int w, unsigned int atom, unsigned int reqType, int longOffset, int longLength, int metaPtr), {
    /* Layout: 0 FOUND, 1 TYPE, 2 FORMAT, 3 NITEMS, 4 BYTES_AFTER,
     *         5 DATA_LEN, 6 PRESENT, 7 reserved. Total 8 ints. */
    var base = metaPtr >> 2;
    for (var i = 0; i < 8; i++) HEAP32[base + i] = 0;
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (!Host) return;
    var r2 = Host.peekProperty(w >>> 0, atom >>> 0, reqType >>> 0,
                               longOffset | 0, longLength | 0, false);
    if (r2 === null) return;
    HEAP32[base + 6] = 1;
    HEAP32[base + 0] = r2.found ? 1 : 0;
    HEAP32[base + 1] = r2.type | 0;
    HEAP32[base + 2] = r2.format | 0;
    HEAP32[base + 3] = r2.nitems | 0;
    HEAP32[base + 4] = r2.bytesAfter | 0;
    HEAP32[base + 5] = r2.data.length | 0;
    /* Stash the data for an immediate-following get_property_data call
     * (cheap two-hop avoids re-fetch). Lives under emX11._caches with
     * the rest of the bridge-owned scratch state. */
    var caches2 = e._caches || (e._caches = {});
    caches2.propStash = r2.data;
});

EM_JS(void, emx11_js_get_property_data, (unsigned int w, unsigned int atom, unsigned int reqType, int longOffset, int longLength, int deleteFlag, int dstPtr, int capacity), {
    var e0 = globalThis.emX11;
    var caches0 = e0 && e0._caches;
    var data = caches0 ? caches0.propStash : null;
    if (caches0) caches0.propStash = null;
    if (data && data.length > 0) {
        /* Use cached from preceding PeekMeta. */
        var n = Math.min(data.length, capacity | 0);
        HEAPU8.set(data.subarray ? data.subarray(0, n) : data.slice(0, n), dstPtr);
        if (deleteFlag !== 0) {
            var e = globalThis.emX11; var Host = e && e._bridge;
            if (Host) Host.deleteProperty(w >>> 0, atom >>> 0);
        }
        return;
    }
    var e = globalThis.emX11; var Host2 = e && e._bridge;
    if (!Host2) return;
    var r3 = Host2.peekProperty(w >>> 0, atom >>> 0, reqType >>> 0,
                                longOffset | 0, longLength | 0, deleteFlag !== 0);
    if (!r3 || !r3.found || r3.data.length === 0) return;
    var n3 = Math.min(r3.data.length, capacity | 0);
    HEAPU8.set(r3.data.subarray(0, n3), dstPtr);
});

EM_JS(void, emx11_js_delete_property, (unsigned int w, unsigned int atom), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.deleteProperty(w >>> 0, atom >>> 0);
});

EM_JS(int, emx11_js_list_properties_count, (unsigned int w), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (!Host) return 0;
    return Host.listProperties(w >>> 0).length;
});

EM_JS(int, emx11_js_list_properties_fetch, (unsigned int w, int dstPtr, int capacity), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (!Host) return 0;
    var atoms2 = Host.listProperties(w >>> 0);
    var n2 = Math.min(atoms2.length, capacity | 0);
    var base2 = dstPtr >> 2;
    for (var j = 0; j < n2; j++) HEAPU32[base2 + j] = atoms2[j] >>> 0;
    return n2;
});

/* --- window -------------------------------------------------------------- */

EM_JS(void, emx11_js_window_create, (int connId, unsigned int id, unsigned int parent, int x, int y, int w, int h, int borderWidth, unsigned int borderPixel, int bgType, unsigned int bgValue), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowCreate(connId, id, parent, x, y, w, h, borderWidth, borderPixel, bgType, bgValue);
});

EM_JS(void, emx11_js_window_set_border, (unsigned int id, int borderWidth, unsigned int borderPixel), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowSetBorder(id, borderWidth, borderPixel);
});

EM_JS(void, emx11_js_window_set_bg, (unsigned int id, int bgType, unsigned int bgValue), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowSetBg(id, bgType, bgValue);
});

EM_JS(void, emx11_js_window_configure, (int connId, unsigned int id, int x, int y, int w, int h), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowConfigure(connId, id, x, y, w, h);
});

EM_JS(void, emx11_js_window_set_bit_gravity, (unsigned int id, int gravity), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowSetBitGravity(id, gravity);
});

EM_JS(void, emx11_js_window_map, (int connId, unsigned int id), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowMap(connId, id);
});

EM_JS(void, emx11_js_window_unmap, (int connId, unsigned int id), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowUnmap(connId, id);
});

EM_JS(void, emx11_js_window_destroy, (unsigned int id), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowDestroy(id);
});

EM_JS(void, emx11_js_window_raise, (unsigned int id), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowRaise(id);
});

EM_JS(void, emx11_js_select_input, (int connId, unsigned int id, unsigned int mask), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onSelectInput(connId, id, mask >>> 0);
});

EM_JS(void, emx11_js_set_override_redirect, (unsigned int id, int flag), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onSetOverrideRedirect(id, flag !== 0);
});

EM_JS(void, emx11_js_reparent_window, (unsigned int id, unsigned int parent, int x, int y), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onReparentWindow(id, parent, x, y);
});

EM_JS(void, emx11_js_window_set_bg_pixmap, (unsigned int id, unsigned int pmId), {
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowSetBgPixmap(id, pmId);
});

EM_JS(void, emx11_js_window_shape, (unsigned int id, int rectsPtr, int count), {
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
    var e = globalThis.emX11; var Host = e && e._bridge;
    if (Host) Host.onWindowShape(id, rects);
});
