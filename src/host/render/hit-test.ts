/**
 * Hit testing. Mirrors xserver/mi/mipointrloc.c (XYToWindow): given
 * canvas-absolute coords, find the topmost mapped (and SHAPE-included)
 * window under the point. Walks the tree in the same parent-before-child
 * DFS order paint uses, so the visually-topmost window wins.
 */

import type { RendererState, ManagedWindow } from './types.js';
import { absOrigin } from './window-tree.js';

export function findWindowAt(r: RendererState, cssX: number, cssY: number): number | null {
  /* Hit test in the same parent-before-child DFS order used for
   * paint, so the topmost painted window under the cursor wins.
   * Honours SHAPE: a point outside the shape rectangles doesn't hit.
   * Uses absOrigin so reparented windows land where they're drawn. */
  let hit: number | null = null;
  const probe = (w: ManagedWindow): void => {
    if (w.mapped) {
      const { ax, ay } = absOrigin(r, w);
      if (
        cssX >= ax &&
        cssX < ax + w.width &&
        cssY >= ay &&
        cssY < ay + w.height
      ) {
        let inside = true;
        if (w.shape) {
          inside = false;
          const lx = cssX - ax;
          const ly = cssY - ay;
          for (const r of w.shape) {
            if (lx >= r.x && lx < r.x + r.w && ly >= r.y && ly < r.y + r.h) {
              inside = true;
              break;
            }
          }
        }
        if (inside) hit = w.id;
      }
    }
    for (const child of r.windows.values()) {
      if (child.parent === w.id) probe(child);
    }
  };
  for (const w of r.windows.values()) {
    if (w.parent === 0) probe(w);
  }
  return hit;
}
