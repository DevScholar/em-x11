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
