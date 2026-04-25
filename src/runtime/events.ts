/**
 * DOM -> X event bridge.
 *
 * Listens on the root canvas for mouse / keyboard events, performs hit
 * testing via the compositor to find the target X window, and calls into
 * the wasm module's exported emx11_push_*_event functions to enqueue
 * equivalent XEvent structures.
 *
 * Focus model (skeleton): the last window that received a mouse click is
 * the keyboard focus. Pointer motion goes to whichever window is under
 * the cursor at the moment the event fires. Real X focus policies
 * (FocusIn/FocusOut, grabs, click-to-focus vs follow-pointer) are a v2
 * concern.
 */

import type { Compositor } from './compositor.js';
import type { EmscriptenModule } from '../types/emscripten.js';
import { keyEventToKeysym, modifiersFromEvent } from './keymap.js';

const X_ButtonPress = 4;
const X_ButtonRelease = 5;
const X_KeyPress = 2;
const X_KeyRelease = 3;

export class EventBridge {
  private readonly canvas: HTMLCanvasElement;
  private readonly compositor: Compositor;
  private readonly module: EmscriptenModule;
  private focusedWindow: number | null = null;

  constructor(canvas: HTMLCanvasElement, compositor: Compositor, module: EmscriptenModule) {
    this.canvas = canvas;
    this.compositor = compositor;
    this.module = module;
    this.attach();
  }

  private attach(): void {
    this.canvas.addEventListener('mousedown', (e) => this.onMouseButton(e, X_ButtonPress));
    this.canvas.addEventListener('mouseup', (e) => this.onMouseButton(e, X_ButtonRelease));
    this.canvas.addEventListener('mousemove', (e) => this.onMouseMove(e));
    this.canvas.addEventListener('contextmenu', (e) => e.preventDefault());

    /* Keyboard events need a focused element; we give the canvas tabIndex=0
     * in RootCanvas but listen on the window so the user doesn't have to
     * click first. Preventing default on keys we handle avoids the browser
     * acting on e.g. Space/Backspace while the demo has focus. */
    window.addEventListener('keydown', (e) => this.onKey(e, X_KeyPress));
    window.addEventListener('keyup', (e) => this.onKey(e, X_KeyRelease));

    /* Clicking anywhere on the canvas gives it focus for key events. */
    this.canvas.addEventListener('mousedown', () => this.canvas.focus());
  }

  private cssPoint(e: MouseEvent): { x: number; y: number } {
    const rect = this.canvas.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  }

  private onMouseButton(e: MouseEvent, xType: number): void {
    const { x, y } = this.cssPoint(e);
    const win = this.compositor.findWindowAt(x, y);
    if (win === null) return;
    if (xType === X_ButtonPress) this.focusedWindow = win;
    this.module.ccall(
      'emx11_push_button_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
      [xType, win, x, y, x, y, e.button + 1, modifiersFromEvent(e)],
    );
  }

  private onMouseMove(e: MouseEvent): void {
    const { x, y } = this.cssPoint(e);
    const win = this.compositor.findWindowAt(x, y);
    if (win === null) return;
    this.module.ccall(
      'emx11_push_motion_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [win, x, y, x, y, modifiersFromEvent(e)],
    );
  }

  private onKey(e: KeyboardEvent, xType: number): void {
    if (this.focusedWindow === null) return;
    const keysym = keyEventToKeysym(e);
    if (keysym === 0) return;
    /* Prevent the browser from scrolling on Space, navigating on Backspace,
     * etc. once the canvas is focused. */
    if (document.activeElement === this.canvas) e.preventDefault();

    this.module.ccall(
      'emx11_push_key_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [xType, this.focusedWindow, keysym, modifiersFromEvent(e), 0, 0],
    );
  }
}
