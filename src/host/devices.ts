/**
 * Input bridge. Translates browser DOM mouse/keyboard events into the
 * X event types the C side expects, and routes each one to the right
 * wasm Module via ccall. Mirrors xserver/dix/devices.c + dix/events.c
 * pointer/key delivery, minus the proper grab semantics (we have a
 * coarse "implicit grab on ButtonPress" approximation, no XGrabPointer).
 *
 * Two delivery paths:
 *
 *   - DOM mode (default): InputBridge attaches its own listeners on
 *     window + canvas in the constructor. Demo / tcldide path.
 *   - Headless / OffscreenCanvas mode: attachment is skipped; caller
 *     is expected to call pushMouseDown/Up/Move/Key + setPointer with
 *     canvas-local coordinates. Used by the pyodide-tk entry that
 *     relays events from the main thread.
 */

import type { Host } from './index.js';
import type { Point } from '../types/emscripten.js';
import type { ModuleCcallSurface } from './connection.js';
import { getDebugFlags } from '../runtime/debug-flags.js';
import { keyEventToKeysym, keyEventToKeycode, modifiersFromEvent } from '../runtime/keymap.js';
import { cursorXidToCss } from './cursor.js';
import {
  X_ButtonPress,
  X_ButtonRelease,
  X_ButtonPressMask,
  X_ButtonReleaseMask,
  X_KeyPress,
  X_KeyPressMask,
  X_KeyRelease,
} from './constants.js';

/** Plain-data shape used by both the DOM path (after rect translation)
 *  and the relay path (pyodide-tk main-thread → Worker). All coords
 *  are canvas-local device pixels. */
export interface MouseEventData {
  x: number;
  y: number;
  button: number;     // X11 button: 1 left, 2 middle, 3 right
  modifiers: number;  // X11 modifier mask
}

export interface KeyEventData {
  keysym: number;
  /** Physical key from KeyboardEvent.code, mapped to evdev keycode.
   *  Stable across keyboard layouts -- the layout-resolved meaning
   *  lives in keysym. 0 if the browser code isn't in the evdev map
   *  (very rare; older codes, vendor-specific keys). */
  keycode: number;
  modifiers: number;
  /** True when the canvas had keyboard focus (DOM path uses
   *  document.activeElement). Worker path can pass true unconditionally
   *  since the canvas is the only interactive surface in that frame. */
  hasFocus: boolean;
  /** Optional UTF-8 string the browser produced for this key. Plain
   *  ASCII typing fills this from KeyboardEvent.key (length-1 char);
   *  IME / paste / autocomplete arrive via the hidden textarea
   *  (text-input.ts) and call pushTextKey instead. */
  text?: string;
}

export class InputBridge {
  private pointerX: number;
  private pointerY: number;
  /** Last window that received a ButtonPress, by connection id. Key
   *  events route here until another ButtonPress moves focus. */
  private focusedWindow: number | null = null;
  /** Bridged from any client's XSetInputFocus call. The WM (twm) issues
   *  XSetInputFocus on the inner client window after click-to-raise to
   *  hand keyboard control off to the app, but that call only updates
   *  the WM's own Display struct; without this hook the host's
   *  focusedWindow stays pointed at the WM's frame and key events keep
   *  going to the WM. None / PointerRoot reset us to "no override".
   *  Whenever this is non-null and resolves to a known window, it wins
   *  over the press-driven focusedWindow. */
  private explicitFocus: number | null = null;
  /** Module that owns the current implicit pointer grab.  Set by the C side
   *  via onImplicitGrabStart bridge (called from em_x11_push_button_event on
   *  ButtonPress); cleared by onImplicitGrabEnd (called on final
   *  ButtonRelease or grab teardown).  Used by pushMouseMove + deliverButton
   *  to route motion and release events to the grabbing module even when the
   *  pointer is outside all mapped windows. */
  implicitDragModule: ModuleCcallSurface | null = null;
  /** XGrabPointer cursor override. While non-null this CSS cursor is
   *  shown everywhere on the canvas, replacing the per-window
   *  XDefineCursor resolution -- twm needs this so MoveCursor /
   *  ResizeCursor stay visible during a drag even as the pointer
   *  crosses other windows. Cleared by XUngrabPointer. */
  private grabCursor: string | null = null;
  /** Worker-mode callback: when set, applyCursorFor() sends the resolved
   *  CSS cursor here instead of writing el.style.cursor (which doesn't
   *  exist on an OffscreenCanvas). The callback typically posts a
   *  message to the main thread where the visible canvas lives. */
  private cursorRemote: ((css: string) => void) | null = null;
  /** Active pointer grab: while non-null, every button + motion event
   *  routes to this module instead of going through passive grab /
   *  subscriber lookup. Mirrors the xserver ActivateGrab path. Set by
   *  the C-side XGrabPointer bridge; cleared by XUngrabPointer. */
  private activePointerGrab:
    | { window: number; module: ModuleCcallSurface }
    | null = null;
  /** DOM listeners attached by attachDom(), recorded so dispose() can
   *  remove them. Without this, recreating a Host (HMR, multi-canvas
   *  test harness) leaks the previous Host through closure refs. */
  private domListeners: Array<{
    target: EventTarget;
    type: string;
    handler: EventListener;
  }> = [];

  constructor(private readonly host: Host) {
    /* Default the pointer to the canvas centre so the first XQueryPointer
     * after XtRealizeWidget -- which fires before the user has had a
     * chance to move the mouse -- returns something sensible instead of
     * the top-left corner. xeyes in particular snaps its pupils here
     * immediately. */
    this.pointerX = (this.host.canvas.cssWidth / 2) | 0;
    this.pointerY = (this.host.canvas.cssHeight / 2) | 0;

    if (!this.host.canvas.headless) this.attachDom();
  }

  getPointerXY(): Point {
    return { x: this.pointerX, y: this.pointerY };
  }

  /** Re-resolve the cursor for the window currently under the pointer.
   *  Called by WindowManager.onSetCursor so XDefineCursor takes effect
   *  even when the pointer is not moving (twm sets several cursors at
   *  startup before the user touches the mouse). */
  refreshCanvasCursor(): void {
    const win = this.host.renderer.findWindowAt(this.pointerX, this.pointerY);
    this.applyCursorFor(win);
  }

  /** XGrabPointer cursor override. cursorXid=0 clears the grab and
   *  reverts to the per-window cursor under the current pointer
   *  position. Called from XGrabPointer / XUngrabPointer. */
  setGrabCursor(cursorXid: number): void {
    this.grabCursor = cursorXid === 0 ? null : cursorXidToCss(cursorXid);
    this.refreshCanvasCursor();
  }

  /** Install a worker→main bridge for cursor changes. In headless mode
   *  (OffscreenCanvas, worker), the resolved CSS cursor is sent to this
   *  callback instead of being written to el.style.cursor. Typically
   *  wired to postMessage a 'cursorChange' to the main thread. */
  setCursorRemote(fn: ((css: string) => void) | null): void {
    this.cursorRemote = fn;
  }

  /** Install an active pointer grab on behalf of the wasm process
   *  identified by `connId`. Until cleared, all button + motion events
   *  route to that connection regardless of which window the pointer
   *  is over. */
  setActivePointerGrab(connId: number, window: number): void {
    const module = this.moduleForConn(connId);
    if (!module) {
      console.warn(
        `em-x11: XGrabPointer on window ${window >>> 0} but conn ${connId} ` +
        `has no bound Module. Grab silently dropped. This is expected ` +
        `during early bootstrap but indicates a timing bug if seen ` +
        `during interactive use.`,
      );
      return;
    }
    this.activePointerGrab = { window, module };
  }

  clearActivePointerGrab(): void {
    if (this.activePointerGrab) {
      this.implicitDragModule = null;
    }
    this.activePointerGrab = null;
  }

  /** Drop every input-routing slot pointing at the closed connection's
   *  Module. Without this, F_RESTART's kill+respawn leaves the active
   *  pointer grab (twm holds one while a menu is up), the implicit drag
   *  grab from the menu click, or the keyboard focus pointing at the
   *  dead Module -- every subsequent ccall lands in unloaded wasm code
   *  and the canvas appears frozen. Resolution by *Module identity*
   *  rather than connId, because windows owned by the dead conn are
   *  destroyed before this runs so focusedWindow's owner is already
   *  gone. */
  forgetConnection(module: ModuleCcallSurface | null): void {
    if (this.activePointerGrab && this.activePointerGrab.module === module) {
      this.activePointerGrab = null;
      this.grabCursor = null;
      this.implicitDragModule = null;
    }
    if (this.implicitDragModule === module) this.implicitDragModule = null;
    /* focusedWindow / explicitFocus may point at a window the dead conn
     * owned; the WindowManager teardown already drops those windows from
     * the renderer, so moduleForWindow returns null. Reset eagerly so
     * the first key event after respawn doesn't have to walk through
     * a stale id. */
    if (this.focusedWindow !== null &&
        this.host.connection.connOf(this.focusedWindow) === undefined) {
      this.focusedWindow = null;
    }
    if (this.explicitFocus !== null &&
        this.host.connection.connOf(this.explicitFocus) === undefined) {
      this.explicitFocus = null;
    }
  }

  /** Walk `winId`'s parent chain in the renderer tree and pick the
   *  nearest ancestor with a non-null `cursor`. Mirrors X server cursor
   *  inheritance (xserver/dix/cursor.c::CheckCursorConfinement walks
   *  toward root until it finds a window with a cursor set). */
  private applyCursorFor(winId: number | null): void {
    let css = 'default';
    if (this.grabCursor) {
      css = this.grabCursor;
    } else if (winId !== null) {
      let cur = this.host.renderer.windows.get(winId);
      while (cur) {
        if (cur.cursor) { css = cur.cursor; break; }
        if (cur.parent === 0) break;
        cur = this.host.renderer.windows.get(cur.parent);
      }
    }
    if (this.host.canvas.headless) {
      this.cursorRemote?.(css);
      return;
    }
    const el = this.host.canvas.element;
    if (el.style.cursor !== css) el.style.cursor = css;
  }

  /* -- public input feed (used by both paths) ---------------------------- */

  /** Update cached pointer position. xeyes & friends poll this from
   *  Tcl/Xt timers; XQueryPointer reads it without a hit test. */
  setPointer(x: number, y: number): void {
    this.pointerX = x | 0;
    this.pointerY = y | 0;
  }

  pushMouseDown(e: MouseEventData): void {
    this.deliverButton(X_ButtonPress, e);
  }
  pushMouseUp(e: MouseEventData): void {
    this.deliverButton(X_ButtonRelease, e);
  }

  /** Translate a browser WheelEvent into paired X11 ButtonPress+ButtonRelease.
   *  Classic X11 maps scroll to Button4 (up) and Button5 (down). Each
   *  wheel "tick" produces one immediate press-then-release pair — no
   *  separate release event, since the wheel has no held state. */
  pushWheel(e: Omit<MouseEventData, 'button'> & { deltaY: number }): void {
    const button = e.deltaY < 0 ? 4 : 5;
    this.pushMouseDown({ x: e.x, y: e.y, button, modifiers: e.modifiers });
    this.pushMouseUp({ x: e.x, y: e.y, button, modifiers: e.modifiers });
  }

  pushMouseMove(e: Omit<MouseEventData, 'button'>): void {
    this.setPointer(e.x, e.y);
    const win = this.host.renderer.findWindowAt(e.x, e.y);
    this.applyCursorFor(win);
    /* Active grab from XGrabPointer wins over the implicit ButtonPress
     * grab and over the under-cursor module: every motion event must
     * reach the grabbing client until XUngrabPointer. Twm relies on
     * this for menu drag-tracking and DeferExecution cursor follow. */
    /* X11 implicit pointer grab (x11protocol.txt §523): once a button is
     * pressed, all Motion and ButtonRelease events route to the grabbing
     * client regardless of where the pointer moves. dragModule holds the
     * module that saw the ButtonPress, so route to it unconditionally
     * while a drag is in progress -- a TWM title-bar drag crosses over
     * xeyes' frame mid-drag, but Motion must still reach TWM, not xeyes. */
    const module =
      this.activePointerGrab?.module ??
      this.implicitDragModule ??
      (win !== null ? this.moduleForWindow(win) : null);
    if (getDebugFlags()?.traceMotion) {
      const route = this.activePointerGrab ? 'activeGrab' :
                    this.implicitDragModule ? 'dragModule' :
                    win !== null ? 'win' : 'NONE';
      console.log(
        `[mot] (${e.x}, ${e.y}) win=${win} route=${route} module=${module ? 'Y' : 'N'}`,
      );
    }
    if (!module) return;
    /* XMotionEvent.x/y are window-local; .x_root/.y_root are screen-
     * absolute. Translate canvas pixels into the delivery window's coord
     * system the same way deliverButton does -- without this twm's
     * UpdateMenu (which uses event.y / EntryHeight to pick the hovered
     * item) sees absolute pixels and never matches any item, so the menu
     * pops up but never highlights. */
    const target = win ?? 0;
    const origin = target !== 0 ? this.host.getWindowAbsOrigin(target) : null;
    const lx = origin ? e.x - origin.ax : e.x;
    const ly = origin ? e.y - origin.ay : e.y;
    module.ccall(
      'em_x11_push_motion_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [target, lx, ly, e.x, e.y, e.modifiers],
    );
  }

  pushKey(xType: number, e: KeyEventData): void {
    /* focusedWindow (set on every ButtonPress to the nearest KeyPressMask
     * ancestor) takes priority over explicitFocus (set by XSetInputFocus
     * bridge, e.g. auto-focus-on-map). Without this order, the first-mapped
     * toplevel (which may not be an Xt-recognised widget window) permanently
     * steals key events away from widgets the user actually clicked on. */
    const focus = this.focusedWindow !== null
      ? this.focusedWindow
      : this.explicitFocus;
    if (focus === null) return;
    const module = this.moduleForWindow(focus);
    if (!module) return;
    if (e.keysym === 0 && e.keycode === 0 && (!e.text || e.text.length === 0)) return;
    /* Stage the typed UTF-8 (if any) so Xutf8LookupString in wasm
     * returns it for the matching XKeyEvent. Empty string clears the
     * slot so a KeyRelease can't inherit text from a previous KeyPress. */
    module.ccall('em_x11_set_pending_key_text', null, ['string'],
                 [e.text ?? '']);
    /* New entry point: pass keycode + keysym separately so the wasm
     * side stores the layout-independent physical key in XKeyEvent
     * (XkbGetMap-friendly) without losing the layout-resolved keysym
     * that drives bindings. Old em_x11_push_key_event is still exported
     * for legacy consumers (it derives keycode by reverse keysym lookup). */
    module.ccall(
      'em_x11_push_key_event_kc',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number', 'number'],
      [xType, focus, e.keycode, e.keysym, e.modifiers, 0, 0],
    );
  }

  /** Synthetic text-only KeyPress used by the hidden-textarea overlay
   *  for IME composition results, paste, and other beforeinput sources
   *  that don't have a useful keysym. Sends a paired KeyPress/KeyRelease
   *  carrying the same UTF-8 bytes -- Tk's tkUnixKey.c only acts on
   *  KeyPress, but bindings on <KeyRelease> are still legal so we keep
   *  them paired for symmetry. */
  pushTextKey(text: string): void {
    if (!text) return;
    const focus = this.focusedWindow !== null
      ? this.focusedWindow
      : this.explicitFocus;
    if (focus === null) return;
    const module = this.moduleForWindow(focus);
    if (!module) return;
    module.ccall('em_x11_set_pending_key_text', null, ['string'], [text]);
    /* Synthetic text-only keys have no physical origin -- keycode 0
     * so XkbGetMap consumers don't misattribute IME / paste bytes to
     * a real key on the user's keyboard. */
    module.ccall(
      'em_x11_push_key_event_kc', null,
      ['number', 'number', 'number', 'number', 'number', 'number', 'number'],
      [X_KeyPress, focus, 0, 0, 0, 0, 0],
    );
    module.ccall('em_x11_set_pending_key_text', null, ['string'], ['']);
    module.ccall(
      'em_x11_push_key_event_kc', null,
      ['number', 'number', 'number', 'number', 'number', 'number', 'number'],
      [X_KeyRelease, focus, 0, 0, 0, 0, 0],
    );
  }

  /* -- Inline preedit bridges (xim.c) ------------------------------------ */

  /** compositionstart: begin inline preedit on the focused widget. */
  pushPreeditStart(window: number): void {
    const module = this.moduleForWindow(window);
    if (!module) return;
    module.ccall('em_x11_xim_preedit_start', null, ['number'], [window]);
  }

  /** compositionupdate: deliver composing text to Tk's preedit draw callback.
   *  caret is the cursor position within the preedit string (character offset).
   *  chgFirst/chgLength indicate the changed range (0/full-length = redraw all). */
  pushPreeditDraw(window: number, text: string, caret: number,
                  chgFirst: number, chgLength: number): void {
    const module = this.moduleForWindow(window);
    if (!module) return;
    module.ccall('em_x11_xim_preedit_draw', null,
                 ['number', 'string', 'number', 'number', 'number'],
                 [window, text, caret, chgFirst, chgLength]);
  }

  /** compositionend: clear inline preedit and fire done callback. */
  pushPreeditDone(window: number): void {
    const module = this.moduleForWindow(window);
    if (!module) return;
    module.ccall('em_x11_xim_preedit_done', null, ['number'], [window]);
  }

  /** Bridged from XSetInputFocus on any module. None (0) / PointerRoot (1)
   *  clear the explicit override and let press-driven focus take over.
   *  XSetInputFocus is an intentional focus change (WM hand-off, Motif
   *  menu posting) that must override the press-driven focusedWindow —
   *  without clearing it, arrow keys in posted Motif menus route to the
   *  menu bar RowColumn (which has no arrow-key translations) instead of
   *  the MenuShell/popup RowColumn that does. */
  setExplicitFocus(window: number): void {
    if (window === 0 || window === 1) {
      this.explicitFocus = null;
      return;
    }
    this.explicitFocus = window;
    this.focusedWindow = null;
  }

  pushKeyDown(e: KeyEventData): void { this.pushKey(X_KeyPress, e); }
  pushKeyUp(e: KeyEventData): void { this.pushKey(X_KeyRelease, e); }

  /* -- internal --------------------------------------------------------- */

  private deliverButton(xType: number, e: MouseEventData): void {
    this.setPointer(e.x, e.y);
    const target = this.host.renderer.findWindowAt(e.x, e.y);
    /* X11 button-event state must be the modifier mask BEFORE this
     * event (xserver/dix/events.c::DeliverDeviceEvents). e.modifiers
     * comes from `MouseEvent.buttons`, which is the state AFTER the
     * DOM event. For ButtonPress, clear the just-pressed button's mask;
     * for ButtonRelease, restore the just-released button's mask so it
     * reflects "was still held before release". Without this, twm sees
     * state=Button1Mask on the very first ButtonPress and skips f.move. */
    const btnMask = 1 << (7 + e.button);
    const x11State = xType === X_ButtonPress
      ? e.modifiers & ~btnMask
      : e.modifiers | btnMask;
    const traceFlag = !!getDebugFlags()?.traceButton;
    if (traceFlag) {
      console.log(
        `[btn] type=${xType} (${e.x}, ${e.y}) target=${target} button=${e.button} mods=0x${e.modifiers.toString(16)} x11State=0x${x11State.toString(16)}`,
      );
    }
    /* ButtonRelease must always reach the C side even when the pointer
     * is outside all mapped windows (findWindowAt returns null). The C
     * side's implicit grab (grab_window / grab_button_count) can only
     * clear via a matching release; if we drop the event here the grab
     * persists forever — dragModule stays stale, motion events route to
     * the wrong module, and multiple widgets stay painted in "armed"
     * state. xorg also generates ButtonRelease for off-root releases. */
    if (target === null) {
      if (xType !== X_ButtonRelease) return;
      if (this.activePointerGrab) {
        const grab = this.activePointerGrab;
        if (grab.module) {
          grab.module.ccall('em_x11_push_button_event', null,
            ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
            [xType, 0, e.x, e.y, e.x, e.y, e.button, x11State]);
        }
      } else if (this.implicitDragModule) {
        this.implicitDragModule.ccall('em_x11_push_button_event', null,
          ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
          [xType, 0, e.x, e.y, e.x, e.y, e.button, x11State]);
      }
      return;
    }

    /* Active pointer grab from XGrabPointer: every button event goes
     * straight to the grabbing client, bypassing passive grab lookup
     * and subscriber propagation. Without this, twm's DeferExecution
     * (set up after the user releases on Iconify/Move/Resize/Focus/
     * Delete in a root menu) can't catch the user's follow-up click on
     * a target window -- the click would otherwise route to the
     * clicked client, leaving twm waiting forever and the menu item
     * looking dead. */
    if (this.activePointerGrab) {
      const grab = this.activePointerGrab;
      const origin = this.host.getWindowAbsOrigin(target);
      const lx = origin ? e.x - origin.ax : e.x;
      const ly = origin ? e.y - origin.ay : e.y;
      if (xType === X_ButtonPress && e.button < 4) {
        this.focusedWindow = grab.window;
      }
      grab.module.ccall(
        'em_x11_push_button_event',
        null,
        ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
        [xType, target, lx, ly, e.x, e.y, e.button, x11State],
      );
      return;
    }

    /* Routing in xorg order (xserver/dix/events.c::DeliverDeviceEvents):
     *
     *   1. ActiveGrab (we don't model server-grabs explicitly here -- the
     *      C side maintains an "implicit pointer grab" that routes
     *      Motion/Release to the press's destination, see implicitDragModule).
     *   2. PassiveGrab: walk from `target` up to root looking for a
     *      passive XGrabButton with matching (button, modifiers).
     *      First match wins; event delivered to grab_window in its coords.
     *   3. Plain delivery: walk from `target` up to root looking for any
     *      ancestor with a client that selected ButtonPress(Release)Mask.
     *      Mirrors DeliverEventsToWindow's propagation. The first
     *      ancestor's subscriber gets the event; further ancestors and
     *      other subscribers on the same window are not consulted (xorg
     *      delivers to all matching clients via OtherClients, but in
     *      practice only one client cares about button events on any
     *      given path -- the WM at the frame, the app at the leaf).
     *
     * If neither (2) nor (3) finds anyone, drop the event. Real xorg
     * silently discards in the same scenario.
     *
     * Why we don't just route to `target`'s owner: an X application
     * may not select for ButtonPress on its leaf widgets at all -- Xt
     * only selects on the toplevel shell, and lets the WM-installed
     * ButtonPress mask on the frame fire instead. The owner of the
     * deepest hit window is the application, but the SUBSCRIBER is
     * the WM at the frame. Without propagation, click-to-raise and
     * title-bar drag both vanish. */
    let deliveryWin: number;
    let deliveryConn: number | null = null;
    let viaGrab = false;

    if (xType === X_ButtonPress) {
      const grabWin = this.host.grabs.lookup(target, e.button, x11State);
      if (grabWin !== null) {
        deliveryWin = grabWin;
        viaGrab = true;
      } else {
        const sub = this.host.events.findSubscriberFor(target, X_ButtonPressMask);
        if (traceFlag) {
          console.log(
            `  subscriber lookup -> ${sub ? `win=${sub.winId} conn=${sub.connId}` : 'null'}`,
          );
        }
        if (!sub) {
          /* No subscriber on the parent chain; fall back to owner so
           * grab-less clicks on simple test apps (xt-hello) still get
           * delivered. xorg drops here, but our flat client-only mode
           * has no propagation safety net otherwise. */
          deliveryWin = target;
          deliveryConn = this.host.connection.connOf(target) ?? null;
        } else {
          /* Route to the subscriber's connection, but keep the original
           * deepest hit window as deliveryWin. The C-side walk_up_for_mask
           * checks its own EmxWindow.event_mask (set via XCreateWindow /
           * XChangeWindowAttributes CWEventMask), which may differ from the
           * JS-side subscription table. When the deepest window (e.g. twm's
           * title bar) does have the mask in the C struct, walk_up_for_mask
           * returns it directly -> event.window stays the title bar ->
           * twm's HandleButtonPress classifies as C_TITLE -> f.move fires.
           * If the C mask is also missing, walk_up_for_mask walks up to the
           * frame as before -- no regression. */
          deliveryWin = target;
          deliveryConn = sub.connId;
        }
      }
    } else {
      /* ButtonRelease: route to whoever holds the implicit grab so it
       * pairs with the press. The C side's grab_window state will set
       * event.window correctly relative to the grab; we just need to
       * deliver to the right MODULE here. dragModule was captured at
       * press time. */
      deliveryWin = target;
      if (this.implicitDragModule) {
        /* Route via dragModule directly without consulting subscriber
         * tables -- the press already chose. */
        const origin = this.host.getWindowAbsOrigin(deliveryWin);
        const lx = origin ? e.x - origin.ax : e.x;
        const ly = origin ? e.y - origin.ay : e.y;
        this.implicitDragModule.ccall(
          'em_x11_push_button_event',
          null,
          ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
          [xType, deliveryWin, lx, ly, e.x, e.y, e.button, x11State],
        );
        return;
      }
      const sub = this.host.events.findSubscriberFor(target, X_ButtonReleaseMask);
      if (sub) {
        deliveryWin = target;
        deliveryConn = sub.connId;
      } else {
        deliveryConn = this.host.connection.connOf(target) ?? null;
      }
    }

    /* Resolve module from grab/subscriber. Grabs go via the grab
     * window's owner (passive grab semantics: event delivered to whoever
     * created and registered the grab). Subscriber path uses the
     * resolved connId directly. */
    const module = viaGrab
      ? this.moduleForWindow(deliveryWin)
      : deliveryConn !== null
        ? this.moduleForConn(deliveryConn)
        : null;
    if (!module) {
      if (traceFlag) console.log(`  no module for delivery, dropping`);
      return;
    }

    /* Translate the press point into delivery window's coord system. */
    const origin = this.host.getWindowAbsOrigin(deliveryWin);
    const lx = origin ? e.x - origin.ax : e.x;
    const ly = origin ? e.y - origin.ay : e.y;

    if (xType === X_ButtonPress && e.button < 4) {
      /* Focus tracking diverges from delivery: route keys to the nearest
       * ancestor that selected KeyPressMask, not whoever owns ButtonPress.
       * twm grabs ButtonPress on its frame for click-to-raise; without
       * this split, focusedWindow lands on the frame and keys go to twm
       * even after the user clicks the inner client. xcalc happens to
       * select KeyPressMask on the same widget Xt selects ButtonPressMask
       * on, so this path matches the old behavior there. glxgears (no
       * WMHints, so twm never calls XSetInputFocus on its behalf) needs
       * this implicit press-driven focus to receive keys at all.
       *
       * Guard e.button < 4: only physical mouse buttons (left/middle/right)
       * change keyboard focus and start the implicit pointer grab. Scroll
       * buttons (4/5) are delivered as immediate press+release pairs and
       * must not steal focus or create a drag. */
      const keySub = this.host.events.findSubscriberFor(target, X_KeyPressMask);
      this.focusedWindow = keySub?.winId ?? deliveryWin;
      /* ButtonPress-driven focus supersedes the auto-focus-on-map explicit
       * override (which may point at a non-Xt toplevel the Xt dispatch
       * can't resolve). Clear it so pushKey prioritises focusedWindow. */
      this.explicitFocus = null;
    }
    module.ccall(
      'em_x11_push_button_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
      [xType, deliveryWin, lx, ly, e.x, e.y, e.button, x11State],
    );
  }

  /** Resolve the Module for a connection id directly. Used by the
   *  subscriber-propagation path -- once findSubscriberFor returns a
   *  connId, we already know who to talk to without going through the
   *  window-owner mapping. */
  private moduleForConn(connId: number): ModuleCcallSurface | null {
    if (connId === 0) {
      for (const conn of this.host.connection.values()) {
        if (conn.module) return conn.module;
      }
      return null;
    }
    const conn = this.host.connection.get(connId);
    return conn?.module ?? null;
  }

  /** Resolve the Module that owns a window. Returns null if the window
   *  isn't tracked, if the owning connection has no Module (legacy
   *  headless case), or if the connection was closed.
   *
   *  Host-owned windows (conn_id=0, currently just the shared root)
   *  have no "owner" Module. As a temporary fallback we route their
   *  events to the first real connection -- which in the session-demo
   *  launch convention is twm, the window manager. A future step will
   *  replace this with an XSelectInput subscription table on the Host
   *  side so MapRequest / SubstructureRedirect events are dispatched
   *  to the actual holder(s), not a positional heuristic. */
  private moduleForWindow(winId: number): ModuleCcallSurface | null {
    const connId = this.host.connection.connOf(winId);
    if (connId === undefined) return null;
    if (connId === 0) {
      for (const conn of this.host.connection.values()) {
        if (conn.module) return conn.module;
      }
      return null;
    }
    const conn = this.host.connection.get(connId);
    return conn?.module ?? null;
  }

  /* -- DOM attachment (legacy / main-thread path) ----------------------- */

  private attachDom(): void {
    const el = this.host.canvas.element;
    const on = <K extends EventTarget>(target: K, type: string, handler: EventListener): void => {
      target.addEventListener(type, handler);
      this.domListeners.push({ target, type, handler });
    };

    /* Track the last-seen pointer position at the host level so polling
     * callers (XQueryPointer; xeyes uses this every 50ms via an Xt timer)
     * can read it without going through the event bridge's hit test.
     * We listen on `window` rather than the canvas so mouse motion
     * outside the canvas (over browser chrome, over another page region)
     * still updates the cached position -- pupils that track the mouse
     * off-canvas look better than pupils that freeze on exit. */
    on(window, 'mousemove', (ev) => {
      const e = ev as MouseEvent;
      const rect = el.getBoundingClientRect();
      this.setPointer(e.clientX - rect.left, e.clientY - rect.top);
    });

    on(el, 'mousedown', (ev) => {
      const e = ev as MouseEvent;
      const { x, y } = this.cssPoint(e, el);
      this.pushMouseDown({ x, y, button: e.button + 1, modifiers: modifiersFromEvent(e) });
    });
    // Listen on window so a ButtonRelease outside the canvas (pointer moved
    // off during a drag) still reaches the grab window via the C-side grab.
    on(window, 'mouseup', (ev) => {
      const e = ev as MouseEvent;
      const { x, y } = this.cssPoint(e, el);
      this.pushMouseUp({ x, y, button: e.button + 1, modifiers: modifiersFromEvent(e) });
    });
    // Pointer left the canvas entirely. If a button is still logically
    // held (dragModule or activePointerGrab), the browser won't send us
    // mouseup because the release happened outside the page (e.g. after
    // alt-tabbing away). Synthesize a release so the C-side implicit
    // grab clears and the user doesn't come back to a stuck arm state.
    on(el, 'mouseleave', (_ev) => {
      const e = _ev as MouseEvent;
      const { x, y } = this.cssPoint(e, el);
      const mods = modifiersFromEvent(e);
      const send = (mod: ModuleCcallSurface, window: number) => {
        mod.ccall('em_x11_push_button_event', null,
          ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
          [X_ButtonRelease, window, x, y, x, y, 1, mods]);
      };
      if (this.activePointerGrab?.module) {
        send(this.activePointerGrab.module, this.activePointerGrab.window);
      }
      if (this.implicitDragModule && this.implicitDragModule !== this.activePointerGrab?.module) {
        send(this.implicitDragModule, 0);
      }
    });
    on(el, 'mousemove', (ev) => {
      const e = ev as MouseEvent;
      const { x, y } = this.cssPoint(e, el);
      this.pushMouseMove({ x, y, modifiers: modifiersFromEvent(e) });
    });
    on(el, 'contextmenu', (e) => e.preventDefault());
    /* Scroll wheel → X11 Button4/Button5. Must be non-passive so
     * preventDefault stops the browser from scrolling the page. */
    const onWheel = (ev: Event): void => {
      const e = ev as WheelEvent;
      if (e.deltaY === 0) return;
      e.preventDefault();
      const { x, y } = this.cssPoint(e, el);
      this.pushWheel({ x, y, modifiers: modifiersFromEvent(e), deltaY: e.deltaY });
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    this.domListeners.push({ target: el, type: 'wheel', handler: onWheel });
    /* Click-to-focus. If the textarea overlay is currently armed for IME
     * (host has called XSetICFocus on a Tk widget that wants text input),
     * keep DOM focus on the overlay so the OS IME stays attached -- moving
     * focus to the canvas blanks the IME and then the next click anywhere
     * fails to bring it back (canvas isn't an editable surface). When no
     * overlay is armed, focus the canvas as before so plain keydown
     * routes work for non-text demos. */
    on(el, 'mousedown', (ev) => {
      const ov = this.host.textInput.element;
      const armed = ov !== null;
      if (armed && document.activeElement === ov) {
        /* Textarea already holds DOM focus and the OS IME has anchored
         * its per-element state (Chinese mode, candidate window, etc.)
         * to it. Browser default mousedown would focus the canvas
         * (tabIndex=0), blurring the textarea -- and on Windows the
         * subsequent ov.focus() restores DOM focus but resets the IME
         * to default English. preventDefault keeps the textarea
         * focused, preserving IME state across same-widget reclicks. */
        (ev as MouseEvent).preventDefault();
      } else if (armed) {
        try {
          (ov.focus as (opts?: { preventScroll?: boolean }) => void)({
            preventScroll: true,
          });
        } catch {
          ov.focus();
        }
      } else {
        el.focus();
        /* Defensive re-focus: the synchronous wasm ButtonPress processing
         * in the earlier mousedown handler may schedule microtasks that
         * move DOM focus (e.g. a Motif XmProcessTraversal that creates a
         * hidden IME textarea).  Re-assert canvas focus after the current
         * event drains. */
        setTimeout(() => {
          if (this.host.canvas.headless) return;
          const cur = document.activeElement;
          const tio = this.host.textInput.element;
          if (cur !== el && (tio === null || cur !== tio)) {
            el.focus();
          }
        }, 0);
      }
    });

    /* Browser → Tk clipboard: the C-side XConvertSelection now does a
     * JSPI await navigator.clipboard.readText() directly — no pre-stage
     * needed. Just push the key; the wasm event pump suspends until the
     * permission prompt (if any) is answered and the text arrives. */
    on(window, 'keydown', (ev) => {
      const e = ev as KeyboardEvent;
      const active = document.activeElement;
      const ov = this.host.textInput.element;
      const hasFocus = active === el || (ov !== null &&
                                         active === ov);
      /* Composing: KeyboardEvent during IME composition carries
       * key='Process' / keyCode=229 / isComposing=true and contains no
       * useful info. The composed result arrives later as a
       * compositionend on the textarea overlay. Drop the keydown so we
       * don't synthesise a junk KeyPress -- and DO NOT preventDefault,
       * because Chromium/Windows treats preventDefault on a Process
       * keydown as "client handled it" and aborts the IME composition
       * before compositionstart can fire. */
      if (e.isComposing || e.key === 'Process') return;
      if (hasFocus) e.preventDefault();
      /* Single printable: carry the UTF-8 byte(s) so Xutf8LookupString
       * returns them. event.key already reflects the keyboard layout
       * and Shift state ('a' vs 'A'). Multi-codepoint keys (rare:
       * "FunctionMenuItem", emoji shortcuts) drop their text -- the
       * keysym path stays. */
      const text = e.key.length === 1 ? e.key : '';
      const data: KeyEventData = {
        keysym: keyEventToKeysym(e),
        keycode: keyEventToKeycode(e),
        modifiers: modifiersFromEvent(e),
        hasFocus,
        text,
      };
      this.pushKeyDown(data);
    });
    on(window, 'keyup', (ev) => {
      const e = ev as KeyboardEvent;
      const active = document.activeElement;
      const ov = this.host.textInput.element;
      const hasFocus = active === el || (ov !== null &&
                                         active === ov);
      if (e.isComposing || e.key === 'Process') return;
      if (hasFocus) e.preventDefault();
      this.pushKeyUp({
        keysym: keyEventToKeysym(e),
        keycode: keyEventToKeycode(e),
        modifiers: modifiersFromEvent(e),
        hasFocus,
        text: '',
      });
    });
  }

  /** Detach every DOM listener attachDom() registered. Idempotent;
   *  callers (HMR teardown, multi-Host test harnesses) invoke after
   *  the owning Host is no longer used so the listener closures stop
   *  pinning the previous `this`. */
  dispose(): void {
    for (const { target, type, handler } of this.domListeners) {
      target.removeEventListener(type, handler);
    }
    this.domListeners = [];
  }

  private cssPoint(e: MouseEvent, el: HTMLCanvasElement): { x: number; y: number } {
    const rect = el.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  }
}

/** Ctrl+V / Cmd+V / Shift+Insert — the three combos that should trigger
 *  a clipboard prefetch. KeyboardEvent.code is layout-independent for
 *  the V key; modifier check uses ctrlKey OR metaKey to cover Linux/
 *  Windows (Ctrl) and macOS (Cmd) without false positives. */
