/*
 * XCopyArea — copy a rectangle between drawables.
 * Upstream: libX11/src/CopyArea.c
 */
#include "em_x11_internal.h"
#include <string.h>

int XCopyArea(Display* dpy,
              Drawable src,
              Drawable dst,
              GC gc,
              int src_x,
              int src_y,
              unsigned int width,
              unsigned int height,
              int dst_x,
              int dst_y) {
  (void)gc;
  if (width == 0 || height == 0)
    return 1;
  em_x11_js_copy_area(src, dst, src_x, src_y, width, height, dst_x, dst_y);
  if (em_x11_window_find(dpy, dst) != NULL) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xnoexpose.type = NoExpose;
    ev.xnoexpose.display = dpy;
    ev.xnoexpose.drawable = dst;
    ev.xnoexpose.major_code = 62; /* X_CopyArea */
    ev.xnoexpose.minor_code = 0;
    em_x11_event_queue_push(dpy, &ev);
  }
  return 1;
}
