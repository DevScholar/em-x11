/*
 * Pixmap lifecycle.
 *
 * X Pixmaps are server-side offscreen drawables. In em-x11 each Pixmap
 * is backed by an OffscreenCanvas on the JS side (the emx11_js_pixmap_*
 * EM_JS bridges in native/src/bridges.c). The C side only tracks the
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
    /* Refcount = 1 from XCreatePixmap, +1 per window holding this pixmap
     * as background_pixmap. XFreePixmap only decrements; the JS canvas
     * is destroyed when it hits zero. Real X servers do this implicitly
     * (server-side resource ownership keeps the pixmap alive while a
     * window references it); twm relies on it -- it XFreePixmap's the
     * hilite tile immediately after XCreateWindow with CWBackPixmap,
     * expecting the server to keep the bits around. */
    unsigned int       refcount;
    struct EmxPixmap  *next;
} EmxPixmap;

static EmxPixmap *g_pixmaps     = NULL;

static EmxPixmap *pixmap_find(Pixmap id) {
    for (EmxPixmap *p = g_pixmaps; p; p = p->next) {
        if (p->id == id) return p;
    }
    return NULL;
}

Pixmap XCreatePixmap(Display *dpy, Drawable d, unsigned int width,
                     unsigned int height, unsigned int depth) {
    (void)d;
    if (width == 0 || height == 0) return None;
    EmxPixmap *p = calloc(1, sizeof(*p));
    if (!p) return None;
    /* Use the per-conn xid allocator (same as XCreateWindow) so pixmap
     * ids never collide across wasm processes. Earlier this counter was
     * a TU-local 0x30000001++ which gave every connection the SAME id
     * range -- twm's siconifyPm (id 30000001) got clobbered by xeyes's
     * first pixmap on the JS-side `pixmaps` Map, so XCopyPlane drew the
     * wrong canvas into icon-manager rows. */
    p->id     = emx11_next_xid(dpy);
    p->width  = width;
    p->height = height;
    p->depth  = depth;
    p->refcount = 1;
    p->next   = g_pixmaps;
    g_pixmaps = p;
    emx11_js_pixmap_create(p->id, (int)width, (int)height, (int)depth);
    return p->id;
}

int XFreePixmap(Display *dpy, Pixmap pixmap) {
    (void)dpy;
    EmxPixmap *p = pixmap_find(pixmap);
    if (!p) return 1;
    if (p->refcount > 1) {
        /* Some window still holds this pixmap as its background tile;
         * defer destruction. The window's set_bg_pixmap unbind path
         * (or window destroy) will release the held reference and
         * trigger the real free. */
        p->refcount--;
        return 1;
    }
    /* refcount == 1 (the XCreatePixmap-issued one): truly destroy. */
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

/* Window-side hooks for "this window is now using pm as bg" / "no
 * longer using it". window.c must call these so the canvas survives a
 * client XFreePixmap that races the bg binding. */
void emx11_pixmap_acquire(Pixmap id) {
    if (id == 0) return;
    EmxPixmap *p = pixmap_find(id);
    if (p) p->refcount++;
}

void emx11_pixmap_release(Display *dpy, Pixmap id) {
    if (id == 0) return;
    /* Mirrors XFreePixmap's decrement-or-destroy: a window letting go
     * of a pixmap whose creator already called XFreePixmap should
     * actually destroy the canvas now. */
    XFreePixmap(dpy, id);
}

/* Internal accessors -------------------------------------------------------- */

Bool emx11_pixmap_exists(Pixmap id) {
    return pixmap_find(id) != NULL;
}

unsigned int emx11_pixmap_depth(Pixmap id) {
    EmxPixmap *p = pixmap_find(id);
    return p ? p->depth : 0;
}
