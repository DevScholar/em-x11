/**
 * Compositor: per-window backing → root canvas blender.
 *
 * Walks the window tree DFS bottom-to-top by stackOrder and for each
 * mapped window paints (a) its server-drawn border ring in parent
 * coords, (b) its backing surface clipped to `clipList`. Both clips
 * are absolute and pre-computed by `recomputeClipsAll` (mi/mivaltree.c
 * equivalent), which already accounts for ancestor occlusion, sibling
 * occlusion AND shape -- so the compositor doesn't re-do any of that
 * geometry. Just clip + drawImage.
 *
 * SHAPE live-through falls out for free: a shaped window's `clipList`
 * only covers shape-inside pixels, so the compositor blits its backing
 * into shape-inside only; shape-outside reveals whatever lower-z
 * siblings already painted underneath. xeyes-over-xcalc Just Works.
 *
 * Scheduling: `markDirty()` requests a re-compose at the next
 * animation frame. Multiple markDirty calls coalesce into one compose.
 * The compose target is the whole canvas; partial regions are not
 * worth tracking at our scale (~10 windows per demo).
 */

import type { RendererState, ManagedWindow } from './types.js';
import { absOrigin } from './window-tree.js';
import { pixelToCssColor, type RootCanvasContext } from '../../runtime/canvas.js';

/** Mutable scheduling state hung off the renderer. */
export interface CompositorState {
  rafHandle: number | null;
  /** True while a compose is mid-flight so a markDirty fired from
   *  inside paint code doesn't schedule a redundant pass. */
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

/** Synchronous compose. Used by callers that need pixels on the canvas
 *  before yielding to the browser (debug helpers, one-shot reads).
 *  Cancels any pending rAF. */
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
  /* Hard clear: every frame is a full repaint. Backing surfaces are
   * the source of truth; whatever WAS on root last frame is stale and
   * will be re-derived by the walk below. */
  ctx.save();
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, w, h);
  ctx.restore();

  /* Iterate roots (parent === 0) bottom-to-top so higher-z siblings
   * paint over lower ones. The X "root window" itself is one of these
   * (added by setup with parent=0 covering the full canvas), so its
   * backing carries the canvas-bg pixels naturally without a special
   * case here. */
  const roots = sortedMappedChildren(r, 0);
  for (const root of roots) compositeSubtree(r, ctx, root);
}

function compositeSubtree(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
): void {
  if (!win.mapped) return;
  /* Border first: ring sits OUTSIDE the content rect in parent coords.
   * borderClip - clipList = ring; borderClip is already occluder-minus. */
  if (win.borderWidth > 0 && win.borderClip.length > 0) {
    paintBorderRing(r, ctx, win);
  }
  /* Content: blit backing onto root, clipped to clipList (which
   * already contains shape ∩ ancestor ∩ sibling-occluder). */
  if (win.clipList.length > 0) {
    const { ax, ay } = absOrigin(r, win);
    ctx.save();
    ctx.beginPath();
    for (const rc of win.clipList) ctx.rect(rc.ax, rc.ay, rc.w, rc.h);
    ctx.clip();
    ctx.drawImage(win.backingSurface as unknown as CanvasImageSource, ax, ay);
    ctx.restore();
  }
  for (const child of sortedMappedChildren(r, win.id)) {
    compositeSubtree(r, ctx, child);
  }
}

function paintBorderRing(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
): void {
  const { ax, ay } = absOrigin(r, win);
  const bw = win.borderWidth;
  ctx.save();
  ctx.beginPath();
  for (const rc of win.borderClip) ctx.rect(rc.ax, rc.ay, rc.w, rc.h);
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
