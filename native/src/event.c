#include "emx11_internal.h"

#include <X11/Xutil.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

bool emx11_event_queue_push(Display *dpy, const XEvent *event) {
    unsigned int next_tail = (dpy->event_tail + 1) % EMX11_EVENT_QUEUE_CAPACITY;
    if (next_tail == dpy->event_head) {
        return false;                           /* queue full -- drop */
    }
    dpy->event_queue[dpy->event_tail] = *event;
    dpy->event_tail = next_tail;
    /* Keep Xlib's public qlen field in sync. Tk's TransferXEventsToTcl
     * reads QLength(display) (= dpy->qlen) to decide whether to drain
     * with XNextEvent; without this update Tk sees 0 events regardless
     * of what XEventsQueued / XPending report. */
    dpy->qlen = (int)emx11_event_queue_size(dpy);
    return true;
}

bool emx11_event_queue_pop(Display *dpy, XEvent *out) {
    if (dpy->event_head == dpy->event_tail) {
        return false;
    }
    *out = dpy->event_queue[dpy->event_head];
    dpy->event_head = (dpy->event_head + 1) % EMX11_EVENT_QUEUE_CAPACITY;
    dpy->qlen = (int)emx11_event_queue_size(dpy);
    return true;
}

unsigned int emx11_event_queue_size(const Display *dpy) {
    if (dpy->event_tail >= dpy->event_head) {
        return dpy->event_tail - dpy->event_head;
    }
    return EMX11_EVENT_QUEUE_CAPACITY - dpy->event_head + dpy->event_tail;
}

/* Map an event type to the input-mask bit(s) that would select it.
 * Used by XCheckMaskEvent / XMaskEvent to decide whether a queued
 * event satisfies the caller's requested mask. Unmaskable events
 * (ClientMessage, SelectionRequest, etc.) return 0 so they never
 * match a mask-scan. */
static long event_type_to_mask(int type) {
    switch (type) {
        case KeyPress:         return KeyPressMask;
        case KeyRelease:       return KeyReleaseMask;
        case ButtonPress:      return ButtonPressMask;
        case ButtonRelease:    return ButtonReleaseMask;
        case MotionNotify:     return PointerMotionMask | ButtonMotionMask;
        case EnterNotify:      return EnterWindowMask;
        case LeaveNotify:      return LeaveWindowMask;
        case FocusIn:
        case FocusOut:         return FocusChangeMask;
        case KeymapNotify:     return KeymapStateMask;
        case Expose:           return ExposureMask;
        case VisibilityNotify: return VisibilityChangeMask;
        case CreateNotify:     return SubstructureNotifyMask;
        case DestroyNotify:
        case UnmapNotify:
        case MapNotify:
        case ReparentNotify:
        case ConfigureNotify:
        case GravityNotify:
        case CirculateNotify:  return StructureNotifyMask | SubstructureNotifyMask;
        case MapRequest:
        case ConfigureRequest:
        case CirculateRequest: return SubstructureRedirectMask;
        case ResizeRequest:    return ResizeRedirectMask;
        case PropertyNotify:   return PropertyChangeMask;
        case ColormapNotify:   return ColormapChangeMask;
        default:               return 0;
    }
}

/* Remove the slot at `idx` from the ring, shifting everything that
 * follows it down by one. `idx` must lie in the range [head, tail). */
static void queue_remove_at(Display *dpy, unsigned int idx) {
    unsigned int cap = EMX11_EVENT_QUEUE_CAPACITY;
    unsigned int cur = idx;
    for (;;) {
        unsigned int next = (cur + 1) % cap;
        if (next == dpy->event_tail) break;
        dpy->event_queue[cur] = dpy->event_queue[next];
        cur = next;
    }
    dpy->event_tail = (dpy->event_tail + cap - 1) % cap;
    dpy->qlen = (int)emx11_event_queue_size(dpy);
}

bool emx11_event_queue_peek_match(Display *dpy, long mask, XEvent *out) {
    unsigned int cap = EMX11_EVENT_QUEUE_CAPACITY;
    unsigned int n = emx11_event_queue_size(dpy);
    for (unsigned int i = 0; i < n; i++) {
        unsigned int idx = (dpy->event_head + i) % cap;
        if (event_type_to_mask(dpy->event_queue[idx].type) & mask) {
            if (out) *out = dpy->event_queue[idx];
            queue_remove_at(dpy, idx);
            return true;
        }
    }
    return false;
}

bool emx11_event_queue_peek_typed(Display *dpy, Window w, int type, XEvent *out) {
    unsigned int cap = EMX11_EVENT_QUEUE_CAPACITY;
    unsigned int n = emx11_event_queue_size(dpy);
    for (unsigned int i = 0; i < n; i++) {
        unsigned int idx = (dpy->event_head + i) % cap;
        const XEvent *ev = &dpy->event_queue[idx];
        if (ev->type == type && ev->xany.window == w) {
            if (out) *out = *ev;
            queue_remove_at(dpy, idx);
            return true;
        }
    }
    return false;
}

int XPending(Display *display) {
    return (int)emx11_event_queue_size(display);
}

int XEventsQueued(Display *display, int mode) {
    /* All three modes (QueuedAlready, QueuedAfterReading, QueuedAfterFlush)
     * reduce to the same answer for us: there is no server, so flushing
     * output and reading input are both no-ops and only the local queue
     * matters. */
    (void)mode;
    return (int)emx11_event_queue_size(display);
}

int XNextEvent(Display *display, XEvent *event_return) {
    /* XNextEvent blocks in real X. emscripten_sleep yields to the browser
     * event loop; Asyncify must be enabled at link time for this to work. */
    while (emx11_event_queue_size(display) == 0) {
        emscripten_sleep(1);
    }
    return emx11_event_queue_pop(display, event_return) ? 1 : 0;
}

/* Deliver a synthesized event. In real X this round-trips to the server,
 * which re-dispatches per the event_mask / propagate rules; we deliver
 * directly into our own queue because em-x11 is a single-process "server".
 *
 * Tk uses XSendEvent heavily for self-addressed traffic: WM_DELETE_WINDOW
 * via ClientMessage, `event generate` virtual events, focus-in/out
 * synthetics. Most of those pass event_mask=NoEventMask (= 0), which
 * means "deliver regardless of selection" -- the simple path.
 *
 * Propagation semantics (x11protocol.txt §SendEvent): if propagate=True
 * and no client on `w` selects any bit of event_mask, walk up the parent
 * chain until one does, or until a window has that bit in its
 * do_not_propagate_mask (we don't track that yet -- TODO). If nothing
 * along the chain matches, the event is discarded.
 *
 * PointerWindow / InputFocus as `w`: real Xlib resolves these against
 * current pointer / focus state. InputFocus resolves to
 * display->focus_window (wired below); PointerWindow is still a TODO
 * (needs a pointer-position hit test, which means a JS round-trip we
 * haven't needed yet). Tk's explicit-send paths use real XIDs, not
 * these sentinels, so the gap hasn't bitten in practice. */
Status XSendEvent(Display *display, Window w, Bool propagate,
                  long event_mask, XEvent *event_send) {
    if (!display || !event_send) return 0;

    /* Resolve InputFocus sentinel to the concrete focus XID before any
     * further bookkeeping -- the rest of this function treats `w` as a
     * real window id. PointerWindow (= 0) coincides numerically with
     * None so we leave it alone; the caller gets delivery on xany.window
     * == 0, which Tk self-filters. */
    if (w == InputFocus) {
        w = display->focus_window;
    }

    /* Clipboard proxy intercept: when a real CLIPBOARD owner responds to
     * our back-channel XConvertSelection by sending SelectionNotify to
     * the proxy, forward the UTF-8 bytes to navigator.clipboard.writeText()
     * and consume the event (nothing on the X side should ever read from
     * the proxy's queue). Returns True only if the event was handled. */
    if (emx11_selection_intercept_send(display, w, event_send)) {
        return 1;
    }

    XEvent ev = *event_send;                    /* caller may reuse buffer */
    ev.xany.display    = display;
    ev.xany.send_event = True;
    ev.xany.window     = w;

    /* NoEventMask or unmaskable event type: deliver to `w` unconditionally. */
    if (event_mask == 0) {
        return emx11_event_queue_push(display, &ev) ? 1 : 0;
    }

    /* Find the closest window up from `w` that selects any bit of
     * event_mask. If propagate=False, only `w` itself is a candidate. */
    EmxWindow *target = (w == PointerWindow || w == None)
                            ? NULL
                            : emx11_window_find(display, w);
    Window root = display->screens[0].root;
    while (target) {
        if (target->event_mask & event_mask) {
            ev.xany.window = target->id;
            return emx11_event_queue_push(display, &ev) ? 1 : 0;
        }
        if (!propagate) break;
        if (target->parent == None || target->parent == target->id) break;
        if (target->id == root) break;
        target = emx11_window_find(display, target->parent);
    }

    /* Nothing along the chain selected for this event. Matches real X's
     * "discard" outcome, but we still queue it on `w` so Tk's direct
     * self-sends (XSendEvent to its own shell with a mask the shell
     * happens not to select) don't silently disappear. Revisit once
     * event_mask selection is well-exercised by real Tk traffic. */
    ev.xany.window = w;
    return emx11_event_queue_push(display, &ev) ? 1 : 0;
}

/* -- Input focus ---------------------------------------------------------- */

/* Push a FocusIn or FocusOut event onto the queue. `mode` / `detail` are
 * the X protocol classifications; NotifyNormal + NotifyNonlinear is the
 * standard pair for an explicit XSetInputFocus between unrelated
 * windows. PointerRoot and None don't get synthetic events -- there's
 * no target window to address. */
static void push_focus_change(Display *dpy, int type, Window w,
                              int mode, int detail) {
    if (w == None || w == PointerRoot) return;
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xfocus.type    = type;
    ev.xfocus.display = dpy;
    ev.xfocus.window  = w;
    ev.xfocus.mode    = mode;
    ev.xfocus.detail  = detail;
    emx11_event_queue_push(dpy, &ev);
}

/* x11protocol.txt §SetInputFocus: update focus iff `time` is no earlier
 * than the last-focus-change-time. CurrentTime (0) in the request is
 * accepted unconditionally and treated as "now". We synthesize a
 * FocusOut on the outgoing window and a FocusIn on the incoming one so
 * Tk's focus tracker (tk/generic/tkFocus.c) sees the transition. */
int XSetInputFocus(Display *display, Window focus, int revert_to, Time time) {
    if (!display) return 0;
    if (time != CurrentTime && time < display->focus_last_time) {
        return 1;                               /* too old -- silent no-op  */
    }

    Window old_focus = display->focus_window;
    display->focus_window    = focus;
    display->focus_revert_to = revert_to;
    display->focus_last_time = (time == CurrentTime) ? display->focus_last_time
                                                      : time;

    if (old_focus != focus) {
        push_focus_change(display, FocusOut, old_focus,
                          NotifyNormal, NotifyNonlinear);
        push_focus_change(display, FocusIn,  focus,
                          NotifyNormal, NotifyNonlinear);
    }
    return 1;
}

int XGetInputFocus(Display *display, Window *focus_return,
                   int *revert_to_return) {
    if (!display) return 0;
    if (focus_return)     *focus_return     = display->focus_window;
    if (revert_to_return) *revert_to_return = display->focus_revert_to;
    return 1;
}

/* -- Keymap management ---------------------------------------------------- */

KeyCode emx11_keysym_to_keycode(Display *dpy, KeySym keysym) {
    if (keysym == NoSymbol) return 0;
    /* Reverse lookup first so repeated keys don't exhaust the table. */
    for (unsigned int i = 8; i < dpy->next_keycode && i < 256; i++) {
        if (dpy->keysym_table[i] == keysym) {
            return (KeyCode)i;
        }
    }
    if (dpy->next_keycode >= 256) {
        return 0;                               /* table exhausted */
    }
    KeyCode kc = (KeyCode)dpy->next_keycode++;
    dpy->keysym_table[kc] = keysym;
    return kc;
}

/* Internal lookup used by both public entry points to avoid a
 * deprecated-self-call chain on XKeycodeToKeysym. */
static KeySym emx11_keysym_for(Display *dpy, unsigned int keycode) {
    if (keycode >= 256) return NoSymbol;
    return dpy->keysym_table[keycode];
}

/* The NeedWidePrototypes convention in Xfuncproto.h (default 1) widens
 * KeyCode to unsigned int across the function-call boundary. Match that
 * here so our definition agrees with the upstream declaration. */
KeySym XKeycodeToKeysym(Display *dpy, unsigned int keycode, int index) {
    (void)index;                                /* no modifier grid yet */
    return emx11_keysym_for(dpy, keycode);
}

KeySym XLookupKeysym(XKeyEvent *event, int index) {
    (void)index;
    if (!event || !event->display) return NoSymbol;
    return emx11_keysym_for(event->display, event->keycode);
}

KeyCode XKeysymToKeycode(Display *dpy, KeySym keysym) {
    return emx11_keysym_to_keycode(dpy, keysym);
}

/* Translate a key event into a UTF-8 byte sequence for XmbLookupString /
 * text entry paths. em-x11's keymap hands us already-shifted keysyms
 * (the browser does the shift/ctrl translation in KeyboardEvent.key),
 * so printable keys map to their keysym value directly. Special keys
 * and modifiers produce a keysym-only result with buffer_return empty. */
int XLookupString(XKeyEvent *event, char *buffer_return, int bytes_buffer,
                  KeySym *keysym_return, XComposeStatus *status_return) {
    (void)status_return;
    if (!event) return 0;
    KeySym ks = XLookupKeysym(event, 0);
    if (keysym_return) *keysym_return = ks;

    if (!buffer_return || bytes_buffer <= 0) return 0;

    /* ASCII range: emit the codepoint as a single byte. */
    if (ks >= 0x20 && ks <= 0x7e) {
        buffer_return[0] = (char)ks;
        if (bytes_buffer > 1) buffer_return[1] = '\0';
        return 1;
    }
    /* Latin-1 supplement: 0xA0..0xFF produces a single 0x80-0xFF byte
     * under the X11 STRING convention (iso8859-1). */
    if (ks >= 0xa0 && ks <= 0xff) {
        buffer_return[0] = (char)ks;
        if (bytes_buffer > 1) buffer_return[1] = '\0';
        return 1;
    }
    /* Return key, Tab, Backspace, Escape are traditional single-char
     * translations too -- many Xt translation tables expect them. */
    switch (ks) {
    case 0xff0d: if (bytes_buffer > 0) buffer_return[0] = '\r'; return 1;
    case 0xff09: if (bytes_buffer > 0) buffer_return[0] = '\t'; return 1;
    case 0xff08: if (bytes_buffer > 0) buffer_return[0] = '\b'; return 1;
    case 0xff1b: if (bytes_buffer > 0) buffer_return[0] = 0x1b; return 1;
    case 0xffff: if (bytes_buffer > 0) buffer_return[0] = 0x7f; return 1;
    default: break;
    }
    return 0;
}

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
        cur = emx11_window_find(dpy, cur->parent);
    }
    if (ax_out)    *ax_out    = ax;
    if (ay_out)    *ay_out    = ay;
    if (depth_out) *depth_out = depth;
}

/* Given a root-relative point, find the deepest mapped window that
 * (a) contains the point and (b) has at least one of `need_mask` bits
 * in its event_mask. If the deepest containing window doesn't select
 * for the event, propagate up the parent chain to the first ancestor
 * that does -- matching real X's event-propagation semantics. Returns
 * NULL if nothing accepts the event. Root window is excluded from the
 * result (we don't deliver events to root in a single-client world).
 *
 * `lx`/`ly` are filled with the winning window's local coordinates. */
static EmxWindow *hit_test(Display *dpy, int rx, int ry, long need_mask,
                           int *lx_out, int *ly_out) {
    EmxWindow *best = NULL;
    int best_depth = -1;
    int best_ax = 0, best_ay = 0;
    Window root = dpy->screens[0].root;

    for (int i = 0; i < EMX11_MAX_WINDOWS; i++) {
        EmxWindow *w = &dpy->windows[i];
        if (!w->in_use || !w->mapped) continue;
        if (w->id == root) continue;

        int ax, ay, depth;
        window_abs_origin(dpy, w, &ax, &ay, &depth);
        if (rx < ax || ry < ay ||
            rx >= ax + (int)w->width || ry >= ay + (int)w->height) {
            continue;
        }
        if (depth > best_depth) {
            best = w;
            best_depth = depth;
            best_ax = ax;
            best_ay = ay;
        }
    }
    if (!best) return NULL;

    /* Propagate up until an ancestor selects for the event. We keep
     * updating (ax, ay) so local coords stay correct wherever we land. */
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
        if (!p || p->id == root) break;
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
    if (grab_window != None) {
        motion_target = emx11_window_find(dpy, grab_window);
        if (!motion_target || !motion_target->mapped) return;
    } else {
        motion_target = pointer_target;
        if (!motion_target) return;
    }
    if (!(motion_target->event_mask & (PointerMotionMask | ButtonMotionMask)))
        return;

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
