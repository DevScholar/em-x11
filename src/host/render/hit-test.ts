/**
 * Hit testing. Mirrors xserver/mi/miwindow.c::miSpriteTrace exactly:
 *
 *   pWin = root->firstChild;
 *   while (pWin) {
 *     if (mapped && in_bbox_with_border &&
 *         (!boundingShape || PointInBorderSize) &&
 *         (!inputShape || RegionContainsPoint(inputShape, ...)) &&
 *         !unhittable) {
 *       record(pWin); pWin = pWin->firstChild;     // descend this branch
 *     } else
 *       pWin = pWin->nextSib;                       // try next sibling at same depth
 *   }
 *
 * Two notes that fall out of that algorithm:
 *
 * 1. firstChild = topmost child (highest stackOrder); nextSib walks
 *    down z. Once a sibling matches, we DESCEND into it and never
 *    revisit lower-z siblings of the same parent. That's how a popaque
 *    upper window naturally occludes lower siblings without a clipList
 *    check (xorg's hit-test does NOT consult clipList -- only bbox +
 *    border + shape).
 *
 * 2. "realized" semantics: each level checks `mapped`. A mapped-but-
 *    not-viewable descendant of an unmapped ancestor is still rejected
 *    naturally because the loop never enters its parent's subtree.
 *
 * Tracing: set `Module['emX11Debug'].traceHit = true` (L1, DevTools) or
 *   `em.debug.traceHit = true` (L2/L3, API) to log every hit-test walk.
 *   Set `traceHitNext = true` to log only the next call,
 * then have the helper auto-clear it. Useful for diagnosing "this
 * point looks like root but reports as <some widget>" symptoms (twm
 * icon-manager / hidden shells still receiving Motion).
 */
import type { RendererState, ManagedWindow } from './types.js';
import { absOrigin } from './window-tree.js';
import { getDebugFlags } from '../../runtime/debug-flags.js';

interface TraceCtx {
  enabled: boolean;
  log: string[];
}

/* Trace flags read fresh on every call so DevTools toggling is live.
 * In L1 (--pre-js inside factory) set Module['emX11Debug'].traceHitNext
 * from DevTools. In L2/L3 use em.debug.traceHitNext from the API. */
export function findWindowAt(r: RendererState, cssX: number, cssY: number): number | null {
  const dbg = getDebugFlags();
  const oneShot = !!dbg?.traceHitNext;
  const trace: TraceCtx = {
    enabled: !!dbg?.traceHit || oneShot,
    log: [],
  };

  const result = traceWalk(r, cssX, cssY, trace);

  if (trace.enabled) {
    console.log(
      `[hit] (${cssX.toFixed(1)}, ${cssY.toFixed(1)}) -> ${result === null ? 'null' : result}`,
    );
    for (const line of trace.log) console.log('  ' + line);
  }
  if (oneShot && dbg) dbg.traceHitNext = false;
  return result;
}

/** Print every mapped window's bbox/shape/clipList state in z-order.
 *  Wired into `em.debug.dumpWindows()` by api/debug.ts. */
export function dumpWindows(r: RendererState): void {
  const all = [...r.windows.values()].sort((a, b) => a.stackOrder - b.stackOrder);
  console.log(`[dump] ${all.length} windows total`);
  for (const w of all) {
    const { ax, ay } = absOrigin(r, w);
    console.log(
      `  #${w.id} parent=${w.parent} stack=${w.stackOrder} mapped=${w.mapped}` +
        ` bbox=(${ax},${ay},${w.width}x${w.height}+${w.borderWidth})` +
        ` bg=${w.bgType} shape=${w.shape ? w.shape.length + 'rects' : 'none'}` +
        ` clipList=${w.clipList.length}rects`,
    );
  }
}

/** @deprecated kept while host/index.ts is migrated; api/debug.ts calls
 *  dumpWindows() directly via the renderer reference it holds. */
export function installDumpHelper(_r: RendererState): void {
  /* No-op: dump is now reached via em.debug.dumpWindows(). */
}

/** Mirrors miSpriteTrace: descend the topmost-matching child at each
 *  level. Result is the deepest matched window (== sprite trace tail). */
function traceWalk(
  r: RendererState,
  x: number,
  y: number,
  trace: TraceCtx,
): number | null {
  let deepest: number | null = null;
  /* Top-level walk: pretend a synthetic "root" with all parent===0 windows
   * as its children, so we can use the same inner loop. */
  const tops = childrenTopDown(r, 0);
  let cursor: ManagedWindow | undefined = tops[0];
  let frontier: ManagedWindow[] = tops; /* current sibling row, top-down */
  let idx = 0;

  while (cursor) {
    const verdict = testWindow(r, cursor, x, y, trace);
    if (verdict.match) {
      deepest = cursor.id;
      const kids = childrenTopDown(r, cursor.id);
      if (kids.length === 0) break;
      frontier = kids;
      idx = 0;
      cursor = frontier[0];
    } else {
      idx += 1;
      cursor = frontier[idx];
    }
  }
  return deepest;
}

function testWindow(
  r: RendererState,
  w: ManagedWindow,
  x: number,
  y: number,
  trace: TraceCtx,
): { match: boolean } {
  const why: string[] = [];
  if (!w.mapped) {
    if (trace.enabled) trace.log.push(`#${w.id} reject: !mapped`);
    return { match: false };
  }
  const { ax, ay } = absOrigin(r, w);
  const bw = w.borderWidth;
  /* xorg includes the border ring in the bbox test (miSpriteTrace:757-762
   * uses x - wBorderWidth ... x + width + wBorderWidth). */
  const inBbox =
    x >= ax - bw && x < ax + w.width + bw && y >= ay - bw && y < ay + w.height + bw;
  if (!inBbox) {
    if (trace.enabled) {
      trace.log.push(
        `#${w.id} reject: outside bbox (ax=${ax}, ay=${ay}, w=${w.width}, h=${w.height}, bw=${bw})`,
      );
    }
    return { match: false };
  }
  why.push(`bbox(${ax},${ay},${w.width},${w.height}+${bw})`);
  if (w.shape) {
    const lx = x - ax;
    const ly = y - ay;
    let inside = false;
    for (const sh of w.shape) {
      if (lx >= sh.x && lx < sh.x + sh.w && ly >= sh.y && ly < sh.y + sh.h) {
        inside = true;
        break;
      }
    }
    if (!inside) {
      if (trace.enabled) trace.log.push(`#${w.id} reject: outside boundingShape`);
      return { match: false };
    }
    why.push('shape');
  }
  if (trace.enabled) {
    trace.log.push(`#${w.id} match: ${why.join(' & ')} bg=${w.bgType} mapped`);
  }
  return { match: true };
}

/** Children of `parentId`, top-most first (descending stackOrder). Mirrors
 *  miSpriteTrace's `firstChild` start + `nextSib` walk: xorg keeps children
 *  in z-order with firstChild at the top and uses ->nextSib for downward z
 *  traversal. */
function childrenTopDown(r: RendererState, parentId: number): ManagedWindow[] {
  const out: ManagedWindow[] = [];
  for (const w of r.windows.values()) {
    if (w.parent === parentId) out.push(w);
  }
  out.sort((a, b) => b.stackOrder - a.stackOrder);
  return out;
}
