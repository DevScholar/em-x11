/**
 * DOM -> X event bridge.
 *
 * Listens on the root canvas for mouse / keyboard events, performs hit
 * testing via the compositor to find the target X window, and calls into
 * the wasm module's exported emx11_push_*_event functions to enqueue
 * equivalent XEvent structures.
 */

import type { Compositor } from './compositor.js';
import type { EmscriptenModule } from '../types/emscripten.js';

const X_ButtonPress = 4;
const X_ButtonRelease = 5;

export class EventBridge {
  private readonly canvas: HTMLCanvasElement;
  private readonly compositor: Compositor;
  private readonly module: EmscriptenModule;

  constructor(canvas: HTMLCanvasElement, compositor: Compositor, module: EmscriptenModule) {
    this.canvas = canvas;
    this.compositor = compositor;
    this.module = module;
    this.attach();
  }

  private attach(): void {
    this.canvas.addEventListener('mousedown', (e) => this.onMouseButton(e, X_ButtonPress));
    this.canvas.addEventListener('mouseup', (e) => this.onMouseButton(e, X_ButtonRelease));
    this.canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  }

  private onMouseButton(e: MouseEvent, xType: number): void {
    const rect = this.canvas.getBoundingClientRect();
    const cssX = e.clientX - rect.left;
    const cssY = e.clientY - rect.top;
    const window = this.compositor.findWindowAt(cssX, cssY);
    if (window === null) return;

    this.module.ccall(
      'emx11_push_button_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
      [xType, window, cssX, cssY, cssX, cssY, e.button + 1, 0],
    );
  }
}
