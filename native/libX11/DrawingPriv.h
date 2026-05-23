/*
 * DrawingPriv.h — internal helpers shared across libX11 drawing files.
 *
 * In upstream libX11 these live in XlibInt.c / Xlibint.h. em-x11 keeps
 * them here so each libX11/src/ .c analogue is self-contained after
 * including this header.
 */
#ifndef EMX11_DRAWING_PRIV_H
#define EMX11_DRAWING_PRIV_H

#include "emx11_internal.h"
#include <stdlib.h>

static inline bool gc_draw_disabled(GC gc) {
    return gc && gc->function != GXcopy;
}

/* Serialize XPoint[] (with mode resolution) into a flat int[] and push. */
static inline int *flatten_points(XPoint *points, int npoints, int mode,
                                  int *out_count) {
    if (npoints <= 0 || !points) {
        *out_count = 0;
        return NULL;
    }
    int *flat = malloc(sizeof(int) * 2 * (size_t)npoints);
    if (!flat) {
        *out_count = 0;
        return NULL;
    }
    int cx = 0, cy = 0;
    for (int i = 0; i < npoints; i++) {
        if (mode == CoordModePrevious && i > 0) {
            cx += points[i].x;
            cy += points[i].y;
        } else {
            cx = points[i].x;
            cy = points[i].y;
        }
        flat[i * 2 + 0] = cx;
        flat[i * 2 + 1] = cy;
    }
    *out_count = npoints;
    return flat;
}

#endif
