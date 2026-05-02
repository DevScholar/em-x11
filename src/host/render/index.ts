/**
 * Renderer: software-rendered window compositor for em-x11.
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
 *
 * Code organisation (mirrors xserver/mi/ split):
 *   types.ts         -- ManagedWindow, RendererState
 *   window-tree.ts   -- structural state (xserver/dix/window.c)
 *   paint.ts         -- subtree/border/clip/repaintRect (mi/mipaintwin.c)
 *   draw.ts          -- GC primitives + arcPath + blits (mi/mi*.c)
 *   hit-test.ts      -- findWindowAt (mi/mipointrloc.c)
 * The Renderer class below is a thin facade: state + 1-line method bindings
 * to the helper modules, so consumers see a single coherent API.
 */

import type { RootCanvas } from '../../runtime/canvas.js';
import type { Point, ShapeRect } from '../../types/emscripten.js';
import type { ManagedWindow, PixmapLookup, RendererState } from './types.js';
import type { Region } from './region.js';
import * as tree from './window-tree.js';
import * as draw from './draw.js';
import { findWindowAt } from './hit-test.js';

export type { ManagedWindow, PixmapLookup, RendererState } from './types.js';
export type { Region } from './region.js';
export { arcPath } from './draw.js';

export class Renderer implements RendererState {
  readonly canvas: RootCanvas;
  readonly pixmapLookup: PixmapLookup;
  readonly windows = new Map<number, ManagedWindow>();
  stackCounter = 0;

  constructor(canvas: RootCanvas, pixmapLookup: PixmapLookup = () => null) {
    this.canvas = canvas;
    this.pixmapLookup = pixmapLookup;
  }

  /* -- structural / state ----------------------------------------------- */

  addWindow(
    id: number, parent: number, x: number, y: number,
    width: number, height: number,
    borderWidth: number, borderPixel: number,
    bgType: 'none' | 'pixel' | 'pixmap', background: number,
  ): void {
    tree.addWindow(this, id, parent, x, y, width, height, borderWidth, borderPixel, bgType, background);
  }
  setWindowBorder(id: number, borderWidth: number, borderPixel: number): Map<number, Region> {
    return tree.setWindowBorder(this, id, borderWidth, borderPixel);
  }
  setWindowBackground(
    id: number, bgType: 'none' | 'pixel' | 'pixmap', background: number,
  ): void {
    tree.setWindowBackground(this, id, bgType, background);
  }
  setWindowBackgroundPixmap(id: number, pmId: number): void {
    tree.setWindowBackgroundPixmap(this, id, pmId);
  }
  configureWindow(id: number, x: number, y: number, w: number, h: number): Map<number, Region> {
    return tree.configureWindow(this, id, x, y, w, h);
  }
  reparentWindow(id: number, parent: number, x: number, y: number): Map<number, Region> {
    return tree.reparentWindow(this, id, parent, x, y);
  }
  mapWindow(id: number): Map<number, Region> { return tree.mapWindow(this, id); }
  unmapWindow(id: number): Map<number, Region> { return tree.unmapWindow(this, id); }
  destroyWindow(id: number): Map<number, Region> { return tree.destroyWindow(this, id); }
  setWindowShape(id: number, rects: ShapeRect[]): Map<number, Region> { return tree.setWindowShape(this, id, rects); }
  raiseWindow(id: number): Map<number, Region> { return tree.raiseWindow(this, id); }

  parentOf(id: number): number { return tree.parentOf(this, id); }
  mappedDescendants(id: number): number[] { return tree.mappedDescendants(this, id); }
  geometryOf(id: number): { width: number; height: number } | null {
    return tree.geometryOf(this, id);
  }
  absBoundingRect(id: number): { ax: number; ay: number; w: number; h: number } | null {
    return tree.absBoundingRect(this, id);
  }
  mappedWindowsIntersecting(
    rax: number, ray: number, rw: number, rh: number, excludeId: number,
  ): number[] {
    return tree.mappedWindowsIntersecting(this, rax, ray, rw, rh, excludeId);
  }
  attrsOf(id: number): {
    x: number; y: number; width: number; height: number;
    mapped: boolean; parent: number; borderWidth: number;
  } | null {
    return tree.attrsOf(this, id);
  }
  isViewable(id: number): boolean { return tree.isViewable(this, id); }

  /* -- GC drawing ------------------------------------------------------- */

  clearArea(id: number, x: number, y: number, w: number, h: number): void {
    draw.clearArea(this, id, x, y, w, h);
  }
  fillRect(id: number, x: number, y: number, w: number, h: number, color: number): void {
    draw.fillRect(this, id, x, y, w, h, color);
  }
  drawLine(
    id: number, x1: number, y1: number, x2: number, y2: number,
    color: number, lineWidth: number,
  ): void {
    draw.drawLine(this, id, x1, y1, x2, y2, color, lineWidth);
  }
  drawArc(
    id: number, x: number, y: number, w: number, h: number,
    angle1: number, angle2: number, color: number, lineWidth: number,
  ): void {
    draw.drawArc(this, id, x, y, w, h, angle1, angle2, color, lineWidth);
  }
  fillArc(
    id: number, x: number, y: number, w: number, h: number,
    angle1: number, angle2: number, color: number,
  ): void {
    draw.fillArc(this, id, x, y, w, h, angle1, angle2, color);
  }
  fillPolygon(id: number, points: Point[], shape: number, mode: number, color: number): void {
    draw.fillPolygon(this, id, points, shape, mode, color);
  }
  drawPoints(id: number, points: Point[], mode: number, color: number): void {
    draw.drawPoints(this, id, points, mode, color);
  }
  drawString(
    id: number, x: number, y: number, font: string, text: string,
    fgColor: number, bgColor: number, imageMode: boolean,
  ): void {
    draw.drawString(this, id, x, y, font, text, fgColor, bgColor, imageMode);
  }
  blitWindowTo(
    srcId: number, srcX: number, srcY: number, w: number, h: number,
    dstCtx: OffscreenCanvasRenderingContext2D, dstX: number, dstY: number,
  ): void {
    draw.blitWindowTo(this, srcId, srcX, srcY, w, h, dstCtx, dstX, dstY);
  }
  blitImageToWindow(
    dstId: number, dstX: number, dstY: number, src: CanvasImageSource,
    srcX: number, srcY: number, w: number, h: number,
  ): void {
    draw.blitImageToWindow(this, dstId, dstX, dstY, src, srcX, srcY, w, h);
  }

  /* -- hit test --------------------------------------------------------- */

  findWindowAt(cssX: number, cssY: number): number | null {
    return findWindowAt(this, cssX, cssY);
  }
}
