/**
 * Window tree mutation + read-only accessors. Mirrors xserver/dix/window.c
 * structural ops, plus absOrigin (resolving local→canvas-absolute coords)
 * which dix/window.c does implicitly via the WindowPtr->drawable.x cache.
 *
 * Each function takes a RendererState so the same logic can be unit-tested
 * with a synthetic state, and we avoid an import cycle on Renderer.
 */

import type { RendererState, ManagedWindow } from './types.js';
import type { ShapeRect } from '../../types/emscripten.js';
import { MAX_PARENT_WALK } from '../constants.js';
import { paintWindowBorder, paintWindowSubtree, snapshotClips, paintExposedRegions, collectSubtreeOldClip, copySubtreeByDelta } from './paint.js';
import {
  EMPTY_REGION,
  intersect as regionIntersect,
  isEmpty as isEmptyRegion,
  subtract as regionSubtract,
  union as unionRegion,
  type Region,
  type Rect,
} from './region.js';

export function addWindow(
  r: RendererState,
  id: number,
  parent: number,
  x: number,
  y: number,
  width: number,
  height: number,
  borderWidth: number,
  borderPixel: number,
  bgType: 'none' | 'pixel' | 'pixmap',
  background: number,
): void {
  /* Create only stores state -- the window isn't visible until
   * mapWindow flips `mapped` and triggers the sync paint. */
  r.windows.set(id, {
    id,
    parent,
    x,
    y,
    width,
    height,
    stackOrder: r.stackCounter++,
    borderWidth,
    borderPixel,
    bgType,
    background,
    backgroundPixmap: null,
    mapped: false,
    shape: null,
    /* Clip lists start empty: an unmapped window has nothing visible.
     * `recomputeClipsAll` populates them when the window (or an
     * ancestor) maps. */
    clipList: EMPTY_REGION,
    borderClip: EMPTY_REGION,
  });
}

/** Recompute `clipList` and `borderClip` for every window in the tree.
 *  Mirrors `xserver/mi/mivaltree.c::miComputeClips` recursion -- each
 *  child receives `parent.universe ∩ child.borderRect`, then the
 *  child's `borderRect` is subtracted from the working universe so
 *  later (lower-z) siblings see only what's left.
 *
 *  Differences vs xorg:
 *  - Recompute-from-scratch on every structural op rather than the
 *    incremental Mark/Validate path. Our scale (~10 mapped windows
 *    in demos) makes the perf delta a non-issue and the from-scratch
 *    version is dramatically easier to verify against the source.
 *  - Bounding-rect approximation for shaped windows: a shaped window
 *    contributes its full bounding rect to occlusion, never its
 *    shape geometry. Conservative -- over-occludes through shape
 *    holes, never under-occludes.
 *  - No ParentRelative / Composite / RANDR / multi-screen handling. */
export function recomputeClipsAll(r: RendererState): void {
  /* Pre-pass: anything not currently mapped (or any descendant of an
   * unmapped ancestor) gets empty clip. xorg gates on `viewable`;
   * since we don't track that explicitly, the "viewable" check happens
   * naturally as `mapped && (mapped ancestor chain)` -- the recursion
   * below only descends into mapped subtrees, so unmapped windows are
   * skipped entirely and stay at whatever they had. Reset everyone
   * first to clear stale state from before an unmap/destroy. */
  for (const w of r.windows.values()) {
    w.clipList = EMPTY_REGION;
    w.borderClip = EMPTY_REGION;
  }

  /* Walk every root-level window (parent === 0). The shared root
   * occupies the canvas; toplevel client windows are direct children
   * of it. We start the recursion at each parent===0 window with its
   * own bounding rect as the universe. */
  const roots: ManagedWindow[] = [];
  for (const w of r.windows.values()) {
    if (w.parent === 0 && w.mapped) roots.push(w);
  }
  roots.sort((a, b) => a.stackOrder - b.stackOrder);

  /* Top-to-bottom (highest stackOrder first), so each level subtracts
   * higher-z siblings before recursing into lower ones, matching
   * mivaltree.c:401 firstChild→nextSib iteration where firstChild is
   * topmost. We store siblings with stackOrder ascending so reverse
   * gives top-to-bottom. */
  for (let i = roots.length - 1; i >= 0; i--) {
    const root = roots[i]!;
    const bounding: Rect = absBoundingRectOf(r, root);
    computeClipsRecursive(r, root, [bounding]);
  }
}

function computeClipsRecursive(
  r: RendererState,
  win: ManagedWindow,
  universe: Region,
): void {
  /* xorg miComputeClips:
   *   pWin->borderClip = universe
   *   universe ∩= winSize          (winSize = content rect, shaped)
   *   for each viewable child top-to-bottom:
   *     childUniverse = universe ∩ child->borderSize  (shaped)
   *     recurse(child, childUniverse)
   *     universe -= child->borderSize                  (shaped)
   *   pWin->clipList = universe
   *
   * SHAPE integration: when a window has a bounding shape (XShape
   * SHAPE_BOUNDING), `borderSize` is the bounding rect intersected
   * with the shape, and `winSize` is the content rect intersected
   * with the shape. Both are used to occlude lower siblings -- so
   * xcalc sitting under a shaped xeyes shell now sees only the
   * SHAPE silhouette as the occluder, not the full bounding rect.
   * That's how the see-through area between the eyes lets lower-z
   * pixels (and incoming Expose) propagate to xcalc, mirroring
   * xserver/Xext/shape.c. */
  win.borderClip = universe;

  let innerUniverse = regionIntersect(universe, winSize(r, win));

  /* Top-to-bottom traversal: descending stackOrder so each iteration
   * sees only what wasn't covered by higher-z siblings. */
  const children = sortedMappedChildrenDescending(r, win.id);
  for (const child of children) {
    const childBSize = borderSize(r, child);
    const childUniverse = regionIntersect(innerUniverse, childBSize);
    computeClipsRecursive(r, child, childUniverse);
    innerUniverse = regionSubtract(innerUniverse, childBSize);
  }

  win.clipList = innerUniverse;
}

/** Window's `borderSize` (xorg term): bounding rect, possibly clipped
 *  by the bounding shape. Used for parent universe intersection +
 *  sibling occlusion subtraction. */
function borderSize(r: RendererState, win: ManagedWindow): Region {
  const bbox = absBoundingRectOf(r, win);
  if (!win.shape) return [bbox];
  const { ax, ay } = absOrigin(r, win);
  /* Shape rects are window-local (relative to content origin). Convert
   * to absolute and intersect with the bounding rect so a shape
   * covering the border ring still respects bw. */
  const shapeAbs: Region = win.shape.map((s) => ({
    ax: ax + s.x,
    ay: ay + s.y,
    w: s.w,
    h: s.h,
  }));
  return regionIntersect(shapeAbs, [bbox]);
}

/** Window's `winSize` (xorg term): content rect, possibly clipped by
 *  the bounding shape. Used for the window's own clipList -- limits
 *  where the window paints. */
function winSize(r: RendererState, win: ManagedWindow): Region {
  const { ax, ay } = absOrigin(r, win);
  const cr: Rect = { ax, ay, w: win.width, h: win.height };
  if (!win.shape) return [cr];
  const shapeAbs: Region = win.shape.map((s) => ({
    ax: ax + s.x,
    ay: ay + s.y,
    w: s.w,
    h: s.h,
  }));
  return regionIntersect(shapeAbs, [cr]);
}

function sortedMappedChildrenDescending(
  r: RendererState,
  parentId: number,
): ManagedWindow[] {
  const out: ManagedWindow[] = [];
  for (const w of r.windows.values()) {
    if (w.parent === parentId && w.mapped) out.push(w);
  }
  out.sort((a, b) => b.stackOrder - a.stackOrder);
  return out;
}

/* Internal helper that avoids the option-type wrapper of the public
 * `absBoundingRect` -- recompute path always has a valid window so
 * the null branch would just be dead weight. */
function absBoundingRectOf(r: RendererState, win: ManagedWindow): Rect {
  const { ax, ay } = absOrigin(r, win);
  const bw = win.borderWidth;
  return { ax: ax - bw, ay: ay - bw, w: win.width + 2 * bw, h: win.height + 2 * bw };
}

/** Border-only update. Width or pixel can change independently of
 *  geometry (XSetWindowBorder vs XSetWindowBorderWidth).
 *
 *  Real X (xserver dix/window.c::ChangeWindowAttributes for CWBorderPixel,
 *  ChangeBorderWidth for CWBorderWidth) repaints only the *border ring*
 *  in the pixel-only case -- the interior is untouched. We used to wipe
 *  the entire subtree via paintWindowSubtree, which under twm's
 *  focus-follows-pointer cost xcalc all its button labels: every focus
 *  change calls XSetWindowBorder(frame, color), our wipe cleared the
 *  whole frame area (including descendants' content), but no Expose
 *  was synthesized to descendants -- so the buttons stayed blank until
 *  some other event (EnterNotify -> Xaw highlight()) re-triggered each
 *  button's redraw individually. Real X protocol forbids touching
 *  pixels outside the ring on a pixel-only change.
 *
 *  Pixel-only path: redraw the ring; nothing else.
 *
 *  Width-changed path: same outer-strip nuke as before for now.
 *  This is a rare case (twm focus doesn't trigger it; only manual
 *  XSetWindowBorderWidth and XConfigureWindow CWBorderWidth do), and
 *  proper handling needs Expose synthesis on overlapped descendants
 *  -- not the simple ring repaint -- which is left for the broader
 *  Expose-on-overpaint pass. */
export function setWindowBorder(
  r: RendererState,
  id: number,
  borderWidth: number,
  borderPixel: number,
): Map<number, Region> {
  const w = r.windows.get(id);
  if (!w) return new Map();
  const oldBw = w.borderWidth;
  const widthChanged = oldBw !== borderWidth;
  w.borderWidth = borderWidth;
  w.borderPixel = borderPixel;
  if (!w.mapped) return new Map();
  if ((globalThis as { __EMX11_TRACE_PAINT__?: boolean }).__EMX11_TRACE_PAINT__) {
    console.log('[paint] setWindowBorder', id, 'widthChanged=', widthChanged);
  }
  if (!widthChanged) {
    /* Color-only change: ring is in the same place, just different
     * pixel. Repaint just the ring -- no Expose needed because
     * `clipList` is unchanged so no client pixels are invalidated. */
    paintWindowBorder(r, r.canvas.ctx, w);
    return new Map();
  }
  /* Width changed: bounding rect grew/shrank, clipLists move. Diff
   * paint via the snapshot/recompute pipeline so we touch only what
   * actually changed visibility. */
  const oldClips = snapshotClips(r);
  recomputeClipsAll(r);
  return paintExposedRegions(r, oldClips);
}

/** Solid-background update (XSetWindowBackground / CWBackPixel, or
 *  CWBackPixmap=None which collapses to bgType='none' at the bridge).
 *  The change takes effect on the next XClearArea or Expose-triggered
 *  bg paint, just like real X -- we don't auto-repaint here because
 *  Xt's XawCommandToggle path sequences this with a ClearArea that
 *  the widget relies on for the pixel update to land. */
export function setWindowBackground(
  r: RendererState,
  id: number,
  bgType: 'none' | 'pixel' | 'pixmap',
  background: number,
): void {
  const w = r.windows.get(id);
  if (!w) return;
  w.bgType = bgType;
  w.background = background;
  /* Switching off pixmap mode (bgType='none' or 'pixel') drops any
   * previously-bound tile so it doesn't reappear after an unrelated
   * set_bg_pixmap(0) was missed. Mirrors xserver dix/window.c:1242
   * which DestroyPixmap on the prior tile when CWBackPixel arrives. */
  if (bgType !== 'pixmap') w.backgroundPixmap = null;
}

export function setWindowBackgroundPixmap(r: RendererState, id: number, pmId: number): void {
  const w = r.windows.get(id);
  if (!w) return;
  if (pmId > 0) {
    w.backgroundPixmap = pmId;
    w.bgType = 'pixmap';
  } else {
    w.backgroundPixmap = null;
    /* pmId==0 here means "no pixmap, fall back to pixel" (the bridge
     * uses set_bg with bgType=0 for the protocol-level None case;
     * it never reaches here). */
    w.bgType = 'pixel';
  }
  /* Repaint sync if the window is currently visible: shared-root
   * setup calls this AFTER mapWindow(root), so we need the weave
   * to land without waiting for some external trigger. */
  if (w.mapped) paintWindowSubtree(r, w);
}

/** Geometry-only update for an existing window. Preserves parent,
 *  shape, background_pixmap, and mapped state. No-op if id is unknown.
 *
 *  xorg CopyWindow path: when the geometry change is a pure move (same
 *  width/height), capture the old subtree's visible pixels before the
 *  clip recompute, then blit them to the new position AFTER bg paint.
 *  Clients only get Expose for residual area -- the translated-old area
 *  is already correct on the canvas. Mirrors mi/miwindow.c::miMoveWindow.
 *
 *  Resize path (or width/height change): no CopyWindow possible; falls
 *  back to erase-old-and-repaint. The client receives Expose for the
 *  full new visible area and redraws content. Matches real xorg's
 *  resize path too (CopyWindow is skipped when the window's size
 *  changed). */
export function configureWindow(
  r: RendererState,
  id: number,
  x: number,
  y: number,
  w: number,
  h: number,
): Map<number, Region> {
  const win = r.windows.get(id);
  if (!win) return new Map();
  const sameGeom =
    win.x === x && win.y === y && win.width === w && win.height === h;
  if (sameGeom) return new Map();
  if ((globalThis as { __EMX11_TRACE_PAINT__?: boolean }).__EMX11_TRACE_PAINT__) {
    console.log('[paint] configureWindow', id, '(', x, y, w, h, ')');
  }
  const isMoveOnly = win.width === w && win.height === h;
  const dx = x - win.x;
  const dy = y - win.y;
  const oldClips = snapshotClips(r);

  /* For the move-only fast path, pre-compute the old subtree clip now
   * (still reading the pre-move state). After recomputeClipsAll the
   * old clip data only survives inside `oldClips`; this collects it
   * into the union the blit needs. */
  const oldSubtreeClip = isMoveOnly ? collectSubtreeOldClip(r, win, oldClips) : null;

  win.x = x;
  win.y = y;
  win.width = w;
  win.height = h;
  recomputeClipsAll(r);
  const exposed = paintExposedRegions(r, oldClips);

  if (oldSubtreeClip && !isEmptyRegion(oldSubtreeClip) && (dx !== 0 || dy !== 0)) {
    /* Union of new clipLists for the moved subtree -- the blit clamps
     * itself to this so we don't spray pixels where an occluder now
     * sits. */
    let newSubtreeClip: Region = win.clipList;
    const visitNew = (pid: number): void => {
      for (const child of r.windows.values()) {
        if (child.parent === pid) {
          if (!isEmptyRegion(child.clipList)) {
            newSubtreeClip = unionRegion(newSubtreeClip, child.clipList);
          }
          visitNew(child.id);
        }
      }
    };
    visitNew(win.id);
    const blitted = copySubtreeByDelta(r, oldSubtreeClip, newSubtreeClip, dx, dy);
    /* Subtract the blitted rects from each window's Expose region: the
     * canvas already has the correct pixels there, so the client
     * doesn't need to redraw. */
    if (blitted.length > 0) {
      for (const [winId, region] of exposed) {
        const reduced = regionSubtract(region, blitted);
        if (isEmptyRegion(reduced)) exposed.delete(winId);
        else exposed.set(winId, reduced);
      }
    }
  }

  return exposed;
}

/** XReparentWindow: change a window's parent link and local origin.
 *  Does not affect mapped state. Unknown id is a no-op (cross-
 *  connection callers may race ahead of the owner's create).
 *
 *  In a real X server reparenting a mapped window implicitly unmaps
 *  (erasing the old pixel rect on the old parent) and remaps (painting
 *  background at the new parent's coordinate system, then sending
 *  Expose) -- x11protocol.txt §1040. Our renderer has no backing
 *  store, so the canvas under the old position has to be reconstructed
 *  from the window tree and the new position has to be painted fresh.
 *  Without this, twm reparenting xeyes' shell left the xeyes drawing
 *  stranded at its original root-relative coordinates while the new
 *  frame+shell composite never actually hit the canvas. */
export function reparentWindow(
  r: RendererState,
  id: number,
  parent: number,
  x: number,
  y: number,
): Map<number, Region> {
  const win = r.windows.get(id);
  if (!win) return new Map();
  const oldClips = snapshotClips(r);
  win.parent = parent;
  win.x = x;
  win.y = y;
  recomputeClipsAll(r);
  return paintExposedRegions(r, oldClips);
}

/** Read-only accessor for redirect decisions in Host. Returns the
 *  parent XID or 0 when the window is unknown / parentless. */
export function parentOf(r: RendererState, id: number): number {
  return r.windows.get(id)?.parent ?? 0;
}

/** XRaiseWindow: give the window the highest stackOrder among siblings
 *  so it paints on top. Repaints the affected rect so the new order
 *  is reflected immediately. */
export function raiseWindow(r: RendererState, id: number): Map<number, Region> {
  const w = r.windows.get(id);
  if (!w) return new Map();
  const oldClips = snapshotClips(r);
  w.stackOrder = r.stackCounter++;
  recomputeClipsAll(r);
  return paintExposedRegions(r, oldClips);
}

/** Enumerate the mapped descendants of `id` in parent-before-child DFS
 *  order. Used by Host to synthesize Expose events for every visible
 *  window whose content was wiped by a subtree repaint (map, move,
 *  resize). `id` itself is NOT included. */
export function mappedDescendants(r: RendererState, id: number): number[] {
  const out: number[] = [];
  const recurse = (parentId: number): void => {
    for (const child of r.windows.values()) {
      if (child.parent === parentId) {
        if (child.mapped) out.push(child.id);
        recurse(child.id);
      }
    }
  };
  recurse(id);
  return out;
}

/** Read-only geometry accessor for Host-side event synthesis
 *  (Expose needs width/height). Null if unknown. */
export function geometryOf(
  r: RendererState,
  id: number,
): { width: number; height: number } | null {
  const w = r.windows.get(id);
  return w ? { width: w.width, height: w.height } : null;
}

/** Absolute bounding box (content + border) of a window, or null if
 *  unknown. Used by Host-side Expose-on-overpaint logic to capture
 *  the area that's about to get repainted before the renderer mutates
 *  the tree. */
export function absBoundingRect(
  r: RendererState,
  id: number,
): { ax: number; ay: number; w: number; h: number } | null {
  const w = r.windows.get(id);
  if (!w) return null;
  const { ax, ay } = absOrigin(r, w);
  const bw = w.borderWidth;
  return { ax: ax - bw, ay: ay - bw, w: w.width + 2 * bw, h: w.height + 2 * bw };
}

/** Enumerate every mapped window (and its mapped descendants) that
 *  intersects the given absolute rectangle, excluding `excludeId`
 *  itself. Used when the renderer is about to repaint backgrounds
 *  inside a dirty rect (unmap, destroy, configure-old-rect): every
 *  affected mapped window has its content wiped and needs a fresh
 *  Expose to redraw. xserver/dix/window.c does this implicitly via
 *  per-window backing pixmaps + DamageReport; we don't have those,
 *  so the Host has to synthesize Expose explicitly via this list. */
export function mappedWindowsIntersecting(
  r: RendererState,
  rax: number,
  ray: number,
  rw: number,
  rh: number,
  excludeId: number,
): number[] {
  const out: number[] = [];
  for (const win of r.windows.values()) {
    if (win.id === excludeId) continue;
    if (!win.mapped) continue;
    const { ax, ay } = absOrigin(r, win);
    const bw = win.borderWidth;
    const wax = ax - bw;
    const way = ay - bw;
    const ww = win.width + 2 * bw;
    const wh = win.height + 2 * bw;
    if (
      wax < rax + rw && wax + ww > rax &&
      way < ray + rh && way + wh > ray
    ) {
      out.push(win.id);
    }
  }
  return out;
}

/** Full authoritative attribute snapshot for cross-connection
 *  XGetWindowAttributes. Local shadows drift (a WM never mirrors
 *  another client's window), so when a client queries a window it
 *  doesn't own, Xlib falls back to this via the JS bridge.
 *  Per dix/window.c, window state is server-authoritative by XID;
 *  this accessor is how we present that view without a full
 *  refactor of the per-client shadow tables. */
export function attrsOf(
  r: RendererState,
  id: number,
): {
  x: number; y: number; width: number; height: number;
  mapped: boolean; parent: number; borderWidth: number;
} | null {
  const w = r.windows.get(id);
  if (!w) return null;
  return { x: w.x, y: w.y, width: w.width, height: w.height,
           mapped: w.mapped, parent: w.parent,
           borderWidth: w.borderWidth };
}

/** Resolve a window's origin to canvas-absolute coordinates by
 *  summing local (x, y) up the parent chain. ManagedWindow.{x,y} is
 *  local-to-parent (matching X semantics); the renderer needs
 *  absolute to actually paint. Root is at (0, 0) so the chain
 *  terminates cleanly. Guarded against cyclic parent links. */
export function absOrigin(r: RendererState, win: ManagedWindow): { ax: number; ay: number } {
  let ax = win.x;
  let ay = win.y;
  let pid = win.parent;
  for (let i = 0; pid !== 0 && i < MAX_PARENT_WALK; i++) {
    const p = r.windows.get(pid);
    if (!p) break;
    ax += p.x;
    ay += p.y;
    pid = p.parent;
  }
  return { ax, ay };
}

export function setWindowShape(r: RendererState, id: number, rects: ShapeRect[]): Map<number, Region> {
  const w = r.windows.get(id);
  if (!w) return new Map();
  if (!w.mapped) {
    w.shape = rects.length > 0 ? rects : null;
    return new Map();
  }
  /* Shape integrated into borderSize/winSize: the diff pipeline now
   * captures who newly sees through (lower-z siblings exposed in the
   * shape "holes") and who newly hides behind shape edges. xorg
   * Xext/shape.c calls `(*pScreen->ResizeWindow)` after SetShape,
   * which runs the full Mark/Validate/HandleExposures dance --
   * snapshotClips / paintExposedRegions is our equivalent. */
  const oldClips = snapshotClips(r);
  w.shape = rects.length > 0 ? rects : null;
  recomputeClipsAll(r);
  return paintExposedRegions(r, oldClips);
}

/** X11 "viewable" semantics: a window produces pixels only if it and
 *  every ancestor up through root is mapped (x11protocol.txt §Window
 *  State). Marked-mapped descendants of an unmapped ancestor are
 *  "mapped but not viewable" -- still in the tree, but invisible.
 *  Xt toolkits exploit this: XtRealizeWidget creates and MAPS all
 *  composite children first, then maps the shell last. The renderer
 *  has to respect this, otherwise children paint their backgrounds
 *  at their parent-local coordinates while the parent is still
 *  unmapped -- which lands as a ghost paint at root (0,0) when the
 *  parent happens to be the shell with no position set yet. */
export function isViewable(r: RendererState, id: number): boolean {
  let cur = r.windows.get(id);
  while (cur) {
    if (!cur.mapped) return false;
    if (cur.parent === 0) return true;
    cur = r.windows.get(cur.parent);
  }
  return false;
}

export function mapWindow(r: RendererState, id: number): Map<number, Region> {
  const w = r.windows.get(id);
  if (!w) return new Map();
  if ((globalThis as { __EMX11_TRACE_PAINT__?: boolean }).__EMX11_TRACE_PAINT__) {
    console.log('[paint] mapWindow', id, 'parent=', w.parent);
  }
  const oldClips = snapshotClips(r);
  w.mapped = true;
  recomputeClipsAll(r);
  /* Mapped-but-not-viewable windows have empty newClip (their ancestor
   * chain blocks them); paintExposedRegions naturally no-ops on them.
   * When the ancestor later maps, ITS recompute discovers the now-
   * viewable subtree and the diff is non-empty. */
  return paintExposedRegions(r, oldClips);
}

export function unmapWindow(r: RendererState, id: number): Map<number, Region> {
  const w = r.windows.get(id);
  if (!w) return new Map();
  if (!w.mapped) return new Map();
  if ((globalThis as { __EMX11_TRACE_PAINT__?: boolean }).__EMX11_TRACE_PAINT__) {
    console.log('[paint] unmapWindow', id, 'parent=', w.parent);
  }
  const oldClips = snapshotClips(r);
  w.mapped = false;
  recomputeClipsAll(r);
  /* Lower-z siblings whose clipLists just absorbed this window's old
   * area get newClip > oldClip in the vacated rect; paintExposedRegions
   * paints their bg there. The unmapped window itself has newClip=empty
   * so contributes nothing to repainting -- correct, it's leaving. */
  return paintExposedRegions(r, oldClips);
}

export function destroyWindow(r: RendererState, id: number): Map<number, Region> {
  const w = r.windows.get(id);
  if (!w) return new Map();
  const oldClips = snapshotClips(r);
  r.windows.delete(id);
  recomputeClipsAll(r);
  /* Same shape as unmap: lower windows reclaim the freed area through
   * their newClip - oldClip. The destroyed window is gone from
   * r.windows so paintExposedRegions skips it. */
  return paintExposedRegions(r, oldClips);
}
