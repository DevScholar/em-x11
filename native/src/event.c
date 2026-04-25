#include "emx11_internal.h"

#include <emscripten.h>
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

/* Called from JS (bindings/emx11.library.js) when the browser generates a
 * DOM event that should surface on the X side. */
EMSCRIPTEN_KEEPALIVE
void emx11_push_button_event(int type, Window window, int x, int y,
                             int x_root, int y_root, unsigned int button,
                             unsigned int state) {
    Display *dpy = emx11_get_display();
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xbutton.type        = type;
    ev.xbutton.display     = dpy;
    ev.xbutton.window      = window;
    ev.xbutton.x           = x;
    ev.xbutton.y           = y;
    ev.xbutton.x_root      = x_root;
    ev.xbutton.y_root      = y_root;
    ev.xbutton.button      = button;
    ev.xbutton.state       = state;
    ev.xbutton.same_screen = True;
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
