/**
 * Backing-store paint helpers. Mirrors xserver/mi/mipaintwin.c at the
 * level of "compute newClip-oldClip and paint window backgrounds for
 * the newly-exposed pixels". The actual pixels-to-canvas translation
 * is done by the compositor (host/render/compositor.ts) on rAF -- this
 * file only writes into per-window backing surfaces and computes the
 * regions that the Host needs to send Expose for.
 *
 * What used to live here that no longer applies:
 *  - applyWindowClip / applyAncestorClip: clipping is now a compositor
 *    concern (clipList drives drawImage clip), draw primitives just
 *    write into their own backing without thinking about siblings.
 *  - paintWindowBorder: the compositor paints the ring directly from
 *    win.borderPixel + win.borderWidth + borderClip every frame.
 *  - captureSubtreePixels / blitCapturedSubtree: a window move no
 *    longer needs pixel copying because the backing IS the window's
 *    pixels in window-local coords -- moving updates ax/ay only.
 *  - repaintAbsoluteRect: erase-by-overpaint is obsolete once the
 *    compositor clears + repaints from backings every frame; reduced
 *    to a markDirty here, dropped entirely in Phase F.
 */

import type { RendererState, ManagedWindow } from './types.js';
import { absOrigin } from './window-tree.js';
import { pixelToCssColor } from '../../runtime/canvas.js';
import {
  EMPTY_REGION,
  intersect as regionIntersect,
  isEmpty as regionIsEmpty,
  subtract as regionSubtract,
  type Region,
} from './region.js';

export interface ClipSnapshot {
  readonly clip: Region;
  readonly border: Region;
}

/** Capture every window's current `clipList` and `borderClip`. Pair with
 *  a later `paintExposedRegions(r, snapshot)` to compute the area whose
 *  visibility changed and synthesise Expose for it, mirroring xserver's
 *  `valdata->after.exposed = newClipList - oldClipList` machinery
 *  (mi/mivaltree.c:724). */
export function snapshotClips(r: RendererState): Map<number, ClipSnapshot> {
  const out = new Map<number, ClipSnapshot>();
  for (const w of r.windows.values()) {
    out.set(w.id, { clip: w.clipList, border: w.borderClip });
  }
  return out;
}

/** For every window still in the tree: detect newly-VISIBLE area and,
 *  for any portion that's still in the window's `unpaintedRegion`,
 *  paint inherited bg into the corresponding backing region and emit
 *  Expose so the client can redraw on top. Already-painted portions
 *  are quietly handled by the compositor (re-blits the existing
 *  backing pixels through the new clip).
 *
 *  Backing-store semantics: this is the "lazy paint" path. Real X
 *  with backing-store enabled does the same: server fires Expose
 *  only for unpainted-and-now-visible pixels, leaving previously-
 *  painted pixels alone (the server's per-window pixmap preserves
 *  them; our per-window OffscreenCanvas plays that role).
 *
 *  Resize-grow uses the same machinery -- the grown strips are added
 *  to unpaintedRegion in `configureWindow`, so paintExposedRegions
 *  picks them up here without any special-case branch. */
export function paintExposedRegions(
  r: RendererState,
  oldClips: Map<number, ClipSnapshot>,
): Map<number, Region> {
  const exposedByWindow = new Map<number, Region>();
  let anyChange = false;
  for (const w of r.windows.values()) {
    const old = oldClips.get(w.id) ?? { clip: EMPTY_REGION, border: EMPTY_REGION };
    const contentExposed = regionSubtract(w.clipList, old.clip);
    if (!regionIsEmpty(contentExposed)) {
      anyChange = true;
      const { ax, ay } = absOrigin(r, w);
      /* Translate exposed (abs) → local, intersect with unpainted,
       * translate back to abs for the Expose dispatch + bg paint.
       *
       * xserver VTMap path (miComputeClips): when a window becomes
       * newly viewable, `exposed = universe - borderVisible` where
       * borderVisible is empty for a previously-unmapped window,
       * yielding the full content area. em-x11's backing-preservation
       * model (unpaintedRegion) breaks this for re-expose after
       * Unmap→Map cycles: a descendant that was painted before the
       * parent Unmap has an empty unpaintedRegion, so no Expose is
       * sent on re-Map and the widget never redraws. Match xorg by
       * treating NotViewable→Viewable transitions as fully unpainted. */
      const wasNotViewable = regionIsEmpty(old.clip);
      /* When old.clip was forced empty (ForgetGravity resize, or genuine
       * not-viewable→viewable transition), expose the full content rect
       * regardless of the current shape. The shape may still be stale from
       * before the resize (the app hasn't called XShapeCombineRectangles
       * yet), and clipping exposed to the old shape causes the window to
       * only partially redraw, leaving the grown areas blank until the
       * next shape update — which may never arrive if the app only sets
       * shape once at startup. */
      const effectiveContentExposed = wasNotViewable
        ? [{ ax, ay, w: w.width, h: w.height }]
        : contentExposed;
      const effectiveUnpainted = wasNotViewable
        ? [{ ax: 0, ay: 0, w: w.width, h: w.height }]
        : w.unpaintedRegion;
      const exposedLocal = effectiveContentExposed.map((rc) => ({
        ax: rc.ax - ax,
        ay: rc.ay - ay,
        w: rc.w,
        h: rc.h,
      }));
      const needsRedrawLocal = regionIntersect(effectiveUnpainted, exposedLocal);
      if (!regionIsEmpty(needsRedrawLocal)) {
        for (const rc of needsRedrawLocal) {
          paintBackgroundIntoBacking(r, w, rc.ax, rc.ay, rc.w, rc.h);
        }
        /* When bgType is 'none', paintBackgroundIntoBacking returns
         * early without calling markBackingPainted, so unpaintedRegion
         * stays full forever. If we just sent a full-content Expose
         * (wasNotViewable), the client will redraw everything -- clear
         * unpaintedRegion so the next paintExposedRegions (e.g. from
         * setWindowShape arriving a microtask later) doesn't send a
         * redundant Expose whose XClearWindow call would overwrite
         * what the client just drew in the center area. We only do
         * this for the wasNotViewable path; normal occluder-driven
         * exposures must still consult unpaintedRegion so areas that
         * were truly never drawn get re-exposed. */
        if (wasNotViewable) {
          for (const rc of needsRedrawLocal) {
            markBackingPainted(w, rc.ax, rc.ay, rc.w, rc.h);
          }
        }
        const exposedAbs: Region = needsRedrawLocal.map((rc) => ({
          ax: rc.ax + ax,
          ay: rc.ay + ay,
          w: rc.w,
          h: rc.h,
        }));
        exposedByWindow.set(w.id, exposedAbs);
      }
      /* Already-painted portion: backing has valid pixels; compositor
       * blits them on the next rAF. No client work needed. */
    }
    if (w.borderWidth > 0) {
      const borderDiff = regionSubtract(w.borderClip, old.border);
      if (borderDiff.length > 0) anyChange = true;
    }
  }
  if (anyChange) r.markDirty();
  return exposedByWindow;
}

/** Paint a (window-local) rectangle of the window's background into
 *  its backing surface. Mirrors xserver/mi/miwindow.c miPaintWindow's
 *  `pWin->backgroundState != None` gate: 'none' is genuinely no-op
 *  (real X "leave the pixels alone"; in our backing model that means
 *  leaving the backing transparent, and the compositor's z-order
 *  overdraw lets lower-z windows show through). 'parentRelative'
 *  walks up the parent chain to the nearest 'pixel'/'pixmap' owner
 *  and tiles that in (skipping further 'parentRelative' or 'none'
 *  ancestors that have nothing of their own to inherit). 'pixel' and
 *  'pixmap' fill solid / tile.
 *
 *  Subtracts the painted rect from the window's `unpaintedRegion`
 *  so paintExposedRegions knows this area no longer needs Expose
 *  when an occluder later moves away. */
export function paintBackgroundIntoBacking(
  r: RendererState,
  win: ManagedWindow,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  if (win.bgType === 'none') return;
  const ctx = win.backingCtx;
  if (win.bgType === 'parentRelative') {
    paintParentRelativeIntoBacking(r, win, ctx, x, y, w, h);
    return;
  }
  if (win.bgType === 'pixmap' && win.backgroundPixmap !== null) {
    const pmCanvas = r.pixmapLookup(win.backgroundPixmap);
    if (pmCanvas) {
      const pattern = ctx.createPattern(
        pmCanvas as unknown as CanvasImageSource,
        'repeat',
      );
      if (pattern) {
        ctx.save();
        ctx.fillStyle = pattern;
        ctx.fillRect(x, y, w, h);
        ctx.restore();
        win.backingDirty = true;
        markBackingPainted(win, x, y, w, h);
        r.markDirty();
        return;
      }
    }
    /* Pixmap vanished or pattern build failed -- fall through to solid
     * fill so we never leave an unpainted hole. */
  }
  ctx.fillStyle = pixelToCssColor(win.background);
  ctx.fillRect(x, y, w, h);
  win.backingDirty = true;
  markBackingPainted(win, x, y, w, h);
  r.markDirty();
}

/** Subtract a window-local rect from the window's unpaintedRegion.
 *  Clamped to content rect; rects entirely outside are no-ops. */
function markBackingPainted(
  win: ManagedWindow,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  if (regionIsEmpty(win.unpaintedRegion)) return;
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

/** Resolve a ParentRelative chain: walk up the parent links until we
 *  find a non-ParentRelative ancestor, then paint that ancestor's bg
 *  into the child's backing at child-local (x,y,w,h). Tile origin
 *  alignment is critical for 'pixmap' bg so a pattern (e.g. xterm's
 *  default stipple) remains seamless across the parent boundary;
 *  the child's local (0,0) corresponds to pattern coord
 *  (child.absOrigin - owner.absOrigin) in the owner's tile space. */
function paintParentRelativeIntoBacking(
  r: RendererState,
  win: ManagedWindow,
  ctx: OffscreenCanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  let owner = r.windows.get(win.parent);
  /* Skip ancestors whose own bg is 'parentRelative' or 'none' (they
   * have nothing of their own to inherit from). Find the nearest
   * 'pixel'/'pixmap' ancestor. Bail on cycles or when the chain ends
   * without a paintable ancestor. */
  let hop = 0;
  while (owner && (owner.bgType === 'parentRelative' || owner.bgType === 'none')) {
    if (++hop > 64) return;
    owner = r.windows.get(owner.parent);
  }
  if (!owner) return;
  if (owner.bgType === 'pixel') {
    ctx.fillStyle = pixelToCssColor(owner.background);
    ctx.fillRect(x, y, w, h);
    win.backingDirty = true;
    markBackingPainted(win, x, y, w, h);
    r.markDirty();
    return;
  }
  if (owner.bgType === 'pixmap' && owner.backgroundPixmap !== null) {
    const pmCanvas = r.pixmapLookup(owner.backgroundPixmap);
    if (!pmCanvas) return;
    const pattern = ctx.createPattern(
      pmCanvas as unknown as CanvasImageSource,
      'repeat',
    );
    if (!pattern) return;
    /* Align tile so child-local (0,0) samples pattern at
     * (child.absOrigin - owner.absOrigin) -- the offset of the child
     * inside the owner's tile space. setTransform translates the
     * pattern's source origin negatively, which means draw at child-
     * local (lx, ly) samples pattern at (lx + offsetX, ly + offsetY). */
    const childOrigin = absOrigin(r, win);
    const ownerOrigin = absOrigin(r, owner);
    const offsetX = childOrigin.ax - ownerOrigin.ax;
    const offsetY = childOrigin.ay - ownerOrigin.ay;
    pattern.setTransform(new DOMMatrix().translate(-offsetX, -offsetY));
    ctx.save();
    ctx.fillStyle = pattern;
    ctx.fillRect(x, y, w, h);
    ctx.restore();
    win.backingDirty = true;
    markBackingPainted(win, x, y, w, h);
    r.markDirty();
    return;
  }
  /* owner is itself ParentRelative — the loop above should have hopped
   * past it, but be defensive. */
}

/** Paint the window's full background into its backing, then recurse
 *  into mapped children. DFS so children's per-backing init runs after
 *  their parent's. The compositor handles z-ordering at composite time;
 *  this function only initialises pixels.
 *
 *  Used by setWindowBackgroundPixmap on the root window during shared-
 *  root setup so the canvas-bg pattern lands before any client paint. */
export function paintWindowSubtree(r: RendererState, w: ManagedWindow): void {
  if (w.mapped) {
    paintBackgroundIntoBacking(r, w, 0, 0, w.width, w.height);
  }
  for (const child of sortedChildren(r, w.id)) paintWindowSubtree(r, child);
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
