/**
 * Fixed-size root canvas.
 *
 * All X windows paint onto a single <canvas>. The canvas stays at a fixed
 * logical size (default 1024x768) and centers in the viewport. Browser
 * window resize does NOT reflow the canvas -- the X screen is a virtual
 * display of constant dimensions.
 *
 * Physical pixels always equal CSS pixels (1:1). There is no
 * devicePixelRatio scaling, so on high-DPI screens the browser will
 * upscale the canvas, which may look softer than native but avoids
 * sub-pixel antialiasing artifacts at widget edges.
 *
 * Three construction modes:
 *
 *   1. Default DOM mode -- creates an HTMLCanvasElement in document and
 *      styles it (legacy demo path).
 *   2. `element: HTMLCanvasElement` -- adopts an existing canvas owned
 *      by the host page (tcldide, pyodide-tk main-thread path).
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
  /** Always 1 — no devicePixelRatio scaling. */
  readonly dpr: number = 1;
  /** True when running against an OffscreenCanvas. */
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
      this.headless = false;
      options.element.width = this.cssWidth;
      options.element.height = this.cssHeight;
      options.element.style.width = `${this.cssWidth}px`;
      options.element.style.height = `${this.cssHeight}px`;
      options.element.tabIndex = 0;
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

  /** Back-compat accessor. Throws in headless/worker mode. */
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
