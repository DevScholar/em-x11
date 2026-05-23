/*
 * Xlib surface that Tk 8.6 demands beyond what Xaw/Xt already pull in.
 *
 * Everything here is either a real (simplified) implementation where Tk
 * might actually act on the return value, or an honest no-op / empty
 * return where Tk only calls for side-effect (screensaver control, IM
 * plumbing, WM hints on a system with no WM).
 *
 * When a symbol starts mattering for a real feature, promote it to a
 * focused source file (e.g. region.c, xim.c) and delete the stub here.
 */

#include "emx11_internal.h"

#include <X11/Xutil.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* -- Region extras beyond the bounding-box core in xt_stubs.c --------- */

typedef struct _XRegion {
    int   x1, y1, x2, y2;
    int   is_empty;
} EmxRegion;

int XRectInRegion(Region r, int x, int y, unsigned int w, unsigned int h) {
    EmxRegion *er = (EmxRegion *)r;
    if (!er || er->is_empty) return RectangleOut;
    int rx2 = x + (int)w;
    int ry2 = y + (int)h;
    if (rx2 <= er->x1 || x >= er->x2 || ry2 <= er->y1 || y >= er->y2)
        return RectangleOut;
    if (x >= er->x1 && y >= er->y1 && rx2 <= er->x2 && ry2 <= er->y2)
        return RectangleIn;
    return RectanglePart;
}

/* Bounding-rect subtraction is ambiguous -- concave results can't be
 * represented. Be conservative: dst becomes src1 unchanged, which
 * over-redraws but is always correct. Tk uses XSubtractRegion for
 * damage-minimisation; painting more pixels than strictly needed is
 * a performance concern, not correctness. */
int XSubtractRegion(Region src1, Region src2, Region dst) {
    (void)src2;
    EmxRegion *a = (EmxRegion *)src1;
    EmxRegion *d = (EmxRegion *)dst;
    if (!d) return 0;
    if (!a || a->is_empty) {
        d->is_empty = 1;
        d->x1 = d->y1 = d->x2 = d->y2 = 0;
        return 1;
    }
    d->x1 = a->x1; d->y1 = a->y1;
    d->x2 = a->x2; d->y2 = a->y2;
    d->is_empty = a->is_empty;
    return 1;
}

/* -- Visual accessors ------------------------------------------------- */

VisualID XVisualIDFromVisual(Visual *visual) {
    return visual ? visual->visualid : 0;
}

/* Return the single TrueColor visual we always hand out. Tk calls this
 * to enumerate candidates (e.g. to pick a matching 32-bit visual for
 * image import); a one-element list is sufficient. Caller XFrees. */
XVisualInfo *XGetVisualInfo(Display *dpy, long mask, XVisualInfo *template_,
                            int *nitems_return) {
    (void)mask; (void)template_;
    if (nitems_return) *nitems_return = 0;
    if (!dpy) return NULL;
    XVisualInfo *info = calloc(1, sizeof(XVisualInfo));
    if (!info) return NULL;
    Visual *v = dpy->screens[0].root_visual;
    info->visual        = v;
    info->visualid      = v ? v->visualid : 0;
    info->screen        = 0;
    info->depth         = dpy->screens[0].root_depth;
    info->class         = TrueColor;
    info->red_mask      = 0x00ff0000;
    info->green_mask    = 0x0000ff00;
    info->blue_mask     = 0x000000ff;
    info->colormap_size = 256;
    info->bits_per_rgb  = 8;
    if (nitems_return) *nitems_return = 1;
    return info;
}

int XSetWindowColormap(Display *dpy, Window w, Colormap cmap) {
    (void)dpy; (void)w; (void)cmap;
    return 1;                                   /* single-colormap world */
}

/* -- Drawable queries ------------------------------------------------- */

/* -- XImage pixel accessors (ZPixmap, 32bpp BGRA) -------------------- */

/* Visual masks (TrueColor): red=0xff0000 green=0xff00 blue=0xff.
 * Pixel values from Tk_GetColorByValue are 0x00RRGGBB.
 * The data buffer is BGRA byte order (matching the PutImage path). */

static unsigned long _emx11_get_pixel(XImage *img, int x, int y) {
    unsigned char *p = (unsigned char *)img->data + y * img->bytes_per_line + x * 4;
    return ((unsigned long)p[2] << 16) | ((unsigned long)p[1] << 8) | (unsigned long)p[0];
}

static int _emx11_put_pixel(XImage *img, int x, int y, unsigned long pixel) {
    unsigned char *p = (unsigned char *)img->data + y * img->bytes_per_line + x * 4;
    p[0] = (unsigned char)(pixel & 0xff);          /* Blue */
    p[1] = (unsigned char)((pixel >> 8) & 0xff);   /* Green */
    p[2] = (unsigned char)((pixel >> 16) & 0xff);  /* Red */
    p[3] = 0xff;                                    /* Alpha (opaque) */
    return 1;
}

static int _emx11_destroy_image(XImage *img) {
    free(img->data);
    img->data = NULL;
    free(img);
    return 1;
}

int _XInitImageFuncPtrs(XImage *image) {
    if (!image) return 0;
    image->f.get_pixel      = _emx11_get_pixel;
    image->f.put_pixel      = _emx11_put_pixel;
    image->f.destroy_image  = _emx11_destroy_image;
    return 1;
}

/* XGetImage — does NOT read back from the browser compositor (that
 * would require an async GPU round-trip). Instead it returns a
 * zero-filled XImage that callers can write to via XPutPixel and then
 * commit with XPutImage. This is sufficient for the "create blank
 * buffer, stamp pixels, blit" pattern that Tk's checkbutton/radiobutton
 * indicator drawing and ttk theme element building rely on.
 *
 * Real readback for photo-image capture and postscript export can be
 * added later via a Host method that returns RGBA bytes. */
XImage *XGetImage(Display *dpy, Drawable d, int x, int y,
                  unsigned int w, unsigned int h,
                  unsigned long plane_mask, int format) {
    (void)dpy; (void)d; (void)x; (void)y; (void)plane_mask;
    if (w == 0 || h == 0) return NULL;

    XImage *img = XCreateImage(dpy, NULL, 24, format, 0, NULL, w, h, 32, 0);
    if (!img) return NULL;

    _XInitImageFuncPtrs(img);

    int data_size = img->bytes_per_line * (int)h;
    img->data = calloc(1, (size_t)data_size);
    if (!img->data) {
        free(img);
        return NULL;
    }
    return img;
}

/* -- Plural-form drawing wrappers ------------------------------------ */

int XFillRectangles(Display *dpy, Drawable d, GC gc,
                    XRectangle *rectangles, int nrectangles) {
    if (!rectangles) return 0;
    for (int i = 0; i < nrectangles; i++) {
        XFillRectangle(dpy, d, gc, rectangles[i].x, rectangles[i].y,
                       rectangles[i].width, rectangles[i].height);
    }
    return 1;
}

/* Xmu's DrRndRect.c (XmuDrawRoundedRectangle / XmuFillRoundedRectangle)
 * stamps arcs for every corner in a single XDrawArcs / XFillArcs call.
 * Xaw's Command/Toggle widgets then call DrRndRect for every button in a
 * Form, which is how xcalc pulls it in. Delegate to the scalar form. */
int XDrawArcs(Display *dpy, Drawable d, GC gc, XArc *arcs, int narcs) {
    if (!arcs) return 0;
    for (int i = 0; i < narcs; i++) {
        XDrawArc(dpy, d, gc, arcs[i].x, arcs[i].y,
                 arcs[i].width, arcs[i].height,
                 arcs[i].angle1, arcs[i].angle2);
    }
    return 1;
}

int XFillArcs(Display *dpy, Drawable d, GC gc, XArc *arcs, int narcs) {
    if (!arcs) return 0;
    for (int i = 0; i < narcs; i++) {
        XFillArc(dpy, d, gc, arcs[i].x, arcs[i].y,
                 arcs[i].width, arcs[i].height,
                 arcs[i].angle1, arcs[i].angle2);
    }
    return 1;
}

/* -- Grabs -- we have no real input grab; all events go to the focus
 *    window already, so reporting GrabSuccess is truthful.
 *    (XGrabPointer already lives in xaw_stubs.c; only XGrabKeyboard is new.) */

int XGrabKeyboard(Display *dpy, Window grab_window, Bool owner_events,
                  int pointer_mode, int keyboard_mode, Time t) {
    (void)dpy; (void)grab_window; (void)owner_events;
    (void)pointer_mode; (void)keyboard_mode; (void)t;
    return GrabSuccess;
}

/* -- Screensaver controls -- not meaningful in a browser tab -------- */

int XForceScreenSaver(Display *dpy, int mode) {
    (void)dpy; (void)mode;
    return 1;
}

int XResetScreenSaver(Display *dpy) {
    (void)dpy;
    return 1;
}

/* -- XNoOp is literally a server round-trip with no side effect ---- */

int XNoOp(Display *dpy) {
    (void)dpy;
    return 1;
}

/* -- Input-method plumbing.
 *
 * Real XIM lifecycle (XOpenIM/XCreateIC/XSetICFocus/Xutf8LookupString/...)
 * lives in xim.c. The two registration-callback APIs below stay here as
 * noops -- Tk uses them to register a "tell me when an XIM appears"
 * watcher; XOpenIM already succeeds on the first call so the callback
 * never has to fire. */

Bool XRegisterIMInstantiateCallback(Display *dpy, struct _XrmHashBucketRec *rdb,
                                    char *res_name, char *res_class,
                                    XIDProc callback, XPointer client_data) {
    (void)dpy; (void)rdb; (void)res_name; (void)res_class;
    (void)callback; (void)client_data;
    return False;
}

Bool XUnregisterIMInstantiateCallback(Display *dpy, struct _XrmHashBucketRec *rdb,
                                      char *res_name, char *res_class,
                                      XIDProc callback, XPointer client_data) {
    (void)dpy; (void)rdb; (void)res_name; (void)res_class;
    (void)callback; (void)client_data;
    return False;
}

char *XSetIMValues(XIM im, ...) {
    (void)im;
    return NULL;
}

/* -- Host-list / access-control APIs -- browser has no ACL ---------- */

XHostAddress *XListHosts(Display *dpy, int *nhosts_return, Bool *state_return) {
    (void)dpy;
    if (nhosts_return) *nhosts_return = 0;
    if (state_return)  *state_return  = False;
    return NULL;
}

/* -- WM hints that a window manager would read. We don't have one,
 *    and Tk just writes them; swallow silently. */

int XSetIconName(Display *dpy, Window w, _Xconst char *icon_name) {
    (void)dpy; (void)w; (void)icon_name;
    return 1;
}

void XSetWMClientMachine(Display *dpy, Window w, XTextProperty *prop) {
    (void)dpy; (void)w; (void)prop;
}

Status XSetWMColormapWindows(Display *dpy, Window w, Window *colormap_windows,
                             int count) {
    (void)dpy; (void)w; (void)colormap_windows; (void)count;
    return 1;
}

/* -- WM reconfiguration -- the point of going via XReconfigureWMWindow
 *    is to let the WM intercept the request; without one we just apply
 *    it directly. */

Status XReconfigureWMWindow(Display *dpy, Window w, int screen_number,
                            unsigned int mask, XWindowChanges *changes) {
    (void)screen_number;
    if (!changes) return 0;
    return (Status)XConfigureWindow(dpy, w, mask, changes);
}
