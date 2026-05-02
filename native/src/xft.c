/*
 * em-x11 Xft implementation.
 *
 * Real Xft would talk Xrender protocol with FreeType-rendered glyphs;
 * we route everything through canvas.fillText via the existing
 * emx11_js_draw_string bridge. Glyph indices are kept as 1:1 with
 * Unicode codepoints so XftCharIndex / XftDrawGlyphFontSpec can shuttle
 * codepoints around without a real glyph table.
 *
 * The XftFont carries the CSS string the canvas backend wants ("13px
 * sans-serif") plus ascent/descent/height computed from canvas
 * measureText, so XftGlyphExtents and Tk_MeasureChars stay accurate
 * with respect to what the renderer will actually paint.
 */

#include "emx11_internal.h"
#include <X11/Xft/Xft.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- UTF-8 emit for codepoints ------------------------------------------- */

static int utf8_emit(unsigned int cp, char *dst) {
    if (cp < 0x80) { dst[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp < 0x110000) {
        dst[0] = (char)(0xF0 | (cp >> 18));
        dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* -- pattern -> CSS ------------------------------------------------------- */

/* Identify CSS generics. Browsers honour these unquoted; everything else
 * (vendor names, system fonts, names with spaces) MUST be quoted in a
 * CSS `font-family` list — `font: 14px Microsoft YaHei` would parse as
 * three independent family tokens, only the last of which the browser
 * recognises. */
static int family_is_css_generic(const char *s) {
    return strcmp(s, "sans-serif") == 0 ||
           strcmp(s, "serif")      == 0 ||
           strcmp(s, "monospace")  == 0 ||
           strcmp(s, "cursive")    == 0 ||
           strcmp(s, "fantasy")    == 0 ||
           strcmp(s, "system-ui")  == 0 ||
           strcmp(s, "ui-sans-serif") == 0 ||
           strcmp(s, "ui-serif")   == 0 ||
           strcmp(s, "ui-monospace") == 0 ||
           strcmp(s, "math")       == 0 ||
           strcmp(s, "emoji")      == 0;
}

/* Pick a CSS generic that's "shaped like" a vendor name, used as the
 * automatic last-resort fallback we append to every list. Helvetica /
 * Arial / Liberation Sans → sans-serif; Courier / Consolas / Cascadia
 * → monospace; Times / Cambria / Georgia → serif. The check is a
 * lowercase substring scan because vendor names ship under wildly
 * inconsistent casing across X clients. */
static const char *vendor_to_generic(const char *name) {
    if (!name || !*name) return "sans-serif";
    char buf[96];
    size_t n = strlen(name);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)name[i]);
    buf[n] = '\0';
    if (strstr(buf, "mono") || strstr(buf, "courier") ||
        strstr(buf, "consolas") || strstr(buf, "cascadia") ||
        strstr(buf, "fixed") || strstr(buf, "terminal") ||
        strstr(buf, "typewriter") || strstr(buf, "menlo") ||
        strstr(buf, "fira code") || strstr(buf, "source code") ||
        strstr(buf, "jetbrains")) {
        return "monospace";
    }
    if (strstr(buf, "times") || strstr(buf, "serif") ||
        strstr(buf, "roman") || strstr(buf, "charter") ||
        strstr(buf, "georgia") || strstr(buf, "cambria") ||
        strstr(buf, "garamond") || strstr(buf, "palatino")) {
        return "serif";
    }
    return "sans-serif";
}

/* Append `name` to the CSS family list at *cursor. Quotes vendor names,
 * passes generics through verbatim, and prepends a leading ", " when
 * the list is non-empty. Truncates silently if outlen is hit; the buffer
 * is sized at 256 bytes which fits ~12 typical family names. */
static void append_family(char *out, size_t outlen, size_t *cursor,
                          const char *name) {
    if (!name || !*name) return;
    size_t pos = *cursor;
    if (pos >= outlen) return;
    /* Skip duplicates (case-insensitive against what's already in the
     * list) so the user-provided list, our keyword-derived hint, and
     * the trailing generic don't all stack up. */
    {
        char tmp[96];
        size_t nl = strlen(name);
        if (nl >= sizeof(tmp)) nl = sizeof(tmp) - 1;
        for (size_t i = 0; i < nl; i++) tmp[i] = (char)tolower((unsigned char)name[i]);
        tmp[nl] = '\0';
        char hay[256];
        size_t hl = pos < sizeof(hay) ? pos : sizeof(hay) - 1;
        for (size_t i = 0; i < hl; i++) hay[i] = (char)tolower((unsigned char)out[i]);
        hay[hl] = '\0';
        if (strstr(hay, tmp)) return;
    }
    int need_comma = pos > 0;
    int generic = family_is_css_generic(name);
    int written;
    if (generic) {
        written = snprintf(out + pos, outlen - pos, "%s%s",
                           need_comma ? ", " : "", name);
    } else {
        /* Quote vendor names. We assume `name` doesn't contain a literal
         * `"`; X font names never do. */
        written = snprintf(out + pos, outlen - pos, "%s\"%s\"",
                           need_comma ? ", " : "", name);
    }
    if (written < 0) return;
    *cursor = pos + (size_t)written;
    if (*cursor >= outlen) *cursor = outlen - 1;
}

/* Build the CSS family list from a FcPattern. Walks every FC_FAMILY
 * value (Tk's TkpGetFontFromAttributes only adds one, but FcNameParse
 * happily fans out a comma-separated list, and rich-text consumers
 * may stack several aliases via FcPatternAddString). Then appends a
 * keyword-derived generic so missing-font fallback always lands on a
 * legible face. */
static void build_family_list(char *out, size_t outlen, FcPattern *pat) {
    out[0] = '\0';
    size_t cursor = 0;
    FcChar8 *first = NULL;
    int      n;
    for (n = 0; ; n++) {
        FcChar8 *fam = NULL;
        if (FcPatternGetString(pat, FC_FAMILY, n, &fam) != FcResultMatch) break;
        if (!fam || !*fam) continue;
        if (n == 0) first = fam;
        append_family(out, outlen, &cursor, (const char *)fam);
    }
    /* Trailing generic: don't double-add if the user's list already ends
     * in one. */
    const char *generic = vendor_to_generic(first ? (const char *)first : "");
    append_family(out, outlen, &cursor, generic);
    if (cursor == 0) {
        snprintf(out, outlen, "sans-serif");
    }
}

static int pixel_size_from_pattern(FcPattern *pat) {
    int    iv;
    double dv;
    /* PIXEL_SIZE wins over SIZE — caller already gave us pixels. */
    if (FcPatternGetInteger(pat, FC_PIXEL_SIZE, 0, &iv) == FcResultMatch && iv > 0)
        return iv;
    if (FcPatternGetDouble(pat, FC_PIXEL_SIZE, 0, &dv) == FcResultMatch && dv > 0)
        return (int)(dv + 0.5);
    /* SIZE is in *points*. Tk's TkpGetFontFromAttributes feeds us
     * faPtr->size (points) via XftPatternAddDouble(XFT_SIZE, ...), so we
     * have to convert. CSS px is fixed at 96 DPI regardless of OS
     * scaling, so 1pt = 96/72 px = 4/3 px. Without this conversion a
     * 9-point default font renders at 9 css-px and is unreadably small,
     * especially on HiDPI / OS-scaled displays. */
    if (FcPatternGetDouble(pat, FC_SIZE, 0, &dv) == FcResultMatch && dv > 0)
        return (int)(dv * 96.0 / 72.0 + 0.5);
    if (FcPatternGetInteger(pat, FC_SIZE, 0, &iv) == FcResultMatch && iv > 0)
        return (int)(iv * 96.0 / 72.0 + 0.5);
    return 13;
}

static void build_css_from_pattern(char *out, size_t outlen, FcPattern *pat,
                                   int *pixel_out) {
    int slant  = FC_SLANT_ROMAN;
    int weight = FC_WEIGHT_MEDIUM;
    FcPatternGetInteger(pat, FC_SLANT,  0, &slant);
    FcPatternGetInteger(pat, FC_WEIGHT, 0, &weight);
    int pixel = pixel_size_from_pattern(pat);
    if (pixel_out) *pixel_out = pixel;

    char families[256];
    build_family_list(families, sizeof(families), pat);

    const char *slant_s  = slant >= FC_SLANT_OBLIQUE ? "oblique " :
                           slant >= FC_SLANT_ITALIC  ? "italic "  : "";
    /* Real CSS weight numbers — closer to fontconfig's 0..210 mapping
     * than just bold/non-bold. Browsers will pick the nearest available
     * face for vendor families that ship multiple weights (Inter,
     * Source Sans, etc.), which a rich-text editor needs. */
    const char *weight_s;
    char weight_buf[8];
    if      (weight >= FC_WEIGHT_BLACK)    weight_s = "900 ";
    else if (weight >= FC_WEIGHT_BOLD)     weight_s = "bold ";
    else if (weight >= FC_WEIGHT_DEMIBOLD) weight_s = "600 ";
    else if (weight >= FC_WEIGHT_MEDIUM)   weight_s = "";
    else if (weight >= FC_WEIGHT_REGULAR)  weight_s = "";
    else if (weight >= FC_WEIGHT_LIGHT)    weight_s = "300 ";
    else                                   weight_s = "200 ";
    (void)weight_buf;
    snprintf(out, outlen, "%s%s%dpx %s", slant_s, weight_s, pixel, families);
}

/* -- XftFont ------------------------------------------------------------- */

XftFont *XftFontOpenPattern(Display *dpy, FcPattern *pattern) {
    (void)dpy;
    if (!pattern) return NULL;
    XftFont *f = (XftFont *)calloc(1, sizeof(XftFont));
    if (!f) return NULL;

    /* Take ownership of `pattern` per upstream Xft semantics: the caller
     * gives us the pattern; we destroy it in XftFontClose. */
    f->pattern = pattern;
    f->charset = NULL;

    int pixel = 0;
    build_css_from_pattern(f->css, sizeof(f->css), pattern, &pixel);
    f->pixel_size = pixel > 0 ? pixel : 13;

    int ascent = 0, descent = 0, max_w = 0;
    int widths[95] = {0};
    emx11_js_measure_font(f->css, &ascent, &descent, &max_w, widths);
    f->ascent  = ascent;
    f->descent = descent;
    f->height  = ascent + descent;
    f->max_advance_width = max_w > 0 ? max_w : (f->pixel_size * 2 / 3);

    /* Stamp pixelsize back into the pattern so Tk's GetTkFontAttributes,
     * which queries XFT_PIXEL_SIZE on the returned font's pattern, can
     * recover it. */
    int pixel_check;
    if (FcPatternGetInteger(pattern, FC_PIXEL_SIZE, 0, &pixel_check) != FcResultMatch) {
        double pxd;
        if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &pxd) != FcResultMatch) {
            FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)f->pixel_size);
        }
    }
    return f;
}

XftFont *XftFontOpenName(Display *dpy, int screen, const char *name) {
    (void)screen;
    if (!name) return NULL;
    FcPattern *p = FcNameParse((const FcChar8 *)name);
    if (!p) return NULL;
    return XftFontOpenPattern(dpy, p);
}

XftFont *XftFontOpenXlfd(Display *dpy, int screen, const char *xlfd) {
    (void)screen;
    FcPattern *p = XftXlfdParse(xlfd, FcFalse, FcFalse);
    if (!p) return NULL;
    return XftFontOpenPattern(dpy, p);
}

XftFont *XftFontMatch(Display *dpy, int screen, FcPattern *pattern, FcResult *result) {
    (void)dpy; (void)screen;
    if (result) *result = FcResultMatch;
    /* Caller retains `pattern`; return a duplicate they can pass to
     * XftFontOpenPattern. Mirrors upstream Xft.h. */
    return (XftFont *)FcPatternDuplicate(pattern);
}

XftFont *XftFontOpen(Display *dpy, int screen, ...) {
    FcPattern *p = FcPatternCreate();
    if (!p) return NULL;
    va_list ap;
    va_start(ap, screen);
    while (1) {
        const char *object = va_arg(ap, const char *);
        if (!object) break;
        FcType type = (FcType)va_arg(ap, int);
        switch (type) {
        case FcTypeInteger: FcPatternAddInteger(p, object, va_arg(ap, int)); break;
        case FcTypeDouble:  FcPatternAddDouble (p, object, va_arg(ap, double)); break;
        case FcTypeString:  FcPatternAddString (p, object, va_arg(ap, const FcChar8 *)); break;
        case FcTypeBool:    FcPatternAddBool   (p, object, va_arg(ap, int)); break;
        case FcTypeMatrix:  FcPatternAddMatrix (p, object, va_arg(ap, const FcMatrix *)); break;
        case FcTypeCharSet: FcPatternAddCharSet(p, object, va_arg(ap, const FcCharSet *)); break;
        default: va_arg(ap, void *); break;
        }
    }
    va_end(ap);
    return XftFontOpenPattern(dpy, p);
}

void XftFontClose(Display *dpy, XftFont *font) {
    (void)dpy;
    if (!font) return;
    if (font->pattern) FcPatternDestroy(font->pattern);
    if (font->charset) FcCharSetDestroy(font->charset);
    free(font);
}

/* -- XftXlfdParse -- minimal pulls family / pixel-size from the XLFD ------ */

static int xlfd_field_copy(const char *src, int field, char *dst, size_t dlen) {
    if (!src || src[0] != '-' || dlen == 0) return 0;
    int f = 0;
    const char *p = src + 1;
    const char *start = p;
    while (1) {
        if (*p == '-' || *p == '\0') {
            ++f;
            if (f == field) {
                size_t n = (size_t)(p - start);
                if (n >= dlen) n = dlen - 1;
                memcpy(dst, start, n);
                dst[n] = '\0';
                return 1;
            }
            if (*p == '\0') return 0;
            start = p + 1;
        }
        p++;
    }
}

FcPattern *XftXlfdParse(const char *xlfd, FcBool ignore_scalable, FcBool complete) {
    (void)ignore_scalable; (void)complete;
    if (!xlfd || xlfd[0] != '-') {
        /* Not an XLFD. Real Xft returns NULL here; Tk's TkpGetNativeFont
         * relies on that to fall through to TkpGetFontFromAttributes,
         * which is where short names like "Helvetica 14 bold" actually
         * get parsed (by tkFont.c, not by us). If we accept the string
         * and stuff in a default family, Tk thinks the font already
         * resolved and never reaches the real attribute-driven path. */
        return NULL;
    }
    FcPattern *p = FcPatternCreate();
    if (!p) return NULL;
    char buf[64];
    if (xlfd_field_copy(xlfd, 2, buf, sizeof(buf)) && buf[0] && buf[0] != '*') {
        FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)buf);
    } else {
        FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"sans-serif");
    }
    if (xlfd_field_copy(xlfd, 3, buf, sizeof(buf))) {
        if (strstr(buf, "bold"))
            FcPatternAddInteger(p, FC_WEIGHT, FC_WEIGHT_BOLD);
    }
    if (xlfd_field_copy(xlfd, 4, buf, sizeof(buf))) {
        if (buf[0] == 'i') FcPatternAddInteger(p, FC_SLANT, FC_SLANT_ITALIC);
        else if (buf[0] == 'o') FcPatternAddInteger(p, FC_SLANT, FC_SLANT_OBLIQUE);
    }
    if (xlfd_field_copy(xlfd, 7, buf, sizeof(buf)) && buf[0] && buf[0] != '*') {
        int v = atoi(buf);
        if (v > 0) FcPatternAddDouble(p, FC_PIXEL_SIZE, (double)v);
    }
    return p;
}

FcBool XftNameUnparse(FcPattern *pat, char *dest, int len) {
    if (!dest || len <= 0) return FcFalse;
    FcChar8 *family = NULL;
    FcPatternGetString(pat, FC_FAMILY, 0, &family);
    int pixel = pixel_size_from_pattern(pat);
    snprintf(dest, len, "%s:pixelsize=%d",
             family ? (const char *)family : "sans-serif", pixel);
    return FcTrue;
}

XftFontSet *XftListFonts(Display *dpy, int screen, ...) {
    (void)dpy; (void)screen;
    return FcFontList(NULL, NULL, NULL);
}

/* -- init / probes -------------------------------------------------------- */

FcBool XftInit(const char *config) { (void)config; return FcInit(); }
FcBool XftInitFtLibrary(void) { return FcTrue; }
int    XftGetVersion(void) { return 20313; }
FcBool XftDefaultHasRender(Display *dpy) { (void)dpy; return FcTrue; }

void XftDefaultSubstitute(Display *dpy, int screen, FcPattern *pattern) {
    (void)dpy; (void)screen;
    FcDefaultSubstitute(pattern);
}

/* -- glyph indexing / metrics -------------------------------------------- */

FT_UInt XftCharIndex(Display *dpy, XftFont *font, FcChar32 ucs4) {
    (void)dpy; (void)font;
    /* 1:1 mapping. The codepoint travels through Tk's spec arrays and
     * comes back to us in XftDrawGlyphFontSpec, where we reconstitute
     * UTF-8 for the canvas backend. */
    return (FT_UInt)ucs4;
}

FcBool XftCharExists(Display *dpy, XftFont *font, FcChar32 ucs4) {
    (void)dpy; (void)font; (void)ucs4;
    return FcTrue;
}

void XftGlyphExtents(Display *dpy, XftFont *font, _Xconst FT_UInt *glyphs,
                     int nglyphs, XGlyphInfo *extents) {
    (void)dpy;
    if (!extents) return;
    memset(extents, 0, sizeof(*extents));
    if (!font || !glyphs || nglyphs <= 0) return;

    char  buf[8 * 32];           /* up to 32 codepoints in a quick path */
    char *p = buf;
    char *heap = NULL;
    int   cap = (int)sizeof(buf);
    if (nglyphs > 32) {
        cap = nglyphs * 4 + 1;
        heap = (char *)malloc((size_t)cap);
        if (!heap) return;
        p = heap;
    }
    int len = 0;
    for (int i = 0; i < nglyphs; i++) {
        len += utf8_emit((unsigned int)glyphs[i], p + len);
    }
    int width = emx11_js_measure_string(font->css, p, len);
    extents->width  = (unsigned short)(width > 0 ? width : 0);
    extents->height = (unsigned short)(font->ascent + font->descent);
    extents->x      = 0;
    extents->y      = (short)font->ascent;
    extents->xOff   = (short)width;
    extents->yOff   = 0;
    if (heap) free(heap);
}

/* -- text extents -------------------------------------------------------- */

static void extents_from_string(XftFont *font, const char *text, int len,
                                XGlyphInfo *out) {
    int width = (text && len > 0)
                ? emx11_js_measure_string(font->css, text, len) : 0;
    out->width  = (unsigned short)(width > 0 ? width : 0);
    out->height = (unsigned short)(font->ascent + font->descent);
    out->x      = 0;
    out->y      = (short)font->ascent;
    out->xOff   = (short)width;
    out->yOff   = 0;
}

void XftTextExtents8(Display *dpy, XftFont *font, _Xconst FcChar8 *string,
                     int len, XGlyphInfo *extents) {
    (void)dpy;
    if (!extents) return;
    memset(extents, 0, sizeof(*extents));
    if (!font) return;
    extents_from_string(font, (const char *)string, len, extents);
}

void XftTextExtentsUtf8(Display *dpy, XftFont *font, _Xconst FcChar8 *string,
                        int len, XGlyphInfo *extents) {
    XftTextExtents8(dpy, font, string, len, extents);
}

void XftTextExtents16(Display *dpy, XftFont *font, _Xconst FcChar16 *string,
                      int len, XGlyphInfo *extents) {
    (void)dpy;
    if (!extents) return;
    memset(extents, 0, sizeof(*extents));
    if (!font || !string || len <= 0) return;
    int cap = len * 4 + 1;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) return;
    int n = 0;
    for (int i = 0; i < len; i++) n += utf8_emit(string[i], buf + n);
    extents_from_string(font, buf, n, extents);
    free(buf);
}

void XftTextExtents32(Display *dpy, XftFont *font, _Xconst FcChar32 *string,
                      int len, XGlyphInfo *extents) {
    (void)dpy;
    if (!extents) return;
    memset(extents, 0, sizeof(*extents));
    if (!font || !string || len <= 0) return;
    int cap = len * 4 + 1;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) return;
    int n = 0;
    for (int i = 0; i < len; i++) n += utf8_emit((unsigned int)string[i], buf + n);
    extents_from_string(font, buf, n, extents);
    free(buf);
}

/* -- XftDraw ------------------------------------------------------------- */

struct _XftDraw {
    Display *dpy;
    Drawable drawable;
    Visual  *visual;
    Colormap colormap;
};

XftDraw *XftDrawCreate(Display *dpy, Drawable drawable, Visual *visual,
                       Colormap colormap) {
    XftDraw *d = (XftDraw *)calloc(1, sizeof(XftDraw));
    if (!d) return NULL;
    d->dpy = dpy;
    d->drawable = drawable;
    d->visual = visual;
    d->colormap = colormap;
    return d;
}

XftDraw *XftDrawCreateBitmap(Display *dpy, Pixmap bitmap) {
    return XftDrawCreate(dpy, bitmap, NULL, 0);
}

void XftDrawDestroy(XftDraw *draw) { free(draw); }

FcBool XftDrawChange(XftDraw *draw, Drawable drawable) {
    if (!draw) return FcFalse;
    draw->drawable = drawable;
    return FcTrue;
}

Display *XftDrawDisplay(XftDraw *draw) { return draw ? draw->dpy : NULL; }
Drawable XftDrawDrawable(XftDraw *draw) { return draw ? draw->drawable : 0; }
Visual  *XftDrawVisual(XftDraw *draw)  { return draw ? draw->visual : NULL; }
Colormap XftDrawColormap(XftDraw *draw) { return draw ? draw->colormap : 0; }

void XftDrawSetClip(XftDraw *draw, Region region) {
    /* Region clipping isn't honoured by the canvas backend right now;
     * Tk's clipping is enforced at the window level via paint reissue,
     * not at the text-draw level, so this is safe to ignore. */
    (void)draw; (void)region;
}

FcBool XftDrawSetClipRectangles(XftDraw *draw, int xOrigin, int yOrigin,
                                _Xconst XRectangle *rects, int n) {
    (void)draw; (void)xOrigin; (void)yOrigin; (void)rects; (void)n;
    return FcTrue;
}

/* -- string draw (UTF-8 / 8-bit) ---------------------------------------- */

void XftDrawString8(XftDraw *draw, _Xconst XftColor *color, XftFont *font,
                    int x, int y, _Xconst FcChar8 *string, int len) {
    XftDrawStringUtf8(draw, color, font, x, y, string, len);
}

void XftDrawStringUtf8(XftDraw *draw, _Xconst XftColor *color, XftFont *font,
                       int x, int y, _Xconst FcChar8 *string, int len) {
    if (!draw || !font || !string || len <= 0) return;
    unsigned long fg = color ? color->pixel : 0;
    emx11_js_draw_string((Window)draw->drawable, x, y, font->css,
                         (const char *)string, len, fg, 0, 0);
}

void XftDrawString16(XftDraw *draw, _Xconst XftColor *color, XftFont *font,
                     int x, int y, _Xconst FcChar16 *string, int len) {
    if (!draw || !font || !string || len <= 0) return;
    int cap = len * 4 + 1;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) return;
    int n = 0;
    for (int i = 0; i < len; i++) n += utf8_emit(string[i], buf + n);
    unsigned long fg = color ? color->pixel : 0;
    emx11_js_draw_string((Window)draw->drawable, x, y, font->css, buf, n, fg, 0, 0);
    free(buf);
}

/* -- glyph draw --------------------------------------------------------- */

void XftDrawGlyphs(XftDraw *draw, _Xconst XftColor *color, XftFont *font,
                   int x, int y, _Xconst FT_UInt *glyphs, int nglyphs) {
    if (!draw || !font || !glyphs || nglyphs <= 0) return;
    int cap = nglyphs * 4 + 1;
    char  stack[256];
    char *buf = stack;
    char *heap = NULL;
    if (cap > (int)sizeof(stack)) {
        heap = (char *)malloc((size_t)cap);
        if (!heap) return;
        buf = heap;
    }
    int n = 0;
    for (int i = 0; i < nglyphs; i++) {
        n += utf8_emit((unsigned int)glyphs[i], buf + n);
    }
    unsigned long fg = color ? color->pixel : 0;
    emx11_js_draw_string((Window)draw->drawable, x, y, font->css, buf, n, fg, 0, 0);
    if (heap) free(heap);
}

void XftDrawGlyphFontSpec(XftDraw *draw, _Xconst XftColor *color,
                          _Xconst XftGlyphFontSpec *specs, int nspec) {
    if (!draw || !specs || nspec <= 0) return;
    /* Group consecutive specs by (font, y) and only break the run when
     * we'd back-step. canvas.fillText is per-call so each break costs a
     * round-trip; in tk the entire visible glyph stream typically shares
     * a font and one y, so the run is the whole call. */
    int i = 0;
    while (i < nspec) {
        XftFont *fnt = specs[i].font;
        if (!fnt) { i++; continue; }
        short  start_x = specs[i].x;
        short  base_y  = specs[i].y;
        char   small[256];
        char  *buf  = small;
        char  *heap = NULL;
        int    cap  = (int)sizeof(small);
        int    used = 0;
        int    j = i;
        short  next_x = start_x;
        while (j < nspec && specs[j].font == fnt && specs[j].y == base_y) {
            unsigned int cp = (unsigned int)specs[j].glyph;
            /* Allow tiny back-steps from rounding (≤1px); larger jumps
             * end the run. */
            if (specs[j].x + 1 < next_x) break;
            if (used + 4 >= cap) {
                int ncap = cap * 2;
                char *np;
                if (heap) {
                    np = (char *)realloc(heap, (size_t)ncap);
                } else {
                    np = (char *)malloc((size_t)ncap);
                    if (np) memcpy(np, small, (size_t)used);
                }
                if (!np) { if (heap) free(heap); return; }
                heap = np; buf = np; cap = ncap;
            }
            used += utf8_emit(cp, buf + used);
            next_x = specs[j].x;
            j++;
        }
        if (used > 0) {
            unsigned long fg = color ? color->pixel : 0;
            emx11_js_draw_string((Window)draw->drawable, start_x, base_y,
                                 fnt->css, buf, used, fg, 0, 0);
        }
        if (heap) free(heap);
        i = (j > i) ? j : i + 1;
    }
}

void XftDrawRect(XftDraw *draw, _Xconst XftColor *color,
                 int x, int y, unsigned int w, unsigned int h) {
    if (!draw || !color) return;
    emx11_js_fill_rect((Window)draw->drawable, x, y, w, h,
                       (unsigned long)color->pixel);
}

/* -- color -------------------------------------------------------------- */

FcBool XftColorAllocValue(Display *dpy, Visual *visual, Colormap cmap,
                          _Xconst XRenderColor *color, XftColor *result) {
    (void)dpy; (void)visual; (void)cmap;
    if (!color || !result) return FcFalse;
    result->color = *color;
    /* Pack 8-bit channels into a 0xRRGGBB pixel; em-x11's TrueColor
     * visual treats pixel values as packed RGB. Alpha is dropped — the
     * canvas backend has no alpha channel for window content. */
    unsigned int r = (color->red   >> 8) & 0xFF;
    unsigned int g = (color->green >> 8) & 0xFF;
    unsigned int b = (color->blue  >> 8) & 0xFF;
    result->pixel = (r << 16) | (g << 8) | b;
    return FcTrue;
}

FcBool XftColorAllocName(Display *dpy, _Xconst Visual *visual, Colormap cmap,
                         _Xconst char *name, XftColor *result) {
    (void)visual; (void)cmap;
    if (!name || !result) return FcFalse;
    XColor xc;
    if (XParseColor(dpy, cmap, name, &xc) == 0) return FcFalse;
    XRenderColor rc = { xc.red, xc.green, xc.blue, 0xFFFF };
    return XftColorAllocValue(dpy, NULL, cmap, &rc, result);
}

void XftColorFree(Display *dpy, Visual *visual, Colormap cmap, XftColor *color) {
    (void)dpy; (void)visual; (void)cmap; (void)color;
}
