/*
 * XStoreColor — store a color in a colormap.
 * Upstream: libX11/src/StColor.c
 *
 * Real X11 sends a StoreColors protocol request. em-x11 synthesises a
 * pixel value from the RGB components.
 */

#include <X11/Xlib.h>

int XStoreColor(Display* dpy, Colormap cmap, XColor* color) {
  (void)dpy;
  (void)cmap;
  if (color) {
    color->pixel =
      (unsigned long)((color->red & 0xFF00) << 8 | (color->green & 0xFF00) |
                      (color->blue & 0xFF00) >> 8);
  }
  return 1;
}
