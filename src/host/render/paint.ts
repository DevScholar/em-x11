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

/** For every window still in the tree: paint background into the
 *  window's backing where its `clipList` newly grew, and request a
 *  re-compose. Returns a Map of `windowId -> Region` containing each
 *  window's newly-exposed content area (in absolute canvas coords).
 *  The caller (WindowManager) feeds this into `pushExposesForRegion`
 *  so clients see one Expose per rect, mirroring xserver's
 *  `miSendExposures` (mi/miexpose.c:419) -- whose input is exactly
 *  the per-window `valdata->after.exposed` set built by
 *  `mi/mivaltree.c::miComputeClips`. */
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
      paintBgInRegion(r, w, contentExposed);
      exposedByWindow.set(w.id, contentExposed);
      anyChange = true;
    }
    /* Border ring exposure doesn't need backing paint -- the compositor
     * draws the ring fresh every frame from borderPixel + borderClip --
     * but we still detect changes so the compositor re-runs. */
    if (w.borderWidth > 0) {
      const borderDiff = regionSubtract(w.borderClip, old.border);
      if (borderDiff.length > 0) anyChange = true;
    }
  }
  if (anyChange) r.markDirty();
  return exposedByWindow;
}

/** Paint window-bg into the backing rectangles intersected with the
 *  given absolute region. Used by paintExposedRegions to lay down a
 *  fresh bg in newly-exposed sub-regions of a window so the Expose
 *  handler's app-paint has a clean canvas to draw on. The compositor
 *  will pick this up on the next rAF. */
function paintBgInRegion(
  r: RendererState,
  w: ManagedWindow,
  region: Region,
): void {
  if (w.bgType === 'none') return;
  const { ax, ay } = absOrigin(r, w);
  for (const rc of region) {
    const lx = rc.ax - ax;
    const ly = rc.ay - ay;
    paintBackgroundIntoBacking(r, w, lx, ly, rc.w, rc.h);
  }
}

/** Paint a (window-local) rectangle of the window's background into
 *  its backing surface. Mirrors xserver/mi/miwindow.c miPaintWindow's
 *  `pWin->backgroundState != None` gate: bgType='none' is a no-op
 *  (application owns the pixels). 'pixmap' tiles the bound Pixmap;
 *  'pixel' fills with `background`. Tile origin is the window's
 *  top-left in local coords (ax/ay = 0,0 in backing frame).
 *
 *  Triggers markDirty so the compositor runs on rAF. */
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
  r.markDirty();
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
