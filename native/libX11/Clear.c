/*
 * XClearWindow — clear entire window to its background.
 * Upstream: libX11/src/Clear.c
 *
 * Equivalent to XClearArea(display, w, 0, 0, 0, 0, True): fill the
 * whole window with its background, then generate one Expose event
 * so the client redraws. Without the Expose, shaped clients like
 * xeyes that call XClearWindow during Resize never learn that their
 * backing pixels were wiped — the compositor blits the cleared
 * backing to canvas forever.
 */
#include "em_x11_internal.h"
#include <string.h>

int XClearWindow(Display* display, Window w) {
  EmxWindow* win = em_x11_window_find(display, w);
  if (!win)
    return 0;
  em_x11_js_clear_area(w, 0, 0, win->width, win->height);
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = Expose;
  ev.xexpose.display = display;
  ev.xexpose.window = w;
  ev.xexpose.x = 0;
  ev.xexpose.y = 0;
  ev.xexpose.width = (int)win->width;
  ev.xexpose.height = (int)win->height;
  ev.xexpose.count = 0;
  em_x11_event_queue_push(display, &ev);
  return 1;
}
