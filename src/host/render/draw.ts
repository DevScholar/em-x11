/**
 * GC drawing primitives. Mirrors xserver/mi/ software rendering
 * (mifillrct.c, mipoly.c, mipolyline.c, miarc.c, miimage.c) plus the
 * window↔drawable copy paths from xserver/dix/dispatch.c (CopyArea,
 * PutImage). All ops resolve the destination drawable's clip via
 * applyWindowClip so children honour their parents' bounds even though
 * our renderer has no per-window backing pixmap.
 */

import type { RendererState } from './types.js';
import type { Point } from '../../types/emscripten.js';
import { absOrigin } from './window-tree.js';
import { applyWindowClip, paintBackgroundRect } from './paint.js';
import { pixelToCssColor } from '../../runtime/canvas.js';

/** XClearWindow / XClearArea: repaint a window rectangle using whatever
 *  background the window currently has (solid or tile). Unlike
 *  `fillRect(id, ..., win.background)` this honours a bound pixmap. */
export function clearArea(
  r: RendererState,
  id: number,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  const win = r.windows.get(id);
  if (!win || !win.mapped) return;
  if ((globalThis as { __EMX11_TRACE_PAINT__?: boolean }).__EMX11_TRACE_PAINT__) {
    console.log('[paint] clearArea', id, '(', x, y, w, h, ') parent=', win.parent);
  }
  const ctx = r.canvas.ctx;
  ctx.save();
  applyWindowClip(r, ctx, win);
  paintBackgroundRect(r, ctx, win, x, y, w, h);
  ctx.restore();
}

export function fillRect(
  r: RendererState,
  id: number,
  x: number,
  y: number,
  w: number,
  h: number,
  color: number,
): void {
  const win = r.windows.get(id);
  if (!win || !win.mapped) return;
  const ctx = r.canvas.ctx;
  const { ax, ay } = absOrigin(r, win);
  ctx.save();
  applyWindowClip(r, ctx, win);
  ctx.fillStyle = pixelToCssColor(color);
  ctx.fillRect(ax + x, ay + y, w, h);
  ctx.restore();
}

export function drawLine(
  r: RendererState,
  id: number,
  x1: number,
  y1: number,
  x2: number,
  y2: number,
  color: number,
  lineWidth: number,
): void {
  const win = r.windows.get(id);
  if (!win || !win.mapped) return;
  const ctx = r.canvas.ctx;
  const { ax, ay } = absOrigin(r, win);
  ctx.save();
  applyWindowClip(r, ctx, win);
  const lw = lineWidth || 1;
  /* X11 lines are Bresenham (no AA). Canvas stroke always antialiases,
   * which leaks partial-alpha into neighbouring columns/rows. With
   * source-over compositing the Leave-time bg overwrite can't fully
   * undo prior partial-alpha pixels, so each Enter/Leave or set/unset
   * cycle on Athena Command/Toggle accumulates an L-shaped residue
   * around the highlight rectangle. For axis-aligned segments (the
   * vast majority -- XDrawRectangle decomposes into 4 of them) we
   * sidestep AA entirely with fillRect. Diagonal lines still go
   * through stroke; they're rare in Xt/Xaw widgets. */
  if (x1 === x2 || y1 === y2) {
    ctx.fillStyle = pixelToCssColor(color);
    let rx: number, ry: number, rw: number, rh: number;
    if (y1 === y2) {
      rx = Math.min(x1, x2);
      rw = Math.abs(x2 - x1) + 1;
      ry = y1 - ((lw - 1) >> 1);
      rh = lw;
    } else {
      ry = Math.min(y1, y2);
      rh = Math.abs(y2 - y1) + 1;
      rx = x1 - ((lw - 1) >> 1);
      rw = lw;
    }
    ctx.fillRect(ax + rx, ay + ry, rw, rh);
  } else {
    ctx.strokeStyle = pixelToCssColor(color);
    ctx.lineWidth = lw;
    ctx.beginPath();
    ctx.moveTo(ax + x1, ay + y1);
    ctx.lineTo(ax + x2, ay + y2);
    ctx.stroke();
  }
  ctx.restore();
}

export function drawArc(
  r: RendererState,
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
  const win = r.windows.get(id);
  if (!win || !win.mapped) return;
  const ctx = r.canvas.ctx;
  const { ax, ay } = absOrigin(r, win);
  ctx.save();
  applyWindowClip(r, ctx, win);
  ctx.strokeStyle = pixelToCssColor(color);
  ctx.lineWidth = lineWidth || 1;
  arcPath(ctx, ax + x, ay + y, w, h, angle1, angle2);
  ctx.stroke();
  ctx.restore();
}

export function fillArc(
  r: RendererState,
  id: number,
  x: number,
  y: number,
  w: number,
  h: number,
  angle1: number,
  angle2: number,
  color: number,
): void {
  const win = r.windows.get(id);
  if (!win || !win.mapped) return;
  const ctx = r.canvas.ctx;
  const { ax, ay } = absOrigin(r, win);
  ctx.save();
  applyWindowClip(r, ctx, win);
  ctx.fillStyle = pixelToCssColor(color);
  arcPath(ctx, ax + x, ay + y, w, h, angle1, angle2);
  ctx.fill();
  ctx.restore();
}

export function fillPolygon(
  r: RendererState,
  id: number,
  points: Point[],
  _shape: number,
  _mode: number,
  color: number,
): void {
  const win = r.windows.get(id);
  if (!win || !win.mapped || points.length < 3) return;
  const ctx = r.canvas.ctx;
  const { ax, ay } = absOrigin(r, win);
  ctx.save();
  applyWindowClip(r, ctx, win);
  ctx.fillStyle = pixelToCssColor(color);
  ctx.beginPath();
  const first = points[0]!;
  ctx.moveTo(ax + first.x, ay + first.y);
  for (let i = 1; i < points.length; i++) {
    const p = points[i]!;
    ctx.lineTo(ax + p.x, ay + p.y);
  }
  ctx.closePath();
  ctx.fill();
  ctx.restore();
}

export function drawPoints(
  r: RendererState,
  id: number,
  points: Point[],
  _mode: number,
  color: number,
): void {
  const win = r.windows.get(id);
  if (!win || !win.mapped || points.length === 0) return;
  const ctx = r.canvas.ctx;
  const { ax, ay } = absOrigin(r, win);
  ctx.save();
  applyWindowClip(r, ctx, win);
  ctx.fillStyle = pixelToCssColor(color);
  for (const p of points) {
    ctx.fillRect(ax + p.x, ay + p.y, 1, 1);
  }
  ctx.restore();
}

export function drawString(
  r: RendererState,
  id: number,
  x: number,
  y: number,
  font: string,
  text: string,
  fgColor: number,
  bgColor: number,
  imageMode: boolean,
): void {
  const win = r.windows.get(id);
  if (!win || !win.mapped || text.length === 0) return;
  const ctx = r.canvas.ctx;
  const { ax, ay } = absOrigin(r, win);
  ctx.save();
  applyWindowClip(r, ctx, win);
  ctx.font = font;
  ctx.textBaseline = 'alphabetic';
  ctx.textAlign = 'left';
  if (imageMode) {
    const metrics = ctx.measureText(text);
    const ascent =
      metrics.fontBoundingBoxAscent ?? metrics.actualBoundingBoxAscent ?? 10;
    const descent =
      metrics.fontBoundingBoxDescent ?? metrics.actualBoundingBoxDescent ?? 2;
    ctx.fillStyle = pixelToCssColor(bgColor);
    /* Round outward to integer pixel grid. measureText/font metrics are
     * floats, and fillRect with fractional bounds applies sub-pixel AA
     * at the edges -- partial-alpha pixels that source-over compositing
     * cannot fully overwrite on the next paint. XawCommand's Set/Unset
     * cycle (LCD click-invert) repaints text-bg in alternating colours;
     * mismatched AA fringes accumulate as L-shaped residue at the
     * rectangle's corners. Snapping to integer + ceil-on-extents makes
     * each cycle's bg cover the previous cycle's full footprint. */
    const bx = Math.floor(ax + x);
    const by = Math.floor(ay + y - ascent);
    const bx2 = Math.ceil(ax + x + metrics.width);
    const by2 = Math.ceil(ay + y + descent);
    ctx.fillRect(bx, by, bx2 - bx, by2 - by);
  }
  ctx.fillStyle = pixelToCssColor(fgColor);
  ctx.fillText(text, ax + x, ay + y);
  ctx.restore();
}

/** XCopyArea source half: grab an (x,y,w,h) rectangle from the window's
 *  painted area on the root canvas and paint it into `dstCtx` at
 *  (dstX,dstY). Coords are window-local on the source side. Returns
 *  silently when the window is unknown or unmapped -- match X semantics
 *  of "unpainted source = zero-filled result" by leaving dstCtx alone
 *  (callers that care can clear first). */
export function blitWindowTo(
  r: RendererState,
  srcId: number,
  srcX: number,
  srcY: number,
  w: number,
  h: number,
  dstCtx: OffscreenCanvasRenderingContext2D,
  dstX: number,
  dstY: number,
): void {
  const win = r.windows.get(srcId);
  if (!win || !win.mapped) return;
  const { ax, ay } = absOrigin(r, win);
  dstCtx.drawImage(
    r.canvas.ctx.canvas as unknown as CanvasImageSource,
    ax + srcX,
    ay + srcY,
    w,
    h,
    dstX,
    dstY,
    w,
    h,
  );
}

/** XCopyArea destination half: draw an image source rectangle onto the
 *  root canvas clipped to the destination window. */
export function blitImageToWindow(
  r: RendererState,
  dstId: number,
  dstX: number,
  dstY: number,
  src: CanvasImageSource,
  srcX: number,
  srcY: number,
  w: number,
  h: number,
): void {
  const win = r.windows.get(dstId);
  if (!win || !win.mapped) return;
  const { ax, ay } = absOrigin(r, win);
  const ctx = r.canvas.ctx;
  ctx.save();
  applyWindowClip(r, ctx, win);
  ctx.drawImage(src, srcX, srcY, w, h, ax + dstX, ay + dstY, w, h);
  ctx.restore();
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
