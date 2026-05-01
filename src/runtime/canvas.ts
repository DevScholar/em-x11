/**
 * Fixed-size root canvas.
 *
 * Plan B architecture: all X windows paint onto a single <canvas>. For the
 * skeleton we keep the canvas at a fixed logical size (default 1024x768)
 * and center it in the viewport. Browser window resize does NOT reflow
 * the canvas -- the X screen is a virtual display of constant dimensions.
 *
 * 1:1 backing store (no DPR scaling). X11 has no concept of logical-vs-
 * device pixels: an X coordinate IS a device pixel. Scaling the canvas by
 * window.devicePixelRatio puts every X integer coord on a fractional
 * device-pixel boundary when DPR is non-integer (Windows 125%/150% are
 * common), and Canvas 2D antialiases fillRect/fillText at those edges.
 * Source-over compositing can't undo partial-alpha pixels, so any widget
 * that repaints in alternating colours (Athena Toggle's Set/Unset cycle
 * on XCalc's LCD) accumulates an L-shaped ghost at rectangle corners. A
 * 1:1 backing store snaps every X paint to whole device pixels, matching
 * the semantics real X11 servers expose to clients.
 *
 * Three construction modes:
 *
 *   1. Default DOM mode -- creates an HTMLCanvasElement in document and
 *      styles it (legacy demo path).
 *   2. `element: HTMLCanvasElement` -- adopts an existing canvas owned
 *      by the host page (wacl-tk, pyodide-tk main-thread path).
 *   3. `surface: OffscreenCanvas` -- worker mode. No DOM is touched.
 *      Caller must own input event delivery via host.devices.push*().
 */

export interface RootCanvasOptions {
  parent?: HTMLElement;
  width?: number;
  height?: number;
  /** Use this existing <canvas> instead of creating one. Width/height
   *  default to its current attribute size. Mirrors Pyodide's
   *  pyodide.canvas.setCanvas2D(canvas) opt-in: the host page owns
   *  layout, em-x11 just paints into the surface it's handed. */
  element?: HTMLCanvasElement;
  /** Worker / OffscreenCanvas mode. When provided, em-x11 runs entirely
   *  off the main thread; no `document` / `window` access is performed.
   *  Width/height MUST be provided (OffscreenCanvas has no clientWidth).
   *  Caller is responsible for relaying input via host.devices.push*. */
  surface?: OffscreenCanvas;
}

export type RootCanvasSurface = HTMLCanvasElement | OffscreenCanvas;
export type RootCanvasContext =
  | CanvasRenderingContext2D
  | OffscreenCanvasRenderingContext2D;

export class RootCanvas {
  readonly surface: RootCanvasSurface;
  readonly ctx: RootCanvasContext;
  readonly cssWidth: number;
  readonly cssHeight: number;
  /** True when running against an OffscreenCanvas. DOM access is then
   *  unavailable (we may be in a Worker). InputBridge consults this to
   *  skip its addEventListener path. */
  readonly headless: boolean;

  constructor(options: RootCanvasOptions = {}) {
    if (options.surface) {
      this.surface = options.surface;
      this.cssWidth = options.width ?? options.surface.width ?? 1024;
      this.cssHeight = options.height ?? options.surface.height ?? 768;
      options.surface.width = this.cssWidth;
      options.surface.height = this.cssHeight;
      this.headless = true;
    } else if (options.element) {
      this.surface = options.element;
      this.cssWidth = options.width ?? (options.element.width || 1024);
      this.cssHeight = options.height ?? (options.element.height || 768);
      options.element.width = this.cssWidth;
      options.element.height = this.cssHeight;
      this.headless = false;
    } else {
      const parent = options.parent ?? document.body;
      this.cssWidth = options.width ?? 1024;
      this.cssHeight = options.height ?? 768;
      const canvas = document.createElement('canvas');
      canvas.width = this.cssWidth;
      canvas.height = this.cssHeight;
      canvas.style.width = `${this.cssWidth}px`;
      canvas.style.height = `${this.cssHeight}px`;
      canvas.style.display = 'block';
      canvas.style.margin = '0 auto';
      canvas.style.touchAction = 'none';
      canvas.style.boxShadow = '0 4px 24px rgba(0, 0, 0, 0.5)';
      canvas.tabIndex = 0;
      parent.appendChild(canvas);
      this.surface = canvas;
      this.headless = false;
    }

    const ctx = this.surface.getContext('2d', { alpha: false });
    if (!ctx) {
      throw new Error('em-x11: 2D canvas context unavailable');
    }
    this.ctx = ctx as RootCanvasContext;
  }

  /** Back-compat accessor. Existing demos read `host.canvas.element` to
   *  attach DOM listeners themselves; throws in headless/worker mode so
   *  callers get a clear error rather than a silent undefined. */
  get element(): HTMLCanvasElement {
    if (this.headless) {
      throw new Error(
        'em-x11: canvas.element unavailable in OffscreenCanvas mode -- use host.devices.push* to feed input',
      );
    }
    return this.surface as HTMLCanvasElement;
  }

  clear(color = '#000'): void {
    this.ctx.fillStyle = color;
    this.ctx.fillRect(0, 0, this.cssWidth, this.cssHeight);
  }
}

export function pixelToCssColor(pixel: number): string {
  const r = (pixel >> 16) & 0xff;
  const g = (pixel >> 8) & 0xff;
  const b = pixel & 0xff;
  return `rgb(${r},${g},${b})`;
}
