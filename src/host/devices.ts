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
 *     window + canvas in the constructor. Demo / wacl-tk path.
 *   - Headless / OffscreenCanvas mode: attachment is skipped; caller
 *     is expected to call pushMouseDown/Up/Move/Key + setPointer with
 *     canvas-local coordinates. Used by the worker entry in
 *     pyodide-tk that relays events from the main thread.
 */

import type { Host } from './index.js';
import type { Point } from '../types/emscripten.js';
import type { ModuleCcallSurface } from './connection.js';
import { keyEventToKeysym, modifiersFromEvent } from '../runtime/keymap.js';
import {
  X_ButtonPress,
  X_ButtonRelease,
  X_KeyPress,
  X_KeyRelease,
} from './constants.js';

/** Plain-data shape used by both the DOM path (after rect translation)
 *  and the relay path (worker postMessage payload). All coords are
 *  canvas-local device pixels. */
export interface MouseEventData {
  x: number;
  y: number;
  button: number;     // X11 button: 1 left, 2 middle, 3 right
  modifiers: number;  // X11 modifier mask
}

export interface KeyEventData {
  keysym: number;
  modifiers: number;
  /** True when the canvas had keyboard focus (DOM path uses
   *  document.activeElement). Worker path can pass true unconditionally
   *  since the canvas is the only interactive surface in that frame. */
  hasFocus: boolean;
}

export class InputBridge {
  private pointerX: number;
  private pointerY: number;
  /** Last window that received a ButtonPress, by connection id. Key
   *  events route here until another ButtonPress moves focus. */
  private focusedWindow: number | null = null;
  /** Module that owns the current implicit pointer grab (set on ButtonPress,
   *  cleared on ButtonRelease). Used by onMouseMove to route motion events
   *  to the grab window even when the pointer is over empty canvas space. */
  private dragModule: ModuleCcallSurface | null = null;

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

  pushMouseMove(e: Omit<MouseEventData, 'button'>): void {
    this.setPointer(e.x, e.y);
    const win = this.host.renderer.findWindowAt(e.x, e.y);
    /* X11 implicit pointer grab (x11protocol.txt §523): once a button is
     * pressed, all Motion and ButtonRelease events route to the grabbing
     * client regardless of where the pointer moves. dragModule holds the
     * module that saw the ButtonPress, so route to it unconditionally
     * while a drag is in progress -- a TWM title-bar drag crosses over
     * xeyes' frame mid-drag, but Motion must still reach TWM, not xeyes. */
    const module = this.dragModule ?? (win !== null ? this.moduleForWindow(win) : null);
    if (!module) return;
    module.ccall(
      'emx11_push_motion_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [win ?? 0, e.x, e.y, e.x, e.y, e.modifiers],
    );
  }

  pushKey(xType: number, e: KeyEventData): void {
    if (this.focusedWindow === null) return;
    const module = this.moduleForWindow(this.focusedWindow);
    if (!module) return;
    if (e.keysym === 0) return;
    module.ccall(
      'emx11_push_key_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [xType, this.focusedWindow, e.keysym, e.modifiers, 0, 0],
    );
  }

  pushKeyDown(e: KeyEventData): void { this.pushKey(X_KeyPress, e); }
  pushKeyUp(e: KeyEventData): void { this.pushKey(X_KeyRelease, e); }

  /* -- internal --------------------------------------------------------- */

  private deliverButton(xType: number, e: MouseEventData): void {
    this.setPointer(e.x, e.y);
    const win = this.host.renderer.findWindowAt(e.x, e.y);
    if (win === null) return;
    const module = this.moduleForWindow(win);
    if (!module) return;
    if (xType === X_ButtonPress) {
      this.focusedWindow = win;
      this.dragModule = module;
    } else {
      this.dragModule = null;
    }
    module.ccall(
      'emx11_push_button_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
      [xType, win, e.x, e.y, e.x, e.y, e.button, e.modifiers],
    );
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

    /* Track the last-seen pointer position at the host level so polling
     * callers (XQueryPointer; xeyes uses this every 50ms via an Xt timer)
     * can read it without going through the event bridge's hit test.
     * We listen on `window` rather than the canvas so mouse motion
     * outside the canvas (over browser chrome, over another page region)
     * still updates the cached position -- pupils that track the mouse
     * off-canvas look better than pupils that freeze on exit. */
    window.addEventListener('mousemove', (e) => {
      const rect = el.getBoundingClientRect();
      this.setPointer(e.clientX - rect.left, e.clientY - rect.top);
    });

    el.addEventListener('mousedown', (e) => {
      const { x, y } = this.cssPoint(e, el);
      this.pushMouseDown({ x, y, button: e.button + 1, modifiers: modifiersFromEvent(e) });
    });
    // Listen on window so a ButtonRelease outside the canvas (pointer moved
    // off during a drag) still reaches the grab window via the C-side grab.
    window.addEventListener('mouseup', (e) => {
      const { x, y } = this.cssPoint(e, el);
      this.pushMouseUp({ x, y, button: e.button + 1, modifiers: modifiersFromEvent(e) });
    });
    el.addEventListener('mousemove', (e) => {
      const { x, y } = this.cssPoint(e, el);
      this.pushMouseMove({ x, y, modifiers: modifiersFromEvent(e) });
    });
    el.addEventListener('contextmenu', (e) => e.preventDefault());
    el.addEventListener('mousedown', () => el.focus());

    window.addEventListener('keydown', (e) => {
      const hasFocus = document.activeElement === el;
      if (hasFocus) e.preventDefault();
      this.pushKeyDown({
        keysym: keyEventToKeysym(e),
        modifiers: modifiersFromEvent(e),
        hasFocus,
      });
    });
    window.addEventListener('keyup', (e) => {
      const hasFocus = document.activeElement === el;
      if (hasFocus) e.preventDefault();
      this.pushKeyUp({
        keysym: keyEventToKeysym(e),
        modifiers: modifiersFromEvent(e),
        hasFocus,
      });
    });
  }

  private cssPoint(e: MouseEvent, el: HTMLCanvasElement): { x: number; y: number } {
    const rect = el.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  }
}
