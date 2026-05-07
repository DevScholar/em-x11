/**
 * Internal types shared between Renderer and its helper modules.
 *
 * Putting these here (rather than in index.ts) lets helper modules under
 * host/render/ import them without pulling in the Renderer class itself,
 * which would create a circular import.
 */

import type { RootCanvas } from '../../runtime/canvas.js';
import type { ShapeRect } from '../../types/emscripten.js';
import type { Region } from './region.js';

export interface ManagedWindow {
  id: number;
  /** Parent window id. 0 (None) means "no parent" — the root window. */
  parent: number;
  x: number;
  y: number;
  width: number;
  height: number;
  /** Monotonically increasing raise counter. Siblings are painted
   *  bottom-to-top by ascending stackOrder so that higher values
   *  appear on top, matching XRaiseWindow semantics. */
  stackOrder: number;
  /** X11 server-drawn border. Lives OUTSIDE (x,y,width,height): the
   *  border ring occupies [x-bw, y-bw, w+2bw, h+2bw] in parent coords.
   *  Children's local (x,y) are still relative to the content rect's
   *  top-left (X semantics). bw=0 means no border. */
  borderWidth: number;
  borderPixel: number;
  /** X11 backgroundState (xserver/dix/window.c around line 1185):
   *    'none'           -- server does not auto-paint the bg. miPaintWindow
   *                        gates on `state != None` (xserver/mi/miwindow.c:115);
   *                        we mirror that in paintBackgroundRect. xeyes' shell
   *                        lives in this state -- the application is the only
   *                        thing that paints inside.
   *    'pixel'          -- solid fill from `background`.
   *    'pixmap'         -- tile fill from `backgroundPixmap`.
   *    'parentRelative' -- tile parent's bg into this window's backing,
   *                        aligned to parent's tile origin. Resolved
   *                        recursively if the parent is also
   *                        ParentRelative. Without this, Xt widgets that
   *                        rely on inheriting parent bg (Xaw default) end
   *                        up with transparent backings, and the compositor
   *                        cascades visibility all the way to root, leaking
   *                        the canvas-bg colour through widget areas. */
  bgType: 'none' | 'pixel' | 'pixmap' | 'parentRelative';
  background: number;
  /** When set (and bgType==='pixmap'), the window background is tiled
   *  with this pixmap's OffscreenCanvas. Tile origin is the window's
   *  top-left (applied via ctx.translate at paint time). */
  backgroundPixmap: number | null;
  mapped: boolean;
  /** SHAPE bounding rectangles (window-local coords). `null` means
   *  unshaped -- the window is a plain rectangle of (width, height). */
  shape: ShapeRect[] | null;
  /** Visible content area in absolute canvas coords, mirroring
   *  xserver `pWin->clipList` (xserver/include/windowstr.h). Empty
   *  when the window is unmapped, has an unmapped ancestor, or is
   *  fully obscured. Recomputed on every structural change by
   *  `recomputeClipsAll` -- it is the source of truth that bg paint
   *  and Expose synthesis read from in step 3. */
  clipList: Region;
  /** Visible content + border ring in absolute canvas coords,
   *  mirroring xserver `pWin->borderClip`. Always a superset of
   *  `clipList`; the border ring is `borderClip - clipList`. */
  borderClip: Region;
  /** Per-window backing pixmap, content-only (width × height in
   *  window-local coords; border lives outside and is composited
   *  separately). Mirrors real X's per-window backing store. All
   *  draw primitives paint into this in window-local coordinates;
   *  the compositor (Phase B+) reads it each frame and blits to
   *  the root canvas with stack order, clip and shape applied.
   *
   *  Phase A: dual-write target — all primitives write here in
   *  parallel to the existing root-canvas path so we can A/B verify
   *  pixel equivalence before flipping the source of truth.
   *
   *  Resized in `configureWindow` when width/height changes (old
   *  pixels carried via drawImage so in-flight content survives a
   *  resize). Released by GC when the window is destroyed. */
  backingSurface: OffscreenCanvas;
  backingCtx: OffscreenCanvasRenderingContext2D;
  /** Set true on every backing write; cleared by the compositor
   *  after a successful blit. */
  backingDirty: boolean;
  /** Region of the backing (window-local coords) that has NEVER been
   *  written to since allocation. Mirrors the "valid pixels" tracking
   *  real X servers do for backing-store windows: the server fires
   *  Expose only for unpainted areas that become visible, leaving
   *  already-painted-and-then-occluded pixels alone (the compositor
   *  re-blits them naturally).
   *
   *  Initial state on `addWindow`: the full content rect is unpainted.
   *  Subtracted from on every paint op (bg fill, draw primitive,
   *  blit). Grown back via `union` on `configureWindow` resize. The
   *  Expose dispatcher (`paintExposedRegions`) intersects newly-
   *  revealed clipList area with this region to decide whether the
   *  client needs to redraw -- if it's all in painted territory, no
   *  Expose; otherwise Expose only the unpainted portion. */
  unpaintedRegion: Region;
  /** X11 bit_gravity (xserver/dix/window.c::ResizeChildrenWinSize):
   *  controls what happens to the backing pixmap on resize.
   *    0 = ForgetGravity (X default) -- backing discarded, server
   *        sends Expose for the entire new content rect; the client
   *        is expected to repaint from scratch. Xt's CoreClassRec
   *        leaves bit_gravity at this default, so Xaw widgets
   *        (Label, Command in xcalc) live here.
   *    1 = NorthWestGravity -- backing's top-left preserved, only
   *        grown strips get Expose. Tk's wrapper widgets explicitly
   *        request this.
   *  Other gravities (NEGravity, CenterGravity, StaticGravity, ...)
   *  would translate the preserved pixels by a gravity offset; we
   *  don't have callers for those yet, so they fall through to the
   *  ForgetGravity (discard) branch -- safe but not pixel-optimal. */
  bitGravity: number;
}

/** Callback the Host supplies so the renderer can reach into the
 *  pixmap table without importing Host (which would be circular). */
export type PixmapLookup = (id: number) => OffscreenCanvas | null;

/**
 * The slice of Renderer state that helper modules under host/render/
 * need to read or mutate. The Renderer class implements this interface;
 * helpers accept any RendererState rather than a concrete Renderer so
 * we avoid an import cycle.
 */
export interface RendererState {
  readonly canvas: RootCanvas;
  readonly windows: Map<number, ManagedWindow>;
  readonly pixmapLookup: PixmapLookup;
  stackCounter: number;
  /** Schedule a compose at next rAF. Called by every helper that
   *  mutates a backing surface or window-tree state that affects
   *  visible pixels. Coalesced internally; safe to call repeatedly. */
  markDirty(): void;
}
