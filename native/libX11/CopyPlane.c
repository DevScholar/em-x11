/*
 * XCopyPlane — copy a single bit plane between drawables.
 * Upstream: libX11/src/CopyPlane.c
 */
#include "emx11_internal.h"

int XCopyPlane(Display *dpy, Drawable src, Drawable dst, GC gc,
               int src_x, int src_y, unsigned int width, unsigned int height,
               int dst_x, int dst_y, unsigned long plane) {
    (void)dpy;
    if (!gc || width == 0 || height == 0) return 1;
    emx11_js_copy_plane(src, dst, src_x, src_y, width, height,
                        dst_x, dst_y, plane,
                        gc->foreground, gc->background, 1);
    return 1;
}
