/*
 * XResourceManager (Xrm) — minimal but functional implementation.
 *
 * Replaces the earlier all-stub version. Xt's app-defaults loading
 * (xcalc, twm, future Tk programs) goes through XrmCombineFileDatabase
 * → XrmGetStringDatabase → per-line parse → XrmQGetResource lookups
 * driven by widget-hierarchy name/class lists. With every entry point
 * stubbed, every widget falls back to its compile-time default and
 * Form-based UIs collapse on top of themselves at (0,0).
 *
 * What this implements:
 *   - Database = singly linked list of (pattern, value) entries.
 *     Patterns are stored as parallel arrays of XrmQuark and binding
 *     (tight `.` / loose `*`) per component. The leaf component carries
 *     the resource name; binding[0] preceding a `*` at start is loose,
 *     otherwise tight.
 *   - Value strings are decoded for `\n` / `\t` / `\\` / `\<octal>` and
 *     `\<newline>` continuations. Values are stored NUL-terminated.
 *   - XrmGetStringDatabase parses a multi-line buffer (handling line
 *     continuations and `!`-comments) and adds each `pattern: value`
 *     line to a fresh DB.
 *   - XrmGet/CombineFileDatabase reads the file via stdio and reuses
 *     XrmGetStringDatabase. MEMFS is fine — everything is access()/fopen.
 *   - XrmQGetResource walks every entry and picks the most-specific
 *     match: scoring (n_tight, n_name_matches) lexicographically, with
 *     last-insertion as tie-breaker (later entries override earlier of
 *     the same specificity, matching the conventional "later-defined
 *     overrides" expectation).
 *   - Per-Display DB pointer lives in display->db (the layout already
 *     reserves it; XrmGetDatabase / XrmSetDatabase just touch it).
 *
 * What this DOESN'T implement:
 *   - The hash-bucket tree the upstream X11 Xrm uses for performance.
 *     A linked list with linear scan is fine for the ~600 lines of an
 *     app-defaults file; the lookup cost is bounded and amortised over
 *     a session.
 *   - XrmEnumerateDatabase, XrmLocaleOfDatabase, sub-database APIs that
 *     no current consumer touches.
 */

#include "emx11_internal.h"

#include <X11/Xresource.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- Quark table ---------------------------------------------------------- */

#define EMX11_MAX_QUARKS 8192

typedef struct {
    char *name;
} QuarkEntry;

static QuarkEntry g_quarks[EMX11_MAX_QUARKS];
static XrmQuark   g_next_quark = 1;             /* 0 reserved for NULLQUARK */

static XrmQuark quark_find(const char *s) {
    if (!s || !*s) return NULLQUARK;
    for (XrmQuark q = 1; q < g_next_quark && q < EMX11_MAX_QUARKS; q++) {
        if (strcmp(g_quarks[q].name, s) == 0) return q;
    }
    return NULLQUARK;
}

XrmQuark XrmStringToQuark(_Xconst char *s) {
    if (!s || !*s) return NULLQUARK;
    XrmQuark existing = quark_find(s);
    if (existing != NULLQUARK) return existing;
    if (g_next_quark >= EMX11_MAX_QUARKS) return NULLQUARK;
    XrmQuark q = g_next_quark++;
    g_quarks[q].name = strdup(s);
    return q;
}

XrmQuark XrmPermStringToQuark(_Xconst char *s) {
    return XrmStringToQuark(s);
}

char *XrmQuarkToString(XrmQuark quark) {
    if (quark <= 0 || quark >= g_next_quark) return NULL;
    return g_quarks[quark].name;
}

XrmQuark XrmUniqueQuark(void) {
    if (g_next_quark >= EMX11_MAX_QUARKS) return NULLQUARK;
    XrmQuark q = g_next_quark++;
    g_quarks[q].name = strdup("");
    return q;
}

/* -- Database -------------------------------------------------------------- */

typedef struct XrmEntry {
    struct XrmEntry *next;
    int             n;             /* component count, including leaf */
    XrmQuark       *quarks;        /* length n */
    char           *bindings;      /* length n; '.' or '*' before each component */
    char           *value;         /* NUL-terminated decoded value */
    int             value_size;    /* strlen(value) + 1 */
    unsigned long   seq;           /* insertion sequence for tie-break */
} XrmEntry;

struct _XrmHashBucketRec {
    XrmEntry      *head;
    unsigned long  next_seq;
};

static XrmDatabase db_new(void) {
    XrmDatabase db = (XrmDatabase)calloc(1, sizeof(struct _XrmHashBucketRec));
    return db;
}

void XrmInitialize(void) { /* no-op */ }

XrmDatabase XrmGetDatabase(Display *dpy) {
    return dpy ? dpy->db : NULL;
}

void XrmSetDatabase(Display *dpy, XrmDatabase db) {
    if (dpy) dpy->db = db;
}

void XrmDestroyDatabase(XrmDatabase db) {
    if (!db) return;
    XrmEntry *e = db->head;
    while (e) {
        XrmEntry *n = e->next;
        free(e->quarks);
        free(e->bindings);
        free(e->value);
        free(e);
        e = n;
    }
    free(db);
}

/* -- Pattern parsing ------------------------------------------------------- */

/* Parse "XCalc*foo.bar" or "*foo.bar" into bindings/quarks. Returns 0 on
 * success. The leading binding is '.' (tight) unless the pattern starts
 * with '*'. */
static int parse_pattern(const char *spec, int *n_out,
                         XrmQuark **quarks_out, char **bindings_out) {
    /* Worst-case component count = strlen / 2 + 1; over-allocate. */
    int cap = 0;
    for (const char *p = spec; *p; p++) cap++;
    cap = cap / 2 + 4;

    XrmQuark *qs = (XrmQuark *)malloc((size_t)cap * sizeof(XrmQuark));
    char     *bs = (char *)malloc((size_t)cap);
    if (!qs || !bs) { free(qs); free(bs); return -1; }

    int n = 0;
    char binding = '.';
    const char *p = spec;
    char buf[256];

    /* Leading '*'/'.' handling: a leading '*' makes first component loose;
     * otherwise tight. Skip leading whitespace too. */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '*') { binding = '*'; p++; }
    else if (*p == '.') { binding = '.'; p++; }

    while (*p) {
        int len = 0;
        while (*p && *p != '.' && *p != '*' && *p != ':' && *p != ' '
               && *p != '\t' && len < (int)sizeof(buf) - 1) {
            buf[len++] = *p++;
        }
        if (len == 0) {
            /* "**" or ".." — treat as a single binding update */
            if (*p == '*' || *p == '.') { binding = *p++; continue; }
            break;
        }
        buf[len] = '\0';
        if (n >= cap) { free(qs); free(bs); return -1; }
        qs[n] = XrmStringToQuark(buf);
        bs[n] = binding;
        n++;
        if (*p == '*') { binding = '*'; p++; }
        else if (*p == '.') { binding = '.'; p++; }
        else break;     /* end of pattern (whitespace or ':') */
    }

    if (n == 0) { free(qs); free(bs); return -1; }
    *n_out = n;
    *quarks_out = qs;
    *bindings_out = bs;
    return 0;
}

/* -- Value decoding -------------------------------------------------------- */

/* Decode the value half of `pattern: value`. Handles \n, \t, \\, \<octal>,
 * and trailing whitespace stripping. Caller frees. */
static char *decode_value(const char *raw, int len) {
    char *out = (char *)malloc((size_t)len + 1);
    if (!out) return NULL;
    int oi = 0;
    int i = 0;

    /* Skip leading whitespace after the colon. */
    while (i < len && (raw[i] == ' ' || raw[i] == '\t')) i++;

    while (i < len) {
        char c = raw[i++];
        if (c == '\\' && i < len) {
            char nc = raw[i];
            if (nc == 'n')      { out[oi++] = '\n'; i++; }
            else if (nc == 't') { out[oi++] = '\t'; i++; }
            else if (nc == '\\'){ out[oi++] = '\\'; i++; }
            else if (nc >= '0' && nc <= '7') {
                /* Up to 3 octal digits. */
                int v = 0, k = 0;
                while (k < 3 && i < len && raw[i] >= '0' && raw[i] <= '7') {
                    v = v * 8 + (raw[i] - '0');
                    i++; k++;
                }
                out[oi++] = (char)v;
            } else {
                out[oi++] = nc; i++;
            }
        } else {
            out[oi++] = c;
        }
    }

    /* Strip trailing whitespace. */
    while (oi > 0 && (out[oi-1] == ' ' || out[oi-1] == '\t')) oi--;
    out[oi] = '\0';
    return out;
}

/* -- Database mutation ----------------------------------------------------- */

static void db_add(XrmDatabase db, int n, XrmQuark *quarks, char *bindings,
                   char *value) {
    if (!db) return;
    XrmEntry *e = (XrmEntry *)calloc(1, sizeof(XrmEntry));
    e->n = n;
    e->quarks = quarks;
    e->bindings = bindings;
    e->value = value;
    e->value_size = (int)strlen(value) + 1;
    e->seq = db->next_seq++;
    e->next = db->head;
    db->head = e;
}

void XrmPutLineResource(XrmDatabase *db_p, _Xconst char *line) {
    if (!db_p || !line) return;
    if (!*db_p) *db_p = db_new();

    /* Skip leading whitespace and '!' comments. */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '!' || *p == '\n') return;

    /* Find ':' separator. */
    const char *colon = strchr(p, ':');
    if (!colon) return;

    /* Pattern is p .. colon-1 (with possible trailing whitespace). */
    int spec_len = (int)(colon - p);
    while (spec_len > 0 && (p[spec_len-1] == ' ' || p[spec_len-1] == '\t'))
        spec_len--;
    char spec[512];
    if (spec_len <= 0 || spec_len >= (int)sizeof(spec)) return;
    memcpy(spec, p, (size_t)spec_len);
    spec[spec_len] = '\0';

    int n;
    XrmQuark *qs = NULL;
    char *bs = NULL;
    if (parse_pattern(spec, &n, &qs, &bs) != 0) return;

    /* Value is colon+1 .. end-of-line (caller guarantees no continuations). */
    const char *vstart = colon + 1;
    int vlen = (int)strlen(vstart);
    /* Strip trailing newline. */
    while (vlen > 0 && (vstart[vlen-1] == '\n' || vstart[vlen-1] == '\r'))
        vlen--;

    char *value = decode_value(vstart, vlen);
    if (!value) { free(qs); free(bs); return; }

    db_add(*db_p, n, qs, bs, value);
}

void XrmPutResource(XrmDatabase *db, _Xconst char *spec, _Xconst char *type,
                    XrmValue *value) {
    (void)type;     /* type is currently always "String"; we don't track it */
    if (!db || !spec || !value || !value->addr) return;
    if (!*db) *db = db_new();

    int n;
    XrmQuark *qs = NULL;
    char *bs = NULL;
    if (parse_pattern(spec, &n, &qs, &bs) != 0) return;

    char *v = (char *)malloc((size_t)value->size + 1);
    if (!v) { free(qs); free(bs); return; }
    memcpy(v, value->addr, (size_t)value->size);
    v[value->size] = '\0';
    db_add(*db, n, qs, bs, v);
}

void XrmPutStringResource(XrmDatabase *db, _Xconst char *spec,
                          _Xconst char *value) {
    if (!db || !spec || !value) return;
    if (!*db) *db = db_new();

    int n;
    XrmQuark *qs = NULL;
    char *bs = NULL;
    if (parse_pattern(spec, &n, &qs, &bs) != 0) return;

    char *v = strdup(value);
    if (!v) { free(qs); free(bs); return; }
    db_add(*db, n, qs, bs, v);
}

/* -- String / file ingestion ---------------------------------------------- */

XrmDatabase XrmGetStringDatabase(_Xconst char *data) {
    XrmDatabase db = db_new();
    if (!data) return db;

    /* Tokenize into logical lines, folding `\<newline>` continuations. */
    const char *p = data;
    while (*p) {
        /* Build one logical line. */
        char line[4096];
        int  li = 0;

        while (*p) {
            if (*p == '\n') { p++; break; }
            /* Backslash-newline = continuation, drop both. */
            if (*p == '\\' && *(p+1) == '\n') { p += 2; continue; }
            if (*p == '\\' && *(p+1) == '\r' && *(p+2) == '\n') { p += 3; continue; }
            if (li < (int)sizeof(line) - 1) line[li++] = *p;
            p++;
        }
        line[li] = '\0';
        XrmPutLineResource(&db, line);
    }
    return db;
}

XrmDatabase XrmGetFileDatabase(_Xconst char *file) {
    if (!file) return NULL;
    FILE *f = fopen(file, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    XrmDatabase db = XrmGetStringDatabase(buf);
    free(buf);
    return db;
}

/* -- Database merging ------------------------------------------------------ */

void XrmCombineDatabase(XrmDatabase src, XrmDatabase *dst, Bool override) {
    if (!dst || !src) return;
    if (!*dst) { *dst = src; return; }

    /* Splice src entries into *dst. With override=False, src entries lose
     * to existing dst entries — we simulate that by giving them lower seq
     * numbers so the tie-break in lookup falls to dst.
     *
     * With override=True, src wins ties — append after current head with
     * higher seq. */
    XrmEntry *e = src->head;
    while (e) {
        XrmEntry *n = e->next;
        if (override) {
            e->seq = (*dst)->next_seq++;
            e->next = (*dst)->head;
            (*dst)->head = e;
        } else {
            /* Push at tail so existing entries remain newer. */
            e->seq = 0;     /* lowest priority on ties */
            XrmEntry **tail = &(*dst)->head;
            while (*tail) tail = &(*tail)->next;
            e->next = NULL;
            *tail = e;
        }
        e = n;
    }
    free(src);
}

Status XrmCombineFileDatabase(_Xconst char *file, XrmDatabase *dst,
                              Bool override) {
    XrmDatabase src = XrmGetFileDatabase(file);
    if (!src) return 0;
    XrmCombineDatabase(src, dst, override);
    return 1;
}

void XrmMergeDatabases(XrmDatabase src, XrmDatabase *dst) {
    /* Per X11: src takes precedence over *dst. */
    XrmCombineDatabase(src, dst, True);
}

/* -- Lookup ---------------------------------------------------------------- */

/* Recursive match of an entry's (binding, quark) sequence against a query's
 * name/class lists. Returns true on full match; on success, *score_out gets
 * (n_tight_bindings * 2 + n_name_matches), used for specificity ranking. */
static Bool match_entry(int ei, int qi, int n_query, int n_entry,
                        const XrmQuark *eq, const char *eb,
                        const XrmName *names, const XrmClass *classes,
                        int score, int *best_score) {
    if (ei == n_entry) {
        if (qi == n_query) {
            if (score > *best_score) *best_score = score;
            return True;
        }
        return False;
    }
    XrmQuark q = eq[ei];
    char b = eb[ei];

    if (b == '*') {
        /* Loose: try matching at any query position from qi to n_query-1. */
        Bool any = False;
        for (int i = qi; i < n_query; i++) {
            int delta = 0;
            if (q == names[i]) delta = 2;
            else if (q == classes[i]) delta = 1;
            else continue;
            if (match_entry(ei + 1, i + 1, n_query, n_entry, eq, eb,
                            names, classes, score + delta, best_score))
                any = True;
        }
        return any;
    } else {
        /* Tight: must match at exactly position qi. */
        if (qi >= n_query) return False;
        int delta = 0;
        if (q == names[qi]) delta = 2 + 2;          /* name + tight bonus */
        else if (q == classes[qi]) delta = 1 + 2;   /* class + tight bonus */
        else return False;
        return match_entry(ei + 1, qi + 1, n_query, n_entry, eq, eb,
                           names, classes, score + delta, best_score);
    }
}

Bool XrmQGetResource(XrmDatabase db, XrmNameList quark_name,
                     XrmClassList quark_class, XrmRepresentation *type_return,
                     XrmValue *value_return) {
    if (type_return)  *type_return = NULLQUARK;
    if (value_return) { value_return->size = 0; value_return->addr = NULL; }
    if (!db || !quark_name || !quark_class) return False;

    int n_query = 0;
    while (quark_name[n_query] != NULLQUARK) n_query++;
    /* class_list MUST be the same length as name_list (Xt convention). */

    XrmEntry *best = NULL;
    int best_score = -1;

    for (XrmEntry *e = db->head; e; e = e->next) {
        int s = -1;
        if (match_entry(0, 0, n_query, e->n, e->quarks, e->bindings,
                        quark_name, quark_class, 0, &s)) {
            if (s > best_score ||
                (s == best_score && best && e->seq > best->seq)) {
                best = e;
                best_score = s;
            }
        }
    }

    if (!best) return False;
    if (type_return) *type_return = XrmStringToQuark("String");
    if (value_return) {
        value_return->size = (unsigned int)best->value_size;
        value_return->addr = (XPointer)best->value;
    }
    return True;
}

Bool XrmGetResource(XrmDatabase db, _Xconst char *str_name,
                    _Xconst char *str_class, char **str_type_return,
                    XrmValue *value_return) {
    if (str_type_return) *str_type_return = NULL;
    if (value_return) { value_return->size = 0; value_return->addr = NULL; }
    if (!db || !str_name || !str_class) return False;

    XrmQuark name_q[32], class_q[32];
    XrmStringToQuarkList(str_name, name_q);
    XrmStringToQuarkList(str_class, class_q);
    /* Pad class list to same length as name list. */
    int nn = 0; while (name_q[nn] != NULLQUARK && nn < 31) nn++;
    int nc = 0; while (class_q[nc] != NULLQUARK && nc < 31) nc++;
    while (nc < nn) class_q[nc++] = NULLQUARK;
    class_q[nn] = NULLQUARK;

    XrmRepresentation type;
    Bool ok = XrmQGetResource(db, name_q, class_q, &type, value_return);
    if (ok && str_type_return) *str_type_return = XrmQuarkToString(type);
    return ok;
}

/* -- Command-line parsing (Xt -name -xrm etc.) ---------------------------- */

void XrmParseCommand(XrmDatabase *db, XrmOptionDescList table, int entries,
                     _Xconst char *name, int *argc_in_out, char **argv_in_out) {
    (void)db; (void)table; (void)entries; (void)name;
    (void)argc_in_out; (void)argv_in_out;
    /* Demos don't pass -xrm options today; if/when they do, wire this up
     * to mutate *db like the real Xrm. */
}

/* -- Quark list helpers (used by Xt for hierarchy lookups) ---------------- */

void XrmStringToQuarkList(_Xconst char *s, XrmQuarkList list) {
    if (!s || !list) { if (list) list[0] = NULLQUARK; return; }
    char buf[256];
    int  out = 0;
    const char *p = s;
    while (*p) {
        int len = 0;
        while (p[len] && p[len] != '.' && p[len] != '*' && len < (int)sizeof(buf) - 1) len++;
        if (len > 0) {
            memcpy(buf, p, (size_t)len);
            buf[len] = '\0';
            list[out++] = XrmStringToQuark(buf);
        }
        p += len;
        if (*p) p++;
    }
    list[out] = NULLQUARK;
}

void XrmStringToBindingQuarkList(_Xconst char *s, XrmBindingList bindings,
                                 XrmQuarkList quarks) {
    if (!s || !bindings || !quarks) {
        if (quarks) quarks[0] = NULLQUARK;
        return;
    }
    char buf[256];
    int  out = 0;
    const char *p = s;
    while (*p) {
        XrmBinding b = XrmBindTightly;
        if (*p == '*') { b = XrmBindLoosely; p++; }
        else if (*p == '.') { p++; }
        int len = 0;
        while (p[len] && p[len] != '.' && p[len] != '*' && len < (int)sizeof(buf) - 1) len++;
        if (len > 0) {
            memcpy(buf, p, (size_t)len);
            buf[len] = '\0';
            bindings[out] = b;
            quarks[out++] = XrmStringToQuark(buf);
        }
        p += len;
    }
    quarks[out] = NULLQUARK;
}
