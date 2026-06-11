/*
 * XKB extension -- minimal slice.
 *
 * Tk 8.6 (when configured with --enable-xkb / HAVE_XKBKEYCODETOKEYSYM)
 * calls into XKB through exactly two entry points:
 *
 *   XkbOpenDisplay     -- wraps XOpenDisplay + extension check. If this
 *                         returns NULL Tk falls back to plain XOpenDisplay
 *                         and skips the XKB branch entirely. We make it
 *                         always succeed so Tk takes the XKB branch.
 *
 *   XkbKeycodeToKeysym -- like XKeycodeToKeysym but with (group, level).
 *                         Our keymap is browser-driven (runtime/keymap.ts
 *                         resolves the platform keyboard layout before
 *                         events ever reach C) so group/level are already
 *                         baked into the keysym sitting in
 *                         dpy->keysym_table. Delegate to the existing
 *                         lookup and ignore the indices.
 *
 * The other entry points (XkbQueryExtension, XkbUseExtension, etc.) are
 * here for Xt/Xaw/future Motif feature-detection; they all advertise
 * XKB 1.0 available.
 *
 * Reference: libX11-1.8.13/src/xkb/. The real client-side library is
 * thousands of lines of state-machine code reconstructing the server's
 * keymap; none of that is meaningful when the keymap originates in the
 * browser. We replace it with a stateless adapter.
 */

#include "em_x11_internal.h"
#include <X11/XKBlib.h>
#include <stdlib.h>
#include <string.h>

/* Defined in event_keysym.c -- the same table backs XKeycodeToKeysym. */
extern KeySym XKeycodeToKeysym(Display* dpy, unsigned int keycode, int index);

Display* XkbOpenDisplay(const char* name,
                        int* ev_rtrn,
                        int* err_rtrn,
                        int* major_rtrn,
                        int* minor_rtrn,
                        int* reason) {
  Display* dpy = XOpenDisplay(name);
  if (!dpy) {
    if (reason)
      *reason = XkbOD_ConnectionRefused;
    return NULL;
  }
  /* Negotiate version: real Xlib treats major/minor as in/out and
   * returns BadServerVersion if the server is older than the client
   * requests. Browser keymap has no version skew -- always succeed
   * at 1.0 (XkbMajorVersion / XkbMinorVersion). */
  if (major_rtrn)
    *major_rtrn = XkbMajorVersion;
  if (minor_rtrn)
    *minor_rtrn = XkbMinorVersion;
  /* We don't allocate XKB event / error codes; report 0 so callers
   * that compare event->type against (ev_rtrn + XkbAnyEvent) simply
   * never match -- there are no XKB events to emit. */
  if (ev_rtrn)
    *ev_rtrn = 0;
  if (err_rtrn)
    *err_rtrn = 0;
  if (reason)
    *reason = XkbOD_Success;
  return dpy;
}

Bool XkbQueryExtension(Display* dpy,
                       int* opcodeReturn,
                       int* eventBaseReturn,
                       int* errorBaseReturn,
                       int* majorRtrn,
                       int* minorRtrn) {
  (void)dpy;
  if (opcodeReturn)
    *opcodeReturn = 0;
  if (eventBaseReturn)
    *eventBaseReturn = 0;
  if (errorBaseReturn)
    *errorBaseReturn = 0;
  if (majorRtrn)
    *majorRtrn = XkbMajorVersion;
  if (minorRtrn)
    *minorRtrn = XkbMinorVersion;
  return True;
}

Bool XkbUseExtension(Display* dpy, int* major_rtrn, int* minor_rtrn) {
  (void)dpy;
  if (major_rtrn)
    *major_rtrn = XkbMajorVersion;
  if (minor_rtrn)
    *minor_rtrn = XkbMinorVersion;
  return True;
}

Bool XkbLibraryVersion(int* libMajorRtrn, int* libMinorRtrn) {
  if (libMajorRtrn)
    *libMajorRtrn = XkbMajorVersion;
  if (libMinorRtrn)
    *libMinorRtrn = XkbMinorVersion;
  return True;
}

KeySym XkbKeycodeToKeysym(Display* dpy, unsigned int kc, int group, int level) {
  (void)group;
  (void)level;
  /* See header comment: the browser-side keymap pre-resolves group /
   * level into the keysym that lands in dpy->keysym_table. Group 0
   * level 0 is the only meaningful index.
   *
   * Suppress the upstream deprecation attribute on XKeycodeToKeysym
   * here -- we're explicitly delegating one legacy keysym lookup to
   * another, and the keysym_table-backed implementation is the right
   * destination regardless of upstream's policy. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  return XKeycodeToKeysym(dpy, kc, 0);
#pragma GCC diagnostic pop
}

Bool XkbLookupKeySym(Display* dpy,
                     KeyCode keycode,
                     unsigned int modifiers,
                     unsigned int* modifiers_return,
                     KeySym* keysym_return) {
  if (!dpy || !keysym_return)
    return False;
  /* Level 1 (shifted) if ShiftMask is set in modifiers; else level 0. */
  int level = (modifiers & ShiftMask) ? 1 : 0;
  *keysym_return = XkbKeycodeToKeysym(dpy, (unsigned int)keycode, 0, level);
  if (modifiers_return)
    *modifiers_return = modifiers;
  return True;
}

Bool XkbSetDetectableAutoRepeat(Display* dpy,
                                Bool detectable,
                                Bool* supported) {
  (void)dpy;
  (void)detectable;
  /* em-x11 already emits autorepeat as a stream of KeyPress events
   * with no intervening KeyRelease (browser keydown repeat semantics),
   * which IS detectable-autorepeat behaviour. Report it as supported
   * regardless of the requested value. */
  if (supported)
    *supported = True;
  return True;
}

/* ------------------------------------------------------------------------- *
 *  Keymap retrieval: XkbAllocKeyboard / XkbGetMap / XkbFreeKeyboard.        *
 *                                                                           *
 *  Strategy: build a self-consistent XkbDescRec from dpy->keysym_table.    *
 *  Every keycode that holds a non-NoSymbol keysym gets a 2-level group     *
 *  (base + shift) entry; the shift level is synthesised from US-shift     *
 *  rules applied to the base keysym (the browser's getLayoutMap() only    *
 *  gives base-level data, so this is best-effort for non-US layouts --    *
 *  characters that don't have a US-shift rule simply repeat the base).   *
 *                                                                           *
 *  We always populate one "key type" (alphabetic / 2-level / shifts on    *
 *  Mod_Shift) that covers every key, instead of the real X server's       *
 *  fully decomposed set of types per group width / level combination.    *
 *  Real X clients that only read syms[] are unaffected; the rare client   *
 *  that walks types[] sees one entry and infers width=2, levels=2.       *
 * ------------------------------------------------------------------------- */

/* US QWERTY shift mapping for a base keysym -- letters uppercase via
 * ASCII, digits/punctuation via a static table, everything else (function
 * keys, modifiers, navigation) gets the same value at level 1. */
static KeySym shift_keysym(KeySym base) {
  if (base >= 'a' && base <= 'z')
    return base - ('a' - 'A');
  switch (base) {
    case '1':
      return '!';
    case '2':
      return '@';
    case '3':
      return '#';
    case '4':
      return '$';
    case '5':
      return '%';
    case '6':
      return '^';
    case '7':
      return '&';
    case '8':
      return '*';
    case '9':
      return '(';
    case '0':
      return ')';
    case '-':
      return '_';
    case '=':
      return '+';
    case '[':
      return '{';
    case ']':
      return '}';
    case '\\':
      return '|';
    case ';':
      return ':';
    case '\'':
      return '"';
    case ',':
      return '<';
    case '.':
      return '>';
    case '/':
      return '?';
    case '`':
      return '~';
    default:
      return base;
  }
}

/* The single key type we expose: 2 levels (base, shift), shift modifier
 * is ShiftMask. Matches what XKB ships as TWO_LEVEL / ALPHABETIC. */
static void init_key_type(XkbKeyTypePtr kt) {
  memset(kt, 0, sizeof(*kt));
  kt->mods.mask = ShiftMask;
  kt->mods.real_mods = ShiftMask;
  kt->mods.vmods = 0;
  kt->num_levels = 2;
  kt->map_count = 1;
  kt->map = (XkbKTMapEntryPtr)calloc(1, sizeof(XkbKTMapEntryRec));
  if (kt->map) {
    kt->map[0].active = True;
    kt->map[0].level = 1;
    kt->map[0].mods.mask = ShiftMask;
    kt->map[0].mods.real_mods = ShiftMask;
  }
  kt->preserve = NULL;
  kt->name = 0;
  kt->level_names = NULL;
}

XkbDescPtr XkbAllocKeyboard(void) {
  XkbDescPtr xkb = (XkbDescPtr)calloc(1, sizeof(XkbDescRec));
  if (!xkb)
    return NULL;
  xkb->device_spec = XkbUseCoreKbd;
  xkb->min_key_code = 8;
  xkb->max_key_code = 255;
  return xkb;
}

/* Allocate the .map (XkbClientMapRec) and its sub-arrays. Real Xlib
 * accepts a nTypes hint; we always allocate a single type, so the hint
 * is ignored. `which` is the same bitmask XkbGetMap uses (XkbKeySymsMask
 * etc.); we always fill the lot. */
Status
XkbAllocClientMap(XkbDescPtr xkb, unsigned int which, unsigned int nTypes) {
  (void)which;
  (void)nTypes;
  if (!xkb)
    return BadValue;
  if (xkb->map)
    return Success; /* already allocated */
  XkbClientMapPtr m = (XkbClientMapPtr)calloc(1, sizeof(XkbClientMapRec));
  if (!m)
    return BadAlloc;
  int kcCount = xkb->max_key_code + 1; /* indexed 0..max */
  m->key_sym_map = (XkbSymMapPtr)calloc((size_t)kcCount, sizeof(XkbSymMapRec));
  m->modmap = (unsigned char*)calloc((size_t)kcCount, sizeof(unsigned char));
  m->size_types = 1;
  m->num_types = 1;
  m->types = (XkbKeyTypePtr)calloc(1, sizeof(XkbKeyTypeRec));
  if (!m->key_sym_map || !m->modmap || !m->types) {
    free(m->key_sym_map);
    free(m->modmap);
    free(m->types);
    free(m);
    return BadAlloc;
  }
  init_key_type(&m->types[0]);
  xkb->map = m;
  return Success;
}

void XkbFreeClientMap(XkbDescPtr xkb, unsigned int what, Bool freeMap) {
  (void)what;
  if (!xkb || !xkb->map)
    return;
  XkbClientMapPtr m = xkb->map;
  if (m->syms) {
    free(m->syms);
    m->syms = NULL;
  }
  if (m->key_sym_map) {
    free(m->key_sym_map);
    m->key_sym_map = NULL;
  }
  if (m->modmap) {
    free(m->modmap);
    m->modmap = NULL;
  }
  if (m->types) {
    for (int i = 0; i < m->num_types; i++) {
      if (m->types[i].map)
        free(m->types[i].map);
      if (m->types[i].preserve)
        free(m->types[i].preserve);
      if (m->types[i].level_names)
        free(m->types[i].level_names);
    }
    free(m->types);
    m->types = NULL;
  }
  if (freeMap) {
    free(m);
    xkb->map = NULL;
  } else {
    memset(m, 0, sizeof(*m));
  }
}

void XkbFreeKeyboard(XkbDescPtr xkb, unsigned int which, Bool freeDesc) {
  (void)which;
  if (!xkb)
    return;
  XkbFreeClientMap(xkb, 0, True);
  if (freeDesc)
    free(xkb);
}

/* Walk keysym_table, count keys that need entries, then allocate the
 * syms flat array sized for 2 levels per active key. Each active
 * keycode kc gets:
 *   m->syms[offset + 0] = base keysym (keysym_table[kc])
 *   m->syms[offset + 1] = shift_keysym(base)
 *   m->key_sym_map[kc] = { group_info=1, width=2, offset, kt_index=0 }
 */
static Status fill_client_map(Display* dpy, XkbDescPtr xkb) {
  if (!xkb->map && XkbAllocClientMap(xkb, XkbAllClientInfoMask, 1) != Success) {
    return BadAlloc;
  }
  XkbClientMapPtr m = xkb->map;

  /* First pass: count populated keycodes. */
  int active = 0;
  for (int kc = xkb->min_key_code; kc <= xkb->max_key_code; kc++) {
    if (dpy->keysym_table[kc] != NoSymbol)
      active++;
  }
  int total_syms = active * 2; /* 2 levels per active key */
  if (m->syms)
    free(m->syms);
  m->syms =
    (KeySym*)calloc((size_t)(total_syms > 0 ? total_syms : 1), sizeof(KeySym));
  if (!m->syms)
    return BadAlloc;
  m->size_syms = (unsigned short)total_syms;
  m->num_syms = (unsigned short)total_syms;

  /* Second pass: fill syms + per-key metadata. offset 0 is reserved
   * as a "no sym" sink that real Xlib uses for unmapped keycodes;
   * we start at 0 too since our empty keycodes have width=0 and
   * never read past the offset anyway. */
  unsigned short off = 0;
  for (int kc = xkb->min_key_code; kc <= xkb->max_key_code; kc++) {
    XkbSymMapPtr sm = &m->key_sym_map[kc];
    KeySym base = dpy->keysym_table[kc];
    if (base == NoSymbol) {
      sm->group_info = 0;
      sm->width = 0;
      sm->offset = 0;
      continue;
    }
    sm->group_info = 1; /* one group */
    sm->width = 2;      /* base + shift */
    sm->offset = off;
    sm->kt_index[0] = 0; /* one type only */
    m->syms[off + 0] = base;
    m->syms[off + 1] = shift_keysym(base);
    off += 2;
  }
  return Success;
}

XkbDescPtr
XkbGetMap(Display* dpy, unsigned int which, unsigned int deviceSpec) {
  (void)which;
  (void)deviceSpec;
  if (!dpy)
    return NULL;
  XkbDescPtr xkb = XkbAllocKeyboard();
  if (!xkb)
    return NULL;
  xkb->dpy = dpy;
  if (XkbAllocClientMap(xkb, XkbAllClientInfoMask, 1) != Success ||
      fill_client_map(dpy, xkb) != Success) {
    XkbFreeKeyboard(xkb, 0, True);
    return NULL;
  }
  return xkb;
}

Status XkbGetUpdatedMap(Display* dpy, unsigned int which, XkbDescPtr desc) {
  (void)which;
  if (!dpy || !desc)
    return BadValue;
  return fill_client_map(dpy, desc);
}

XkbDescPtr
XkbGetKeyboard(Display* dpy, unsigned int which, unsigned int deviceSpec) {
  /* Real Xlib also wires in indicators / compat / names / geom for the
   * caller. We only do the map slice; downstream consumers that touch
   * the other pointers see NULL (which their code typically guards
   * with `if (xkb->compat)`). */
  return XkbGetMap(dpy, which, deviceSpec);
}

XkbDescPtr XkbGetKeyboardByName(Display* dpy,
                                unsigned int deviceSpec,
                                void* names,
                                unsigned int want,
                                unsigned int need,
                                Bool load) {
  (void)names;
  (void)want;
  (void)need;
  (void)load;
  return XkbGetMap(dpy, XkbAllClientInfoMask, deviceSpec);
}
