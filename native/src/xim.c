/*
 * XIM / XIC -- input-method plumbing.
 *
 * Tier A: enough plumbing that Tk's tkUnixKey.c takes the XIM branch and
 * we can carry typed UTF-8 across the JS->C boundary into Xutf8LookupString.
 * No preedit callbacks yet (composing strings reach Tk only after the OS
 * IME finalises them, via a single KeyPress carrying the composed bytes).
 *
 * Browser binding: XSetICFocus / XSetICValues(XNSpotLocation) cross over
 * to the host so the hidden <textarea> overlay (the OS IME's anchor in
 * canvas-land) follows the focused X widget. See src/host/text-input.ts.
 *
 * Side-channel for typed text: emx11_set_pending_key_text stuffs UTF-8
 * bytes into a per-display slot; emx11_push_key_event snapshots them into
 * a parallel queue at the event_queue tail; emx11_event_queue_pop copies
 * the corresponding slot into dpy->current_key_text; Xutf8LookupString
 * reads from there.
 */

#include "emx11_internal.h"

#include <emscripten.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -- Opaque XIM / XIC structs (Xlib only forward-decls _XIM/_XIC). ------- */

struct _XIM {
    Display *dpy;
};

struct _XIC {
    XIM      im;
    Window   client_window;
    Window   focus_window;
    XIMStyle input_style;
    /* Spot location (XNSpotLocation): caret position in window-local
     * pixels, set by Tk every time the entry's caret moves. We forward
     * (focus_window, x, y) to the host so the hidden textarea overlay
     * tracks the caret -- OS IME candidate windows then anchor there. */
    int      spot_x;
    int      spot_y;
    int      have_spot;
};

/* -- Bridges to host (text-input.ts) ------------------------------------- */

extern void emx11_js_xim_set_focus(Window window);
extern void emx11_js_xim_clear_focus(void);
extern void emx11_js_xim_set_spot(Window window, int x, int y);

/* Decode a captured nested list from XvaCreateNestedList.
 * Returns 1 + fills count/names/values when the pointer is one of ours,
 * 0 otherwise (Tk wraps preedit attrs in XVaCreateNestedList, so any
 * pointer we see at XCreateIC / XSetICValues for XNPreeditAttributes
 * IS one of ours). */
int emx11_nested_list_decode(void *list, int *count_out,
                             const char ***names_out, void ***values_out);

/* Apply XNSpotLocation / XNFontSet / etc. found inside a preedit /
 * status nested list to ic. Tk's XCreateIC always seeds a (0,0) spot
 * so we'd otherwise overwrite a valid spot from a previous Tk_SetCaretPos
 * with zeros -- gate the host bridge on `notify` (only fire from
 * XSetICValues, not from XCreateIC's seed). */
static void apply_preedit_attrs(XIC ic, void *list, int notify) {
    int n = 0;
    const char **names = NULL;
    void **values = NULL;
    if (!emx11_nested_list_decode(list, &n, &names, &values)) return;
    for (int i = 0; i < n; i++) {
        const char *k = names[i];
        if (!k) continue;
        if (strcmp(k, XNSpotLocation) == 0) {
            XPoint *pt = (XPoint *)values[i];
            if (pt) {
                ic->spot_x = pt->x;
                ic->spot_y = pt->y;
                ic->have_spot = 1;
                if (notify) {
                    emx11_js_xim_set_spot(ic->focus_window, pt->x, pt->y);
                }
            }
        }
        /* XNFontSet, XNArea, XNAreaNeeded, XNColormap, XNStdColormap,
         * XNForeground, XNBackground, XNBackgroundPixmap, XNLineSpace,
         * XNCursor: we don't draw preedit so the host doesn't need
         * font metrics from Tk's XCreateFontSet. The OS IME provides
         * its own candidate-window styling. */
    }
}

/* -- XIM / XIC lifecycle ------------------------------------------------- */

XIM XOpenIM(Display *dpy, struct _XrmHashBucketRec *db,
            char *res_name, char *res_class) {
    (void)db; (void)res_name; (void)res_class;
    if (!dpy) return NULL;
    XIM im = (XIM)calloc(1, sizeof(struct _XIM));
    if (!im) return NULL;
    im->dpy = dpy;
    return im;
}

Status XCloseIM(XIM im) {
    if (!im) return 0;
    free(im);
    return 1;
}

XIM XIMOfIC(XIC ic) {
    return ic ? ic->im : NULL;
}

/* XCreateIC reads varargs in (name, value, ...) form, terminated by NULL.
 * We honour XNClientWindow, XNFocusWindow, XNInputStyle. Preedit/Status
 * attribute lists (themselves nested name/value lists wrapped via
 * XVaCreateNestedList) are skipped -- Tier A doesn't draw preedit. */
static void ic_apply_va(XIC ic, va_list ap) {
    for (;;) {
        const char *name = va_arg(ap, const char *);
        if (!name) break;
        if (strcmp(name, XNClientWindow) == 0) {
            ic->client_window = (Window)va_arg(ap, unsigned long);
        } else if (strcmp(name, XNFocusWindow) == 0) {
            ic->focus_window = (Window)va_arg(ap, unsigned long);
        } else if (strcmp(name, XNInputStyle) == 0) {
            ic->input_style = (XIMStyle)va_arg(ap, unsigned long);
        } else if (strcmp(name, XNPreeditAttributes) == 0 ||
                   strcmp(name, XNStatusAttributes)  == 0) {
            void *list = va_arg(ap, void *);
            apply_preedit_attrs(ic, list, /*notify=*/0);
        } else {
            /* Unknown attr; consume one value to keep va_list aligned. */
            (void)va_arg(ap, void *);
        }
    }
}

XIC XCreateIC(XIM im, ...) {
    if (!im) return NULL;
    XIC ic = (XIC)calloc(1, sizeof(struct _XIC));
    if (!ic) return NULL;
    ic->im = im;
    va_list ap;
    va_start(ap, im);
    ic_apply_va(ic, ap);
    va_end(ap);
    return ic;
}

void XDestroyIC(XIC ic) {
    if (!ic) return;
    free(ic);
}

/* Tk only ever calls XSetICFocus when its focus window changes. We
 * mirror that to the host so the hidden textarea grabs DOM focus and
 * the OS IME starts pointing at this widget. */
void XSetICFocus(XIC ic) {
    if (!ic) return;
    emx11_js_xim_set_focus(ic->focus_window);
}

void XUnsetICFocus(XIC ic) {
    (void)ic;
    emx11_js_xim_clear_focus();
}

/* XSetICValues parses the same name/value form as XCreateIC. The one
 * field we act on at runtime is XNSpotLocation -- a pointer to an
 * XPoint with the caret position in window-local pixels. Tk's
 * tkUnixKey.c calls this on every cursor motion in entries/text. */
char *XSetICValues(XIC ic, ...) {
    if (!ic) return NULL;
    va_list ap;
    va_start(ap, ic);
    for (;;) {
        const char *name = va_arg(ap, const char *);
        if (!name) break;
        if (strcmp(name, XNFocusWindow) == 0) {
            ic->focus_window = (Window)va_arg(ap, unsigned long);
        } else if (strcmp(name, XNClientWindow) == 0) {
            ic->client_window = (Window)va_arg(ap, unsigned long);
        } else if (strcmp(name, XNInputStyle) == 0) {
            ic->input_style = (XIMStyle)va_arg(ap, unsigned long);
        } else if (strcmp(name, XNSpotLocation) == 0) {
            /* Direct (un-nested) XNSpotLocation -- not how Tk uses it,
             * but legal per spec. Some toolkits set it directly. */
            XPoint *pt = va_arg(ap, XPoint *);
            if (pt) {
                ic->spot_x = pt->x;
                ic->spot_y = pt->y;
                ic->have_spot = 1;
                emx11_js_xim_set_spot(ic->focus_window, pt->x, pt->y);
            }
        } else if (strcmp(name, XNPreeditAttributes) == 0 ||
                   strcmp(name, XNStatusAttributes)  == 0) {
            /* Tk's Tk_SetCaretPos always wraps the spot in
             * XNPreeditAttributes -> XVaCreateNestedList(XNSpotLocation,
             * &spot, NULL). Decode and forward. */
            void *list = va_arg(ap, void *);
            apply_preedit_attrs(ic, list, /*notify=*/1);
        } else {
            (void)va_arg(ap, void *);
        }
    }
    va_end(ap);
    return NULL;                                /* NULL == success */
}

char *XGetICValues(XIC ic, ...) {
    if (!ic) return NULL;
    va_list ap;
    va_start(ap, ic);
    for (;;) {
        const char *name = va_arg(ap, const char *);
        if (!name) break;
        void *out = va_arg(ap, void *);
        if (!out) continue;
        if (strcmp(name, XNFilterEvents) == 0) {
            /* Tell Tk which extra event-mask bits XIM needs selected on
             * the focus window. We only consume KeyPress; report 0 so
             * Tk's mask stays as it would have been without XIM. */
            *(unsigned long *)out = 0;
        }
        /* Other queries return whatever the slot was zero-initialised to;
         * Tk doesn't read XNFocusWindow/XNClientWindow back. */
    }
    va_end(ap);
    return NULL;
}

char *XGetIMValues(XIM im, ...) {
    (void)im;
    /* Tk asks for XNQueryInputStyle. We advertise XIMPreeditPosition so
     * Tk's tkUnixKey.c::Tk_SetCaretPos does fire XSetICValues with the
     * caret position -- without this Tk skips the spot update entirely
     * and our hidden-textarea overlay sits at (0,0) of the focus window.
     * XIMStatusNothing keeps Tk from asking us for a status area widget. */
    va_list ap;
    va_start(ap, im);
    for (;;) {
        const char *name = va_arg(ap, const char *);
        if (!name) break;
        void *out = va_arg(ap, void *);
        if (!out) continue;
        if (strcmp(name, XNQueryInputStyle) == 0) {
            static XIMStyle style_buf[] = {
                XIMPreeditPosition | XIMStatusNothing,
                XIMPreeditNothing  | XIMStatusNothing,
            };
            /* Tk frees this with XFree; allocate a block whose
             * supported_styles slot points into the same block so a
             * single free() releases everything. */
            XIMStyles *blob = (XIMStyles *)malloc(sizeof(XIMStyles) +
                                                  sizeof(style_buf));
            if (!blob) continue;
            XIMStyle *arr = (XIMStyle *)(blob + 1);
            memcpy(arr, style_buf, sizeof(style_buf));
            blob->count_styles    = sizeof(style_buf) / sizeof(style_buf[0]);
            blob->supported_styles = arr;
            *(XIMStyles **)out = blob;
        }
    }
    va_end(ap);
    return NULL;
}

/* -- Side-channel: typed UTF-8 carried alongside KeyPress ---------------- */

EMSCRIPTEN_KEEPALIVE
void emx11_set_pending_key_text(const char *utf8) {
    Display *dpy = emx11_get_display();
    if (!dpy) return;
    if (!utf8) {
        dpy->pending_key_text[0]   = '\0';
        dpy->pending_key_text_len  = 0;
        return;
    }
    size_t n = strnlen(utf8, EMX11_KEY_TEXT_SLOT - 1);
    memcpy(dpy->pending_key_text, utf8, n);
    dpy->pending_key_text[n] = '\0';
    dpy->pending_key_text_len = (int)n;
}

/* Called from event.c::emx11_push_key_event right after the event is
 * pushed; `slot` is the queue index that was just written. Snapshots the
 * pending-text buffer into the parallel array and clears pending so a
 * subsequent KeyRelease without text doesn't inherit stale bytes. */
void emx11_xim_capture_key_text(Display *dpy, unsigned int slot) {
    if (!dpy || slot >= EMX11_EVENT_QUEUE_CAPACITY) return;
    int n = dpy->pending_key_text_len;
    if (n < 0) n = 0;
    if (n >= EMX11_KEY_TEXT_SLOT) n = EMX11_KEY_TEXT_SLOT - 1;
    if (n > 0) memcpy(dpy->key_text_queue[slot], dpy->pending_key_text, n);
    dpy->key_text_queue[slot][n] = '\0';
    dpy->key_text_len_queue[slot] = n;
    /* Pending is consumed by this push. */
    dpy->pending_key_text[0]  = '\0';
    dpy->pending_key_text_len = 0;
}

/* Called from event_queue.c::emx11_event_queue_pop right BEFORE head
 * advances. Mirrors the popped slot's text into dpy->current_key_text
 * so Xutf8LookupString below reads the bytes that belong to the event
 * the caller is about to handle. */
void emx11_xim_capture_pop_text(Display *dpy, unsigned int slot) {
    if (!dpy) return;
    if (slot >= EMX11_EVENT_QUEUE_CAPACITY) {
        dpy->current_key_text[0]  = '\0';
        dpy->current_key_text_len = 0;
        return;
    }
    int n = dpy->key_text_len_queue[slot];
    if (n < 0) n = 0;
    if (n >= EMX11_KEY_TEXT_SLOT) n = EMX11_KEY_TEXT_SLOT - 1;
    if (n > 0) memcpy(dpy->current_key_text,
                      dpy->key_text_queue[slot], n);
    dpy->current_key_text[n]  = '\0';
    dpy->current_key_text_len = n;
    /* Don't bother clearing key_text_queue[slot] -- next push to that
     * slot overwrites unconditionally. */
}

/* -- Xutf8LookupString --------------------------------------------------- *
 *
 * Tk's tkUnixKey.c calls this when an XIC is bound to the toplevel.
 * Returns the number of UTF-8 bytes copied (excluding NUL), the
 * resolved keysym, and a status code:
 *   XLookupNone  -- no keysym, no string
 *   XLookupKeySym -- keysym only (function/cursor keys, modifiers)
 *   XLookupChars -- string only (composed input; keysym=NoSymbol)
 *   XLookupBoth  -- both (typical printable key)
 * Buffer is NOT NUL-terminated by spec, but Tk null-terminates after.
 */
int Xutf8LookupString(XIC ic, XKeyPressedEvent *event,
                      char *buffer_return, int bytes_buffer,
                      KeySym *keysym_return, Status *status_return) {
    (void)ic;
    Display *dpy = event ? event->display : emx11_get_display();
    KeySym sym = NoSymbol;
    if (event) sym = XLookupKeysym(event, 0);
    if (keysym_return) *keysym_return = sym;

    int written = 0;
    int n = dpy ? dpy->current_key_text_len : 0;
    if (n > 0 && buffer_return && bytes_buffer > 0) {
        int copy = n < bytes_buffer ? n : bytes_buffer;
        memcpy(buffer_return, dpy->current_key_text, copy);
        written = copy;
    } else if (buffer_return && bytes_buffer > 0) {
        buffer_return[0] = '\0';
    }

    if (status_return) {
        if (written > 0 && sym != NoSymbol)        *status_return = XLookupBoth;
        else if (written > 0)                      *status_return = XLookupChars;
        else if (sym != NoSymbol)                  *status_return = XLookupKeySym;
        else                                       *status_return = XLookupNone;
    }
    return written;
}

/* XmbLookupString is the multibyte variant; clients almost always link
 * XutfLookupString, but Xt/Tk fall back to it when locale isn't UTF-8.
 * Browser side is always UTF-8 so we route through the same machinery. */
int XmbLookupString(XIC ic, XKeyPressedEvent *event,
                    char *buffer_return, int bytes_buffer,
                    KeySym *keysym_return, Status *status_return) {
    return Xutf8LookupString(ic, event, buffer_return, bytes_buffer,
                             keysym_return, status_return);
}

/* XwcLookupString writes wchar_t into the buffer. Decode our UTF-8 slot
 * back to codepoints. wchar_t on emscripten is 32-bit. */
int XwcLookupString(XIC ic, XKeyPressedEvent *event,
                    wchar_t *buffer_return, int wchars_buffer,
                    KeySym *keysym_return, Status *status_return) {
    (void)ic;
    Display *dpy = event ? event->display : emx11_get_display();
    KeySym sym = NoSymbol;
    if (event) sym = XLookupKeysym(event, 0);
    if (keysym_return) *keysym_return = sym;

    int written = 0;
    if (dpy && dpy->current_key_text_len > 0 &&
        buffer_return && wchars_buffer > 0) {
        const unsigned char *p = (const unsigned char *)dpy->current_key_text;
        int n = dpy->current_key_text_len;
        int i = 0;
        while (i < n && written < wchars_buffer) {
            unsigned int cp = 0;
            unsigned char c = p[i];
            if (c < 0x80) { cp = c; i += 1; }
            else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
                cp = ((c & 0x1F) << 6) | (p[i+1] & 0x3F); i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
                cp = ((c & 0x0F) << 12) | ((p[i+1] & 0x3F) << 6) |
                     (p[i+2] & 0x3F); i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
                cp = ((c & 0x07) << 18) | ((p[i+1] & 0x3F) << 12) |
                     ((p[i+2] & 0x3F) << 6) | (p[i+3] & 0x3F); i += 4;
            } else { i += 1; continue; }
            buffer_return[written++] = (wchar_t)cp;
        }
    }

    if (status_return) {
        if (written > 0 && sym != NoSymbol)  *status_return = XLookupBoth;
        else if (written > 0)                *status_return = XLookupChars;
        else if (sym != NoSymbol)            *status_return = XLookupKeySym;
        else                                 *status_return = XLookupNone;
    }
    return written;
}

/* XFilterEvent stays False for Tier A: we don't intercept keys for
 * preedit redraw. Tk delivers the KeyPress straight to its handler
 * which calls Xutf8LookupString and inserts the bytes. The stub in
 * Cursor.c remains authoritative. */

/* -- XIM registration callbacks. Tk uses them to register a "tell me when
 * an XIM appears" watcher; XOpenIM already succeeds on the first call so
 * the callback never has to fire. */

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

/* -- XDisplayOfIM -- */

Display *XDisplayOfIM(XIM im) {
    (void)im; return NULL;
}

/* -- XVaCreateNestedList + decoder -- */

static const char EMX11_NESTED_LIST_MAGIC[] = "emx11-nested-list";

XVaNestedList XVaCreateNestedList(int unused_dummy, ...) {
    (void)unused_dummy;
    va_list ap;
    int n = 0;
    va_start(ap, unused_dummy);
    for (;;) {
        const char *name = va_arg(ap, const char *);
        if (!name) break;
        (void)va_arg(ap, void *);
        n++;
    }
    va_end(ap);

    void **buf = (void **)calloc(2 + 2 * n + 1, sizeof(void *));
    if (!buf) return NULL;
    buf[0] = (void *)EMX11_NESTED_LIST_MAGIC;
    buf[1] = (void *)(uintptr_t)n;
    int o = 2;
    va_start(ap, unused_dummy);
    for (int i = 0; i < n; i++) {
        const char *name = va_arg(ap, const char *);
        void *value      = va_arg(ap, void *);
        buf[o++] = (void *)name;
        buf[o++] = value;
    }
    va_end(ap);
    buf[o] = NULL;
    return (XVaNestedList)buf;
}

int emx11_nested_list_decode(void *list, int *count_out,
                             const char ***names_out, void ***values_out) {
    if (!list) return 0;
    void **slots = (void **)list;
    if (slots[0] != (void *)EMX11_NESTED_LIST_MAGIC) return 0;
    int n = (int)(uintptr_t)slots[1];
    static const char *name_buf[16];
    static void       *val_buf[16];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        name_buf[i] = (const char *)slots[2 + 2 * i + 0];
        val_buf[i]  = slots[2 + 2 * i + 1];
    }
    if (count_out)  *count_out  = n;
    if (names_out)  *names_out  = name_buf;
    if (values_out) *values_out = val_buf;
    return 1;
}
