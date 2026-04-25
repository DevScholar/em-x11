/**
 * Software compositor.
 *
 * Owns the list of mapped X windows, their positions, sizes, and per-window
 * pixel buffers. On each frame it composites them onto the RootCanvas in
 * z-order. A dirty-rectangle set is maintained so we don't repaint the
 * whole screen on every draw call.
 *
 * The skeleton's compositor is intentionally minimal: one top-level window
 * fills from a single pixel buffer, no z-order shuffling, no clipping of
 * overlapping windows. v1 will flesh this out to match X semantics.
 */

import type { RootCanvas } from './canvas.js';
import { pixelToCssColor } from './canvas.js';
import type { ShapeRect } from '../types/emscripten.js';

interface ManagedWindow {
  id: number;
  x: number;
  y: number;
  width: number;
  height: number;
  background: number;
  mapped: boolean;
  /** SHAPE bounding rectangles (window-local coords). `null` means
   *  unshaped -- the window is a plain rectangle of (width, height). */
  shape: ShapeRect[] | null;
}

export class Compositor {
  private readonly canvas: RootCanvas;
  private readonly windows = new Map<number, ManagedWindow>();
  private dirty = true;
  private rafScheduled = false;

  constructor(canvas: RootCanvas) {
    this.canvas = canvas;
  }

  addWindow(
    id: number,
    x: number,
    y: number,
    width: number,
    height: number,
    background: number,
  ): void {
    this.windows.set(id, {
      id,
      x,
      y,
      width,
      height,
      background,
      mapped: false,
      shape: null,
    });
    this.markDirty();
  }

  setWindowShape(id: number, rects: ShapeRect[]): void {
    const w = this.windows.get(id);
    if (!w) return;
    w.shape = rects.length > 0 ? rects : null;
    this.markDirty();
  }

  mapWindow(id: number): void {
    const w = this.windows.get(id);
    if (!w) return;
    w.mapped = true;
    this.markDirty();
  }

  unmapWindow(id: number): void {
    const w = this.windows.get(id);
    if (!w) return;
    w.mapped = false;
    this.markDirty();
  }

  destroyWindow(id: number): void {
    this.windows.delete(id);
    this.markDirty();
  }

  /** Immediate-mode fill. In the skeleton we paint straight onto the root
   *  canvas, clipped to the window rectangle (and its shape, if any).
   *  Real implementation will go through per-window offscreen buffers +
   *  deferred compositing. */
  fillRect(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    color: number,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped) return;
    const ctx = this.canvas.ctx;
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.fillStyle = pixelToCssColor(color);
    ctx.fillRect(win.x + x, win.y + y, w, h);
    ctx.restore();
  }

  drawLine(
    id: number,
    x1: number,
    y1: number,
    x2: number,
    y2: number,
    color: number,
    lineWidth: number,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped) return;
    const ctx = this.canvas.ctx;
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.strokeStyle = pixelToCssColor(color);
    ctx.lineWidth = lineWidth || 1;
    ctx.beginPath();
    ctx.moveTo(win.x + x1 + 0.5, win.y + y1 + 0.5);
    ctx.lineTo(win.x + x2 + 0.5, win.y + y2 + 0.5);
    ctx.stroke();
    ctx.restore();
  }

  findWindowAt(cssX: number, cssY: number): number | null {
    /* Naive hit test in insertion order (top-most last). Honours SHAPE:
     * a point outside the shape rectangles does not count as a hit. v1
     * will maintain an explicit z-order and honour override_redirect +
     * InputOnly semantics. */
    let hit: number | null = null;
    for (const w of this.windows.values()) {
      if (!w.mapped) continue;
      if (
        cssX < w.x ||
        cssX >= w.x + w.width ||
        cssY < w.y ||
        cssY >= w.y + w.height
      ) {
        continue;
      }
      if (w.shape) {
        const lx = cssX - w.x;
        const ly = cssY - w.y;
        let inside = false;
        for (const r of w.shape) {
          if (lx >= r.x && lx < r.x + r.w && ly >= r.y && ly < r.y + r.h) {
            inside = true;
            break;
          }
        }
        if (!inside) continue;
      }
      hit = w.id;
    }
    return hit;
  }

  /** Push a clip region matching the window's visible area onto `ctx`.
   *  Caller must have done ctx.save() and must ctx.restore() after. */
  private applyWindowClip(ctx: CanvasRenderingContext2D, win: ManagedWindow): void {
    ctx.beginPath();
    if (win.shape) {
      for (const r of win.shape) {
        ctx.rect(win.x + r.x, win.y + r.y, r.w, r.h);
      }
    } else {
      ctx.rect(win.x, win.y, win.width, win.height);
    }
    ctx.clip();
  }

  private markDirty(): void {
    this.dirty = true;
    if (!this.rafScheduled) {
      this.rafScheduled = true;
      requestAnimationFrame(() => this.present());
    }
  }

  private present(): void {
    this.rafScheduled = false;
    if (!this.dirty) return;
    /* Skeleton: background-fill the viewport, then let subsequent drawing
     * calls paint into clipped regions. Dirty-rect tracking comes with the
     * full backing-store implementation. */
    this.canvas.clear('#111');
    for (const w of this.windows.values()) {
      if (!w.mapped) continue;
      const ctx = this.canvas.ctx;
      ctx.save();
      this.applyWindowClip(ctx, w);
      ctx.fillStyle = pixelToCssColor(w.background);
      ctx.fillRect(w.x, w.y, w.width, w.height);
      ctx.restore();
    }
    this.dirty = false;
  }
}
