/**
 * Hit testing. Mirrors xserver/mi/mipointrloc.c (XYToWindow): given
 * canvas-absolute coords, find the topmost mapped (and SHAPE-included)
 * window under the point.
 *
 * "realized" semantics (xserver/dix/window.c::RealizeTree): a window
 * participates in the hit-test only if every ancestor up through root
 * is mapped. Recurse into children only when the parent is mapped --
 * a "mapped but not viewable" descendant of an unmapped ancestor must
 * not be reachable. Without this, twm iconify (XUnmapWindow on the
 * frame) leaves the still-mapped shell+widgets reachable through
 * hit-test: the icon-manager hides pixels but Motion/ButtonPress
 * still target the hidden widgets, which then repaint hover effects
 * onto the canvas.
 */
import type { RendererState, ManagedWindow } from './types.js';
import { absOrigin } from './window-tree.js';

export function findWindowAt(r: RendererState, cssX: number, cssY: number): number | null {
  let hit: number | null = null;
  const probe = (w: ManagedWindow): void => {
    if (!w.mapped) return; /* unmapped → skip the entire subtree */
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
    /* Stable child order matches paint order so the topmost painted
     * window under the cursor wins. */
    const children: ManagedWindow[] = [];
    for (const c of r.windows.values()) {
      if (c.parent === w.id) children.push(c);
    }
    children.sort((a, b) => a.stackOrder - b.stackOrder);
    for (const c of children) probe(c);
  };
  const roots: ManagedWindow[] = [];
  for (const w of r.windows.values()) {
    if (w.parent === 0) roots.push(w);
  }
  roots.sort((a, b) => a.stackOrder - b.stackOrder);
  for (const w of roots) probe(w);
  return hit;
}
