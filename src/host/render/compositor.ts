/**
 * Compositor: per-window backing → root canvas blender.
 *
 * Walks the window tree DFS, parent-before-children, siblings in
 * stackOrder ascending. For each mapped window paints (a) its
 * server-drawn border ring in parent-clip area, (b) its backing
 * surface clipped to a "composite clip" = window's bounding (∩ shape
 * if shaped) ∩ ancestor chain.
 *
 * Why NOT use `clipList`: clipList has SIBLING-OCCLUDER subtraction
 * baked in (correct for application drawing semantics — a draw inside
 * a window must not paint where higher-z siblings cover). For the
 * compositor we want strict z-order overdraw without subtracting
 * sibling areas: parent paints first, children/higher-z siblings
 * paint over it. Subtracting sibling area would leave gaps wherever
 * a child has bgType='none' (transparent backing) — twm's frames
 * are bgType='none', so the area between xeyes' eyes (frame-clip
 * passes through, shell.shape excludes it from shell) ends up with
 * canvas-clear-colour bleeding through if the frame's clipList
 * subtracted the shell's bounds.
 *
 * SHAPE: a shaped window's compositeClip uses its shape rects (in
 * absolute coords) instead of its bounding rect. Shape-outside is
 * never painted by the shaped window; ancestors painted earlier
 * remain visible. This is what makes xeyes' shape-outside reveal
 * whatever's underneath (frame bg, lower siblings, root).
 *
 * Scheduling: `markDirty()` requests a re-compose at the next rAF.
 * Multiple markDirty calls coalesce.
 */

import type { RendererState, ManagedWindow } from './types.js';
import { absOrigin } from './window-tree.js';
import { pixelToCssColor, type RootCanvasContext } from '../../runtime/canvas.js';
import {
  EMPTY_REGION,
  intersect as regionIntersect,
  isEmpty as regionIsEmpty,
  type Region,
  type Rect,
} from './region.js';

/** Mutable scheduling state hung off the renderer. */
export interface CompositorState {
  rafHandle: number | null;
  composing: boolean;
}

export function createCompositorState(): CompositorState {
  return { rafHandle: null, composing: false };
}

/** Request a fresh compose at next rAF. Idempotent within a frame. */
export function markDirty(r: RendererState, cs: CompositorState): void {
  if (cs.composing) return;
  if (cs.rafHandle !== null) return;
  cs.rafHandle = requestAnimationFrame(() => {
    cs.rafHandle = null;
    composeNow(r, cs);
  });
}

/** Synchronous compose. Cancels any pending rAF. */
export function composeNow(r: RendererState, cs: CompositorState): void {
  if (cs.rafHandle !== null) {
    cancelAnimationFrame(cs.rafHandle);
    cs.rafHandle = null;
  }
  cs.composing = true;
  try {
    compose(r);
  } finally {
    cs.composing = false;
  }
}

function compose(r: RendererState): void {
  const ctx = r.canvas.ctx;
  const w = r.canvas.surface.width;
  const h = r.canvas.surface.height;
  ctx.save();
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, w, h);
  ctx.restore();

  const fullCanvas: Region = [{ ax: 0, ay: 0, w, h }];
  /* parent===0 mapped windows are top-level. The X "root window"
   * itself sits among them; iteration in stackOrder ascending puts
   * lower-z first, higher-z later (over). */
  const roots = sortedMappedChildren(r, 0);
  for (const root of roots) {
    walk(r, ctx, root, fullCanvas);
  }
}

function walk(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
  parentClip: Region,
): void {
  if (!win.mapped) return;
  /* The window's "paint shape" in absolute coords: bounding rect
   * intersected with shape rects when shaped. Border ring lives
   * OUTSIDE this region (in parent-clip area only). */
  const myShape = paintShape(r, win);
  const myCompositeClip = regionIntersect(myShape, parentClip);
  /* Border ring is allowed to paint anywhere inside parent-clip that
   * lies between the window's bounding outer edge and content rect.
   * No sibling-occluder subtraction (z-order overdraw handles it). */
  if (win.borderWidth > 0) {
    paintBorderRing(r, ctx, win, parentClip);
  }
  if (!regionIsEmpty(myCompositeClip)) {
    const { ax, ay } = absOrigin(r, win);
    ctx.save();
    ctx.beginPath();
    for (const rc of myCompositeClip) ctx.rect(rc.ax, rc.ay, rc.w, rc.h);
    ctx.clip();
    ctx.drawImage(win.backingSurface as unknown as CanvasImageSource, ax, ay);
    ctx.restore();
  }
  /* Recurse: children paint over their parent's content, with
   * myCompositeClip as their ancestor cone. */
  for (const child of sortedMappedChildren(r, win.id)) {
    walk(r, ctx, child, myCompositeClip);
  }
}

/** Window's paint area in absolute coords: content rect, shape-clipped
 *  if the window is shaped. Used as the "what pixels can this window
 *  contribute" set, intersected with the ancestor-clip cone at compose
 *  time. */
function paintShape(r: RendererState, win: ManagedWindow): Region {
  const { ax, ay } = absOrigin(r, win);
  const contentRect: Rect = { ax, ay, w: win.width, h: win.height };
  if (!win.shape) return [contentRect];
  const shapeAbs: Region = win.shape.map((s) => ({
    ax: ax + s.x,
    ay: ay + s.y,
    w: s.w,
    h: s.h,
  }));
  return regionIntersect(shapeAbs, [contentRect]);
}

function paintBorderRing(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
  parentClip: Region,
): void {
  if (regionIsEmpty(parentClip)) return;
  const { ax, ay } = absOrigin(r, win);
  const bw = win.borderWidth;
  ctx.save();
  /* Clip to parent's clip (ancestor cone). The four-strip math
   * naturally limits the paint to the ring (outside content rect). */
  ctx.beginPath();
  for (const rc of parentClip) ctx.rect(rc.ax, rc.ay, rc.w, rc.h);
  ctx.clip();
  ctx.fillStyle = pixelToCssColor(win.borderPixel);
  ctx.fillRect(ax - bw, ay - bw, win.width + 2 * bw, bw);
  ctx.fillRect(ax - bw, ay + win.height, win.width + 2 * bw, bw);
  ctx.fillRect(ax - bw, ay, bw, win.height);
  ctx.fillRect(ax + win.width, ay, bw, win.height);
  ctx.restore();
}

function sortedMappedChildren(r: RendererState, parentId: number): ManagedWindow[] {
  const out: ManagedWindow[] = [];
  for (const w of r.windows.values()) {
    if (w.parent === parentId && w.mapped) out.push(w);
  }
  out.sort((a, b) => a.stackOrder - b.stackOrder);
  return out;
}

void EMPTY_REGION;
