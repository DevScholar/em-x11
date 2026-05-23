/*
 * XFillRectangles — fill multiple rectangles.
 * Upstream: libX11/src/FillRcts.c
 */
#include "DrawingPriv.h"

int XFillRectangles(Display *dpy, Drawable d, GC gc,
                    XRectangle *rectangles, int nrectangles) {
    if (!rectangles) return 0;
    for (int i = 0; i < nrectangles; i++) {
        XFillRectangle(dpy, d, gc, rectangles[i].x, rectangles[i].y,
                       rectangles[i].width, rectangles[i].height);
    }
    return 1;
}
