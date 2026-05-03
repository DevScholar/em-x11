/*
 * Input dispatch: hit-testing, pointer-window tracking, and the C entry
 * points the JS host calls (emx11_push_button_event / motion / key /
 * expose / map_request / reparent_notify). Queue plumbing lives in
 * event_queue.c; XSendEvent + focus in event_send.c; keysyms in
 * event_keysym.c.
 */

#include "emx11_internal.h"
#include "emx11_meta_layout.h"

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

/* Push an EnterNotify / LeaveNotify on `w`, iff the window selects for that
 * mask. Coords are root-relative; we derive window-local ones from the
 * window's absolute origin. */
static void emit_crossing(Display *dpy, int type, Window w,
                          int x_root, int y_root, unsigned int state) {
    EmxWindow *win = emx11_window_find(dpy, w);
    if (!win) return;
    long mask = (type == EnterNotify) ? EnterWindowMask : LeaveWindowMask;
    if (!(win->event_mask & mask)) return;

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
    ev.xcrossing.detail      = NotifyNonlinear;
    ev.xcrossing.same_screen = True;
    ev.xcrossing.focus       = (w == dpy->focus_window);
    ev.xcrossing.state       = state;
    ev.xcrossing.time        = event_now();
    emx11_event_queue_push(dpy, &ev);
}

/* Update last_pointer_window, emitting Leave on the outgoing window and
 * Enter on the incoming one. Called on every motion, and on ButtonPress
 * (so the "first interaction is a click, no prior mousemove" path still
 * delivers the Enter that Tk's button bindings depend on). */
static void update_pointer_window(Display *dpy, Window cur,
                                  int x_root, int y_root, unsigned int state) {
    if (cur == last_pointer_window) return;
    if (last_pointer_window != None) {
        emit_crossing(dpy, LeaveNotify, last_pointer_window,
                      x_root, y_root, state);
    }
    if (cur != None) {
        emit_crossing(dpy, EnterNotify, cur, x_root, y_root, state);
    }
    last_pointer_window = cur;
}

EMSCRIPTEN_KEEPALIVE
void emx11_push_button_event(int type, Window window, int x, int y,
                             int x_root, int y_root, unsigned int button,
                             unsigned int state) {
    (void)window; (void)x; (void)y;             /* hint from JS, discarded */
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
        long mask = (type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask;
        target = hit_test(dpy, x_root, y_root, mask, &lx, &ly);
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
     * each wasm process receives. Gate on a pointer so the JS side can
     * toggle it via globalThis.__EMX11_TRACE_C_BTN__. Gate via EM_ASM
     * so it's free when disabled; the EM_ASM is itself ~one-line
     * runtime cost when enabled. */
    EM_ASM({
        if (globalThis.__EMX11_TRACE_C_BTN__) {
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
    (void)window; (void)x; (void)y;
    Display *dpy = emx11_get_display();

    /* Pointer-window tracking is independent of grab: crossings fire
     * whenever the pointer crosses a window boundary. hit_test with
     * need_mask=0 falls through to the "deepest hit" branch and returns
     * the window the pointer is actually over, regardless of selection. */
    int lx = 0, ly = 0;
    EmxWindow *pointer_target = hit_test(dpy, x_root, y_root, 0, &lx, &ly);
    Window cur_pw = pointer_target ? pointer_target->id : None;
    update_pointer_window(dpy, cur_pw, x_root, y_root, state);

    /* MotionNotify routing: grab window during a grab, pointer window
     * otherwise. */
    EmxWindow *motion_target;
    bool via_grab = false;
    if (grab_window != None) {
        motion_target = emx11_window_find(dpy, grab_window);
        if (!motion_target || !motion_target->mapped) return;
        via_grab = true;
    } else {
        motion_target = pointer_target;
        if (!motion_target) return;
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
    if (!via_grab &&
        !(motion_target->event_mask & (PointerMotionMask | ButtonMotionMask)))
        return;

    EM_ASM({
        if (globalThis.__EMX11_TRACE_C_MOT__) {
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
void emx11_push_key_event(int type, Window window, unsigned int keysym,
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
    ev.xkey.keycode     = emx11_keysym_to_keycode(dpy, (KeySym)keysym);
    ev.xkey.same_screen = True;
    ev.xkey.time        = event_now();
    emx11_event_queue_push(dpy, &ev);
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
