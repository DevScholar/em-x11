/*
 * ICCCM selection protocol + CLIPBOARD ↔ browser clipboard bridge.
 *
 * This file implements X11 selection semantics end-to-end:
 *
 *   Layer 1 — real ICCCM.  Per-Display table of (atom → owner) with
 *     SelectionClear on owner transition; XConvertSelection routes a
 *     SelectionRequest to the owner, which responds via XChangeProperty
 *     plus XSendEvent of a SelectionNotify (Tk's tkUnixSelect.c already
 *     implements that side). Generic — any X11 client benefits.
 *
 *   Layer 2 — browser bridge (CLIPBOARD atom only).  A sentinel XID
 *     (EMX11_CLIPBOARD_PROXY_WIN) stands in as the virtual owner of
 *     CLIPBOARD whenever no real client has claimed it. When the proxy
 *     "owns" CLIPBOARD:
 *       * XConvertSelection(CLIPBOARD, target, ...) awaits
 *         navigator.clipboard.readText() (via Asyncify), encodes per
 *         target atom, stores into requestor's property, and queues
 *         SelectionNotify.
 *     When a real client claims CLIPBOARD:
 *       * We replace the proxy in the table AND fire a back-channel
 *         XConvertSelection at the new owner with the proxy as requestor;
 *         XSendEvent intercepts the owner's SelectionNotify reply (see
 *         emx11_selection_intercept_send in event.c) and forwards the
 *         UTF-8 bytes to navigator.clipboard.writeText().
 *
 *   PRIMARY and custom atoms are Layer 1 only (no browser bridge); this
 *   preserves the X11 separation between "mouse-selection" and "copy-
 *   buffer" that Unix users expect.
 *
 * References:
 *   ICCCM §2 — Peer-to-Peer Communication by Means of Selections
 *   x11protocol.txt — SendEvent, ChangeProperty, ConvertSelection
 */

#include "emx11_internal.h"

#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/*  Cached atoms.                                                            */
/* ------------------------------------------------------------------------- */

/* Populate the CLIPBOARD / UTF8_STRING / TARGETS / ... atoms on first
 * use, and allocate + register the virtual CLIPBOARD owner window with
 * the Host. The proxy has to be a real XID the Host has seen via
 * emx11_js_window_create, otherwise XChangeProperty and XGetWindowProperty
 * fail silently (attrsOf check in host.ts:changeProperty/peekProperty).
 *
 * We create the proxy invisibly: root as parent, 1x1 size, never mapped.
 * The Host's compositor keeps an attr record for it so property ops
 * succeed; nothing ever paints because mapped stays false. */
void emx11_selection_ensure_atoms(Display *dpy) {
    if (!dpy->atom_clipboard) {
        dpy->atom_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    }
    if (!dpy->atom_utf8_string) {
        dpy->atom_utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    }
    if (!dpy->atom_targets) {
        dpy->atom_targets = XInternAtom(dpy, "TARGETS", False);
    }
    if (!dpy->atom_timestamp) {
        dpy->atom_timestamp = XInternAtom(dpy, "TIMESTAMP", False);
    }
    if (!dpy->atom_text) {
        dpy->atom_text = XInternAtom(dpy, "TEXT", False);
    }
    if (!dpy->atom_incr) {
        dpy->atom_incr = XInternAtom(dpy, "INCR", False);
    }
    if (!dpy->atom_emx11_clipboard_data) {
        dpy->atom_emx11_clipboard_data =
            XInternAtom(dpy, "_EMX11_CLIPBOARD_DATA", False);
    }
    if (!dpy->clipboard_proxy_win) {
        Window root = dpy->screens[dpy->default_screen].root;
        if (root != None) {
            Window w = emx11_next_xid(dpy);
            emx11_js_window_create(dpy->conn_id, w, root,
                                   0, 0, 1, 1, 0, 0, 0, 0);
            dpy->clipboard_proxy_win = w;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Selection table.                                                         */
/* ------------------------------------------------------------------------- */

#define SEL_TABLE_SIZE                                                   \
    (sizeof(((Display *)0)->selections) /                                \
     sizeof(((Display *)0)->selections[0]))

/* Look up the slot for `sel`. Returns NULL if not present. */
static int sel_find(Display *dpy, Atom sel) {
    for (int i = 0; i < (int)SEL_TABLE_SIZE; i++) {
        if (dpy->selections[i].sel == sel && sel != 0) return i;
    }
    return -1;
}

/* Allocate a free slot (sel==0). Returns -1 if full. */
static int sel_alloc(Display *dpy) {
    for (int i = 0; i < (int)SEL_TABLE_SIZE; i++) {
        if (dpy->selections[i].sel == 0) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/*  Event push helpers.                                                      */
/*                                                                           */
/*  Selection events are unmaskable in X (see event.c:event_type_to_mask),  */
/*  so we push them directly onto the target window's queue without mask    */
/*  filtering. emx11_event_queue_push maintains dpy->qlen for Tk's event    */
/*  pump (TransferXEventsToTcl).                                             */
/* ------------------------------------------------------------------------- */

void emx11_push_selection_clear(Display *dpy, Window owner,
                                Atom selection, Time time) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xselectionclear.type      = SelectionClear;
    ev.xselectionclear.display   = dpy;
    ev.xselectionclear.window    = owner;
    ev.xselectionclear.selection = selection;
    ev.xselectionclear.time      = time;
    emx11_event_queue_push(dpy, &ev);
}

void emx11_push_selection_request(Display *dpy, Window owner,
                                  Window requestor, Atom selection,
                                  Atom target, Atom property, Time time) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xselectionrequest.type      = SelectionRequest;
    ev.xselectionrequest.display   = dpy;
    ev.xselectionrequest.owner     = owner;
    ev.xselectionrequest.requestor = requestor;
    ev.xselectionrequest.selection = selection;
    ev.xselectionrequest.target    = target;
    ev.xselectionrequest.property  = property;
    ev.xselectionrequest.time      = time;
    /* xany.window is what emx11_event_queue_peek_typed keys on; mirror
     * the owner there so Tk's dispatch (by xany.window) finds it. */
    ev.xany.window = owner;
    emx11_event_queue_push(dpy, &ev);
}

void emx11_push_selection_notify(Display *dpy, Window requestor,
                                 Atom selection, Atom target,
                                 Atom property, Time time) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xselection.type      = SelectionNotify;
    ev.xselection.display   = dpy;
    ev.xselection.requestor = requestor;
    ev.xselection.selection = selection;
    ev.xselection.target    = target;
    ev.xselection.property  = property;
    ev.xselection.time      = time;
    ev.xany.window = requestor;
    emx11_event_queue_push(dpy, &ev);
}

void emx11_push_property_notify(Display *dpy, Window win, Atom prop, int state) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xproperty.type    = PropertyNotify;
    ev.xproperty.display = dpy;
    ev.xproperty.window  = win;
    ev.xproperty.atom    = prop;
    ev.xproperty.state   = state;
    ev.xany.window       = win;
    emx11_event_queue_push(dpy, &ev);
}

/* ------------------------------------------------------------------------- */
/*  Back-channel: extract UTF-8 from the new CLIPBOARD owner and push it    */
/*  to the browser. Called from XSetSelectionOwner(CLIPBOARD, real_win).    */
/*                                                                           */
/*  We can't synchronously extract the text here — the owner hasn't had a   */
/*  chance to process any SelectionRequest yet. So we fire a normal         */
/*  XConvertSelection targeting our proxy window; the owner will respond    */
/*  on its next event-loop turn via XChangeProperty + XSendEvent. The       */
/*  XSendEvent intercept path (emx11_selection_intercept_send) detects      */
/*  the proxy requestor and bridges to navigator.clipboard.writeText().     */
/* ------------------------------------------------------------------------- */

static void fire_proxy_convert(Display *dpy, Window owner, Time time) {
    emx11_selection_ensure_atoms(dpy);
    emx11_push_selection_request(dpy, owner,
                                 dpy->clipboard_proxy_win,
                                 dpy->atom_clipboard,
                                 dpy->atom_utf8_string,
                                 dpy->atom_emx11_clipboard_data,
                                 time);
}

/* ------------------------------------------------------------------------- */
/*  Virtual-owner service: serve CLIPBOARD reads from the browser.          */
/*                                                                           */
/*  Called from XConvertSelection when the proxy "owns" CLIPBOARD. Encodes  */
/*  the browser's clipboard text per the requested target, writes the data  */
/*  into the requestor's property, and queues a SelectionNotify reply.      */
/*                                                                           */
/*  Target fall-back: anything unrecognised gets SelectionNotify{property=  */
/*  None}, the ICCCM "refuse" response. Tk then surfaces this as an empty   */
/*  selection, which matches how real X clients treat unsupported targets.  */
/* ------------------------------------------------------------------------- */

/* Normalize Windows-style CRLF (\r\n) to Unix LF (\n). In UTF-8, \r (0x0D)
 * and \n (0x0A) are always single bytes regardless of surrounding multi-byte
 * sequences, so a straight byte scan is safe. Returns the new length; caller
 * must free *out_bytes. */
static int strip_crlf(const unsigned char *in, int in_len,
                      unsigned char **out_bytes) {
    unsigned char *buf = malloc((size_t)in_len + 1);
    if (!buf) { *out_bytes = NULL; return 0; }
    int w = 0;
    for (int i = 0; i < in_len; i++) {
        if (in[i] == 0x0D && i + 1 < in_len && in[i+1] == 0x0A)
            continue;              /* skip \r immediately before \n */
        buf[w++] = in[i];
    }
    *out_bytes = buf;
    return w;
}

/* Convert `text_len` UTF-8 bytes starting at `text` into a fresh Latin-1
 * buffer, substituting '?' for any codepoint > 0xFF. Returns the new
 * length; caller must free *out_bytes. */
static int utf8_to_latin1(const unsigned char *text, int text_len,
                          unsigned char **out_bytes) {
    unsigned char *buf = malloc((size_t)text_len + 1);
    if (!buf) { *out_bytes = NULL; return 0; }
    int w = 0;
    int i = 0;
    while (i < text_len) {
        unsigned int c = text[i];
        unsigned int cp;
        int advance;
        if (c < 0x80) {
            cp = c; advance = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text_len) {
            cp = ((c & 0x1Fu) << 6) | (text[i+1] & 0x3Fu); advance = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text_len) {
            cp = ((c & 0x0Fu) << 12) | ((text[i+1] & 0x3Fu) << 6)
                 | (text[i+2] & 0x3Fu);
            advance = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text_len) {
            cp = ((c & 0x07u) << 18) | ((text[i+1] & 0x3Fu) << 12)
                 | ((text[i+2] & 0x3Fu) << 6) | (text[i+3] & 0x3Fu);
            advance = 4;
        } else {
            cp = '?'; advance = 1;                   /* malformed: skip */
        }
        buf[w++] = (cp <= 0xFF) ? (unsigned char)cp : (unsigned char)'?';
        i += advance;
    }
    *out_bytes = buf;
    return w;
}

/* Answer TARGETS: atom list of formats we can synthesize from the
 * browser clipboard (plus the meta-atoms TARGETS / TIMESTAMP per ICCCM). */
static void serve_targets(Display *dpy, Window requestor, Atom property) {
    emx11_selection_ensure_atoms(dpy);
    Atom atoms[5];
    atoms[0] = dpy->atom_targets;
    atoms[1] = dpy->atom_timestamp;
    atoms[2] = dpy->atom_utf8_string;
    atoms[3] = XA_STRING;
    atoms[4] = dpy->atom_text;
    XChangeProperty(dpy, requestor, property, XA_ATOM, 32,
                    PropModeReplace,
                    (unsigned char *)atoms,
                    (int)(sizeof(atoms) / sizeof(atoms[0])));
}

/* Serve the conversion and return True if the property was written
 * (requestor gets SelectionNotify{property=prop}), or False if we
 * refused (caller will queue SelectionNotify{property=None}). */
static Bool serve_clipboard_from_browser(Display *dpy, Atom target,
                                         Window requestor, Atom property,
                                         Time time) {
    emx11_selection_ensure_atoms(dpy);

    if (target == dpy->atom_targets) {
        serve_targets(dpy, requestor, property);
        return True;
    }
    if (target == dpy->atom_timestamp) {
        long t = (long)time;
        XChangeProperty(dpy, requestor, property, XA_INTEGER, 32,
                        PropModeReplace, (unsigned char *)&t, 1);
        return True;
    }

    /* Everything below needs the actual clipboard text. Split read:
     *   _begin returns the byte length (>=0) after awaiting the browser,
     *   _fetch copies those bytes into our malloc'd buffer.
     * Asyncify suspends only on _begin; _fetch is synchronous. */
    int text_len = emx11_js_clipboard_read_begin();
    if (text_len < 0) return False;          /* permission denied / error */

    unsigned char *utf8 = NULL;
    if (text_len > 0) {
        unsigned char *raw = malloc((size_t)text_len);
        if (!raw) return False;
        emx11_js_clipboard_read_fetch(raw, text_len);
        /* Normalize CRLF → LF: Windows clipboard sources use \r\n, but Tk
         * entry/text widgets expect bare \n. strip_crlf always allocates a
         * fresh buffer so `raw` can be freed unconditionally. */
        text_len = strip_crlf(raw, text_len, &utf8);
        free(raw);
        if (!utf8) return False;
    } else {
        /* Empty clipboard: still write a zero-byte property per ICCCM
         * (nitems=0 is a valid response). Avoids leaking malloc(0). */
        utf8 = (unsigned char *)"";
    }

    Bool ok = False;
    if (target == dpy->atom_utf8_string || target == dpy->atom_text) {
        if (XChangeProperty(dpy, requestor, property,
                            dpy->atom_utf8_string, 8,
                            PropModeReplace, utf8, text_len)) {
            ok = True;
        }
    } else if (target == XA_STRING) {
        unsigned char *latin1 = NULL;
        int latin1_len = utf8_to_latin1(utf8, text_len, &latin1);
        if (latin1) {
            if (XChangeProperty(dpy, requestor, property,
                                XA_STRING, 8,
                                PropModeReplace, latin1, latin1_len)) {
                ok = True;
            }
            free(latin1);
        }
    }
    /* Other targets (COMPOUND_TEXT, MULTIPLE, ...) deliberately unhandled:
     * the ICCCM "refuse" response (property=None) is correct, and Tk will
     * retry with UTF8_STRING on its own. */

    if (text_len > 0) free(utf8);
    return ok;
}

/* ------------------------------------------------------------------------- */
/*  INCR back-channel chunk handler (ICCCM §2.7).                           */
/*                                                                           */
/*  Called from XChangeProperty when the owner writes a chunk (or the       */
/*  zero-length terminator) to the proxy window's property during an active  */
/*  INCR transfer. Accumulates bytes and drives the handshake by pushing    */
/*  PropertyNotify(Delete) back into the event queue so Tk's INCR owner     */
/*  state machine continues on its next event-loop turn.                    */
/* ------------------------------------------------------------------------- */

Bool emx11_incr_handle_chunk(Display *dpy, Atom property,
                             const unsigned char *data, int nelements,
                             int format) {
    if (!dpy->incr_active)              return False;
    if (property != dpy->incr_property) return False;

    if (nelements == 0) {
        /* Zero-length chunk: end-of-transfer. Write accumulated UTF-8 to
         * the browser clipboard and clean up state. */
        if (dpy->incr_buf && dpy->incr_len > 0) {
            emx11_js_clipboard_write_utf8(dpy->incr_buf, dpy->incr_len);
        }
        free(dpy->incr_buf);
        dpy->incr_buf    = NULL;
        dpy->incr_len    = 0;
        dpy->incr_cap    = 0;
        dpy->incr_active = 0;

        /* Evict the CLIPBOARD owner so subsequent Ctrl+C re-enters the
         * back-channel (same logic as the inline path in intercept_send). */
        int slot = sel_find(dpy, dpy->atom_clipboard);
        if (slot >= 0) {
            Window ex_owner = dpy->selections[slot].owner;
            Time   ex_time  = dpy->selections[slot].time;
            dpy->selections[slot].sel   = 0;
            dpy->selections[slot].owner = None;
            dpy->selections[slot].time  = 0;
            if (ex_owner != None && ex_owner != dpy->clipboard_proxy_win) {
                emx11_push_selection_clear(dpy, ex_owner,
                                           dpy->atom_clipboard, ex_time);
            }
        }
        return True;
    }

    /* Non-zero chunk: only format==8 carries text bytes. Ignore anything
     * else (INCR size-hint is fmt=32; the owner writes that only once as
     * the initial marker, which intercept_send already consumed). */
    if (format == 8) {
        int needed = dpy->incr_len + nelements;
        if (needed > dpy->incr_cap) {
            int new_cap = needed + needed / 2 + 1024;
            unsigned char *nb = realloc(dpy->incr_buf, (size_t)new_cap);
            if (!nb) {
                /* Allocation failure: abort, discard accumulated data. */
                free(dpy->incr_buf);
                dpy->incr_buf    = NULL;
                dpy->incr_len    = 0;
                dpy->incr_cap    = 0;
                dpy->incr_active = 0;
                return True;    /* consume but throw away */
            }
            dpy->incr_buf = nb;
            dpy->incr_cap = new_cap;
        }
        memcpy(dpy->incr_buf + dpy->incr_len, data, (size_t)nelements);
        dpy->incr_len += nelements;
    }

    /* Signal the owner: "property deleted, ready for next chunk". */
    emx11_push_property_notify(dpy, dpy->clipboard_proxy_win,
                                dpy->incr_property, PropertyDelete);
    return True;
}

/* ------------------------------------------------------------------------- */
/*  Public Xlib entry points.                                                */
/* ------------------------------------------------------------------------- */

int XSetSelectionOwner(Display *dpy, Atom selection, Window owner, Time time) {
    if (!dpy || selection == 0) return 0;
    emx11_selection_ensure_atoms(dpy);

    int slot = sel_find(dpy, selection);
    Window old_owner = (slot >= 0) ? dpy->selections[slot].owner : None;

    if (owner == None) {
        /* Relinquishing: free the slot. Don't push SelectionClear to
         * ourselves — the caller knows they're releasing. */
        if (slot >= 0) {
            dpy->selections[slot].sel   = 0;
            dpy->selections[slot].owner = None;
            dpy->selections[slot].time  = 0;
        }
        return 1;
    }

    /* Claiming. Evict old owner (if any, and distinct from the new one)
     * per ICCCM §2.1: the losing owner gets a SelectionClear. Skip if
     * the previous "owner" was our own clipboard proxy — it's a virtual
     * window, no real client to notify. */
    if (old_owner != None &&
        old_owner != owner &&
        old_owner != dpy->clipboard_proxy_win) {
        emx11_push_selection_clear(dpy, old_owner, selection, time);
    }

    if (slot < 0) slot = sel_alloc(dpy);
    if (slot < 0) return 0;                  /* table full — rare */
    dpy->selections[slot].sel   = selection;
    dpy->selections[slot].owner = owner;
    dpy->selections[slot].time  = time;

    /* Layer 2: CLIPBOARD write-through. The real owner holds the data in
     * its own buffers; we ask for it via a proxy-targeted convert, then
     * the XSendEvent intercept routes the UTF-8 reply to
     * navigator.clipboard.writeText(). */
    if (selection == dpy->atom_clipboard &&
        owner != dpy->clipboard_proxy_win) {
        fire_proxy_convert(dpy, owner, time);
    }
    return 1;
}

Window XGetSelectionOwner(Display *dpy, Atom selection) {
    if (!dpy || selection == 0) return None;
    emx11_selection_ensure_atoms(dpy);

    int slot = sel_find(dpy, selection);
    if (slot >= 0) return dpy->selections[slot].owner;

    /* CLIPBOARD virtual ownership: if nobody has claimed it, the browser
     * clipboard is conceptually the owner. Return the proxy XID so
     * Tk_GetSelection doesn't short-circuit to "no owner". */
    if (selection == dpy->atom_clipboard) {
        return dpy->clipboard_proxy_win;
    }
    return None;
}

int XConvertSelection(Display *dpy, Atom selection, Atom target,
                      Atom property, Window requestor, Time time) {
    if (!dpy) return 0;
    emx11_selection_ensure_atoms(dpy);

    Window owner = XGetSelectionOwner(dpy, selection);

    /* Proxy owns CLIPBOARD → serve from browser clipboard. */
    if (selection == dpy->atom_clipboard &&
        owner == dpy->clipboard_proxy_win) {
        Atom reply_prop = property;
        if (!serve_clipboard_from_browser(dpy, target, requestor,
                                          property, time)) {
            reply_prop = None;               /* ICCCM "refuse" response */
        }
        emx11_push_selection_notify(dpy, requestor, selection, target,
                                    reply_prop, time);
        return 1;
    }

    /* Standard path: no owner → null SelectionNotify; owner present →
     * SelectionRequest to owner. */
    if (owner == None) {
        emx11_push_selection_notify(dpy, requestor, selection, target,
                                    None, time);
    } else {
        emx11_push_selection_request(dpy, owner, requestor, selection,
                                     target, property, time);
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/*  XSendEvent intercept: proxy-window SelectionNotify → browser clipboard. */
/*                                                                           */
/*  Called from event.c:XSendEvent before the event is queued. When a real  */
/*  CLIPBOARD owner has finished filling in its property for our proxy's    */
/*  self-request, it calls XSendEvent(proxy, SelectionNotify{...}). We      */
/*  intercept that reply here: read the UTF-8 bytes the owner wrote, push   */
/*  them to navigator.clipboard.writeText(), and consume the event.         */
/* ------------------------------------------------------------------------- */

Bool emx11_selection_intercept_send(Display *dpy, Window w, const XEvent *ev) {
    if (!dpy || !ev) return False;
    emx11_selection_ensure_atoms(dpy);

    if (w != dpy->clipboard_proxy_win) return False;
    if (ev->type != SelectionNotify)   return False;

    /* Owner refused the conversion: nothing to push, but we still consume
     * the event since queuing it to the proxy accomplishes nothing. */
    if (ev->xselection.property == None) return True;

    Atom          actual_type  = None;
    int           actual_fmt   = 0;
    unsigned long nitems       = 0;
    unsigned long bytes_after  = 0;
    unsigned char *data        = NULL;

    int rc = XGetWindowProperty(dpy, w, ev->xselection.property,
                                0, 0x7FFFFFFF, True /* delete */,
                                AnyPropertyType,
                                &actual_type, &actual_fmt,
                                &nitems, &bytes_after, &data);
    if (rc == Success && actual_type == dpy->atom_incr && actual_fmt == 32) {
        /* INCR marker: owner's data exceeds its single-message threshold.
         * The INCR property value (a size hint) was already deleted by the
         * XGetWindowProperty call above (delete=True). Initialise the state
         * machine and push the first PropertyNotify(Delete) so Tk's INCR
         * owner handler fires on its next event-loop turn and writes chunk 1. */
        if (data) free(data);
        dpy->incr_active   = 1;
        dpy->incr_property = ev->xselection.property;
        dpy->incr_buf      = NULL;
        dpy->incr_len      = 0;
        dpy->incr_cap      = 0;
        emx11_push_property_notify(dpy, dpy->clipboard_proxy_win,
                                    dpy->incr_property, PropertyDelete);
        return True;
    }

    if (rc == Success && data && nitems > 0 && actual_fmt == 8) {
        emx11_js_clipboard_write_utf8(data, (int)nitems);

        /* Evict the current CLIPBOARD owner so subsequent Ctrl+C re-enters
         * Tk_ClipboardClear's `!clipboardActive` branch (tkClipboard.c:284)
         * and re-fires the back-channel. Without this, Tk thinks it owns
         * CLIPBOARD forever (a real X server would have delivered
         * SelectionClear when another client took ownership; the browser
         * clipboard gives us no such signal). Side benefit: handles the
         * "user copied something in another tab externally" case -- the
         * next XConvertSelection falls to the browser proxy path and
         * reads fresh text.
         *
         * We use the recorded time of the current owner so Tk's
         * SelectionClear handler accepts it (x11protocol §SelectionClear:
         * time >= last owner-set time). */
        int slot = sel_find(dpy, dpy->atom_clipboard);
        if (slot >= 0) {
            Window ex_owner = dpy->selections[slot].owner;
            Time   ex_time  = dpy->selections[slot].time;
            dpy->selections[slot].sel   = 0;
            dpy->selections[slot].owner = None;
            dpy->selections[slot].time  = 0;
            if (ex_owner != None && ex_owner != dpy->clipboard_proxy_win) {
                emx11_push_selection_clear(dpy, ex_owner,
                                           dpy->atom_clipboard, ex_time);
            }
        }
    }
    if (data) free(data);
    return True;
}
