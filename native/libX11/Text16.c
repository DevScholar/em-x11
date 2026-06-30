/*
 * Text16 — XChar2b (16-bit) text drawing and measurement.
 *
 * Tk's Unix font layer classifies em-x11's XFontStruct as a two-byte
 * (iso10646-1 / ucs-2be) font, so it issues XDrawString16 / XTextWidth16
 * with XChar2b arrays in UCS-2 big-endian. We translate those back into
 * UTF-8 (combining high + low surrogate pairs so astral codepoints like
 * emoji round-trip correctly) and hand the UTF-8 buffer to the same
 * browser-side text pipeline used for 8-bit strings.
 */

#include "em_x11_internal.h"
#include "em_x11_utf8.h"

#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

static int
xchar2b_to_utf8(const XChar2b* s, int n, unsigned char* out, int cap) {
  int w = 0, i = 0;
  while (i < n) {
    unsigned int cp = ((unsigned int)s[i].byte1 << 8) | s[i].byte2;
    i++;
    if (cp >= 0xD800 && cp <= 0xDBFF && i < n) {
      unsigned int lo = ((unsigned int)s[i].byte1 << 8) | s[i].byte2;
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i++;
      }
    }
    if (w + 4 > cap)
      break;
    w += em_x11_utf8_encode(cp, out + w);
  }
  return w;
}

#define EM_X11_UTF8_BUFLEN(n) ((size_t)(n) * 4 + 1)

int XDrawString16(Display* dpy,
                  Drawable d,
                  GC gc,
                  int x,
                  int y,
                  _Xconst XChar2b* string,
                  int length) {
  if (!string || length <= 0)
    return 1;
  size_t cap = EM_X11_UTF8_BUFLEN(length);
  unsigned char stack[512];
  unsigned char* buf =
    cap <= sizeof stack ? stack : (unsigned char*)malloc(cap);
  if (!buf)
    return 0;
  int used = xchar2b_to_utf8(string, length, buf, (int)cap - 1);
  buf[used] = 0;
  int r = XDrawString(dpy, d, gc, x, y, (const char*)buf, used);
  if (buf != stack)
    free(buf);
  return r;
}

int XDrawImageString16(Display* dpy,
                       Drawable d,
                       GC gc,
                       int x,
                       int y,
                       _Xconst XChar2b* string,
                       int length) {
  if (!string || length <= 0)
    return 1;
  size_t cap = EM_X11_UTF8_BUFLEN(length);
  unsigned char stack[512];
  unsigned char* buf =
    cap <= sizeof stack ? stack : (unsigned char*)malloc(cap);
  if (!buf)
    return 0;
  int used = xchar2b_to_utf8(string, length, buf, (int)cap - 1);
  buf[used] = 0;
  int r = XDrawImageString(dpy, d, gc, x, y, (const char*)buf, used);
  if (buf != stack)
    free(buf);
  return r;
}

int XTextWidth16(XFontStruct* fs, _Xconst XChar2b* string, int count) {
  if (!string || count <= 0)
    return 0;
  size_t cap = EM_X11_UTF8_BUFLEN(count);
  unsigned char stack[512];
  unsigned char* buf =
    cap <= sizeof stack ? stack : (unsigned char*)malloc(cap);
  if (!buf)
    return 0;
  int used = xchar2b_to_utf8(string, count, buf, (int)cap - 1);
  buf[used] = 0;
  int w = XTextWidth(fs, (const char*)buf, used);
  if (buf != stack)
    free(buf);
  return w;
}
