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
import type { Point, ShapeRect } from '../types/emscripten.js';

interface ManagedWindow {
  id: number;
  x: number;
  y: number;
  width: number;
  height: number;
  background: number;
  /** When non-null, the window background is tiled with this pixmap's
   *  OffscreenCanvas instead of painted with `background`. Tile origin
   *  is the window's top-left (applied via ctx.translate at paint time). */
  backgroundPixmap: number | null;
  mapped: boolean;
  /** SHAPE bounding rectangles (window-local coords). `null` means
   *  unshaped -- the window is a plain rectangle of (width, height). */
  shape: ShapeRect[] | null;
}

/** Callback the Host supplies so the compositor can reach into the
 *  pixmap table without importing Host (which would be circular). */
type PixmapLookup = (id: number) => OffscreenCanvas | null;

export class Compositor {
  private readonly canvas: RootCanvas;
  private readonly pixmapLookup: PixmapLookup;
  private readonly windows = new Map<number, ManagedWindow>();
  private dirty = true;
  private rafScheduled = false;

  constructor(canvas: RootCanvas, pixmapLookup: PixmapLookup = () => null) {
    this.canvas = canvas;
    this.pixmapLookup = pixmapLookup;
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
      backgroundPixmap: null,
      mapped: false,
      shape: null,
    });
    this.markDirty();
  }

  setWindowBackgroundPixmap(id: number, pmId: number): void {
    const w = this.windows.get(id);
    if (!w) return;
    w.backgroundPixmap = pmId > 0 ? pmId : null;
    this.markDirty();
  }

  /** XClearWindow / XClearArea: repaint a window rectangle using whatever
   *  background the window currently has (solid or tile). Unlike
   *  `fillRect(id, ..., win.background)` this honours a bound pixmap. */
  clearArea(id: number, x: number, y: number, w: number, h: number): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped) return;
    const ctx = this.canvas.ctx;
    ctx.save();
    this.applyWindowClip(ctx, win);
    this.paintBackgroundRect(ctx, win, x, y, w, h);
    ctx.restore();
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

  drawArc(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
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
    this.arcPath(ctx, win.x + x, win.y + y, w, h, angle1, angle2);
    ctx.stroke();
    ctx.restore();
  }

  fillArc(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
    color: number,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped) return;
    const ctx = this.canvas.ctx;
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.fillStyle = pixelToCssColor(color);
    this.arcPath(ctx, win.x + x, win.y + y, w, h, angle1, angle2);
    ctx.fill();
    ctx.restore();
  }

  fillPolygon(
    id: number,
    points: Point[],
    _shape: number,
    _mode: number,
    color: number,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped || points.length < 3) return;
    const ctx = this.canvas.ctx;
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.fillStyle = pixelToCssColor(color);
    ctx.beginPath();
    const first = points[0]!;
    ctx.moveTo(win.x + first.x, win.y + first.y);
    for (let i = 1; i < points.length; i++) {
      const p = points[i]!;
      ctx.lineTo(win.x + p.x, win.y + p.y);
    }
    ctx.closePath();
    ctx.fill();
    ctx.restore();
  }

  drawPoints(id: number, points: Point[], _mode: number, color: number): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped || points.length === 0) return;
    const ctx = this.canvas.ctx;
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.fillStyle = pixelToCssColor(color);
    for (const p of points) {
      ctx.fillRect(win.x + p.x, win.y + p.y, 1, 1);
    }
    ctx.restore();
  }

  drawString(
    id: number,
    x: number,
    y: number,
    font: string,
    text: string,
    fgColor: number,
    bgColor: number,
    imageMode: boolean,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped || text.length === 0) return;
    const ctx = this.canvas.ctx;
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.font = font;
    ctx.textBaseline = 'alphabetic';
    ctx.textAlign = 'left';
    if (imageMode) {
      const metrics = ctx.measureText(text);
      /* Approximate the rectangle Xlib expects: advance width, and a
       * vertical extent from font ascent to descent. measureText gives
       * fontBoundingBoxAscent/Descent on modern browsers; fall back to
       * actualBoundingBox if absent. */
      const ascent =
        metrics.fontBoundingBoxAscent ?? metrics.actualBoundingBoxAscent ?? 10;
      const descent =
        metrics.fontBoundingBoxDescent ?? metrics.actualBoundingBoxDescent ?? 2;
      ctx.fillStyle = pixelToCssColor(bgColor);
      ctx.fillRect(
        win.x + x,
        win.y + y - ascent,
        metrics.width,
        ascent + descent,
      );
    }
    ctx.fillStyle = pixelToCssColor(fgColor);
    ctx.fillText(text, win.x + x, win.y + y);
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

  /** Build a canvas path for an X-semantics arc. Exposed as a free
   *  function below so Host can reuse it for pixmap drawing. */
  private arcPath(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1_64: number,
    angle2_64: number,
  ): void {
    arcPath(ctx, x, y, w, h, angle1_64, angle2_64);
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
      this.paintBackgroundRect(ctx, w, 0, 0, w.width, w.height);
      ctx.restore();
    }
    this.dirty = false;
  }

  /** Paint a (window-local) rectangle using the window's current
   *  background: solid colour, or tile pattern when backgroundPixmap is
   *  bound. Tile origin is the window's top-left, so moving the window
   *  shifts the tile with it (matching X semantics). Caller owns
   *  ctx.save/restore and clipping. */
  private paintBackgroundRect(
    ctx: CanvasRenderingContext2D,
    win: ManagedWindow,
    x: number,
    y: number,
    w: number,
    h: number,
  ): void {
    const pmId = win.backgroundPixmap;
    if (pmId !== null) {
      const pmCanvas = this.pixmapLookup(pmId);
      if (pmCanvas) {
        const pattern = ctx.createPattern(
          pmCanvas as unknown as CanvasImageSource,
          'repeat',
        );
        if (pattern) {
          /* Translate before filling so the pattern tiles from the
           * window's top-left, not the canvas origin. Canvas patterns
           * are transformed along with the ctx. */
          ctx.save();
          ctx.translate(win.x, win.y);
          ctx.fillStyle = pattern;
          ctx.fillRect(x, y, w, h);
          ctx.restore();
          return;
        }
      }
      /* Pixmap vanished or pattern build failed -- fall through to solid
       * fill so we never leave an unpainted hole. */
    }
    ctx.fillStyle = pixelToCssColor(win.background);
    ctx.fillRect(win.x + x, win.y + y, w, h);
  }
}

/** Build a canvas path for an X-semantics arc.
 *
 *  X arc arguments: (x, y, w, h) is the axis-aligned bounding box of the
 *  ellipse; angle1 is the start angle and angle2 is the extent, both in
 *  1/64ths of a degree, measured counterclockwise from 3 o'clock.
 *
 *  Canvas 2D ellipse arguments: centre + radii, angles in radians
 *  measured clockwise from 3 o'clock. We flip the sign on angles to
 *  switch rotational direction.
 *
 *  Exported so Host can paint arcs into pixmap OffscreenCanvases with the
 *  same semantics as the window path. */
export function arcPath(
  ctx: CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
  angle1_64: number,
  angle2_64: number,
): void {
  const cx = x + w / 2;
  const cy = y + h / 2;
  const rx = w / 2;
  const ry = h / 2;
  const toRad = Math.PI / (180 * 64);
  const start = -angle1_64 * toRad;
  const end = -(angle1_64 + angle2_64) * toRad;
  ctx.beginPath();
  ctx.ellipse(cx, cy, rx, ry, 0, start, end, angle2_64 > 0);
}
