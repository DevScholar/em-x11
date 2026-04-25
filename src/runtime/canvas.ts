/**
 * Fixed-size root canvas.
 *
 * Plan B architecture: all X windows paint onto a single <canvas>. For the
 * skeleton we keep the canvas at a fixed logical size (default 1024x768)
 * and center it in the viewport. Browser window resize does NOT reflow
 * the canvas -- the X screen is a virtual display of constant dimensions.
 *
 * Backing-store dimensions are scaled by devicePixelRatio so lines stay
 * sharp on HiDPI displays. CSS dimensions remain the logical size.
 */

export interface RootCanvasOptions {
  parent?: HTMLElement;
  width?: number;
  height?: number;
}

export class RootCanvas {
  readonly element: HTMLCanvasElement;
  readonly ctx: CanvasRenderingContext2D;
  readonly cssWidth: number;
  readonly cssHeight: number;
  private readonly dpr: number;

  constructor(options: RootCanvasOptions = {}) {
    const parent = options.parent ?? document.body;
    this.cssWidth = options.width ?? 1024;
    this.cssHeight = options.height ?? 768;
    this.dpr = window.devicePixelRatio || 1;

    const canvas = document.createElement('canvas');
    canvas.width = Math.round(this.cssWidth * this.dpr);
    canvas.height = Math.round(this.cssHeight * this.dpr);
    canvas.style.width = `${this.cssWidth}px`;
    canvas.style.height = `${this.cssHeight}px`;
    canvas.style.display = 'block';
    canvas.style.margin = '0 auto';
    canvas.style.touchAction = 'none';
    canvas.style.boxShadow = '0 4px 24px rgba(0, 0, 0, 0.5)';
    canvas.tabIndex = 0;
    parent.appendChild(canvas);

    const ctx = canvas.getContext('2d', { alpha: false });
    if (!ctx) {
      throw new Error('em-x11: 2D canvas context unavailable');
    }
    ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);

    this.element = canvas;
    this.ctx = ctx;
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
