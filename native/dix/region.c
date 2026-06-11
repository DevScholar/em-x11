/*
 * Region — bounding-box region implementation.
 *
 * Xt uses regions for expose coalescing -- XCreateRegion, then
 * XUnionRectWithRegion for every expose rect, then XClipBox to get the
 * enclosing rectangle for a single redraw pass. We model a region as a
 * single bounding rectangle plus an "empty" flag; that's enough for
 * Xt's use pattern (bounding box only) even though it loses concavity.
 *
 * Tk uses XPolygonRegion for non-rectangular clip regions (canvas polygon
 * items, rounded buttons). With our bounding-rect model we collapse the
 * polygon to its AABB -- the clip becomes rectangular, losing concavity
 * but still tight to the shape's extent.
 */

#include "em_x11_internal.h"

#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

typedef struct _XRegion {
  int x1, y1, x2, y2;
  int is_empty;
} EmxRegion;

Region XCreateRegion(void) {
  EmxRegion* r = calloc(1, sizeof(EmxRegion));
  if (r)
    r->is_empty = 1;
  return (Region)r;
}

int XDestroyRegion(Region region) {
  free(region);
  return 1;
}

int XUnionRectWithRegion(XRectangle* rect, Region src, Region dst) {
  if (!rect || !dst)
    return 0;
  EmxRegion* d = (EmxRegion*)dst;

  if (src && src != dst) {
    *d = *(EmxRegion*)src;
  }

  int rx1 = rect->x;
  int ry1 = rect->y;
  int rx2 = rect->x + rect->width;
  int ry2 = rect->y + rect->height;

  if (d->is_empty) {
    d->x1 = rx1;
    d->y1 = ry1;
    d->x2 = rx2;
    d->y2 = ry2;
    d->is_empty = 0;
  } else {
    if (rx1 < d->x1)
      d->x1 = rx1;
    if (ry1 < d->y1)
      d->y1 = ry1;
    if (rx2 > d->x2)
      d->x2 = rx2;
    if (ry2 > d->y2)
      d->y2 = ry2;
  }
  return 1;
}

int XClipBox(Region r, XRectangle* rect_return) {
  if (!rect_return)
    return 0;
  EmxRegion* er = (EmxRegion*)r;
  if (!er || er->is_empty) {
    memset(rect_return, 0, sizeof(*rect_return));
    return 1;
  }
  rect_return->x = (short)er->x1;
  rect_return->y = (short)er->y1;
  rect_return->width = (unsigned short)(er->x2 - er->x1);
  rect_return->height = (unsigned short)(er->y2 - er->y1);
  return 1;
}

Bool XEmptyRegion(Region r) {
  EmxRegion* er = (EmxRegion*)r;
  return (!er || er->is_empty) ? True : False;
}

int XIntersectRegion(Region src1, Region src2, Region dst) {
  EmxRegion* a = (EmxRegion*)src1;
  EmxRegion* b = (EmxRegion*)src2;
  EmxRegion* d = (EmxRegion*)dst;
  if (!d)
    return 0;
  if (!a || !b || a->is_empty || b->is_empty) {
    d->is_empty = 1;
    d->x1 = d->y1 = d->x2 = d->y2 = 0;
    return 1;
  }
  int x1 = a->x1 > b->x1 ? a->x1 : b->x1;
  int y1 = a->y1 > b->y1 ? a->y1 : b->y1;
  int x2 = a->x2 < b->x2 ? a->x2 : b->x2;
  int y2 = a->y2 < b->y2 ? a->y2 : b->y2;
  if (x1 >= x2 || y1 >= y2) {
    d->is_empty = 1;
    d->x1 = d->y1 = d->x2 = d->y2 = 0;
  } else {
    d->is_empty = 0;
    d->x1 = x1;
    d->y1 = y1;
    d->x2 = x2;
    d->y2 = y2;
  }
  return 1;
}

int XUnionRegion(Region src1, Region src2, Region dst) {
  EmxRegion* a = (EmxRegion*)src1;
  EmxRegion* b = (EmxRegion*)src2;
  EmxRegion* d = (EmxRegion*)dst;
  if (!d)
    return 0;
  int empty_a = (!a || a->is_empty);
  int empty_b = (!b || b->is_empty);
  if (empty_a && empty_b) {
    d->is_empty = 1;
    d->x1 = d->y1 = d->x2 = d->y2 = 0;
    return 1;
  }
  if (empty_a) {
    d->x1 = b->x1;
    d->y1 = b->y1;
    d->x2 = b->x2;
    d->y2 = b->y2;
    d->is_empty = 0;
    return 1;
  }
  if (empty_b) {
    d->x1 = a->x1;
    d->y1 = a->y1;
    d->x2 = a->x2;
    d->y2 = a->y2;
    d->is_empty = 0;
    return 1;
  }
  d->x1 = a->x1 < b->x1 ? a->x1 : b->x1;
  d->y1 = a->y1 < b->y1 ? a->y1 : b->y1;
  d->x2 = a->x2 > b->x2 ? a->x2 : b->x2;
  d->y2 = a->y2 > b->y2 ? a->y2 : b->y2;
  d->is_empty = 0;
  return 1;
}

Bool XPointInRegion(Region r, int x, int y) {
  EmxRegion* er = (EmxRegion*)r;
  if (!er || er->is_empty)
    return False;
  return (x >= er->x1 && x < er->x2 && y >= er->y1 && y < er->y2) ? True
                                                                  : False;
}

Region XPolygonRegion(XPoint* points, int n, int fill_rule) {
  (void)fill_rule;
  EmxRegion* r = calloc(1, sizeof(EmxRegion));
  if (!r)
    return NULL;
  if (!points || n <= 0) {
    r->is_empty = 1;
    return (Region)r;
  }
  int x1 = points[0].x, y1 = points[0].y;
  int x2 = x1, y2 = y1;
  for (int i = 1; i < n; i++) {
    if (points[i].x < x1)
      x1 = points[i].x;
    if (points[i].y < y1)
      y1 = points[i].y;
    if (points[i].x > x2)
      x2 = points[i].x;
    if (points[i].y > y2)
      y2 = points[i].y;
  }
  r->x1 = x1;
  r->y1 = y1;
  r->x2 = x2 + 1;
  r->y2 = y2 + 1;
  r->is_empty = 0;
  return (Region)r;
}

int XRectInRegion(Region r, int x, int y, unsigned int w, unsigned int h) {
  EmxRegion* er = (EmxRegion*)r;
  if (!er || er->is_empty)
    return RectangleOut;
  int rx2 = x + (int)w;
  int ry2 = y + (int)h;
  if (rx2 <= er->x1 || x >= er->x2 || ry2 <= er->y1 || y >= er->y2)
    return RectangleOut;
  if (x >= er->x1 && y >= er->y1 && rx2 <= er->x2 && ry2 <= er->y2)
    return RectangleIn;
  return RectanglePart;
}

int XSubtractRegion(Region src1, Region src2, Region dst) {
  (void)src2;
  EmxRegion* a = (EmxRegion*)src1;
  EmxRegion* d = (EmxRegion*)dst;
  if (!d)
    return 0;
  if (!a || a->is_empty) {
    d->is_empty = 1;
    d->x1 = d->y1 = d->x2 = d->y2 = 0;
    return 1;
  }
  d->x1 = a->x1;
  d->y1 = a->y1;
  d->x2 = a->x2;
  d->y2 = a->y2;
  d->is_empty = a->is_empty;
  return 1;
}
