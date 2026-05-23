/**
 * Cursor support. Real X servers ship a font of cursor glyphs (the
 * "cursor font" in X11/cursorfont.h, 78 named shapes) plus a
 * client-defined-pixmap path. The browser's CSS `cursor:` property
 * already covers every shape twm/Xt actually use, so we map cursor
 * shape -> CSS keyword instead of rasterising glyphs.
 *
 * Cursor xids arrive from C through `emx11_js_window_set_cursor`.
 * Two encodings:
 *
 *   - 0x70000000 | shape  (Cursor.c::XCreateFontCursor) -- decode
 *     `shape` directly via `cursorFontCss`. shape is the X11 cursorfont
 *     constant (0..152, even values; odd values are the mask glyph and
 *     never appear here).
 *   - any other id -- pixmap cursor (XCreatePixmapCursor); we have no
 *     callers that need the actual bitmap, so fall through to 'default'.
 *
 * The mapping is intentionally partial: shapes nobody calls map to
 * 'default' rather than something approximate. If a client starts
 * relying on a missing shape, add it here.
 */

const FONT_CURSOR_TAG = 0x70000000;
const FONT_CURSOR_MASK = 0xFFF;

/* X11 cursorfont.h shape -> CSS cursor keyword. Shapes not listed
 * resolve to 'default'. Source for the shape constants:
 * libX11-1.8.13/include/X11/cursorfont.h */
const SHAPE_TO_CSS: Record<number, string> = {
  0:   'not-allowed',  // XC_X_cursor   (twm root cursor)
  2:   'default',      // XC_arrow
  12:  'sw-resize',    // XC_bottom_left_corner
  14:  'se-resize',    // XC_bottom_right_corner
  16:  's-resize',     // XC_bottom_side
  24:  'crosshair',    // XC_circle
  30:  'crosshair',    // XC_cross
  34:  'crosshair',    // XC_crosshair
  38:  'crosshair',    // XC_dot
  42:  'ns-resize',    // XC_double_arrow
  50:  'all-scroll',   // XC_exchange
  52:  'move',         // XC_fleur      (twm move)
  58:  'pointer',      // XC_hand1
  60:  'pointer',      // XC_hand2
  68:  'default',      // XC_left_ptr
  70:  'w-resize',     // XC_left_side
  86:  'crosshair',    // XC_pencil
  88:  'not-allowed',  // XC_pirate
  90:  'crosshair',    // XC_plus
  92:  'help',         // XC_question_arrow
  96:  'e-resize',     // XC_right_side
  106: 's-resize',     // XC_sb_down_arrow
  108: 'ew-resize',    // XC_sb_h_double_arrow (twm horizontal resize)
  110: 'w-resize',     // XC_sb_left_arrow
  112: 'e-resize',     // XC_sb_right_arrow
  114: 'n-resize',     // XC_sb_up_arrow
  116: 'ns-resize',    // XC_sb_v_double_arrow (twm vertical resize)
  120: 'nwse-resize',  // XC_sizing
  128: 'crosshair',    // XC_target
  132: 'default',      // XC_top_left_arrow
  134: 'nw-resize',    // XC_top_left_corner
  136: 'ne-resize',    // XC_top_right_corner
  138: 'n-resize',     // XC_top_side
  150: 'wait',         // XC_watch
  152: 'text',         // XC_xterm
};

/** Map a Cursor xid (as encoded by Cursor.c) to a CSS `cursor:`
 *  keyword. Pass 0 for "no cursor set" (XUndefineCursor / never set);
 *  callers should treat 0 as "inherit from parent". */
export function cursorXidToCss(xid: number): string | null {
  if (xid === 0) return null;
  if ((xid & ~FONT_CURSOR_MASK) === FONT_CURSOR_TAG) {
    const shape = xid & FONT_CURSOR_MASK;
    return SHAPE_TO_CSS[shape] ?? 'default';
  }
  return 'default';
}
