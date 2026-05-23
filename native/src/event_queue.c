/*
 * Event queue: ring buffer + maskable scans + the public XPending /
 * XEventsQueued / XNextEvent entry points. Pure storage layer; no
 * synthesis, no input routing -- those live in event.c (input)
 * and event_send.c (synthetic delivery).
 */

#include "emx11_internal.h"

#include <emscripten.h>

bool emx11_event_queue_push(Display *dpy, const XEvent *event) {
    unsigned int next_tail = (dpy->event_tail + 1) % EMX11_EVENT_QUEUE_CAPACITY;
    if (next_tail == dpy->event_head) {
        return false;
    }
    dpy->event_queue[dpy->event_tail] = *event;
    dpy->event_tail = next_tail;
    dpy->qlen = (int)emx11_event_queue_size(dpy);
    return true;
}

bool emx11_event_queue_pop(Display *dpy, XEvent *out) {
    if (dpy->event_head == dpy->event_tail) {
        return false;
    }
    *out = dpy->event_queue[dpy->event_head];
    /* Mirror the event's UTF-8 text slot into dpy->current_key_text so
     * a subsequent Xutf8LookupString picks up exactly what JS staged
     * for this event. Cheap no-op for non-key events. */
    if (out->type == KeyPress || out->type == KeyRelease) {
        emx11_xim_capture_pop_text(dpy, dpy->event_head);
    }
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
    if (emx11_event_queue_size(display) == 0) return 0;
    return emx11_event_queue_pop(display, event_return) ? 1 : 0;
}

/* -- Event helpers -- */

Bool XFilterEvent(XEvent *event, Window w) {
    (void)event; (void)w;
    return False;
}

Bool XCheckIfEvent(Display *dpy, XEvent *event_return,
                   Bool (*predicate)(Display *, XEvent *, XPointer),
                   XPointer arg) {
    if (!dpy || !event_return || !predicate) return False;
    unsigned int i = dpy->event_head;
    while (i != dpy->event_tail) {
        XEvent *e = &dpy->event_queue[i];
        if (predicate(dpy, e, arg)) {
            *event_return = *e;
            unsigned int next = (i + 1) % EMX11_EVENT_QUEUE_CAPACITY;
            while (next != dpy->event_tail) {
                dpy->event_queue[i] = dpy->event_queue[next];
                i = next;
                next = (next + 1) % EMX11_EVENT_QUEUE_CAPACITY;
            }
            dpy->event_tail =
                (dpy->event_tail + EMX11_EVENT_QUEUE_CAPACITY - 1) %
                EMX11_EVENT_QUEUE_CAPACITY;
            return True;
        }
        i = (i + 1) % EMX11_EVENT_QUEUE_CAPACITY;
    }
    return False;
}

int XIfEvent(Display *dpy, XEvent *event_return,
             Bool (*predicate)(Display *, XEvent *, XPointer),
             XPointer arg) {
    while (!XCheckIfEvent(dpy, event_return, predicate, arg)) {
        emscripten_sleep(1);
    }
    return 1;
}

int XPeekEvent(Display *dpy, XEvent *event_return) {
    while (dpy->event_head == dpy->event_tail) emscripten_sleep(1);
    *event_return = dpy->event_queue[dpy->event_head];
    return 1;
}

int XPutBackEvent(Display *dpy, XEvent *event) {
    if (!dpy || !event) return 0;
    dpy->event_head = (dpy->event_head + EMX11_EVENT_QUEUE_CAPACITY - 1) %
                       EMX11_EVENT_QUEUE_CAPACITY;
    dpy->event_queue[dpy->event_head] = *event;
    return 1;
}

static int XSynchronize_noop(Display *dpy) { (void)dpy; return 0; }

int (*XSynchronize(Display *dpy, Bool onoff))(Display *) {
    (void)dpy; (void)onoff;
    return XSynchronize_noop;
}

Bool XCheckMaskEvent(Display *dpy, long event_mask, XEvent *ev) {
    return emx11_event_queue_peek_match(dpy, event_mask, ev) ? True : False;
}

Bool XCheckTypedWindowEvent(Display *dpy, Window w, int event_type, XEvent *ev) {
    return emx11_event_queue_peek_typed(dpy, w, event_type, ev) ? True : False;
}

int XMaskEvent(Display *dpy, long event_mask, XEvent *ev) {
    for (;;) {
        if (emx11_event_queue_peek_match(dpy, event_mask, ev)) return 1;
        emscripten_sleep(10);
    }
}
