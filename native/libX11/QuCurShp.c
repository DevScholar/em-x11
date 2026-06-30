/*
 * XQueryBestCursor — query the best cursor size.
 * Upstream: libX11/src/QuCurShp.c
 *
 * Real X11 sends a QueryBestSize(CursorShape) request and reads the
 * reply. em-x11 caps at 32×32, matching common X cursor sizes.
 */

#include <X11/Xlib.h>

Status XQueryBestCursor(Display* dpy,
                        Drawable d,
                        unsigned int width,
                        unsigned int height,
                        unsigned int* width_return,
                        unsigned int* height_return) {
  (void)dpy;
  (void)d;
  if (width_return)
    *width_return = width > 32 ? 32 : width;
  if (height_return)
    *height_return = height > 32 ? 32 : height;
  return 1;
}
