/**
 * Internal paint helpers. Mirrors xserver/mi/mipaintwin.c (paintWindow,
 * miPaintWindow) plus the per-window clip stacking that real X does
 * implicitly through the per-window backing pixmap. We don't have backing
 * store, so every drawing op walks the parent chain to install one
 * canvas clip per ancestor.
 */

import type { RendererState, ManagedWindow } from './types.js';
import { absOrigin } from './window-tree.js';
import { pixelToCssColor, type RootCanvasContext } from '../../runtime/canvas.js';
import {
  EMPTY_REGION,
  extents as regionExtents,
  isEmpty as regionIsEmpty,
  subtract as regionSubtract,
  translate as regionTranslate,
  union as regionUnion,
  type Region,
} from './region.js';

export interface ClipSnapshot {
  readonly clip: Region;
  readonly border: Region;
}

/** Capture every window's current `clipList` and `borderClip`. Pair with
 *  a later `paintExposedRegions(r, snapshot)` to repaint only the area
 *  that changed visibility, mirroring xserver's
 *  `valdata->after.exposed = newClipList - oldClipList` machinery
 *  (mi/mivaltree.c:724). Cheaper than the full Mark/Validate pipeline
 *  and good enough for our scale. */
export function snapshotClips(r: RendererState): Map<number, ClipSnapshot> {
  const out = new Map<number, ClipSnapshot>();
  for (const w of r.windows.values()) {
    out.set(w.id, { clip: w.clipList, border: w.borderClip });
  }
  return out;
}

/** For every window still in the tree: paint background where its
 *  `clipList` newly grew, and paint the border ring where its
 *  `borderClip` newly grew. Returns a Map of `windowId -> Region`
 *  containing each window's newly-exposed content area (in absolute
 *  canvas coords). The caller (WindowManager) feeds this into
 *  `pushExposesForRegion` so clients see one Expose per rect, mirroring
 *  xserver's `miSendExposures` (mi/miexpose.c:419) -- whose input is
 *  exactly the per-window `valdata->after.exposed` set built by
 *  `mi/mivaltree.c::miComputeClips`. */
export function paintExposedRegions(
  r: RendererState,
  oldClips: Map<number, ClipSnapshot>,
): Map<number, Region> {
  const ctx = r.canvas.ctx;
  const exposedByWindow = new Map<number, Region>();
  /* Iterate windows in painting order (ascending stackOrder, parent
   * before child). DFS so a parent's bg lands before any child's
   * paint that might cover the same pixels in `exposed`. */
  for (const root of sortedChildren(r, 0)) walk(root);
  return exposedByWindow;

  function walk(w: ManagedWindow): void {
    const old = oldClips.get(w.id) ?? { clip: EMPTY_REGION, border: EMPTY_REGION };
    const contentExposed = regionSubtract(w.clipList, old.clip);
    if (!regionIsEmpty(contentExposed)) {
      paintBgInRegion(r, ctx, w, contentExposed);
      exposedByWindow.set(w.id, contentExposed);
    }
    /* Border ring: newly-exposed part of (borderClip - contentRect).
     * Compute via (newBorder - oldBorder) - newContent, matching
     * miComputeClips:373's `borderExposed = exposed - winSize`. */
    if (w.borderWidth > 0) {
      const borderDiff = regionSubtract(w.borderClip, old.border);
      if (borderDiff.length > 0) {
        const { ax, ay } = absOrigin(r, w);
        const contentRect = { ax, ay, w: w.width, h: w.height };
        const ringExposed = regionSubtract(borderDiff, [contentRect]);
        if (!regionIsEmpty(ringExposed)) paintBorderInRegion(r, ctx, w, ringExposed);
      }
    }
    for (const child of sortedChildren(r, w.id)) walk(child);
  }
}

function paintBgInRegion(
  r: RendererState,
  ctx: RootCanvasContext,
  w: ManagedWindow,
  region: Region,
): void {
  if (w.bgType === 'none') return;
  ctx.save();
  ctx.beginPath();
  for (const rc of region) ctx.rect(rc.ax, rc.ay, rc.w, rc.h);
  ctx.clip();
  paintBackgroundRect(r, ctx, w, 0, 0, w.width, w.height);
  ctx.restore();
}

function paintBorderInRegion(
  r: RendererState,
  ctx: RootCanvasContext,
  w: ManagedWindow,
  region: Region,
): void {
  ctx.save();
  ctx.beginPath();
  for (const rc of region) ctx.rect(rc.ax, rc.ay, rc.w, rc.h);
  ctx.clip();
  ctx.fillStyle = pixelToCssColor(w.borderPixel);
  const { ax, ay } = absOrigin(r, w);
  const bw = w.borderWidth;
  /* Same four-strip layout as paintWindowBorder; clip restricts to
   * the exposed sub-region. */
  ctx.fillRect(ax - bw, ay - bw, w.width + 2 * bw, bw);
  ctx.fillRect(ax - bw, ay + w.height, w.width + 2 * bw, bw);
  ctx.fillRect(ax - bw, ay, bw, w.height);
  ctx.fillRect(ax + w.width, ay, bw, w.height);
  ctx.restore();
}

/** Children of `parentId` sorted bottom-to-top by stackOrder. */
function sortedChildren(r: RendererState, parentId: number): ManagedWindow[] {
  const out: ManagedWindow[] = [];
  for (const w of r.windows.values()) {
    if (w.parent === parentId) out.push(w);
  }
  out.sort((a, b) => a.stackOrder - b.stackOrder);
  return out;
}

/** Push the window's effective drawing clip onto `ctx`. Mirrors xorg's
 *  GC composite clip: every drawing primitive clips to `pWin->clipList`
 *  (xserver/dix/window.c::ChangeWindowAttributes via miComputeClips),
 *  intersected with the GC's `clientClip` when set. We don't model
 *  per-GC clientClip so this is just `clipList`.
 *
 *  Caller must have done ctx.save() and must ctx.restore() after.
 *
 *  Why this is the load-bearing fix: an unmapped window (or a "mapped
 *  but not viewable" descendant of an unmapped ancestor) has an empty
 *  clipList after recomputeClipsAll, so every draw op naturally
 *  no-ops. Without this, clearArea/fillRect/etc. only check the
 *  shallow `win.mapped` flag and let a hidden window keep painting
 *  hover effects (twm icon-manager iconify symptom: invisible widget
 *  still receives Motion → repaints itself onto the canvas).
 *
 *  Shape: when set, intersect with shape rects too. xorg integrates
 *  shape into clipList via miSetShape; we keep shape separate for now
 *  and AND it in here. Either way the visible region shrinks; the
 *  result is identical for drawing. */
export function applyWindowClip(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
): void {
  ctx.beginPath();
  for (const rc of win.clipList) ctx.rect(rc.ax, rc.ay, rc.w, rc.h);
  ctx.clip();
  if (win.shape) {
    const { ax, ay } = absOrigin(r, win);
    ctx.beginPath();
    for (const sh of win.shape) ctx.rect(ax + sh.x, ay + sh.y, sh.w, sh.h);
    ctx.clip();
  }
}

/** Push the border-ring clip onto `ctx`. The ring region is
 *  `borderClip - contentRect` -- the part of the bounding rect that
 *  lies OUTSIDE the window's content area. Used by paintWindowBorder
 *  so the ring honours every ancestor's clip and every higher-z
 *  sibling's occlusion (both already baked into borderClip by
 *  recomputeClipsAll, mirroring miComputeClips:373). */
export function applyAncestorClip(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
): void {
  const { ax, ay } = absOrigin(r, win);
  const contentRect = { ax, ay, w: win.width, h: win.height };
  const ring = regionSubtract(win.borderClip, [contentRect]);
  ctx.beginPath();
  for (const rc of ring) ctx.rect(rc.ax, rc.ay, rc.w, rc.h);
  ctx.clip();
}

/** Paint a window's background, then recurse into its mapped children
 *  in stacking order (ascending stackOrder = bottom to top). DFS in
 *  parent-before-child order matches X: a child must paint over its
 *  parent's background, never the other way around. */
export function paintWindowSubtree(r: RendererState, w: ManagedWindow): void {
  if (w.mapped) {
    const ctx = r.canvas.ctx;
    /* Border first, in parent's coord system: paints outside the
     * window's content rect. Drawing it before the bg means content-
     * area painting (and any later child paints) cleanly overlays
     * the corners where border meets content. */
    paintWindowBorder(r, ctx, w);
    ctx.save();
    applyWindowClip(r, ctx, w);
    paintBackgroundRect(r, ctx, w, 0, 0, w.width, w.height);
    ctx.restore();
  }
  for (const child of sortedChildren(r, w.id)) paintWindowSubtree(r, child);
}

/** Paint the X11 server-drawn border ring around a window. The ring
 *  occupies the area between the window's outer rect (content +
 *  borderWidth on each side) and its content rect, in parent
 *  coordinates. Clipped only by ancestors -- the window's own clip
 *  excludes the border by definition.
 *
 *  Why bw=0 short-circuits: the four-strip math still works, but
 *  fillRect with zero-sized rects is wasted work for the (very
 *  common) borderless override-redirect / shaped-shell case. */
export function paintWindowBorder(
  r: RendererState,
  ctx: RootCanvasContext,
  w: ManagedWindow,
): void {
  const bw = w.borderWidth;
  if (bw <= 0) return;
  const { ax, ay } = absOrigin(r, w);
  ctx.save();
  /* Clip to ancestors only. The window itself is not in the chain
   * because the border is, by X11 semantics, outside its content
   * rect -- applying the window's own clip would erase the ring. */
  applyAncestorClip(r, ctx, w);
  ctx.fillStyle = pixelToCssColor(w.borderPixel);
  /* Top, bottom, left, right strips. Corners belong to top/bottom. */
  ctx.fillRect(ax - bw, ay - bw, w.width + 2 * bw, bw);
  ctx.fillRect(ax - bw, ay + w.height, w.width + 2 * bw, bw);
  ctx.fillRect(ax - bw, ay, bw, w.height);
  ctx.fillRect(ax + w.width, ay, bw, w.height);
  ctx.restore();
}

/** Paint a (window-local) rectangle using the window's current
 *  background. Mirrors xserver/mi/miwindow.c miPaintWindow's
 *  `pWin->backgroundState != None` gate at line 115: when bgType is
 *  'none' (XCreateWindow without CWBackPixel, CWBackPixmap=None,
 *  ParentRelative collapsed) the server does not paint at all, leaving
 *  whatever pixels were there (the application owns the pixels --
 *  e.g. xeyes' shell, twm's iconmgr root). 'pixmap' tiles the bound
 *  Pixmap; 'pixel' fills with `background`. Tile origin is the
 *  window's absolute top-left, so moving the window (or its parent)
 *  shifts the tile with it. Caller owns ctx.save/restore and clipping. */
export function paintBackgroundRect(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  if (win.bgType === 'none') return;
  const { ax, ay } = absOrigin(r, win);
  if (win.bgType === 'pixmap' && win.backgroundPixmap !== null) {
    const pmCanvas = r.pixmapLookup(win.backgroundPixmap);
    if (pmCanvas) {
      const pattern = ctx.createPattern(
        pmCanvas as unknown as CanvasImageSource,
        'repeat',
      );
      if (pattern) {
        ctx.save();
        ctx.translate(ax, ay);
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
  ctx.fillRect(ax + x, ay + y, w, h);
}

/** Erase a canvas rectangle by repainting it from the window tree.
 *  Used for unmap, destroy, and the "old position" half of move/
 *  resize. Walks every root window DFS, painting its background
 *  (clipped to its own rect intersected with the dirty rect) only
 *  if it intersects the dirty rect.
 *
 *  The clip stack is set up so each window's bg paint can't escape
 *  the dirty rect. applyWindowClip inside the loop adds the window's
 *  own bounding clip on top, so siblings outside the rect aren't
 *  touched even if their bg paint call covers their full rectangle.
 *
 *  Note: this paints sibling backgrounds, which overwrites whatever
 *  application drawings those siblings had inside the dirty rect. In
 *  real X the server would send Expose to those siblings; we don't
 *  re-synthesize Expose here. Acceptable for current demos (no
 *  overlapping siblings). For overlapping cases (e.g. a WM with
 *  managed windows), the affected siblings need to redraw via some
 *  other trigger. */
export function repaintAbsoluteRect(
  r: RendererState,
  rax: number,
  ray: number,
  rw: number,
  rh: number,
): void {
  if (rw <= 0 || rh <= 0) return;
  const ctx = r.canvas.ctx;
  ctx.save();
  ctx.beginPath();
  ctx.rect(rax, ray, rw, rh);
  ctx.clip();
  const visit = (win: ManagedWindow): void => {
    if (win.mapped && intersectsAbsRect(r, win, rax, ray, rw, rh)) {
      /* Border first (parent-coord, ancestor-clipped), bg second
       * (window-clipped). Mirrors paintWindowSubtree ordering so a
       * sibling that overlaps the dirty rect re-emerges with its
       * full ring + content area. */
      paintWindowBorder(r, ctx, win);
      ctx.save();
      applyWindowClip(r, ctx, win);
      paintBackgroundRect(r, ctx, win, 0, 0, win.width, win.height);
      ctx.restore();
    }
    for (const child of sortedChildren(r, win.id)) visit(child);
  };
  for (const win of sortedChildren(r, 0)) visit(win);
  ctx.restore();
}

/** Axis-aligned rect overlap test, comparing the window's absolute
 *  bounds (content + border ring) with the given rect. Used by
 *  repaintAbsoluteRect to skip windows that don't intersect the
 *  dirty area. Includes the border so a window whose only
 *  intersection is in its ring still gets its border re-emitted. */
function intersectsAbsRect(
  r: RendererState,
  win: ManagedWindow,
  rax: number,
  ray: number,
  rw: number,
  rh: number,
): boolean {
  const { ax, ay } = absOrigin(r, win);
  const bw = win.borderWidth;
  return (
    ax - bw < rax + rw &&
    ax + win.width + bw > rax &&
    ay - bw < ray + rh &&
    ay + win.height + bw > ray
  );
}

/** Collect the union of `oldClip` for `win` plus every mapped descendant,
 *  using the pre-move clip snapshot captured by `snapshotClips`. This is
 *  the "source pixels" that CopyWindow translates -- each descendant's
 *  clipList was already occluder-minus, so the union contains exactly
 *  the pixels on the canvas that belong to the moved subtree (and nothing
 *  from above-z siblings, even if a sibling corner clipped into the
 *  subtree's bounding rect). */
export function collectSubtreeOldClip(
  r: RendererState,
  win: ManagedWindow,
  oldClips: Map<number, ClipSnapshot>,
): Region {
  let out: Region = EMPTY_REGION;
  const visit = (w: ManagedWindow): void => {
    const snap = oldClips.get(w.id);
    if (snap && !regionIsEmpty(snap.clip)) out = regionUnion(out, snap.clip);
    for (const child of r.windows.values()) {
      if (child.parent === w.id) visit(child);
    }
  };
  visit(win);
  return out;
}

/** xorg `CopyWindow` equivalent, split into capture + blit so the caller
 *  can sequence them correctly around paintExposedRegions:
 *
 *    1. capture OLD pixels BEFORE recompute (canvas still has original
 *       content at the old position)
 *    2. recompute clips + paintExposedRegions (bg paint wipes old
 *       position's pixels via lower siblings / root whose newClip grew)
 *    3. blit captured pixels to NEW position
 *
 *  Reading the canvas after step 2 is too late -- the old-position
 *  pixels have been repainted with sibling/root bg, so the blit would
 *  carry bg through to the new position instead of the original
 *  window content. mi/miwindow.c::miMoveWindow uses a similar two-phase
 *  flow: it saves the "source region" before the window structure
 *  updates, then CopyWindow fires after the clip recompute.
 *
 *  Uses a temp OffscreenCanvas rather than direct same-canvas drawImage
 *  so overlapping src/dst (small drag deltas) don't hit implementation-
 *  defined behavior -- a round-trip through an intermediate surface is
 *  the spec's well-defined escape hatch. */
export interface CapturedSubtree {
  readonly canvas: OffscreenCanvas;
  readonly extAx: number;
  readonly extAy: number;
  readonly extW: number;
  readonly extH: number;
}

export function captureSubtreePixels(
  r: RendererState,
  oldSubtreeClip: Region,
): CapturedSubtree | null {
  if (regionIsEmpty(oldSubtreeClip)) return null;
  const ext = regionExtents(oldSubtreeClip);
  if (!ext || ext.w <= 0 || ext.h <= 0) return null;
  const tmp = new OffscreenCanvas(ext.w, ext.h);
  const tctx = tmp.getContext('2d');
  if (!tctx) return null;
  /* Clip to the exact old subtree clipList so we don't pick up sibling
   * pixels that happen to sit inside the bbox. */
  tctx.save();
  tctx.beginPath();
  for (const rc of oldSubtreeClip) {
    tctx.rect(rc.ax - ext.ax, rc.ay - ext.ay, rc.w, rc.h);
  }
  tctx.clip();
  tctx.drawImage(
    r.canvas.surface as unknown as CanvasImageSource,
    -ext.ax,
    -ext.ay,
  );
  tctx.restore();
  return { canvas: tmp, extAx: ext.ax, extAy: ext.ay, extW: ext.w, extH: ext.h };
}

export function blitCapturedSubtree(
  r: RendererState,
  captured: CapturedSubtree,
  oldSubtreeClip: Region,
  newSubtreeClip: Region,
  dx: number,
  dy: number,
): Region {
  if (dx === 0 && dy === 0) return EMPTY_REGION;
  /* Safe blit area: (oldSubtreeClip + delta) ∩ newSubtreeClip. A pixel
   * outside newSubtreeClip might now be occluded by something that was
   * behind the moved window before; blitting there would paint over
   * unrelated content. */
  const translated = regionTranslate(oldSubtreeClip, dx, dy);
  const blitDest: Array<{ ax: number; ay: number; w: number; h: number }> = [];
  for (const tr of translated) {
    for (const nr of newSubtreeClip) {
      const ix = Math.max(tr.ax, nr.ax);
      const iy = Math.max(tr.ay, nr.ay);
      const ex = Math.min(tr.ax + tr.w, nr.ax + nr.w);
      const ey = Math.min(tr.ay + tr.h, nr.ay + nr.h);
      if (ex > ix && ey > iy) {
        blitDest.push({ ax: ix, ay: iy, w: ex - ix, h: ey - iy });
      }
    }
  }
  if (blitDest.length === 0) return EMPTY_REGION;

  const ctx = r.canvas.ctx;
  ctx.save();
  ctx.beginPath();
  for (const rc of blitDest) ctx.rect(rc.ax, rc.ay, rc.w, rc.h);
  ctx.clip();
  ctx.drawImage(
    captured.canvas as unknown as CanvasImageSource,
    captured.extAx + dx,
    captured.extAy + dy,
  );
  ctx.restore();
  return blitDest;
}
