/**
 * Passive button grabs (XGrabButton / XUngrabButton).
 *
 * Mirrors xserver/dix/events.c::CheckPassiveGrabsOnWindow: at ButtonPress
 * time, the server walks from the deepest hit window UP through the parent
 * chain looking for a window that has a passive grab matching this
 * (button, modifier-mask) tuple. The first match wins -- the event is
 * redirected to the grab window in its coords; subsequent Motion +
 * ButtonRelease events route to the grab window too (via the implicit
 * "active grab" the C side already maintains in event.c).
 *
 * Without this, twm's add_window.c:1039 grabs (every managed frame:
 * Button1+modifiers for raise/drag, etc.) silently no-op and the WM never
 * sees clicks on its frames or root. Symptom: titlebar drag dead, root
 * menu dead, click-to-raise dead, icon manager half-broken.
 *
 * What we deliberately DON'T model (and don't need to, for twm + Tk):
 *   - Sync mode + XAllowEvents replay queue. We always treat the grab as
 *     async ("owner_events"-ish): event goes to the grab window, full stop.
 *     The first click of a click-to-focus is consumed by the WM -- not
 *     replayed to the client. Acceptable trade-off for now.
 *   - Per-grab event_mask filtering. We dispatch ButtonPress/Release and
 *     Motion events to grab holders unconditionally; if the holder didn't
 *     XSelectInput for them, the C-side queue filter drops them anyway.
 *   - confine_to / cursor. Cosmetic in our environment.
 *
 * Wildcards: AnyButton (0) and AnyModifier (1<<15) match anything in the
 * corresponding axis -- mirrors xorg's handling of the same constants.
 *
 * Lookup uses parent-chain walk (not tree scan), keyed off the `parent`
 * field on ManagedWindow. That's the source of truth for hit-test stack
 * traversal too, so input routing and paint semantics agree on what the
 * window tree looks like.
 */

import type { Host } from './index.js';

const AnyButton = 0;
const AnyModifier = 1 << 15;

/** Host-side modifier mask we care about. xorg's grab match uses the
 *  state byte (low 8 bits) -- same set we extract from the browser
 *  event in modifiersFromEvent. */
const MODIFIER_MASK = 0xff;

interface GrabEntry {
  window: number;
  button: number;
  modifiers: number; // already masked with MODIFIER_MASK or AnyModifier
  ownerEvents: boolean;
  eventMask: number;
  pointerMode: number;
  keyboardMode: number;
  confineTo: number;
  cursor: number;
}

export class GrabManager {
  /** windowId -> list of passive grabs registered on it. Order doesn't
   *  matter: at most one (button, mod) tuple per window is meaningful;
   *  duplicates from XGrabButton replace earlier entries. */
  private readonly byWindow = new Map<number, GrabEntry[]>();

  constructor(private readonly host: Host) {}

  /** XGrabButton -> install passive grab. Replace any existing entry
   *  with the same (button, modifiers) pair on the same window
   *  (xorg's CheckGrabValues-like semantics: re-grab updates). */
  add(
    window: number,
    button: number,
    modifiers: number,
    ownerEvents: boolean,
    eventMask: number,
    pointerMode: number,
    keyboardMode: number,
    confineTo: number,
    cursor: number,
  ): void {
    const mods = modifiers === AnyModifier ? AnyModifier : (modifiers & MODIFIER_MASK);
    const list = this.byWindow.get(window) ?? [];
    const existing = list.findIndex(
      (e) => e.button === button && e.modifiers === mods,
    );
    const entry: GrabEntry = {
      window,
      button,
      modifiers: mods,
      ownerEvents,
      eventMask,
      pointerMode,
      keyboardMode,
      confineTo,
      cursor,
    };
    if (existing >= 0) list[existing] = entry;
    else list.push(entry);
    this.byWindow.set(window, list);
  }

  /** XUngrabButton -> remove matching entry. AnyButton/AnyModifier here
   *  also act as wildcards (xorg matches the same way). */
  remove(window: number, button: number, modifiers: number): void {
    const list = this.byWindow.get(window);
    if (!list) return;
    const mods = modifiers === AnyModifier ? AnyModifier : (modifiers & MODIFIER_MASK);
    const filtered = list.filter(
      (e) =>
        !(
          (button === AnyButton || e.button === button) &&
          (mods === AnyModifier || e.modifiers === mods)
        ),
    );
    if (filtered.length === 0) this.byWindow.delete(window);
    else this.byWindow.set(window, filtered);
  }

  /** Drop every grab a destroyed window held. Called from
   *  WindowManager.onDestroy; xorg cleans up the same way via
   *  FreeAllAttachedGrabs. */
  forgetWindow(window: number): void {
    this.byWindow.delete(window);
  }

  /** Walk from `target` up the parent chain looking for the deepest
   *  matching grab. Mirrors CheckPassiveGrabsOnWindow + the implicit
   *  "first ancestor with a matching grab wins" rule. Returns the grab
   *  window's id, or null when no ancestor has a matching grab.
   *
   *  Why deepest-first: twm grabs both root (for the menu) and every
   *  frame (for click-to-raise). Clicking inside an application -- whose
   *  hit chain is `client < frame < root` -- must give the click to the
   *  frame, not the root. CheckPassiveGrabsOnWindow walks
   *  pWin->parent->parent->... but starts from the original event window,
   *  so the first hit IS the deepest. We replicate that order here. */
  lookup(targetId: number, button: number, state: number): number | null {
    const stateMask = state & MODIFIER_MASK;
    let cur: number | null = targetId;
    while (cur !== null && cur !== 0) {
      const list = this.byWindow.get(cur);
      if (list) {
        for (const e of list) {
          const buttonOk = e.button === AnyButton || e.button === button;
          const modsOk =
            e.modifiers === AnyModifier || e.modifiers === stateMask;
          if (buttonOk && modsOk) return cur;
        }
      }
      cur = this.parentOf(cur);
    }
    return null;
  }

  private parentOf(id: number): number | null {
    const w = this.host.renderer.windows.get(id);
    if (!w) return null;
    return w.parent === 0 ? null : w.parent;
  }

  /** DevTools-callable: dump every registered grab. Call as
   *  `__EMX11_DUMP_GRABS__()`. If the table is empty, twm never
   *  registered its grabs (or our XGrabButton stub still wins). */
  dump(): void {
    const total = [...this.byWindow.values()].reduce((n, l) => n + l.length, 0);
    console.log(`[grabs] ${total} entries on ${this.byWindow.size} windows`);
    for (const [id, list] of this.byWindow) {
      for (const e of list) {
        const btn = e.button === AnyButton ? 'AnyButton' : `b${e.button}`;
        const mods =
          e.modifiers === AnyModifier
            ? 'AnyModifier'
            : `0x${e.modifiers.toString(16)}`;
        console.log(
          `  win=${id} ${btn} mods=${mods} ownerEvents=${e.ownerEvents}` +
            ` mask=0x${e.eventMask.toString(16)}`,
        );
      }
    }
  }
}
