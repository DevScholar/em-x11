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
    return true;
}

bool emx11_event_queue_pop(Display *dpy, XEvent *out) {
    if (dpy->event_head == dpy->event_tail) {
        return false;
    }
    *out = dpy->event_queue[dpy->event_head];
    dpy->event_head = (dpy->event_head + 1) % EMX11_EVENT_QUEUE_CAPACITY;
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
 * current pointer / focus state. em-x11 has no focus tracker yet, so we
 * treat them as literal XIDs; Tk's routing code inspects xany.window
 * post-delivery and copes. Revisit when XSetInputFocus is promoted from
 * stub to real. */
Status XSendEvent(Display *display, Window w, Bool propagate,
                  long event_mask, XEvent *event_send) {
    if (!display || !event_send) return 0;

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
    EmxWindow *target = (w == PointerWindow || w == InputFocus)
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

EMSCRIPTEN_KEEPALIVE
void emx11_push_button_event(int type, Window window, int x, int y,
                             int x_root, int y_root, unsigned int button,
                             unsigned int state) {
    (void)window; (void)x; (void)y;             /* hint from JS, discarded */
    Display *dpy = emx11_get_display();
    long mask = (type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask;
    int lx = 0, ly = 0;
    EmxWindow *target = hit_test(dpy, x_root, y_root, mask, &lx, &ly);
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
    int lx = 0, ly = 0;
    EmxWindow *target = hit_test(dpy, x_root, y_root,
                                 PointerMotionMask | ButtonMotionMask,
                                 &lx, &ly);
    if (!target) return;

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xmotion.type        = MotionNotify;
    ev.xmotion.display     = dpy;
    ev.xmotion.window      = target->id;
    ev.xmotion.x           = lx;
    ev.xmotion.y           = ly;
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
