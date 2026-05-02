/*
 * em-x11 fontconfig implementation.
 *
 * Real fontconfig keeps an indexed font cache backed by FreeType. We
 * have neither — the browser's font stack is the only renderer — so
 * fontconfig here is reduced to:
 *
 *   - FcPattern: a key->value bag (linked list of FcValue cells).
 *   - FcFontSet: an array of FcPattern*.
 *   - FcCharSet: a singleton "matches everything" sentinel. The browser
 *     can render any Unicode codepoint via canvas.fillText, so the
 *     coverage test is uniformly true.
 *   - FcFontSort / FcFontMatch / FcFontRenderPrepare: return a single
 *     candidate (a copy of the requested pattern), so Tk picks face[0]
 *     and routes the whole string through it.
 *
 * This is deliberately enough to keep tkUnixRFont.c happy and nothing
 * more. Properties are stored verbatim and round-tripped without
 * substitution.
 */

#include "emx11_internal.h"
#include <fontconfig/fontconfig.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* -- internal value types ------------------------------------------------- */

typedef struct FcValue {
    FcType type;
    union {
        int        i;
        double     d;
        FcBool     b;
        FcChar8   *s;            /* owned */
        FcMatrix  *m;            /* owned */
        FcCharSet *c;            /* shared sentinel; not owned */
    } u;
} FcValue;

typedef struct FcElt {
    struct FcElt *next;
    char         *object;        /* owned */
    FcValue       val;
} FcElt;

struct _FcPattern {
    int    refcount;             /* not currently used; future-proofed */
    FcElt *elts;
};

struct _FcCharSet {
    int refcount;
    /* No backing data: HasChar always returns true. The struct exists so
     * pointer identity / alloc-count assertions in callers don't break. */
};

struct _FcConfig { int dummy; };
struct _FcObjectSet { int dummy; };

/* -- charset sentinel ----------------------------------------------------- */

static FcCharSet g_default_charset = { 1 };

FcCharSet *FcCharSetCreate(void) {
    FcCharSet *cs = (FcCharSet *)calloc(1, sizeof(FcCharSet));
    if (cs) cs->refcount = 1;
    return cs;
}

FcCharSet *FcCharSetCopy(FcCharSet *src) {
    if (src && src != &g_default_charset) src->refcount++;
    return src ? src : &g_default_charset;
}

void FcCharSetDestroy(FcCharSet *cs) {
    if (!cs || cs == &g_default_charset) return;
    if (--cs->refcount <= 0) free(cs);
}

FcBool FcCharSetAddChar(FcCharSet *cs, FcChar32 ucs4) {
    (void)cs; (void)ucs4;
    return FcTrue;
}

FcBool FcCharSetHasChar(const FcCharSet *cs, FcChar32 ucs4) {
    (void)cs; (void)ucs4;
    return FcTrue;
}

/* -- pattern -------------------------------------------------------------- */

FcPattern *FcPatternCreate(void) {
    FcPattern *p = (FcPattern *)calloc(1, sizeof(FcPattern));
    if (p) p->refcount = 1;
    return p;
}

static void elt_free(FcElt *e) {
    if (!e) return;
    free(e->object);
    if (e->val.type == FcTypeString) free(e->val.u.s);
    else if (e->val.type == FcTypeMatrix) free(e->val.u.m);
    else if (e->val.type == FcTypeCharSet) FcCharSetDestroy(e->val.u.c);
    free(e);
}

void FcPatternDestroy(FcPattern *p) {
    if (!p) return;
    FcElt *e = p->elts;
    while (e) {
        FcElt *next = e->next;
        elt_free(e);
        e = next;
    }
    free(p);
}

static FcElt *pattern_find_n(const FcPattern *p, const char *object, int n) {
    if (!p || !object) return NULL;
    int seen = 0;
    for (FcElt *e = p->elts; e; e = e->next) {
        if (strcmp(e->object, object) == 0) {
            if (seen == n) return e;
            seen++;
        }
    }
    return NULL;
}

static FcBool pattern_add(FcPattern *p, const char *object, FcValue v) {
    if (!p || !object) return FcFalse;
    FcElt *e = (FcElt *)calloc(1, sizeof(FcElt));
    if (!e) return FcFalse;
    e->object = strdup(object);
    if (!e->object) { free(e); return FcFalse; }
    e->val = v;
    /* Append to tail so multi-valued properties keep insertion order. */
    if (!p->elts) {
        p->elts = e;
    } else {
        FcElt *tail = p->elts;
        while (tail->next) tail = tail->next;
        tail->next = e;
    }
    return FcTrue;
}

FcBool FcPatternAddInteger(FcPattern *p, const char *object, int i) {
    FcValue v = { FcTypeInteger, { .i = i } };
    return pattern_add(p, object, v);
}

FcBool FcPatternAddDouble(FcPattern *p, const char *object, double d) {
    FcValue v = { FcTypeDouble, { 0 } };
    v.u.d = d;
    return pattern_add(p, object, v);
}

FcBool FcPatternAddBool(FcPattern *p, const char *object, FcBool b) {
    FcValue v = { FcTypeBool, { 0 } };
    v.u.b = b;
    return pattern_add(p, object, v);
}

FcBool FcPatternAddString(FcPattern *p, const char *object, const FcChar8 *s) {
    FcValue v = { FcTypeString, { 0 } };
    v.u.s = (FcChar8 *)strdup(s ? (const char *)s : "");
    if (!v.u.s) return FcFalse;
    if (!pattern_add(p, object, v)) { free(v.u.s); return FcFalse; }
    return FcTrue;
}

FcBool FcPatternAddMatrix(FcPattern *p, const char *object, const FcMatrix *m) {
    FcValue v = { FcTypeMatrix, { 0 } };
    v.u.m = (FcMatrix *)malloc(sizeof(FcMatrix));
    if (!v.u.m) return FcFalse;
    if (m) *v.u.m = *m; else FcMatrixInit(v.u.m);
    if (!pattern_add(p, object, v)) { free(v.u.m); return FcFalse; }
    return FcTrue;
}

FcBool FcPatternAddCharSet(FcPattern *p, const char *object, const FcCharSet *cs) {
    FcValue v = { FcTypeCharSet, { 0 } };
    v.u.c = FcCharSetCopy((FcCharSet *)cs);
    return pattern_add(p, object, v);
}

FcBool FcPatternDel(FcPattern *p, const char *object) {
    if (!p || !object) return FcFalse;
    FcElt **link = &p->elts;
    FcBool removed = FcFalse;
    while (*link) {
        if (strcmp((*link)->object, object) == 0) {
            FcElt *gone = *link;
            *link = gone->next;
            elt_free(gone);
            removed = FcTrue;
        } else {
            link = &(*link)->next;
        }
    }
    return removed;
}

FcResult FcPatternGetInteger(const FcPattern *p, const char *o, int n, int *out) {
    FcElt *e = pattern_find_n(p, o, n);
    if (!e) return FcResultNoMatch;
    if (e->val.type == FcTypeInteger) { if (out) *out = e->val.u.i; return FcResultMatch; }
    if (e->val.type == FcTypeDouble)  { if (out) *out = (int)e->val.u.d; return FcResultMatch; }
    return FcResultTypeMismatch;
}

FcResult FcPatternGetDouble(const FcPattern *p, const char *o, int n, double *out) {
    FcElt *e = pattern_find_n(p, o, n);
    if (!e) return FcResultNoMatch;
    if (e->val.type == FcTypeDouble)  { if (out) *out = e->val.u.d; return FcResultMatch; }
    if (e->val.type == FcTypeInteger) { if (out) *out = (double)e->val.u.i; return FcResultMatch; }
    return FcResultTypeMismatch;
}

FcResult FcPatternGetString(const FcPattern *p, const char *o, int n, FcChar8 **out) {
    FcElt *e = pattern_find_n(p, o, n);
    if (!e) return FcResultNoMatch;
    if (e->val.type != FcTypeString) return FcResultTypeMismatch;
    if (out) *out = e->val.u.s;
    return FcResultMatch;
}

FcResult FcPatternGetBool(const FcPattern *p, const char *o, int n, FcBool *out) {
    FcElt *e = pattern_find_n(p, o, n);
    if (!e) return FcResultNoMatch;
    if (e->val.type != FcTypeBool) return FcResultTypeMismatch;
    if (out) *out = e->val.u.b;
    return FcResultMatch;
}

FcResult FcPatternGetMatrix(const FcPattern *p, const char *o, int n, FcMatrix **out) {
    FcElt *e = pattern_find_n(p, o, n);
    if (!e) return FcResultNoMatch;
    if (e->val.type != FcTypeMatrix) return FcResultTypeMismatch;
    if (out) *out = e->val.u.m;
    return FcResultMatch;
}

FcResult FcPatternGetCharSet(const FcPattern *p, const char *o, int n, FcCharSet **out) {
    FcElt *e = pattern_find_n(p, o, n);
    if (!e) {
        /* Tk reads FC_CHARSET on every face it inspects; never failing
         * here keeps tkUnixRFont.c's GetFont fast path happy. */
        if (out) *out = &g_default_charset;
        return strcmp(o, FC_CHARSET) == 0 ? FcResultMatch : FcResultNoMatch;
    }
    if (e->val.type != FcTypeCharSet) return FcResultTypeMismatch;
    if (out) *out = e->val.u.c;
    return FcResultMatch;
}

FcPattern *FcPatternDuplicate(const FcPattern *src) {
    if (!src) return NULL;
    FcPattern *dst = FcPatternCreate();
    if (!dst) return NULL;
    for (FcElt *e = src->elts; e; e = e->next) {
        switch (e->val.type) {
        case FcTypeInteger:  FcPatternAddInteger(dst, e->object, e->val.u.i); break;
        case FcTypeDouble:   FcPatternAddDouble (dst, e->object, e->val.u.d); break;
        case FcTypeString:   FcPatternAddString (dst, e->object, e->val.u.s); break;
        case FcTypeBool:     FcPatternAddBool   (dst, e->object, e->val.u.b); break;
        case FcTypeMatrix:   FcPatternAddMatrix (dst, e->object, e->val.u.m); break;
        case FcTypeCharSet:  FcPatternAddCharSet(dst, e->object, e->val.u.c); break;
        default: break;
        }
    }
    return dst;
}

/* -- UTF-8 helper (FcUtf8ToUcs4) ----------------------------------------- */

int FcUtf8ToUcs4(const FcChar8 *src, FcChar32 *dst, int len) {
    if (!src || !dst || len <= 0) return 0;
    unsigned int b0 = src[0];
    unsigned int cp;
    int n;
    if (b0 < 0x80)        { cp = b0; n = 1; }
    else if (b0 < 0xC0)   { *dst = (FcChar32)b0; return 1; } /* stray cont */
    else if (b0 < 0xE0)   { cp = b0 & 0x1F; n = 2; }
    else if (b0 < 0xF0)   { cp = b0 & 0x0F; n = 3; }
    else if (b0 < 0xF8)   { cp = b0 & 0x07; n = 4; }
    else if (b0 < 0xFC)   { cp = b0 & 0x03; n = 5; }
    else                  { cp = b0 & 0x01; n = 6; }
    if (n > len) return 0;
    for (int i = 1; i < n; i++) {
        if ((src[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (src[i] & 0x3F);
    }
    *dst = (FcChar32)cp;
    return n;
}

/* -- name parsing --------------------------------------------------------- */

/* FcNameParse implements only the family-then-modifiers grammar Tk feeds
 * us via XftFontOpenName: "Family[,Family2[,...]]:size=N:slant=N:..."
 * Everything before the first ':' is treated as the family list (with
 * commas separating fallback faces); each ':'-delimited tail is a
 * key=value (or boolean key) pair. Unknown keys go in as strings. */
static char *trim(char *s) {
    if (!s) return NULL;
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    return s;
}

static FcBool is_numeric(const char *s) {
    if (!s || !*s) return FcFalse;
    if (*s == '-' || *s == '+') s++;
    int seen_digit = 0, seen_dot = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') seen_digit = 1;
        else if (*s == '.' && !seen_dot) seen_dot = 1;
        else return FcFalse;
        s++;
    }
    return seen_digit ? FcTrue : FcFalse;
}

static void apply_kv(FcPattern *p, char *key, char *val) {
    key = trim(key);
    val = val ? trim(val) : NULL;
    if (!key || !*key) return;
    if (val) {
        if (is_numeric(val)) {
            if (strchr(val, '.')) FcPatternAddDouble (p, key, atof(val));
            else                  FcPatternAddInteger(p, key, atoi(val));
        } else if (strcmp(val, "true") == 0)  FcPatternAddBool(p, key, FcTrue);
          else if (strcmp(val, "false") == 0) FcPatternAddBool(p, key, FcFalse);
          else FcPatternAddString(p, key, (const FcChar8 *)val);
    } else {
        /* Bare key — treat as boolean true. */
        FcPatternAddBool(p, key, FcTrue);
    }
}

FcPattern *FcNameParse(const FcChar8 *name) {
    FcPattern *p = FcPatternCreate();
    if (!p || !name) return p;
    char *buf = strdup((const char *)name);
    if (!buf) return p;

    char *colon = strchr(buf, ':');
    if (colon) *colon = '\0';

    /* Family list: comma-separated. */
    char *fam = buf, *next;
    while (fam && *fam) {
        next = strchr(fam, ',');
        if (next) *next = '\0';
        char *t = trim(fam);
        if (*t) FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)t);
        fam = next ? next + 1 : NULL;
    }

    if (colon) {
        char *kv = colon + 1, *kv_next;
        while (kv && *kv) {
            kv_next = strchr(kv, ':');
            if (kv_next) *kv_next = '\0';
            char *eq = strchr(kv, '=');
            if (eq) { *eq = '\0'; apply_kv(p, kv, eq + 1); }
            else    { apply_kv(p, kv, NULL); }
            kv = kv_next ? kv_next + 1 : NULL;
        }
    }
    free(buf);
    return p;
}

/* -- fontset -------------------------------------------------------------- */

FcFontSet *FcFontSetCreate(void) {
    FcFontSet *s = (FcFontSet *)calloc(1, sizeof(FcFontSet));
    return s;
}

void FcFontSetDestroy(FcFontSet *s) {
    if (!s) return;
    for (int i = 0; i < s->nfont; i++) FcPatternDestroy(s->fonts[i]);
    free(s->fonts);
    free(s);
}

FcBool FcFontSetAdd(FcFontSet *s, FcPattern *font) {
    if (!s || !font) return FcFalse;
    if (s->nfont >= s->sfont) {
        int nsfont = s->sfont ? s->sfont * 2 : 4;
        FcPattern **np = (FcPattern **)realloc(s->fonts, nsfont * sizeof(FcPattern *));
        if (!np) return FcFalse;
        s->fonts = np;
        s->sfont = nsfont;
    }
    s->fonts[s->nfont++] = font;
    return FcTrue;
}

/* -- match ---------------------------------------------------------------- */

FcBool FcConfigSubstitute(FcConfig *cfg, FcPattern *p, FcMatchKind kind) {
    (void)cfg; (void)p; (void)kind;
    return FcTrue;
}

void FcDefaultSubstitute(FcPattern *p) {
    if (!p) return;
    /* Mirror upstream defaults Tk relies on: stamp pixelsize from size if
     * the caller omitted it. FC_SIZE is in *points*; pixels = pt * 96/72
     * because CSS px is locked to 96 DPI. */
    int pixel_int;
    if (FcPatternGetInteger(p, FC_PIXEL_SIZE, 0, &pixel_int) != FcResultMatch) {
        double size;
        int    size_int;
        if (FcPatternGetDouble(p, FC_PIXEL_SIZE, 0, &size) == FcResultMatch && size > 0) {
            /* already pixels */
        } else if (FcPatternGetDouble(p, FC_SIZE, 0, &size) == FcResultMatch && size > 0) {
            FcPatternAddDouble(p, FC_PIXEL_SIZE, size * 96.0 / 72.0);
        } else if (FcPatternGetInteger(p, FC_SIZE, 0, &size_int) == FcResultMatch && size_int > 0) {
            FcPatternAddDouble(p, FC_PIXEL_SIZE, size_int * 96.0 / 72.0);
        } else {
            FcPatternAddDouble(p, FC_PIXEL_SIZE, 13.0);
        }
    }
}

FcPattern *FcFontMatch(FcConfig *cfg, FcPattern *p, FcResult *result) {
    (void)cfg;
    if (result) *result = FcResultMatch;
    return FcPatternDuplicate(p);
}

FcFontSet *FcFontSort(FcConfig *cfg, FcPattern *p, FcBool trim_,
                      FcCharSet **csp, FcResult *result) {
    (void)cfg; (void)trim_; (void)csp;
    FcFontSet *set = FcFontSetCreate();
    if (!set) { if (result) *result = FcResultOutOfMemory; return NULL; }
    FcPattern *dup = FcPatternDuplicate(p);
    if (!dup) {
        FcFontSetDestroy(set);
        if (result) *result = FcResultOutOfMemory;
        return NULL;
    }
    FcFontSetAdd(set, dup);
    if (result) *result = FcResultMatch;
    return set;
}

FcPattern *FcFontRenderPrepare(FcConfig *cfg, FcPattern *pat, FcPattern *font) {
    (void)cfg;
    /* Real fontconfig merges `font` over `pat`. We lean on `pat` (which
     * Tk built up from XftPatternAdd*) and only fall back to `font`
     * (the FcFontSort result) if pat is missing entirely. */
    if (pat) return FcPatternDuplicate(pat);
    return FcPatternDuplicate(font);
}

FcFontSet *FcFontList(FcConfig *cfg, FcPattern *p, FcObjectSet *os) {
    (void)cfg; (void)os;
    /* Tk's [font families] uses this to enumerate families. Return three
     * generic options matching the CSS generics our backend honours. */
    FcFontSet *set = FcFontSetCreate();
    if (!set) return NULL;
    static const char *families[] = { "sans-serif", "serif", "monospace" };
    for (size_t i = 0; i < sizeof(families)/sizeof(families[0]); i++) {
        FcPattern *fp = FcPatternCreate();
        if (fp) {
            FcPatternAddString(fp, FC_FAMILY, (const FcChar8 *)families[i]);
            FcFontSetAdd(set, fp);
        }
    }
    (void)p;
    return set;
}

/* -- init / version ------------------------------------------------------- */

FcBool FcInit(void) { return FcTrue; }
void   FcFini(void) { }
int    FcGetVersion(void) { return 21300; }
FcConfig *FcConfigGetCurrent(void) {
    static FcConfig g; return &g;
}
