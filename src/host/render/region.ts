/**
 * Rect-based region arithmetic for em-x11's clipList / borderClip /
 * exposed-region tracking. Mirrors the subset of pixman/RegionRec API
 * we need:
 *
 *   xserver/include/regionstr.h   typedef struct _RegRec { ... }
 *   xserver/mi/mivaltree.c        miComputeClips uses RegionIntersect /
 *                                 RegionSubtract / RegionUnion / etc.
 *   xserver/mi/miexpose.c         miSendExposures iterates RegionRects.
 *
 * Design notes (vs. real pixman):
 *
 * - We use a flat disjoint-rect list, not pixman's banded representation.
 *   Asymptotically slower (O(n*m) for ops vs. O(n+m) banded), but |n|, |m|
 *   are tiny in this codebase: a window's clipList rarely exceeds a handful
 *   of rects (one per exposed strip after a sibling overlap), and we have
 *   ~10 mapped windows on screen total. The constant-factor advantage of
 *   not having to maintain band invariants is worth the worse big-O.
 *
 * - Coordinates are absolute canvas-CSS-pixels (matching everything in
 *   `host/render/`'s `absOrigin`). Empty rects (w<=0 or h<=0) are dropped
 *   on construction; an empty Region is represented as `[]`.
 *
 * - Region invariant: returned arrays contain only disjoint, positive-size
 *   rectangles. Inputs are assumed to satisfy the same invariant; the
 *   constructors `singleton` / `fromRects` enforce it on entry. Pure
 *   functions: never mutate inputs, always return fresh arrays.
 *
 * - Coalescing: we deliberately do NOT merge adjacent rects into a single
 *   larger one. That's a perf optimisation pixman makes; for our scale it
 *   adds complexity without measurable wins, and it actively hurts
 *   debuggability (a clipList with five distinct sibling-shadowed strips
 *   reads more clearly than one merged blob). If profiling later shows
 *   clipLists growing pathologically, add a `coalesce()` pass.
 *
 * Tested via the build path (paint regression on the demo set); no unit
 * suite yet -- if we add vitest, the obvious targets are subtractRect
 * (4-strip case) and intersect/union round-trips.
 */

export interface Rect {
  /** Absolute canvas X (CSS pixels). Matches `absOrigin().ax`. */
  readonly ax: number;
  readonly ay: number;
  readonly w: number;
  readonly h: number;
}

/** Disjoint set of positive-size rectangles. Empty array == empty region. */
export type Region = readonly Rect[];

export const EMPTY_REGION: Region = Object.freeze([]);

export function isEmpty(r: Region): boolean {
  return r.length === 0;
}

/** Single-rect region, or empty when (w,h) is non-positive. */
export function singleton(ax: number, ay: number, w: number, h: number): Region {
  if (w <= 0 || h <= 0) return EMPTY_REGION;
  return [{ ax, ay, w, h }];
}

/** Build a region from an arbitrary list of rects, dropping zero-sized
 *  ones and unioning the rest. Use when the caller can't guarantee
 *  disjointness (e.g. union of mapped sibling subtree rects). */
export function fromRects(rects: ReadonlyArray<Rect>): Region {
  let out: Region = EMPTY_REGION;
  for (const r of rects) {
    if (r.w <= 0 || r.h <= 0) continue;
    out = union(out, [r]);
  }
  return out;
}

/** Translate every rect by (dx, dy). */
export function translate(r: Region, dx: number, dy: number): Region {
  if (dx === 0 && dy === 0) return r;
  return r.map(rc => ({ ax: rc.ax + dx, ay: rc.ay + dy, w: rc.w, h: rc.h }));
}

/** Tight bounding box of the region, or null when empty. Mirrors
 *  pixman's RegionExtents. */
export function extents(r: Region): Rect | null {
  if (r.length === 0) return null;
  let x1 = Infinity, y1 = Infinity, x2 = -Infinity, y2 = -Infinity;
  for (const rc of r) {
    if (rc.ax < x1) x1 = rc.ax;
    if (rc.ay < y1) y1 = rc.ay;
    if (rc.ax + rc.w > x2) x2 = rc.ax + rc.w;
    if (rc.ay + rc.h > y2) y2 = rc.ay + rc.h;
  }
  return { ax: x1, ay: y1, w: x2 - x1, h: y2 - y1 };
}

/** Two-rect intersection, or null when disjoint. */
export function intersectRect(a: Rect, b: Rect): Rect | null {
  const ax = Math.max(a.ax, b.ax);
  const ay = Math.max(a.ay, b.ay);
  const x2 = Math.min(a.ax + a.w, b.ax + b.w);
  const y2 = Math.min(a.ay + a.h, b.ay + b.h);
  if (x2 <= ax || y2 <= ay) return null;
  return { ax, ay, w: x2 - ax, h: y2 - ay };
}

/** Subtract `b` from `a`, producing up to four axis-aligned strips:
 *  top, bottom, left of overlap, right of overlap. Strips are disjoint
 *  by construction. When b doesn't intersect a, returns [a]. */
export function subtractRect(a: Rect, b: Rect): Rect[] {
  const ix = intersectRect(a, b);
  if (!ix) return [a];
  const out: Rect[] = [];
  // Top strip: full width of a, above overlap.
  if (ix.ay > a.ay) {
    out.push({ ax: a.ax, ay: a.ay, w: a.w, h: ix.ay - a.ay });
  }
  // Bottom strip: full width of a, below overlap.
  const aBottom = a.ay + a.h;
  const ixBottom = ix.ay + ix.h;
  if (ixBottom < aBottom) {
    out.push({ ax: a.ax, ay: ixBottom, w: a.w, h: aBottom - ixBottom });
  }
  // Left strip: between top/bottom of overlap, left of overlap.
  if (ix.ax > a.ax) {
    out.push({ ax: a.ax, ay: ix.ay, w: ix.ax - a.ax, h: ix.h });
  }
  // Right strip: between top/bottom of overlap, right of overlap.
  const aRight = a.ax + a.w;
  const ixRight = ix.ax + ix.w;
  if (ixRight < aRight) {
    out.push({ ax: ixRight, ay: ix.ay, w: aRight - ixRight, h: ix.h });
  }
  return out;
}

/** Region intersection. Pairwise intersect; result is disjoint because
 *  inputs are disjoint and intersection preserves containment. */
export function intersect(a: Region, b: Region): Region {
  if (a.length === 0 || b.length === 0) return EMPTY_REGION;
  const out: Rect[] = [];
  for (const ar of a) {
    for (const br of b) {
      const ix = intersectRect(ar, br);
      if (ix) out.push(ix);
    }
  }
  return out;
}

/** Subtract region `b` from region `a`. Iteratively splits each rect
 *  in `a` by every rect in `b`, accumulating strips. Disjointness
 *  preserved because each split produces disjoint strips contained in
 *  the original. */
export function subtract(a: Region, b: Region): Region {
  if (a.length === 0) return EMPTY_REGION;
  if (b.length === 0) return a.slice();
  let work: Rect[] = a.slice();
  for (const br of b) {
    const next: Rect[] = [];
    for (const ar of work) {
      for (const piece of subtractRect(ar, br)) next.push(piece);
    }
    work = next;
    if (work.length === 0) break;
  }
  return work;
}

/** Region union. Implemented as a + (b - a) so the result remains
 *  disjoint without needing to merge potentially-overlapping pairs. */
export function union(a: Region, b: Region): Region {
  if (a.length === 0) return b.slice();
  if (b.length === 0) return a.slice();
  const bMinusA = subtract(b, a);
  if (bMinusA.length === 0) return a.slice();
  return [...a, ...bMinusA];
}

/** True iff any point of `a` lies in `b`. Cheaper than computing the
 *  full intersection when only the boolean is needed. */
export function intersects(a: Region, b: Region): boolean {
  for (const ar of a) {
    for (const br of b) {
      if (intersectRect(ar, br)) return true;
    }
  }
  return false;
}

/** True iff `b` is wholly contained in `a`. */
export function contains(a: Region, b: Region): boolean {
  return subtract(b, a).length === 0;
}
