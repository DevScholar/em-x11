/*
 * XFillRectangle — fill a solid rectangle.
 * Upstream: libX11/src/FillRct.c
 */

#include "em_x11_internal.h"

int XFillRectangle(Display* display,
                   Drawable d,
                   GC gc,
                   int x,
                   int y,
                   unsigned int width,
                   unsigned int height) {
  (void)display;
  if (!gc || gc_draw_disabled(gc))
    return 0;
  if (width == 0 || height == 0)
    return 1;

  /* FillTiled: decompose into XCopyArea calls that repeat the tile pixmap
   * across the fill rectangle. Motif's notebook spiral binding relies on
   * this — real X servers support it natively, but em-x11's Canvas 2D
   * backend has no createPattern, so we tile manually. */
  if (gc->fill_style == FillTiled && gc->tile != None) {
    unsigned int tw, th, td;
    if (!em_x11_pixmap_get_geometry(gc->tile, &tw, &th, &td))
      goto solid;
    if (tw == 0 || th == 0)
      goto solid;

    int ox = gc->ts_x_origin;
    int oy = gc->ts_y_origin;

    /* Align to tile grid: find the first tile column/row that covers x/y. */
    int dx = (x - ox) % (int)tw;
    if (dx < 0)
      dx += (int)tw;
    int dy = (y - oy) % (int)th;
    if (dy < 0)
      dy += (int)th;

    int tx = x - dx;
    int ty = y - dy;

    for (int cy = ty; cy < (int)(y + height); cy += (int)th) {
      int copy_h = (int)th;
      int src_y = 0;
      if (cy < y) {
        src_y = y - cy;
        copy_h -= src_y;
      }
      if (cy + copy_h > (int)(y + height))
        copy_h = (int)(y + height) - cy;
      if (copy_h <= 0)
        continue;

      for (int cx = tx; cx < (int)(x + width); cx += (int)tw) {
        int copy_w = (int)tw;
        int src_x = 0;
        if (cx < x) {
          src_x = x - cx;
          copy_w -= src_x;
        }
        if (cx + copy_w > (int)(x + width))
          copy_w = (int)(x + width) - cx;
        if (copy_w <= 0)
          continue;

        XCopyArea(display,
                  (Drawable)gc->tile,
                  d,
                  gc,
                  src_x,
                  src_y,
                  (unsigned)copy_w,
                  (unsigned)copy_h,
                  cx + src_x,
                  cy + src_y);
      }
    }
    return 1;
  }

  /* FillStippled / FillOpaqueStippled: use the stipple pixmap as a
   * monochrome mask. Where stipple bit=1, the foreground is drawn; where
   * bit=0, the destination is left unchanged (FillStippled) or painted
   * with the background colour (FillOpaqueStippled). Motif's text-widget
   * I-beam caret relies on this. */
  if ((gc->fill_style == FillStippled ||
       gc->fill_style == FillOpaqueStippled) &&
      gc->stipple != None) {
    em_x11_js_fill_stippled_rect((unsigned int)d,
                                 x,
                                 y,
                                 width,
                                 height,
                                 gc->foreground,
                                 gc->background,
                                 (unsigned int)gc->stipple,
                                 gc->ts_x_origin,
                                 gc->ts_y_origin,
                                 (int)(gc->fill_style == FillOpaqueStippled));
    return 1;
  }

solid:
  em_x11_js_fill_rect((Window)d, x, y, width, height, gc->foreground);
  return 1;
}
