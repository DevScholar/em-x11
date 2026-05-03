/*
 * em-x11 host bridges, embedded as EM_JS / EM_ASYNC_JS so libemx11 is
 * self-contained (embedded in a wasm custom section, readable by dlopen).
 *
 * Each bridge supports two modes at runtime:
 *
 *   1. **Worker mode** (new; multi-threaded):
 *      `globalThis.__EMX11_CHANNEL__` is an RpcChannel to the Server
 *      Worker; `globalThis.__EMX11_SAB__` holds SAB views for hot reads;
 *      `globalThis.__EMX11_CONN__` has { connId, xidBase, xidMask,
 *      rootWindow } injected at client-worker bootstrap.
 *
 *   2. **Legacy mode** (compat; single-threaded main-thread host):
 *      `globalThis.__EMX11__` is the Host facade with synchronous
 *      onXxx / onYyy methods. This path is what pyodide-tk depends on.
 *
 * Category split:
 *
 *   Void fire-and-forget (29) — EM_JS. Worker path posts on channel,
 *     no reply. Legacy path calls Host method directly.
 *
 *   Hot sync queries (3) — EM_JS. Worker path reads SAB via seqlock
 *     loop. Legacy path calls Host method. These are pointer_xy,
 *     get_window_attrs, get_window_abs_origin (called from
 *     XQueryPointer / XGetWindowAttributes / per-motion hit-test).
 *
 *   Async sync returners (15) — EM_ASYNC_JS. Worker path awaits reply
 *     on channel. Legacy path returns synchronously (the extra
 *     Asyncify wrap is benign: Asyncify is already required by
 *     emscripten_sleep, so the save/restore path is live; a no-await
 *     body just pays a tiny constant).
 *
 * ORDERING: single-port postMessage is ordered. Void-then-sync
 * sequences (FillRect then Atom.Intern in one C frame) arrive in
 * order at the Server Worker.
 *
 * INDICES: EM_JS bodies are stringified BEFORE CPP runs, so the named
 * indices in emx11_meta_layout.h and sab.ts cannot be used here.
 * Keep them in lockstep.
 */

#include <emscripten.h>

/* Link anchor: this TU only contains EM_JS data symbols, which the
 * archive linker drops unless a real ref pulls the .o in. display.c
 * calls this function so emcc's post-link pass sees the JS bodies. */
void emx11_bridges_link_anchor(void) {}

/* --- core ---------------------------------------------------------------- */

EM_JS(void, emx11_js_init, (int screenWidth, int screenHeight), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Core.Init', screenWidth: screenWidth, screenHeight: screenHeight }); return; }
    var h = globalThis.__EMX11__;
    if (h) h.onInit(screenWidth, screenHeight);
});

EM_JS(void, emx11_js_open_display, (int connIdPtr, int basePtr, int maskPtr), {
    var conn = globalThis.__EMX11_CONN__;
    if (conn) {
        /* Worker mode: values were pre-assigned at BootstrapClient and
         * are already known. XOpenDisplay just echoes them back. */
        HEAP32[connIdPtr >> 2] = conn.connId | 0;
        HEAPU32[basePtr >> 2] = conn.xidBase >>> 0;
        HEAPU32[maskPtr >> 2] = conn.xidMask >>> 0;
        return;
    }
    var h = globalThis.__EMX11__;
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
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Display.Close' }); return; }
    var h = globalThis.__EMX11__;
    if (h) h.closeDisplay(connId);
});

EM_JS(unsigned int, emx11_js_get_root_window, (void), {
    var conn = globalThis.__EMX11_CONN__;
    if (conn) return conn.rootWindow >>> 0;
    var h = globalThis.__EMX11__;
    if (!h) return 0;
    return h.getRootWindow() >>> 0;
});

EM_JS(void, emx11_js_flush, (void), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Core.Flush' }); return; }
    var h = globalThis.__EMX11__;
    if (h) h.onFlush();
});

/* Hot SAB read: XQueryPointer fires at 50ms cadence per xeyes + on
 * every pointer-related Xt dispatch. Avoid an await per call. */
EM_JS(void, emx11_js_pointer_xy, (int xPtr, int yPtr), {
    var sab = globalThis.__EMX11_SAB__;
    if (sab) {
        /* Seqlock: retry until writer not mid-update and gen unchanged.
         * SAB_GEN_IDX=0, POINTER_X_IDX=1, POINTER_Y_IDX=2. */
        var i32 = sab.i32;
        for (;;) {
            var g0 = Atomics.load(i32, 0);
            if (g0 & 1) continue;
            var px = i32[1];
            var py = i32[2];
            if (Atomics.load(i32, 0) === g0) {
                HEAP32[xPtr >> 2] = px;
                HEAP32[yPtr >> 2] = py;
                return;
            }
        }
    }
    var h = globalThis.__EMX11__;
    if (!h) {
        HEAP32[xPtr >> 2] = 0;
        HEAP32[yPtr >> 2] = 0;
        return;
    }
    var pt = h.getPointerXY();
    HEAP32[xPtr >> 2] = pt.x | 0;
    HEAP32[yPtr >> 2] = pt.y | 0;
});

/* Hot SAB read: called on every motion/button hit-test + each client's
 * own XGetWindowAttributes. Output layout matches emx11_meta_layout.h
 * EMX11_WIN_ATTRS_* (0 PRESENT, 1 X, 2 Y, 3 W, 4 H, 5 MAPPED,
 * 6 OVERRIDE, 7 BORDER_WIDTH). */
EM_JS(void, emx11_js_get_window_attrs, (unsigned int id, int outPtr), {
    var out = outPtr >> 2;
    var sab = globalThis.__EMX11_SAB__;
    if (sab) {
        var xts = globalThis.__EMX11_XID_TO_SLOT__;
        var conn = globalThis.__EMX11_CONN__;
        if (xts && conn) {
            var rel = (id >>> 0) - (conn.xidBase >>> 0);
            var slot = (rel >= 0 && rel < xts.length) ? xts[rel] : 0;
            /* slot 0 = not mirrored (either not ours, or not yet
             * pushed). Fall through to RPC. */
            if (slot > 0) {
                /* Header = 16 ints; attr stride = 8. */
                var base = 16 + slot * 8;
                var i32 = sab.i32;
                for (;;) {
                    var g0 = Atomics.load(i32, 0);
                    if (g0 & 1) continue;
                    var p = i32[base + 0];
                    var x = i32[base + 1], y = i32[base + 2];
                    var w = i32[base + 3], hh = i32[base + 4];
                    var m = i32[base + 5], ov = i32[base + 6], bw = i32[base + 7];
                    if (Atomics.load(i32, 0) === g0) {
                        HEAP32[out + 0] = p;
                        HEAP32[out + 1] = x;
                        HEAP32[out + 2] = y;
                        HEAP32[out + 3] = w;
                        HEAP32[out + 4] = hh;
                        HEAP32[out + 5] = m;
                        HEAP32[out + 6] = ov;
                        HEAP32[out + 7] = bw;
                        return;
                    }
                }
            }
        }
        /* SAB miss → synchronous "not present"; the async RPC
         * fallback would require EM_ASYNC_JS, too heavy for every
         * hit-test. Clients that race the SAB seed get 0 briefly,
         * same as today's "conn not bound yet" behaviour. */
        HEAP32[out + 0] = 0;
        return;
    }
    var h = globalThis.__EMX11__;
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

/* Hot SAB read: output layout matches EMX11_ABS_ORIGIN_* (0 PRESENT,
 * 1 AX, 2 AY). */
EM_JS(void, emx11_js_get_window_abs_origin, (unsigned int id, int outPtr), {
    var out = outPtr >> 2;
    var sab = globalThis.__EMX11_SAB__;
    if (sab) {
        var xts = globalThis.__EMX11_XID_TO_SLOT__;
        var conn = globalThis.__EMX11_CONN__;
        if (xts && conn) {
            var rel = (id >>> 0) - (conn.xidBase >>> 0);
            var slot = (rel >= 0 && rel < xts.length) ? xts[rel] : 0;
            if (slot > 0) {
                /* Header 16 ints + attr table 128*8=1024 ints; origin
                 * stride = 3. */
                var base = 16 + 128 * 8 + slot * 3;
                var i32 = sab.i32;
                for (;;) {
                    var g0 = Atomics.load(i32, 0);
                    if (g0 & 1) continue;
                    var p = i32[base + 0];
                    var ax = i32[base + 1], ay = i32[base + 2];
                    if (Atomics.load(i32, 0) === g0) {
                        HEAP32[out + 0] = p;
                        HEAP32[out + 1] = ax;
                        HEAP32[out + 2] = ay;
                        return;
                    }
                }
            }
        }
        HEAP32[out + 0] = 0;
        return;
    }
    var h = globalThis.__EMX11__;
    if (!h) { HEAP32[out + 0] = 0; return; }
    var o = h.getWindowAbsOrigin(id >>> 0);
    if (!o) { HEAP32[out + 0] = 0; return; }
    HEAP32[out + 0] = 1;
    HEAP32[out + 1] = o.ax | 0;
    HEAP32[out + 2] = o.ay | 0;
});

/* --- passive grabs (XGrabButton / XUngrabButton) ------------------------- */

EM_JS(void, emx11_js_grab_button,
      (unsigned int window, unsigned int button, unsigned int modifiers,
       int owner_events, unsigned int event_mask,
       int pointer_mode, int keyboard_mode,
       unsigned int confine_to, unsigned int cursor),
      {
          var ch = globalThis.__EMX11_CHANNEL__;
          if (ch) {
              ch.send({
                  kind: 'Grab.Button',
                  window: window >>> 0, button: button >>> 0, modifiers: modifiers >>> 0,
                  ownerEvents: owner_events !== 0, eventMask: event_mask >>> 0,
                  pointerMode: pointer_mode | 0, keyboardMode: keyboard_mode | 0,
                  confineTo: confine_to >>> 0, cursor: cursor >>> 0,
              });
              return;
          }
          var h = globalThis.__EMX11__;
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
          var ch = globalThis.__EMX11_CHANNEL__;
          if (ch) {
              ch.send({
                  kind: 'Ungrab.Button',
                  window: window >>> 0, button: button >>> 0, modifiers: modifiers >>> 0,
              });
              return;
          }
          var h = globalThis.__EMX11__;
          if (h) h.onUngrabButton(window >>> 0, button >>> 0, modifiers >>> 0);
      });

/* --- atom (sync return: EM_ASYNC_JS) ------------------------------------- */

EM_ASYNC_JS(unsigned int, emx11_js_intern_atom, (int namePtr, int onlyIfExists), {
    if (namePtr === 0) return 0;
    var name = UTF8ToString(namePtr);
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        try {
            var r = await ch.call({
                kind: 'Atom.Intern', name: name, onlyIfExists: onlyIfExists !== 0,
            });
            return r.atom >>> 0;
        } catch (e) {
            console.warn('[emx11] intern_atom RPC failed:', e);
            return 0;
        }
    }
    var h = globalThis.__EMX11__;
    if (!h) return 0;
    return h.internAtom(name, onlyIfExists !== 0) >>> 0;
});

EM_ASYNC_JS(int, emx11_js_get_atom_name, (unsigned int atom), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        try {
            var r = await ch.call({ kind: 'Atom.GetName', atom: atom >>> 0 });
            if (r.name === null || r.name === undefined) return 0;
            return stringToNewUTF8(r.name);
        } catch (e) {
            console.warn('[emx11] get_atom_name RPC failed:', e);
            return 0;
        }
    }
    var h = globalThis.__EMX11__;
    if (!h) return 0;
    var name = h.getAtomName(atom >>> 0);
    if (name === null) return 0;
    return stringToNewUTF8(name);
});

/* --- clipboard ----------------------------------------------------------- */

EM_JS(int, emx11_js_clipboard_read_begin, (void), {
    /* Async read still requires JSPI or Asyncify unwind; stubbed for
     * now in both modes. Tcl's selection layer treats -1 as "empty
     * paste". */
    return -1;
});

EM_JS(int, emx11_js_clipboard_read_fetch, (int dstPtr, int capacity), {
    return 0;
});

EM_JS(void, emx11_js_clipboard_write_utf8, (int dataPtr, int len), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        var bytes = HEAPU8.slice(dataPtr, dataPtr + len);
        ch.send({ kind: 'Clipboard.WriteUtf8', bytes: bytes }, [bytes.buffer]);
        return;
    }
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
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.ClearArea', id: id >>> 0, x: x, y: y, w: w, h: h }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onClearArea(id, x, y, w, h);
});

EM_JS(void, emx11_js_fill_rect, (unsigned int id, int x, int y, int w, int h, unsigned int color), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.FillRect', id: id >>> 0, x: x, y: y, w: w, h: h, color: color >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onFillRect(id, x, y, w, h, color);
});

EM_JS(void, emx11_js_draw_line, (unsigned int id, int x1, int y1, int x2, int y2, unsigned int color, int lineWidth), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.Line', id: id >>> 0, x1: x1, y1: y1, x2: x2, y2: y2, color: color >>> 0, lineWidth: lineWidth }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onDrawLine(id, x1, y1, x2, y2, color, lineWidth);
});

EM_JS(void, emx11_js_draw_arc, (unsigned int id, int x, int y, int w, int h, int angle1, int angle2, unsigned int color, int lineWidth), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.Arc', id: id >>> 0, x: x, y: y, w: w, h: h, angle1: angle1, angle2: angle2, color: color >>> 0, lineWidth: lineWidth }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onDrawArc(id, x, y, w, h, angle1, angle2, color, lineWidth);
});

EM_JS(void, emx11_js_fill_arc, (unsigned int id, int x, int y, int w, int h, int angle1, int angle2, unsigned int color), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.FillArc', id: id >>> 0, x: x, y: y, w: w, h: h, angle1: angle1, angle2: angle2, color: color >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
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
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.FillPolygon', id: id >>> 0, pts: pts, shape: shape, mode: mode, color: color >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
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
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.Points', id: id >>> 0, pts: pts, mode: mode, color: color >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onDrawPoints(id, pts, mode, color);
});

/* --- font ---------------------------------------------------------------- */

EM_JS(void, emx11_js_draw_string, (unsigned int id, int x, int y, int fontPtr, int textPtr, int length, unsigned int fg, unsigned int bg, int imageMode), {
    var font = fontPtr !== 0 ? UTF8ToString(fontPtr) : '13px monospace';
    var text = length > 0 && textPtr !== 0 ? UTF8ToString(textPtr, length) : '';
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        ch.send({
            kind: 'Draw.String', id: id >>> 0, x: x, y: y, font: font, text: text,
            fg: fg >>> 0, bg: bg >>> 0, imageMode: imageMode,
        });
        return;
    }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onDrawString(id, x, y, font, text, fg, bg, imageMode);
});

/* measure_font and measure_string are pure-JS measurements with no
 * shared state. They can safely run inside the Client Worker using
 * its OWN OffscreenCanvas (no RPC needed). We keep the existing
 * lazy-init of globalThis.__emx11_measureCtx__ -- it now lives
 * per-worker, but each worker only measures its own fonts. Cache is
 * also per-worker. */
EM_JS(void, emx11_js_measure_font, (int fontPtr, int ascentPtr, int descentPtr, int maxWidthPtr, int widthsPtr), {
    if (globalThis.__emx11_measureCtx__ === undefined) {
        var c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1)
              : typeof document !== 'undefined' ? document.createElement('canvas') : null;
        globalThis.__emx11_measureCtx__ = c ? c.getContext('2d', { willReadFrequently: true }) : null;
    }
    if (!globalThis.__emx11_fontCache__) globalThis.__emx11_fontCache__ = new Map();
    var ctx = globalThis.__emx11_measureCtx__;
    var fallbackWidth = 8, fallbackAscent = 10, fallbackDescent = 3;

    if (!ctx) {
        HEAP32[ascentPtr >> 2] = fallbackAscent;
        HEAP32[descentPtr >> 2] = fallbackDescent;
        HEAP32[maxWidthPtr >> 2] = fallbackWidth;
        for (var i = 0; i < 95; i++) HEAP32[(widthsPtr >> 2) + i] = fallbackWidth;
        return;
    }

    var css = UTF8ToString(fontPtr);
    var entry = globalThis.__emx11_fontCache__.get(css);
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
    globalThis.__emx11_fontCache__.set(css, { ascent: ascent, descent: descent, maxW: maxW, widths: widths });
});

EM_JS(int, emx11_js_measure_string, (int fontPtr, int textPtr, int length), {
    if (length <= 0 || textPtr === 0) return 0;
    if (globalThis.__emx11_measureCtx__ === undefined) {
        var c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1)
              : typeof document !== 'undefined' ? document.createElement('canvas') : null;
        globalThis.__emx11_measureCtx__ = c ? c.getContext('2d', { willReadFrequently: true }) : null;
    }
    if (!globalThis.__emx11_textCache__) globalThis.__emx11_textCache__ = new Map();
    var ctx = globalThis.__emx11_measureCtx__;
    if (!ctx) return length * 8;
    var css = fontPtr !== 0 ? UTF8ToString(fontPtr) : '13px monospace';
    var text = UTF8ToString(textPtr, length);
    var key = css + '' + text;
    var cache = globalThis.__emx11_textCache__;
    var hit = cache.get(key);
    if (hit !== undefined) return hit;
    ctx.font = css;
    var w = Math.ceil(ctx.measureText(text).width);
    if (cache.size >= 8192) cache.clear();
    cache.set(key, w);
    return w;
});

EM_JS(int, emx11_js_parse_color, (int namePtr, int rPtr, int gPtr, int bPtr), {
    /* Pure-JS color parse; same as measure — no shared state needed.
     * Each worker parses into its own ctx. */
    if (namePtr === 0) return 0;
    var name = UTF8ToString(namePtr);
    if (typeof CSS !== 'undefined' && CSS.supports &&
        !CSS.supports('color', name)) {
        return 0;
    }
    if (globalThis.__emx11_measureCtx__ === undefined) {
        var c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1)
              : typeof document !== 'undefined' ? document.createElement('canvas') : null;
        globalThis.__emx11_measureCtx__ = c ? c.getContext('2d', { willReadFrequently: true }) : null;
    }
    var ctx = globalThis.__emx11_measureCtx__;
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
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.PixmapCreate', id: id >>> 0, width: width, height: height, depth: depth }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onPixmapCreate(id, width, height, depth);
});

EM_JS(void, emx11_js_pixmap_destroy, (unsigned int id), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.PixmapDestroy', id: id >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onPixmapDestroy(id);
});

EM_JS(void, emx11_js_shape_combine_mask, (unsigned int destId, unsigned int srcId, int xOff, int yOff, int op), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.ShapeCombineMask', destId: destId >>> 0, srcId: srcId >>> 0, xOff: xOff, yOff: yOff, op: op }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onShapeCombineMask(destId, srcId, xOff, yOff, op);
});

EM_JS(void, emx11_js_copy_area, (unsigned int srcId, unsigned int dstId, int srcX, int srcY, int w, int h, int dstX, int dstY), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Draw.CopyArea', srcId: srcId >>> 0, dstId: dstId >>> 0, srcX: srcX, srcY: srcY, w: w, h: h, dstX: dstX, dstY: dstY }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onCopyArea(srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY);
});

EM_JS(void, emx11_js_copy_plane, (unsigned int srcId, unsigned int dstId, int srcX, int srcY, int w, int h, int dstX, int dstY, unsigned int plane, unsigned int fg, unsigned int bg, int applyBg), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        ch.send({
            kind: 'Draw.CopyPlane',
            srcId: srcId >>> 0, dstId: dstId >>> 0, srcX: srcX, srcY: srcY,
            w: w, h: h, dstX: dstX, dstY: dstY, plane: plane >>> 0,
            fg: fg >>> 0, bg: bg >>> 0, applyBg: applyBg !== 0,
        });
        return;
    }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onCopyPlane(srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY, plane >>> 0, fg >>> 0, bg >>> 0, applyBg !== 0);
});

EM_JS(void, emx11_js_put_image, (unsigned int dstId, int dstX, int dstY, int w, int h, int format, int depth, int bytesPerLine, int dataPtr, int dataLen, unsigned int fg, unsigned int bg), {
    var data = dataLen > 0 && dataPtr !== 0
        ? HEAPU8.slice(dataPtr, dataPtr + dataLen)
        : new Uint8Array(0);
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        ch.send({
            kind: 'Draw.PutImage', dstId: dstId >>> 0, dstX: dstX, dstY: dstY,
            w: w, h: h, format: format, depth: depth, bytesPerLine: bytesPerLine,
            data: data, fg: fg >>> 0, bg: bg >>> 0,
        }, data.buffer ? [data.buffer] : []);
        return;
    }
    var Host = globalThis.__EMX11__;
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
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        ch.send({
            kind: 'Property.Change',
            w: w >>> 0, atom: atom >>> 0, type: type >>> 0,
            format: format | 0, mode: mode | 0, data: data,
        }, data.buffer && data.byteLength > 0 ? [data.buffer] : []);
        return 1;
    }
    var Host = globalThis.__EMX11__;
    if (!Host) return -1;
    var ok = Host.changeProperty(w >>> 0, atom >>> 0, type >>> 0, format | 0, mode | 0, data);
    return ok ? 1 : 0;
});

EM_ASYNC_JS(void, emx11_js_get_property_meta, (unsigned int w, unsigned int atom, unsigned int reqType, int longOffset, int longLength, int metaPtr), {
    /* Layout: 0 FOUND, 1 TYPE, 2 FORMAT, 3 NITEMS, 4 BYTES_AFTER,
     *         5 DATA_LEN, 6 PRESENT, 7 reserved. Total 8 ints. */
    var base = metaPtr >> 2;
    for (var i = 0; i < 8; i++) HEAP32[base + i] = 0;
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        try {
            var r = await ch.call({
                kind: 'Property.PeekMeta',
                w: w >>> 0, atom: atom >>> 0, reqType: reqType >>> 0,
                longOffset: longOffset | 0, longLength: longLength | 0,
            });
            if (r === null || r === undefined) return;
            HEAP32[base + 6] = 1;
            HEAP32[base + 0] = r.found ? 1 : 0;
            HEAP32[base + 1] = r.type | 0;
            HEAP32[base + 2] = r.format | 0;
            HEAP32[base + 3] = r.nitems | 0;
            HEAP32[base + 4] = r.bytesAfter | 0;
            HEAP32[base + 5] = r.dataLen | 0;
            /* Stash the data for an immediate-following get_property_data
             * call. Server-side held the underlying buffer between these
             * two calls (two hops, but the second is trivially fast). */
            globalThis.__EMX11_PROP_STASH__ = r.data;
            return;
        } catch (e) {
            console.warn('[emx11] property meta RPC failed:', e);
            return;
        }
    }
    var Host = globalThis.__EMX11__;
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
    globalThis.__EMX11_PROP_STASH__ = r2.data;
});

EM_ASYNC_JS(void, emx11_js_get_property_data, (unsigned int w, unsigned int atom, unsigned int reqType, int longOffset, int longLength, int deleteFlag, int dstPtr, int capacity), {
    var ch = globalThis.__EMX11_CHANNEL__;
    var data = globalThis.__EMX11_PROP_STASH__;
    globalThis.__EMX11_PROP_STASH__ = null;
    if (data && data.length > 0) {
        /* Use cached from preceding PeekMeta. */
        var n = Math.min(data.length, capacity | 0);
        HEAPU8.set(data.subarray ? data.subarray(0, n) : data.slice(0, n), dstPtr);
        if (ch && deleteFlag !== 0) {
            ch.send({ kind: 'Property.Delete', w: w >>> 0, atom: atom >>> 0 });
        } else if (!ch && deleteFlag !== 0) {
            var Host = globalThis.__EMX11__;
            if (Host) Host.deleteProperty(w >>> 0, atom >>> 0);
        }
        return;
    }
    if (ch) {
        try {
            var r = await ch.call({
                kind: 'Property.PeekData',
                w: w >>> 0, atom: atom >>> 0, reqType: reqType >>> 0,
                longOffset: longOffset | 0, longLength: longLength | 0,
                deleteFlag: deleteFlag !== 0,
            });
            if (!r || !r.data || r.data.byteLength === 0) return;
            var bytes = new Uint8Array(r.data);
            var n2 = Math.min(bytes.length, capacity | 0);
            HEAPU8.set(bytes.subarray(0, n2), dstPtr);
        } catch (e) {
            console.warn('[emx11] property data RPC failed:', e);
        }
        return;
    }
    var Host2 = globalThis.__EMX11__;
    if (!Host2) return;
    var r3 = Host2.peekProperty(w >>> 0, atom >>> 0, reqType >>> 0,
                                longOffset | 0, longLength | 0, deleteFlag !== 0);
    if (!r3 || !r3.found || r3.data.length === 0) return;
    var n3 = Math.min(r3.data.length, capacity | 0);
    HEAPU8.set(r3.data.subarray(0, n3), dstPtr);
});

EM_JS(void, emx11_js_delete_property, (unsigned int w, unsigned int atom), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Property.Delete', w: w >>> 0, atom: atom >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.deleteProperty(w >>> 0, atom >>> 0);
});

EM_ASYNC_JS(int, emx11_js_list_properties_count, (unsigned int w), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        try {
            var r = await ch.call({ kind: 'Property.ListCount', w: w >>> 0 });
            /* Stash atoms for immediate-following fetch call to avoid a
             * second RPC round trip. */
            globalThis.__EMX11_PROPLIST_STASH__ = r.atoms;
            return r.atoms.length | 0;
        } catch (e) {
            console.warn('[emx11] property list count RPC failed:', e);
            return 0;
        }
    }
    var Host = globalThis.__EMX11__;
    if (!Host) return 0;
    return Host.listProperties(w >>> 0).length;
});

EM_JS(int, emx11_js_list_properties_fetch, (unsigned int w, int dstPtr, int capacity), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        var atoms = globalThis.__EMX11_PROPLIST_STASH__;
        globalThis.__EMX11_PROPLIST_STASH__ = null;
        if (!atoms) return 0;
        var n = Math.min(atoms.length, capacity | 0);
        var base = dstPtr >> 2;
        for (var i = 0; i < n; i++) HEAPU32[base + i] = atoms[i] >>> 0;
        return n;
    }
    var Host = globalThis.__EMX11__;
    if (!Host) return 0;
    var atoms2 = Host.listProperties(w >>> 0);
    var n2 = Math.min(atoms2.length, capacity | 0);
    var base2 = dstPtr >> 2;
    for (var j = 0; j < n2; j++) HEAPU32[base2 + j] = atoms2[j] >>> 0;
    return n2;
});

/* --- window -------------------------------------------------------------- */

EM_JS(void, emx11_js_window_create, (int connId, unsigned int id, unsigned int parent, int x, int y, int w, int h, int borderWidth, unsigned int borderPixel, int bgType, unsigned int bgValue), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) {
        ch.send({
            kind: 'Window.Create', connId: connId | 0, id: id >>> 0, parent: parent >>> 0,
            x: x, y: y, w: w, h: h, borderWidth: borderWidth, borderPixel: borderPixel >>> 0,
            bgType: bgType, bgValue: bgValue >>> 0,
        });
        return;
    }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onWindowCreate(connId, id, parent, x, y, w, h, borderWidth, borderPixel, bgType, bgValue);
});

EM_JS(void, emx11_js_window_set_border, (unsigned int id, int borderWidth, unsigned int borderPixel), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.SetBorder', id: id >>> 0, borderWidth: borderWidth, borderPixel: borderPixel >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onWindowSetBorder(id, borderWidth, borderPixel);
});

EM_JS(void, emx11_js_window_set_bg, (unsigned int id, int bgType, unsigned int bgValue), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.SetBg', id: id >>> 0, bgType: bgType, bgValue: bgValue >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onWindowSetBg(id, bgType, bgValue);
});

EM_JS(void, emx11_js_window_configure, (unsigned int id, int x, int y, int w, int h), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.Configure', id: id >>> 0, x: x, y: y, w: w, h: h }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onWindowConfigure(id, x, y, w, h);
});

EM_JS(void, emx11_js_window_map, (int connId, unsigned int id), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.Map', connId: connId | 0, id: id >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onWindowMap(connId, id);
});

EM_JS(void, emx11_js_window_unmap, (int connId, unsigned int id), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.Unmap', connId: connId | 0, id: id >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onWindowUnmap(connId, id);
});

EM_JS(void, emx11_js_window_destroy, (unsigned int id), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.Destroy', id: id >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onWindowDestroy(id);
});

EM_JS(void, emx11_js_window_raise, (unsigned int id), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.Raise', id: id >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onWindowRaise(id);
});

EM_JS(void, emx11_js_select_input, (int connId, unsigned int id, unsigned int mask), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.SelectInput', connId: connId | 0, id: id >>> 0, mask: mask >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onSelectInput(connId, id, mask >>> 0);
});

EM_JS(void, emx11_js_set_override_redirect, (unsigned int id, int flag), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.SetOverrideRedirect', id: id >>> 0, flag: flag !== 0 }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onSetOverrideRedirect(id, flag !== 0);
});

EM_JS(void, emx11_js_reparent_window, (unsigned int id, unsigned int parent, int x, int y), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.Reparent', id: id >>> 0, parent: parent >>> 0, x: x, y: y }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onReparentWindow(id, parent, x, y);
});

EM_JS(void, emx11_js_window_set_bg_pixmap, (unsigned int id, unsigned int pmId), {
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.SetBgPixmap', id: id >>> 0, pmId: pmId >>> 0 }); return; }
    var Host = globalThis.__EMX11__;
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
    var ch = globalThis.__EMX11_CHANNEL__;
    if (ch) { ch.send({ kind: 'Window.Shape', id: id >>> 0, rects: rects }); return; }
    var Host = globalThis.__EMX11__;
    if (Host) Host.onWindowShape(id, rects);
});
