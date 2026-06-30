/*
 * XIM / XIC -- input-method plumbing with inline preedit support.
 *
 * Host bridges: XSetICFocus / XSetICValues(XNSpotLocation) cross over
 * to the host so the hidden <textarea> overlay anchors the OS IME. The
 * host feeds composition* events back through em_x11_xim_preedit_start /
 * _draw / _done, which invoke Tk's preedit callbacks so Tk renders
 * composing text inline (underlined) at the caret position.
 *
 * Side-channel for typed text: em_x11_set_pending_key_text stuffs UTF-8
 * bytes into a per-display slot; em_x11_push_key_event snapshots them into
 * a parallel queue at the event_queue tail; em_x11_event_queue_pop copies
 * the corresponding slot into dpy->current_key_text; Xutf8LookupString
 * reads from there.
 */

#include "em_x11_internal.h"
#include "em_x11_utf8.h"

#include <emscripten.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -- Opaque XIM / XIC structs --------------------------------------------- */

struct _XIM {
  Display* dpy;
};

struct _XIC {
  XIM im;
  Window client_window;
  Window focus_window;
  XIMStyle input_style;
  /* Spot location (XNSpotLocation): caret position in window-local
   * pixels, set by Tk every time the entry's caret moves. */
  int spot_x;
  int spot_y;
  int have_spot;

  /* Linked list (anchor in dpy->xic_list). */
  struct _XIC* next;

  /* Preedit callbacks registered by the client (Tk). Stored as
   * XICCallback = { XPointer client_data; XICProc callback; }.
   * Signature: Bool callback(XIC, XPointer client_data, XPointer data).
   * data is NULL for start/done; &XIMPreeditDrawCallbackStruct for draw;
   * &XIMPreeditCaretCallbackStruct for caret. */
  XICCallback preedit_start_cb;
  XICCallback preedit_done_cb;
  XICCallback preedit_draw_cb;
  XICCallback preedit_caret_cb;
  int have_preedit_start;
  int have_preedit_done;
  int have_preedit_draw;
  int have_preedit_caret;

  /* True while an OS IME composition session is active. Composition
   * updates between start and done fire the draw callback; KeyPress
   * events on this window are filtered so Tk doesn't double-process. */
  int preedit_active;
};

/* Forward: used by XFilterEvent in event_queue.c. */
static XIC find_xic_for_window(Display* dpy, Window w);

/* -- Bridges to host (text-input.ts) ------------------------------------- */

extern void em_x11_js_xim_set_focus(Window window);
extern void em_x11_js_xim_clear_focus(void);
extern void em_x11_js_xim_set_spot(Window window, int x, int y);

/* Decode a captured nested list from XvaCreateNestedList.
 * Returns 1 + fills count/names/values when the pointer is one of ours,
 * 0 otherwise. */
int em_x11_nested_list_decode(void* list,
                              int* count_out,
                              const char*** names_out,
                              void*** values_out);

/* UTF-8 character count: use shared em_x11_utf8_char_count (em_x11_utf8.h). */

/* Apply XNSpotLocation + preedit callback fields found inside a preedit /
 * status nested list to ic. Tk's XCreateIC always seeds a (0,0) spot
 * so gate the host bridge on `notify` (only fire from XSetICValues, not
 * from XCreateIC's seed). */
static void apply_preedit_attrs(XIC ic, void* list, int notify) {
  int n = 0;
  const char** names = NULL;
  void** values = NULL;
  if (!em_x11_nested_list_decode(list, &n, &names, &values))
    return;
  for (int i = 0; i < n; i++) {
    const char* k = names[i];
    if (!k)
      continue;
    if (strcmp(k, XNSpotLocation) == 0) {
      XPoint* pt = (XPoint*)values[i];
      if (pt) {
        ic->spot_x = pt->x;
        ic->spot_y = pt->y;
        ic->have_spot = 1;
        if (notify) {
          em_x11_js_xim_set_spot(ic->focus_window, pt->x, pt->y);
        }
      }
    } else if (strcmp(k, XNPreeditStartCallback) == 0) {
      XICCallback* cb = (XICCallback*)values[i];
      if (cb) {
        ic->preedit_start_cb = *cb;
        ic->have_preedit_start = 1;
      }
    } else if (strcmp(k, XNPreeditDoneCallback) == 0) {
      XICCallback* cb = (XICCallback*)values[i];
      if (cb) {
        ic->preedit_done_cb = *cb;
        ic->have_preedit_done = 1;
      }
    } else if (strcmp(k, XNPreeditDrawCallback) == 0) {
      XICCallback* cb = (XICCallback*)values[i];
      if (cb) {
        ic->preedit_draw_cb = *cb;
        ic->have_preedit_draw = 1;
      }
    } else if (strcmp(k, XNPreeditCaretCallback) == 0) {
      XICCallback* cb = (XICCallback*)values[i];
      if (cb) {
        ic->preedit_caret_cb = *cb;
        ic->have_preedit_caret = 1;
      }
    }
    /* XNFontSet, XNArea, XNAreaNeeded, XNColormap, XNStdColormap,
     * XNForeground, XNBackground, XNBackgroundPixmap, XNLineSpace,
     * XNCursor: inline preedit is rendered by Tk itself using its
     * own GC/font, so we don't need these. */
  }
}

/* -- XIM / XIC lifecycle ------------------------------------------------- */

XIM XOpenIM(Display* dpy,
            struct _XrmHashBucketRec* db,
            char* res_name,
            char* res_class) {
  (void)db;
  (void)res_name;
  (void)res_class;
  if (!dpy)
    return NULL;
  XIM im = (XIM)calloc(1, sizeof(struct _XIM));
  if (!im)
    return NULL;
  im->dpy = dpy;
  return im;
}

Status XCloseIM(XIM im) {
  if (!im)
    return 0;
  free(im);
  return 1;
}

XIM XIMOfIC(XIC ic) { return ic ? ic->im : NULL; }

/* XCreateIC reads varargs in (name, value, ...) form, terminated by NULL.
 * We honour XNClientWindow, XNFocusWindow, XNInputStyle. Preedit/Status
 * attribute lists (themselves nested name/value lists wrapped via
 * XVaCreateNestedList) are skipped -- Tier A doesn't draw preedit. */
static void ic_apply_va(XIC ic, va_list ap) {
  for (;;) {
    const char* name = va_arg(ap, const char*);
    if (!name)
      break;
    if (strcmp(name, XNClientWindow) == 0) {
      ic->client_window = (Window)va_arg(ap, unsigned long);
    } else if (strcmp(name, XNFocusWindow) == 0) {
      ic->focus_window = (Window)va_arg(ap, unsigned long);
    } else if (strcmp(name, XNInputStyle) == 0) {
      ic->input_style = (XIMStyle)va_arg(ap, unsigned long);
    } else if (strcmp(name, XNPreeditAttributes) == 0 ||
               strcmp(name, XNStatusAttributes) == 0) {
      void* list = va_arg(ap, void*);
      apply_preedit_attrs(ic, list, /*notify=*/0);
    } else if (strcmp(name, XNPreeditStartCallback) == 0) {
      XICCallback* cb = va_arg(ap, XICCallback*);
      if (cb) {
        ic->preedit_start_cb = *cb;
        ic->have_preedit_start = 1;
      }
    } else if (strcmp(name, XNPreeditDoneCallback) == 0) {
      XICCallback* cb = va_arg(ap, XICCallback*);
      if (cb) {
        ic->preedit_done_cb = *cb;
        ic->have_preedit_done = 1;
      }
    } else if (strcmp(name, XNPreeditDrawCallback) == 0) {
      XICCallback* cb = va_arg(ap, XICCallback*);
      if (cb) {
        ic->preedit_draw_cb = *cb;
        ic->have_preedit_draw = 1;
      }
    } else if (strcmp(name, XNPreeditCaretCallback) == 0) {
      XICCallback* cb = va_arg(ap, XICCallback*);
      if (cb) {
        ic->preedit_caret_cb = *cb;
        ic->have_preedit_caret = 1;
      }
    } else {
      /* Unknown attr; consume one value to keep va_list aligned. */
      (void)va_arg(ap, void*);
    }
  }
}

XIC XCreateIC(XIM im, ...) {
  if (!im)
    return NULL;
  XIC ic = (XIC)calloc(1, sizeof(struct _XIC));
  if (!ic)
    return NULL;
  ic->im = im;
  va_list ap;
  va_start(ap, im);
  ic_apply_va(ic, ap);
  va_end(ap);
  /* Insert at head of dpy->xic_list so find_xic_for_window can locate
   * this XIC when the host delivers preedit events. */
  ic->next = im->dpy->xic_list;
  im->dpy->xic_list = ic;
  return ic;
}

void XDestroyIC(XIC ic) {
  if (!ic)
    return;
  /* Unlink from dpy->xic_list. */
  Display* dpy = ic->im ? ic->im->dpy : NULL;
  if (dpy) {
    XIC* p = &dpy->xic_list;
    while (*p) {
      if (*p == ic) {
        *p = ic->next;
        break;
      }
      p = &(*p)->next;
    }
  }
  free(ic);
}

/* Tk only ever calls XSetICFocus when its focus window changes. We
 * mirror that to the host so the hidden textarea grabs DOM focus and
 * the OS IME starts pointing at this widget. */
void XSetICFocus(XIC ic) {
  if (!ic)
    return;
  em_x11_js_xim_set_focus(ic->focus_window);
}

void XUnsetICFocus(XIC ic) {
  (void)ic;
  em_x11_js_xim_clear_focus();
}

/* XSetICValues parses the same name/value form as XCreateIC. The one
 * field we act on at runtime is XNSpotLocation -- a pointer to an
 * XPoint with the caret position in window-local pixels. Tk's
 * tkUnixKey.c calls this on every cursor motion in entries/text. */
char* XSetICValues(XIC ic, ...) {
  if (!ic)
    return NULL;
  va_list ap;
  va_start(ap, ic);
  for (;;) {
    const char* name = va_arg(ap, const char*);
    if (!name)
      break;
    if (strcmp(name, XNFocusWindow) == 0) {
      ic->focus_window = (Window)va_arg(ap, unsigned long);
    } else if (strcmp(name, XNClientWindow) == 0) {
      ic->client_window = (Window)va_arg(ap, unsigned long);
    } else if (strcmp(name, XNInputStyle) == 0) {
      ic->input_style = (XIMStyle)va_arg(ap, unsigned long);
    } else if (strcmp(name, XNSpotLocation) == 0) {
      /* Direct (un-nested) XNSpotLocation -- not how Tk uses it,
       * but legal per spec. Some toolkits set it directly. */
      XPoint* pt = va_arg(ap, XPoint*);
      if (pt) {
        ic->spot_x = pt->x;
        ic->spot_y = pt->y;
        ic->have_spot = 1;
        em_x11_js_xim_set_spot(ic->focus_window, pt->x, pt->y);
      }
    } else if (strcmp(name, XNPreeditAttributes) == 0 ||
               strcmp(name, XNStatusAttributes) == 0) {
      /* Tk's Tk_SetCaretPos always wraps the spot in
       * XNPreeditAttributes -> XVaCreateNestedList(XNSpotLocation,
       * &spot, NULL). Decode and forward. */
      void* list = va_arg(ap, void*);
      apply_preedit_attrs(ic, list, /*notify=*/1);
    } else if (strcmp(name, XNPreeditStartCallback) == 0) {
      XICCallback* cb = va_arg(ap, XICCallback*);
      if (cb) {
        ic->preedit_start_cb = *cb;
        ic->have_preedit_start = 1;
      }
    } else if (strcmp(name, XNPreeditDoneCallback) == 0) {
      XICCallback* cb = va_arg(ap, XICCallback*);
      if (cb) {
        ic->preedit_done_cb = *cb;
        ic->have_preedit_done = 1;
      }
    } else if (strcmp(name, XNPreeditDrawCallback) == 0) {
      XICCallback* cb = va_arg(ap, XICCallback*);
      if (cb) {
        ic->preedit_draw_cb = *cb;
        ic->have_preedit_draw = 1;
      }
    } else if (strcmp(name, XNPreeditCaretCallback) == 0) {
      XICCallback* cb = va_arg(ap, XICCallback*);
      if (cb) {
        ic->preedit_caret_cb = *cb;
        ic->have_preedit_caret = 1;
      }
    } else {
      (void)va_arg(ap, void*);
    }
  }
  va_end(ap);
  return NULL; /* NULL == success */
}

char* XGetICValues(XIC ic, ...) {
  if (!ic)
    return NULL;
  va_list ap;
  va_start(ap, ic);
  for (;;) {
    const char* name = va_arg(ap, const char*);
    if (!name)
      break;
    void* out = va_arg(ap, void*);
    if (!out)
      continue;
    if (strcmp(name, XNFilterEvents) == 0) {
      /* Tell Tk which extra event-mask bits XIM needs selected on
       * the focus window. We only consume KeyPress; report 0 so
       * Tk's mask stays as it would have been without XIM. */
      *(unsigned long*)out = 0;
    }
    /* Other queries return whatever the slot was zero-initialised to;
     * Tk doesn't read XNFocusWindow/XNClientWindow back. */
  }
  va_end(ap);
  return NULL;
}

char* XGetIMValues(XIM im, ...) {
  (void)im;
  /* Advertise styles in Tk's preference order so it picks the best
   * one it supports:
   *   XIMPreeditCallbacks — inline preedit via draw callbacks
   *   XIMPreeditNothing   — no IME, pure-ASCII fallback
   * XIMStatusNothing keeps Tk from asking for a status area widget. */
  va_list ap;
  va_start(ap, im);
  for (;;) {
    const char* name = va_arg(ap, const char*);
    if (!name)
      break;
    void* out = va_arg(ap, void*);
    if (!out)
      continue;
    if (strcmp(name, XNQueryInputStyle) == 0) {
      static XIMStyle style_buf[] = {
        XIMPreeditCallbacks | XIMStatusNothing,
        XIMPreeditNothing | XIMStatusNothing,
      };
      XIMStyles* blob =
        (XIMStyles*)malloc(sizeof(XIMStyles) + sizeof(style_buf));
      if (!blob)
        continue;
      XIMStyle* arr = (XIMStyle*)(blob + 1);
      memcpy(arr, style_buf, sizeof(style_buf));
      blob->count_styles = sizeof(style_buf) / sizeof(style_buf[0]);
      blob->supported_styles = arr;
      *(XIMStyles**)out = blob;
    }
  }
  va_end(ap);
  return NULL;
}

/* -- Side-channel: typed UTF-8 carried alongside KeyPress ---------------- */

EMSCRIPTEN_KEEPALIVE
void em_x11_set_pending_key_text(const char* utf8) {
  Display* dpy = em_x11_get_display();
  if (!dpy)
    return;
  if (!utf8) {
    dpy->pending_key_text[0] = '\0';
    dpy->pending_key_text_len = 0;
    return;
  }
  size_t n = strnlen(utf8, EM_X11_KEY_TEXT_SLOT - 1);
  memcpy(dpy->pending_key_text, utf8, n);
  dpy->pending_key_text[n] = '\0';
  dpy->pending_key_text_len = (int)n;
}

/* Called from event.c::em_x11_push_key_event right after the event is
 * pushed; `slot` is the queue index that was just written. Snapshots the
 * pending-text buffer into the parallel array and clears pending so a
 * subsequent KeyRelease without text doesn't inherit stale bytes. */
void em_x11_xim_capture_key_text(Display* dpy, unsigned int slot) {
  if (!dpy || slot >= EM_X11_EVENT_QUEUE_CAPACITY)
    return;
  int n = dpy->pending_key_text_len;
  if (n < 0)
    n = 0;
  if (n >= EM_X11_KEY_TEXT_SLOT)
    n = EM_X11_KEY_TEXT_SLOT - 1;
  if (n > 0)
    memcpy(dpy->key_text_queue[slot], dpy->pending_key_text, n);
  dpy->key_text_queue[slot][n] = '\0';
  dpy->key_text_len_queue[slot] = n;
  /* Pending is consumed by this push. */
  dpy->pending_key_text[0] = '\0';
  dpy->pending_key_text_len = 0;
}

/* Called from event_queue.c::em_x11_event_queue_pop right BEFORE head
 * advances. Mirrors the popped slot's text into dpy->current_key_text
 * so Xutf8LookupString below reads the bytes that belong to the event
 * the caller is about to handle. */
void em_x11_xim_capture_pop_text(Display* dpy, unsigned int slot) {
  if (!dpy)
    return;
  if (slot >= EM_X11_EVENT_QUEUE_CAPACITY) {
    dpy->current_key_text[0] = '\0';
    dpy->current_key_text_len = 0;
    return;
  }
  int n = dpy->key_text_len_queue[slot];
  if (n < 0)
    n = 0;
  if (n >= EM_X11_KEY_TEXT_SLOT)
    n = EM_X11_KEY_TEXT_SLOT - 1;
  if (n > 0)
    memcpy(dpy->current_key_text, dpy->key_text_queue[slot], n);
  dpy->current_key_text[n] = '\0';
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
int Xutf8LookupString(XIC ic,
                      XKeyPressedEvent* event,
                      char* buffer_return,
                      int bytes_buffer,
                      KeySym* keysym_return,
                      Status* status_return) {
  (void)ic;
  Display* dpy = event ? event->display : em_x11_get_display();
  KeySym sym = NoSymbol;
  if (event)
    sym = XLookupKeysym(event, 0);
  if (keysym_return)
    *keysym_return = sym;

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
    if (written > 0 && sym != NoSymbol)
      *status_return = XLookupBoth;
    else if (written > 0)
      *status_return = XLookupChars;
    else if (sym != NoSymbol)
      *status_return = XLookupKeySym;
    else
      *status_return = XLookupNone;
  }
  return written;
}

/* XmbLookupString is the multibyte variant; clients almost always link
 * XutfLookupString, but Xt/Tk fall back to it when locale isn't UTF-8.
 * Browser side is always UTF-8 so we route through the same machinery. */
int XmbLookupString(XIC ic,
                    XKeyPressedEvent* event,
                    char* buffer_return,
                    int bytes_buffer,
                    KeySym* keysym_return,
                    Status* status_return) {
  return Xutf8LookupString(
    ic, event, buffer_return, bytes_buffer, keysym_return, status_return);
}

/* XwcLookupString writes wchar_t into the buffer. Decode our UTF-8 slot
 * back to codepoints. wchar_t on emscripten is 32-bit. */
int XwcLookupString(XIC ic,
                    XKeyPressedEvent* event,
                    wchar_t* buffer_return,
                    int wchars_buffer,
                    KeySym* keysym_return,
                    Status* status_return) {
  (void)ic;
  Display* dpy = event ? event->display : em_x11_get_display();
  KeySym sym = NoSymbol;
  if (event)
    sym = XLookupKeysym(event, 0);
  if (keysym_return)
    *keysym_return = sym;

  int written = 0;
  if (dpy && dpy->current_key_text_len > 0 && buffer_return &&
      wchars_buffer > 0) {
    const unsigned char* p = (const unsigned char*)dpy->current_key_text;
    int n = dpy->current_key_text_len;
    int i = 0;
    while (i < n && written < wchars_buffer) {
      unsigned int cp = 0;
      unsigned char c = p[i];
      if (c < 0x80) {
        cp = c;
        i += 1;
      } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
        cp = ((c & 0x1F) << 6) | (p[i + 1] & 0x3F);
        i += 2;
      } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
        cp = ((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
        i += 3;
      } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
        cp = ((c & 0x07) << 18) | ((p[i + 1] & 0x3F) << 12) |
             ((p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
        i += 4;
      } else {
        i += 1;
        continue;
      }
      buffer_return[written++] = (wchar_t)cp;
    }
  }

  if (status_return) {
    if (written > 0 && sym != NoSymbol)
      *status_return = XLookupBoth;
    else if (written > 0)
      *status_return = XLookupChars;
    else if (sym != NoSymbol)
      *status_return = XLookupKeySym;
    else
      *status_return = XLookupNone;
  }
  return written;
}

/* -- Preedit: inline composition-text rendering -------------------------- */

/* Find the XIC whose focus_window matches `w`. Returns NULL when no
 * XIC is attached to that window (not a text-input widget). */
static XIC find_xic_for_window(Display* dpy, Window w) {
  if (!dpy || !w)
    return NULL;
  XIC ic = dpy->xic_list;
  while (ic) {
    if (ic->focus_window == w)
      return ic;
    ic = ic->next;
  }
  return NULL;
}

/* Public accessor for event_queue.c::XFilterEvent. */
XIC em_x11_find_xic_for_window(Display* dpy, Window w) {
  return find_xic_for_window(dpy, w);
}

/* Build a single-block XIMText containing `text` (UTF-8, length `text_len`
 * bytes) with XIMUnderline feedback on every character. The block layout:
 *   [XIMText] [XIMFeedback * char_count] [char * (text_len + 1)]
 * Caller frees with a single free(text). */
static XIMText* make_preedit_text(const char* text, int text_len) {
  unsigned short char_count =
    (unsigned short)em_x11_utf8_char_count(text, text_len);
  size_t fb_off = sizeof(XIMText);
  size_t str_off = fb_off + char_count * sizeof(XIMFeedback);
  size_t total = str_off + text_len + 1;

  XIMText* t = (XIMText*)malloc(total);
  if (!t)
    return NULL;
  memset(t, 0, total);

  t->length = char_count;
  t->encoding_is_wchar = False;
  t->feedback = (XIMFeedback*)((char*)t + fb_off);
  t->string.multi_byte = (char*)t + str_off;

  /* Every character gets XIMUnderline so Tk renders it as composing. */
  for (unsigned short i = 0; i < char_count; i++)
    t->feedback[i] = XIMUnderline;

  memcpy(t->string.multi_byte, text, text_len);
  t->string.multi_byte[text_len] = '\0';
  return t;
}

/* Host calls this on compositionstart (user begins an IME composition).
 * Fires the preedit_start callback so Tk prepares for inline rendering. */
EMSCRIPTEN_KEEPALIVE
void em_x11_xim_preedit_start(Window window) {
  Display* dpy = em_x11_get_display();
  if (!dpy)
    return;
  XIC ic = find_xic_for_window(dpy, window);
  if (!ic)
    return;
  ic->preedit_active = 1;
  if (ic->have_preedit_start && ic->preedit_start_cb.callback) {
    ic->preedit_start_cb.callback(
      (XIC)ic, ic->preedit_start_cb.client_data, (XPointer)NULL);
  }
}

/* Host calls this on compositionupdate. Builds an XIMPreeditDrawCallbackStruct
 * and fires the draw callback. Tk renders the composing text inline at the
 * caret position. Uses XIMUnderline feedback so Tk styles it as composing. */
EMSCRIPTEN_KEEPALIVE
void em_x11_xim_preedit_draw(
  Window window, const char* text, int caret, int chg_first, int chg_length) {
  Display* dpy = em_x11_get_display();
  if (!dpy)
    return;
  XIC ic = find_xic_for_window(dpy, window);
  if (!ic)
    return;
  if (!ic->have_preedit_draw || !ic->preedit_draw_cb.callback)
    return;

  XIMPreeditDrawCallbackStruct cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.caret = caret;
  cbs.chg_first = chg_first;
  cbs.chg_length = chg_length;

  if (text && text[0]) {
    int len = (int)strlen(text);
    cbs.text = make_preedit_text(text, len);
  } else {
    cbs.text = NULL; /* empty/clear preedit */
  }

  ic->preedit_draw_cb.callback(
    (XIC)ic, ic->preedit_draw_cb.client_data, (XPointer)&cbs);

  /* XIMText was a single malloc block. */
  if (cbs.text)
    free(cbs.text);
}

/* Host calls this on compositionend. Clears residual preedit text (draw
 * callback with text=NULL), fires the done callback, and marks preedit
 * inactive so XFilterEvent resumes passing keys through. */
EMSCRIPTEN_KEEPALIVE
void em_x11_xim_preedit_done(Window window) {
  Display* dpy = em_x11_get_display();
  if (!dpy)
    return;
  XIC ic = find_xic_for_window(dpy, window);
  if (!ic)
    return;
  if (!ic->preedit_active)
    return;

  /* Clear any remaining preedit text before marking done. */
  if (ic->have_preedit_draw && ic->preedit_draw_cb.callback) {
    XIMPreeditDrawCallbackStruct cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.text = NULL;
    ic->preedit_draw_cb.callback(
      (XIC)ic, ic->preedit_draw_cb.client_data, (XPointer)&cbs);
  }

  ic->preedit_active = 0;

  if (ic->have_preedit_done && ic->preedit_done_cb.callback) {
    ic->preedit_done_cb.callback(
      (XIC)ic, ic->preedit_done_cb.client_data, (XPointer)NULL);
  }
}

/* XFilterEvent: returns True for KeyPress during active preedit on the
 * focused window, telling Tk to skip normal key processing (preedit
 * callbacks handle the composing text separately). The stub body in
 * event_queue.c delegates here. */
Bool em_x11_xim_filter_event(XEvent* event, Window w) {
  if (!event || !event->xany.display)
    return False;
  XIC ic = find_xic_for_window(event->xany.display, w);
  if (ic && ic->preedit_active && event->type == KeyPress)
    return True;
  return False;
}

/* -- XIM registration callbacks. Tk uses them to register a "tell me when
 * an XIM appears" watcher; XOpenIM already succeeds on the first call so
 * the callback never has to fire. */

Bool XRegisterIMInstantiateCallback(Display* dpy,
                                    struct _XrmHashBucketRec* rdb,
                                    char* res_name,
                                    char* res_class,
                                    XIDProc callback,
                                    XPointer client_data) {
  (void)dpy;
  (void)rdb;
  (void)res_name;
  (void)res_class;
  (void)callback;
  (void)client_data;
  return False;
}

Bool XUnregisterIMInstantiateCallback(Display* dpy,
                                      struct _XrmHashBucketRec* rdb,
                                      char* res_name,
                                      char* res_class,
                                      XIDProc callback,
                                      XPointer client_data) {
  (void)dpy;
  (void)rdb;
  (void)res_name;
  (void)res_class;
  (void)callback;
  (void)client_data;
  return False;
}

char* XSetIMValues(XIM im, ...) {
  (void)im;
  return NULL;
}

/* -- XDisplayOfIM -- */

Display* XDisplayOfIM(XIM im) {
  (void)im;
  return NULL;
}

/* -- XVaCreateNestedList + decoder -- */

static const char EM_X11_NESTED_LIST_MAGIC[] = "em-x11-nested-list";

XVaNestedList XVaCreateNestedList(int unused_dummy, ...) {
  (void)unused_dummy;
  va_list ap;
  int n = 0;
  va_start(ap, unused_dummy);
  for (;;) {
    const char* name = va_arg(ap, const char*);
    if (!name)
      break;
    (void)va_arg(ap, void*);
    n++;
  }
  va_end(ap);

  void** buf = (void**)calloc(2 + 2 * n + 1, sizeof(void*));
  if (!buf)
    return NULL;
  buf[0] = (void*)EM_X11_NESTED_LIST_MAGIC;
  buf[1] = (void*)(uintptr_t)n;
  int o = 2;
  va_start(ap, unused_dummy);
  for (int i = 0; i < n; i++) {
    const char* name = va_arg(ap, const char*);
    void* value = va_arg(ap, void*);
    buf[o++] = (void*)name;
    buf[o++] = value;
  }
  va_end(ap);
  buf[o] = NULL;
  return (XVaNestedList)buf;
}

int em_x11_nested_list_decode(void* list,
                              int* count_out,
                              const char*** names_out,
                              void*** values_out) {
  if (!list)
    return 0;
  void** slots = (void**)list;
  if (slots[0] != (void*)EM_X11_NESTED_LIST_MAGIC)
    return 0;
  int n = (int)(uintptr_t)slots[1];
  static const char* name_buf[16];
  static void* val_buf[16];
  if (n > 16)
    n = 16;
  for (int i = 0; i < n; i++) {
    name_buf[i] = (const char*)slots[2 + 2 * i + 0];
    val_buf[i] = slots[2 + 2 * i + 1];
  }
  if (count_out)
    *count_out = n;
  if (names_out)
    *names_out = name_buf;
  if (values_out)
    *values_out = val_buf;
  return 1;
}
