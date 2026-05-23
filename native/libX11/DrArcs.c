/*
 * XDrawArcs — outline multiple arcs.
 * Upstream: libX11/src/DrArcs.c
 */
#include "DrawingPriv.h"

int XDrawArcs(Display *dpy, Drawable d, GC gc, XArc *arcs, int narcs) {
    if (!arcs) return 0;
    for (int i = 0; i < narcs; i++) {
        XDrawArc(dpy, d, gc, arcs[i].x, arcs[i].y,
                 arcs[i].width, arcs[i].height,
                 arcs[i].angle1, arcs[i].angle2);
    }
    return 1;
}
