/*
 * Keysym <-> keycode mapping plus XLookupString text translation.
 * Keysym table lives on the Display; entries are added on first use.
 * The browser already shifts/composes keys before they reach us, so
 * XLookupString is mostly a passthrough for ASCII / Latin-1 plus a
 * few terminal-control specials that Xt translation tables expect.
 */

#include "emx11_internal.h"

#include <X11/Xutil.h>

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
