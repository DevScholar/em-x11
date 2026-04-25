/*
 * Colors.
 *
 * em-x11 always serves a single 24-bit TrueColor visual (see display.c).
 * XColor.pixel maps directly to 0x00RRGGBB, bypassing the colormap
 * machinery that real X servers carry for PseudoColor visuals. We still
 * populate the XColor struct fields so clients that round-trip through
 * colormaps don't get surprises.
 *
 * XParseColor accepts these forms:
 *    #RGB, #RRGGBB, #RRRGGGBBB, #RRRRGGGGBBBB  -- hex literals
 *    rgb:R/G/B                                  -- hex with explicit slashes
 *    <name>                                     -- delegated to the Host,
 *                                                   which reuses the
 *                                                   browser's CSS color
 *                                                   parser. CSS3's named
 *                                                   colour set is X11's
 *                                                   rgb.txt plus a handful
 *                                                   of CSS additions, so
 *                                                   the browser's table
 *                                                   is mostly authoritative.
 *                                                   Exception: X11 has
 *                                                   grayN / greyN (N=0..100)
 *                                                   which CSS never picked
 *                                                   up; we match that on
 *                                                   the C side so twm's
 *                                                   "gray70" / "gray85"
 *                                                   resolve correctly.
 */

#include "emx11_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Parse one hex component of `digits` hex digits, returning a value
 * scaled into [0, 65535]. Returns -1 on parse error. */
static int parse_hex_component(const char *s, int digits) {
    int v = 0;
    for (int i = 0; i < digits; i++) {
        int d = hexval(s[i]);
        if (d < 0) return -1;
        v = (v << 4) | d;
    }
    switch (digits) {
    case 1: v = v * 0x1111; break;              /* scale #XYZ -> 0xXXYYZZ... */
    case 2: v = v * 0x0101; break;
    case 3: v = v * 0x0011; break;              /* rough, keeps top bits */
    case 4: break;                              /* already 16-bit */
    default: return -1;
    }
    return v;
}

static bool parse_hex_triplet(const char *spec, unsigned short *rr,
                              unsigned short *gg, unsigned short *bb) {
    size_t len = strlen(spec);
    if (len < 3) return false;
    int d;
    switch (len) {
    case 3:  d = 1; break;
    case 6:  d = 2; break;
    case 9:  d = 3; break;
    case 12: d = 4; break;
    default: return false;
    }
    int r = parse_hex_component(spec,             d);
    int g = parse_hex_component(spec + d,         d);
    int b = parse_hex_component(spec + 2 * d,     d);
    if (r < 0 || g < 0 || b < 0) return false;
    *rr = (unsigned short)r;
    *gg = (unsigned short)g;
    *bb = (unsigned short)b;
    return true;
}

static bool parse_rgb_slashed(const char *spec, unsigned short *rr,
                              unsigned short *gg, unsigned short *bb) {
    /* "rgb:R/G/B" where each component is 1-4 hex digits. */
    const char *r_start = spec;
    const char *slash1 = strchr(r_start, '/');
    if (!slash1) return false;
    const char *slash2 = strchr(slash1 + 1, '/');
    if (!slash2) return false;

    int r_digits = (int)(slash1 - r_start);
    int g_digits = (int)(slash2 - (slash1 + 1));
    int b_digits = (int)strlen(slash2 + 1);
    if (r_digits < 1 || r_digits > 4) return false;
    if (g_digits < 1 || g_digits > 4) return false;
    if (b_digits < 1 || b_digits > 4) return false;

    int r = parse_hex_component(r_start,       r_digits);
    int g = parse_hex_component(slash1 + 1,    g_digits);
    int b = parse_hex_component(slash2 + 1,    b_digits);
    if (r < 0 || g < 0 || b < 0) return false;
    *rr = (unsigned short)r;
    *gg = (unsigned short)g;
    *bb = (unsigned short)b;
    return true;
}

/* Match X11's grayN / greyN / GrayN / GreyN where N is 0..100. CSS
 * named-colors don't include these (CSS has bare "gray"/"grey" plus
 * light/dark/dim variants, but not the numbered shades). Formula per
 * X11 rgb.txt: each channel = round(N * 255 / 100). Bare "gray" and
 * "grey" (no digits) deliberately fall through to CSS so we match the
 * browser's 190,190,190 value there, not our own. */
static bool parse_gray_n(const char *spec, unsigned short *rr,
                         unsigned short *gg, unsigned short *bb) {
    const char *p = spec;
    if ((p[0] == 'g' || p[0] == 'G') &&
        (p[1] == 'r' || p[1] == 'R') &&
        (p[2] == 'a' || p[2] == 'A' || p[2] == 'e' || p[2] == 'E') &&
        (p[3] == 'y' || p[3] == 'Y')) {
        p += 4;
    } else {
        return false;
    }
    if (!*p) return false;                          /* bare gray/grey */
    int n = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        n = n * 10 + (*p - '0');
        if (n > 100) return false;
    }
    /* round-half-up: (n * 255 + 50) / 100. Matches X11 rgb.txt for
     * gray30=77, gray70=179, gray85=217. gray50 differs by 1 (128 vs
     * X11's 127), which no real program cares about. */
    int v8 = (n * 255 + 50) / 100;
    int v16 = v8 * 0x101;
    *rr = (unsigned short)v16;
    *gg = (unsigned short)v16;
    *bb = (unsigned short)v16;
    return true;
}

Status XParseColor(Display *dpy, Colormap cmap, _Xconst char *spec,
                   XColor *color_out) {
    (void)dpy; (void)cmap;
    if (!spec || !color_out) return 0;

    unsigned short r = 0, g = 0, b = 0;
    bool ok = false;

    if (spec[0] == '#') {
        ok = parse_hex_triplet(spec + 1, &r, &g, &b);
    } else if (strncmp(spec, "rgb:", 4) == 0) {
        ok = parse_rgb_slashed(spec + 4, &r, &g, &b);
    } else if (parse_gray_n(spec, &r, &g, &b)) {
        ok = true;
    } else {
        ok = emx11_js_parse_color(spec, &r, &g, &b) != 0;
    }
    if (!ok) return 0;

    color_out->red   = r;
    color_out->green = g;
    color_out->blue  = b;
    color_out->flags = DoRed | DoGreen | DoBlue;
    color_out->pixel =
        ((unsigned long)(r >> 8) << 16) |
        ((unsigned long)(g >> 8) << 8)  |
         (unsigned long)(b >> 8);
    return 1;
}

Status XAllocColor(Display *dpy, Colormap cmap, XColor *screen_in_out) {
    (void)dpy; (void)cmap;
    if (!screen_in_out) return 0;
    screen_in_out->pixel =
        ((unsigned long)(screen_in_out->red   >> 8) << 16) |
        ((unsigned long)(screen_in_out->green >> 8) << 8)  |
         (unsigned long)(screen_in_out->blue  >> 8);
    screen_in_out->flags = DoRed | DoGreen | DoBlue;
    return 1;
}

Status XAllocNamedColor(Display *dpy, Colormap cmap, _Xconst char *name,
                        XColor *screen_def_return, XColor *exact_def_return) {
    XColor parsed;
    if (!XParseColor(dpy, cmap, name, &parsed)) return 0;
    if (screen_def_return) *screen_def_return = parsed;
    if (exact_def_return)  *exact_def_return  = parsed;
    return 1;
}

int XQueryColor(Display *dpy, Colormap cmap, XColor *def_in_out) {
    (void)dpy; (void)cmap;
    if (!def_in_out) return 0;
    /* TrueColor: decompose pixel back into 16-bit components. */
    unsigned long p = def_in_out->pixel;
    unsigned int r8 = (p >> 16) & 0xFF;
    unsigned int g8 = (p >> 8)  & 0xFF;
    unsigned int b8 =  p        & 0xFF;
    def_in_out->red   = (unsigned short)(r8 * 0x101);
    def_in_out->green = (unsigned short)(g8 * 0x101);
    def_in_out->blue  = (unsigned short)(b8 * 0x101);
    def_in_out->flags = DoRed | DoGreen | DoBlue;
    return 1;
}

int XQueryColors(Display *dpy, Colormap cmap, XColor *defs, int ncolors) {
    for (int i = 0; i < ncolors; i++) XQueryColor(dpy, cmap, &defs[i]);
    return 1;
}

int XFreeColors(Display *dpy, Colormap cmap, unsigned long *pixels,
                int npixels, unsigned long planes) {
    (void)dpy; (void)cmap; (void)pixels; (void)npixels; (void)planes;
    /* No colormap to reference-count: pixel values ARE the colors. */
    return 1;
}

int XFreeColormap(Display *dpy, Colormap cmap) {
    (void)dpy; (void)cmap;
    return 1;
}

Colormap XCreateColormap(Display *dpy, Window w, Visual *visual, int alloc) {
    (void)dpy; (void)w; (void)visual; (void)alloc;
    return 1;                                   /* same dummy as default cmap */
}

Status XLookupColor(Display *dpy, Colormap cmap, _Xconst char *name,
                    XColor *exact_def_return, XColor *screen_def_return) {
    return XAllocNamedColor(dpy, cmap, name, screen_def_return, exact_def_return);
}
