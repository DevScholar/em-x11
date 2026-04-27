/**
 * Software compositor.
 *
 * Owns the list of mapped X windows, their positions, sizes, and per-window
 * pixel buffers. Paints onto the RootCanvas synchronously at the moment a
 * structural change happens (mapWindow / setWindowBackgroundPixmap /
 * setWindowShape) -- no RAF/markDirty cycle.
 *
 * Why synchronous: an RAF-deferred present() that wipes the canvas and
 * repaints window backgrounds runs AFTER the wasm client's Expose handler
 * (which fires synchronously between XMapWindow and the next emscripten_sleep
 * yield). The wipe then erases application drawings -- xeyes paints its
 * eye sockets in Expose, present() runs on the next browser frame, sockets
 * disappear, the canvas-bg colour shows through where the SHAPE clip
 * passes, and pupil moves leave white trails on the now-black sockets.
 *
 * Sync paint puts the background down BEFORE the Expose is processed, so
 * the application's drawings layer on top and persist. Real X has the same
 * ordering guarantee via per-window backing store; we approximate it by
 * touching the canvas at the same instant the C side requests it. Edge
 * cases (move/resize, unmap leaving ghost regions) need a full backing-
 * store rewrite; for now they're tolerated -- xeyes/xt-hello/xaw-hello
 * don't trip them.
 */

import type { RootCanvas } from './canvas.js';
import { pixelToCssColor } from './canvas.js';
import type { Point, ShapeRect } from '../types/emscripten.js';

interface ManagedWindow {
  id: number;
  /** Parent window id. 0 (None) means "no parent" — the root window. */
  parent: number;
  x: number;
  y: number;
  width: number;
  height: number;
  background: number;
  /** When non-null, the window background is tiled with this pixmap's
   *  OffscreenCanvas instead of painted with `background`. Tile origin
   *  is the window's top-left (applied via ctx.translate at paint time). */
  backgroundPixmap: number | null;
  mapped: boolean;
  /** SHAPE bounding rectangles (window-local coords). `null` means
   *  unshaped -- the window is a plain rectangle of (width, height). */
  shape: ShapeRect[] | null;
}

/** Callback the Host supplies so the compositor can reach into the
 *  pixmap table without importing Host (which would be circular). */
type PixmapLookup = (id: number) => OffscreenCanvas | null;

export class Compositor {
  private readonly canvas: RootCanvas;
  private readonly pixmapLookup: PixmapLookup;
  private readonly windows = new Map<number, ManagedWindow>();

  constructor(canvas: RootCanvas, pixmapLookup: PixmapLookup = () => null) {
    this.canvas = canvas;
    this.pixmapLookup = pixmapLookup;
  }

  addWindow(
    id: number,
    parent: number,
    x: number,
    y: number,
    width: number,
    height: number,
    background: number,
  ): void {
    /* Create only stores state -- the window isn't visible until
     * mapWindow flips `mapped` and triggers the sync paint. */
    this.windows.set(id, {
      id,
      parent,
      x,
      y,
      width,
      height,
      background,
      backgroundPixmap: null,
      mapped: false,
      shape: null,
    });
  }

  setWindowBackgroundPixmap(id: number, pmId: number): void {
    const w = this.windows.get(id);
    if (!w) return;
    w.backgroundPixmap = pmId > 0 ? pmId : null;
    /* Repaint sync if the window is currently visible: shared-root
     * setup calls this AFTER mapWindow(root), so we need the weave
     * to land without waiting for some external trigger. */
    if (w.mapped) this.paintWindowSubtree(w);
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
  configureWindow(id: number, x: number, y: number, w: number, h: number): void {
    const win = this.windows.get(id);
    if (!win) return;
    let oldRect: { ax: number; ay: number; w: number; h: number } | null = null;
    if (win.mapped) {
      const { ax, ay } = this.absOrigin(win);
      oldRect = { ax, ay, w: win.width, h: win.height };
    }
    win.x = x;
    win.y = y;
    win.width = w;
    win.height = h;
    if (oldRect) {
      /* Erase old position. mapped is still true at this point but the
       * window now sits at the NEW position, so painting from root will
       * fill the old rect with parents/siblings, and the window's bg
       * lands at the new rect (which intersects oldRect only on a
       * shrinking move -- harmless, the new paint goes on top below). */
      this.repaintAbsoluteRect(oldRect.ax, oldRect.ay, oldRect.w, oldRect.h);
      /* Paint new position fresh. paintWindowSubtree handles children. */
      this.paintWindowSubtree(win);
    }
  }

  /** XReparentWindow: change a window's parent link and local origin.
   *  Does not affect mapped state. Unknown id is a no-op (cross-
   *  connection callers may race ahead of the owner's create).
   *
   *  In a real X server reparenting a mapped window implicitly unmaps
   *  (erasing the old pixel rect on the old parent) and remaps (painting
   *  background at the new parent's coordinate system, then sending
   *  Expose) -- x11protocol.txt §1040. Our compositor has no backing
   *  store, so the canvas under the old position has to be reconstructed
   *  from the window tree and the new position has to be painted fresh.
   *  Without this, twm reparenting xeyes' shell left the xeyes drawing
   *  stranded at its original root-relative coordinates while the new
   *  frame+shell composite never actually hit the canvas. */
  reparentWindow(id: number, parent: number, x: number, y: number): void {
    const win = this.windows.get(id);
    if (!win) return;
    let oldRect: { ax: number; ay: number; w: number; h: number } | null = null;
    if (win.mapped) {
      const { ax, ay } = this.absOrigin(win);
      oldRect = { ax, ay, w: win.width, h: win.height };
    }
    win.parent = parent;
    win.x = x;
    win.y = y;
    if (oldRect) {
      this.repaintAbsoluteRect(oldRect.ax, oldRect.ay, oldRect.w, oldRect.h);
      this.paintWindowSubtree(win);
    }
  }

  /** Read-only accessor for redirect decisions in Host. Returns the
   *  parent XID or 0 when the window is unknown / parentless. */
  parentOf(id: number): number {
    return this.windows.get(id)?.parent ?? 0;
  }

  /** Enumerate the mapped descendants of `id` in parent-before-child DFS
   *  order. Used by Host to synthesize Expose events for every visible
   *  window whose content was wiped by a subtree repaint (map, move,
   *  resize). `id` itself is NOT included. */
  mappedDescendants(id: number): number[] {
    const out: number[] = [];
    const recurse = (parentId: number): void => {
      for (const child of this.windows.values()) {
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
  geometryOf(id: number): { width: number; height: number } | null {
    const w = this.windows.get(id);
    return w ? { width: w.width, height: w.height } : null;
  }

  /** Full authoritative attribute snapshot for cross-connection
   *  XGetWindowAttributes. Local shadows drift (a WM never mirrors
   *  another client's window), so when a client queries a window it
   *  doesn't own, Xlib falls back to this via the JS bridge.
   *  Per dix/window.c, window state is server-authoritative by XID;
   *  this accessor is how we present that view without a full
   *  refactor of the per-client shadow tables. */
  attrsOf(id: number): {
    x: number; y: number; width: number; height: number;
    mapped: boolean; parent: number;
  } | null {
    const w = this.windows.get(id);
    if (!w) return null;
    return { x: w.x, y: w.y, width: w.width, height: w.height,
             mapped: w.mapped, parent: w.parent };
  }

  /** Resolve a window's origin to canvas-absolute coordinates by
   *  summing local (x, y) up the parent chain. ManagedWindow.{x,y} is
   *  local-to-parent (matching X semantics); the compositor needs
   *  absolute to actually paint. Root is at (0, 0) so the chain
   *  terminates cleanly. Guarded to 32 levels against cycles. */
  private absOrigin(win: ManagedWindow): { ax: number; ay: number } {
    let ax = win.x;
    let ay = win.y;
    let pid = win.parent;
    for (let i = 0; pid !== 0 && i < 32; i++) {
      const p = this.windows.get(pid);
      if (!p) break;
      ax += p.x;
      ay += p.y;
      pid = p.parent;
    }
    return { ax, ay };
  }

  /** XClearWindow / XClearArea: repaint a window rectangle using whatever
   *  background the window currently has (solid or tile). Unlike
   *  `fillRect(id, ..., win.background)` this honours a bound pixmap. */
  clearArea(id: number, x: number, y: number, w: number, h: number): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped) return;
    const ctx = this.canvas.ctx;
    ctx.save();
    this.applyWindowClip(ctx, win);
    this.paintBackgroundRect(ctx, win, x, y, w, h);
    ctx.restore();
  }

  setWindowShape(id: number, rects: ShapeRect[]): void {
    const w = this.windows.get(id);
    if (!w) return;
    w.shape = rects.length > 0 ? rects : null;
    /* Shape change while mapped means the visible region just changed;
     * repaint so newly-uncovered parts pick up bg and newly-covered
     * parts get clipped on next paint. */
    if (w.mapped) this.paintWindowSubtree(w);
  }

  mapWindow(id: number): void {
    const w = this.windows.get(id);
    if (!w) return;
    w.mapped = true;
    /* Sync paint: the C-side XMapWindow synthesizes an Expose to the
     * caller's queue immediately after this call returns. The Expose
     * handler runs synchronously before the next emscripten_sleep yield,
     * so we MUST have the bg down before then or the handler's drawing
     * gets overwritten on the next RAF. */
    this.paintWindowSubtree(w);
  }

  unmapWindow(id: number): void {
    const w = this.windows.get(id);
    if (!w) return;
    if (!w.mapped) return;
    const { ax, ay } = this.absOrigin(w);
    const oldW = w.width;
    const oldH = w.height;
    w.mapped = false;
    /* Repaint the freed area from root downwards. The just-unmapped
     * window is skipped (mapped=false), so the parent + still-mapped
     * siblings get to fill in. Note: any sibling whose drawing
     * intersected this rect loses it (we paint sibling backgrounds,
     * not their app content) -- a backing-store rewrite would fix
     * that. Current demos don't have overlapping siblings. */
    this.repaintAbsoluteRect(ax, ay, oldW, oldH);
  }

  destroyWindow(id: number): void {
    const w = this.windows.get(id);
    if (!w) return;
    let oldRect: { ax: number; ay: number; w: number; h: number } | null = null;
    if (w.mapped) {
      const { ax, ay } = this.absOrigin(w);
      oldRect = { ax, ay, w: w.width, h: w.height };
    }
    this.windows.delete(id);
    if (oldRect) {
      /* Same caveat as unmapWindow: siblings overlapping the destroyed
       * window's rect lose their drawn content. */
      this.repaintAbsoluteRect(oldRect.ax, oldRect.ay, oldRect.w, oldRect.h);
    }
  }

  fillRect(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    color: number,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped) return;
    const ctx = this.canvas.ctx;
    const { ax, ay } = this.absOrigin(win);
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.fillStyle = pixelToCssColor(color);
    ctx.fillRect(ax + x, ay + y, w, h);
    ctx.restore();
  }

  drawLine(
    id: number,
    x1: number,
    y1: number,
    x2: number,
    y2: number,
    color: number,
    lineWidth: number,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped) return;
    const ctx = this.canvas.ctx;
    const { ax, ay } = this.absOrigin(win);
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.strokeStyle = pixelToCssColor(color);
    ctx.lineWidth = lineWidth || 1;
    ctx.beginPath();
    ctx.moveTo(ax + x1 + 0.5, ay + y1 + 0.5);
    ctx.lineTo(ax + x2 + 0.5, ay + y2 + 0.5);
    ctx.stroke();
    ctx.restore();
  }

  drawArc(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
    color: number,
    lineWidth: number,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped) return;
    const ctx = this.canvas.ctx;
    const { ax, ay } = this.absOrigin(win);
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.strokeStyle = pixelToCssColor(color);
    ctx.lineWidth = lineWidth || 1;
    this.arcPath(ctx, ax + x, ay + y, w, h, angle1, angle2);
    ctx.stroke();
    ctx.restore();
  }

  fillArc(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
    color: number,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped) return;
    const ctx = this.canvas.ctx;
    const { ax, ay } = this.absOrigin(win);
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.fillStyle = pixelToCssColor(color);
    this.arcPath(ctx, ax + x, ay + y, w, h, angle1, angle2);
    ctx.fill();
    ctx.restore();
  }

  fillPolygon(
    id: number,
    points: Point[],
    _shape: number,
    _mode: number,
    color: number,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped || points.length < 3) return;
    const ctx = this.canvas.ctx;
    const { ax, ay } = this.absOrigin(win);
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.fillStyle = pixelToCssColor(color);
    ctx.beginPath();
    const first = points[0]!;
    ctx.moveTo(ax + first.x, ay + first.y);
    for (let i = 1; i < points.length; i++) {
      const p = points[i]!;
      ctx.lineTo(ax + p.x, ay + p.y);
    }
    ctx.closePath();
    ctx.fill();
    ctx.restore();
  }

  drawPoints(id: number, points: Point[], _mode: number, color: number): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped || points.length === 0) return;
    const ctx = this.canvas.ctx;
    const { ax, ay } = this.absOrigin(win);
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.fillStyle = pixelToCssColor(color);
    for (const p of points) {
      ctx.fillRect(ax + p.x, ay + p.y, 1, 1);
    }
    ctx.restore();
  }

  drawString(
    id: number,
    x: number,
    y: number,
    font: string,
    text: string,
    fgColor: number,
    bgColor: number,
    imageMode: boolean,
  ): void {
    const win = this.windows.get(id);
    if (!win || !win.mapped || text.length === 0) return;
    const ctx = this.canvas.ctx;
    const { ax, ay } = this.absOrigin(win);
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.font = font;
    ctx.textBaseline = 'alphabetic';
    ctx.textAlign = 'left';
    if (imageMode) {
      const metrics = ctx.measureText(text);
      const ascent =
        metrics.fontBoundingBoxAscent ?? metrics.actualBoundingBoxAscent ?? 10;
      const descent =
        metrics.fontBoundingBoxDescent ?? metrics.actualBoundingBoxDescent ?? 2;
      ctx.fillStyle = pixelToCssColor(bgColor);
      ctx.fillRect(
        ax + x,
        ay + y - ascent,
        metrics.width,
        ascent + descent,
      );
    }
    ctx.fillStyle = pixelToCssColor(fgColor);
    ctx.fillText(text, ax + x, ay + y);
    ctx.restore();
  }

  /** XCopyArea source half: grab an (x,y,w,h) rectangle from the window's
   *  painted area on the root canvas and paint it into `dstCtx` at
   *  (dstX,dstY). Coords are window-local on the source side. Returns
   *  silently when the window is unknown or unmapped -- match X semantics
   *  of "unpainted source = zero-filled result" by leaving dstCtx alone
   *  (callers that care can clear first). */
  blitWindowTo(
    srcId: number,
    srcX: number,
    srcY: number,
    w: number,
    h: number,
    dstCtx: OffscreenCanvasRenderingContext2D,
    dstX: number,
    dstY: number,
  ): void {
    const win = this.windows.get(srcId);
    if (!win || !win.mapped) return;
    const { ax, ay } = this.absOrigin(win);
    dstCtx.drawImage(
      this.canvas.ctx.canvas as unknown as CanvasImageSource,
      ax + srcX,
      ay + srcY,
      w,
      h,
      dstX,
      dstY,
      w,
      h,
    );
  }

  /** XCopyArea destination half: draw an image source rectangle onto the
   *  root canvas clipped to the destination window. */
  blitImageToWindow(
    dstId: number,
    dstX: number,
    dstY: number,
    src: CanvasImageSource,
    srcX: number,
    srcY: number,
    w: number,
    h: number,
  ): void {
    const win = this.windows.get(dstId);
    if (!win || !win.mapped) return;
    const { ax, ay } = this.absOrigin(win);
    const ctx = this.canvas.ctx;
    ctx.save();
    this.applyWindowClip(ctx, win);
    ctx.drawImage(src, srcX, srcY, w, h, ax + dstX, ay + dstY, w, h);
    ctx.restore();
  }

  findWindowAt(cssX: number, cssY: number): number | null {
    /* Hit test in the same parent-before-child DFS order used for
     * paint, so the topmost painted window under the cursor wins.
     * Honours SHAPE: a point outside the shape rectangles doesn't hit.
     * Uses absOrigin so reparented windows land where they're drawn. */
    let hit: number | null = null;
    const probe = (w: ManagedWindow): void => {
      if (w.mapped) {
        const { ax, ay } = this.absOrigin(w);
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
      for (const child of this.windows.values()) {
        if (child.parent === w.id) probe(child);
      }
    };
    for (const w of this.windows.values()) {
      if (w.parent === 0) probe(w);
    }
    return hit;
  }

  /** Push a clip region matching the window's visible area onto `ctx`.
   *  Caller must have done ctx.save() and must ctx.restore() after.
   *
   *  Walks the parent chain: real X clips a child to every ancestor's
   *  bounding shape. Our compositor is flat (no native parent/child),
   *  so we enforce this explicitly. Without it, a shell window with
   *  SHAPE (xeyes) has its eye-cutout covered by the Eyes widget child's
   *  full-rectangle bg paint, because the child's own clip is just its
   *  rectangle.
   *
   *  Canvas 2D clip semantics: successive ctx.clip() calls intersect
   *  with the existing clip, so we emit one clip per chain level and
   *  they combine correctly. Uses absOrigin for each level so coords
   *  in reparented sub-trees stay consistent with the paint path. */
  private applyWindowClip(ctx: CanvasRenderingContext2D, win: ManagedWindow): void {
    const chain: ManagedWindow[] = [win];
    let parentId = win.parent;
    for (let i = 0; parentId !== 0 && i < 32; i++) {
      const p = this.windows.get(parentId);
      if (!p) break;
      chain.push(p);
      parentId = p.parent;
    }

    for (let i = chain.length - 1; i >= 0; i--) {
      const w = chain[i]!;
      const { ax, ay } = this.absOrigin(w);
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

  /** Paint a window's background, then recurse into its mapped children
   *  in insertion order (closest we have to X stacking order). DFS in
   *  parent-before-child order matches X: a child must paint over its
   *  parent's background, never the other way around. After a reparent
   *  the Map's insertion order is no longer tree order, so flat
   *  iteration would put the wrong thing on top. */
  private paintWindowSubtree(w: ManagedWindow): void {
    if (w.mapped) {
      const ctx = this.canvas.ctx;
      ctx.save();
      this.applyWindowClip(ctx, w);
      this.paintBackgroundRect(ctx, w, 0, 0, w.width, w.height);
      ctx.restore();
    }
    for (const child of this.windows.values()) {
      if (child.parent === w.id) this.paintWindowSubtree(child);
    }
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
  private repaintAbsoluteRect(
    rax: number,
    ray: number,
    rw: number,
    rh: number,
  ): void {
    if (rw <= 0 || rh <= 0) return;
    const ctx = this.canvas.ctx;
    ctx.save();
    ctx.beginPath();
    ctx.rect(rax, ray, rw, rh);
    ctx.clip();
    const visit = (win: ManagedWindow): void => {
      if (win.mapped && this.intersectsAbsRect(win, rax, ray, rw, rh)) {
        ctx.save();
        this.applyWindowClip(ctx, win);
        this.paintBackgroundRect(ctx, win, 0, 0, win.width, win.height);
        ctx.restore();
      }
      for (const child of this.windows.values()) {
        if (child.parent === win.id) visit(child);
      }
    };
    for (const win of this.windows.values()) {
      if (win.parent === 0) visit(win);
    }
    ctx.restore();
  }

  /** Axis-aligned rect overlap test, comparing the window's absolute
   *  bounds with the given rect. Used by repaintAbsoluteRect to skip
   *  windows that don't intersect the dirty area. */
  private intersectsAbsRect(
    win: ManagedWindow,
    rax: number,
    ray: number,
    rw: number,
    rh: number,
  ): boolean {
    const { ax, ay } = this.absOrigin(win);
    return (
      ax < rax + rw &&
      ax + win.width > rax &&
      ay < ray + rh &&
      ay + win.height > ray
    );
  }

  /** Build a canvas path for an X-semantics arc. Exposed as a free
   *  function below so Host can reuse it for pixmap drawing. */
  private arcPath(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1_64: number,
    angle2_64: number,
  ): void {
    arcPath(ctx, x, y, w, h, angle1_64, angle2_64);
  }

  /** Paint a (window-local) rectangle using the window's current
   *  background: solid colour, or tile pattern when backgroundPixmap is
   *  bound. Tile origin is the window's absolute top-left, so moving
   *  the window (or its parent) shifts the tile with it. Caller owns
   *  ctx.save/restore and clipping. */
  private paintBackgroundRect(
    ctx: CanvasRenderingContext2D,
    win: ManagedWindow,
    x: number,
    y: number,
    w: number,
    h: number,
  ): void {
    const { ax, ay } = this.absOrigin(win);
    const pmId = win.backgroundPixmap;
    if (pmId !== null) {
      const pmCanvas = this.pixmapLookup(pmId);
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
}

/** Build a canvas path for an X-semantics arc.
 *
 *  X arc arguments: (x, y, w, h) is the axis-aligned bounding box of the
 *  ellipse; angle1 is the start angle and angle2 is the extent, both in
 *  1/64ths of a degree, measured counterclockwise from 3 o'clock.
 *
 *  Canvas 2D ellipse arguments: centre + radii, angles in radians
 *  measured clockwise from 3 o'clock. We flip the sign on angles to
 *  switch rotational direction.
 *
 *  Exported so Host can paint arcs into pixmap OffscreenCanvases with the
 *  same semantics as the window path. */
export function arcPath(
  ctx: CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
  angle1_64: number,
  angle2_64: number,
): void {
  const cx = x + w / 2;
  const cy = y + h / 2;
  const rx = w / 2;
  const ry = h / 2;
  const toRad = Math.PI / (180 * 64);
  const start = -angle1_64 * toRad;
  const end = -(angle1_64 + angle2_64) * toRad;
  ctx.beginPath();
  ctx.ellipse(cx, cy, rx, ry, 0, start, end, angle2_64 > 0);
}
