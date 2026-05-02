/*
 * em-x11 minimal Xrender.h.
 *
 * Just enough surface to compile Xft and Tk's tkUnixRFont.c. Real
 * Xrender's protocol/composition isn't implemented — the only Xrender
 * call Xft routes through is XftDraw, which em-x11 short-circuits to
 * canvas.fillText, so XRender* functions here are stubs.
 */
#ifndef X11_EXTENSIONS_XRENDER_H
#define X11_EXTENSIONS_XRENDER_H

#include <X11/Xlib.h>
#include <X11/Xfuncproto.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef XID Picture;
typedef XID PictFormat;
typedef XID Glyph;
typedef XID GlyphSet;

typedef struct _XRenderDirectFormat {
    short red, redMask, green, greenMask, blue, blueMask, alpha, alphaMask;
} XRenderDirectFormat;

typedef struct _XRenderPictFormat {
    PictFormat            id;
    int                   type;
    int                   depth;
    XRenderDirectFormat   direct;
    Colormap              colormap;
} XRenderPictFormat;

typedef struct _XGlyphInfo {
    unsigned short width;
    unsigned short height;
    short          x;
    short          y;
    short          xOff;
    short          yOff;
} XGlyphInfo;

typedef struct _XRenderColor {
    unsigned short red, green, blue, alpha;
} XRenderColor;

typedef struct _XRenderPictureAttributes {
    int    repeat;
    Picture alpha_map;
    int    alpha_x_origin;
    int    alpha_y_origin;
    int    clip_x_origin;
    int    clip_y_origin;
    Pixmap clip_mask;
    Bool   graphics_exposures;
    int    subwindow_mode;
    int    poly_edge;
    int    poly_mode;
    Atom   dither;
    Bool   component_alpha;
} XRenderPictureAttributes;

#define PictTypeIndexed 0
#define PictTypeDirect  1

#define PictStandardARGB32 0
#define PictStandardRGB24  1
#define PictStandardA8     2
#define PictStandardA4     3
#define PictStandardA1     4
#define PictStandardNUM    5

extern Bool XRenderQueryExtension(Display *dpy, int *event_base, int *error_base);
extern Status XRenderQueryVersion(Display *dpy, int *major, int *minor);
extern XRenderPictFormat *XRenderFindVisualFormat(Display *dpy, _Xconst Visual *visual);
extern XRenderPictFormat *XRenderFindStandardFormat(Display *dpy, int format);
extern Picture XRenderCreatePicture(Display *dpy, Drawable d,
                                    _Xconst XRenderPictFormat *format,
                                    unsigned long valuemask,
                                    _Xconst XRenderPictureAttributes *attrs);
extern void XRenderFreePicture(Display *dpy, Picture p);
extern void XRenderComposite(Display *dpy, int op,
                             Picture src, Picture mask, Picture dst,
                             int sx, int sy, int mx, int my,
                             int dx, int dy, unsigned int w, unsigned int h);
extern void XRenderFillRectangle(Display *dpy, int op, Picture dst,
                                 _Xconst XRenderColor *color,
                                 int x, int y, unsigned int w, unsigned int h);

#ifdef __cplusplus
}
#endif

#endif
