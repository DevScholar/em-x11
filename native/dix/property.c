/*
 * Window properties (XChangeProperty / XGetWindowProperty etc).
 *
 * Storage lives on the Host TS side, keyed by (window XID, atom), so
 * any client can read back what any client wrote -- the shape real
 * X.Org uses (dix/property.c hangs PropertyRec off the server's
 * WindowPtr, not off any client). This file is now a thin bridge.
 *
 * Scope simplifications:
 *   - No PropertyNotify event generated on change/delete.
 *   - PropModeReplace / PropModeAppend / PropModePrepend all work; no
 *     per-mode atomicity concerns in a single-threaded wasm process.
 */

#include "emx11_internal.h"
#include "emx11_meta_layout.h"

#include <stdlib.h>
#include <string.h>

static int format_bytes(int format) {
    switch (format) {
    case 8:  return 1;
    case 16: return 2;
    case 32: return 4;
    default: return 0;
    }
}

int XChangeProperty(Display *display, Window w, Atom property, Atom type,
                    int format, int mode,
                    _Xconst unsigned char *data, int nelements) {
    if (format_bytes(format) == 0) return 0;
    /* INCR back-channel: when the clipboard proxy window is the requestor of
     * a large selection transfer, Tk writes chunks here. Intercept them to
     * accumulate and eventually push to navigator.clipboard.writeText(). */
    if (display &&
        display->clipboard_proxy_win != None &&
        w == display->clipboard_proxy_win &&
        display->incr_active) {
        if (emx11_incr_handle_chunk(display, property,
                                    (const unsigned char *)data,
                                    nelements, format)) {
            return 1;   /* consumed; do not store in JS property table */
        }
    }
    int rc = emx11_js_change_property(w, property, type, format, mode,
                                      data, nelements);
    /* -1 = BadWindow, 0 = BadMatch, 1 = Success. Xlib's XChangeProperty
     * returns an int that clients rarely check; we map both errors to 0
     * so the signature is consistent with the previous implementation. */
    return rc == 1 ? 1 : 0;
}

int XGetWindowProperty(Display *display, Window w, Atom property,
                       long long_offset, long long_length, Bool delete_prop,
                       Atom req_type, Atom *actual_type_return,
                       int *actual_format_return,
                       unsigned long *nitems_return,
                       unsigned long *bytes_after_return,
                       unsigned char **prop_return) {
    (void)display;
    if (actual_type_return)   *actual_type_return   = None;
    if (actual_format_return) *actual_format_return = 0;
    if (nitems_return)        *nitems_return        = 0;
    if (bytes_after_return)   *bytes_after_return   = 0;
    if (prop_return)          *prop_return          = NULL;

    int meta[EMX11_PROP_META_SIZE] = {0};
    emx11_js_get_property_meta(w, property, req_type,
                               long_offset, long_length, meta);
    if (!meta[EMX11_PROP_META_PRESENT]) return BadWindow;

    if (actual_type_return)   *actual_type_return   = (Atom)meta[EMX11_PROP_META_TYPE];
    if (actual_format_return) *actual_format_return = meta[EMX11_PROP_META_FORMAT];
    if (nitems_return)        *nitems_return        = (unsigned long)meta[EMX11_PROP_META_NITEMS];
    if (bytes_after_return)   *bytes_after_return   = (unsigned long)meta[EMX11_PROP_META_BYTES_AFTER];

    /* No data to return (atom missing, or type mismatch): just ack the
     * meta. dix/property.c ProcGetProperty in the type-mismatch case
     * still reports actualType/format; our meta path already did that. */
    if (!meta[EMX11_PROP_META_FOUND] || meta[EMX11_PROP_META_DATA_LEN] == 0) {
        return Success;
    }

    /* Xlib convention: allocate one extra byte and NUL-terminate so
     * callers printing the result as a C string don't walk off the end. */
    size_t data_bytes = (size_t)meta[EMX11_PROP_META_DATA_LEN];
    unsigned char *buf = malloc(data_bytes + 1);
    if (!buf) return BadAlloc;
    emx11_js_get_property_data(w, property, req_type, long_offset,
                               long_length, delete_prop ? 1 : 0,
                               buf, (int)data_bytes);
    buf[data_bytes] = 0;
    if (prop_return) *prop_return = buf;
    else             free(buf);
    return Success;
}

int XDeleteProperty(Display *display, Window w, Atom property) {
    (void)display;
    emx11_js_delete_property(w, property);
    return 1;
}

Atom *XListProperties(Display *display, Window w, int *num_prop_return) {
    (void)display;
    if (num_prop_return) *num_prop_return = 0;
    int n = emx11_js_list_properties_count(w);
    if (n <= 0) return NULL;
    Atom *list = malloc(sizeof(Atom) * (size_t)n);
    if (!list) return NULL;
    int got = emx11_js_list_properties_fetch(w, list, n);
    if (num_prop_return) *num_prop_return = got;
    return list;
}
