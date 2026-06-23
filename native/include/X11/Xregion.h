/*
 * Xregion.h — Region structure and macros
 *
 * This matches the real Xlib REGION struct, which is binary-compatible
 * with Motif's XmRegion (see XmP.h).  Motif code casts Region→XmRegion
 * freely, so the layout must match exactly.
 *
 * Ported from libX11-1.8.13/include/X11/Xregion.h
 */

#ifndef _X11_XREGION_H_
#define _X11_XREGION_H_

typedef struct {
    short x1, x2, y1, y2;
} Box, BOX, BoxRec, *BoxPtr;

typedef struct {
    short x, y, width, height;
} RECTANGLE, RectangleRec, *RectanglePtr;

#define MAXSHORT 32767
#define MINSHORT -MAXSHORT
#ifndef MAX
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

#define TRUE 1
#define FALSE 0

/*
 *   clip region — must match Motif's XmRegionRec layout:
 *     long size, long numRects, XmRegionBox *rects, XmRegionBox extents;
 */
typedef struct _XRegion {
    long size;
    long numRects;
    BOX *rects;
    BOX extents;
} REGION;

/*  1 if two BOXs overlap.  0 if they do not overlap.
 *  x2 and y2 are not in the region.
 */
#define EXTENTCHECK(r1, r2) \
    ((r1)->x2 > (r2)->x1 && \
     (r1)->x1 < (r2)->x2 && \
     (r1)->y2 > (r2)->y1 && \
     (r1)->y1 < (r2)->y2)

/*  update region extents */
#define EXTENTS(r,idRect) {                             \
    if ((r)->x1 < (idRect)->extents.x1)                  \
      (idRect)->extents.x1 = (r)->x1;                    \
    if ((r)->y1 < (idRect)->extents.y1)                  \
      (idRect)->extents.y1 = (r)->y1;                    \
    if ((r)->x2 > (idRect)->extents.x2)                  \
      (idRect)->extents.x2 = (r)->x2;                    \
    if ((r)->y2 > (idRect)->extents.y2)                  \
      (idRect)->extents.y2 = (r)->y2;                    \
}

/*
 * Check to see if there is enough memory in the present region.
 * NOTE: re-declared in region.c to use the local allocator.
 */
#define MEMCHECK(reg, rect, firstrect) {                         \
    if ((reg)->numRects >= ((reg)->size - 1)) {                   \
      BoxPtr tmpRect = Xrealloc((firstrect),                     \
                               (2 * sizeof(BOX) * ((reg)->size))); \
      if (tmpRect == NULL)                                        \
        return(0);                                                \
      (firstrect) = tmpRect;                                      \
      (reg)->size *= 2;                                           \
      (rect) = &(firstrect)[(reg)->numRects];                     \
    }                                                             \
}

/*  this routine checks to see if the previous rectangle is the same
 *  or subsumes the new rectangle to add.
 */
#define CHECK_PREVIOUS(Reg, R, Rx1, Ry1, Rx2, Ry2) \
    (!(((Reg)->numRects > 0) &&                     \
       ((R-1)->y1 == (Ry1)) &&                       \
       ((R-1)->y2 == (Ry2)) &&                       \
       ((R-1)->x1 <= (Rx1)) &&                       \
       ((R-1)->x2 >= (Rx2))))

/*  add a rectangle to the given Region */
#define ADDRECT(reg, r, rx1, ry1, rx2, ry2) {       \
    if (((rx1) < (rx2)) && ((ry1) < (ry2)) &&        \
        CHECK_PREVIOUS((reg), (r), (rx1), (ry1), (rx2), (ry2))) { \
      (r)->x1 = (rx1); (r)->y1 = (ry1);              \
      (r)->x2 = (rx2); (r)->y2 = (ry2);              \
      EXTENTS((r), (reg));                            \
      (reg)->numRects++; (r)++;                       \
    }                                                 \
}

/*  add a rectangle to the given Region (no extent update) */
#define ADDRECTNOX(reg, r, rx1, ry1, rx2, ry2) {     \
    if ((rx1 < rx2) && (ry1 < ry2) &&                  \
        CHECK_PREVIOUS((reg), (r), (rx1), (ry1), (rx2), (ry2))) { \
      (r)->x1 = (rx1); (r)->y1 = (ry1);               \
      (r)->x2 = (rx2); (r)->y2 = (ry2);               \
      (reg)->numRects++; (r)++;                        \
    }                                                  \
}

#define EMPTY_REGION(pReg)  ((pReg)->numRects = 0)
#define REGION_NOT_EMPTY(pReg)  ((pReg)->numRects)

#define INBOX(r, x, y) \
      ( ((r).x2 >  (x)) && \
        ((r).x1 <= (x)) && \
        ((r).y2 >  (y)) && \
        ((r).y1 <= (y)) )

/*
 * number of points to buffer before sending them off
 * to scanlines(): Must be an even number
 */
#define NUMPTSTOBUFFER 200

/*
 * used to allocate buffers for points and link the buffers together
 */
typedef struct _POINTBLOCK {
    XPoint pts[NUMPTSTOBUFFER];
    struct _POINTBLOCK *next;
} POINTBLOCK;

#endif /* _X11_XREGION_H_ */
