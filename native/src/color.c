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
 *    <name>                                     -- entry in the rgb.txt-style
 *                                                   table below
 */

#include "emx11_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Short rgb.txt-derived table. Covers the ~30 names X apps use most.
 * Tk / xclock / xeyes default palettes are fully inside this set. */
typedef struct {
    const char    *name;
    unsigned char  r, g, b;
} NamedColor;

static const NamedColor NAMED_COLORS[] = {
    { "black",         0,   0,   0   },
    { "white",         255, 255, 255 },
    { "red",           255, 0,   0   },
    { "green",         0,   128, 0   },
    { "blue",          0,   0,   255 },
    { "yellow",        255, 255, 0   },
    { "magenta",       255, 0,   255 },
    { "cyan",          0,   255, 255 },
    { "gray",          190, 190, 190 },
    { "grey",          190, 190, 190 },
    { "darkgray",      169, 169, 169 },
    { "darkgrey",      169, 169, 169 },
    { "lightgray",     211, 211, 211 },
    { "lightgrey",     211, 211, 211 },
    { "orange",        255, 165, 0   },
    { "purple",        160, 32,  240 },
    { "brown",         165, 42,  42  },
    { "pink",          255, 192, 203 },
    { "gold",          255, 215, 0   },
    { "silver",        192, 192, 192 },
    { "navy",          0,   0,   128 },
    { "maroon",        128, 0,   0   },
    { "olive",         128, 128, 0   },
    { "teal",          0,   128, 128 },
    { "lime",          0,   255, 0   },
    { "aqua",          0,   255, 255 },
    { "fuchsia",       255, 0,   255 },
    { "wheat",         245, 222, 179 },
    { "khaki",         240, 230, 140 },
    { "lightblue",     173, 216, 230 },
    { "lightgreen",    144, 238, 144 },
    { "lightyellow",   255, 255, 224 },
    { "lightpink",     255, 182, 193 },
    { "darkblue",      0,   0,   139 },
    { "darkgreen",     0,   100, 0   },
    { "darkred",       139, 0,   0   },
    { "tan",           210, 180, 140 },
    { "beige",         245, 245, 220 },
    { "ivory",         255, 255, 240 },
    { "mint cream",    245, 255, 250 },
};
#define NAMED_COLOR_COUNT (sizeof(NAMED_COLORS) / sizeof(NAMED_COLORS[0]))

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

static bool lookup_named(const char *name, unsigned short *rr,
                         unsigned short *gg, unsigned short *bb) {
    /* Case-insensitive compare, ignoring spaces so "Light Gray" matches. */
    char norm[64];
    size_t o = 0;
    for (size_t i = 0; name[i] && o + 1 < sizeof(norm); i++) {
        char c = (char)tolower((unsigned char)name[i]);
        if (c == ' ') continue;
        norm[o++] = c;
    }
    norm[o] = '\0';

    for (size_t i = 0; i < NAMED_COLOR_COUNT; i++) {
        /* Table is already lowercase and space-free (with one exception). */
        char tab[64];
        size_t j = 0;
        for (size_t k = 0; NAMED_COLORS[i].name[k] && j + 1 < sizeof(tab); k++) {
            char c = (char)tolower((unsigned char)NAMED_COLORS[i].name[k]);
            if (c == ' ') continue;
            tab[j++] = c;
        }
        tab[j] = '\0';
        if (strcmp(norm, tab) == 0) {
            *rr = NAMED_COLORS[i].r * 0x101;
            *gg = NAMED_COLORS[i].g * 0x101;
            *bb = NAMED_COLORS[i].b * 0x101;
            return true;
        }
    }
    return false;
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
    } else {
        ok = lookup_named(spec, &r, &g, &b);
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
