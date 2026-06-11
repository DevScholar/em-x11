/*
 * XPutImage — write image data to a drawable.
 * Upstream: libX11/src/PutImage.c
 */
#include "em_x11_internal.h"

int XPutImage(Display* dpy,
              Drawable d,
              GC gc,
              XImage* image,
              int src_x,
              int src_y,
              int dst_x,
              int dst_y,
              unsigned int w,
              unsigned int h) {
  (void)dpy;
  if (!gc || !image || !image->data || w == 0 || h == 0)
    return 1;
  int bpl = image->bytes_per_line;
  int bpp = image->bits_per_pixel;
  const unsigned char* base = (const unsigned char*)image->data;
  int byte_offset;
  if (image->format == XYBitmap || image->depth == 1) {
    byte_offset = src_y * bpl + (src_x >> 3);
  } else {
    byte_offset = src_y * bpl + src_x * (bpp >> 3);
  }
  int data_len = (int)h * bpl;
  em_x11_js_put_image(d,
                      dst_x,
                      dst_y,
                      w,
                      h,
                      image->format,
                      image->depth,
                      bpl,
                      base + byte_offset,
                      data_len,
                      gc->foreground,
                      gc->background);
  return 1;
}
