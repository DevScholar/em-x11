/*
 * Cursor — cursor creation and lifecycle.
 *
 * No cursor art in a browser, but we want the right CSS cursor to appear
 * when the pointer crosses into a window that XDefineCursor'd a particular
 * shape. The trick is encoding enough info in the Cursor xid that the JS
 * host can map it to a CSS keyword without a separate registration
 * round-trip:
 *
 *   font cursors  : 0x70000000 | shape  (shape == X11/cursorfont.h constant)
 *   pixmap cursors: counter-based, no shape info -- host falls back to
 *                   'default'. No client depends on per-pixmap cursor art yet.
 */

#include "emx11_internal.h"

#include <X11/Xutil.h>
#include <stdlib.h>

#define EMX11_FONT_CURSOR_TAG 0x70000000u

static XID g_cursor_next2 = 0x40000001;

Cursor XCreateFontCursor(Display *dpy, unsigned int shape) {
    (void)dpy;
    return (Cursor)(EMX11_FONT_CURSOR_TAG | (shape & 0xFFFu));
}

int XFreeCursor(Display *dpy, Cursor cursor) {
    (void)dpy; (void)cursor;
    return 1;
}

Cursor XCreatePixmapCursor(Display *dpy, Pixmap src, Pixmap mask,
                           XColor *fg, XColor *bg, unsigned int x, unsigned int y) {
    (void)dpy; (void)src; (void)mask; (void)fg; (void)bg; (void)x; (void)y;
    return (Cursor)(g_cursor_next2++);
}

Cursor XCreateGlyphCursor(Display *dpy, Font src_font, Font mask_font,
                          unsigned int src_ch, unsigned int mask_ch,
                          _Xconst XColor *fg, _Xconst XColor *bg) {
    (void)dpy; (void)src_font; (void)mask_font;
    (void)mask_ch; (void)fg; (void)bg;
    return (Cursor)(EMX11_FONT_CURSOR_TAG | (src_ch & 0xFFFu));
}

int XRecolorCursor(Display *dpy, Cursor cursor, XColor *fg, XColor *bg) {
    (void)dpy; (void)cursor; (void)fg; (void)bg;
    return 1;
}
