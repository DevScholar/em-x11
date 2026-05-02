/**
 * Internal types shared between Renderer and its helper modules.
 *
 * Putting these here (rather than in index.ts) lets helper modules under
 * host/render/ import them without pulling in the Renderer class itself,
 * which would create a circular import.
 */

import type { RootCanvas } from '../../runtime/canvas.js';
import type { ShapeRect } from '../../types/emscripten.js';

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
   *    'none'   -- server does not auto-paint the bg. miPaintWindow
   *                gates on `state != None` (xserver/mi/miwindow.c:115);
   *                we mirror that in paintBackgroundRect. xeyes' shell
   *                lives in this state -- the application is the only
   *                thing that paints inside.
   *    'pixel'  -- solid fill from `background`.
   *    'pixmap' -- tile fill from `backgroundPixmap`.
   *  ParentRelative is currently collapsed to 'none' at the C bridge. */
  bgType: 'none' | 'pixel' | 'pixmap';
  background: number;
  /** When set (and bgType==='pixmap'), the window background is tiled
   *  with this pixmap's OffscreenCanvas. Tile origin is the window's
   *  top-left (applied via ctx.translate at paint time). */
  backgroundPixmap: number | null;
  mapped: boolean;
  /** SHAPE bounding rectangles (window-local coords). `null` means
   *  unshaped -- the window is a plain rectangle of (width, height). */
  shape: ShapeRect[] | null;
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
}
