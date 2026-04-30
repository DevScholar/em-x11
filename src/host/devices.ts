/**
 * Input bridge. Translates browser DOM mouse/keyboard events into the
 * X event types the C side expects, and routes each one to the right
 * wasm Module via ccall. Mirrors xserver/dix/devices.c + dix/events.c
 * pointer/key delivery, minus the proper grab semantics (we have a
 * coarse "implicit grab on ButtonPress" approximation, no XGrabPointer).
 */

import type { Host } from './index.js';
import type { EmscriptenModule, Point } from '../types/emscripten.js';
import { keyEventToKeysym, modifiersFromEvent } from '../runtime/keymap.js';
import {
  X_ButtonPress,
  X_ButtonRelease,
  X_KeyPress,
  X_KeyRelease,
} from './constants.js';

export class InputBridge {
  private pointerX: number;
  private pointerY: number;
  /** Last window that received a ButtonPress, by connection id. Key
   *  events route here until another ButtonPress moves focus. */
  private focusedWindow: number | null = null;
  /** Module that owns the current implicit pointer grab (set on ButtonPress,
   *  cleared on ButtonRelease). Used by onMouseMove to route motion events
   *  to the grab window even when the pointer is over empty canvas space. */
  private dragModule: EmscriptenModule | null = null;

  constructor(private readonly host: Host) {
    /* Default the pointer to the canvas centre so the first XQueryPointer
     * after XtRealizeWidget -- which fires before the user has had a
     * chance to move the mouse -- returns something sensible instead of
     * the top-left corner. xeyes in particular snaps its pupils here
     * immediately. */
    this.pointerX = (this.host.canvas.element.clientWidth / 2) | 0;
    this.pointerY = (this.host.canvas.element.clientHeight / 2) | 0;

    /* Track the last-seen pointer position at the host level so polling
     * callers (XQueryPointer; xeyes uses this every 50ms via an Xt timer)
     * can read it without going through the event bridge's hit test.
     * We listen on `window` rather than the canvas so mouse motion
     * outside the canvas (over browser chrome, over another page region)
     * still updates the cached position -- pupils that track the mouse
     * off-canvas look better than pupils that freeze on exit. */
    window.addEventListener('mousemove', (e) => {
      const rect = this.host.canvas.element.getBoundingClientRect();
      this.pointerX = (e.clientX - rect.left) | 0;
      this.pointerY = (e.clientY - rect.top) | 0;
    });

    this.attach();
  }

  getPointerXY(): Point {
    return { x: this.pointerX, y: this.pointerY };
  }

  private attach(): void {
    const el = this.host.canvas.element;
    el.addEventListener('mousedown', (e) => this.onMouseButton(e, X_ButtonPress));
    // Listen on window so a ButtonRelease outside the canvas (pointer moved
    // off during a drag) still reaches the grab window via the C-side grab.
    window.addEventListener('mouseup', (e) => this.onMouseButton(e, X_ButtonRelease));
    el.addEventListener('mousemove', (e) => this.onMouseMove(e));
    el.addEventListener('contextmenu', (e) => e.preventDefault());
    el.addEventListener('mousedown', () => el.focus());

    window.addEventListener('keydown', (e) => this.onKey(e, X_KeyPress));
    window.addEventListener('keyup', (e) => this.onKey(e, X_KeyRelease));
  }

  private cssPoint(e: MouseEvent): { x: number; y: number } {
    const rect = this.host.canvas.element.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
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
  private moduleForWindow(winId: number): EmscriptenModule | null {
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

  private onMouseButton(e: MouseEvent, xType: number): void {
    const { x, y } = this.cssPoint(e);
    const win = this.host.renderer.findWindowAt(x, y);
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
      [xType, win, x, y, x, y, e.button + 1, modifiersFromEvent(e)],
    );
  }

  private onMouseMove(e: MouseEvent): void {
    const { x, y } = this.cssPoint(e);
    const win = this.host.renderer.findWindowAt(x, y);
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
      [win ?? 0, x, y, x, y, modifiersFromEvent(e)],
    );
  }

  private onKey(e: KeyboardEvent, xType: number): void {
    if (this.focusedWindow === null) return;
    const module = this.moduleForWindow(this.focusedWindow);
    if (!module) return;
    const keysym = keyEventToKeysym(e);
    if (keysym === 0) return;
    if (document.activeElement === this.host.canvas.element) e.preventDefault();

    module.ccall(
      'emx11_push_key_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [xType, this.focusedWindow, keysym, modifiersFromEvent(e), 0, 0],
    );
  }
}
