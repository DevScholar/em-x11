/*
 * XClearArea — clear a rectangular area of a window.
 * Upstream: libX11/src/ClearArea.c
 */
#include "em_x11_internal.h"
#include <string.h>

int XClearArea(Display* display,
               Window w,
               int x,
               int y,
               unsigned int width,
               unsigned int height,
               Bool exposures) {
  EmX11Window* win = em_x11_window_find(display, w);
  if (!win)
    return 0;
  if (width == 0)
    width = win->width - (unsigned int)x;
  if (height == 0)
    height = win->height - (unsigned int)y;
  em_x11_js_clear_area(w, x, y, width, height);
  if (exposures) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = Expose;
    ev.xexpose.display = display;
    ev.xexpose.window = w;
    ev.xexpose.x = x;
    ev.xexpose.y = y;
    ev.xexpose.width = (int)width;
    ev.xexpose.height = (int)height;
    ev.xexpose.count = 0;
    em_x11_event_queue_push(display, &ev);
  }
  return 1;
}
