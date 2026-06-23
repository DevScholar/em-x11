#include "em_x11_internal.h"

#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

static void apply_values(GC gc, unsigned long valuemask, XGCValues* values) {
  if (!gc || !values)
    return;
  if (valuemask & GCFunction)
    gc->function = values->function;
  if (valuemask & GCPlaneMask) { /* ignored */
  }
  if (valuemask & GCForeground)
    gc->foreground = values->foreground;
  if (valuemask & GCBackground)
    gc->background = values->background;
  if (valuemask & GCLineWidth)
    gc->line_width = values->line_width;
  if (valuemask & GCLineStyle)
    gc->line_style = values->line_style;
  if (valuemask & GCFillStyle)
    gc->fill_style = values->fill_style;
  if (valuemask & GCFont)
    gc->font = values->font;
  if (valuemask & GCTile)
    gc->tile = values->tile;
  if (valuemask & GCStipple)
    gc->stipple = values->stipple;
  if (valuemask & GCTileStipXOrigin)
    gc->ts_x_origin = values->ts_x_origin;
  if (valuemask & GCTileStipYOrigin)
    gc->ts_y_origin = values->ts_y_origin;
}

GC XCreateGC(Display* display,
             Drawable d,
             unsigned long valuemask,
             XGCValues* values) {
  (void)display;
  (void)d;

  struct _XGC* gc = calloc(1, sizeof(struct _XGC));
  if (!gc) {
    return NULL;
  }
  gc->foreground = 0x00000000UL;
  gc->background = 0x00FFFFFFUL;
  gc->line_width = 0;
  gc->line_style = LineSolid;
  gc->fill_style = FillSolid;
  gc->function = GXcopy;
  gc->font = None;
  gc->tile = None;
  gc->stipple = None;
  gc->ts_x_origin = 0;
  gc->ts_y_origin = 0;

  apply_values(gc, valuemask, values);
  return gc;
}

int XChangeGC(Display* display,
              GC gc,
              unsigned long valuemask,
              XGCValues* values) {
  (void)display;
  apply_values(gc, valuemask, values);
  return 1;
}

int XCopyGC(Display* display, GC src, unsigned long valuemask, GC dst) {
  (void)display;
  if (!src || !dst)
    return 0;
  XGCValues v;
  memset(&v, 0, sizeof(v));
  v.foreground = src->foreground;
  v.background = src->background;
  v.line_width = src->line_width;
  v.line_style = src->line_style;
  v.fill_style = src->fill_style;
  v.function = src->function;
  v.font = src->font;
  v.tile = src->tile;
  v.stipple = src->stipple;
  v.ts_x_origin = src->ts_x_origin;
  v.ts_y_origin = src->ts_y_origin;
  apply_values(dst, valuemask, &v);
  return 1;
}

int XGetGCValues(Display* display,
                 GC gc,
                 unsigned long valuemask,
                 XGCValues* values_return) {
  (void)display;
  if (!gc || !values_return)
    return 0;
  if (valuemask & GCForeground)
    values_return->foreground = gc->foreground;
  if (valuemask & GCBackground)
    values_return->background = gc->background;
  if (valuemask & GCLineWidth)
    values_return->line_width = gc->line_width;
  if (valuemask & GCLineStyle)
    values_return->line_style = gc->line_style;
  if (valuemask & GCFillStyle)
    values_return->fill_style = gc->fill_style;
  if (valuemask & GCFunction)
    values_return->function = gc->function;
  if (valuemask & GCFont)
    values_return->font = gc->font;
  if (valuemask & GCTile)
    values_return->tile = gc->tile;
  if (valuemask & GCStipple)
    values_return->stipple = gc->stipple;
  if (valuemask & GCTileStipXOrigin)
    values_return->ts_x_origin = gc->ts_x_origin;
  if (valuemask & GCTileStipYOrigin)
    values_return->ts_y_origin = gc->ts_y_origin;
  return 1;
}

int XFreeGC(Display* display, GC gc) {
  (void)display;
  free(gc);
  return 1;
}

GContext XGContextFromGC(GC gc) { return gc ? gc->gid : 0; }

int XSetForeground(Display* display, GC gc, unsigned long foreground) {
  (void)display;
  if (!gc)
    return 0;
  gc->foreground = foreground;
  return 1;
}

int XSetBackground(Display* display, GC gc, unsigned long background) {
  (void)display;
  if (!gc)
    return 0;
  gc->background = background;
  return 1;
}

int XSetLineAttributes(Display* display,
                       GC gc,
                       unsigned int line_width,
                       int line_style,
                       int cap_style,
                       int join_style) {
  (void)display;
  (void)cap_style;
  (void)join_style;
  if (!gc)
    return 0;
  gc->line_width = (int)line_width;
  gc->line_style = line_style;
  return 1;
}

int XSetFillStyle(Display* display, GC gc, int fill_style) {
  (void)display;
  if (!gc)
    return 0;
  gc->fill_style = fill_style;
  return 1;
}

int XSetFunction(Display* display, GC gc, int function) {
  (void)display;
  if (!gc)
    return 0;
  gc->function = function;
  return 1;
}

/* --- Tile / Stipple --- */

int XSetTile(Display* dpy, GC gc, Pixmap tile) {
  (void)dpy;
  if (!gc)
    return 0;
  gc->tile = tile;
  return 1;
}

int XSetStipple(Display* dpy, GC gc, Pixmap stipple) {
  (void)dpy;
  if (!gc)
    return 0;
  gc->stipple = stipple;
  return 1;
}

int XSetTSOrigin(Display* dpy, GC gc, int ts_x_origin, int ts_y_origin) {
  (void)dpy;
  if (!gc)
    return 0;
  gc->ts_x_origin = ts_x_origin;
  gc->ts_y_origin = ts_y_origin;
  return 1;
}

/* --- Clipping stubs --- */

int XSetClipMask(Display* display, GC gc, Pixmap pixmap) {
  (void)display;
  (void)gc;
  (void)pixmap;
  return 1;
}

int XSetClipOrigin(Display* display,
                   GC gc,
                   int clip_x_origin,
                   int clip_y_origin) {
  (void)display;
  (void)gc;
  (void)clip_x_origin;
  (void)clip_y_origin;
  return 1;
}

int XSetClipRectangles(Display* display,
                       GC gc,
                       int clip_x_origin,
                       int clip_y_origin,
                       XRectangle* rectangles,
                       int n,
                       int ordering) {
  (void)display;
  (void)gc;
  (void)clip_x_origin;
  (void)clip_y_origin;
  (void)rectangles;
  (void)n;
  (void)ordering;
  return 1;
}

/* -- GC setter stubs -- */

int XSetArcMode(Display* dpy, GC gc, int arc_mode) {
  (void)dpy;
  (void)gc;
  (void)arc_mode;
  return 1;
}
int XSetDashes(
  Display* dpy, GC gc, int dash_offset, _Xconst char* dash_list, int n) {
  (void)dpy;
  (void)gc;
  (void)dash_offset;
  (void)dash_list;
  (void)n;
  return 1;
}
int XSetFillRule(Display* dpy, GC gc, int fill_rule) {
  (void)dpy;
  (void)gc;
  (void)fill_rule;
  return 1;
}
int XSetGraphicsExposures(Display* dpy, GC gc, Bool graphics_exposures) {
  (void)dpy;
  (void)gc;
  (void)graphics_exposures;
  return 1;
}
int XSetPlaneMask(Display* dpy, GC gc, unsigned long plane_mask) {
  (void)dpy;
  (void)gc;
  (void)plane_mask;
  return 1;
}
int XSetSubwindowMode(Display* dpy, GC gc, int subwindow_mode) {
  (void)dpy;
  (void)gc;
  (void)subwindow_mode;
  return 1;
}
