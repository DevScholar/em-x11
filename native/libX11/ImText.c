/*
 * ImText — Xmb / Xwc / Xutf8 text drawing and measurement.
 *
 * The fontset's first font is the one we want to render with. The
 * Xmb/Xutf8 variants route the UTF-8 bytes straight through XDrawString;
 * the Xwc variants encode wchar_t (UCS-4 in Emscripten) into UTF-8 first.
 *
 * All three families temporarily install the fontset's font into the GC
 * so widgets that build a separate fontset per render-table tag get the
 * font they asked for, not whatever font is sitting in the GC.
 */

#include "em_x11_internal.h"
#include "em_x11_utf8.h"

#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

extern XFontStruct* em_x11_fontset_font(XFontSet font_set);

/* UTF-8 encode / char-count: use shared helpers from em_x11_utf8.h. */

static unsigned char* wcs_to_utf8(const wchar_t* ws, int nw, int* out_bytes) {
  if (!ws || nw <= 0) {
    unsigned char* empty = malloc(1);
    if (empty)
      empty[0] = 0;
    if (out_bytes)
      *out_bytes = 0;
    return empty;
  }
  size_t cap = (size_t)nw * 4 + 1;
  unsigned char* buf = malloc(cap);
  if (!buf) {
    if (out_bytes)
      *out_bytes = 0;
    return NULL;
  }
  int used = 0;
  for (int i = 0; i < nw; i++) {
    used += em_x11_utf8_encode((unsigned int)ws[i], buf + used);
  }
  buf[used] = 0;
  if (out_bytes)
    *out_bytes = used;
  return buf;
}

static void draw_with_fontset(Display* dpy,
                              Drawable d,
                              XFontSet font_set,
                              GC gc,
                              int x,
                              int y,
                              const char* text,
                              int bytes,
                              int image_mode) {
  if (!gc || !text || bytes <= 0)
    return;
  XFontStruct* fs = em_x11_fontset_font(font_set);
  Font saved = gc->font;
  if (fs)
    gc->font = fs->fid;
  if (image_mode)
    XDrawImageString(dpy, d, gc, x, y, text, bytes);
  else
    XDrawString(dpy, d, gc, x, y, text, bytes);
  gc->font = saved;
}

void XmbDrawString(Display* dpy,
                   Drawable d,
                   XFontSet font_set,
                   GC gc,
                   int x,
                   int y,
                   _Xconst char* text,
                   int bytes) {
  draw_with_fontset(dpy, d, font_set, gc, x, y, text, bytes, 0);
}

void Xutf8DrawString(Display* dpy,
                     Drawable d,
                     XFontSet font_set,
                     GC gc,
                     int x,
                     int y,
                     _Xconst char* text,
                     int bytes) {
  draw_with_fontset(dpy, d, font_set, gc, x, y, text, bytes, 0);
}

void XwcDrawString(Display* dpy,
                   Drawable d,
                   XFontSet font_set,
                   GC gc,
                   int x,
                   int y,
                   _Xconst wchar_t* text,
                   int num_wchars) {
  int bytes = 0;
  unsigned char* u8 = wcs_to_utf8(text, num_wchars, &bytes);
  if (u8) {
    draw_with_fontset(dpy, d, font_set, gc, x, y, (const char*)u8, bytes, 0);
    free(u8);
  }
}

void XmbDrawImageString(Display* dpy,
                        Drawable d,
                        XFontSet font_set,
                        GC gc,
                        int x,
                        int y,
                        _Xconst char* text,
                        int bytes) {
  draw_with_fontset(dpy, d, font_set, gc, x, y, text, bytes, 1);
}

void Xutf8DrawImageString(Display* dpy,
                          Drawable d,
                          XFontSet font_set,
                          GC gc,
                          int x,
                          int y,
                          _Xconst char* text,
                          int bytes) {
  draw_with_fontset(dpy, d, font_set, gc, x, y, text, bytes, 1);
}

void XwcDrawImageString(Display* dpy,
                        Drawable d,
                        XFontSet font_set,
                        GC gc,
                        int x,
                        int y,
                        _Xconst wchar_t* text,
                        int num_wchars) {
  int bytes = 0;
  unsigned char* u8 = wcs_to_utf8(text, num_wchars, &bytes);
  if (u8) {
    draw_with_fontset(dpy, d, font_set, gc, x, y, (const char*)u8, bytes, 1);
    free(u8);
  }
}

static int
escapement_with_fontset(XFontSet font_set, const char* text, int bytes) {
  if (!text || bytes <= 0)
    return 0;
  XFontStruct* fs = em_x11_fontset_font(font_set);
  if (fs)
    return XTextWidth(fs, text, bytes);
  return bytes * 7;
}

int XmbTextEscapement(XFontSet font_set, _Xconst char* text, int bytes) {
  return escapement_with_fontset(font_set, text, bytes);
}

int Xutf8TextEscapement(XFontSet font_set, _Xconst char* text, int bytes) {
  return escapement_with_fontset(font_set, text, bytes);
}

int XwcTextEscapement(XFontSet font_set,
                      _Xconst wchar_t* text,
                      int num_wchars) {
  int bytes = 0;
  unsigned char* u8 = wcs_to_utf8(text, num_wchars, &bytes);
  if (!u8)
    return 0;
  int w = escapement_with_fontset(font_set, (const char*)u8, bytes);
  free(u8);
  return w;
}

static int extents_with_fontset(XFontSet font_set,
                                const char* text,
                                int bytes,
                                XRectangle* ink,
                                XRectangle* logical) {
  XFontStruct* fs = em_x11_fontset_font(font_set);
  int width = fs ? XTextWidth(fs, text, bytes) : bytes * 7;
  int ascent = fs ? fs->ascent : 10;
  int descent = fs ? fs->descent : 2;
  XRectangle r = {0,
                  (short)-ascent,
                  (unsigned short)width,
                  (unsigned short)(ascent + descent)};
  if (ink)
    *ink = r;
  if (logical)
    *logical = r;
  return width;
}

int XmbTextExtents(XFontSet font_set,
                   _Xconst char* text,
                   int nbytes,
                   XRectangle* ink,
                   XRectangle* logical) {
  return extents_with_fontset(font_set, text, nbytes, ink, logical);
}

int Xutf8TextExtents(XFontSet font_set,
                     _Xconst char* text,
                     int nbytes,
                     XRectangle* ink,
                     XRectangle* logical) {
  return extents_with_fontset(font_set, text, nbytes, ink, logical);
}

int XwcTextExtents(XFontSet font_set,
                   _Xconst wchar_t* text,
                   int num_wchars,
                   XRectangle* ink,
                   XRectangle* logical) {
  int bytes = 0;
  unsigned char* u8 = wcs_to_utf8(text, num_wchars, &bytes);
  if (!u8)
    return 0;
  int w = extents_with_fontset(font_set, (const char*)u8, bytes, ink, logical);
  free(u8);
  return w;
}

static Status percharextents_utf8(XFontSet font_set,
                                  const char* text,
                                  int bytes,
                                  XRectangle* ink_buf,
                                  XRectangle* log_buf,
                                  int buf_size,
                                  int* num_chars,
                                  XRectangle* overall_ink,
                                  XRectangle* overall_log) {
  int total_chars = em_x11_utf8_char_count(text, bytes);
  if (num_chars)
    *num_chars = total_chars;
  if (total_chars > buf_size)
    return 0;

  XFontStruct* fs = em_x11_fontset_font(font_set);
  int ascent = fs ? fs->ascent : 10;
  int descent = fs ? fs->descent : 2;
  int prev_w = 0;
  int idx = 0;

  for (int i = 0; i < bytes;) {
    unsigned char c = (unsigned char)text[i];
    int step = (c < 0x80)             ? 1
               : ((c & 0xE0) == 0xC0) ? 2
               : ((c & 0xF0) == 0xE0) ? 3
               : ((c & 0xF8) == 0xF0) ? 4
                                      : 1;
    if (i + step > bytes)
      step = bytes - i;
    int run_w = fs ? XTextWidth(fs, text, i + step) : (i + step) * 7;
    int adv = run_w - prev_w;
    XRectangle r = {(short)prev_w,
                    (short)-ascent,
                    (unsigned short)(adv > 0 ? adv : 0),
                    (unsigned short)(ascent + descent)};
    if (ink_buf)
      ink_buf[idx] = r;
    if (log_buf)
      log_buf[idx] = r;
    prev_w = run_w;
    idx++;
    i += step;
  }
  XRectangle overall = {0,
                        (short)-ascent,
                        (unsigned short)prev_w,
                        (unsigned short)(ascent + descent)};
  if (overall_ink)
    *overall_ink = overall;
  if (overall_log)
    *overall_log = overall;
  return 1;
}

Status XmbTextPerCharExtents(XFontSet font_set,
                             _Xconst char* text,
                             int bytes,
                             XRectangle* ink_buf,
                             XRectangle* log_buf,
                             int buf_size,
                             int* num_chars,
                             XRectangle* overall_ink,
                             XRectangle* overall_log) {
  return percharextents_utf8(font_set,
                             text,
                             bytes,
                             ink_buf,
                             log_buf,
                             buf_size,
                             num_chars,
                             overall_ink,
                             overall_log);
}

Status Xutf8TextPerCharExtents(XFontSet font_set,
                               _Xconst char* text,
                               int bytes,
                               XRectangle* ink_buf,
                               XRectangle* log_buf,
                               int buf_size,
                               int* num_chars,
                               XRectangle* overall_ink,
                               XRectangle* overall_log) {
  return percharextents_utf8(font_set,
                             text,
                             bytes,
                             ink_buf,
                             log_buf,
                             buf_size,
                             num_chars,
                             overall_ink,
                             overall_log);
}

Status XwcTextPerCharExtents(XFontSet font_set,
                             _Xconst wchar_t* text,
                             int num_wchars,
                             XRectangle* ink_buf,
                             XRectangle* log_buf,
                             int buf_size,
                             int* num_chars,
                             XRectangle* overall_ink,
                             XRectangle* overall_log) {
  int bytes = 0;
  unsigned char* u8 = wcs_to_utf8(text, num_wchars, &bytes);
  if (!u8)
    return 0;
  Status s = percharextents_utf8(font_set,
                                 (const char*)u8,
                                 bytes,
                                 ink_buf,
                                 log_buf,
                                 buf_size,
                                 num_chars,
                                 overall_ink,
                                 overall_log);
  free(u8);
  return s;
}

void XmbDrawText(Display* dpy,
                 Drawable d,
                 GC gc,
                 int x,
                 int y,
                 XmbTextItem* items,
                 int nitems) {
  if (!items)
    return;
  for (int i = 0; i < nitems; i++) {
    if (i > 0)
      x += items[i].delta;
    XmbDrawString(
      dpy, d, items[i].font_set, gc, x, y, items[i].chars, items[i].nchars);
    x += XmbTextEscapement(items[i].font_set, items[i].chars, items[i].nchars);
  }
}

void Xutf8DrawText(Display* dpy,
                   Drawable d,
                   GC gc,
                   int x,
                   int y,
                   XmbTextItem* items,
                   int nitems) {
  if (!items)
    return;
  for (int i = 0; i < nitems; i++) {
    if (i > 0)
      x += items[i].delta;
    Xutf8DrawString(
      dpy, d, items[i].font_set, gc, x, y, items[i].chars, items[i].nchars);
    x +=
      Xutf8TextEscapement(items[i].font_set, items[i].chars, items[i].nchars);
  }
}

void XwcDrawText(Display* dpy,
                 Drawable d,
                 GC gc,
                 int x,
                 int y,
                 XwcTextItem* items,
                 int nitems) {
  if (!items)
    return;
  for (int i = 0; i < nitems; i++) {
    if (i > 0)
      x += items[i].delta;
    XwcDrawString(
      dpy, d, items[i].font_set, gc, x, y, items[i].chars, items[i].nchars);
    x += XwcTextEscapement(items[i].font_set, items[i].chars, items[i].nchars);
  }
}
