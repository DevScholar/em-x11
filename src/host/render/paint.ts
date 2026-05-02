/**
 * Internal paint helpers. Mirrors xserver/mi/mipaintwin.c (paintWindow,
 * miPaintWindow) plus the per-window clip stacking that real X does
 * implicitly through the per-window backing pixmap. We don't have backing
 * store, so every drawing op walks the parent chain to install one
 * canvas clip per ancestor.
 */

import type { RendererState, ManagedWindow } from './types.js';
import { absOrigin } from './window-tree.js';
import { MAX_PARENT_WALK } from '../constants.js';
import { pixelToCssColor, type RootCanvasContext } from '../../runtime/canvas.js';

/** Children of `parentId` sorted bottom-to-top by stackOrder. */
function sortedChildren(r: RendererState, parentId: number): ManagedWindow[] {
  const out: ManagedWindow[] = [];
  for (const w of r.windows.values()) {
    if (w.parent === parentId) out.push(w);
  }
  out.sort((a, b) => a.stackOrder - b.stackOrder);
  return out;
}

/** Push the absolute bounding rect (content + border) of `w` if mapped. */
function pushOccluderRect(
  r: RendererState,
  w: ManagedWindow,
  out: Array<{ ax: number; ay: number; w: number; h: number }>,
): void {
  if (!w.mapped) return;
  const { ax, ay } = absOrigin(r, w);
  const bw = w.borderWidth;
  out.push({
    ax: ax - bw,
    ay: ay - bw,
    w: w.width + 2 * bw,
    h: w.height + 2 * bw,
  });
}

/** Collect rect of `root` and every mapped descendant. Border included
 *  so the ring counts. Recursion is needed because a child can sit at
 *  a coordinate that, while inside its parent's *paint clip*, is not
 *  fully inside the parent's bounding rect once you account for shape
 *  and our coarse rect-only model: missing the descendant lets paints
 *  underneath leak into spots the descendant covers. */
function collectMappedSubtreeRects(
  r: RendererState,
  root: ManagedWindow,
  out: Array<{ ax: number; ay: number; w: number; h: number }>,
): void {
  pushOccluderRect(r, root, out);
  if (!root.mapped) return;
  for (const child of r.windows.values()) {
    if (child.parent === root.id) collectMappedSubtreeRects(r, child, out);
  }
}

/** Compute occluder rects for `win`: every window that paints AFTER `win`
 *  in the global stacking order, walking up `win`'s parent chain.
 *
 *  At each level, siblings of the path-window with higher `stackOrder`
 *  render later (their subtrees paint over `win`); their entire mapped
 *  subtree is added as an occluder. Used to subtract covered areas from
 *  the canvas clip so that draws into `win` don't leak over windows
 *  that are visually above it.
 *
 *  Real X computes the full visible region taking shapes into account;
 *  we use bounding rects only. Conservative: over-subtracts when
 *  occluders are shaped, which costs pixels (drawings clipped out of
 *  shape-holes) but never paints into wrong areas. */
function getOccluderRects(
  r: RendererState,
  win: ManagedWindow,
): Array<{ ax: number; ay: number; w: number; h: number }> {
  const out: Array<{ ax: number; ay: number; w: number; h: number }> = [];
  let current: ManagedWindow | undefined = win;
  for (let i = 0; current && current.parent !== 0 && i < MAX_PARENT_WALK; i++) {
    const parentId = current.parent;
    const myStackOrder = current.stackOrder;
    const myId = current.id;
    for (const sibling of r.windows.values()) {
      if (sibling.parent !== parentId) continue;
      if (sibling.id === myId) continue;
      if (sibling.stackOrder <= myStackOrder) continue;
      collectMappedSubtreeRects(r, sibling, out);
    }
    current = r.windows.get(parentId);
  }
  return out;
}

/** Subtract higher-z sibling rects from the current canvas clip. We
 *  apply ONE occluder per ctx.clip() call using evenodd on (canvas +
 *  occluder), instead of stuffing them all into a single path: with
 *  many overlapping/nested rects the evenodd parity flips back to
 *  "inside" wherever an odd number of rects overlap (a frame + its
 *  shell + a widget = 3 = inside), leaving holes in the occluder that
 *  let lower-z paints leak through. Per-occluder clipping is robust
 *  to nesting because each call only ever touches 2 rects. Caller
 *  must have already called ctx.save() and installed the window's
 *  own/ancestor clip; this further intersects with each (canvas -
 *  one_occluder). No-op when nothing occludes. */
function applyOcclusionClip(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
): void {
  const occluders = getOccluderRects(r, win);
  if (occluders.length === 0) return;
  const cw = r.canvas.cssWidth;
  const ch = r.canvas.cssHeight;
  for (const o of occluders) {
    ctx.beginPath();
    ctx.rect(0, 0, cw, ch);
    ctx.rect(o.ax, o.ay, o.w, o.h);
    ctx.clip('evenodd');
  }
}

/** Push a clip region matching the window's visible area onto `ctx`.
 *  Caller must have done ctx.save() and must ctx.restore() after.
 *
 *  Walks the parent chain: real X clips a child to every ancestor's
 *  bounding shape. Our renderer is flat (no native parent/child),
 *  so we enforce this explicitly. Without it, a shell window with
 *  SHAPE (xeyes) has its eye-cutout covered by the Eyes widget child's
 *  full-rectangle bg paint, because the child's own clip is just its
 *  rectangle.
 *
 *  Canvas 2D clip semantics: successive ctx.clip() calls intersect
 *  with the existing clip, so we emit one clip per chain level and
 *  they combine correctly. Uses absOrigin for each level so coords
 *  in reparented sub-trees stay consistent with the paint path. */
export function applyWindowClip(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
): void {
  const chain: ManagedWindow[] = [win];
  let parentId = win.parent;
  for (let i = 0; parentId !== 0 && i < MAX_PARENT_WALK; i++) {
    const p = r.windows.get(parentId);
    if (!p) break;
    chain.push(p);
    parentId = p.parent;
  }

  for (let i = chain.length - 1; i >= 0; i--) {
    const w = chain[i]!;
    const { ax, ay } = absOrigin(r, w);
    ctx.beginPath();
    if (w.shape) {
      for (const r of w.shape) {
        ctx.rect(ax + r.x, ay + r.y, r.w, r.h);
      }
    } else {
      ctx.rect(ax, ay, w.width, w.height);
    }
    ctx.clip();
  }
  applyOcclusionClip(r, ctx, win);
}

/** Like applyWindowClip but skips the window's own bounding rect.
 *  Used for painting the border ring, which lives outside the
 *  window's content rect but must still be clipped to every ancestor
 *  so it doesn't bleed past the parent's bounds. */
export function applyAncestorClip(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
): void {
  const chain: ManagedWindow[] = [];
  let parentId = win.parent;
  for (let i = 0; parentId !== 0 && i < MAX_PARENT_WALK; i++) {
    const p = r.windows.get(parentId);
    if (!p) break;
    chain.push(p);
    parentId = p.parent;
  }
  for (let i = chain.length - 1; i >= 0; i--) {
    const w = chain[i]!;
    const { ax, ay } = absOrigin(r, w);
    ctx.beginPath();
    if (w.shape) {
      for (const rect of w.shape) {
        ctx.rect(ax + rect.x, ay + rect.y, rect.w, rect.h);
      }
    } else {
      ctx.rect(ax, ay, w.width, w.height);
    }
    ctx.clip();
  }
  applyOcclusionClip(r, ctx, win);
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
