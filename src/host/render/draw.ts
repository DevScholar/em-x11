/**
 * GC drawing primitives. Mirrors xserver/mi/ software rendering
 * (mifillrct.c, mipoly.c, mipolyline.c, miarc.c, miimage.c) plus the
 * window↔drawable copy paths from xserver/dix/dispatch.c (CopyArea,
 * PutImage). All ops write to the destination window's per-window
 * backing surface in window-local coordinates and mark the renderer
 * dirty; the compositor (host/render/compositor.ts) reads each
 * backing on rAF and produces the root canvas with stack order,
 * clip and shape applied. Sibling occlusion and shape are NOT
 * applied at draw time -- they're the compositor's job.
 */

import type { RendererState, ManagedWindow } from './types.js';
import type { Point } from '../../types/emscripten.js';
import { getDebugFlags } from '../../runtime/debug-flags.js';
import { paintBackgroundIntoBacking } from './paint.js';
import { pixelToCssColor } from '../../runtime/canvas.js';
import {
  subtract as regionSubtract,
  isEmpty as regionIsEmpty,
} from './region.js';

/** Subtract the local rect from the window's unpaintedRegion. Each
 *  draw primitive calls this with its bounding box (clamped to the
 *  window's content rect) so the next paintExposedRegions knows that
 *  area now has client content -- no Expose needed when occluder
 *  later moves away. Negative or out-of-bounds rects are normalised
 *  to the content rect; out-of-bounds entirely is a no-op. */
function markPainted(
  win: ManagedWindow,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  if (regionIsEmpty(win.unpaintedRegion)) return;
  /* Clamp to content rect so we don't accidentally subtract bogus
   * rects. */
  const lx = Math.max(0, Math.floor(x));
  const ly = Math.max(0, Math.floor(y));
  const rx = Math.min(win.width, Math.ceil(x + w));
  const ry = Math.min(win.height, Math.ceil(y + h));
  if (rx <= lx || ry <= ly) return;
  win.unpaintedRegion = regionSubtract(
    win.unpaintedRegion,
    [{ ax: lx, ay: ly, w: rx - lx, h: ry - ly }],
  );
}

/** Backing-write helper: invoke `fn` on the window's backing context
 *  with save/restore around it, mark dirty, request a re-compose, and
 *  subtract the painted bbox from the window's unpaintedRegion. */
function paintBacking(
  r: RendererState,
  win: ManagedWindow,
  bx: number,
  by: number,
  bw: number,
  bh: number,
  fn: (ctx: OffscreenCanvasRenderingContext2D) => void,
): void {
  const ctx = win.backingCtx;
  ctx.save();
  fn(ctx);
  ctx.restore();
  win.backingDirty = true;
  markPainted(win, bx, by, bw, bh);
  r.markDirty();
}

/** Conservative bbox of a stroked line of width `lw` between two
 *  points. Includes ±lw/2 padding so we cover the antialiased
 *  shoulder. */
function strokedLineBbox(
  x1: number, y1: number, x2: number, y2: number, lw: number,
): { x: number; y: number; w: number; h: number } {
  const pad = (lw + 1) >> 1;
  const x = Math.min(x1, x2) - pad;
  const y = Math.min(y1, y2) - pad;
  const w = Math.abs(x2 - x1) + 2 * pad + 1;
  const h = Math.abs(y2 - y1) + 2 * pad + 1;
  return { x, y, w, h };
}

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
  if (getDebugFlags()?.tracePaint) {
    console.log('[paint] clearArea', id, '(', x, y, w, h, ') parent=', win.parent);
  }
  paintBackgroundIntoBacking(r, win, x, y, w, h);
  r.markDirty();
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
  paintBacking(r, win, x, y, w, h, (bctx) => {
    bctx.fillStyle = pixelToCssColor(color);
    bctx.fillRect(x, y, w, h);
  });
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
    paintBacking(r, win, rx, ry, rw, rh, (bctx) => {
      bctx.fillStyle = pixelToCssColor(color);
      bctx.fillRect(rx, ry, rw, rh);
    });
  } else {
    const bb = strokedLineBbox(x1, y1, x2, y2, lw);
    paintBacking(r, win, bb.x, bb.y, bb.w, bb.h, (bctx) => {
      bctx.strokeStyle = pixelToCssColor(color);
      bctx.lineWidth = lw;
      bctx.beginPath();
      bctx.moveTo(x1, y1);
      bctx.lineTo(x2, y2);
      bctx.stroke();
    });
  }
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
  const lw = lineWidth || 1;
  const pad = (lw + 1) >> 1;
  paintBacking(r, win, x - pad, y - pad, w + 2 * pad, h + 2 * pad, (bctx) => {
    bctx.strokeStyle = pixelToCssColor(color);
    bctx.lineWidth = lw;
    arcPath(bctx, x, y, w, h, angle1, angle2);
    bctx.stroke();
  });
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
  paintBacking(r, win, x, y, w, h, (bctx) => {
    bctx.fillStyle = pixelToCssColor(color);
    arcPath(bctx, x, y, w, h, angle1, angle2);
    bctx.fill();
  });
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
  const first = points[0]!;
  let minX = first.x, minY = first.y, maxX = first.x, maxY = first.y;
  for (let i = 1; i < points.length; i++) {
    const p = points[i]!;
    if (p.x < minX) minX = p.x;
    else if (p.x > maxX) maxX = p.x;
    if (p.y < minY) minY = p.y;
    else if (p.y > maxY) maxY = p.y;
  }
  paintBacking(r, win, minX, minY, maxX - minX + 1, maxY - minY + 1, (bctx) => {
    bctx.fillStyle = pixelToCssColor(color);
    bctx.beginPath();
    bctx.moveTo(first.x, first.y);
    for (let i = 1; i < points.length; i++) {
      const p = points[i]!;
      bctx.lineTo(p.x, p.y);
    }
    bctx.closePath();
    bctx.fill();
  });
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
  const first = points[0]!;
  let minX = first.x, minY = first.y, maxX = first.x, maxY = first.y;
  for (let i = 1; i < points.length; i++) {
    const p = points[i]!;
    if (p.x < minX) minX = p.x;
    else if (p.x > maxX) maxX = p.x;
    if (p.y < minY) minY = p.y;
    else if (p.y > maxY) maxY = p.y;
  }
  paintBacking(r, win, minX, minY, maxX - minX + 1, maxY - minY + 1, (bctx) => {
    bctx.fillStyle = pixelToCssColor(color);
    for (const p of points) {
      bctx.fillRect(p.x, p.y, 1, 1);
    }
  });
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
  /* Pre-measure once on the backing ctx to derive the bbox (font is
   * the same as inside the paintBacking closure). */
  const bctxMeasure = win.backingCtx;
  bctxMeasure.save();
  bctxMeasure.font = font;
  bctxMeasure.textBaseline = 'alphabetic';
  bctxMeasure.textAlign = 'left';
  const metrics = bctxMeasure.measureText(text);
  bctxMeasure.restore();
  const ascent =
    metrics.fontBoundingBoxAscent ?? metrics.actualBoundingBoxAscent ?? 10;
  const descent =
    metrics.fontBoundingBoxDescent ?? metrics.actualBoundingBoxDescent ?? 2;
  const bx = Math.floor(x);
  const by = Math.floor(y - ascent);
  const bx2 = Math.ceil(x + metrics.width);
  const by2 = Math.ceil(y + descent);
  paintBacking(r, win, bx, by, bx2 - bx, by2 - by, (bctx) => {
    bctx.font = font;
    bctx.textBaseline = 'alphabetic';
    bctx.textAlign = 'left';
    /* Always clear the text background rect before drawing. Canvas
     * fillText anti-aliases every glyph; with source-over compositing
     * the partial-alpha edge pixels cannot be fully overwritten by a
     * subsequent fillText at the same position. Each re-draw on top of
     * the existing backing-store text darkens the fringe, and after a
     * few cycles of Enter↔Leave crossings or occluder-driven Expose
     * the text looks visibly bolder.
     *
     * Painter-model callers expect the bg to be clean before every
     * XDrawString; layer-model backing stores do not guarantee that --
     * the previous text pixels live on in the OffscreenCanvas until
     * actively overwritten. Always painting the bg rect makes text
     * drawing idempotent regardless of imageMode, at the cost of one
     * extra fillRect per XDrawString call. */
    bctx.fillStyle = pixelToCssColor(bgColor);
    bctx.fillRect(bx, by, bx2 - bx, by2 - by);
    bctx.fillStyle = pixelToCssColor(fgColor);
    bctx.fillText(text, x, y);
  });
}

/** XCopyArea source half: grab an (x,y,w,h) rectangle from the source
 *  window's backing surface and paint it into `dstCtx` at (dstX,dstY).
 *  Coords are window-local on the source side. Returns silently when
 *  the window is unknown or unmapped -- match X semantics of
 *  "unpainted source = zero-filled result" by leaving dstCtx alone
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
  dstCtx.drawImage(
    win.backingSurface as unknown as CanvasImageSource,
    srcX,
    srcY,
    w,
    h,
    dstX,
    dstY,
    w,
    h,
  );
}

/** XCopyArea destination half: draw an image source rectangle into the
 *  destination window's backing surface at window-local (dstX,dstY). */
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
  paintBacking(r, win, dstX, dstY, w, h, (bctx) => {
    bctx.drawImage(src, srcX, srcY, w, h, dstX, dstY, w, h);
  });
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
