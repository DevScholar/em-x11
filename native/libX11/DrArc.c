/*
 * XDrawArc — outline an arc.
 * Upstream: libX11/src/DrArc.c
 *
 * Drawable validation intentionally skipped: em-x11 trusts its wasm
 * clients (they share an address space) and the browser's canvas API is
 * the ultimate validator — operations on non-existent backing stores
 * are harmless no-ops. Adding per-call lookup+check overhead on the
 * hot drawing path would penalize legitimate draws for no benefit.
 */

#include "em_x11_internal.h"

int XDrawArc(Display* display,
             Drawable d,
             GC gc,
             int x,
             int y,
             unsigned int width,
             unsigned int height,
             int angle1,
             int angle2) {
  (void)display;
  if (!gc || gc_draw_disabled(gc))
    return 0;
  em_x11_js_draw_arc((Window)d,
                     x,
                     y,
                     width,
                     height,
                     angle1,
                     angle2,
                     gc->foreground,
                     gc->line_width);
  return 1;
}
