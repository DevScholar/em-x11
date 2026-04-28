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

  constructor(options: RootCanvasOptions = {}) {
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

    const ctx = canvas.getContext('2d', { alpha: false });
    if (!ctx) {
      throw new Error('em-x11: 2D canvas context unavailable');
    }

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
