/*
 * em-x11 host bridges, embedded as EM_JS for the SIDE_MODULE path
 * (Pyodide dlopen).  In the static-link path (-sUSE_EM_X11) the JS
 * library at native/src/lib/library_em-x11.js overrides these EM_JS
 * bodies.
 *
 * SYNC CONTRACT: Every em_x11_js_* function defined here MUST have a
 * matching entry in library_em-x11.js with the same name and compatible
 * signature. When you add, rename, or remove a bridge function here,
 * update the JS library too. A mismatched set produces silent failures
 * (EM_JS body wins over library stub, or vice versa) with no build error.
 * The check-bridge-sync CMake target verifies the two lists match.
 *
 * Host reference: every bridge reads Module['emX11Host'] (a flat
 * Module property, per emscripten convention).  Internal caches
 * live under Module['emX11Caches']; debug flags under
 * Module['emX11Debug'].
 *
 * Public API (em.fs, em.spawn, em.display) is on the TypeScript
 * Host object, not on Module.
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
 * had to unwind via Asyncify/JSPI. In direct mode every JS body returns
 * sync, so EM_ASYNC_JS is pure overhead -- worse, it forces an
 * unwind/rewind on EVERY call, which makes user-side `tcldide_eval`
 * always return a Promise (suspends mid-eval), breaking the sync
 * runTcl entry point.
 */

#include <emscripten.h>

/* Forward declarations for the two non-EM_JS entries this TU exports.
 * Pulling in em_x11_internal.h would conflict with the looser
 * `unsigned int` parameter types EM_JS uses below. */
typedef struct _XDisplay Display;
Display* em_x11_get_display(void);
void em_x11_repoll_pointer_window(Display* dpy);
void em_x11_repoll_pointer_window_hint(Display* dpy, unsigned long cur_hint);

/* Link anchor: this TU only contains EM_JS data symbols, which the
 * archive linker drops unless a real ref pulls the .o in. display_init.c
 * calls this function so emcc's post-link pass sees the JS bodies. */
void em_x11_bridges_link_anchor(void) {}

/* --- core ---------------------------------------------------------------- */

/* clang-format off */

EM_JS(void, em_x11_js_init, (int screenWidth, int screenHeight), {
  var host = Module['emX11Host'];
  if (host)
    host.onInit(screenWidth, screenHeight);
});

EM_JS(void, em_x11_js_open_display, (int connIdPtr, int basePtr, int maskPtr), {
  /* Side-module path (Pyodide dlopen in a Worker): Module['emX11Host'] is
   * unset until openDisplay writes it. Fall back to the globalThis slot
   * that attachToBridge() populates. */
  var host = Module['emX11Host'] || globalThis.__emX11Host;
  if (!host) {
    HEAP32[connIdPtr >> 2] = 0;
    HEAPU32[basePtr >> 2] = 0;
    HEAPU32[maskPtr >> 2] = 0x001FFFFF;
    return;
  }
  var info = host.openDisplay(Module);
  if (typeof EmX11Host !== 'undefined') EmX11Host.connId = info.connId;
  HEAP32[connIdPtr >> 2] = info.connId | 0;
  HEAPU32[basePtr >> 2] = info.xidBase >>> 0;
  HEAPU32[maskPtr >> 2] = info.xidMask >>> 0;
});

EM_JS(void, em_x11_js_close_display, (int connId), {
  if (typeof EmX11Host !== 'undefined') EmX11Host.connId = 0;
  var host = Module['emX11Host'];
  if (host) {
    host.closeDisplay(connId);
  }
});

EM_JS(unsigned int, em_x11_js_get_root_window, (void), {
  var host = Module['emX11Host'];
  if (!host)
    return 0;
  return host.getRootWindow() >>> 0;
});

EM_JS(void, em_x11_js_flush, (void), {
  var host = Module['emX11Host'];
  if (host)
    host.onFlush();
});

EM_JS(void, em_x11_js_flush_roundtrip, (void), {
  var host = Module['emX11Host'];
  if (host && host.onFlushRoundtrip)
    host.onFlushRoundtrip();
});

/* Hot read: XQueryPointer fires at 50ms cadence per xeyes + on every
 * pointer-related Xt dispatch. */
EM_JS(void, em_x11_js_pointer_xy, (int xPtr, int yPtr), {
  var host = Module['emX11Host'];
  if (!host) {
    HEAP32[xPtr >> 2] = 0;
    HEAP32[yPtr >> 2] = 0;
    return;
  }
  var pt = host.getPointerXY();
  HEAP32[xPtr >> 2] = pt.x | 0;
  HEAP32[yPtr >> 2] = pt.y | 0;
});

/* Hot read: called on every motion/button hit-test + each client's
 * own XGetWindowAttributes. Output layout matches em_x11_meta_layout.h
 * EM_X11_WIN_ATTRS_* (0 PRESENT, 1 X, 2 Y, 3 W, 4 H, 5 MAPPED,
 * 6 OVERRIDE, 7 BORDER_WIDTH). */
EM_JS(void, em_x11_js_get_window_attrs, (unsigned int id, int outPtr), {
  var out = outPtr >> 2;
  var host = Module['emX11Host'];
  if (!host) {
    HEAP32[out + 0] = 0;
    return;
  }
  var a = host.getWindowAttrs(id >>> 0);
  if (!a) {
    HEAP32[out + 0] = 0;
    return;
  }
  HEAP32[out + 0] = 1;
  HEAP32[out + 1] = a.x | 0;
  HEAP32[out + 2] = a.y | 0;
  HEAP32[out + 3] = a.width | 0;
  HEAP32[out + 4] = a.height | 0;
  HEAP32[out + 5] = a.mapped ? 1 : 0;
  HEAP32[out + 6] = a.overrideRedirect ? 1 : 0;
  HEAP32[out + 7] = a.borderWidth | 0;
});

/* Hot read: output layout matches EM_X11_ABS_ORIGIN_* (0 PRESENT,
 * 1 AX, 2 AY). */
EM_JS(void, em_x11_js_get_window_abs_origin, (unsigned int id, int outPtr), {
  var out = outPtr >> 2;
  var host = Module['emX11Host'];
  if (!host) {
    HEAP32[out + 0] = 0;
    return;
  }
  var o = host.getWindowAbsOrigin(id >>> 0);
  if (!o) {
    HEAP32[out + 0] = 0;
    return;
  }
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
EM_JS(int, em_x11_js_get_window_shape_count, (unsigned int id), {
  var host = Module['emX11Host'];
  if (!host)
    return -1;
  var rects = host.getWindowShape(id >>> 0);
  if (rects == null || rects == undefined)
    return -1;
  /* Stash for the immediate-following fetch so we don't re-query. */
  var caches = Module['emX11Caches'] || (Module['emX11Caches'] = {});
  caches.shapeStash = {id : id >>> 0, rects : rects};
  return rects.length | 0;
});

EM_JS(int,
      em_x11_js_get_window_shape_rects,
      (unsigned int id, int dstPtr, int capacity),
      {
        var caches = Module['emX11Caches'];
        var stashed = caches && caches.shapeStash;
        var rects =
          (stashed && stashed.id == (id >>> 0)) ? stashed.rects : null;
        if (caches)
          caches.shapeStash = null;
        if (!rects) {
          var host = Module['emX11Host'];
          if (!host)
            return 0;
          rects = host.getWindowShape(id >>> 0);
          if (!rects)
            return 0;
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

/* XQueryTree cross-conn: list mapped children of `parent` (root is the
 * usual caller, from twm's RestartPreviousState walk). Two-call pattern
 * matching shape: count first, then fetch into a sized buffer. The
 * count-call stashes the array so the fetch doesn't re-query. */
EM_JS(int, em_x11_js_get_window_children_count, (unsigned int parent), {
  var host = Module['emX11Host'];
  if (!host)
    return 0;
  var kids = host.getWindowChildren(parent >>> 0);
  if (!kids)
    return 0;
  var caches = Module['emX11Caches'] || (Module['emX11Caches'] = {});
  caches.childrenStash = {parent : parent >>> 0, kids : kids};
  return kids.length | 0;
});

EM_JS(int,
      em_x11_js_get_window_children,
      (unsigned int parent, int dstPtr, int capacity),
      {
        var caches = Module['emX11Caches'];
        var stashed = caches && caches.childrenStash;
        var kids =
          (stashed && stashed.parent == (parent >>> 0)) ? stashed.kids : null;
        if (caches)
          caches.childrenStash = null;
        if (!kids) {
          var host = Module['emX11Host'];
          if (!host)
            return 0;
          kids = host.getWindowChildren(parent >>> 0);
          if (!kids)
            return 0;
        }
        var n = Math.min(kids.length | 0, capacity | 0);
        var base = dstPtr >> 2;
        for (var i = 0; i < n; i++)
          HEAPU32[base + i] = kids[i] >>> 0;
        return n;
      });

/* --- window-tree queries (cross-connection) ----------------------------- */

EM_JS(int, em_x11_js_is_ancestor, (unsigned int ancestor, unsigned int descendant), {
  var host = Module['emX11Host'];
  if (!host)
    return 0;
  return host.isAncestor(ancestor >>> 0, descendant >>> 0) ? 1 : 0;
});

EM_JS(unsigned int, em_x11_js_get_parent, (unsigned int window), {
  var host = Module['emX11Host'];
  if (!host)
    return 0;
  return host.getParent(window >>> 0) >>> 0;
});

EM_JS(unsigned int, em_x11_js_common_ancestor, (unsigned int a, unsigned int b), {
  var host = Module['emX11Host'];
  if (!host)
    return 0;
  return host.commonAncestor(a >>> 0, b >>> 0) >>> 0;
});

/* --- passive grabs (XGrabButton / XUngrabButton) ------------------------- */

EM_JS(void,
      em_x11_js_grab_button,
      (unsigned int window,
       unsigned int button,
       unsigned int modifiers,
       int owner_events,
       unsigned int event_mask,
       int pointer_mode,
       int keyboard_mode,
       unsigned int confine_to,
       unsigned int cursor),
      {
        var host = Module['emX11Host'];
        if (!host)
          return;
        host.onGrabButton(window >>> 0,
                          button >>> 0,
                          modifiers >>> 0,
                          owner_events != 0,
                          event_mask >>> 0,
                          pointer_mode | 0,
                          keyboard_mode | 0,
                          confine_to >>> 0,
                          cursor >>> 0);
      });

EM_JS(void,
      em_x11_js_ungrab_button,
      (unsigned int window, unsigned int button, unsigned int modifiers),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onUngrabButton(window >>> 0, button >>> 0, modifiers >>> 0);
      });

/* --- active pointer grabs (XGrabPointer / XUngrabPointer) ----------------
 *
 * Real X servers redirect every pointer event to the grab window's client
 * for the lifetime of an active grab (xserver/dix/events.c::ActivateGrab).
 * Twm's DeferExecution path depends on this: f.iconify / f.move / f.resize
 * etc. on a root menu install an active grab on Scr->Root, then expect
 * the next button press anywhere to be delivered to twm so it can apply
 * the deferred function to the clicked window. Without forwarding the
 * grab to the host's input router, that second click goes through normal
 * passive-grab + subscriber resolution, which routes it to whichever
 * client owns the press target (the clicked app, not twm) -- the deferred
 * function never fires and the user sees "the menu item did nothing".
 *
 * conn_id lets the host look up the calling Module so subsequent events
 * route to the same wasm process that issued the grab. owner_events
 * survives the bridge for completeness but our delivery model is
 * effectively owner_events=True regardless (we route to grab_window's
 * owner module; the C side then does its own context lookup on
 * Event.xany.window). */
EM_JS(void,
      em_x11_js_grab_pointer,
      (unsigned int conn_id, unsigned int window, int owner_events),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onGrabPointer(conn_id >>> 0, window >>> 0, owner_events != 0);
      });

EM_JS(void, em_x11_js_ungrab_pointer, (void), {
  var host = Module['emX11Host'];
  if (host)
    host.onUngrabPointer();
});

/* --- implicit grab (ButtonPress → ButtonRelease tracking) ----------------
 *
 * The C side owns the implicit-grab state machine (which window, how many
 * buttons).  These bridges tell the TS host which wasm module currently
 * holds the implicit grab so it can route subsequent Motion and
 * ButtonRelease events without duplicating the state machine. */

EM_JS(void, em_x11_js_implicit_grab_start, (unsigned int conn_id, unsigned int window), {
  var host = Module['emX11Host'];
  if (host)
    host.onImplicitGrabStart(conn_id >>> 0, window >>> 0);
});

EM_JS(void, em_x11_js_implicit_grab_end, (unsigned int conn_id), {
  var host = Module['emX11Host'];
  if (host)
    host.onImplicitGrabEnd(conn_id >>> 0);
});

/* --- deferred pointer-window repoll (XMapWindow / XUnmapWindow) ----------
 *
 * X protocol semantics: a state-changing map or unmap that alters the
 * topmost window under the sprite must synthesise crossing events
 * (xserver/dix/window.c::WindowsRestructured). Doing this synchronously
 * inside XMapWindow's call stack -- as the immediate-mode repoll
 * implementation did -- pushes the new crossings onto the *calling
 * client's* event queue while it is still mid-dispatch on the original
 * Enter/Leave that motivated the map. Twm's HandleEnterNotify maps
 * hilite_w on entering a frame; the synchronous repoll then queues a
 * Leave/Enter pair that twm processes inside its own next dooneevent
 * iteration with no yield to JS. Under the right cursor geometry this
 * self-amplifies into a tab-wedging tight loop (xeyes-frame-first 50%
 * repro).
 *
 * The fix is structural: defer the repoll across a JS event-loop tick.
 * XMapWindow / XUnmapWindow request a repoll, the bridge schedules it
 * via setTimeout(0), and the actual emit_crossing run only fires after
 * twm has finished dispatching its current event and yielded back to
 * the browser via emscripten_sleep. Per-conn coalescing collapses
 * burst maps (Tk widget realize) into one repoll.                          */
EM_JS(void, em_x11_js_schedule_repoll, (unsigned int conn_id), {
  var host = Module['emX11Host'];
  if (host)
    host.onScheduleRepoll(conn_id >>> 0);
});

EMSCRIPTEN_KEEPALIVE
void em_x11_repoll_pointer_window_now(void) {
  Display* dpy = em_x11_get_display();
  if (dpy)
    em_x11_repoll_pointer_window(dpy);
}

EMSCRIPTEN_KEEPALIVE
void em_x11_repoll_pointer_window_hint_now(unsigned long cur_hint) {
  Display* dpy = em_x11_get_display();
  if (dpy)
    em_x11_repoll_pointer_window_hint(dpy, cur_hint);
}

/* --- input focus (XSetInputFocus) ---------------------------------------- *
 *
 * The WM uses XSetInputFocus to hand the keyboard to a client whose own
 * window did not subscribe to ButtonPressMask (e.g. glxgears: only
 * Key/Expose/StructureNotify selected). Without bridging this to the host,
 * the press-driven focus tracker stays pointed at the WM frame and key
 * events never reach the app. None (0) / PointerRoot (1) clear the
 * override.                                                                 */
EM_JS(void, em_x11_js_set_input_focus, (unsigned int window), {
  var host = Module['emX11Host'];
  if (host && host.onSetInputFocus)
    host.onSetInputFocus(window >>> 0);
});

/* --- XIM (xim_bridge.c) --------------------------------------------------------- *
 *
 * Tk calls XSetICFocus / XSetICValues(XNSpotLocation) as it moves focus
 * between entries / texts and drags the caret around inside them. The
 * host translates these into "move the hidden <textarea> to this
 * window's caret position and grab DOM focus there", which is what the
 * OS IME actually anchors candidate windows on. See src/host/text-input.ts. */

EM_JS(void, em_x11_js_xim_set_focus, (unsigned int window), {
  var host = Module['emX11Host'];
  if (host && host.onXimSetFocus)
    host.onXimSetFocus(window >>> 0);
});

EM_JS(void, em_x11_js_xim_clear_focus, (void), {
  var host = Module['emX11Host'];
  if (host && host.onXimClearFocus)
    host.onXimClearFocus();
});

EM_JS(void, em_x11_js_xim_set_spot, (unsigned int window, int x, int y), {
  var host = Module['emX11Host'];
  if (host && host.onXimSetSpot)
    host.onXimSetSpot(window >>> 0, x | 0, y | 0);
});

/* --- exec self (twm F_RESTART) ------------------------------------------
 *
 * Emscripten's libc execvp is a stub. process.c (linked into the twm
 * wasm only) intercepts execvp/execv and routes to this bridge so the
 * host can kill+respawn the calling connection -- the wasm analogue of
 * a Linux fork-less exec. ProcessImpl registers a respawn handler when
 * its connection lands; that handler triggers a new Module instance
 * with the supplied argv. */
EM_JS(void, em_x11_js_exec_self, (int conn_id, int argv_ptrs, int argc), {
  var args = [];
  if (argv_ptrs != 0 && argc > 0) {
    var base = argv_ptrs >> 2;
    for (var i = 0; i < argc; i++) {
      var p = HEAPU32[base + i] >>> 0;
      args.push(p == 0 ? '' : UTF8ToString(p));
    }
  }
  var host = Module['emX11Host'];
  if (host && host.onExecSelf)
    host.onExecSelf(conn_id | 0, args);
});

/* --- atom ---------------------------------------------------------------- */

EM_JS(unsigned int, em_x11_js_intern_atom, (int namePtr, int onlyIfExists), {
  if (namePtr == 0)
    return 0;
  var name = UTF8ToString(namePtr);
  var host = Module['emX11Host'];
  if (!host)
    return 0;
  return host.internAtom(name, onlyIfExists != 0) >>> 0;
});

EM_JS(int, em_x11_js_get_atom_name, (unsigned int atom), {
  var host = Module['emX11Host'];
  if (!host)
    return 0;
  var name = host.getAtomName(atom >>> 0);
  if (name == null)
    return 0;
  return stringToNewUTF8(name);
});

/* --- clipboard ----------------------------------------------------------- */

/* Browser → Tk clipboard: async JSPI read.
 *
 * Called from serve_clipboard_from_browser when the proxy owns CLIPBOARD.
 * JSPI suspends the wasm call stack until navigator.clipboard.readText()
 * resolves. The caller C-side receives a malloc'd UTF-8 string and must
 * free() it. Returns NULL on permission denial or error. */
EM_ASYNC_JS(char*, em_x11_js_clipboard_read_async, (void), {
  try {
    var text = await navigator.clipboard.readText();
    if (!text) return 0;
    var len = lengthBytesUTF8(text) + 1;
    var ptr = _malloc(len);
    stringToUTF8(text, ptr, len);
    return ptr;
  } catch(e) {
    console.warn('[em-x11] clipboard read failed:', e);
    return 0;
  }
});

/* Browser → Tk clipboard: synchronous pre-stage read (legacy).
 *
 * The async path above is the canonical way. These sync helpers read from
 * a pre-staged cache (Module['emX11ClipboardBytes'] or globalThis fallback)
 * and remain available for non-JSPI consumers and the XConvertSelection
 * eviction safety-net. */
EM_JS(int, em_x11_js_clipboard_read_begin, (void), {
  var bytes = Module['emX11ClipboardBytes'] || globalThis.__emX11ClipboardBytes;
  if (!bytes)
    return -1;
  return bytes.length | 0;
});

EM_JS(int, em_x11_js_clipboard_read_fetch, (int dstPtr, int capacity), {
  var bytes = Module['emX11ClipboardBytes'] || globalThis.__emX11ClipboardBytes;
  if (!bytes)
    return 0;
  var n = Math.min(bytes.length, capacity) | 0;
  HEAPU8.set(bytes.subarray(0, n), dstPtr);
  Module['emX11ClipboardBytes'] = null;
  globalThis.__emX11ClipboardBytes = null;
  return n;
});

EM_JS(void, em_x11_js_clipboard_write_utf8, (int dataPtr, int len), {
  var b2 = HEAPU8.subarray(dataPtr, dataPtr + len);
  var copy = new Uint8Array(b2);
  /* Worker-mode hosts (pyodide-tk) install a remote hook on the
   * em-x11 bridge facade; the call gets posted to the main thread
   * which holds the DOM + clipboard permission. Without the hook
   * we fall through to the direct DOM path -- works in main-thread
   * Hosts (em-x11 demos, tcldide). */
  var host = Module['emX11Host'];
  if (host && typeof host.clipboardWriteRemote == 'function') {
    host.clipboardWriteRemote(copy);
    return;
  }
  if (typeof navigator == 'undefined' || !navigator.clipboard ||
      !navigator.clipboard.writeText) {
    console.warn('[em_x11] clipboard write: API unavailable');
    return;
  }
  /* Strip trailing NUL bytes that Motif's clipboard system includes in
   * its byte count, otherwise the NUL turns into a visible glyph (R or
   * space) when pasted back into the same app. */
  var end = copy.length;
  while (end > 0 && copy[end - 1] === 0)
    end--;
  var text = new TextDecoder('utf-8').decode(copy.subarray(0, end));
  navigator.clipboard.writeText(text).catch(
    function(e) { console.warn('[em_x11] clipboard write failed:', e); });
});

/* --- draw ---------------------------------------------------------------- */

EM_JS(void,
      em_x11_js_clear_area,
      (unsigned int id, int x, int y, int w, int h),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onClearArea(id, x, y, w, h);
      });

EM_JS(void,
      em_x11_js_fill_rect,
      (unsigned int id, int x, int y, int w, int h, unsigned int color),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onFillRect(id, x, y, w, h, color);
      });

EM_JS(void,
      em_x11_js_fill_stippled_rect,
      (unsigned int dst_id,
       int x,
       int y,
       unsigned int w,
       unsigned int h,
       unsigned int fg,
       unsigned int bg,
       unsigned int stipple_id,
       int ts_x_origin,
       int ts_y_origin,
       int opaque),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onFillStippledRect(dst_id,
                                  x,
                                  y,
                                  w,
                                  h,
                                  fg,
                                  bg,
                                  stipple_id,
                                  ts_x_origin,
                                  ts_y_origin,
                                  opaque);
      });

EM_JS(void,
      em_x11_js_draw_line,
      (unsigned int id,
       int x1,
       int y1,
       int x2,
       int y2,
       unsigned int color,
       int lineWidth),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onDrawLine(id, x1, y1, x2, y2, color, lineWidth);
      });

EM_JS(void,
      em_x11_js_draw_arc,
      (unsigned int id,
       int x,
       int y,
       int w,
       int h,
       int angle1,
       int angle2,
       unsigned int color,
       int lineWidth),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onDrawArc(id, x, y, w, h, angle1, angle2, color, lineWidth);
      });

EM_JS(void,
      em_x11_js_fill_arc,
      (unsigned int id,
       int x,
       int y,
       int w,
       int h,
       int angle1,
       int angle2,
       unsigned int color),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onFillArc(id, x, y, w, h, angle1, angle2, color);
      });

EM_JS(void,
      em_x11_js_fill_polygon,
      (unsigned int id,
       int ptsPtr,
       int count,
       int shape,
       int mode,
       unsigned int color),
      {
        var pts = [];
        if (count > 0 && ptsPtr != 0) {
          var base = ptsPtr >> 2;
          for (var i = 0; i < count; i++) {
            pts.push({x : HEAP32[base + i * 2], y : HEAP32[base + i * 2 + 1]});
          }
        }
        var host = Module['emX11Host'];
        if (host)
          host.onFillPolygon(id, pts, shape, mode, color);
      });

EM_JS(void,
      em_x11_js_draw_points,
      (unsigned int id, int ptsPtr, int count, int mode, unsigned int color),
      {
        var pts = [];
        if (count > 0 && ptsPtr != 0) {
          var base = ptsPtr >> 2;
          for (var i = 0; i < count; i++) {
            pts.push({x : HEAP32[base + i * 2], y : HEAP32[base + i * 2 + 1]});
          }
        }
        var host = Module['emX11Host'];
        if (host)
          host.onDrawPoints(id, pts, mode, color);
      });

/* --- font ---------------------------------------------------------------- */

EM_JS(void,
      em_x11_js_draw_string,
      (unsigned int id,
       int x,
       int y,
       int fontPtr,
       int textPtr,
       int length,
       unsigned int fg,
       unsigned int bg,
       int imageMode),
      {
        var font = fontPtr != 0 ? UTF8ToString(fontPtr) : '13px monospace';
        var text =
          length > 0 && textPtr != 0 ? UTF8ToString(textPtr, length) : '';
        var host = Module['emX11Host'];
        if (host)
          host.onDrawString(id, x, y, font, text, fg, bg, imageMode);
      });

/* Latin-1 variant for core X11 fonts (dixfonts.c dispatch_draw_string).
 * XDrawString text is font-encoded, i.e. single-byte ISO 8859-1 for
 * Western fonts. UTF8ToString would warn + garble on bytes 0x80-0xFF. */
EM_JS(void,
      em_x11_js_draw_string_latin1,
      (unsigned int id,
       int x,
       int y,
       int fontPtr,
       int textPtr,
       int length,
       unsigned int fg,
       unsigned int bg,
       int imageMode),
      {
        var font = fontPtr != 0 ? UTF8ToString(fontPtr) : '13px monospace';
        var text = '';
        if (length > 0 && textPtr != 0) {
          var u8 = HEAPU8;
          for (var i = 0; i < length; i++)
            text += String.fromCharCode(u8[textPtr + i]);
        }
        var host = Module['emX11Host'];
        if (host)
          host.onDrawString(id, x, y, font, text, fg, bg, imageMode);
      });

/* measure_font and measure_string are pure-JS measurements with no
 * shared state with the host bridge. Their scratchpads (one canvas
 * context, two LRU-ish maps) live under Module['emX11Caches']
 * so every bridge-owned bit of state stays in one namespace. */
EM_JS(
  void,
  em_x11_js_measure_font,
  (int fontPtr, int ascentPtr, int descentPtr, int maxWidthPtr, int widthsPtr),
  {
    var C = Module['emX11Caches'] || (Module['emX11Caches'] = {});
    var caches = C;
    if (caches && caches.measureCtx == undefined) {
      var c = typeof OffscreenCanvas != 'undefined' ? new OffscreenCanvas(1, 1)
              : typeof document != 'undefined'
                ? document.createElement('canvas')
                : null;
      caches.measureCtx =
        c ? c.getContext('2d', {willReadFrequently : true}) : null;
    }
    if (caches && !caches.fontCache)
      caches.fontCache = new Map();
    var ctx = caches ? caches.measureCtx : null;
    var fallbackWidth = 8, fallbackAscent = 10, fallbackDescent = 3;

    if (!ctx) {
      HEAP32[ascentPtr >> 2] = fallbackAscent;
      HEAP32[descentPtr >> 2] = fallbackDescent;
      HEAP32[maxWidthPtr >> 2] = fallbackWidth;
      for (var i = 0; i < 95; i++)
        HEAP32[(widthsPtr >> 2) + i] = fallbackWidth;
      return;
    }

    var css = UTF8ToString(fontPtr);
    var entry = caches.fontCache.get(css);
    if (entry) {
      HEAP32[ascentPtr >> 2] = entry.ascent;
      HEAP32[descentPtr >> 2] = entry.descent;
      HEAP32[maxWidthPtr >> 2] = entry.maxW;
      var bbase = widthsPtr >> 2;
      for (var k = 0; k < 95; k++)
        HEAP32[bbase + k] = entry.widths[k];
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
      if (w > maxW)
        maxW = w;
      widths[j] = w;
      HEAP32[base + j] = w;
    }
    HEAP32[maxWidthPtr >> 2] = maxW;
    caches.fontCache.set(
      css, {ascent : ascent, descent : descent, maxW : maxW, widths : widths});
  });

EM_JS(int, em_x11_js_measure_string, (int fontPtr, int textPtr, int length), {
  if (length <= 0 || textPtr == 0)
    return 0;
  var C = Module['emX11Caches'] || (Module['emX11Caches'] = {});
  var caches = C;
  if (caches && caches.measureCtx == undefined) {
    var c = typeof OffscreenCanvas != 'undefined' ? new OffscreenCanvas(1, 1)
            : typeof document != 'undefined' ? document.createElement('canvas')
                                             : null;
    caches.measureCtx =
      c ? c.getContext('2d', {willReadFrequently : true}) : null;
  }
  if (caches && !caches.textCache)
    caches.textCache = new Map();
  var ctx = caches ? caches.measureCtx : null;
  if (!ctx)
    return length * 8;
  var css = fontPtr != 0 ? UTF8ToString(fontPtr) : '13px monospace';
  var text = UTF8ToString(textPtr, length);
  var key = css + '' + text;
  var cache = caches.textCache;
  var hit = cache.get(key);
  if (hit != undefined)
    return hit;
  ctx.font = css;
  var w = Math.ceil(ctx.measureText(text).width);
  if (cache.size >= 8192)
    cache.clear();
  cache.set(key, w);
  return w;
});

EM_JS(int,
      em_x11_js_measure_string_latin1,
      (int fontPtr, int textPtr, int length),
      {
        if (length <= 0 || textPtr == 0)
          return 0;
        var C = Module['emX11Caches'] || (Module['emX11Caches'] = {});
        var caches = C;
        if (caches && caches.measureCtx == undefined) {
          var c =
            typeof OffscreenCanvas != 'undefined' ? new OffscreenCanvas(1, 1)
            : typeof document != 'undefined' ? document.createElement('canvas')
                                             : null;
          caches.measureCtx =
            c ? c.getContext('2d', {willReadFrequently : true}) : null;
        }
        if (caches && !caches.textCache)
          caches.textCache = new Map();
        var ctx = caches ? caches.measureCtx : null;
        if (!ctx)
          return length * 8;
        var css = fontPtr != 0 ? UTF8ToString(fontPtr) : '13px monospace';
        var text = '';
        if (textPtr != 0) {
          var u8 = HEAPU8;
          for (var i = 0; i < length; i++)
            text += String.fromCharCode(u8[textPtr + i]);
        }
        var key = css + '' + text;
        var cache = caches.textCache;
        var hit = cache.get(key);
        if (hit != undefined)
          return hit;
        ctx.font = css;
        var w = Math.ceil(ctx.measureText(text).width);
        if (cache.size >= 8192)
          cache.clear();
        cache.set(key, w);
        return w;
      });

EM_JS(int, em_x11_js_parse_color, (int namePtr, int rPtr, int gPtr, int bPtr), {
  /* Pure-JS color parse; reuses the shared measureCtx under em_x11._caches. */
  if (namePtr == 0)
    return 0;
  var name = UTF8ToString(namePtr);
  if (typeof CSS != 'undefined' && CSS.supports &&
      !CSS.supports('color', name)) {
    return 0;
  }
  var C = Module['emX11Caches'] || (Module['emX11Caches'] = {});
  var caches = C;
  if (caches && caches.measureCtx == undefined) {
    var c = typeof OffscreenCanvas != 'undefined' ? new OffscreenCanvas(1, 1)
            : typeof document != 'undefined' ? document.createElement('canvas')
                                             : null;
    caches.measureCtx =
      c ? c.getContext('2d', {willReadFrequently : true}) : null;
  }
  var ctx = caches ? caches.measureCtx : null;
  if (!ctx)
    return 0;
  ctx.fillStyle = '#010203';
  var sentinel = ctx.fillStyle;
  ctx.fillStyle = name;
  if (ctx.fillStyle == sentinel)
    return 0;
  ctx.clearRect(0, 0, 1, 1);
  ctx.fillRect(0, 0, 1, 1);
  var p = ctx.getImageData(0, 0, 1, 1).data;
  HEAPU16[rPtr >> 1] = (p[0] * 0x101) & 0xFFFF;
  HEAPU16[gPtr >> 1] = (p[1] * 0x101) & 0xFFFF;
  HEAPU16[bPtr >> 1] = (p[2] * 0x101) & 0xFFFF;
  return 1;
});

/* --- pixmap -------------------------------------------------------------- */

EM_JS(void,
      em_x11_js_pixmap_create,
      (unsigned int id, int width, int height, int depth),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onPixmapCreate(id, width, height, depth);
      });

EM_JS(void, em_x11_js_pixmap_destroy, (unsigned int id), {
  var host = Module['emX11Host'];
  if (host)
    host.onPixmapDestroy(id);
});

EM_JS(void,
      em_x11_js_shape_combine_mask,
      (unsigned int destId, unsigned int srcId, int xOff, int yOff, int op),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onShapeCombineMask(destId, srcId, xOff, yOff, op);
      });

EM_JS(void,
      em_x11_js_shape_select_input,
      (int connId, unsigned int window, unsigned int mask),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onShapeSelectInput(connId, window, mask);
      });

EM_JS(void,
      em_x11_js_copy_area,
      (unsigned int srcId,
       unsigned int dstId,
       int srcX,
       int srcY,
       int w,
       int h,
       int dstX,
       int dstY),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onCopyArea(
            srcId >>> 0, dstId >>> 0, srcX, srcY, w, h, dstX, dstY);
      });

EM_JS(void,
      em_x11_js_copy_plane,
      (unsigned int srcId,
       unsigned int dstId,
       int srcX,
       int srcY,
       int w,
       int h,
       int dstX,
       int dstY,
       unsigned int plane,
       unsigned int fg,
       unsigned int bg,
       int applyBg),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onCopyPlane(srcId >>> 0,
                           dstId >>> 0,
                           srcX,
                           srcY,
                           w,
                           h,
                           dstX,
                           dstY,
                           plane >>> 0,
                           fg >>> 0,
                           bg >>> 0,
                           applyBg != 0);
      });

EM_JS(void,
      em_x11_js_put_image,
      (unsigned int dstId,
       int dstX,
       int dstY,
       int w,
       int h,
       int format,
       int depth,
       int bytesPerLine,
       int dataPtr,
       int dataLen,
       unsigned int fg,
       unsigned int bg),
      {
        var data = dataLen > 0 && dataPtr != 0
                     ? HEAPU8.slice(dataPtr, dataPtr + dataLen)
                     : new Uint8Array(0);
        var host = Module['emX11Host'];
        if (host)
          host.onPutImage(dstId >>> 0,
                          dstX,
                          dstY,
                          w,
                          h,
                          format,
                          depth,
                          bytesPerLine,
                          data,
                          fg >>> 0,
                          bg >>> 0);
      });

EM_JS(void,
      em_x11_js_get_image,
      (unsigned int drawable,
       int x,
       int y,
       int w,
       int h,
       int dstPtr,
       int dstCapacity),
      {
        var host = Module['emX11Host'];
        var data =
          host ? host.readDrawablePixels(drawable >>> 0, x, y, w, h) : null;
        if (!data) {
          /* Fallback: fill with default gray (X11 bg #d9d9d9).
           * BGRA byte order so GetPixel(R,G,B) picks it up correctly. */
          var n4 = (dstCapacity | 0) & ~3;
          for (var i = 0; i < n4; i += 4) {
            HEAPU8[dstPtr + i] = 0xd9;     /* B */
            HEAPU8[dstPtr + i + 1] = 0xd9; /* G */
            HEAPU8[dstPtr + i + 2] = 0xd9; /* R */
            HEAPU8[dstPtr + i + 3] =
              0xff; /* padding (protocol: 0 for planes outside mask) */
          }
          return;
        }
        var n = Math.min(data.length, dstCapacity | 0);
        HEAPU8.set(data.subarray ? data.subarray(0, n) : data.slice(0, n),
                   dstPtr);
      });

/* --- property ------------------------------------------------------------ */

EM_JS(int,
      em_x11_js_change_property,
      (unsigned int w,
       unsigned int atom,
       unsigned int type,
       int format,
       int mode,
       int dataPtr,
       int nelements),
      {
        var unit = format == 8 ? 1 : format == 16 ? 2 : format == 32 ? 4 : 0;
        if (unit == 0)
          return 0;
        var bytes = unit * (nelements | 0);
        var data = bytes > 0 ? HEAPU8.slice(dataPtr, dataPtr + bytes)
                             : new Uint8Array(0);
        var host = Module['emX11Host'];
        if (!host)
          return -1;
        var ok = host.changeProperty(
          w >>> 0, atom >>> 0, type >>> 0, format | 0, mode | 0, data);
        return ok ? 1 : 0;
      });

EM_JS(void,
      em_x11_js_get_property_meta,
      (unsigned int w,
       unsigned int atom,
       unsigned int reqType,
       int longOffset,
       int longLength,
       int metaPtr),
      {
        /* Layout: 0 FOUND, 1 TYPE, 2 FORMAT, 3 NITEMS, 4 BYTES_AFTER,
         *         5 DATA_LEN, 6 PRESENT, 7 reserved. Total 8 ints. */
        var base = metaPtr >> 2;
        for (var i = 0; i < 8; i++)
          HEAP32[base + i] = 0;
        var host = Module['emX11Host'];
        if (!host)
          return;
        var r2 = host.peekProperty(w >>> 0,
                                   atom >>> 0,
                                   reqType >>> 0,
                                   longOffset | 0,
                                   longLength | 0,
                                   false);
        if (r2 == null)
          return;
        HEAP32[base + 6] = 1;
        HEAP32[base + 0] = r2.found ? 1 : 0;
        HEAP32[base + 1] = r2.type | 0;
        HEAP32[base + 2] = r2.format | 0;
        HEAP32[base + 3] = r2.nitems | 0;
        HEAP32[base + 4] = r2.bytesAfter | 0;
        HEAP32[base + 5] = r2.data.length | 0;
        /* Stash the data for an immediate-following get_property_data call
         * (cheap two-hop avoids re-fetch). Lives under em_x11._caches with
         * the rest of the bridge-owned scratch state. Key by (w, atom, reqType,
         * longOffset, longLength) so a re-entrant peekProperty (e.g. via
         * internAtom or another bridge that runs between the meta and the data
         * call) can't smuggle a different property's bytes into our reply. */
        var caches2 = Module['emX11Caches'] || (Module['emX11Caches'] = {});
        caches2.propStash = {
          w : w >>> 0,
          atom : atom >>> 0,
          reqType : reqType >>> 0,
          longOffset : longOffset | 0,
          longLength : longLength | 0,
          data : r2.data,
        };
      });

EM_JS(void,
      em_x11_js_get_property_data,
      (unsigned int w,
       unsigned int atom,
       unsigned int reqType,
       int longOffset,
       int longLength,
       int deleteFlag,
       int dstPtr,
       int capacity),
      {
        var C0 = Module['emX11Caches'] || (Module['emX11Caches'] = {});
        var caches0 = C0;
        var stash = caches0 ? caches0.propStash : null;
        if (caches0)
          caches0.propStash = null;
        var data =
          (stash && stash.w == (w >>> 0) && stash.atom == (atom >>> 0) &&
           stash.reqType == (reqType >>> 0) &&
           stash.longOffset == (longOffset | 0) &&
           stash.longLength == (longLength | 0))
            ? stash.data
            : null;
        if (data && data.length > 0) {
          /* Use cached from preceding PeekMeta. */
          var n = Math.min(data.length, capacity | 0);
          HEAPU8.set(data.subarray ? data.subarray(0, n) : data.slice(0, n),
                     dstPtr);
          if (deleteFlag != 0) {
            var host = Module['emX11Host'];
            if (host)
              host.deleteProperty(w >>> 0, atom >>> 0);
          }
          return;
        }
        var host2 = Module['emX11Host'];
        if (!host2)
          return;
        var r3 = host2.peekProperty(w >>> 0,
                                    atom >>> 0,
                                    reqType >>> 0,
                                    longOffset | 0,
                                    longLength | 0,
                                    deleteFlag != 0);
        if (!r3 || !r3.found || r3.data.length == 0)
          return;
        var n3 = Math.min(r3.data.length, capacity | 0);
        HEAPU8.set(r3.data.subarray(0, n3), dstPtr);
      });

EM_JS(void, em_x11_js_delete_property, (unsigned int w, unsigned int atom), {
  var host = Module['emX11Host'];
  if (host)
    host.deleteProperty(w >>> 0, atom >>> 0);
});

EM_JS(int, em_x11_js_list_properties_count, (unsigned int w), {
  var host = Module['emX11Host'];
  if (!host)
    return 0;
  return host.listProperties(w >>> 0).length;
});

EM_JS(int,
      em_x11_js_list_properties_fetch,
      (unsigned int w, int dstPtr, int capacity),
      {
        var host = Module['emX11Host'];
        if (!host)
          return 0;
        var atoms2 = host.listProperties(w >>> 0);
        var n2 = Math.min(atoms2.length, capacity | 0);
        var base2 = dstPtr >> 2;
        for (var j = 0; j < n2; j++)
          HEAPU32[base2 + j] = atoms2[j] >>> 0;
        return n2;
      });

/* --- window -------------------------------------------------------------- */

EM_JS(void,
      em_x11_js_window_create,
      (int connId,
       unsigned int id,
       unsigned int parent,
       int x,
       int y,
       int w,
       int h,
       int borderWidth,
       unsigned int borderPixel,
       int bgType,
       unsigned int bgValue),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onWindowCreate(connId,
                              id,
                              parent,
                              x,
                              y,
                              w,
                              h,
                              borderWidth,
                              borderPixel,
                              bgType,
                              bgValue);
      });

EM_JS(void,
      em_x11_js_window_set_border,
      (unsigned int id, int borderWidth, unsigned int borderPixel),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onWindowSetBorder(id, borderWidth, borderPixel);
      });

EM_JS(void,
      em_x11_js_window_set_bg,
      (unsigned int id, int bgType, unsigned int bgValue),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onWindowSetBg(id, bgType, bgValue);
      });

EM_JS(void,
      em_x11_js_window_configure,
      (int connId, unsigned int id, int x, int y, int w, int h),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onWindowConfigure(connId, id, x, y, w, h);
      });

EM_JS(void, em_x11_js_window_set_bit_gravity, (unsigned int id, int gravity), {
  var host = Module['emX11Host'];
  if (host)
    host.onWindowSetBitGravity(id, gravity);
});

EM_JS(void, em_x11_js_window_map, (int connId, unsigned int id), {
  var host = Module['emX11Host'];
  if (host)
    host.onWindowMap(connId, id);
});

EM_JS(void, em_x11_js_window_unmap, (int connId, unsigned int id), {
  var host = Module['emX11Host'];
  if (host)
    host.onWindowUnmap(connId, id);
});

EM_JS(void, em_x11_js_window_destroy, (unsigned int id), {
  var host = Module['emX11Host'];
  if (host)
    host.onWindowDestroy(id);
});

EM_JS(void, em_x11_js_window_raise, (unsigned int id), {
  var host = Module['emX11Host'];
  if (host)
    host.onWindowRaise(id);
});

EM_JS(void, em_x11_js_window_lower, (unsigned int id), {
  var host = Module['emX11Host'];
  if (host)
    host.onWindowLower(id);
});

EM_JS(void,
      em_x11_js_select_input,
      (int connId, unsigned int id, unsigned int mask),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onSelectInput(connId, id, mask >>> 0);
      });

EM_JS(void, em_x11_js_set_override_redirect, (unsigned int id, int flag), {
  var host = Module['emX11Host'];
  if (host)
    host.onSetOverrideRedirect(id, flag != 0);
});

EM_JS(void,
      em_x11_js_reparent_window,
      (unsigned int id, unsigned int parent, int x, int y),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onReparentWindow(id, parent, x, y);
      });

EM_JS(void,
      em_x11_js_window_set_bg_pixmap,
      (unsigned int id, unsigned int pmId),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onWindowSetBgPixmap(id, pmId);
      });

EM_JS(void,
      em_x11_js_window_set_cursor,
      (unsigned int id, unsigned int cursor),
      {
        var host = Module['emX11Host'];
        if (host)
          host.onWindowSetCursor(id, cursor);
      });

EM_JS(void, em_x11_js_set_grab_cursor, (unsigned int cursor), {
  var host = Module['emX11Host'];
  if (host)
    host.onSetGrabCursor(cursor);
});

EM_JS(void,
      em_x11_js_window_shape,
      (unsigned int id, int rectsPtr, int count),
      {
        var rects = [];
        if (count > 0 && rectsPtr != 0) {
          var base = rectsPtr >> 2;
          for (var i = 0; i < count; i++) {
            rects.push({
              x : HEAP32[base + i * 4 + 0],
              y : HEAP32[base + i * 4 + 1],
              w : HEAP32[base + i * 4 + 2],
              h : HEAP32[base + i * 4 + 3],
            });
          }
        }
        var host = Module['emX11Host'];
        if (host)
          host.onWindowShape(id, rects);
      });

/* --- GLX (libGL via emscripten LEGACY_GL_EMULATION on per-context
 *      OffscreenCanvas; SwapBuffers blits into the X window's backing) -- */

/* Allocate an OffscreenCanvas + targetId for a new GLXContext, register
 * the canvas under the calling wasm's Module.specialHTMLTargets so
 * emscripten_webgl_create_context (called from glx.c) resolves it.
 * Writes the targetId into outTargetIdPtr (UTF-8, capped at outTargetIdLen
 * including NUL); returns the GlxManager-side context id (>=1) or 0 on
 * failure. */
EM_JS(int,
      em_x11_js_glx_create_context,
      (int width, int height, int outTargetIdPtr, int outTargetIdLen),
      {
        var host = Module['emX11Host'];
        if (!host || !host.glx)
          return 0;
        var info = host.glx.createContext(width | 0, height | 0);
        if (!info)
          return 0;
        /* Module.specialHTMLTargets is exported via EXPORTED_RUNTIME_METHODS
         * and aliases the same Array instance emscripten's findEventTarget
         * reads through. Mutate it in place; replacing it would break
         * the alias and findEventTarget would fall through to
         * document.querySelector('!em-x11-glx-N'), which is invalid. */
        Module.specialHTMLTargets[info.targetId] = info.canvas;
        stringToUTF8(info.targetId, outTargetIdPtr, outTargetIdLen);
        return info.id | 0;
      });

/* Called once from glx.c right after the FIRST
 * emscripten_webgl_make_context_current succeeds. emscripten's
 * LEGACY_GL_EMULATION assumes Browser.createContext drives setup -- that path
 * sets Browser.useWebGL=true and runs Browser.moduleContextCreatedCallbacks
 * (containing GLImmediate.init, which queries GLctx.getParameter for MAX_*).
 * Going through emscripten_webgl_create_context bypasses Browser, so we
 * replicate that init manually -- but only AFTER make_context_current binds
 * GLctx, otherwise GLImmediate.init reads `undefined.getParameter`. */
EM_JS(void, em_x11_js_glx_legacy_init_once, (void), {
  if (typeof GLImmediate == 'undefined' || GLImmediate.initted)
    return;
  if (typeof Browser == 'undefined')
    return;
  Browser.useWebGL = true;
  if (Browser.moduleContextCreatedCallbacks) {
    Browser.moduleContextCreatedCallbacks.forEach(function(cb) { cb(); });
  }
});

EM_JS(void, em_x11_js_glx_destroy_context, (int id), {
  var host = Module['emX11Host'];
  if (!host || !host.glx)
    return;
  var targetId = host.glx.targetIdOf(id | 0);
  if (targetId && Module.specialHTMLTargets) {
    delete Module.specialHTMLTargets[targetId];
  }
  host.glx.destroyContext(id | 0);
});

EM_JS(void, em_x11_js_glx_swap_buffers, (int id, unsigned int drawable), {
  var host = Module['emX11Host'];
  if (!host || !host.glx)
    return;
  host.glx.swapBuffers(id | 0, drawable >>> 0);
});

EM_JS(void, em_x11_js_glx_resize, (int id, int width, int height), {
  var host = Module['emX11Host'];
  if (!host || !host.glx)
    return;
  host.glx.resize(id | 0, width | 0, height | 0);
});

/* clang-format on */
