/*
 * draw_utils — shared drawing helpers.
 *
 * flatten_points serialises XPoint[] with CoordMode resolution into a
 * flat int[] that the JS bridge consumes. Inlined gc_draw_disabled stays
 * in em_x11_internal.h.
 */

#include "em_x11_internal.h"
#include <stdlib.h>

int* flatten_points(XPoint* points, int npoints, int mode, int* out_count) {
  if (npoints <= 0 || !points) {
    *out_count = 0;
    return NULL;
  }
  int* flat = malloc(sizeof(int) * 2 * (size_t)npoints);
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
