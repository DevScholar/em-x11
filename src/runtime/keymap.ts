/**
 * Browser KeyboardEvent -> X11 (keycode, keysym) translation.
 *
 * Two halves, deliberately split so XKB layout introspection works:
 *
 *   - keysym: the *meaning* of the key, layout-resolved by the OS+browser
 *     (`event.key`). Latin-1 characters use their codepoint directly, the
 *     SPECIAL_KEYS table handles the non-printing navigation / function /
 *     modifier set.
 *
 *   - keycode: the *physical key*, layout-INDEPENDENT, from `event.code`.
 *     We map to the standard evdev keycode numbering Linux X11 servers
 *     use (KEYCODE_EVDEV table below; same values that xkbcomp's
 *     `keycodes/evdev` produces). This makes XkbGetMap useful for
 *     applications that need to know "physical Q on this user's
 *     keyboard produces XK_a" -- they index by the stable evdev keycode
 *     and look up the layout-resolved keysym in keysym_table.
 *
 * Real X11 has the keymap on the server side; clients call XGetKeyboardMapping
 * or XkbGetMap to download it. We synthesise the equivalent from the browser:
 * the (code -> keycode) mapping is fixed at build time, the (keycode -> keysym)
 * map is filled at Display init from navigator.keyboard.getLayoutMap() (with
 * US QWERTY fallback for browsers that lack the Keyboard API).
 */

const NoSymbol = 0;

/**
 * KeyboardEvent.code -> evdev keycode. Numbers match the standard
 * Linux/X11 evdev keymap that xkbcomp ships -- see
 * https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/blob/master/keycodes/evdev
 *
 * Coverage: 104-key US ANSI + 105-key ISO additional key + the most common
 * extras (numpad, media keys, browser keys, IME composition keys). Codes
 * not in this map fall back to 0 and the C side will allocate a synthetic
 * keycode lazily -- correct behaviour but the key isn't introspectable.
 */
export const KEYCODE_EVDEV: Record<string, number> = {
  /* Top row */
  Escape:        9,
  F1:           67, F2: 68, F3: 69, F4: 70,
  F5:           71, F6: 72, F7: 73, F8: 74,
  F9:           75, F10: 76, F11: 95, F12: 96,
  F13:         191, F14: 192, F15: 193, F16: 194,
  F17:         195, F18: 196, F19: 197, F20: 198,
  F21:         199, F22: 200, F23: 201, F24: 202,
  PrintScreen: 107, ScrollLock: 78, Pause: 127,

  /* Digit row */
  Backquote:    49,
  Digit1:       10, Digit2: 11, Digit3: 12, Digit4: 13, Digit5: 14,
  Digit6:       15, Digit7: 16, Digit8: 17, Digit9: 18, Digit0: 19,
  Minus:        20, Equal: 21, Backspace: 22,
  Insert:      118, Home: 110, PageUp: 112,
  NumLock:      77,

  /* QWERTY row */
  Tab:          23,
  KeyQ:         24, KeyW: 25, KeyE: 26, KeyR: 27, KeyT: 28,
  KeyY:         29, KeyU: 30, KeyI: 31, KeyO: 32, KeyP: 33,
  BracketLeft:  34, BracketRight: 35, Backslash: 51,
  Delete:      119, End: 115, PageDown: 117,

  /* ASDF row */
  CapsLock:     66,
  KeyA:         38, KeyS: 39, KeyD: 40, KeyF: 41, KeyG: 42,
  KeyH:         43, KeyJ: 44, KeyK: 45, KeyL: 46,
  Semicolon:    47, Quote: 48, Enter: 36,

  /* ZXCV row */
  ShiftLeft:    50,
  IntlBackslash: 94,   /* ISO 105-key extra key, near LShift */
  KeyZ:         52, KeyX: 53, KeyC: 54, KeyV: 55, KeyB: 56,
  KeyN:         57, KeyM: 58,
  Comma:        59, Period: 60, Slash: 61,
  IntlRo:       97,    /* Japanese 109-key extra (\ near RShift) */
  ShiftRight:   62,
  ArrowUp:     111,

  /* Bottom row */
  ControlLeft:  37, MetaLeft: 133, AltLeft: 64,
  Space:        65,
  AltRight:    108, MetaRight: 134, ContextMenu: 135, ControlRight: 105,
  ArrowLeft:   113, ArrowDown: 116, ArrowRight: 114,

  /* Numpad */
  NumpadDivide:    106,
  NumpadMultiply:   63,
  NumpadSubtract:   82,
  NumpadAdd:        86,
  NumpadEnter:     104,
  NumpadDecimal:    91,
  Numpad0:          90,
  Numpad1:          87, Numpad2: 88, Numpad3: 89,
  Numpad4:          83, Numpad5: 84, Numpad6: 85,
  Numpad7:          79, Numpad8: 80, Numpad9: 81,
  NumpadEqual:     125,

  /* IME composition keys (Japanese / Korean / Chinese keyboards) */
  Convert:         100,
  NonConvert:      102,
  KanaMode:        101,
  Lang1:           130,   /* Hangul */
  Lang2:           131,   /* Hanja */
  Lang3:           132,   /* Katakana */
  Lang4:           133,   /* Hiragana */
  IntlYen:         133,

  /* Media / browser keys (when present) */
  VolumeMute:      121,
  VolumeDown:      122,
  VolumeUp:        123,
  MediaPlayPause:  172,
  MediaStop:       174,
  MediaTrackNext:  171,
  MediaTrackPrevious: 173,
  Power:           124,
};

/**
 * KeyboardEvent.key -> X11 keysym for non-printing keys.
 *
 * Printable characters are NOT in this table -- they come from
 * `event.key.charCodeAt(0)` directly (rule below in keyEventToKeysym).
 */
const SPECIAL_KEYS: Record<string, number> = {
  Enter: 0xff0d, // XK_Return
  Tab: 0xff09, // XK_Tab
  Backspace: 0xff08, // XK_BackSpace
  Escape: 0xff1b, // XK_Escape
  Delete: 0xffff, // XK_Delete
  Insert: 0xff63, // XK_Insert
  Home: 0xff50, // XK_Home
  End: 0xff57, // XK_End
  PageUp: 0xff55, // XK_Page_Up
  PageDown: 0xff56, // XK_Page_Down
  ArrowLeft: 0xff51, // XK_Left
  ArrowUp: 0xff52, // XK_Up
  ArrowRight: 0xff53, // XK_Right
  ArrowDown: 0xff54, // XK_Down
  CapsLock: 0xffe5, // XK_Caps_Lock
  NumLock: 0xff7f, // XK_Num_Lock
  ScrollLock: 0xff14, // XK_Scroll_Lock
  Pause: 0xff13, // XK_Pause
  PrintScreen: 0xff15, // XK_Sys_Req / Print
  Shift: 0xffe1, // XK_Shift_L
  Control: 0xffe3, // XK_Control_L
  Alt: 0xffe9, // XK_Alt_L
  AltGraph: 0xfe03, // XK_ISO_Level3_Shift
  Meta: 0xffeb, // XK_Super_L
  ContextMenu: 0xff67, // XK_Menu
  F1: 0xffbe, F2: 0xffbf, F3: 0xffc0, F4: 0xffc1, F5: 0xffc2, F6: 0xffc3,
  F7: 0xffc4, F8: 0xffc5, F9: 0xffc6, F10: 0xffc7, F11: 0xffc8, F12: 0xffc9,
  F13: 0xffca, F14: 0xffcb, F15: 0xffcc, F16: 0xffcd, F17: 0xffce, F18: 0xffcf,
  F19: 0xffd0, F20: 0xffd1, F21: 0xffd2, F22: 0xffd3, F23: 0xffd4, F24: 0xffd5,
};

export function keyEventToKeysym(e: KeyboardEvent): number {
  const mapped = SPECIAL_KEYS[e.key];
  if (mapped !== undefined) return mapped;
  if (e.key.length === 1) {
    const code = e.key.charCodeAt(0);
    if (code >= 0x20 && code <= 0xff) return code;
    /* Multi-byte codepoint (CJK / emoji / etc.): return the codepoint as
     * a Unicode-area keysym. The X11 convention is keysym = 0x01000000 |
     * codepoint, but Tk and most consumers happily read the codepoint
     * directly via Xutf8LookupString -- the keysym mostly drives
     * bindings (Bind <keysym>) for printable ASCII anyway. Use the
     * X11 Unicode convention to stay spec-compliant. */
    return 0x01000000 | code;
  }
  return NoSymbol;
}

/**
 * KeyboardEvent.code -> stable evdev keycode (or 0 if unknown).
 *
 * Stable means: the same physical key always produces the same keycode
 * regardless of the user's keyboard layout. The layout-resolved meaning
 * lives separately in the keysym (see keyEventToKeysym above).
 */
export function keyEventToKeycode(e: KeyboardEvent): number {
  return KEYCODE_EVDEV[e.code] ?? 0;
}

/* X modifier mask bits (from X11/X.h). */
const ShiftMask   = 1 << 0;
const LockMask    = 1 << 1;
const ControlMask = 1 << 2;
const Mod1Mask    = 1 << 3; // typically Alt
const Mod4Mask    = 1 << 6; // typically Meta / Super

/* X button state mask bits (from X11/X.h).
 * DOM e.buttons bit order differs from X11: primary=bit0, secondary=bit1,
 * auxiliary/middle=bit2, back=bit3, forward=bit4. Map to X11 Button1..5Mask. */
const Button1Mask = 1 << 8;
const Button2Mask = 1 << 9;
const Button3Mask = 1 << 10;
const Button4Mask = 1 << 11;
const Button5Mask = 1 << 12;

export function modifiersFromEvent(e: MouseEvent | KeyboardEvent): number {
  let state = 0;
  if (e.shiftKey) state |= ShiftMask;
  if (e.ctrlKey) state |= ControlMask;
  if (e.altKey) state |= Mod1Mask;
  if (e.metaKey) state |= Mod4Mask;
  if (e.getModifierState('CapsLock')) state |= LockMask;
  if (e instanceof MouseEvent) {
    const b = e.buttons;
    if (b & 1)  state |= Button1Mask;
    if (b & 4)  state |= Button2Mask;  // middle is bit2 in DOM
    if (b & 2)  state |= Button3Mask;  // right is bit1 in DOM
    if (b & 8)  state |= Button4Mask;
    if (b & 16) state |= Button5Mask;
  }
  return state;
}
