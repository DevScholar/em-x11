/*
 * XTextExtents16 — 16-bit (XChar2b) text extents.
 * Upstream: libX11/src/TextExt16.c
 *
 * Converts UCS-2 big-endian to UTF-8 (including surrogate pairs), then
 * delegates to the 8-bit XTextExtents which uses the browser's canvas
 * text measurement.
 */

#include "../em_x11/em_x11_utf8.h"
#include <X11/Xlib.h>
#include <stdlib.h>
#include <string.h>

static int
ucs2_to_utf8(const XChar2b* string, int nchars, unsigned char* out, int cap) {
  int w = 0, i = 0;
  while (i < nchars && w + 4 <= cap) {
    unsigned int cp = ((unsigned int)string[i].byte1 << 8) | string[i].byte2;
    i++;
    if (cp >= 0xD800 && cp <= 0xDBFF && i < nchars) {
      unsigned int lo = ((unsigned int)string[i].byte1 << 8) | string[i].byte2;
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i++;
      }
    }
    w += em_x11_utf8_encode(cp, out + w);
  }
  return w;
}

int XTextExtents16(XFontStruct* font,
                   const XChar2b* string,
                   int nchars,
                   int* direction_return,
                   int* font_ascent_return,
                   int* font_descent_return,
                   XCharStruct* overall_return) {
  if (!string || nchars <= 0)
    return 0;
  int cap = nchars * 4 + 1;
  unsigned char stack[512];
  unsigned char* buf =
    (cap <= (int)sizeof(stack)) ? stack : (unsigned char*)malloc(cap);
  if (!buf)
    return 0;
  int used = ucs2_to_utf8(string, nchars, buf, cap - 1);
  buf[used] = '\0';
  int ret = XTextExtents(font,
                         (const char*)buf,
                         used,
                         direction_return,
                         font_ascent_return,
                         font_descent_return,
                         overall_return);
  if (buf != stack)
    free(buf);
  return ret;
}
