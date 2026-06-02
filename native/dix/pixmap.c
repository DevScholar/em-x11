/*
 * Pixmap lifecycle.
 *
 * X Pixmaps are server-side offscreen drawables. In em-x11 each Pixmap
 * is backed by an OffscreenCanvas on the JS side (the emx11_js_pixmap_*
 * EM_JS bridges in native/emx11/bridges.c). The C side only tracks the
 * (id, width, height, depth) triple so drawing calls and SHAPE can
 * resolve ids without round-tripping through JS.
 *
 * Drawing routing: XFillRectangle / XFillArc / XDrawLine / etc. all
 * push through emx11_js_fill_rect et al. keyed on a Drawable id. The
 * JS host recognises pixmap ids and dispatches to the pixmap's own
 * ctx; windows go through the compositor as before. The C side does
 * not need to know the difference.
 *
 * Today's scope: depth-1 bitmap pixmaps (for SHAPE masks -- xeyes).
 * Color pixmaps and XCopyArea are valid callers of the same machinery
 * but are not exercised yet.
 */

#include "emx11_internal.h"

#include <X11/Xutil.h>
#include <stdlib.h>

typedef struct EmxPixmap {
  Pixmap id;
  unsigned int width;
  unsigned int height;
  unsigned int depth;
  /* Refcount = 1 from XCreatePixmap, +1 per window holding this pixmap
   * as background_pixmap. XFreePixmap only decrements; the JS canvas
   * is destroyed when it hits zero. Real X servers do this implicitly
   * (server-side resource ownership keeps the pixmap alive while a
   * window references it); twm relies on it -- it XFreePixmap's the
   * hilite tile immediately after XCreateWindow with CWBackPixmap,
   * expecting the server to keep the bits around. */
  unsigned int refcount;
  struct EmxPixmap* next;
} EmxPixmap;

static EmxPixmap* g_pixmaps = NULL;

static EmxPixmap* pixmap_find(Pixmap id) {
  for (EmxPixmap* p = g_pixmaps; p; p = p->next) {
    if (p->id == id)
      return p;
  }
  return NULL;
}

Pixmap XCreatePixmap(Display* dpy,
                     Drawable d,
                     unsigned int width,
                     unsigned int height,
                     unsigned int depth) {
  (void)d;
  if (width == 0 || height == 0)
    return None;
  /* Mirror the host-side OffscreenCanvas cap (src/host/gc.ts:
   * MAX_PIXMAP_EDGE = 4096). Without this gate the C struct still
   * gets refcount=1 and a valid id, but the JS side silently refuses
   * to allocate the canvas -- subsequent XCopyArea / XCopyPlane to
   * that id no-op with no error path back to the caller. Fail in C
   * so XFillRectangle on a None Drawable is the only downstream
   * symptom the caller has to handle. */
  if (width > 4096 || height > 4096)
    return None;
  EmxPixmap* p = calloc(1, sizeof(*p));
  if (!p)
    return None;
  /* Use the per-conn xid allocator (same as XCreateWindow) so pixmap
   * ids never collide across wasm processes. Earlier this counter was
   * a TU-local 0x30000001++ which gave every connection the SAME id
   * range -- twm's siconifyPm (id 30000001) got clobbered by xeyes's
   * first pixmap on the JS-side `pixmaps` Map, so XCopyPlane drew the
   * wrong canvas into icon-manager rows. */
  p->id = emx11_next_xid(dpy);
  p->width = width;
  p->height = height;
  p->depth = depth;
  p->refcount = 1;
  p->next = g_pixmaps;
  g_pixmaps = p;
  emx11_js_pixmap_create(p->id, (int)width, (int)height, (int)depth);
  return p->id;
}

int XFreePixmap(Display* dpy, Pixmap pixmap) {
  (void)dpy;
  EmxPixmap* p = pixmap_find(pixmap);
  if (!p)
    return 1;
  if (p->refcount > 1) {
    /* Some window still holds this pixmap as its background tile;
     * defer destruction. The window's set_bg_pixmap unbind path
     * (or window destroy) will release the held reference and
     * trigger the real free. */
    p->refcount--;
    return 1;
  }
  /* refcount == 1 (the XCreatePixmap-issued one): truly destroy. */
  EmxPixmap** prev = &g_pixmaps;
  while (*prev && (*prev)->id != pixmap)
    prev = &(*prev)->next;
  if (*prev) {
    EmxPixmap* doomed = *prev;
    *prev = doomed->next;
    free(doomed);
    emx11_js_pixmap_destroy(pixmap);
  }
  return 1;
}

/* Window-side hooks for "this window is now using pm as bg" / "no
 * longer using it". window.c must call these so the canvas survives a
 * client XFreePixmap that races the bg binding. */
void emx11_pixmap_acquire(Pixmap id) {
  if (id == 0)
    return;
  EmxPixmap* p = pixmap_find(id);
  if (p)
    p->refcount++;
}

void emx11_pixmap_release(Display* dpy, Pixmap id) {
  if (id == 0)
    return;
  /* Mirrors XFreePixmap's decrement-or-destroy: a window letting go
   * of a pixmap whose creator already called XFreePixmap should
   * actually destroy the canvas now. */
  XFreePixmap(dpy, id);
}

/* Internal accessors --------------------------------------------------------
 */

Bool emx11_pixmap_exists(Pixmap id) { return pixmap_find(id) != NULL; }

unsigned int emx11_pixmap_depth(Pixmap id) {
  EmxPixmap* p = pixmap_find(id);
  return p ? p->depth : 0;
}

/* -- Pixmap-from-bitmap-data -- */

Pixmap XCreatePixmapFromBitmapData(Display* dpy,
                                   Drawable d,
                                   char* data,
                                   unsigned int w,
                                   unsigned int h,
                                   unsigned long fg,
                                   unsigned long bg,
                                   unsigned int depth) {
  Pixmap pm = XCreatePixmap(dpy, d, w, h, depth);
  if (pm == None || !data || w == 0 || h == 0)
    return pm;
  int bpl = (int)((w + 7u) / 8u);
  int data_len = bpl * (int)h;
  emx11_js_put_image(pm,
                     0,
                     0,
                     w,
                     h,
                     XYBitmap,
                     1,
                     bpl,
                     (const unsigned char*)data,
                     data_len,
                     fg,
                     bg);
  return pm;
}

int XReadBitmapFileData(_Xconst char* filename,
                        unsigned int* w,
                        unsigned int* h,
                        unsigned char** data,
                        int* x_hot,
                        int* y_hot) {
  (void)filename;
  if (w)
    *w = 0;
  if (h)
    *h = 0;
  if (data)
    *data = NULL;
  if (x_hot)
    *x_hot = -1;
  if (y_hot)
    *y_hot = -1;
  return BitmapFileInvalid;
}

int XReadBitmapFile(Display* dpy,
                    Drawable d,
                    _Xconst char* filename,
                    unsigned int* w,
                    unsigned int* h,
                    Pixmap* bitmap_return,
                    int* x_hot,
                    int* y_hot) {
  (void)dpy;
  (void)d;
  return XReadBitmapFileData(filename, w, h, NULL, x_hot, y_hot) == 0
           ? BitmapSuccess
           : BitmapFileInvalid;
  (void)bitmap_return;
}

int XWriteBitmapFile(Display* dpy,
                     _Xconst char* filename,
                     Pixmap bitmap,
                     unsigned int w,
                     unsigned int h,
                     int x_hot,
                     int y_hot) {
  (void)dpy;
  (void)filename;
  (void)bitmap;
  (void)w;
  (void)h;
  (void)x_hot;
  (void)y_hot;
  return BitmapNoMemory;
}

Pixmap XCreateBitmapFromData(Display* dpy,
                             Drawable d,
                             _Xconst char* data,
                             unsigned int width,
                             unsigned int height) {
  return XCreatePixmapFromBitmapData(
    dpy, d, (char*)data, width, height, 1, 0, 1);
}

/* -- XImage pipeline -- */

#define ROUNDUP(nbytes, pad) (((((nbytes) - 1) + (pad)) / (pad)) * (pad))

static int _emx11_bits_per_pixel(Display* dpy, int depth) {
  ScreenFormat* fmt = dpy->pixmap_format;
  for (int i = dpy->nformats; i > 0; i--, fmt++) {
    if (fmt->depth == depth)
      return fmt->bits_per_pixel;
  }
  if (depth <= 1)
    return 1;
  if (depth <= 4)
    return 4;
  if (depth <= 8)
    return 8;
  if (depth <= 16)
    return 16;
  return 32;
}

int _XInitImageFuncPtrs(XImage* image);

XImage* XCreateImage(Display* dpy,
                     Visual* visual,
                     unsigned int depth,
                     int format,
                     int offset,
                     char* data,
                     unsigned int width,
                     unsigned int height,
                     int bitmap_pad,
                     int bytes_per_line) {
  if (depth == 0 || depth > 32)
    return NULL;
  if (format != XYBitmap && format != XYPixmap && format != ZPixmap)
    return NULL;
  if (format == XYBitmap && depth != 1)
    return NULL;
  if (bitmap_pad != 8 && bitmap_pad != 16 && bitmap_pad != 32)
    return NULL;
  if (offset < 0)
    return NULL;

  XImage* img = calloc(1, sizeof(*img));
  if (!img)
    return NULL;

  img->width = (int)width;
  img->height = (int)height;
  img->format = format;
  img->depth = (int)depth;
  img->data = data;
  img->xoffset = offset;
  img->bitmap_pad = bitmap_pad;

  img->byte_order = dpy->byte_order;
  img->bitmap_unit = dpy->bitmap_unit;
  img->bitmap_bit_order = dpy->bitmap_bit_order;

  if (visual) {
    img->red_mask = visual->red_mask;
    img->green_mask = visual->green_mask;
    img->blue_mask = visual->blue_mask;
  }

  int bpp = (format == ZPixmap) ? _emx11_bits_per_pixel(dpy, (int)depth) : 1;
  img->bits_per_pixel = bpp;

  int min_bpl;
  if (format == ZPixmap)
    min_bpl = ROUNDUP(bpp * (int)width, bitmap_pad) / 8;
  else
    min_bpl = ROUNDUP((int)width + offset, bitmap_pad) / 8;

  if (bytes_per_line == 0)
    img->bytes_per_line = min_bpl;
  else if (bytes_per_line < min_bpl) {
    free(img);
    return NULL;
  } else
    img->bytes_per_line = bytes_per_line;

  _XInitImageFuncPtrs(img);
  return img;
}

static unsigned long _emx11_get_pixel(XImage* img, int x, int y) {
  unsigned char* p =
    (unsigned char*)img->data + y * img->bytes_per_line + x * 4;
  return ((unsigned long)p[2] << 16) | ((unsigned long)p[1] << 8) |
         (unsigned long)p[0];
}

static int _emx11_put_pixel(XImage* img, int x, int y, unsigned long pixel) {
  unsigned char* p =
    (unsigned char*)img->data + y * img->bytes_per_line + x * 4;
  p[0] = (unsigned char)(pixel & 0xff);
  p[1] = (unsigned char)((pixel >> 8) & 0xff);
  p[2] = (unsigned char)((pixel >> 16) & 0xff);
  p[3] = 0xff;
  return 1;
}

static int _emx11_destroy_image(XImage* img) {
  free(img->data);
  img->data = NULL;
  free(img);
  return 1;
}

int _XInitImageFuncPtrs(XImage* image) {
  if (!image)
    return 0;
  image->f.get_pixel = _emx11_get_pixel;
  image->f.put_pixel = _emx11_put_pixel;
  image->f.destroy_image = _emx11_destroy_image;
  return 1;
}

XImage* XGetImage(Display* dpy,
                  Drawable d,
                  int x,
                  int y,
                  unsigned int w,
                  unsigned int h,
                  unsigned long plane_mask,
                  int format) {
  (void)dpy;
  (void)d;
  (void)x;
  (void)y;
  (void)plane_mask;
  if (w == 0 || h == 0)
    return NULL;

  int depth = (format == XYBitmap) ? 1 : (int)dpy->screens[0].root_depth;
  XImage* img = XCreateImage(
    dpy, NULL, (unsigned int)depth, format, 0, NULL, w, h, dpy->bitmap_pad, 0);
  if (!img)
    return NULL;

  int data_size = img->bytes_per_line * (int)h;
  img->data = malloc((size_t)data_size);
  if (!img->data) {
    free(img);
    return NULL;
  }
  emx11_js_get_image(d, x, y, w, h, (unsigned char*)img->data, data_size);
  return img;
}
