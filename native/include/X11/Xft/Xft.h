/*
 * em-x11 minimal Xft.h
 *
 * Subset sufficient for Tk's tkUnixRFont.c. Text rendering bypasses real
 * Xft/FreeType: XftCharIndex returns the Unicode codepoint as the glyph
 * id, and XftDrawGlyphFontSpec / XftDrawGlyphs reconstruct UTF-8 from
 * those codepoints and route through em-x11's canvas.fillText path.
 *
 * XftPattern* / XftResult* / XftFontSet* are aliased to the FcPattern /
 * FcResult / FcFontSet types from fontconfig.h, matching upstream Xft.
 */
#ifndef X11_XFT_XFT_H
#define X11_XFT_XFT_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <fontconfig/fontconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FreeType type aliases (we don't ship real FreeType). */
typedef unsigned int  FT_UInt;
typedef long          FT_Int32;

/* Property name macros — same strings as fontconfig. */
#define XFT_FAMILY        FC_FAMILY
#define XFT_STYLE         FC_STYLE
#define XFT_SLANT         FC_SLANT
#define XFT_WEIGHT        FC_WEIGHT
#define XFT_SIZE          FC_SIZE
#define XFT_PIXEL_SIZE    FC_PIXEL_SIZE
#define XFT_SPACING       FC_SPACING
#define XFT_FOUNDRY       FC_FOUNDRY
#define XFT_ENCODING      FC_ENCODING
#define XFT_MATRIX        FC_MATRIX
#define XFT_CHARSET       FC_CHARSET
#define XFT_LANG          FC_LANG
#define XFT_SCALABLE      FC_SCALABLE
#define XFT_DPI           FC_DPI
#define XFT_RGBA          FC_RGBA
#define XFT_FILE          FC_FILE
#define XFT_RENDER        "render"
/* Note: XFT_HAS_FIXED_ROTATED_PLACEMENT is intentionally NOT defined.
 * Tk 8.6.6's tkUnixRFont.c declares sinA/cosA only in the !defined()
 * branch, then uses them unconditionally in the underline/strikeout
 * code path — defining the macro produces a compile error. We don't
 * implement Xft's rotated glyph placement anyway, so leaving the macro
 * undefined is honest. */

#define XFT_SLANT_ROMAN   FC_SLANT_ROMAN
#define XFT_SLANT_ITALIC  FC_SLANT_ITALIC
#define XFT_SLANT_OBLIQUE FC_SLANT_OBLIQUE

#define XFT_WEIGHT_LIGHT   FC_WEIGHT_LIGHT
#define XFT_WEIGHT_MEDIUM  FC_WEIGHT_MEDIUM
#define XFT_WEIGHT_DEMIBOLD FC_WEIGHT_DEMIBOLD
#define XFT_WEIGHT_BOLD    FC_WEIGHT_BOLD
#define XFT_WEIGHT_BLACK   FC_WEIGHT_BLACK

#define XFT_PROPORTIONAL FC_PROPORTIONAL
#define XFT_MONO         FC_MONO
#define XFT_CHARCELL     FC_CHARCELL

/* Pattern aliases. */
typedef FcPattern  XftPattern;
typedef FcFontSet  XftFontSet;
typedef FcResult   XftResult;
#define XftResultMatch         FcResultMatch
#define XftResultNoMatch       FcResultNoMatch
#define XftResultTypeMismatch  FcResultTypeMismatch
#define XftResultNoId          FcResultNoId

#define XftPatternCreate            FcPatternCreate
#define XftPatternDuplicate         FcPatternDuplicate
#define XftPatternDestroy           FcPatternDestroy
#define XftPatternAddInteger        FcPatternAddInteger
#define XftPatternAddDouble         FcPatternAddDouble
#define XftPatternAddString(p,o,s)  FcPatternAddString((p),(o),(const FcChar8*)(s))
#define XftPatternAddBool           FcPatternAddBool
#define XftPatternAddMatrix         FcPatternAddMatrix
#define XftPatternGetInteger        FcPatternGetInteger
#define XftPatternGetDouble         FcPatternGetDouble
#define XftPatternGetString(p,o,n,sp) \
    FcPatternGetString((p),(o),(n),(FcChar8 **)(sp))
#define XftPatternGetBool           FcPatternGetBool
#define XftPatternGetMatrix         FcPatternGetMatrix

/* The runtime XftFont struct. The first six fields match upstream Xft so
 * Tk's direct field access (->ascent / ->descent / ->height / ->pattern)
 * lines up. We append a CSS string for the canvas backend. */
typedef struct _XftFont {
    int          ascent;
    int          descent;
    int          height;
    int          max_advance_width;
    FcCharSet   *charset;
    FcPattern   *pattern;
    /* em-x11 private tail: */
    char         css[160];
    int          pixel_size;
} XftFont;

typedef struct _XftDraw  XftDraw;

typedef struct _XftColor {
    unsigned long pixel;
    XRenderColor  color;
} XftColor;

typedef struct _XftGlyphFontSpec {
    XftFont *font;
    FT_UInt  glyph;
    short    x;
    short    y;
} XftGlyphFontSpec;

typedef struct _XftGlyphSpec {
    FT_UInt glyph;
    short   x;
    short   y;
} XftGlyphSpec;

/* --- init / version / probe --------------------------------------------- */
FcBool XftInit(const char *config);
FcBool XftInitFtLibrary(void);
int    XftGetVersion(void);
FcBool XftDefaultHasRender(Display *dpy);
void   XftDefaultSubstitute(Display *dpy, int screen, FcPattern *pattern);

/* --- font open / close / pattern utility -------------------------------- */
XftFont *XftFontOpen(Display *dpy, int screen, ...);
XftFont *XftFontOpenName(Display *dpy, int screen, const char *name);
XftFont *XftFontOpenXlfd(Display *dpy, int screen, const char *xlfd);
XftFont *XftFontOpenPattern(Display *dpy, FcPattern *pattern);
XftFont *XftFontMatch(Display *dpy, int screen, FcPattern *pattern, FcResult *result);
void     XftFontClose(Display *dpy, XftFont *font);

FcPattern *XftXlfdParse(const char *xlfd, FcBool ignore_scalable, FcBool complete);
FcBool     XftNameUnparse(FcPattern *pat, char *dest, int len);

XftFontSet *XftListFonts(Display *dpy, int screen, ...);

/* Tk calls XftFontSetDestroy after XftListFonts; in real Xft this is
 * just an alias for FcFontSetDestroy. */
#define XftFontSetDestroy FcFontSetDestroy

/* --- draw --------------------------------------------------------------- */
XftDraw *XftDrawCreate(Display *dpy, Drawable drawable, Visual *visual, Colormap colormap);
XftDraw *XftDrawCreateBitmap(Display *dpy, Pixmap bitmap);
void     XftDrawDestroy(XftDraw *draw);
FcBool   XftDrawChange(XftDraw *draw, Drawable drawable);
Display *XftDrawDisplay(XftDraw *draw);
Drawable XftDrawDrawable(XftDraw *draw);
Visual  *XftDrawVisual(XftDraw *draw);
Colormap XftDrawColormap(XftDraw *draw);

void XftDrawSetClip(XftDraw *draw, Region region);
FcBool XftDrawSetClipRectangles(XftDraw *draw, int xOrigin, int yOrigin,
                                _Xconst XRectangle *rects, int n);

/* Tk-friendly alias used by tkUnixRFont.c (XftClipRegion). */
#define XftClipRegion XftDrawSetClip

/* --- glyph rendering ---------------------------------------------------- */
FT_UInt XftCharIndex(Display *dpy, XftFont *font, FcChar32 ucs4);
FcBool  XftCharExists(Display *dpy, XftFont *font, FcChar32 ucs4);

void XftGlyphExtents(Display *dpy, XftFont *font, _Xconst FT_UInt *glyphs,
                     int nglyphs, XGlyphInfo *extents);

void XftDrawString8 (XftDraw *draw, _Xconst XftColor *color, XftFont *font,
                     int x, int y, _Xconst FcChar8 *string, int len);
void XftDrawStringUtf8(XftDraw *draw, _Xconst XftColor *color, XftFont *font,
                       int x, int y, _Xconst FcChar8 *string, int len);
void XftDrawString16(XftDraw *draw, _Xconst XftColor *color, XftFont *font,
                     int x, int y, _Xconst FcChar16 *string, int len);
void XftDrawGlyphs (XftDraw *draw, _Xconst XftColor *color, XftFont *font,
                    int x, int y, _Xconst FT_UInt *glyphs, int nglyphs);
void XftDrawGlyphFontSpec(XftDraw *draw, _Xconst XftColor *color,
                          _Xconst XftGlyphFontSpec *glyphs, int nglyphs);

void XftDrawRect(XftDraw *draw, _Xconst XftColor *color,
                 int x, int y, unsigned int w, unsigned int h);

void XftTextExtents8 (Display *dpy, XftFont *font,
                      _Xconst FcChar8 *string, int len, XGlyphInfo *extents);
void XftTextExtentsUtf8(Display *dpy, XftFont *font,
                        _Xconst FcChar8 *string, int len, XGlyphInfo *extents);
void XftTextExtents16(Display *dpy, XftFont *font,
                      _Xconst FcChar16 *string, int len, XGlyphInfo *extents);
void XftTextExtents32(Display *dpy, XftFont *font,
                      _Xconst FcChar32 *string, int len, XGlyphInfo *extents);

/* --- color -------------------------------------------------------------- */
FcBool XftColorAllocValue(Display *dpy, Visual *visual, Colormap cmap,
                          _Xconst XRenderColor *color, XftColor *result);
FcBool XftColorAllocName (Display *dpy, _Xconst Visual *visual, Colormap cmap,
                          _Xconst char *name, XftColor *result);
void   XftColorFree(Display *dpy, Visual *visual, Colormap cmap, XftColor *color);

#ifdef __cplusplus
}
#endif

#endif /* X11_XFT_XFT_H */
