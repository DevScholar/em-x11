/*
 * em-x11 font support.
 *
 * X fonts are historically handled by the server (core protocol fonts
 * delivered as bitmap glyphs) or by a separate font server (xfs). Modern
 * X apps mostly use Xft/FreeType/fontconfig on top of XRender. em-x11
 * bypasses all of that and routes text drawing to the browser's
 * canvas.fillText, which gives us free Unicode coverage, real anti-aliased
 * glyphs, and HiDPI correctness for no work.
 *
 * Metrics come from the browser too: at XLoadQueryFont time we call
 * canvas.measureText once per ASCII printable character (32..126) and
 * once for the font-level ascent/descent. The results populate a real
 * XFontStruct with per_char widths, so clients' XTextWidth / XTextExtents
 * compute correctly for proportional fonts as well as monospace.
 *
 * Scope limits:
 *   - Only printable 7-bit ASCII gets precise per-character widths.
 *     Anything outside 32..126 falls back to max_bounds.
 *   - Family mapping follows an explicit XLFD-family-to-CSS-generic
 *     table. Vendor families ("helvetica", "times", "courier") are
 *     resolved to CSS generics ("sans-serif", "serif", "monospace").
 *     Specific family names are NOT forwarded as CSS, because the user
 *     running the browser is unlikely to have a named X font installed
 *     and CSS fallback to a generic is already what we want.
 */

#include "emx11_internal.h"

#include <X11/Xatom.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMX11_MAX_FONTS         64
#define EMX11_PER_CHAR_MIN      32
#define EMX11_PER_CHAR_MAX      126
#define EMX11_PER_CHAR_COUNT    (EMX11_PER_CHAR_MAX - EMX11_PER_CHAR_MIN + 1)

typedef struct EmxFont {
    bool         in_use;
    Font         fid;
    int          pixel_size;
    char         css[128];
    XFontStruct  fs;
    XFontProp    props[1];              /* XA_FONT -> atom of XLFD name */
    XCharStruct  per_char[EMX11_PER_CHAR_COUNT];   /* retained for XTextWidth
                                                    * fast paths that key on
                                                    * the ASCII band */
} EmxFont;

static EmxFont g_fonts[EMX11_MAX_FONTS];

/* -- XLFD parsing ---------------------------------------------------------- */

/* Copy the Nth dash-separated XLFD field into `buf` (NUL-terminated,
 * truncated if too long). Returns true if the field exists. Field
 * numbering is 1-indexed starting after the leading '-'. */
static bool xlfd_field(const char *name, int field, char *buf, size_t buflen) {
    if (!name || name[0] != '-' || buflen == 0) return false;
    int f = 0;
    const char *p = name + 1;
    const char *start = p;
    while (1) {
        if (*p == '-' || *p == '\0') {
            ++f;
            if (f == field) {
                size_t n = (size_t)(p - start);
                if (n >= buflen) n = buflen - 1;
                memcpy(buf, start, n);
                buf[n] = '\0';
                return true;
            }
            if (*p == '\0') return false;
            start = p + 1;
        }
        p++;
    }
}

static int parse_xlfd_pixel_size(const char *name) {
    /* Field 7 is PIXEL_SIZE. */
    char buf[16];
    if (!xlfd_field(name, 7, buf, sizeof(buf))) return 0;
    if (buf[0] == '*' || buf[0] == '\0') return 0;
    int v = atoi(buf);
    return v > 0 ? v : 0;
}

/* Short alias handling: "6x13" means 6-wide 13-pixel. The second number
 * is the pixel size. */
static int parse_alias_pixel_size(const char *name) {
    const char *x = strchr(name, 'x');
    if (!x) return 0;
    int v = atoi(x + 1);
    return v > 0 ? v : 0;
}

static int resolve_pixel_size(const char *name) {
    if (!name || !*name) return 13;
    if (name[0] == '-') {
        int v = parse_xlfd_pixel_size(name);
        if (v > 0) return v;
    }
    int v = parse_alias_pixel_size(name);
    if (v > 0) return v;
    return 13;
}

static void str_tolower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* Map XLFD family (field 2) to a CSS font-family. */
static const char *resolve_css_family(const char *name) {
    char family[64];
    if (!xlfd_field(name, 2, family, sizeof(family))) {
        /* Short alias like "fixed" or "6x13" -- both are monospace. */
        return "monospace";
    }
    if (family[0] == '*' || family[0] == '\0') return "sans-serif";
    str_tolower(family);
    if (strstr(family, "helvetica") || strstr(family, "arial") ||
        strstr(family, "sans")) {
        return "sans-serif";
    }
    if (strstr(family, "times") || strstr(family, "serif") ||
        strstr(family, "roman") || strstr(family, "charter")) {
        return "serif";
    }
    if (strstr(family, "courier") || strstr(family, "mono") ||
        strstr(family, "fixed") || strstr(family, "terminal") ||
        strstr(family, "typewriter")) {
        return "monospace";
    }
    return "sans-serif";
}

/* Field 4 is SLANT: 'r' (roman), 'i' (italic), 'o' (oblique). */
static const char *resolve_css_style(const char *name) {
    char buf[8];
    if (!xlfd_field(name, 4, buf, sizeof(buf))) return "";
    if (buf[0] == 'i') return "italic ";
    if (buf[0] == 'o') return "oblique ";
    return "";
}

/* Field 3 is WEIGHT_NAME. */
static const char *resolve_css_weight(const char *name) {
    char buf[32];
    if (!xlfd_field(name, 3, buf, sizeof(buf))) return "";
    str_tolower(buf);
    if (strstr(buf, "bold"))  return "bold ";
    if (strstr(buf, "heavy") || strstr(buf, "black")) return "900 ";
    if (strstr(buf, "light") || strstr(buf, "thin"))  return "300 ";
    return "";
}

static void build_css_font(char *out, size_t outlen,
                           const char *name, int pixel_size) {
    const char *style  = resolve_css_style(name);
    const char *weight = resolve_css_weight(name);
    const char *family = resolve_css_family(name);
    snprintf(out, outlen, "%s%s%dpx %s", style, weight, pixel_size, family);
}

/* -- Font table -------------------------------------------------------- */

static EmxFont *font_lookup(Font fid) {
    for (int i = 0; i < EMX11_MAX_FONTS; i++) {
        if (g_fonts[i].in_use && g_fonts[i].fid == fid) return &g_fonts[i];
    }
    return NULL;
}

static EmxFont *font_alloc_slot(void) {
    for (int i = 0; i < EMX11_MAX_FONTS; i++) {
        if (!g_fonts[i].in_use) return &g_fonts[i];
    }
    return NULL;
}

const char *emx11_font_css(Font font) {
    EmxFont *f = font_lookup(font);
    return f ? f->css : NULL;
}

/* -- XFontStruct construction ----------------------------------------- */

/* -- XLFD synthesis for the FONT property --------------------------------
 *
 * We synthesize a canonical XLFD ending in `-iso10646-1` for every font
 * we hand out, and intern it as an atom exposed through the FONT
 * property. This is how Tk's tkUnixFont.c/GetFontAttributes learns our
 * charset: it calls XGetFontProperty(fs, XA_FONT, &val), resolves
 * val through XGetAtomName, and TkFontParseXLFD pulls the registry-
 * encoding pair out of the last two dash-fields.
 *
 * Why iso10646-1 specifically: Tk aliases that charset to the Tcl
 * "ucs-2be" encoding and sets familyPtr->isTwoByteFont = 1 (see
 * AllocFontFamily, tkUnixFont.c:1915). That makes Tk_DrawChars route
 * non-ASCII codepoints through XDrawString16 as 2-byte UCS-2BE pairs,
 * which our xaw_stubs.c rewriter expands back into UTF-8 for
 * canvas.fillText. Without iso10646-1 the font's encoding defaults to
 * iso8859-1 and every codepoint >= 0x100 gets routed through
 * ControlUtfProc's `\uXXXX` hex-escape fallback. */
static const char *xlfd_family_token(const char *css_family) {
    if (strstr(css_family, "serif") && !strstr(css_family, "sans")) {
        return "times";
    }
    if (strstr(css_family, "mono")) return "courier";
    return "helvetica";
}

static const char *xlfd_weight_token(const char *css_font) {
    if (strstr(css_font, "bold") || strstr(css_font, "900")) return "bold";
    if (strstr(css_font, "300"))                             return "light";
    return "medium";
}

static char xlfd_slant_token(const char *css_font) {
    if (strstr(css_font, "italic"))  return 'i';
    if (strstr(css_font, "oblique")) return 'o';
    return 'r';
}

static void build_xlfd(char *out, size_t outlen, const char *css_font,
                       int pixel_size) {
    const char *family = xlfd_family_token(css_font);
    const char *weight = xlfd_weight_token(css_font);
    char slant = xlfd_slant_token(css_font);
    /* Classic 14-field XLFD with iso10646-1 at registry-encoding. */
    snprintf(out, outlen,
             "-emx11-%s-%s-%c-normal--%d-*-*-*-*-*-iso10646-1",
             family, weight, slant, pixel_size);
}

static void fill_font_struct(Display *dpy, EmxFont *f) {
    int ascent = 0, descent = 0, max_width = 0;
    int widths[EMX11_PER_CHAR_COUNT] = {0};
    emx11_js_measure_font(f->css, &ascent, &descent, &max_width, widths);

    XFontStruct *fs = &f->fs;
    memset(fs, 0, sizeof(*fs));
    fs->fid = f->fid;
    fs->direction = FontLeftToRight;

    /* Full-BMP two-byte span. Tk's AllocFontFamily classifies anything
     * with max_byte1 > 0 or max_char_or_byte2 >= 256 as a two-byte font
     * (tkUnixFont.c:1915); that toggle is what unlocks CJK. */
    fs->min_byte1         = 0;
    fs->max_byte1         = 0xFF;
    fs->min_char_or_byte2 = 0;
    fs->max_char_or_byte2 = 0xFF;
    fs->all_chars_exist   = True;
    fs->default_char      = '?';

    /* per_char = NULL tells FontMapLoadPage (tkUnixFont.c:2306) "accept
     * every row+col the encoding produced", which is exactly what we
     * want given the browser can render any Unicode codepoint. The
     * per_char[] we still populate below is unused by Tk through the
     * 2-byte path, but ASCII-only clients that inspect XFontStruct
     * directly still see reasonable width data. */
    fs->per_char = NULL;

    int min_w = widths[0] > 0 ? widths[0] : max_width;
    for (int i = 0; i < EMX11_PER_CHAR_COUNT; i++) {
        short w = (short)widths[i];
        if (w > 0 && w < min_w) min_w = w;
        f->per_char[i].lbearing   = 0;
        f->per_char[i].rbearing   = w;
        f->per_char[i].width      = w;
        f->per_char[i].ascent     = (short)ascent;
        f->per_char[i].descent    = (short)descent;
        f->per_char[i].attributes = 0;
    }

    fs->min_bounds.lbearing = 0;
    fs->min_bounds.rbearing = (short)min_w;
    fs->min_bounds.width    = (short)min_w;
    fs->min_bounds.ascent   = (short)ascent;
    fs->min_bounds.descent  = (short)descent;

    fs->max_bounds.lbearing = 0;
    fs->max_bounds.rbearing = (short)max_width;
    fs->max_bounds.width    = (short)max_width;
    fs->max_bounds.ascent   = (short)ascent;
    fs->max_bounds.descent  = (short)descent;

    fs->ascent  = ascent;
    fs->descent = descent;

    /* FONT property: atom of the synthesized XLFD. Tk reads this via
     * XGetFontProperty (xaw_stubs.c implementation scans fs->properties). */
    char xlfd[192];
    build_xlfd(xlfd, sizeof(xlfd), f->css, f->pixel_size);
    f->props[0].name  = XA_FONT;
    f->props[0].card32 = (unsigned long)XInternAtom(dpy, xlfd, False);
    fs->properties    = f->props;
    fs->n_properties  = 1;
}

/* -- Public API ------------------------------------------------------------ */

XFontStruct *XLoadQueryFont(Display *dpy, const char *name) {
    EmxFont *f = font_alloc_slot();
    if (!f) return NULL;

    int pixel_size = resolve_pixel_size(name);
    f->fid        = emx11_next_xid(dpy);
    f->pixel_size = pixel_size;
    build_css_font(f->css, sizeof(f->css), name, pixel_size);
    fill_font_struct(dpy, f);
    f->in_use = true;
    return &f->fs;
}

Font XLoadFont(Display *dpy, const char *name) {
    XFontStruct *fs = XLoadQueryFont(dpy, name);
    return fs ? fs->fid : None;
}

int XUnloadFont(Display *dpy, Font font) {
    (void)dpy;
    EmxFont *f = font_lookup(font);
    if (f) f->in_use = false;
    return 1;
}

int XFreeFont(Display *dpy, XFontStruct *fs) {
    if (!fs) return 0;
    XUnloadFont(dpy, fs->fid);
    return 1;
}

XFontStruct *XQueryFont(Display *dpy, XID font_id) {
    (void)dpy;
    EmxFont *f = font_lookup((Font)font_id);
    return f ? &f->fs : NULL;
}

int XSetFont(Display *dpy, GC gc, Font font) {
    (void)dpy;
    if (!gc) return 0;
    gc->font = font;
    return 1;
}

int XTextWidth(XFontStruct *fs, _Xconst char *string, int count) {
    if (!fs || !string || count <= 0) return 0;
    /* Route through the browser rather than summing per_char: handles
     * Unicode / proportional fonts / ligatures exactly, matches fillText
     * pixel-for-pixel, and means per_char is purely a legacy convenience
     * for clients that poke at XFontStruct directly. */
    EmxFont *f = font_lookup(fs->fid);
    const char *css = f ? f->css : "13px sans-serif";
    return emx11_js_measure_string(css, string, count);
}

int XTextExtents(XFontStruct *fs, _Xconst char *string, int nchars,
                 int *direction_return, int *font_ascent_return,
                 int *font_descent_return, XCharStruct *overall_return) {
    if (!fs) return 0;
    if (direction_return)    *direction_return    = (int)fs->direction;
    if (font_ascent_return)  *font_ascent_return  = fs->ascent;
    if (font_descent_return) *font_descent_return = fs->descent;
    if (overall_return) {
        short w = (short)XTextWidth(fs, string, nchars);
        overall_return->lbearing   = 0;
        overall_return->rbearing   = w;
        overall_return->width      = w;
        overall_return->ascent     = (short)fs->ascent;
        overall_return->descent    = (short)fs->descent;
        overall_return->attributes = 0;
    }
    return 0;
}

static void dispatch_draw_string(Display *dpy, Drawable d, GC gc,
                                 int x, int y,
                                 const char *string, int length,
                                 int image_mode) {
    (void)dpy;
    if (!gc || !string || length <= 0) return;
    const char *css = gc->font != None ? emx11_font_css(gc->font) : NULL;
    if (!css) css = "13px sans-serif";
    emx11_js_draw_string((Window)d, x, y, css, string, length,
                         gc->foreground, gc->background, image_mode);
}

int XDrawString(Display *dpy, Drawable d, GC gc,
                int x, int y, _Xconst char *string, int length) {
    dispatch_draw_string(dpy, d, gc, x, y, string, length, 0);
    return 1;
}

int XDrawImageString(Display *dpy, Drawable d, GC gc,
                     int x, int y, _Xconst char *string, int length) {
    dispatch_draw_string(dpy, d, gc, x, y, string, length, 1);
    return 1;
}

/* -- Stubs for rarely-used font APIs --------------------------------------- */

char **XListFonts(Display *dpy, _Xconst char *pattern,
                  int maxnames, int *actual_count_return) {
    (void)dpy; (void)maxnames;
    if (!pattern || !pattern[0]) {
        if (actual_count_return) *actual_count_return = 0;
        return NULL;
    }
    /* We can load any XLFD via CSS, so echo the pattern back as a
     * single match. Tk then calls XLoadQueryFont with this name, which
     * we parse correctly. Returning NULL here makes Tk fall back to
     * "fixed" (monospace) for all fonts. */
    char **list = malloc(sizeof(char *));
    if (!list) { if (actual_count_return) *actual_count_return = 0; return NULL; }
    list[0] = strdup(pattern);
    if (!list[0]) { free(list); if (actual_count_return) *actual_count_return = 0; return NULL; }
    if (actual_count_return) *actual_count_return = 1;
    return list;
}

int XFreeFontNames(char **list) {
    if (list) { free(list[0]); free(list); }
    return 1;
}

int XFreeFontInfo(char **names, XFontStruct *free_info, int actual_count) {
    (void)names; (void)free_info; (void)actual_count;
    return 1;
}

char *XGetDefault(Display *dpy, _Xconst char *program, _Xconst char *option) {
    (void)dpy; (void)program; (void)option;
    return NULL;
}
