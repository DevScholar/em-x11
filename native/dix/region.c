/*
 * Region — full y-x-banded region implementation.
 *
 * Ported from libX11 Region.c to match the real REGION struct layout,
 * which is binary-compatible with Motif's XmRegion. The previous
 * bounding-box simplification broke Motif's _XmRegionDrawShadow() which
 * relies on the region having properly banded rectangles.
 */

#include "em_x11_internal.h"

#include <X11/Xregion.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

/* Xregion.h provides the REGION struct, BOX, and macros */

/*
 * Internal helpers — ported from libX11 poly.h / reallocarray
 */

static inline void* xmalloc(size_t n) { return malloc(n); }
static inline void xfree(void* p) { free(p); }
static inline void* xrealloc(void* p, size_t n) { return realloc(p, n); }
static inline void* xreallocarray(void* p, size_t n, size_t s) {
  return realloc(p, n * s);
}
static inline void* xmallocarray(size_t n, size_t s) { return malloc(n * s); }

/* Re-declare macros from Xregion.h in terms of our allocators */
#undef MEMCHECK
#define MEMCHECK(reg, rect, firstrect)                                         \
  {                                                                            \
    if ((reg)->numRects >= ((reg)->size - 1)) {                                \
      BoxPtr tmpRect =                                                         \
        xrealloc((firstrect), (2 * sizeof(BOX) * ((reg)->size)));              \
      if (tmpRect == NULL)                                                     \
        return (0);                                                            \
      (firstrect) = tmpRect;                                                   \
      (reg)->size *= 2;                                                        \
      (rect) = &(firstrect)[(reg)->numRects];                                  \
    }                                                                          \
  }

/*
 *  Function prototypes (static)
 */
typedef int (*overlapProcp)(Region pReg,
                            BoxPtr r1,
                            BoxPtr r1End,
                            BoxPtr r2,
                            BoxPtr r2End,
                            short y1,
                            short y2);

typedef int (*nonOverlapProcp)(
  Region pReg, BoxPtr r, BoxPtr rEnd, short y1, short y2);

static void miRegionOp(
  Region newReg,
  Region reg1,
  Region reg2,
  int (*overlapFunc)(Region, BoxPtr, BoxPtr, BoxPtr, BoxPtr, short, short),
  int (*nonOverlap1Func)(Region, BoxPtr, BoxPtr, short, short),
  int (*nonOverlap2Func)(Region, BoxPtr, BoxPtr, short, short));

static int miUnionNonO(Region pReg, BoxPtr r, BoxPtr rEnd, short y1, short y2);
static int miUnionO(Region pReg,
                    BoxPtr r1,
                    BoxPtr r1End,
                    BoxPtr r2,
                    BoxPtr r2End,
                    short y1,
                    short y2);
static int miIntersectO(Region pReg,
                        BoxPtr r1,
                        BoxPtr r1End,
                        BoxPtr r2,
                        BoxPtr r2End,
                        short y1,
                        short y2);
static int
miIntersectNonO(Region pReg, BoxPtr r, BoxPtr rEnd, short y1, short y2);
static int
miSubtractNonO1(Region pReg, BoxPtr r, BoxPtr rEnd, short y1, short y2);
static int miSubtractO(Region pReg,
                       BoxPtr r1,
                       BoxPtr r1End,
                       BoxPtr r2,
                       BoxPtr r2End,
                       short y1,
                       short y2);
static int miCoalesce(Region pReg, int prevStart, int curStart);
static int miRegionCopy(Region dstrgn, Region rgn);
static void miSetExtents(Region pReg);

/*======================================================================
 *        Region Create / Destroy / Clip
 *====================================================================*/

Region XCreateRegion(void) {
  Region temp;
  if (!(temp = malloc(sizeof(REGION))))
    return (Region)NULL;
  if (!(temp->rects = malloc(sizeof(BOX)))) {
    free(temp);
    return (Region)NULL;
  }
  temp->numRects = 0;
  temp->extents.x1 = 0;
  temp->extents.y1 = 0;
  temp->extents.x2 = 0;
  temp->extents.y2 = 0;
  temp->size = 1;
  return temp;
}

int XDestroyRegion(Region r) {
  free(r->rects);
  free(r);
  return 1;
}

int XClipBox(Region r, XRectangle* rect) {
  rect->x = r->extents.x1;
  rect->y = r->extents.y1;
  rect->width = r->extents.x2 - r->extents.x1;
  rect->height = r->extents.y2 - r->extents.y1;
  return 1;
}

int XEmptyRegion(Region r) { return (r->numRects == 0) ? TRUE : FALSE; }

/*======================================================================
 *        Region Union
 *====================================================================*/

int XUnionRectWithRegion(XRectangle* rect, Region source, Region dest) {
  REGION region;

  if (!rect->width || !rect->height)
    return 0;
  region.rects = &region.extents;
  region.numRects = 1;
  region.extents.x1 = rect->x;
  region.extents.y1 = rect->y;
  region.extents.x2 = rect->x + rect->width;
  region.extents.y2 = rect->y + rect->height;
  region.size = 1;

  return XUnionRegion(&region, source, dest);
}

/*-
 * miSetExtents — Reset the extents of a region.
 */
static void miSetExtents(Region pReg) {
  BoxPtr pBox, pBoxEnd, pExtents;

  if (pReg->numRects == 0) {
    pReg->extents.x1 = 0;
    pReg->extents.y1 = 0;
    pReg->extents.x2 = 0;
    pReg->extents.y2 = 0;
    return;
  }

  pExtents = &pReg->extents;
  pBox = pReg->rects;
  pBoxEnd = &pBox[pReg->numRects - 1];

  pExtents->x1 = pBox->x1;
  pExtents->y1 = pBox->y1;
  pExtents->x2 = pBoxEnd->x2;
  pExtents->y2 = pBoxEnd->y2;

  while (pBox <= pBoxEnd) {
    if (pBox->x1 < pExtents->x1)
      pExtents->x1 = pBox->x1;
    if (pBox->x2 > pExtents->x2)
      pExtents->x2 = pBox->x2;
    pBox++;
  }
}

static int miRegionCopy(Region dstrgn, Region rgn) {
  if (dstrgn != rgn) {
    if (dstrgn->size < rgn->numRects) {
      if (dstrgn->rects) {
        BOX* prevRects = dstrgn->rects;
        dstrgn->rects =
          xreallocarray(dstrgn->rects, rgn->numRects, sizeof(BOX));
        if (!dstrgn->rects) {
          free(prevRects);
          dstrgn->size = 0;
          return 0;
        }
      }
      dstrgn->size = rgn->numRects;
    }
    dstrgn->numRects = rgn->numRects;
    dstrgn->extents.x1 = rgn->extents.x1;
    dstrgn->extents.y1 = rgn->extents.y1;
    dstrgn->extents.x2 = rgn->extents.x2;
    dstrgn->extents.y2 = rgn->extents.y2;
    memcpy(dstrgn->rects, rgn->rects, rgn->numRects * sizeof(BOX));
  }
  return 1;
}

/*-
 * miCoalesce — Merge boxes in current band with previous band.
 */
static int miCoalesce(Region pReg, int prevStart, int curStart) {
  BoxPtr pPrevBox, pCurBox, pRegEnd;
  int curNumRects, prevNumRects;
  short bandY1;

  pRegEnd = &pReg->rects[pReg->numRects];
  pPrevBox = &pReg->rects[prevStart];
  prevNumRects = curStart - prevStart;

  pCurBox = &pReg->rects[curStart];
  bandY1 = pCurBox->y1;
  for (curNumRects = 0; (pCurBox != pRegEnd) && (pCurBox->y1 == bandY1);
       curNumRects++) {
    pCurBox++;
  }

  if (pCurBox != pRegEnd) {
    pRegEnd--;
    while (pRegEnd[-1].y1 == pRegEnd->y1)
      pRegEnd--;
    curStart = pRegEnd - pReg->rects;
    pRegEnd = pReg->rects + pReg->numRects;
  }

  if ((curNumRects == prevNumRects) && (curNumRects != 0)) {
    pCurBox -= curNumRects;
    if (pPrevBox->y2 == pCurBox->y1) {
      do {
        if ((pPrevBox->x1 != pCurBox->x1) || (pPrevBox->x2 != pCurBox->x2))
          return curStart;
        pPrevBox++;
        pCurBox++;
        prevNumRects -= 1;
      } while (prevNumRects != 0);

      pReg->numRects -= curNumRects;
      pCurBox -= curNumRects;
      pPrevBox -= curNumRects;

      do {
        pPrevBox->y2 = pCurBox->y2;
        pPrevBox++;
        pCurBox++;
        curNumRects -= 1;
      } while (curNumRects != 0);

      if (pCurBox == pRegEnd) {
        curStart = prevStart;
      } else {
        do {
          *pPrevBox++ = *pCurBox++;
        } while (pCurBox != pRegEnd);
      }
    }
  }
  return curStart;
}

/*-
 * miRegionOp — Apply an operation to two regions.
 */
static void miRegionOp(
  Region newReg,
  Region reg1,
  Region reg2,
  int (*overlapFunc)(Region, BoxPtr, BoxPtr, BoxPtr, BoxPtr, short, short),
  int (*nonOverlap1Func)(Region, BoxPtr, BoxPtr, short, short),
  int (*nonOverlap2Func)(Region, BoxPtr, BoxPtr, short, short)) {
  BoxPtr r1, r2, r1End, r2End;
  short ybot, ytop;
  BoxPtr oldRects;
  int prevBand, curBand;
  BoxPtr r1BandEnd, r2BandEnd;
  short top, bot;

  r1 = reg1->rects;
  r2 = reg2->rects;
  r1End = r1 + reg1->numRects;
  r2End = r2 + reg2->numRects;

  oldRects = newReg->rects;
  EMPTY_REGION(newReg);

  newReg->size =
    (reg1->numRects > reg2->numRects ? reg1->numRects : reg2->numRects) * 2;
  if (newReg->size < 2)
    newReg->size = 2;

  if (!(newReg->rects = xmallocarray(newReg->size, sizeof(BOX)))) {
    newReg->size = 0;
    return;
  }

  if (reg1->extents.y1 < reg2->extents.y1)
    ybot = reg1->extents.y1;
  else
    ybot = reg2->extents.y1;

  prevBand = 0;

  do {
    curBand = newReg->numRects;

    r1BandEnd = r1;
    while ((r1BandEnd != r1End) && (r1BandEnd->y1 == r1->y1))
      r1BandEnd++;

    r2BandEnd = r2;
    while ((r2BandEnd != r2End) && (r2BandEnd->y1 == r2->y1))
      r2BandEnd++;

    if (r1->y1 < r2->y1) {
      top = r1->y1 > ybot ? r1->y1 : ybot;
      bot = r1->y2 < r2->y1 ? r1->y2 : r2->y1;
      if ((top != bot) && (nonOverlap1Func != NULL))
        (*nonOverlap1Func)(newReg, r1, r1BandEnd, top, bot);
      ytop = r2->y1;
    } else if (r2->y1 < r1->y1) {
      top = r2->y1 > ybot ? r2->y1 : ybot;
      bot = r2->y2 < r1->y1 ? r2->y2 : r1->y1;
      if ((top != bot) && (nonOverlap2Func != NULL))
        (*nonOverlap2Func)(newReg, r2, r2BandEnd, top, bot);
      ytop = r1->y1;
    } else {
      ytop = r1->y1;
    }

    if (newReg->numRects != curBand)
      prevBand = miCoalesce(newReg, prevBand, curBand);

    ybot = r1->y2 < r2->y2 ? r1->y2 : r2->y2;
    curBand = newReg->numRects;
    if (ybot > ytop)
      (*overlapFunc)(newReg, r1, r1BandEnd, r2, r2BandEnd, ytop, ybot);

    if (newReg->numRects != curBand)
      prevBand = miCoalesce(newReg, prevBand, curBand);

    if (r1->y2 == ybot)
      r1 = r1BandEnd;
    if (r2->y2 == ybot)
      r2 = r2BandEnd;
  } while ((r1 != r1End) && (r2 != r2End));

  curBand = newReg->numRects;
  if (r1 != r1End) {
    if (nonOverlap1Func != NULL) {
      do {
        r1BandEnd = r1;
        while ((r1BandEnd < r1End) && (r1BandEnd->y1 == r1->y1))
          r1BandEnd++;
        (*nonOverlap1Func)(
          newReg, r1, r1BandEnd, r1->y1 > ybot ? r1->y1 : ybot, r1->y2);
        r1 = r1BandEnd;
      } while (r1 != r1End);
    }
  } else if ((r2 != r2End) && (nonOverlap2Func != NULL)) {
    do {
      r2BandEnd = r2;
      while ((r2BandEnd < r2End) && (r2BandEnd->y1 == r2->y1))
        r2BandEnd++;
      (*nonOverlap2Func)(
        newReg, r2, r2BandEnd, r2->y1 > ybot ? r2->y1 : ybot, r2->y2);
      r2 = r2BandEnd;
    } while (r2 != r2End);
  }

  if (newReg->numRects != curBand)
    miCoalesce(newReg, prevBand, curBand);

  if (newReg->numRects < (newReg->size >> 1)) {
    if (REGION_NOT_EMPTY(newReg)) {
      BoxPtr prev_rects = newReg->rects;
      newReg->rects =
        xreallocarray(newReg->rects, newReg->numRects, sizeof(BOX));
      if (!newReg->rects)
        newReg->rects = prev_rects;
      else
        newReg->size = newReg->numRects;
    } else {
      newReg->size = 1;
    }
  }

  free(oldRects);
}

/*
 * Union non-overlap — copy rectangles to the new region.
 */
static int miUnionNonO(Region pReg, BoxPtr r, BoxPtr rEnd, short y1, short y2) {
  BoxPtr pNextRect = &pReg->rects[pReg->numRects];
  while (r != rEnd) {
    MEMCHECK(pReg, pNextRect, pReg->rects);
    pNextRect->x1 = r->x1;
    pNextRect->y1 = y1;
    pNextRect->x2 = r->x2;
    pNextRect->y2 = y2;
    pReg->numRects += 1;
    pNextRect++;
    r++;
  }
  return 0;
}

/*
 * Union overlap — merge overlapping bands.
 */
static int miUnionO(Region pReg,
                    BoxPtr r1,
                    BoxPtr r1End,
                    BoxPtr r2,
                    BoxPtr r2End,
                    short y1,
                    short y2) {
  BoxPtr pNextRect = &pReg->rects[pReg->numRects];

#define MERGERECT(r)                                                           \
  if ((pReg->numRects != 0) && (pNextRect[-1].y1 == y1) &&                     \
      (pNextRect[-1].y2 == y2) && (pNextRect[-1].x2 >= (r)->x1)) {             \
    if (pNextRect[-1].x2 < (r)->x2)                                            \
      pNextRect[-1].x2 = (r)->x2;                                              \
  } else {                                                                     \
    MEMCHECK(pReg, pNextRect, pReg->rects);                                    \
    pNextRect->y1 = y1;                                                        \
    pNextRect->y2 = y2;                                                        \
    pNextRect->x1 = (r)->x1;                                                   \
    pNextRect->x2 = (r)->x2;                                                   \
    pReg->numRects += 1;                                                       \
    pNextRect += 1;                                                            \
  }                                                                            \
  (r)++;

  while ((r1 != r1End) && (r2 != r2End)) {
    if (r1->x1 < r2->x1) {
      MERGERECT(r1);
    } else {
      MERGERECT(r2);
    }
  }
  if (r1 != r1End) {
    do {
      MERGERECT(r1);
    } while (r1 != r1End);
  } else
    while (r2 != r2End) {
      MERGERECT(r2);
    }
#undef MERGERECT
  return 0;
}

int XUnionRegion(Region reg1, Region reg2, Region newReg) {
  if ((reg1 == reg2) || (!(reg1->numRects))) {
    if (newReg != reg2)
      return miRegionCopy(newReg, reg2);
    return 1;
  }
  if (!(reg2->numRects)) {
    if (newReg != reg1)
      return miRegionCopy(newReg, reg1);
    return 1;
  }
  if ((reg1->numRects == 1) && (reg1->extents.x1 <= reg2->extents.x1) &&
      (reg1->extents.y1 <= reg2->extents.y1) &&
      (reg1->extents.x2 >= reg2->extents.x2) &&
      (reg1->extents.y2 >= reg2->extents.y2)) {
    if (newReg != reg1)
      return miRegionCopy(newReg, reg1);
    return 1;
  }
  if ((reg2->numRects == 1) && (reg2->extents.x1 <= reg1->extents.x1) &&
      (reg2->extents.y1 <= reg1->extents.y1) &&
      (reg2->extents.x2 >= reg1->extents.x2) &&
      (reg2->extents.y2 >= reg1->extents.y2)) {
    if (newReg != reg2)
      return miRegionCopy(newReg, reg2);
    return 1;
  }

  miRegionOp(newReg, reg1, reg2, miUnionO, miUnionNonO, miUnionNonO);

  newReg->extents.x1 =
    reg1->extents.x1 < reg2->extents.x1 ? reg1->extents.x1 : reg2->extents.x1;
  newReg->extents.y1 =
    reg1->extents.y1 < reg2->extents.y1 ? reg1->extents.y1 : reg2->extents.y1;
  newReg->extents.x2 =
    reg1->extents.x2 > reg2->extents.x2 ? reg1->extents.x2 : reg2->extents.x2;
  newReg->extents.y2 =
    reg1->extents.y2 > reg2->extents.y2 ? reg1->extents.y2 : reg2->extents.y2;
  return 1;
}

/*======================================================================
 *        Region Intersection
 *====================================================================*/

static int miIntersectO(Region pReg,
                        BoxPtr r1,
                        BoxPtr r1End,
                        BoxPtr r2,
                        BoxPtr r2End,
                        short y1,
                        short y2) {
  short x1, x2;
  BoxPtr pNextRect = &pReg->rects[pReg->numRects];
  (void)y1;
  (void)y2;

  while ((r1 != r1End) && (r2 != r2End)) {
    x1 = r1->x1 > r2->x1 ? r1->x1 : r2->x1;
    x2 = r1->x2 < r2->x2 ? r1->x2 : r2->x2;
    if (x1 < x2) {
      MEMCHECK(pReg, pNextRect, pReg->rects);
      pNextRect->x1 = x1;
      pNextRect->y1 = y1;
      pNextRect->x2 = x2;
      pNextRect->y2 = y2;
      pReg->numRects += 1;
      pNextRect++;
    }
    if (r1->x2 < r2->x2)
      r1++;
    else if (r2->x2 < r1->x2)
      r2++;
    else {
      r1++;
      r2++;
    }
  }
  return 0;
}

int XIntersectRegion(Region reg1, Region reg2, Region newReg) {
  if ((!(reg1->numRects)) || (!(reg2->numRects)) ||
      (!EXTENTCHECK(&reg1->extents, &reg2->extents)))
    newReg->numRects = 0;
  else
    miRegionOp(newReg, reg1, reg2, miIntersectO, NULL, NULL);
  miSetExtents(newReg);
  return 1;
}

/*======================================================================
 *        Region Subtraction
 *====================================================================*/

static int
miSubtractNonO1(Region pReg, BoxPtr r, BoxPtr rEnd, short y1, short y2) {
  BoxPtr pNextRect = &pReg->rects[pReg->numRects];
  while (r != rEnd) {
    MEMCHECK(pReg, pNextRect, pReg->rects);
    pNextRect->x1 = r->x1;
    pNextRect->y1 = y1;
    pNextRect->x2 = r->x2;
    pNextRect->y2 = y2;
    pReg->numRects += 1;
    pNextRect++;
    r++;
  }
  return 0;
}

static int miSubtractO(Region pReg,
                       BoxPtr r1,
                       BoxPtr r1End,
                       BoxPtr r2,
                       BoxPtr r2End,
                       short y1,
                       short y2) {
  BoxPtr pNextRect;
  short x1;

  pNextRect = &pReg->rects[pReg->numRects];
  x1 = r1->x1;

  while ((r1 != r1End) && (r2 != r2End)) {
    if (r2->x2 <= x1) {
      r2++;
    } else if (r2->x1 <= x1) {
      x1 = r2->x2;
      if (x1 >= r1->x2) {
        r1++;
        if (r1 != r1End)
          x1 = r1->x1;
      }
      r2++;
    } else if (r2->x1 < r1->x2) {
      if (r2->x1 > x1) {
        MEMCHECK(pReg, pNextRect, pReg->rects);
        pNextRect->x1 = x1;
        pNextRect->y1 = y1;
        pNextRect->x2 = r2->x1;
        pNextRect->y2 = y2;
        pReg->numRects += 1;
        pNextRect++;
      }
      x1 = r2->x2;
      if (x1 >= r1->x2) {
        r1++;
        if (r1 != r1End)
          x1 = r1->x1;
      }
      r2++;
    } else {
      if (x1 < r1->x2) {
        MEMCHECK(pReg, pNextRect, pReg->rects);
        pNextRect->x1 = x1;
        pNextRect->y1 = y1;
        pNextRect->x2 = r1->x2;
        pNextRect->y2 = y2;
        pReg->numRects += 1;
        pNextRect++;
      }
      r1++;
      if (r1 != r1End)
        x1 = r1->x1;
    }
  }
  while (r1 != r1End) {
    MEMCHECK(pReg, pNextRect, pReg->rects);
    pNextRect->x1 = x1;
    pNextRect->y1 = y1;
    pNextRect->x2 = r1->x2;
    pNextRect->y2 = y2;
    pReg->numRects += 1;
    pNextRect++;
    r1++;
    if (r1 != r1End)
      x1 = r1->x1;
  }
  return 0;
}

int XSubtractRegion(Region regM, Region regS, Region regD) {
  if ((!(regM->numRects)) || (!(regS->numRects)) ||
      (!EXTENTCHECK(&regM->extents, &regS->extents)))
    return miRegionCopy(regD, regM);
  miRegionOp(regD, regM, regS, miSubtractO, miSubtractNonO1, NULL);
  miSetExtents(regD);
  return 1;
}

/*======================================================================
 *        Point / Rect in Region
 *====================================================================*/

Bool XPointInRegion(Region r, int x, int y) {
  int i;
  if (!r || r->numRects == 0)
    return False;
  if (!INBOX(r->extents, x, y))
    return False;
  for (i = 0; i < r->numRects; i++) {
    if (INBOX(r->rects[i], x, y))
      return True;
  }
  return False;
}

int XRectInRegion(Region r, int x, int y, unsigned int w, unsigned int h) {
  /* Simplified: just check if rect overlaps extents */
  BoxRec box;
  box.x1 = x;
  box.y1 = y;
  box.x2 = x + (int)w;
  box.y2 = y + (int)h;
  if (!r || r->numRects == 0)
    return RectangleOut;
  if (!EXTENTCHECK(&r->extents, &box))
    return RectangleOut;
  /* Check if fully contained in extents */
  if (x >= r->extents.x1 && y >= r->extents.y1 && x + (int)w <= r->extents.x2 &&
      y + (int)h <= r->extents.y2) {
    /* Check each rect */
    int i;
    for (i = 0; i < r->numRects; i++) {
      BoxPtr b = &r->rects[i];
      if (x >= b->x1 && y >= b->y1 && x + (int)w <= b->x2 &&
          y + (int)h <= b->y2)
        return RectangleIn;
    }
  }
  return RectanglePart;
}

/*======================================================================
 *        Polygon Region
 *====================================================================*/

Region XPolygonRegion(XPoint* points, int n, int fill_rule) {
  (void)fill_rule;
  Region r = XCreateRegion();
  if (!r)
    return NULL;
  if (!points || n <= 0)
    return r;

  /* Compute bounding box and use that as a single-rect region */
  int x1 = points[0].x, y1 = points[0].y;
  int x2 = x1, y2 = y1;
  int i;
  for (i = 1; i < n; i++) {
    if (points[i].x < x1)
      x1 = points[i].x;
    if (points[i].y < y1)
      y1 = points[i].y;
    if (points[i].x > x2)
      x2 = points[i].x;
    if (points[i].y > y2)
      y2 = points[i].y;
  }
  if (x2 > x1 && y2 > y1) {
    XRectangle rect;
    rect.x = x1;
    rect.y = y1;
    rect.width = x2 - x1 + 1;
    rect.height = y2 - y1 + 1;
    XUnionRectWithRegion(&rect, r, r);
  }
  return r;
}

/*======================================================================
 *        Offset / Shrink
 *====================================================================*/

int XOffsetRegion(Region pRegion, int x, int y) {
  int nbox = pRegion->numRects;
  BoxPtr pbox = pRegion->rects;
  while (nbox--) {
    pbox->x1 += x;
    pbox->x2 += x;
    pbox->y1 += y;
    pbox->y2 += y;
    pbox++;
  }
  pRegion->extents.x1 += x;
  pRegion->extents.x2 += x;
  pRegion->extents.y1 += y;
  pRegion->extents.y2 += y;
  return 1;
}

/*
 * Compress — helper for XShrinkRegion.
 */
#define ZOpRegion(a, b, c)                                                     \
  if (grow)                                                                    \
    XUnionRegion(a, b, c);                                                     \
  else                                                                         \
    XIntersectRegion(a, b, c)
#define ZShiftRegion(a, b)                                                     \
  if (xdir)                                                                    \
    XOffsetRegion(a, b, 0);                                                    \
  else                                                                         \
    XOffsetRegion(a, 0, b)
#define ZCopyRegion(a, b) XUnionRegion(a, a, b)

static void
Compress(Region r, Region s, Region t, unsigned dx, int xdir, int grow) {
  unsigned shift = 1;
  ZCopyRegion(r, s);
  while (dx) {
    if (dx & shift) {
      ZShiftRegion(r, -(int)shift);
      ZOpRegion(r, s, r);
      dx -= shift;
      if (!dx)
        break;
    }
    ZCopyRegion(s, t);
    ZShiftRegion(s, -(int)shift);
    ZOpRegion(s, t, s);
    shift <<= 1;
  }
}

#undef ZOpRegion
#undef ZShiftRegion
#undef ZCopyRegion

int XShrinkRegion(Region r, int dx, int dy) {
  Region s, t;
  int grow;

  if (!dx && !dy)
    return 0;
  if (!(s = XCreateRegion()))
    return 0;
  if (!(t = XCreateRegion())) {
    XDestroyRegion(s);
    return 0;
  }

  if ((grow = (dx < 0)))
    dx = -dx;
  if (dx)
    Compress(r, s, t, (unsigned)2 * dx, TRUE, grow);

  if ((grow = (dy < 0)))
    dy = -dy;
  if (dy)
    Compress(r, s, t, (unsigned)2 * dy, FALSE, grow);

  XOffsetRegion(r, dx, dy);
  XDestroyRegion(s);
  XDestroyRegion(t);
  return 0;
}

/*======================================================================
 *        XSetRegion (GC clip)
 *====================================================================*/

int XSetRegion(Display* dpy, GC gc, Region r) {
  /* TODO: relay region rects to canvas-backend clip.
   * For now this is a no-op — the old bounding-box GC clip
   * handling was also no-op. */
  (void)dpy;
  (void)gc;
  (void)r;
  return 1;
}

/*======================================================================
 *        XEqualRegion
 *====================================================================*/

int XEqualRegion(Region r1, Region r2) {
  int i;
  if (r1->numRects != r2->numRects)
    return FALSE;
  if (r1->numRects == 0)
    return TRUE;
  if (r1->extents.x1 != r2->extents.x1)
    return FALSE;
  if (r1->extents.x2 != r2->extents.x2)
    return FALSE;
  if (r1->extents.y1 != r2->extents.y1)
    return FALSE;
  if (r1->extents.y2 != r2->extents.y2)
    return FALSE;
  for (i = 0; i < r1->numRects; i++) {
    if (r1->rects[i].x1 != r2->rects[i].x1)
      return FALSE;
    if (r1->rects[i].x2 != r2->rects[i].x2)
      return FALSE;
    if (r1->rects[i].y1 != r2->rects[i].y1)
      return FALSE;
    if (r1->rects[i].y2 != r2->rects[i].y2)
      return FALSE;
  }
  return TRUE;
}
