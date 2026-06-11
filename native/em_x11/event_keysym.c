/*
 * Keysym <-> keycode mapping plus XLookupString text translation.
 *
 * Keysym table is INDEXED BY EVDEV PHYSICAL KEYCODE (matching
 * src/runtime/keymap.ts KEYCODE_EVDEV). At Display init we install
 * the US QWERTY layout into keysym_table; the host then calls
 * em_x11_install_keysym for each entry returned by
 * navigator.keyboard.getLayoutMap() to patch in the user's actual
 * keyboard layout, before any KeyPress is dispatched.
 *
 * The browser already shifts/composes keys before they reach us, so
 * XLookupString is mostly a passthrough for ASCII / Latin-1 plus a
 * few terminal-control specials that Xt translation tables expect.
 */

#include "em_x11_internal.h"

#include <X11/Xutil.h>
#include <emscripten.h>
#include <string.h>

/* US QWERTY keysym for each evdev keycode 8..255. Empty slots are
 * NoSymbol; filled slots match the kernel/X.org evdev keymap (e.g.
 * keycode 38 = 'a' = KeyA's base level on a US ANSI layout). When
 * the host patches in a user's layout, only base-level keysyms get
 * overwritten -- modifiers / function keys / arrows / numpad keys
 * stay at the values below because navigator.keyboard.getLayoutMap()
 * doesn't report on them. */
static const KeySym em_x11_us_qwerty[256] = {
  /* 0..7 reserved by X11 */
  [9] = 0xff1b, /* XK_Escape          */
  [10] = '1',
  [11] = '2',
  [12] = '3',
  [13] = '4',
  [14] = '5',
  [15] = '6',
  [16] = '7',
  [17] = '8',
  [18] = '9',
  [19] = '0',
  [20] = '-',
  [21] = '=',
  [22] = 0xff08, /* XK_BackSpace       */
  [23] = 0xff09, /* XK_Tab             */
  [24] = 'q',
  [25] = 'w',
  [26] = 'e',
  [27] = 'r',
  [28] = 't',
  [29] = 'y',
  [30] = 'u',
  [31] = 'i',
  [32] = 'o',
  [33] = 'p',
  [34] = '[',
  [35] = ']',
  [36] = 0xff0d, /* XK_Return          */
  [37] = 0xffe3, /* XK_Control_L       */
  [38] = 'a',
  [39] = 's',
  [40] = 'd',
  [41] = 'f',
  [42] = 'g',
  [43] = 'h',
  [44] = 'j',
  [45] = 'k',
  [46] = 'l',
  [47] = ';',
  [48] = '\'',
  [49] = '`',
  [50] = 0xffe1, /* XK_Shift_L         */
  [51] = '\\',
  [52] = 'z',
  [53] = 'x',
  [54] = 'c',
  [55] = 'v',
  [56] = 'b',
  [57] = 'n',
  [58] = 'm',
  [59] = ',',
  [60] = '.',
  [61] = '/',
  [62] = 0xffe2, /* XK_Shift_R         */
  [63] = 0xffaa, /* XK_KP_Multiply     */
  [64] = 0xffe9, /* XK_Alt_L           */
  [65] = ' ',    /* space               */
  [66] = 0xffe5, /* XK_Caps_Lock       */
  [67] = 0xffbe,
  [68] = 0xffbf,
  [69] = 0xffc0,
  [70] = 0xffc1,
  [71] = 0xffc2,
  [72] = 0xffc3,
  [73] = 0xffc4,
  [74] = 0xffc5,
  [75] = 0xffc6,
  [76] = 0xffc7, /* F1..F10           */
  [77] = 0xff7f, /* XK_Num_Lock        */
  [78] = 0xff14, /* XK_Scroll_Lock     */
  [79] = 0xffb7,
  [80] = 0xffb8,
  [81] = 0xffb9, /* KP 7 8 9 */
  [82] = 0xffad, /* XK_KP_Subtract     */
  [83] = 0xffb4,
  [84] = 0xffb5,
  [85] = 0xffb6, /* KP 4 5 6 */
  [86] = 0xffab, /* XK_KP_Add          */
  [87] = 0xffb1,
  [88] = 0xffb2,
  [89] = 0xffb3, /* KP 1 2 3 */
  [90] = 0xffb0, /* XK_KP_0            */
  [91] = 0xffae, /* XK_KP_Decimal      */
  [94] = '<',    /* ISO 105-key extra (less than on most layouts) */
  [95] = 0xffc8,
  [96] = 0xffc9,      /* F11 F12          */
  [97] = 0x5c,        /* Japanese yen / backslash  */
  [100] = 0xff21,     /* XK_Henkan_Mode (Convert)   */
  [101] = 0xff2d,     /* XK_Hiragana_Katakana       */
  [102] = 0xff22,     /* XK_Muhenkan (NonConvert)   */
  [104] = 0xff8d,     /* XK_KP_Enter         */
  [105] = 0xffe4,     /* XK_Control_R        */
  [106] = 0xffaf,     /* XK_KP_Divide        */
  [107] = 0xff15,     /* XK_Sys_Req / Print  */
  [108] = 0xffea,     /* XK_Alt_R / AltGraph */
  [110] = 0xff50,     /* XK_Home             */
  [111] = 0xff52,     /* XK_Up               */
  [112] = 0xff55,     /* XK_Page_Up          */
  [113] = 0xff51,     /* XK_Left             */
  [114] = 0xff53,     /* XK_Right            */
  [115] = 0xff57,     /* XK_End              */
  [116] = 0xff54,     /* XK_Down             */
  [117] = 0xff56,     /* XK_Page_Down        */
  [118] = 0xff63,     /* XK_Insert           */
  [119] = 0xffff,     /* XK_Delete           */
  [121] = 0x1008ff12, /* XF86XK_AudioMute     */
  [122] = 0x1008ff11, /* XF86XK_AudioLowerVolume */
  [123] = 0x1008ff13, /* XF86XK_AudioRaiseVolume */
  [124] = 0x1008ff2a, /* XF86XK_PowerOff      */
  [125] = 0xffbd,     /* XK_KP_Equal         */
  [127] = 0xff13,     /* XK_Pause            */
  [130] = 0xff31,     /* XK_Hangul (Lang1)   */
  [131] = 0xff34,     /* XK_Hangul_Hanja (Lang2) */
  [132] = 0xff2e,     /* XK_Katakana (Lang3) */
  [133] = 0xffeb,     /* XK_Super_L (Meta)   */
  [134] = 0xffec,     /* XK_Super_R          */
  [135] = 0xff67,     /* XK_Menu             */
  /* Media keys (XF86) -- frequently sent by laptop keyboards. */
  [171] = 0x1008ff17, /* XF86XK_AudioNext   */
  [172] = 0x1008ff14, /* XF86XK_AudioPlay   */
  [173] = 0x1008ff16, /* XF86XK_AudioPrev   */
  [174] = 0x1008ff15, /* XF86XK_AudioStop   */
};

void em_x11_keysym_table_install_us_qwerty(Display* dpy) {
  if (!dpy)
    return;
  memcpy(dpy->keysym_table, em_x11_us_qwerty, sizeof(em_x11_us_qwerty));
  dpy->next_keycode = 8; /* lazy alloc starts here for unmapped syms */
}

/* Host calls this once per entry in navigator.keyboard.getLayoutMap()
 * during Display setup, replacing the US QWERTY default with the
 * user's layout-resolved base-level keysym at the given evdev keycode.
 * Safe to call any time -- KeyPress events also update the slot via
 * em_x11_push_key_event_kc so XkbGetMap consumers always see the most
 * recent known keysym for each physical key. */
EMSCRIPTEN_KEEPALIVE
void em_x11_install_keysym(unsigned int keycode, unsigned int keysym) {
  Display* dpy = em_x11_get_display();
  if (!dpy || keycode == 0 || keycode >= 256)
    return;
  dpy->keysym_table[keycode] = (KeySym)keysym;
}

KeyCode em_x11_keysym_to_keycode(Display* dpy, KeySym keysym) {
  if (keysym == NoSymbol)
    return 0;
  /* Forward lookup over the WHOLE keysym_table -- pre-fill at evdev
   * positions means slots can be non-zero before we ever lazy-allocate,
   * so scan the entire 8..255 range, not just up to next_keycode. */
  for (unsigned int i = 8; i < 256; i++) {
    if (dpy->keysym_table[i] == keysym) {
      return (KeyCode)i;
    }
  }
  /* Synthetic slot for keysyms outside the evdev keymap (e.g.
   * Unicode-area keysyms produced by IME composition or paste, where
   * no physical key applies). Walk forward from next_keycode to find
   * an empty slot; bump next_keycode only when we install. */
  for (unsigned int i = dpy->next_keycode; i < 256; i++) {
    if (dpy->keysym_table[i] == NoSymbol) {
      dpy->keysym_table[i] = keysym;
      if (i >= dpy->next_keycode)
        dpy->next_keycode = i + 1;
      return (KeyCode)i;
    }
  }
  return 0; /* table exhausted */
}

/* Internal lookup used by both public entry points to avoid a
 * deprecated-self-call chain on XKeycodeToKeysym. */
static KeySym em_x11_keysym_for(Display* dpy, unsigned int keycode) {
  if (keycode >= 256)
    return NoSymbol;
  return dpy->keysym_table[keycode];
}

/* The NeedWidePrototypes convention in Xfuncproto.h (default 1) widens
 * KeyCode to unsigned int across the function-call boundary. Match that
 * here so our definition agrees with the upstream declaration. */
KeySym XKeycodeToKeysym(Display* dpy, unsigned int keycode, int index) {
  (void)index; /* no modifier grid yet */
  return em_x11_keysym_for(dpy, keycode);
}

KeySym XLookupKeysym(XKeyEvent* event, int index) {
  (void)index;
  if (!event || !event->display)
    return NoSymbol;
  return em_x11_keysym_for(event->display, event->keycode);
}

KeyCode XKeysymToKeycode(Display* dpy, KeySym keysym) {
  return em_x11_keysym_to_keycode(dpy, keysym);
}

/* Translate a key event into a UTF-8 byte sequence for XmbLookupString /
 * text entry paths. em-x11's keymap hands us already-shifted keysyms
 * (the browser does the shift/ctrl translation in KeyboardEvent.key),
 * so printable keys map to their keysym value directly. Special keys
 * and modifiers produce a keysym-only result with buffer_return empty. */
int XLookupString(XKeyEvent* event,
                  char* buffer_return,
                  int bytes_buffer,
                  KeySym* keysym_return,
                  XComposeStatus* status_return) {
  (void)status_return;
  if (!event)
    return 0;
  KeySym ks = XLookupKeysym(event, 0);
  if (keysym_return)
    *keysym_return = ks;

  if (!buffer_return || bytes_buffer <= 0)
    return 0;

  /* ASCII range: emit the codepoint as a single byte. */
  if (ks >= 0x20 && ks <= 0x7e) {
    buffer_return[0] = (char)ks;
    if (bytes_buffer > 1)
      buffer_return[1] = '\0';
    return 1;
  }
  /* Latin-1 supplement: 0xA0..0xFF produces a single 0x80-0xFF byte
   * under the X11 STRING convention (iso8859-1). */
  if (ks >= 0xa0 && ks <= 0xff) {
    buffer_return[0] = (char)ks;
    if (bytes_buffer > 1)
      buffer_return[1] = '\0';
    return 1;
  }
  /* Return key, Tab, Backspace, Escape are traditional single-char
   * translations too -- many Xt translation tables expect them. */
  switch (ks) {
    case 0xff0d:
      if (bytes_buffer > 0)
        buffer_return[0] = '\r';
      return 1;
    case 0xff09:
      if (bytes_buffer > 0)
        buffer_return[0] = '\t';
      return 1;
    case 0xff08:
      if (bytes_buffer > 0)
        buffer_return[0] = '\b';
      return 1;
    case 0xff1b:
      if (bytes_buffer > 0)
        buffer_return[0] = 0x1b;
      return 1;
    case 0xffff:
      if (bytes_buffer > 0)
        buffer_return[0] = 0x7f;
      return 1;
    default:
      break;
  }
  return 0;
}

/* -- Keyboard mapping -- */

KeySym* XGetKeyboardMapping(Display* dpy,
                            unsigned int first,
                            int count,
                            int* keysyms_per_keycode_return) {
  if (keysyms_per_keycode_return)
    *keysyms_per_keycode_return = 1;
  if (count <= 0)
    return NULL;
  KeySym* out = calloc((size_t)count, sizeof(KeySym));
  if (!out)
    return NULL;
  for (int i = 0; i < count; i++) {
    unsigned int kc = (unsigned int)first + (unsigned int)i;
    out[i] = (kc < 256) ? dpy->keysym_table[kc] : NoSymbol;
  }
  return out;
}

XModifierKeymap* XGetModifierMapping(Display* dpy) {
  (void)dpy;
  XModifierKeymap* m = calloc(1, sizeof(*m));
  if (!m)
    return NULL;
  m->max_keypermod = 2;
  m->modifiermap = calloc(8 * m->max_keypermod, sizeof(KeyCode));
  return m;
}

int XFreeModifiermap(XModifierKeymap* modmap) {
  if (!modmap)
    return 0;
  free(modmap->modifiermap);
  free(modmap);
  return 1;
}

int XRefreshKeyboardMapping(XMappingEvent* event) {
  (void)event;
  return 0;
}

/* -- Keysym <-> name -- */

#include "keysym_table.h"

char* XKeysymToString(KeySym keysym) {
  for (const struct KeysymEntry* e = g_keysym_table; e->name; e++) {
    if (e->keysym == keysym)
      return (char*)e->name;
  }
  return NULL;
}

KeySym XStringToKeysym(_Xconst char* s) {
  if (!s)
    return NoSymbol;
  if (s[0] && !s[1])
    return (KeySym)(unsigned char)s[0];
  for (const struct KeysymEntry* e = g_keysym_table; e->name; e++) {
    if (strcmp(e->name, s) == 0)
      return e->keysym;
  }
  return NoSymbol;
}

void XConvertCase(KeySym keysym, KeySym* lower_return, KeySym* upper_return) {
  KeySym lo = keysym, hi = keysym;
  if (keysym >= 'A' && keysym <= 'Z')
    lo = keysym + ('a' - 'A');
  if (keysym >= 'a' && keysym <= 'z')
    hi = keysym - ('a' - 'A');
  if (lower_return)
    *lower_return = lo;
  if (upper_return)
    *upper_return = hi;
}
