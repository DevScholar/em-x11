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
  for (let i = 0; parentId !== 0 && i < 32; i++) {
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
  for (let i = 0; parentId !== 0 && i < 32; i++) {
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
}

/** Paint a window's background, then recurse into its mapped children
 *  in insertion order (closest we have to X stacking order). DFS in
 *  parent-before-child order matches X: a child must paint over its
 *  parent's background, never the other way around. After a reparent
 *  the Map's insertion order is no longer tree order, so flat
 *  iteration would put the wrong thing on top. */
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
  for (const child of r.windows.values()) {
    if (child.parent === w.id) paintWindowSubtree(r, child);
  }
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
 *  background: solid colour, or tile pattern when backgroundPixmap is
 *  bound. Tile origin is the window's absolute top-left, so moving
 *  the window (or its parent) shifts the tile with it. Caller owns
 *  ctx.save/restore and clipping. */
export function paintBackgroundRect(
  r: RendererState,
  ctx: RootCanvasContext,
  win: ManagedWindow,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  const { ax, ay } = absOrigin(r, win);
  const pmId = win.backgroundPixmap;
  if (pmId !== null) {
    const pmCanvas = r.pixmapLookup(pmId);
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
    for (const child of r.windows.values()) {
      if (child.parent === win.id) visit(child);
    }
  };
  for (const win of r.windows.values()) {
    if (win.parent === 0) visit(win);
  }
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
