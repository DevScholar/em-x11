#include "emx11_internal.h"

#include <X11/Xutil.h>
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

/* -- JS -> C event bridges ------------------------------------------------- */

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
void emx11_push_motion_event(Window window, int x, int y,
                             int x_root, int y_root,
                             unsigned int state) {
    Display *dpy = emx11_get_display();
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xmotion.type        = MotionNotify;
    ev.xmotion.display     = dpy;
    ev.xmotion.window      = window;
    ev.xmotion.x           = x;
    ev.xmotion.y           = y;
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
