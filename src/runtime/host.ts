/**
 * Global host singleton.
 *
 * Installed on `globalThis.__EMX11__` before the wasm module starts so the
 * C-side JS library functions (src/bindings/emx11.library.js) can reach
 * into the TS runtime without going through Emscripten's Module object.
 *
 * Separating host state from Module lets a single wasm binary share one
 * RootCanvas/Compositor across multiple Emscripten module instances
 * (relevant later for Pyodide + Tcl-Tk, which run in the same page).
 */

import { RootCanvas } from './canvas.js';
import type { RootCanvasOptions } from './canvas.js';
import { Compositor } from './compositor.js';
import type { EmX11Host, ShapeRect } from '../types/emscripten.js';

export type HostOptions = RootCanvasOptions;

export class Host implements EmX11Host {
  readonly canvas: RootCanvas;
  readonly compositor: Compositor;

  constructor(options: HostOptions = {}) {
    this.canvas = new RootCanvas(options);
    this.compositor = new Compositor(this.canvas);
  }

  install(): void {
    globalThis.__EMX11__ = this;
  }

  onInit(_screenWidth: number, _screenHeight: number): void {
    /* C expects us to adopt its idea of screen size, but the browser is the
     * authority. We ignore the hint and the C side will be corrected the
     * next time it queries through XDisplayWidth/Height once we wire that
     * back through. TODO: plumb the correction call. */
  }

  onWindowCreate(
    id: number,
    x: number,
    y: number,
    width: number,
    height: number,
    background: number,
  ): void {
    this.compositor.addWindow(id, x, y, width, height, background);
  }

  onWindowMap(id: number): void {
    this.compositor.mapWindow(id);
  }

  onWindowUnmap(id: number): void {
    this.compositor.unmapWindow(id);
  }

  onWindowDestroy(id: number): void {
    this.compositor.destroyWindow(id);
  }

  onFillRect(id: number, x: number, y: number, w: number, h: number, color: number): void {
    this.compositor.fillRect(id, x, y, w, h, color);
  }

  onDrawLine(
    id: number,
    x1: number,
    y1: number,
    x2: number,
    y2: number,
    color: number,
    lineWidth: number,
  ): void {
    this.compositor.drawLine(id, x1, y1, x2, y2, color, lineWidth);
  }

  onFlush(): void {
    /* No-op: the compositor already presents through requestAnimationFrame.
     * Kept as a hook for future synchronous-flush scenarios. */
  }

  onWindowShape(id: number, rects: ShapeRect[]): void {
    this.compositor.setWindowShape(id, rects);
  }
}
