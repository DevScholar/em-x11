/*
 * Input dispatch: hit-testing, pointer-window tracking, and the C entry
 * points the JS host calls (emx11_push_button_event / motion / key /
 * expose / map_request / reparent_notify). Queue plumbing lives in
 * event_queue.c; XSendEvent + focus in event_send.c; keysyms in
 * event_keysym.c.
 */

#include "emx11_internal.h"
#include "emx11_meta_layout.h"

#include <X11/extensions/XInput2.h>
#include <emscripten.h>
#include <string.h>

/* -- Window hit testing -------------------------------------------------- */

/* Walk the parent chain to compute the window's origin in root-relative
 * coordinates, plus its depth in the tree (root = 0, shell = 1, ...).
 * Cycles in `parent` links (should never happen) are defended against
 * with a hard iteration limit. */
static void window_abs_origin(Display *dpy, EmxWindow *w,
                              int *ax_out, int *ay_out, int *depth_out) {
    int ax = 0, ay = 0, depth = 0;
    EmxWindow *cur = w;
    for (int guard = 0; cur && guard < EMX11_MAX_WINDOWS; guard++, depth++) {
        ax += cur->x;
        ay += cur->y;
        if (cur->parent == None || cur->parent == cur->id) break;
        EmxWindow *parent = emx11_window_find(dpy, cur->parent);
        if (!parent) {
            /* Parent is owned by a different connection (e.g. xcalc
             * shell reparented under a twm-owned frame). Our local table
             * doesn't have it, so the cumulative origin needs to come
             * from Host's authoritative tree. Without this fallback the
             * walk terminates with cur->parent's offset missing, every
             * input event lands at canvas-absolute - frame.position
             * inside the conn's coordinate system, and Xaw widgets
             * highlight the wrong button on hover. */
            int buf[EMX11_ABS_ORIGIN_SIZE] = {0};
            emx11_js_get_window_abs_origin(cur->parent, buf);
            if (buf[EMX11_ABS_ORIGIN_PRESENT]) {
                ax += buf[EMX11_ABS_ORIGIN_AX];
                ay += buf[EMX11_ABS_ORIGIN_AY];
            }
            break;
        }
        cur = parent;
    }
    if (ax_out)    *ax_out    = ax;
    if (ay_out)    *ay_out    = ay;
    if (depth_out) *depth_out = depth;
}

/* Given a root-relative point, find the deepest mapped window that
 * (a) contains the point and (b) has at least one of `need_mask` bits
 * in its event_mask. If the deepest containing window doesn't select
 * for the event, propagate up the parent chain (INCLUDING root) to the
 * first ancestor that does -- matching xserver/dix/events.c's
 * DeliverEventsToWindow semantics. Returns NULL only when literally
 * nothing in the table or in the ancestor chain wants the event.
 *
 * Root IS a valid hit and a valid delivery target. Twm selects on root
 * for the bare-button menus and for SubstructureRedirect; any
 * "single-client world" pruning here breaks the WM. xserver's
 * miSpriteTrace starts from root and hands deliveries to root too.
 *
 * `lx`/`ly` are filled with the winning window's local coordinates. */
static EmxWindow *hit_test(Display *dpy, int rx, int ry, long need_mask,
                           int *lx_out, int *ly_out) {
    EmxWindow *best = NULL;
    int best_depth = -1;
    int best_ax = 0, best_ay = 0;

    for (int i = 0; i < EMX11_MAX_WINDOWS; i++) {
        EmxWindow *w = &dpy->windows[i];
        if (!w->in_use || !w->mapped) continue;

        int ax, ay, depth;
        window_abs_origin(dpy, w, &ax, &ay, &depth);
        if (rx < ax || ry < ay ||
            rx >= ax + (int)w->width || ry >= ay + (int)w->height) {
            continue;
        }
        /* depth tie-break by stack-ish order doesn't apply here -- we keep
         * deepest-by-tree-depth which matches xorg's "child wins over
         * parent at same point" semantics. Same depth means siblings,
         * which our tree doesn't disambiguate; first-found wins. */
        if (depth > best_depth) {
            best = w;
            best_depth = depth;
            best_ax = ax;
            best_ay = ay;
        }
    }
    if (!best) return NULL;

    /* Propagate up to AND including root. We keep updating (ax, ay) so
     * local coords stay correct wherever the chain terminates. */
    EmxWindow *cur = best;
    int ax = best_ax, ay = best_ay;
    while (cur) {
        if (cur->event_mask & need_mask) {
            if (lx_out) *lx_out = rx - ax;
            if (ly_out) *ly_out = ry - ay;
            return cur;
        }
        if (cur->parent == None || cur->parent == cur->id) break;
        EmxWindow *p = emx11_window_find(dpy, cur->parent);
        if (!p) break;                          /* parent in another conn's table */
        ax -= cur->x;                           /* un-offset into parent frame */
        ay -= cur->y;
        cur = p;
    }
    /* Nothing along the chain selected for this event. Fall back to the
     * deepest hit anyway so the event isn't lost -- matches the shape
     * of the old "always dispatch somewhere" behavior while we build out
     * the rest of the dispatch logic. */
    if (lx_out) *lx_out = rx - best_ax;
    if (ly_out) *ly_out = ry - best_ay;
    return best;
}

/* -- JS -> C event bridges ------------------------------------------------- */

/* Note on the `window` argument: the JS bridge passes whatever window it
 * thinks the pointer is over, but that hint is unreliable for nested
 * widgets (the compositor doesn't know parent chains). We ignore it for
 * button/motion and re-run the hit test here with authoritative parent
 * data from the EmxWindow table. */

/* Implicit pointer grab state (x11protocol.txt §523).
 * A ButtonPress initiates a grab: subsequent ButtonRelease and MotionNotify
 * events are routed to the grab window regardless of current pointer position.
 * The grab is released when the last simultaneously-held button is released. */
static Window       grab_window         = None;
static unsigned int grab_button_count   = 0;

/* Active pointer grab (XGrabPointer). Tracked separately from the implicit
 * grab so that the mask-gate bypass in emx11_push_motion_event works even
 * when the implicit grab_window is legitimately None (XGrabPointer clears the
 * stale implicit grab so ButtonRelease routes to the window under the pointer
 * instead of the original press window -- needed for MenuButton/ComboBox
 * popup entries). Without this, twm f.move/f.resize loop XMaskEvent never
 * sees MotionNotify: none of twm's frame/title_w/client windows select for
 * PointerMotionMask, so every motion event hits the mask gate and is dropped,
 * and the drag deadlocks. */
static bool active_grab = false;

/* Monotonic millisecond timestamp for xbutton/xmotion/xkey/xcrossing `time`
 * fields. Some WMs (twm's ConstrainedMove in particular: menus.c:1500) compare
 * `event.time - last_click_time` against a timeout to detect rapid successive
 * clicks. Leaving time=0 on every event makes the delta always 0, which trips
 * the < 400ms gate on *every* press -- twm then enters ConstMove and freezes
 * one axis of the drag. emscripten_get_now is a double of ms since epoch, so
 * unsigned-casting it is fine for the 32-bit Time field (wraps every ~49 days
 * like a real X server does). */
static Time event_now(void) {
    return (Time)(unsigned long)emscripten_get_now();
}

/* Window the pointer is currently over, as of the last motion or press we
 * observed. Real X servers synthesize EnterNotify / LeaveNotify whenever the
 * pointer crosses a window boundary (x11protocol §Window crossing), grab or
 * no grab. The browser DOM only delivers raw mousemove, so we track the
 * pointer window here and emit crossings from update_pointer_window. Without
 * this, Tk's <Enter>/<Leave> bindings never fire on simple hover: e.g.
 * tk::ButtonUp's `$Priv(window) eq $w` check stays false for every widget
 * except ones the user has grabbed-and-dragged through, which looks like
 * "button press/release works visually but -command never fires". */
static Window       last_pointer_window = None;

/* Reset the implicit pointer grab. Called from XGrabPointer (which
 * establishes an active server grab) so the stale implicit grab from the
 * ButtonPress that triggered the popup doesn't pin future ButtonReleases to
 * the original press window. Without this, MenuButton and ComboBox popup
 * entries never see ButtonRelease: the C-side grab_window stays on the
 * button that opened the menu, and every subsequent button-up gets delivered
 * there instead of to the menu entry under the pointer. */
void emx11_reset_implicit_grab(void) {
    grab_window       = None;
    grab_button_count = 0;
}

/* Window-relationship classifier for crossing detail computation.
 * Returns 1 if `descendant` is in the parent chain of `ancestor` (so
 * `descendant` is inferior, `ancestor` is ancestor); 0 otherwise.
 * Walks until parent==None / self / not-in-this-conn. */
static int win_is_inferior_of(Display *dpy, Window descendant, Window ancestor) {
    if (descendant == None || ancestor == None || descendant == ancestor) return 0;
    EmxWindow *cur = emx11_window_find(dpy, descendant);
    while (cur && cur->parent != None && cur->parent != cur->id) {
        if (cur->parent == ancestor) return 1;
        cur = emx11_window_find(dpy, cur->parent);
    }
    return 0;
}

/* Compute X11 crossing detail (NotifyAncestor / NotifyInferior /
 * NotifyNonlinear) per x11protocol §10.4 for a Leave(`from`)+Enter(`to`)
 * pair. Real X servers branch on this; twm relies on it to elide
 * Map/Unmap-induced crossings between a frame and its hilite_w child
 * (events.c::HandleLeaveNotify gates XUnmapWindow on
 * `detail != NotifyInferior`). Without proper detail, twm interprets
 * every "we mapped a child window" as "the user left the frame",
 * unmaps the child, repoll fires the inverse pair, twm re-maps,
 * infinite C loop wedges the tab.
 *
 * Returned values per X11:
 *   pointer A→B with B inferior of A: Leave(A,Inferior), Enter(B,Ancestor)
 *   pointer A→B with A inferior of B: Leave(A,Ancestor), Enter(B,Inferior)
 *   otherwise (siblings / unrelated): both Nonlinear. (Virtuals on
 *   intermediate windows are not synthesised here -- they require
 *   per-window-on-the-path emit_crossing calls; see follow-up.) */
static int crossing_detail(Display *dpy, int type, Window from, Window to) {
    if (win_is_inferior_of(dpy, to, from)) {
        return (type == LeaveNotify) ? NotifyInferior : NotifyAncestor;
    }
    if (win_is_inferior_of(dpy, from, to)) {
        return (type == LeaveNotify) ? NotifyAncestor : NotifyInferior;
    }
    return NotifyNonlinear;
}

/* Push an EnterNotify / LeaveNotify on `w`, iff the window selects for that
 * mask. Coords are root-relative; we derive window-local ones from the
 * window's absolute origin. `peer` is the other end of the crossing
 * (the window the pointer is leaving from on EnterNotify, the window
 * it's entering on LeaveNotify) -- used to compute xcrossing.detail. */
static void emit_crossing(Display *dpy, int type, Window w, Window peer,
                          int x_root, int y_root, unsigned int state) {
    EmxWindow *win = emx11_window_find(dpy, w);
    if (!win) return;
    long mask = (type == EnterNotify) ? EnterWindowMask : LeaveWindowMask;
    if (!(win->event_mask & mask)) return;

    /* crossing_detail wants (from, to) as the pointer's actual travel
     * direction. On LeaveNotify the pointer moved away from `w` toward
     * `peer`; on EnterNotify it moved from `peer` onto `w`. update_pointer_window
     * passes (cur, prev) for Leave and (cur, prev) for Enter (i.e. the
     * caller's `peer` plays opposite roles in the two callsites), so
     * disentangle here based on which crossing this is. */
    bool is_leave = (type == LeaveNotify);
    Window from   = is_leave ? w    : peer;
    Window to     = is_leave ? peer : w;
    int detail    = crossing_detail(dpy, type, from, to);

    int ax = 0, ay = 0, depth;
    window_abs_origin(dpy, win, &ax, &ay, &depth);

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type                  = type;
    ev.xcrossing.display     = dpy;
    ev.xcrossing.window      = w;
    ev.xcrossing.root        = dpy->screens[0].root;
    ev.xcrossing.x           = x_root - ax;
    ev.xcrossing.y           = y_root - ay;
    ev.xcrossing.x_root      = x_root;
    ev.xcrossing.y_root      = y_root;
    ev.xcrossing.mode        = NotifyNormal;
    ev.xcrossing.detail      = detail;
    ev.xcrossing.same_screen = True;
    ev.xcrossing.focus       = (w == dpy->focus_window);
    ev.xcrossing.state       = state;
    ev.xcrossing.time        = event_now();
    emx11_event_queue_push(dpy, &ev);
}

/* Update last_pointer_window, emitting Leave on the outgoing window and
 * Enter on the incoming one. Called on every motion, on ButtonPress
 * (so the "first interaction is a click, no prior mousemove" path still
 * delivers the Enter that Tk's button bindings depend on), and from
 * emx11_repoll_pointer_window after a map/unmap that may have changed
 * the topmost window under a static pointer (twm root menu pop-up). */
static void update_pointer_window(Display *dpy, Window cur,
                                  int x_root, int y_root, unsigned int state) {
    if (cur == last_pointer_window) return;
    Window prev = last_pointer_window;
    if (prev != None) {
        emit_crossing(dpy, LeaveNotify, prev, cur,
                      x_root, y_root, state);
    }
    if (cur != None) {
        emit_crossing(dpy, EnterNotify, cur, prev, x_root, y_root, state);
    }
    last_pointer_window = cur;
}

/* Re-evaluate which window is under the cursor and fire any Enter/Leave
 * pair that the change implies. Real X servers synthesize crossings
 * whenever a map/unmap/raise/lower changes the topmost window at the
 * sprite position (xserver/dix/events.c::CheckMotion via WindowsRestructured).
 * Without this hook, twm's root menu pops up *under* the cursor: hit_test
 * now resolves to the menu, but last_pointer_window is still root, so the
 * next motion event fires Leave/Enter only after the user crosses the
 * boundary -- twm's UpdateMenu (menus.c:512) gates every hover update on
 * `ActiveMenu->entered`, which is set on EnterNotify, so until that fires
 * no menu item highlights and release lands with ActiveItem=NULL.
 *
 * Called from window.c after XMapWindow / XUnmapWindow on this display.
 * `state` defaults to 0 since we have no fresh modifier sample to attach;
 * Tk and twm don't read xcrossing.state for menu logic. */
void emx11_repoll_pointer_window(Display *dpy) {
    if (!dpy) return;
    int px = 0, py = 0;
    emx11_js_pointer_xy(&px, &py);
    int lx = 0, ly = 0;
    EmxWindow *cur = hit_test(dpy, px, py, 0, &lx, &ly);
    Window cur_id = cur ? cur->id : None;
    update_pointer_window(dpy, cur_id, px, py, 0);
}

/* Variant called from the host's deferred-repoll path with a cur_id the
 * host already resolved via its (stacking-aware) findWindowAt. The C-side
 * hit_test in this file walks dpy->windows[] by tree-depth + first-found,
 * which gives wrong answers for sibling top-levels whose stack order was
 * changed (twm root menu's shadow XRaiseWindow'd before the menu itself
 * is mapped: shadow created first → first-found returns shadow, but host's
 * stackOrder correctly puts menu on top). Trusting the host avoids that
 * divergence. cur_hint may name a window owned by another connection or
 * may be 0 (None); both are valid -- emit_crossing's mask check handles
 * the foreign / unowned cases by no-op'ing. */
void emx11_repoll_pointer_window_hint(Display *dpy, Window cur_hint) {
    if (!dpy) return;
    int px = 0, py = 0;
    emx11_js_pointer_xy(&px, &py);
    update_pointer_window(dpy, cur_hint, px, py, 0);
}

EMSCRIPTEN_KEEPALIVE
void emx11_push_button_event(int type, Window window, int x, int y,
                             int x_root, int y_root, unsigned int button,
                             unsigned int state) {
    Display *dpy = emx11_get_display();
    int lx = 0, ly = 0;
    EmxWindow *target;

    if (type == ButtonRelease && grab_window != None) {
        /* Implicit grab: deliver ButtonRelease to the grab window even if
         * the pointer has moved elsewhere. Compute local coords from the
         * grab window's current absolute position. */
        target = emx11_window_find(dpy, grab_window);
        if (target) {
            int ax = 0, ay = 0, depth;
            window_abs_origin(dpy, target, &ax, &ay, &depth);
            lx = x_root - ax;
            ly = y_root - ay;
        }
        if (grab_button_count > 0) grab_button_count--;
        if (grab_button_count == 0) grab_window = None;
    } else {
        /* Trust the Host's z-order-aware findWindowAt result. The Host
         * already did the ancestor-chain subscriber propagation
         * (findSubscriberFor / grabs.lookup) and computed window-local
         * coords in (x, y). Fall back to C-side hit_test only when the
         * hint is invalid -- the C-side test is depth-based and can't
         * distinguish a popup menu from a deeper widget underneath. */
        target = emx11_window_find(dpy, window);
        if (target) {
            lx = x;
            ly = y;
        } else {
            long mask = (type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask;
            target = hit_test(dpy, x_root, y_root, mask, &lx, &ly);
        }
        if (target && type == ButtonPress) {
            /* Ensure Tk has seen an Enter on this widget before its
             * <ButtonPress-1> binding runs. Covers the case where a click
             * is the first pointer interaction and no mousemove has yet
             * advanced last_pointer_window to this widget. */
            update_pointer_window(dpy, target->id, x_root, y_root, state);
            if (grab_button_count == 0) grab_window = target->id;
            grab_button_count++;
        }
    }

    /* Diagnostic trace: dump the C-side resolution so we can see what
     * each wasm process receives. Toggled via
     * `globalThis.emX11._debug.traceCBtn` (set from DevTools). Gated
     * inside EM_ASM so it's effectively free when disabled. */
    EM_ASM({
        var d = globalThis.emX11 && globalThis.emX11._debug;
        if (d && d.traceCBtn) {
            console.log('[c-btn] conn=' + $0 + ' type=' + $1 +
                        ' hint=' + ($2 >>> 0) + ' rx=' + $3 + ' ry=' + $4 +
                        ' button=' + $5 + ' state=0x' + ($6 >>> 0).toString(16) +
                        ' -> target=' + ($7 >>> 0) + ' lx=' + $8 + ' ly=' + $9);
        }
    }, dpy->conn_id, type, window, x_root, y_root, button, state,
       target ? target->id : 0, lx, ly);

    if (!target) return;

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xbutton.type        = type;
    ev.xbutton.display     = dpy;
    ev.xbutton.window      = target->id;
    ev.xbutton.x           = lx;
    ev.xbutton.y           = ly;
    ev.xbutton.x_root      = x_root;
    ev.xbutton.y_root      = y_root;
    ev.xbutton.button      = button;
    ev.xbutton.state       = state;
    ev.xbutton.same_screen = True;
    ev.xbutton.time        = event_now();
    emx11_event_queue_push(dpy, &ev);
}

EMSCRIPTEN_KEEPALIVE
void emx11_push_motion_event(Window window, int x, int y,
                             int x_root, int y_root,
                             unsigned int state) {
    Display *dpy = emx11_get_display();

    /* Pointer-window tracking: trust the Host's z-order-aware findWindowAt
     * result passed as `window`. The C-side hit_test is depth-based and
     * can't distinguish a popup menu (shallow top-level) from a deeper
     * widget underneath it. Fall back to C-side hit_test only when the
     * Host hint is None or names a window we don't know. */
    Window cur_pw = window;
    if (cur_pw == None || !emx11_window_find(dpy, cur_pw)) {
        int lx_fb = 0, ly_fb = 0;
        EmxWindow *pt = hit_test(dpy, x_root, y_root, 0, &lx_fb, &ly_fb);
        cur_pw = pt ? pt->id : None;
    }
    update_pointer_window(dpy, cur_pw, x_root, y_root, state);

    /* MotionNotify routing: grab window during a grab, Host-hint window
     * otherwise. The Host's findWindowAt is z-order-aware and correctly
     * picks a popup menu item over a deeper ancestor underneath. */
    EmxWindow *motion_target;
    bool via_grab = false;
    if (grab_window != None) {
        motion_target = emx11_window_find(dpy, grab_window);
        if (!motion_target || !motion_target->mapped) return;
        via_grab = true;
    } else {
        motion_target = emx11_window_find(dpy, window);
        if (!motion_target) {
            /* Host hint invalid -- fall back to C-side hit_test. */
            int lx_fb = 0, ly_fb = 0;
            motion_target = hit_test(dpy, x_root, y_root,
                                     PointerMotionMask | ButtonMotionMask,
                                     &lx_fb, &ly_fb);
            if (!motion_target) return;
        }
    }
    /* Mask gate. Skipped during an implicit pointer grab: x11protocol
     * §523 specifies that MotionNotify (and ButtonRelease) are reported
     * to the grabbing client regardless of the grab window's selected
     * event mask. xserver/dix/events.c::CheckMotion mirrors this -- grab
     * delivery bypasses the per-window mask check. Without this skip,
     * twm's f.move loop never sees motion: twm selects only
     * ButtonPressMask|Expose|Enter|Leave on its title-bar frame, and the
     * grab during a drag pins motion_target to that frame, so every
     * motion event hits the mask gate and gets dropped. The drag loop's
     * XQueryPointer keeps reading the press position, abs(...) <
     * MoveDelta stays true, and twm's `f.deltastop` aborts the move
     * without ever calling XMoveWindow -- so the window never moves and
     * controls under the press point remain hot. */
    if (!via_grab && !active_grab &&
        !(motion_target->event_mask & (PointerMotionMask | ButtonMotionMask)))
        return;

    EM_ASM({
        var d = globalThis.emX11 && globalThis.emX11._debug;
        if (d && d.traceCMot) {
            console.log('[c-mot] conn=' + $0 + ' rx=' + $1 + ' ry=' + $2 +
                        ' grab=' + ($3 ? 'Y' : 'N') +
                        ' target=' + ($4 >>> 0) +
                        ' mask=0x' + ($5 >>> 0).toString(16));
        }
    }, dpy->conn_id, x_root, y_root, via_grab ? 1 : 0,
       motion_target->id, (unsigned long)motion_target->event_mask);

    int ax = 0, ay = 0, depth;
    window_abs_origin(dpy, motion_target, &ax, &ay, &depth);

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xmotion.type        = MotionNotify;
    ev.xmotion.display     = dpy;
    ev.xmotion.window      = motion_target->id;
    ev.xmotion.x           = x_root - ax;
    ev.xmotion.y           = y_root - ay;
    ev.xmotion.x_root      = x_root;
    ev.xmotion.y_root      = y_root;
    ev.xmotion.state       = state;
    ev.xmotion.is_hint     = NotifyNormal;
    ev.xmotion.same_screen = True;
    ev.xmotion.time        = event_now();
    emx11_event_queue_push(dpy, &ev);
}

EMSCRIPTEN_KEEPALIVE
void emx11_push_key_event_kc(int type, Window window,
                             unsigned int keycode, unsigned int keysym,
                             unsigned int state, int x, int y);

EMSCRIPTEN_KEEPALIVE
void emx11_push_key_event(int type, Window window, unsigned int keysym,
                          unsigned int state, int x, int y) {
    /* Legacy entry point: only the keysym is supplied; we derive a
     * synthetic keycode by reverse-looking-up the keysym in
     * keysym_table. Kept so external consumers built against an older
     * em-x11 still link. New host bridges should call
     * emx11_push_key_event_kc which threads the physical keycode
     * through from the browser's KeyboardEvent.code. */
    Display *dpy = emx11_get_display();
    KeyCode kc = emx11_keysym_to_keycode(dpy, (KeySym)keysym);
    emx11_push_key_event_kc(type, window, (unsigned int)kc, keysym, state, x, y);
}

EMSCRIPTEN_KEEPALIVE
void emx11_push_key_event_kc(int type, Window window,
                             unsigned int keycode, unsigned int keysym,
                             unsigned int state, int x, int y) {
    Display *dpy = emx11_get_display();
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xkey.type        = type;
    ev.xkey.display     = dpy;
    ev.xkey.window      = window;
    ev.xkey.x           = x;
    ev.xkey.y           = y;
    ev.xkey.x_root      = x;
    ev.xkey.y_root      = y;
    ev.xkey.state       = state;
    ev.xkey.keycode     = (KeyCode)keycode;
    ev.xkey.same_screen = True;
    ev.xkey.time        = event_now();
    /* Make sure keysym_table[kc] reflects the keysym we just dispatched
     * so subsequent XLookupKeysym / XkbKeycodeToKeysym calls for this
     * physical key return the right value -- including layout-specific
     * keysyms patched in after Display init by the host layoutmap. */
    if (keycode > 0 && keycode < 256 && keysym != 0) {
        dpy->keysym_table[keycode] = (KeySym)keysym;
    }
    /* Record the slot BEFORE push -- emx11_event_queue_push will write
     * to dpy->event_tail then advance it. The text snapshot has to land
     * in the same slot. */
    unsigned int slot = dpy->event_tail;
    if (emx11_event_queue_push(dpy, &ev)) {
        emx11_xim_capture_key_text(dpy, slot);
    } else {
        /* Queue full: drop the staged text too so the next successful
         * push doesn't claim text that belonged to the dropped event. */
        dpy->pending_key_text[0]  = '\0';
        dpy->pending_key_text_len = 0;
    }
}

EMSCRIPTEN_KEEPALIVE
void emx11_push_expose_event(Window window, int x, int y, int w, int h) {
    Display *dpy = emx11_get_display();
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xexpose.type    = Expose;
    ev.xexpose.display = dpy;
    ev.xexpose.window  = window;
    ev.xexpose.x       = x;
    ev.xexpose.y       = y;
    ev.xexpose.width   = w;
    ev.xexpose.height  = h;
    emx11_event_queue_push(dpy, &ev);
}

/* -- Substructure redirect dispatch ---------------------------------------
 *
 * The Host ccall's these on the holder module whenever a redirect would
 * normally have been served by the X server. They construct the canonical
 * XMapRequestEvent (etc.) and push it onto the holder's queue so the WM's
 * normal event loop picks it up via XNextEvent (x11protocol.txt §1592).
 */

EMSCRIPTEN_KEEPALIVE
void emx11_push_map_request(Window parent, Window window) {
    Display *dpy = emx11_get_display();
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xmaprequest.type    = MapRequest;
    ev.xmaprequest.display = dpy;
    ev.xmaprequest.parent  = parent;
    ev.xmaprequest.window  = window;
    emx11_event_queue_push(dpy, &ev);
}

/* Cross-connection ReparentNotify delivery. Called by the Host on the
 * window's *owner* module after a (typically WM-issued) XReparentWindow
 * succeeds. Two jobs in one entry point:
 *
 *   1. Update the local EmxWindow shadow unconditionally. The owner did
 *      not issue the reparent (twm did, on its own display), so its
 *      shadow still has the pre-reparent parent/x/y. Without this fix,
 *      window_abs_origin walks the stale chain to root and reports the
 *      pre-reparent absolute coords; ButtonPress/Motion get translated
 *      with the wrong offset, hover/click hit the wrong widget, and
 *      Tk/Xt's redraw paths (which read x,y from local shadow) draw at
 *      the wrong place too. See project_emx11_mask_gating.md for the
 *      adjacent crash this enables, and the session notes for why the
 *      shadow fix has to be unconditional rather than mask-gated.
 *
 *   2. Synthesise the ReparentNotify XEvent and push it -- but only if
 *      the owner actually selected StructureNotifyMask on the window
 *      (or SubstructureNotifyMask on the new parent). Matches dix's
 *      DeliverEvents gating; Xt's Shell widget selects StructureNotify
 *      on its shell, so it does receive this. */
EMSCRIPTEN_KEEPALIVE
void emx11_push_reparent_notify(Window window, Window parent, int x, int y) {
    Display *dpy = emx11_get_display();
    EmxWindow *win = emx11_window_find(dpy, window);
    if (win) {
        win->parent = parent;
        win->x      = x;
        win->y      = y;
    }

    /* Mask gate: same shape as window.c::wants_structure -- StructureNotify
     * on the window itself OR SubstructureNotify on the new parent. */
    bool wants = false;
    if (win && (win->event_mask & StructureNotifyMask)) wants = true;
    if (!wants && parent != None) {
        EmxWindow *p = emx11_window_find(dpy, parent);
        if (p && (p->event_mask & SubstructureNotifyMask)) wants = true;
    }
    if (!wants) return;

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xreparent.type        = ReparentNotify;
    ev.xreparent.display     = dpy;
    ev.xreparent.event       = window;
    ev.xreparent.window      = window;
    ev.xreparent.parent      = parent;
    ev.xreparent.x           = x;
    ev.xreparent.y           = y;
    ev.xreparent.override_redirect = win ? win->override_redirect : False;
    emx11_event_queue_push(dpy, &ev);
}

/* Cross-connection ConfigureNotify delivery. Called by the Host on the
 * window's *owner* module after a (typically WM-issued) XConfigureWindow
 * / XMoveResizeWindow / XResizeWindow / XMoveWindow on the owner's
 * shell. Mirrors emx11_push_reparent_notify:
 *
 *   1. Update the local EmxWindow shadow unconditionally so subsequent
 *      drawing / hit-testing on the owner uses the correct geometry.
 *      Without this, Tk/Xt/Xlib clients keep painting and propagating
 *      events with their stale pre-resize size.
 *
 *   2. Synthesise the ConfigureNotify XEvent and push it -- mask-gated
 *      on StructureNotifyMask on the window itself, or
 *      SubstructureNotifyMask on the parent (mirrors dix's DeliverEvents).
 *      Xt's Shell widget and Tk's TopLevel both select StructureNotify
 *      on their shells, which is what triggers their re-layout pass. */
EMSCRIPTEN_KEEPALIVE
void emx11_push_configure_notify(Window window, int x, int y,
                                 int width, int height, int border_width) {
    Display *dpy = emx11_get_display();
    EmxWindow *win = emx11_window_find(dpy, window);
    if (win) {
        win->x            = x;
        win->y            = y;
        win->width        = (unsigned int)width;
        win->height       = (unsigned int)height;
        win->border_width = (unsigned int)border_width;
    }

    bool wants = false;
    if (win && (win->event_mask & StructureNotifyMask)) wants = true;
    if (!wants && win && win->parent != None) {
        EmxWindow *p = emx11_window_find(dpy, win->parent);
        if (p && (p->event_mask & SubstructureNotifyMask)) wants = true;
    }
    if (!wants) return;

    /* Bump request serial so Tk/Xt's WaitForConfigureNotify accepts the
     * synthetic event on the first poll. The owner did not issue the
     * configure, so its dpy->request hasn't moved; matching what
     * notify_js_reconfigure does for the local-issuer case. */
    dpy->request++;

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xconfigure.type        = ConfigureNotify;
    ev.xconfigure.serial      = dpy->request;
    ev.xconfigure.send_event  = False;
    ev.xconfigure.display     = dpy;
    ev.xconfigure.event       = window;
    ev.xconfigure.window      = window;
    ev.xconfigure.x           = x;
    ev.xconfigure.y           = y;
    ev.xconfigure.width       = width;
    ev.xconfigure.height      = height;
    ev.xconfigure.border_width = border_width;
    ev.xconfigure.above       = None;
    ev.xconfigure.override_redirect = win ? win->override_redirect : False;
    emx11_event_queue_push(dpy, &ev);
}

/* Cross-connection DestroyNotify dispatch. Called by the Host when a
 * client connection closes (process exit, XCloseDisplay) so the WM
 * watching that client's parent (typically twm holding SubstructureNotify
 * on its frames) tears down its TwmWindow record + frame. Without this,
 * the dead client's frame lingers as a transparent outline -- twm has
 * no way to learn the inner window is gone otherwise.
 *
 * `event_window` is the parent (the observer); `window` is the dying
 * window. Mask gate matches dix's DeliverEvents: SubstructureNotify
 * on the parent. We don't model the StructureNotify-on-window path
 * here because by the time this fires, the window's own owner is
 * already gone -- nobody on this side cares. */
EMSCRIPTEN_KEEPALIVE
void emx11_push_destroy_notify(Window window, Window event_window) {
    Display *dpy = emx11_get_display();
    EmxWindow *evw = emx11_window_find(dpy, event_window);
    if (!evw || !(evw->event_mask & SubstructureNotifyMask)) return;

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xdestroywindow.type    = DestroyNotify;
    ev.xdestroywindow.display = dpy;
    ev.xdestroywindow.event   = event_window;
    ev.xdestroywindow.window  = window;
    emx11_event_queue_push(dpy, &ev);
}

/* -- Passive input grabs -- */

int XGrabButton(Display *dpy, unsigned int button, unsigned int modifiers,
                Window grab_window, Bool owner_events,
                unsigned int event_mask, int pointer_mode, int keyboard_mode,
                Window confine_to, Cursor cursor) {
    (void)dpy;
    emx11_js_grab_button(grab_window, button, modifiers,
                         owner_events ? 1 : 0, event_mask,
                         pointer_mode, keyboard_mode, confine_to, cursor);
    return 1;
}

int XUngrabButton(Display *dpy, unsigned int button, unsigned int modifiers,
                  Window grab_window) {
    (void)dpy;
    emx11_js_ungrab_button(grab_window, button, modifiers);
    return 1;
}

int XGrabKey(Display *a, int b, unsigned int c, Window d, Bool e,
             int f, int g) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return 1;
}

int XUngrabKeyboard(Display *dpy, Time t) { (void)dpy; (void)t; return 1; }

int XUngrabPointer(Display *dpy, Time t) {
    (void)t;
    dpy->request++;
    active_grab = false;
    emx11_js_set_grab_cursor(0);
    emx11_js_ungrab_pointer();
    return 1;
}

int XGrabPointer(Display *dpy, Window grab_window, Bool owner_events,
                 unsigned int event_mask, int pointer_mode, int keyboard_mode,
                 Window confine_to, Cursor cursor, Time t) {
    (void)event_mask; (void)pointer_mode; (void)keyboard_mode;
    (void)confine_to; (void)t;
    dpy->request++;
    emx11_reset_implicit_grab();
    active_grab = true;
    emx11_js_set_grab_cursor((unsigned int)cursor);
    emx11_js_grab_pointer((unsigned int)dpy->conn_id,
                          grab_window, owner_events ? 1 : 0);
    return GrabSuccess;
}

int XWarpPointer(Display *dpy, Window src_w, Window dest_w,
                 int src_x, int src_y, unsigned int src_width, unsigned int src_height,
                 int dest_x, int dest_y) {
    (void)dpy; (void)src_w; (void)dest_w;
    (void)src_x; (void)src_y; (void)src_width; (void)src_height;
    (void)dest_x; (void)dest_y;
    return 1;
}

int XUngrabKey(Display *dpy, int keycode, unsigned int modifiers,
               Window grab_window) {
    (void)dpy; (void)keycode; (void)modifiers; (void)grab_window;
    return 1;
}

int XGrabKeyboard(Display *dpy, Window grab_window, Bool owner_events,
                  int pointer_mode, int keyboard_mode, Time t) {
    (void)dpy; (void)grab_window; (void)owner_events;
    (void)pointer_mode; (void)keyboard_mode; (void)t;
    return GrabSuccess;
}

/* -- XQueryPointer -- */

Bool XQueryPointer(Display *dpy, Window w, Window *root_return,
                   Window *child_return, int *root_x_return, int *root_y_return,
                   int *win_x_return, int *win_y_return,
                   unsigned int *mask_return) {
    int px = 0, py = 0;
    emx11_js_pointer_xy(&px, &py);
    int wx = px, wy = py;
    if (w != None) {
        int origin[EMX11_ABS_ORIGIN_SIZE] = {0};
        emx11_js_get_window_abs_origin(w, origin);
        if (origin[EMX11_ABS_ORIGIN_PRESENT]) {
            wx = px - origin[EMX11_ABS_ORIGIN_AX];
            wy = py - origin[EMX11_ABS_ORIGIN_AY];
        }
    }
    EM_ASM({
        var d = globalThis.emX11 && globalThis.emX11._debug;
        if (d && d.traceQp) {
            console.log('[c-qp] conn=' + $0 + ' win=' + $1 +
                        ' root=(' + $2 + ',' + $3 + ')' +
                        ' local=(' + $4 + ',' + $5 + ')');
        }
    }, dpy->conn_id, w, px, py, wx, wy);
    if (root_return)     *root_return     = dpy->screens[0].root;
    if (child_return)    *child_return    = None;
    if (root_x_return)   *root_x_return   = px;
    if (root_y_return)   *root_y_return   = py;
    if (win_x_return)    *win_x_return    = wx;
    if (win_y_return)    *win_y_return    = wy;
    if (mask_return)     *mask_return     = 0;
    return True;
}

/* -- XInput2 stubs -- */

Status XIQueryVersion(Display *dpy, int *major_version_inout,
                      int *minor_version_inout) {
    (void)dpy; (void)major_version_inout; (void)minor_version_inout;
    return BadRequest;
}

int XISelectEvents(Display *dpy, Window win, XIEventMask *masks,
                   int num_masks) {
    (void)dpy; (void)win; (void)masks; (void)num_masks;
    return Success;
}
