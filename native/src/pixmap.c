/*
 * Pixmap lifecycle.
 *
 * X Pixmaps are server-side offscreen drawables. In em-x11 each Pixmap
 * is backed by an OffscreenCanvas on the JS side (see src/bindings/
 * pixmap.js -- emx11_js_pixmap_*). The C side only tracks the
 * (id, width, height, depth) triple so drawing calls and SHAPE can
 * resolve ids without round-tripping through JS.
 *
 * Drawing routing: XFillRectangle / XFillArc / XDrawLine / etc. all
 * push through emx11_js_fill_rect et al. keyed on a Drawable id. The
 * JS host recognises pixmap ids and dispatches to the pixmap's own
 * ctx; windows go through the compositor as before. The C side does
 * not need to know the difference.
 *
 * Today's scope: depth-1 bitmap pixmaps (for SHAPE masks -- xeyes).
 * Color pixmaps and XCopyArea are valid callers of the same machinery
 * but are not exercised yet.
 */

#include "emx11_internal.h"

#include <stdlib.h>

typedef struct EmxPixmap {
    Pixmap             id;
    unsigned int       width;
    unsigned int       height;
    unsigned int       depth;
    struct EmxPixmap  *next;
} EmxPixmap;

static EmxPixmap *g_pixmaps     = NULL;
static Pixmap     g_pixmap_next = 0x30000001;

static EmxPixmap *pixmap_find(Pixmap id) {
    for (EmxPixmap *p = g_pixmaps; p; p = p->next) {
        if (p->id == id) return p;
    }
    return NULL;
}

Pixmap XCreatePixmap(Display *dpy, Drawable d, unsigned int width,
                     unsigned int height, unsigned int depth) {
    (void)dpy; (void)d;
    if (width == 0 || height == 0) return None;
    EmxPixmap *p = calloc(1, sizeof(*p));
    if (!p) return None;
    p->id     = g_pixmap_next++;
    p->width  = width;
    p->height = height;
    p->depth  = depth;
    p->next   = g_pixmaps;
    g_pixmaps = p;
    emx11_js_pixmap_create(p->id, (int)width, (int)height, (int)depth);
    return p->id;
}

int XFreePixmap(Display *dpy, Pixmap pixmap) {
    (void)dpy;
    EmxPixmap **prev = &g_pixmaps;
    while (*prev && (*prev)->id != pixmap) prev = &(*prev)->next;
    if (*prev) {
        EmxPixmap *doomed = *prev;
        *prev = doomed->next;
        free(doomed);
        emx11_js_pixmap_destroy(pixmap);
    }
    return 1;
}

/* Internal accessors -------------------------------------------------------- */

Bool emx11_pixmap_exists(Pixmap id) {
    return pixmap_find(id) != NULL;
}

unsigned int emx11_pixmap_depth(Pixmap id) {
    EmxPixmap *p = pixmap_find(id);
    return p ? p->depth : 0;
}
