/**
 * Global host singleton.
 *
 * Installed on `globalThis.__EMX11__` before the wasm module starts so the
 * C-side JS library functions (src/bindings/emx11.library.js) can reach
 * into the TS runtime without going through Emscripten's Module object.
 *
 * Separating host state from Module lets a single wasm binary share one
 * RootCanvas/Compositor across multiple Emscripten module instances
 * (relevant later for Pyodide + Tcl-Tk, which run in the same page).
 */

import { RootCanvas } from './canvas.js';
import { pixelToCssColor } from './canvas.js';
import type { RootCanvasOptions } from './canvas.js';
import { Compositor, arcPath } from './compositor.js';
import type { EmX11Host, Point, ShapeRect } from '../types/emscripten.js';

export type HostOptions = RootCanvasOptions;

interface Pixmap {
  canvas: OffscreenCanvas;
  ctx: OffscreenCanvasRenderingContext2D;
  width: number;
  height: number;
  depth: number;
}

/* Per-connection bookkeeping. Each wasm Module that calls XOpenDisplay
 * gets one of these. Right now we only track the connection's XID
 * range; Step 2 will hang owned windows / atoms / property subscribers
 * / the ccall-back Module reference off this record. */
interface Connection {
  connId: number;
  xidBase: number;
  xidMask: number;
}

const MAX_PIXMAP_EDGE = 4096;

/* XID layout: top 3 bits always 0 (x11protocol.txt §935). We dedicate
 * the low 0x00200000 slot (conn_id=0, 2 M IDs) to Host-owned resources
 * -- root window, default cursor, etc. -- so no client can ever forge
 * one. Real connections start at conn_id=1. */
const XID_RANGE_BITS = 21;
const XID_PER_CONN = 1 << XID_RANGE_BITS;         // 0x00200000
const XID_MASK = XID_PER_CONN - 1;                // 0x001FFFFF

export class Host implements EmX11Host {
  readonly canvas: RootCanvas;
  readonly compositor: Compositor;
  private pointerX: number;
  private pointerY: number;
  private readonly pixmaps = new Map<number, Pixmap>();
  private readonly connections = new Map<number, Connection>();
  private nextConnId = 0;

  constructor(options: HostOptions = {}) {
    this.canvas = new RootCanvas(options);
    this.compositor = new Compositor(this.canvas, (id) => {
      const pm = this.pixmaps.get(id);
      return pm ? pm.canvas : null;
    });

    /* Default the pointer to the canvas centre so the first XQueryPointer
     * after XtRealizeWidget -- which fires before the user has had a
     * chance to move the mouse -- returns something sensible instead of
     * the top-left corner. xeyes in particular snaps its pupils here
     * immediately. */
    this.pointerX = (this.canvas.element.clientWidth / 2) | 0;
    this.pointerY = (this.canvas.element.clientHeight / 2) | 0;

    /* Track the last-seen pointer position at the host level so polling
     * callers (XQueryPointer; xeyes uses this every 50ms via an Xt timer)
     * can read it without going through the event bridge's hit test.
     * We listen on `window` rather than the canvas so mouse motion
     * outside the canvas (over browser chrome, over another page region)
     * still updates the cached position -- pupils that track the mouse
     * off-canvas look better than pupils that freeze on exit. */
    window.addEventListener('mousemove', (e) => {
      const rect = this.canvas.element.getBoundingClientRect();
      this.pointerX = (e.clientX - rect.left) | 0;
      this.pointerY = (e.clientY - rect.top) | 0;
    });
  }

  install(): void {
    globalThis.__EMX11__ = this;
  }

  getPointerXY(): Point {
    return { x: this.pointerX, y: this.pointerY };
  }

  onInit(_screenWidth: number, _screenHeight: number): void {
    /* C expects us to adopt its idea of screen size, but the browser is the
     * authority. We ignore the hint and the C side will be corrected the
     * next time it queries through XDisplayWidth/Height once we wire that
     * back through. TODO: plumb the correction call. */
  }

  openDisplay(): { connId: number; xidBase: number; xidMask: number } {
    const connId = ++this.nextConnId;
    const xidBase = connId * XID_PER_CONN;
    const xidMask = XID_MASK;
    this.connections.set(connId, { connId, xidBase, xidMask });
    return { connId, xidBase, xidMask };
  }

  closeDisplay(connId: number): void {
    /* Step 1: just drop the record. Step 2 will sweep owned windows,
     * pixmaps, property subscribers, and cached Module reference. */
    this.connections.delete(connId);
  }

  onWindowCreate(
    id: number,
    parent: number,
    x: number,
    y: number,
    width: number,
    height: number,
    background: number,
  ): void {
    this.compositor.addWindow(id, parent, x, y, width, height, background);
  }

  onWindowMap(id: number): void {
    this.compositor.mapWindow(id);
  }

  onWindowUnmap(id: number): void {
    this.compositor.unmapWindow(id);
  }

  onWindowDestroy(id: number): void {
    this.compositor.destroyWindow(id);
  }

  onWindowSetBgPixmap(id: number, pmId: number): void {
    this.compositor.setWindowBackgroundPixmap(id, pmId);
  }

  onClearArea(id: number, x: number, y: number, w: number, h: number): void {
    this.compositor.clearArea(id, x, y, w, h);
  }

  onFillRect(id: number, x: number, y: number, w: number, h: number, color: number): void {
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapFillRect(pm, x, y, w, h, color);
      return;
    }
    this.compositor.fillRect(id, x, y, w, h, color);
  }

  onDrawLine(
    id: number,
    x1: number,
    y1: number,
    x2: number,
    y2: number,
    color: number,
    lineWidth: number,
  ): void {
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapLine(pm, x1, y1, x2, y2, color, lineWidth);
      return;
    }
    this.compositor.drawLine(id, x1, y1, x2, y2, color, lineWidth);
  }

  onDrawArc(
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
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapArc(pm, x, y, w, h, angle1, angle2, color, lineWidth);
      return;
    }
    this.compositor.drawArc(id, x, y, w, h, angle1, angle2, color, lineWidth);
  }

  onFillArc(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
    color: number,
  ): void {
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapArc(pm, x, y, w, h, angle1, angle2, color, null);
      return;
    }
    this.compositor.fillArc(id, x, y, w, h, angle1, angle2, color);
  }

  onFillPolygon(id: number, points: Point[], shape: number, mode: number, color: number): void {
    this.compositor.fillPolygon(id, points, shape, mode, color);
  }

  onDrawPoints(id: number, points: Point[], mode: number, color: number): void {
    this.compositor.drawPoints(id, points, mode, color);
  }

  onDrawString(
    id: number,
    x: number,
    y: number,
    font: string,
    text: string,
    fgColor: number,
    bgColor: number,
    imageMode: number,
  ): void {
    this.compositor.drawString(id, x, y, font, text, fgColor, bgColor, imageMode !== 0);
  }

  onFlush(): void {
    /* No-op: the compositor already presents through requestAnimationFrame.
     * Kept as a hook for future synchronous-flush scenarios. */
  }

  onWindowShape(id: number, rects: ShapeRect[]): void {
    this.compositor.setWindowShape(id, rects);
  }

  /* -- Pixmap lifecycle --------------------------------------------------- */

  onPixmapCreate(id: number, width: number, height: number, depth: number): void {
    if (width <= 0 || height <= 0) return;
    if (width > MAX_PIXMAP_EDGE || height > MAX_PIXMAP_EDGE) {
      console.warn(
        `em-x11: refusing pixmap ${id} at ${width}x${height} (cap ${MAX_PIXMAP_EDGE})`,
      );
      return;
    }
    const oc = new OffscreenCanvas(width, height);
    const ctx = oc.getContext('2d');
    if (!ctx) return;
    this.pixmaps.set(id, { canvas: oc, ctx, width, height, depth });
  }

  onPixmapDestroy(id: number): void {
    this.pixmaps.delete(id);
  }

  /* -- SHAPE: decode pixmap bits into a bounding region ------------------ */

  onShapeCombineMask(
    destId: number,
    srcId: number,
    xOff: number,
    yOff: number,
    _op: number,
  ): void {
    const pm = this.pixmaps.get(srcId);
    if (!pm) return;

    const image = pm.ctx.getImageData(0, 0, pm.width, pm.height);
    const data = image.data;
    const rects: ShapeRect[] = [];

    /* Row-wise run-length encoding: each horizontal run of "set" pixels
     * becomes one 1-pixel-tall rectangle. Two eyes @ 200x200 generate
     * roughly 2 * 2 * radius rects; fine for the compositor's clip path.
     * We could do better with a proper region coalescing pass, but every
     * frame re-applies the clip, so O(h) rects per shape change is OK. */
    for (let y = 0; y < pm.height; y++) {
      let runStart = -1;
      for (let x = 0; x < pm.width; x++) {
        const alpha = data[(y * pm.width + x) * 4 + 3];
        if (alpha !== undefined && alpha > 0) {
          if (runStart < 0) runStart = x;
        } else if (runStart >= 0) {
          rects.push({ x: runStart + xOff, y: y + yOff, w: x - runStart, h: 1 });
          runStart = -1;
        }
      }
      if (runStart >= 0) {
        rects.push({
          x: runStart + xOff,
          y: y + yOff,
          w: pm.width - runStart,
          h: 1,
        });
      }
    }

    this.compositor.setWindowShape(destId, rects);
  }

  /* -- Pixmap drawing helpers -------------------------------------------- */

  /** Depth-1 pixmaps treat foreground==0 as "bit off" (transparent) and
   *  anything else as "bit on" (opaque). Color pixmaps paint the value
   *  directly as RGB. */
  private paintPixmapFillRect(
    pm: Pixmap,
    x: number,
    y: number,
    w: number,
    h: number,
    color: number,
  ): void {
    if (pm.depth === 1 && color === 0) {
      pm.ctx.clearRect(x, y, w, h);
      return;
    }
    pm.ctx.fillStyle = pixelToCssColor(color);
    pm.ctx.fillRect(x, y, w, h);
  }

  private paintPixmapArc(
    pm: Pixmap,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
    color: number,
    lineWidth: number | null,
  ): void {
    pm.ctx.save();
    if (pm.depth === 1 && color === 0) {
      /* "Erase" -- paint the arc with destination-out so any previously
       * set bits inside the ellipse become unset. */
      pm.ctx.globalCompositeOperation = 'destination-out';
      pm.ctx.fillStyle = '#000';
    } else if (lineWidth === null) {
      pm.ctx.fillStyle = pixelToCssColor(color);
    } else {
      pm.ctx.strokeStyle = pixelToCssColor(color);
      pm.ctx.lineWidth = lineWidth || 1;
    }
    arcPath(pm.ctx, x, y, w, h, angle1, angle2);
    if (lineWidth === null) pm.ctx.fill();
    else pm.ctx.stroke();
    pm.ctx.restore();
  }

  private paintPixmapLine(
    pm: Pixmap,
    x1: number,
    y1: number,
    x2: number,
    y2: number,
    color: number,
    lineWidth: number,
  ): void {
    pm.ctx.save();
    if (pm.depth === 1 && color === 0) {
      pm.ctx.globalCompositeOperation = 'destination-out';
      pm.ctx.strokeStyle = '#000';
    } else {
      pm.ctx.strokeStyle = pixelToCssColor(color);
    }
    pm.ctx.lineWidth = lineWidth || 1;
    pm.ctx.beginPath();
    pm.ctx.moveTo(x1 + 0.5, y1 + 0.5);
    pm.ctx.lineTo(x2 + 0.5, y2 + 0.5);
    pm.ctx.stroke();
    pm.ctx.restore();
  }
}
