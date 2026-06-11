/**
 * Keyboard layout pre-fetch + push to each wasm process.
 *
 * On a real X server, every client downloads the keymap on connect via
 * XGetKeyboardMapping (or XkbGetMap). Our equivalent: at Host startup
 * we ask the browser's Keyboard API (`navigator.keyboard.getLayoutMap()`)
 * what the user's current keyboard layout is producing for each
 * physical key, and patch our keysym_table accordingly inside each
 * wasm process AFTER it's loaded but BEFORE its first KeyPress event.
 *
 * The C side (event_keysym.c::em_x11_us_qwerty) pre-fills US QWERTY as
 * the fallback for every evdev keycode. This module overwrites the
 * letter / digit / punctuation slots with what the user actually
 * types -- so a French AZERTY user's physical Q key reports the
 * keysym XK_a, matching what `event.key` returns when they press it.
 *
 * Browser support:
 *   - Chrome / Edge / Opera (desktop): full layoutmap available
 *   - Firefox / Safari / all mobile: navigator.keyboard is undefined,
 *     we skip the patch and keep US QWERTY defaults. Character input
 *     still works (KeyPress carries the resolved keysym), only
 *     XkbGetMap-style layout introspection reports US QWERTY for
 *     these browsers.
 */

import { KEYCODE_EVDEV } from '../runtime/keymap.js';
import type { ModuleCcallSurface } from './connection.js';

/**
 * Resolved layout entries: evdev keycode -> base-level keysym. Built
 * once per Host from navigator.keyboard.getLayoutMap() and reused
 * across every wasm process that connects.
 */
interface ResolvedLayout {
  /** Key: evdev keycode (8..255). Value: X11 keysym (Latin-1 codepoint
   *  for printable keys, XK_* for specials). */
  entries: Map<number, number>;
  /** True if navigator.keyboard.getLayoutMap() returned data; false
   *  when the API is missing or the user denied access. Callers can
   *  use this to decide whether XkbGetMap is reporting real layout
   *  data or just US QWERTY fallback. */
  fromBrowser: boolean;
}

/**
 * Convert a layoutmap label string (what `getLayoutMap().get('KeyQ')`
 * returns) to an X11 keysym.
 *
 * - Length-1 printable: codepoint directly (matches X11 Latin-1 and
 *   Unicode-area conventions).
 * - Length > 1 special strings: 'Dead' / 'Compose' / 'Combining...' all
 *   map to NoSymbol; we keep the US QWERTY default for those slots so
 *   the key still produces SOMETHING when pressed. (Real dead-key
 *   composition is handled by the OS+browser before the KeyboardEvent
 *   reaches us; we never see the dead key as a discrete press.)
 */
function labelToKeysym(label: string): number {
  if (label.length === 1) {
    const code = label.charCodeAt(0);
    if (code >= 0x20 && code <= 0xff) return code;
    /* Unicode-area keysym (per X11 spec) for codepoints above Latin-1.
     * Most layouts won't hit this (a single keystroke producing a
     * non-Latin-1 codepoint on the base level is rare outside CJK and
     * those go through composition anyway), but the keysym slot has to
     * be SOMETHING so XkbGetMap reports a value. */
    return 0x01000000 | code;
  }
  return 0;
}

export class KeyboardLayoutManager {
  private cache: Promise<ResolvedLayout> | null = null;
  /** Modules we've already pushed the layout into. The keysym_table is
   *  per-Display state inside each wasm, so a new wasm process needs its
   *  own patch pass; one Set entry per module avoids re-ccalling 100+
   *  em_x11_install_keysym for a module that's already up to date. */
  private appliedTo = new WeakSet<ModuleCcallSurface>();

  /** Fetch (or return cached) layout. First call kicks off the
   *  getLayoutMap() Promise; subsequent calls return the same Promise
   *  so all connections await the same network request. */
  async resolve(): Promise<ResolvedLayout> {
    if (this.cache) return this.cache;
    this.cache = this.fetchAndBuild();
    return this.cache;
  }

  /** Force a re-fetch on the next resolve(). Call after a layout change
   *  if you somehow detect one -- the Keyboard API doesn't emit events
   *  for layout switches, so this is mostly a manual escape hatch. */
  invalidate(): void {
    this.cache = null;
    this.appliedTo = new WeakSet();
  }

  /** Push the resolved layout into the given wasm module's
   *  keysym_table. Idempotent per module; safe to call repeatedly.
   *  Must run before the module's first KeyPress event, but after the
   *  module's XOpenDisplay has populated the US QWERTY defaults --
   *  ConnectionManager bindModule / launchClient is the right hook. */
  async applyToModule(module: ModuleCcallSurface): Promise<void> {
    if (this.appliedTo.has(module)) return;
    this.appliedTo.add(module);
    const layout = await this.resolve();
    if (layout.entries.size === 0) return;     /* US QWERTY suffices */
    for (const [keycode, keysym] of layout.entries) {
      module.ccall(
        'em_x11_install_keysym',
        null,
        ['number', 'number'],
        [keycode, keysym],
      );
    }
  }

  /** Expose the resolved layout to consumers (host.keyboard.getLayoutMap
   *  on the API surface) without re-fetching. The returned map mirrors
   *  the browser's KeyboardLayoutMap shape (KeyboardEvent.code ->
   *  label string) for application-level use; internally we maintain
   *  keycode->keysym for the wasm patch path. */
  async getLabels(): Promise<Map<string, string>> {
    return this.fetchLabels();
  }

  /* -- internal -------------------------------------------------------- */

  private async fetchAndBuild(): Promise<ResolvedLayout> {
    const labels = await this.fetchLabels();
    const entries = new Map<number, number>();
    for (const [code, label] of labels) {
      const keycode = KEYCODE_EVDEV[code];
      if (keycode === undefined) continue;     /* code we don't track */
      const keysym = labelToKeysym(label);
      if (keysym === 0) continue;              /* dead / empty / weird */
      entries.set(keycode, keysym);
    }
    return { entries, fromBrowser: labels.size > 0 };
  }

  private async fetchLabels(): Promise<Map<string, string>> {
    /* Feature-detect the Keyboard API. Firefox/Safari: undefined. */
    const nav = navigator as Navigator & {
      keyboard?: { getLayoutMap?: () => Promise<Map<string, string>> };
    };
    const api = nav.keyboard?.getLayoutMap;
    if (typeof api !== 'function') return new Map();
    try {
      return await api.call(nav.keyboard);
    } catch {
      /* Permission denied or hardware lookup failed -- fall through to
       * empty map; US QWERTY default keeps every demo working. */
      return new Map();
    }
  }
}
