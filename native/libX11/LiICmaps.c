/*
 * XListInstalledColormaps — list the installed colormaps for a screen.
 * Upstream: libX11/src/LiICmaps.c
 *
 * Real X11 sends a ListInstalledColormaps request and reads the reply.
 * em-x11 returns the default colormap.
 */

#include <X11/Xlib.h>
#include <stdlib.h>

Colormap* XListInstalledColormaps(Display* dpy, Window w, int* num_return) {
  (void)w;
  Colormap* list = (Colormap*)malloc(sizeof(Colormap));
  if (list) {
    list[0] = DefaultColormap(dpy, DefaultScreen(dpy));
    *num_return = 1;
  } else {
    *num_return = 0;
  }
  return list;
}
