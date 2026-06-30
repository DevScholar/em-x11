/*
 * XAllocColorCells — allocate read/write color cells.
 * Upstream: libX11/src/AllCells.c
 *
 * Real X11 sends an AllocColorCells protocol request and reads the reply
 * with pixel/mask arrays. em-x11 allocates sequential pixel values.
 */

#include <X11/Xlib.h>

Status XAllocColorCells(Display* dpy,
                        Colormap cmap,
                        Bool contig,
                        unsigned long* plane_masks,
                        unsigned int nplanes,
                        unsigned long* pixels,
                        unsigned int npixels) {
  (void)dpy;
  (void)cmap;
  (void)contig;
  if (plane_masks && nplanes > 0)
    *plane_masks = 0;
  for (unsigned int i = 0; i < npixels && pixels; i++) {
    pixels[i] = (unsigned long)i;
  }
  return 1;
}
