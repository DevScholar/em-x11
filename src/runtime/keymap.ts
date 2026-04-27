/**
 * Browser KeyboardEvent -> X11 keysym translation.
 *
 * We only send a KEYSYM from the TS side; the C side allocates a synthetic
 * keycode on demand (native/src/event.c `emx11_keysym_to_keycode`). Clients
 * retrieving the keysym through XLookupKeysym / XKeycodeToKeysym will see
 * the value we produce here.
 *
 * Rules:
 *   1. Printable ASCII characters (space .. tilde) use their Latin-1 code
 *      point as the keysym -- X11 defined Latin-1 range 0x20..0xFF to
 *      match Unicode, which matches what `event.key` returns.
 *   2. Non-printing navigation / function / modifier keys use XK_* keysyms
 *      from X11/keysymdef.h, expressed numerically in the SPECIAL_KEYS
 *      table below. If `event.key` isn't a known special name, we fall
 *      back to NoSymbol (0).
 */

const NoSymbol = 0;

/** KeyboardEvent.key -> X11 keysym. */
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
  Shift: 0xffe1, // XK_Shift_L
  Control: 0xffe3, // XK_Control_L
  Alt: 0xffe9, // XK_Alt_L
  Meta: 0xffeb, // XK_Super_L
  F1: 0xffbe,
  F2: 0xffbf,
  F3: 0xffc0,
  F4: 0xffc1,
  F5: 0xffc2,
  F6: 0xffc3,
  F7: 0xffc4,
  F8: 0xffc5,
  F9: 0xffc6,
  F10: 0xffc7,
  F11: 0xffc8,
  F12: 0xffc9,
};

export function keyEventToKeysym(e: KeyboardEvent): number {
  const mapped = SPECIAL_KEYS[e.key];
  if (mapped !== undefined) return mapped;
  if (e.key.length === 1) {
    const code = e.key.charCodeAt(0);
    if (code >= 0x20 && code <= 0xff) return code;
  }
  return NoSymbol;
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
