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
import { paintWindowBorder, paintWindowSubtree, repaintAbsoluteRect } from './paint.js';

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
  });
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
): void {
  const w = r.windows.get(id);
  if (!w) return;
  const oldBw = w.borderWidth;
  const widthChanged = oldBw !== borderWidth;
  w.borderWidth = borderWidth;
  w.borderPixel = borderPixel;
  if (!w.mapped) return;
  if ((globalThis as { __EMX11_TRACE_PAINT__?: boolean }).__EMX11_TRACE_PAINT__) {
    console.log('[paint] setWindowBorder', id, 'widthChanged=', widthChanged);
  }
  if (!widthChanged) {
    /* Color-only change: ring is in the same place, just different
     * pixel. Repaint just the ring. paintWindowBorder is ancestor-
     * clipped so it can't bleed past the parent. */
    paintWindowBorder(r, r.canvas.ctx, w);
    return;
  }
  /* Width changed: fall through to the old wipe-and-repaint. The
   * subtree will lose its drawn content; descendants need an Expose
   * burst from the caller (WindowManager) to redraw. Not currently
   * exercised by twm focus-follow path. */
  const { ax, ay } = absOrigin(r, w);
  const maxBw = Math.max(oldBw, borderWidth);
  repaintAbsoluteRect(
    r,
    ax - maxBw,
    ay - maxBw,
    w.width + 2 * maxBw,
    w.height + 2 * maxBw,
  );
  paintWindowSubtree(r, w);
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
 *  Repaint strategy: erase the OLD absolute rect (paints whatever's
 *  underneath -- root + ancestors + still-mapped siblings that
 *  intersect), then paint the moved window's subtree at the NEW
 *  absolute position. The window's app-level drawings are gone; the C
 *  side synthesizes an Expose in notify_js_reconfigure so the client
 *  redraws content. Children that moved with their parent ALSO lost
 *  their drawings -- they don't get Expose under our model (real X
 *  has backing store; we don't), accepted as a known visual gap. */
export function configureWindow(
  r: RendererState,
  id: number,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  const win = r.windows.get(id);
  if (!win) return;
  let oldRect: { ax: number; ay: number; w: number; h: number } | null = null;
  if (win.mapped) {
    const { ax, ay } = absOrigin(r, win);
    const bw = win.borderWidth;
    oldRect = { ax: ax - bw, ay: ay - bw, w: win.width + 2 * bw, h: win.height + 2 * bw };
  }
  const sameGeom =
    win.x === x && win.y === y && win.width === w && win.height === h;
  win.x = x;
  win.y = y;
  win.width = w;
  win.height = h;
  /* No-op configure (twm sends XMoveWindow / XConfigureWindow with the
   * current geometry on click-without-drag once the move loop exits
   * with a synthetic SetupWindow call). Without this guard our wipe
   * runs on a no-op move and erases descendant content (Xaw button
   * labels) without sending Expose -- same shape as the setWindowBorder
   * over-wipe fix. */
  if (sameGeom) return;
  if ((globalThis as { __EMX11_TRACE_PAINT__?: boolean }).__EMX11_TRACE_PAINT__) {
    console.log('[paint] configureWindow', id, '(', x, y, w, h, ')');
  }
  if (oldRect) {
    /* Erase old position. mapped is still true at this point but the
     * window now sits at the NEW position, so painting from root will
     * fill the old rect with parents/siblings, and the window's bg
     * lands at the new rect (which intersects oldRect only on a
     * shrinking move -- harmless, the new paint goes on top below). */
    repaintAbsoluteRect(r, oldRect.ax, oldRect.ay, oldRect.w, oldRect.h);
    /* Paint new position fresh. paintWindowSubtree handles children. */
    paintWindowSubtree(r, win);
  }
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
): void {
  const win = r.windows.get(id);
  if (!win) return;
  let oldRect: { ax: number; ay: number; w: number; h: number } | null = null;
  if (win.mapped) {
    const { ax, ay } = absOrigin(r, win);
    const bw = win.borderWidth;
    oldRect = { ax: ax - bw, ay: ay - bw, w: win.width + 2 * bw, h: win.height + 2 * bw };
  }
  win.parent = parent;
  win.x = x;
  win.y = y;
  if (oldRect) {
    repaintAbsoluteRect(r, oldRect.ax, oldRect.ay, oldRect.w, oldRect.h);
    paintWindowSubtree(r, win);
  }
}

/** Read-only accessor for redirect decisions in Host. Returns the
 *  parent XID or 0 when the window is unknown / parentless. */
export function parentOf(r: RendererState, id: number): number {
  return r.windows.get(id)?.parent ?? 0;
}

/** XRaiseWindow: give the window the highest stackOrder among siblings
 *  so it paints on top. Repaints the affected rect so the new order
 *  is reflected immediately. */
export function raiseWindow(r: RendererState, id: number): void {
  const w = r.windows.get(id);
  if (!w) return;
  w.stackOrder = r.stackCounter++;
  if (w.mapped && isViewable(r, id)) {
    const { ax, ay } = absOrigin(r, w);
    const bw = w.borderWidth;
    /* repaintAbsoluteRect already paints all windows in the area in
     * the new stackOrder, including this window on top. paintWindowSubtree
     * would be redundant here and re-wiping descendant content. */
    repaintAbsoluteRect(r, ax - bw, ay - bw, w.width + 2 * bw, w.height + 2 * bw);
  }
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

export function setWindowShape(r: RendererState, id: number, rects: ShapeRect[]): void {
  const w = r.windows.get(id);
  if (!w) return;
  w.shape = rects.length > 0 ? rects : null;
  /* Shape change while mapped means the visible region just changed;
   * repaint so newly-uncovered parts pick up bg and newly-covered
   * parts get clipped on next paint. */
  if (w.mapped) paintWindowSubtree(r, w);
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

export function mapWindow(r: RendererState, id: number): void {
  const w = r.windows.get(id);
  if (!w) return;
  w.mapped = true;
  if ((globalThis as { __EMX11_TRACE_PAINT__?: boolean }).__EMX11_TRACE_PAINT__) {
    console.log('[paint] mapWindow', id, 'parent=', w.parent);
  }
  /* Skip paint when an ancestor is still unmapped: the window is
   * "mapped but not viewable" in X terms and MUST NOT produce pixels
   * yet. When the ancestor later maps, paintWindowSubtree recurses
   * into this now-viewable subtree and paints it then. This prevents
   * xeyes' Xt child widgets from drawing at root-local (0,0) during
   * the brief window between child-map and shell-map. */
  if (!isViewable(r, id)) return;
  /* Sync paint: the C-side XMapWindow synthesizes an Expose to the
   * caller's queue immediately after this call returns. The Expose
   * handler runs synchronously before the next emscripten_sleep yield,
   * so we MUST have the bg down before then or the handler's drawing
   * gets overwritten on the next RAF. */
  paintWindowSubtree(r, w);
}

export function unmapWindow(r: RendererState, id: number): void {
  const w = r.windows.get(id);
  if (!w) return;
  if (!w.mapped) return;
  if ((globalThis as { __EMX11_TRACE_PAINT__?: boolean }).__EMX11_TRACE_PAINT__) {
    console.log('[paint] unmapWindow', id, 'parent=', w.parent);
  }
  const { ax, ay } = absOrigin(r, w);
  const bw = w.borderWidth;
  const oldX = ax - bw;
  const oldY = ay - bw;
  const oldW = w.width + 2 * bw;
  const oldH = w.height + 2 * bw;
  w.mapped = false;
  /* Repaint the freed area (content + border) from root downwards. */
  repaintAbsoluteRect(r, oldX, oldY, oldW, oldH);
}

export function destroyWindow(r: RendererState, id: number): void {
  const w = r.windows.get(id);
  if (!w) return;
  let oldRect: { ax: number; ay: number; w: number; h: number } | null = null;
  if (w.mapped) {
    const { ax, ay } = absOrigin(r, w);
    const bw = w.borderWidth;
    oldRect = { ax: ax - bw, ay: ay - bw, w: w.width + 2 * bw, h: w.height + 2 * bw };
  }
  r.windows.delete(id);
  if (oldRect) {
    /* Same caveat as unmapWindow: siblings overlapping the destroyed
     * window's rect lose their drawn content. */
    repaintAbsoluteRect(r, oldRect.ax, oldRect.ay, oldRect.w, oldRect.h);
  }
}
