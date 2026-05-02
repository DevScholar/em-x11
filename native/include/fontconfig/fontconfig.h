/*
 * em-x11 minimal fontconfig.h
 *
 * Public API surface needed by Tk's tkUnixRFont.c plus a small headroom
 * for any future Xft consumer that lands in this tree. We do NOT match
 * real fontconfig binary layout — FcPattern / FcCharSet / FcFontSet are
 * em-x11 private types, and clients only ever touch them through the
 * functions declared here.
 *
 * Numeric constants (FC_SLANT_*, FC_WEIGHT_*, FC_PROPORTIONAL) are kept
 * in sync with real fontconfig so that Tk's compile-time comparisons
 * (e.g. XFT_WEIGHT_BOLD == 200) line up with what we hand back through
 * FcPatternGet*.
 */
#ifndef FONTCONFIG_FONTCONFIG_H
#define FONTCONFIG_FONTCONFIG_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int            FcBool;
typedef unsigned char  FcChar8;
typedef unsigned short FcChar16;
typedef unsigned int   FcChar32;

#define FcFalse 0
#define FcTrue  1

typedef enum _FcType {
    FcTypeUnknown = -1,
    FcTypeVoid    = 0,
    FcTypeInteger,
    FcTypeDouble,
    FcTypeString,
    FcTypeBool,
    FcTypeMatrix,
    FcTypeCharSet,
    FcTypeFTFace,
    FcTypeLangSet,
    FcTypeRange
} FcType;

typedef enum _FcResult {
    FcResultMatch,
    FcResultNoMatch,
    FcResultTypeMismatch,
    FcResultNoId,
    FcResultOutOfMemory
} FcResult;

typedef enum _FcMatchKind {
    FcMatchPattern,
    FcMatchFont,
    FcMatchScan
} FcMatchKind;

typedef struct _FcMatrix {
    double xx, xy;
    double yx, yy;
} FcMatrix;

#define FcMatrixInit(m) ((m)->xx = (m)->yy = 1.0, (m)->xy = (m)->yx = 0.0)

typedef struct _FcPattern  FcPattern;
typedef struct _FcCharSet  FcCharSet;
typedef struct _FcFontSet {
    int         nfont;
    int         sfont;
    FcPattern **fonts;
} FcFontSet;
typedef struct _FcConfig   FcConfig;
typedef struct _FcObjectSet FcObjectSet;

/* Property-name constants. Real fontconfig defines these as string
 * literals; Tk's tkUnixRFont.c uses them in FcPatternAdd / FcPatternGet
 * calls with strcmp semantics, so identity-equality is not required. */
#define FC_FAMILY        "family"
#define FC_STYLE         "style"
#define FC_SLANT         "slant"
#define FC_WEIGHT        "weight"
#define FC_SIZE          "size"
#define FC_PIXEL_SIZE    "pixelsize"
#define FC_SPACING       "spacing"
#define FC_FOUNDRY       "foundry"
#define FC_ENCODING      "encoding"
#define FC_MATRIX        "matrix"
#define FC_CHARSET       "charset"
#define FC_LANG          "lang"
#define FC_FONTFORMAT    "fontformat"
#define FC_SCALABLE      "scalable"
#define FC_DPI           "dpi"
#define FC_RGBA          "rgba"
#define FC_FILE          "file"

#define FC_SLANT_ROMAN     0
#define FC_SLANT_ITALIC  100
#define FC_SLANT_OBLIQUE 110

#define FC_WEIGHT_THIN      0
#define FC_WEIGHT_LIGHT    50
#define FC_WEIGHT_REGULAR  80
#define FC_WEIGHT_MEDIUM  100
#define FC_WEIGHT_DEMIBOLD 180
#define FC_WEIGHT_BOLD    200
#define FC_WEIGHT_BLACK   210

#define FC_PROPORTIONAL  0
#define FC_DUAL          90
#define FC_MONO         100
#define FC_CHARCELL     110

/* --- init ---------------------------------------------------------------- */
FcBool   FcInit(void);
void     FcFini(void);
int      FcGetVersion(void);
FcConfig *FcConfigGetCurrent(void);

/* --- pattern ------------------------------------------------------------- */
FcPattern *FcPatternCreate(void);
FcPattern *FcPatternDuplicate(const FcPattern *p);
void       FcPatternDestroy(FcPattern *p);

FcBool FcPatternAddInteger(FcPattern *p, const char *object, int i);
FcBool FcPatternAddDouble (FcPattern *p, const char *object, double d);
FcBool FcPatternAddString (FcPattern *p, const char *object, const FcChar8 *s);
FcBool FcPatternAddBool   (FcPattern *p, const char *object, FcBool b);
FcBool FcPatternAddMatrix (FcPattern *p, const char *object, const FcMatrix *m);
FcBool FcPatternAddCharSet(FcPattern *p, const char *object, const FcCharSet *cs);

FcResult FcPatternGetInteger(const FcPattern *p, const char *object, int n, int *i);
FcResult FcPatternGetDouble (const FcPattern *p, const char *object, int n, double *d);
FcResult FcPatternGetString (const FcPattern *p, const char *object, int n, FcChar8 **s);
FcResult FcPatternGetBool   (const FcPattern *p, const char *object, int n, FcBool *b);
FcResult FcPatternGetMatrix (const FcPattern *p, const char *object, int n, FcMatrix **m);
FcResult FcPatternGetCharSet(const FcPattern *p, const char *object, int n, FcCharSet **c);

FcBool FcPatternDel(FcPattern *p, const char *object);

FcPattern *FcNameParse(const FcChar8 *name);

/* UTF-8 → UCS-4 decode (single codepoint). Returns the byte length
 * consumed, or 0 on error. Tk's tkUnixRFont.c calls this directly when
 * its own decoder bails on a 6-byte sequence. */
int FcUtf8ToUcs4(const FcChar8 *src, FcChar32 *dst, int len);

/* --- charset ------------------------------------------------------------- */
FcCharSet *FcCharSetCreate(void);
FcCharSet *FcCharSetCopy  (FcCharSet *src);
void       FcCharSetDestroy(FcCharSet *fcs);
FcBool     FcCharSetAddChar(FcCharSet *fcs, FcChar32 ucs4);
FcBool     FcCharSetHasChar(const FcCharSet *fcs, FcChar32 ucs4);

/* --- fontset ------------------------------------------------------------- */
FcFontSet *FcFontSetCreate(void);
void       FcFontSetDestroy(FcFontSet *s);
FcBool     FcFontSetAdd(FcFontSet *s, FcPattern *font);

/* --- match / sort -------------------------------------------------------- */
FcBool     FcConfigSubstitute(FcConfig *config, FcPattern *p, FcMatchKind kind);
void       FcDefaultSubstitute(FcPattern *p);
FcPattern *FcFontMatch(FcConfig *config, FcPattern *p, FcResult *result);
FcFontSet *FcFontSort (FcConfig *config, FcPattern *p, FcBool trim,
                       FcCharSet **csp, FcResult *result);
FcPattern *FcFontRenderPrepare(FcConfig *config, FcPattern *pat, FcPattern *font);
FcFontSet *FcFontList(FcConfig *config, FcPattern *p, FcObjectSet *os);

#ifdef __cplusplus
}
#endif

#endif /* FONTCONFIG_FONTCONFIG_H */
