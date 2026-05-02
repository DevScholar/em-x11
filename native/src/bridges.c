/*
 * em-x11 host bridges, embedded as EM_JS so libemx11 is self-contained.
 *
 * EM_JS embeds the JS body into a wasm custom section that Emscripten's
 * dlopen reads back, so the JS imports are wired up at instantiate time
 * regardless of how the .so is loaded -- works under both static link
 * (wacl-tk) and SIDE_MODULE dlopen (Pyodide), where the latter can't
 * reach the consumer's --js-library closure.
 *
 * Each bridge calls into globalThis.__EMX11__ (installed by the TS host)
 * and falls through to a harmless default when no host is present, so
 * libemx11 also dlopens cleanly in headless / pre-host environments.
 */

#include <emscripten.h>

/* Note: emx11_meta_layout.h defines named indices for the multi-int
 * outputs of get_window_attrs / get_window_abs_origin / get_property_meta.
 * The C consumers (window.c, event.c, property.c) use the names; the
 * EM_JS bodies below cannot, because EM_JS stringifies its body verbatim
 * without running CPP expansion. Keep the literal indices in EM_JS bodies
 * in lockstep with the names in emx11_meta_layout.h. */

/* Link anchor: this TU only contains EM_JS data symbols (__em_js__*),
 * which the archive linker treats as not satisfying any undefined ref
 * and therefore drops the whole .o, taking the EM_JS bodies with it.
 * display.c calls this function so the archive pulls bridges.c.o in,
 * making the JS imports visible to emcc's post-link pass. */
void emx11_bridges_link_anchor(void) {}

/* Note: EM_JS bodies are extracted as JS source BEFORE the C preprocessor
 * runs, so #defines do not expand inside them. The font/colour bridges
 * each repeat a 5-line lazy-init for the shared 2D ctx (cached on
 * globalThis so all three bridges share the underlying canvas at runtime;
 * the source duplication is unavoidable given the EM_JS extraction). */


/* --- core ---------------------------------------------------------------- */

EM_JS(void, emx11_js_init, (int screenWidth, int screenHeight), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onInit(screenWidth, screenHeight);
});

EM_JS(void, emx11_js_open_display, (int connIdPtr, int basePtr, int maskPtr), {
    if (!globalThis.__EMX11__) {
        HEAP32[connIdPtr >> 2] = 0;
        HEAPU32[basePtr >> 2] = 0;
        HEAPU32[maskPtr >> 2] = 0x001FFFFF;
        return;
    }
    var info = globalThis.__EMX11__.openDisplay();
    HEAP32[connIdPtr >> 2] = info.connId | 0;
    HEAPU32[basePtr >> 2] = info.xidBase >>> 0;
    HEAPU32[maskPtr >> 2] = info.xidMask >>> 0;
});

EM_JS(void, emx11_js_close_display, (int connId), {
    globalThis.__EMX11__ && globalThis.__EMX11__.closeDisplay(connId);
});

EM_JS(unsigned int, emx11_js_get_root_window, (void), {
    if (!globalThis.__EMX11__) return 0;
    return globalThis.__EMX11__.getRootWindow() >>> 0;
});

EM_JS(void, emx11_js_flush, (void), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onFlush();
});

EM_JS(void, emx11_js_pointer_xy, (int xPtr, int yPtr), {
    if (!globalThis.__EMX11__) {
        HEAP32[xPtr >> 2] = 0;
        HEAP32[yPtr >> 2] = 0;
        return;
    }
    var pt = globalThis.__EMX11__.getPointerXY();
    HEAP32[xPtr >> 2] = pt.x | 0;
    HEAP32[yPtr >> 2] = pt.y | 0;
});

EM_JS(void, emx11_js_get_window_attrs, (unsigned int id, int outPtr), {
    /* Layout (mirrors emx11_meta_layout.h EMX11_WIN_ATTRS_*):
     *   0 PRESENT, 1 X, 2 Y, 3 WIDTH, 4 HEIGHT, 5 MAPPED,
     *   6 OVERRIDE_RED, 7 BORDER_WIDTH. EM_JS bodies are stringified
     *   without CPP expansion, so indices stay literal here. */
    var base = outPtr >> 2;
    if (!globalThis.__EMX11__) {
        HEAP32[base + 0] = 0;
        return;
    }
    var a = globalThis.__EMX11__.getWindowAttrs(id >>> 0);
    if (!a) {
        HEAP32[base + 0] = 0;
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
});

EM_JS(void, emx11_js_get_window_abs_origin, (unsigned int id, int outPtr), {
    /* Layout (mirrors EMX11_ABS_ORIGIN_*): 0 PRESENT, 1 AX, 2 AY. */
    var base = outPtr >> 2;
    if (!globalThis.__EMX11__) {
        HEAP32[base + 0] = 0;
        return;
    }
    var o = globalThis.__EMX11__.getWindowAbsOrigin(id >>> 0);
    if (!o) {
        HEAP32[base + 0] = 0;
        return;
    }
    HEAP32[base + 0] = 1;
    HEAP32[base + 1] = o.ax | 0;
    HEAP32[base + 2] = o.ay | 0;
});

/* --- atom ---------------------------------------------------------------- */

EM_JS(unsigned int, emx11_js_intern_atom, (int namePtr, int onlyIfExists), {
    if (!globalThis.__EMX11__ || namePtr === 0) return 0;
    var name = UTF8ToString(namePtr);
    return globalThis.__EMX11__.internAtom(name, onlyIfExists !== 0) >>> 0;
});

EM_JS(int, emx11_js_get_atom_name, (unsigned int atom), {
    if (!globalThis.__EMX11__) return 0;
    var name = globalThis.__EMX11__.getAtomName(atom >>> 0);
    if (name === null) return 0;
    return stringToNewUTF8(name);
});

/* --- clipboard ----------------------------------------------------------- */
/*
 * Async clipboard read needs Asyncify or JSPI. Pyodide builds with wasm-EH
 * and no Asyncify; wacl-tk builds with Asyncify. Until JSPI lands as the
 * common ground, we expose the begin/fetch pair as a sync stub that
 * reports "unavailable" — Tcl's selection layer treats this as an empty
 * paste, same as if no clipboard permission was granted. Write is fire-
 * and-forget and works fine without unwinding tricks.
 */
EM_JS(int, emx11_js_clipboard_read_begin, (void), {
    return -1;
});

EM_JS(int, emx11_js_clipboard_read_fetch, (int dstPtr, int capacity), {
    return 0;
});

EM_JS(void, emx11_js_clipboard_write_utf8, (int dataPtr, int len), {
    if (typeof navigator === "undefined" ||
        !navigator.clipboard ||
        !navigator.clipboard.writeText) {
        console.warn("[emx11] clipboard write: API unavailable");
        return;
    }
    var bytes = HEAPU8.subarray(dataPtr, dataPtr + len);
    var copy = new Uint8Array(bytes);
    var text = new TextDecoder("utf-8").decode(copy);
    navigator.clipboard.writeText(text).catch(function (e) {
        console.warn("[emx11] clipboard write failed:", e);
    });
});

/* --- draw ---------------------------------------------------------------- */

EM_JS(void, emx11_js_clear_area, (unsigned int id, int x, int y, int w, int h), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onClearArea(id, x, y, w, h);
});

EM_JS(void, emx11_js_fill_rect, (unsigned int id, int x, int y, int w, int h, unsigned int color), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onFillRect(id, x, y, w, h, color);
});

EM_JS(void, emx11_js_draw_line, (unsigned int id, int x1, int y1, int x2, int y2, unsigned int color, int lineWidth), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onDrawLine(id, x1, y1, x2, y2, color, lineWidth);
});

EM_JS(void, emx11_js_draw_arc, (unsigned int id, int x, int y, int w, int h, int angle1, int angle2, unsigned int color, int lineWidth), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onDrawArc(id, x, y, w, h, angle1, angle2, color, lineWidth);
});

EM_JS(void, emx11_js_fill_arc, (unsigned int id, int x, int y, int w, int h, int angle1, int angle2, unsigned int color), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onFillArc(id, x, y, w, h, angle1, angle2, color);
});

EM_JS(void, emx11_js_fill_polygon, (unsigned int id, int ptsPtr, int count, int shape, int mode, unsigned int color), {
    if (!globalThis.__EMX11__) return;
    var pts = [];
    if (count > 0 && ptsPtr !== 0) {
        var base = ptsPtr >> 2;
        for (var i = 0; i < count; i++) {
            pts.push({ x: HEAP32[base + i * 2], y: HEAP32[base + i * 2 + 1] });
        }
    }
    globalThis.__EMX11__.onFillPolygon(id, pts, shape, mode, color);
});

EM_JS(void, emx11_js_draw_points, (unsigned int id, int ptsPtr, int count, int mode, unsigned int color), {
    if (!globalThis.__EMX11__) return;
    var pts = [];
    if (count > 0 && ptsPtr !== 0) {
        var base = ptsPtr >> 2;
        for (var i = 0; i < count; i++) {
            pts.push({ x: HEAP32[base + i * 2], y: HEAP32[base + i * 2 + 1] });
        }
    }
    globalThis.__EMX11__.onDrawPoints(id, pts, mode, color);
});

/* --- font ---------------------------------------------------------------- */

EM_JS(void, emx11_js_draw_string, (unsigned int id, int x, int y, int fontPtr, int textPtr, int length, unsigned int fg, unsigned int bg, int imageMode), {
    if (!globalThis.__EMX11__) return;
    var font = fontPtr !== 0 ? UTF8ToString(fontPtr) : "13px monospace";
    var text = length > 0 && textPtr !== 0 ? UTF8ToString(textPtr, length) : "";
    globalThis.__EMX11__.onDrawString(id, x, y, font, text, fg, bg, imageMode);
});

EM_JS(void, emx11_js_measure_font, (int fontPtr, int ascentPtr, int descentPtr, int maxWidthPtr, int widthsPtr), {
    if (globalThis.__emx11_measureCtx__ === undefined) {
        var c = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(1, 1)
              : typeof document !== "undefined" ? document.createElement("canvas") : null;
        globalThis.__emx11_measureCtx__ = c ? c.getContext("2d", { willReadFrequently: true }) : null;
    }
    if (!globalThis.__emx11_fontCache__) globalThis.__emx11_fontCache__ = new Map();
    var ctx = globalThis.__emx11_measureCtx__;
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
    var refMetrics = ctx.measureText("Mg");
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
        var c = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(1, 1)
              : typeof document !== "undefined" ? document.createElement("canvas") : null;
        globalThis.__emx11_measureCtx__ = c ? c.getContext("2d", { willReadFrequently: true }) : null;
    }
    if (!globalThis.__emx11_textCache__) globalThis.__emx11_textCache__ = new Map();
    var ctx = globalThis.__emx11_measureCtx__;
    if (!ctx) return length * 8;
    var css = fontPtr !== 0 ? UTF8ToString(fontPtr) : "13px monospace";
    var text = UTF8ToString(textPtr, length);
    var key = css + "" + text;
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
    if (namePtr === 0) return 0;
    var name = UTF8ToString(namePtr);
    if (typeof CSS !== "undefined" && CSS.supports &&
        !CSS.supports("color", name)) {
        return 0;
    }
    if (globalThis.__emx11_measureCtx__ === undefined) {
        var c = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(1, 1)
              : typeof document !== "undefined" ? document.createElement("canvas") : null;
        globalThis.__emx11_measureCtx__ = c ? c.getContext("2d", { willReadFrequently: true }) : null;
    }
    var ctx = globalThis.__emx11_measureCtx__;
    if (!ctx) return 0;
    ctx.fillStyle = "#010203";
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
    globalThis.__EMX11__ && globalThis.__EMX11__.onPixmapCreate(id, width, height, depth);
});

EM_JS(void, emx11_js_pixmap_destroy, (unsigned int id), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onPixmapDestroy(id);
});

EM_JS(void, emx11_js_shape_combine_mask, (unsigned int destId, unsigned int srcId, int xOff, int yOff, int op), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onShapeCombineMask(destId, srcId, xOff, yOff, op);
});

EM_JS(void, emx11_js_copy_area, (unsigned int srcId, unsigned int dstId, int srcX, int srcY, int w, int h, int dstX, int dstY), {
    globalThis.__EMX11__ &&
        globalThis.__EMX11__.onCopyArea(
            srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY,
        );
});

EM_JS(void, emx11_js_copy_plane, (unsigned int srcId, unsigned int dstId, int srcX, int srcY, int w, int h, int dstX, int dstY, unsigned int plane, unsigned int fg, unsigned int bg, int applyBg), {
    globalThis.__EMX11__ &&
        globalThis.__EMX11__.onCopyPlane(
            srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY,
            plane >>> 0, fg >>> 0, bg >>> 0, applyBg !== 0,
        );
});

EM_JS(void, emx11_js_put_image, (unsigned int dstId, int dstX, int dstY, int w, int h, int format, int depth, int bytesPerLine, int dataPtr, int dataLen, unsigned int fg, unsigned int bg), {
    if (!globalThis.__EMX11__) return;
    var data =
        dataLen > 0 && dataPtr !== 0
            ? HEAPU8.slice(dataPtr, dataPtr + dataLen)
            : new Uint8Array(0);
    globalThis.__EMX11__.onPutImage(
        dstId >>> 0, dstX, dstY, w, h, format, depth, bytesPerLine,
        data, fg >>> 0, bg >>> 0,
    );
});

/* --- property ------------------------------------------------------------ */

EM_JS(int, emx11_js_change_property, (unsigned int w, unsigned int atom, unsigned int type, int format, int mode, int dataPtr, int nelements), {
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
});

EM_JS(void, emx11_js_get_property_meta, (unsigned int w, unsigned int atom, unsigned int reqType, int longOffset, int longLength, int metaPtr), {
    /* Layout (mirrors EMX11_PROP_META_*, total 8 ints):
     *   0 FOUND, 1 TYPE, 2 FORMAT, 3 NITEMS, 4 BYTES_AFTER,
     *   5 DATA_LEN, 6 PRESENT, 7 reserved. */
    var base = metaPtr >> 2;
    for (var i = 0; i < 8; i++) HEAP32[base + i] = 0;
    if (!globalThis.__EMX11__) return;
    var r = globalThis.__EMX11__.peekProperty(
        w >>> 0, atom >>> 0, reqType >>> 0,
        longOffset | 0, longLength | 0, false,
    );
    if (r === null) {
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
});

EM_JS(void, emx11_js_get_property_data, (unsigned int w, unsigned int atom, unsigned int reqType, int longOffset, int longLength, int deleteFlag, int dstPtr, int capacity), {
    if (!globalThis.__EMX11__) return;
    var r = globalThis.__EMX11__.peekProperty(
        w >>> 0, atom >>> 0, reqType >>> 0,
        longOffset | 0, longLength | 0, deleteFlag !== 0,
    );
    if (!r || !r.found || r.data.length === 0) return;
    var n = Math.min(r.data.length, capacity | 0);
    HEAPU8.set(r.data.subarray(0, n), dstPtr);
});

EM_JS(void, emx11_js_delete_property, (unsigned int w, unsigned int atom), {
    globalThis.__EMX11__ && globalThis.__EMX11__.deleteProperty(w >>> 0, atom >>> 0);
});

EM_JS(int, emx11_js_list_properties_count, (unsigned int w), {
    if (!globalThis.__EMX11__) return 0;
    return globalThis.__EMX11__.listProperties(w >>> 0).length;
});

EM_JS(int, emx11_js_list_properties_fetch, (unsigned int w, int dstPtr, int capacity), {
    if (!globalThis.__EMX11__) return 0;
    var atoms = globalThis.__EMX11__.listProperties(w >>> 0);
    var n = Math.min(atoms.length, capacity | 0);
    var base = dstPtr >> 2;
    for (var i = 0; i < n; i++) HEAPU32[base + i] = atoms[i] >>> 0;
    return n;
});

/* --- window -------------------------------------------------------------- */

EM_JS(void, emx11_js_window_create, (int connId, unsigned int id, unsigned int parent, int x, int y, int w, int h, int borderWidth, unsigned int borderPixel, int bgType, unsigned int bgValue), {
    globalThis.__EMX11__ &&
        globalThis.__EMX11__.onWindowCreate(connId, id, parent, x, y, w, h, borderWidth, borderPixel, bgType, bgValue);
});

EM_JS(void, emx11_js_window_set_border, (unsigned int id, int borderWidth, unsigned int borderPixel), {
    globalThis.__EMX11__ &&
        globalThis.__EMX11__.onWindowSetBorder(id, borderWidth, borderPixel);
});

EM_JS(void, emx11_js_window_set_bg, (unsigned int id, int bgType, unsigned int bgValue), {
    globalThis.__EMX11__ &&
        globalThis.__EMX11__.onWindowSetBg(id, bgType, bgValue);
});

EM_JS(void, emx11_js_window_configure, (unsigned int id, int x, int y, int w, int h), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowConfigure(id, x, y, w, h);
});

EM_JS(void, emx11_js_window_map, (int connId, unsigned int id), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowMap(connId, id);
});

EM_JS(void, emx11_js_window_unmap, (int connId, unsigned int id), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowUnmap(connId, id);
});

EM_JS(void, emx11_js_window_destroy, (unsigned int id), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowDestroy(id);
});

EM_JS(void, emx11_js_window_raise, (unsigned int id), {
    globalThis.__EMX11__ && globalThis.__EMX11__.onWindowRaise(id);
});

EM_JS(void, emx11_js_select_input, (int connId, unsigned int id, unsigned int mask), {
    globalThis.__EMX11__ &&
        globalThis.__EMX11__.onSelectInput(connId, id, mask >>> 0);
});

EM_JS(void, emx11_js_set_override_redirect, (unsigned int id, int flag), {
    globalThis.__EMX11__ &&
        globalThis.__EMX11__.onSetOverrideRedirect(id, flag !== 0);
});

EM_JS(void, emx11_js_reparent_window, (unsigned int id, unsigned int parent, int x, int y), {
    globalThis.__EMX11__ &&
        globalThis.__EMX11__.onReparentWindow(id, parent, x, y);
});

EM_JS(void, emx11_js_window_set_bg_pixmap, (unsigned int id, unsigned int pmId), {
    globalThis.__EMX11__ &&
        globalThis.__EMX11__.onWindowSetBgPixmap(id, pmId);
});

EM_JS(void, emx11_js_window_shape, (unsigned int id, int rectsPtr, int count), {
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
});
