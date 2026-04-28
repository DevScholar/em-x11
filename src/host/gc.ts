/**
 * GC drawing dispatch + pixmap lifecycle. Each X drawing call from the
 * C side lands here; we route to either the renderer (when the target
 * id is a window) or to a pixmap's OffscreenCanvas ctx (when the id
 * is a Pixmap). Mirrors xserver/dix/gc.c + dix/dispatch.c request
 * handlers (PolyFillRectangle, PolyLine, PolyArc, PolyFillArc, FillPoly,
 * PolyPoint, PolyText / ImageText, ClearArea, CopyArea, CopyPlane,
 * PutImage) plus xserver/dix/pixmap.c (CreatePixmap, FreePixmap) and
 * the Shape extension's ShapeMask combine.
 *
 * Pixmap drawing duplicates the renderer's logic because pixmaps don't
 * live in the renderer (they're not in the window tree, they have no
 * clip stack, no tile background). The duplication is deliberate: a
 * pixmap is a flat OffscreenCanvas, and the helpers below paint
 * directly into its ctx without absOrigin / applyWindowClip.
 */

import type { Host } from './index.js';
import type { Point, ShapeRect } from '../types/emscripten.js';
import { pixelToCssColor } from '../runtime/canvas.js';
import { arcPath } from './render/index.js';

const MAX_PIXMAP_EDGE = 4096;

interface Pixmap {
  canvas: OffscreenCanvas;
  ctx: OffscreenCanvasRenderingContext2D;
  width: number;
  height: number;
  depth: number;
}

export class GcManager {
  private readonly pixmaps = new Map<number, Pixmap>();

  constructor(private readonly host: Host) {}

  /** Lookup used by Renderer.pixmapLookup so window backgrounds can
   *  tile a Pixmap. Returns the OffscreenCanvas only, not the ctx. */
  pixmapCanvas(id: number): OffscreenCanvas | null {
    return this.pixmaps.get(id)?.canvas ?? null;
  }

  /** Install a pre-built host-owned pixmap (currently just the weave at
   *  HOST_WEAVE_PIXMAP_ID, set up during installSharedRoot). The
   *  caller has already done the OffscreenCanvas + ctx + initial fill
   *  outside the MAX_PIXMAP_EDGE check (the weave is 2x2). */
  installHostPixmap(
    id: number,
    canvas: OffscreenCanvas,
    ctx: OffscreenCanvasRenderingContext2D,
    width: number,
    height: number,
    depth: number,
  ): void {
    this.pixmaps.set(id, { canvas, ctx, width, height, depth });
  }

  /* -- C-side bridge entry points --------------------------------------- */

  onInit(_screenWidth: number, _screenHeight: number): void {
    /* C expects us to adopt its idea of screen size, but the browser is the
     * authority. We ignore the hint and the C side will be corrected the
     * next time it queries through XDisplayWidth/Height once we wire that
     * back through. TODO: plumb the correction call. */
  }

  onClearArea(id: number, x: number, y: number, w: number, h: number): void {
    this.host.renderer.clearArea(id, x, y, w, h);
  }

  onFillRect(id: number, x: number, y: number, w: number, h: number, color: number): void {
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapFillRect(pm, x, y, w, h, color);
      return;
    }
    this.host.renderer.fillRect(id, x, y, w, h, color);
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
    this.host.renderer.drawLine(id, x1, y1, x2, y2, color, lineWidth);
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
    this.host.renderer.drawArc(id, x, y, w, h, angle1, angle2, color, lineWidth);
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
    this.host.renderer.fillArc(id, x, y, w, h, angle1, angle2, color);
  }

  onFillPolygon(id: number, points: Point[], shape: number, mode: number, color: number): void {
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapPolygon(pm, points, color);
      return;
    }
    this.host.renderer.fillPolygon(id, points, shape, mode, color);
  }

  onDrawPoints(id: number, points: Point[], mode: number, color: number): void {
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapPoints(pm, points, color);
      return;
    }
    this.host.renderer.drawPoints(id, points, mode, color);
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
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapString(pm, x, y, font, text, fgColor, bgColor, imageMode !== 0);
      return;
    }
    this.host.renderer.drawString(id, x, y, font, text, fgColor, bgColor, imageMode !== 0);
  }

  onFlush(): void {
    /* No-op: the renderer paints synchronously at the moment of each
     * C-side request, so there's nothing to flush. Kept as a hook for
     * future scenarios where we batch and need an explicit boundary. */
  }

  onWindowShape(id: number, rects: ShapeRect[]): void {
    this.host.renderer.setWindowShape(id, rects);
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
     * roughly 2 * 2 * radius rects; fine for the renderer's clip path.
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

    this.host.renderer.setWindowShape(destId, rects);
  }

  /* -- Drawable-to-drawable copy ---------------------------------------- */

  /** XCopyArea: blit a rectangle from src drawable to dst drawable. Either
   *  endpoint may be a Window or a Pixmap; any combination is legal per
   *  xorg/xserver's ProcCopyArea. W→W can't use same-canvas drawImage
   *  safely (spec says overlap is impl-defined), so we stage through an
   *  OffscreenCanvas. Also the reason clipped-to-visible-root bytes
   *  satisfy Tk's double-buffering path: the source window's pixels on
   *  the root canvas ARE the current contents. */
  onCopyArea(
    srcId: number,
    dstId: number,
    srcX: number,
    srcY: number,
    w: number,
    h: number,
    dstX: number,
    dstY: number,
  ): void {
    if (w <= 0 || h <= 0) return;
    const srcPm = this.pixmaps.get(srcId);
    const dstPm = this.pixmaps.get(dstId);
    if (dstPm) {
      if (srcPm) {
        dstPm.ctx.drawImage(srcPm.canvas, srcX, srcY, w, h, dstX, dstY, w, h);
      } else {
        this.host.renderer.blitWindowTo(srcId, srcX, srcY, w, h, dstPm.ctx, dstX, dstY);
      }
      return;
    }
    if (srcPm) {
      this.host.renderer.blitImageToWindow(
        dstId,
        dstX,
        dstY,
        srcPm.canvas,
        srcX,
        srcY,
        w,
        h,
      );
      return;
    }
    /* W→W: capture through an intermediate so drawImage doesn't overlap
     * the root canvas with itself. */
    const stage = new OffscreenCanvas(w, h);
    const sctx = stage.getContext('2d');
    if (!sctx) return;
    this.host.renderer.blitWindowTo(srcId, srcX, srcY, w, h, sctx, 0, 0);
    this.host.renderer.blitImageToWindow(dstId, dstX, dstY, stage, 0, 0, w, h);
  }

  /** XCopyPlane: simplified to the case Tk/Xaw actually use -- a depth-1
   *  source pixmap whose set bits paint with `fg` and unset bits (when
   *  `applyBg` is true) paint with `bg`. `plane` is accepted for signature
   *  fidelity but ignored (the source canvas's alpha IS the only plane).
   *  W→anything CopyPlane is not exercised by our callers. */
  onCopyPlane(
    srcId: number,
    dstId: number,
    srcX: number,
    srcY: number,
    w: number,
    h: number,
    dstX: number,
    dstY: number,
    _plane: number,
    fg: number,
    bg: number,
    applyBg: boolean,
  ): void {
    if (w <= 0 || h <= 0) return;
    const srcPm = this.pixmaps.get(srcId);
    if (!srcPm) return;
    /* Build a coloured stencil: fg where src alpha>0, bg where alpha==0
     * (if applyBg), transparent otherwise. Then blit to dst. */
    const stage = new OffscreenCanvas(w, h);
    const sctx = stage.getContext('2d');
    if (!sctx) return;
    if (applyBg) {
      sctx.fillStyle = pixelToCssColor(bg);
      sctx.fillRect(0, 0, w, h);
    }
    /* Paint the src canvas clipped to (srcX, srcY, w, h) into a mask,
     * then use source-in to colour the mask with fg. */
    const maskCtx = new OffscreenCanvas(w, h).getContext('2d');
    if (!maskCtx) return;
    maskCtx.drawImage(srcPm.canvas, srcX, srcY, w, h, 0, 0, w, h);
    maskCtx.globalCompositeOperation = 'source-in';
    maskCtx.fillStyle = pixelToCssColor(fg);
    maskCtx.fillRect(0, 0, w, h);
    sctx.drawImage(maskCtx.canvas, 0, 0);

    const dstPm = this.pixmaps.get(dstId);
    if (dstPm) {
      dstPm.ctx.drawImage(stage, 0, 0, w, h, dstX, dstY, w, h);
      return;
    }
    this.host.renderer.blitImageToWindow(dstId, dstX, dstY, stage, 0, 0, w, h);
  }

  /** XPutImage: blit a raw pixel buffer (ZPixmap, RGBA-ish) or 1-bit
   *  bitmap (XYBitmap) into a Drawable. ZPixmap arrives as 32bpp BGRA in
   *  memory order on little-endian wasm (format0 is LSBFirst, per
   *  display.c). We reorder into RGBA for ImageData. XYBitmap treats each
   *  set bit as gc->foreground, unset as gc->background. */
  onPutImage(
    dstId: number,
    dstX: number,
    dstY: number,
    w: number,
    h: number,
    format: number,
    depth: number,
    bytesPerLine: number,
    data: Uint8Array,
    fg: number,
    bg: number,
  ): void {
    if (w <= 0 || h <= 0) return;
    const stage = new OffscreenCanvas(w, h);
    const sctx = stage.getContext('2d');
    if (!sctx) return;
    const img = sctx.createImageData(w, h);
    const out = img.data;
    /* format: 0=XYBitmap (depth must be 1), 2=ZPixmap. */
    if (format === 0 || depth === 1) {
      const fgR = (fg >> 16) & 0xff;
      const fgG = (fg >> 8) & 0xff;
      const fgB = fg & 0xff;
      const bgR = (bg >> 16) & 0xff;
      const bgG = (bg >> 8) & 0xff;
      const bgB = bg & 0xff;
      for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
          const byte = data[y * bytesPerLine + (x >> 3)] ?? 0;
          /* bitmap_bit_order LSBFirst (display.c). */
          const bit = (byte >> (x & 7)) & 1;
          const o = (y * w + x) * 4;
          if (bit) {
            out[o] = fgR;
            out[o + 1] = fgG;
            out[o + 2] = fgB;
            out[o + 3] = 0xff;
          } else {
            out[o] = bgR;
            out[o + 1] = bgG;
            out[o + 2] = bgB;
            out[o + 3] = 0xff;
          }
        }
      }
    } else {
      /* ZPixmap 32bpp. display.c advertises LSBFirst + 32-bit unit, so
       * pixel layout in bytes is B,G,R,A. Convert to canvas ImageData
       * (R,G,B,A). */
      for (let y = 0; y < h; y++) {
        const src = y * bytesPerLine;
        const dst = y * w * 4;
        for (let x = 0; x < w; x++) {
          const si = src + x * 4;
          const di = dst + x * 4;
          out[di] = data[si + 2] ?? 0;
          out[di + 1] = data[si + 1] ?? 0;
          out[di + 2] = data[si] ?? 0;
          out[di + 3] = data[si + 3] ?? 0xff;
        }
      }
    }
    sctx.putImageData(img, 0, 0);

    const dstPm = this.pixmaps.get(dstId);
    if (dstPm) {
      dstPm.ctx.drawImage(stage, 0, 0, w, h, dstX, dstY, w, h);
      return;
    }
    this.host.renderer.blitImageToWindow(dstId, dstX, dstY, stage, 0, 0, w, h);
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
    const lw = lineWidth || 1;
    /* Axis-aligned via fillRect (sharp, no AA); diagonals fall back to
     * stroke. Mirrors Renderer.drawLine -- see notes there. */
    if (x1 === x2 || y1 === y2) {
      if (pm.depth === 1 && color === 0) {
        pm.ctx.globalCompositeOperation = 'destination-out';
        pm.ctx.fillStyle = '#000';
      } else {
        pm.ctx.fillStyle = pixelToCssColor(color);
      }
      let rx: number, ry: number, rw: number, rh: number;
      if (y1 === y2) {
        rx = Math.min(x1, x2);
        rw = Math.abs(x2 - x1) + 1;
        ry = y1 - ((lw - 1) >> 1);
        rh = lw;
      } else {
        ry = Math.min(y1, y2);
        rh = Math.abs(y2 - y1) + 1;
        rx = x1 - ((lw - 1) >> 1);
        rw = lw;
      }
      pm.ctx.fillRect(rx, ry, rw, rh);
    } else {
      if (pm.depth === 1 && color === 0) {
        pm.ctx.globalCompositeOperation = 'destination-out';
        pm.ctx.strokeStyle = '#000';
      } else {
        pm.ctx.strokeStyle = pixelToCssColor(color);
      }
      pm.ctx.lineWidth = lw;
      pm.ctx.beginPath();
      pm.ctx.moveTo(x1, y1);
      pm.ctx.lineTo(x2, y2);
      pm.ctx.stroke();
    }
    pm.ctx.restore();
  }

  private paintPixmapPolygon(pm: Pixmap, points: Point[], color: number): void {
    if (points.length < 3) return;
    pm.ctx.save();
    if (pm.depth === 1 && color === 0) {
      pm.ctx.globalCompositeOperation = 'destination-out';
      pm.ctx.fillStyle = '#000';
    } else {
      pm.ctx.fillStyle = pixelToCssColor(color);
    }
    pm.ctx.beginPath();
    const first = points[0]!;
    pm.ctx.moveTo(first.x, first.y);
    for (let i = 1; i < points.length; i++) {
      const p = points[i]!;
      pm.ctx.lineTo(p.x, p.y);
    }
    pm.ctx.closePath();
    pm.ctx.fill();
    pm.ctx.restore();
  }

  private paintPixmapPoints(pm: Pixmap, points: Point[], color: number): void {
    if (points.length === 0) return;
    pm.ctx.save();
    if (pm.depth === 1 && color === 0) {
      pm.ctx.globalCompositeOperation = 'destination-out';
      pm.ctx.fillStyle = '#000';
    } else {
      pm.ctx.fillStyle = pixelToCssColor(color);
    }
    for (const p of points) pm.ctx.fillRect(p.x, p.y, 1, 1);
    pm.ctx.restore();
  }

  private paintPixmapString(
    pm: Pixmap,
    x: number,
    y: number,
    font: string,
    text: string,
    fgColor: number,
    bgColor: number,
    imageMode: boolean,
  ): void {
    if (text.length === 0) return;
    pm.ctx.save();
    pm.ctx.font = font;
    pm.ctx.textBaseline = 'alphabetic';
    pm.ctx.textAlign = 'left';
    if (imageMode) {
      const metrics = pm.ctx.measureText(text);
      const ascent =
        metrics.fontBoundingBoxAscent ?? metrics.actualBoundingBoxAscent ?? 10;
      const descent =
        metrics.fontBoundingBoxDescent ?? metrics.actualBoundingBoxDescent ?? 2;
      if (pm.depth === 1 && bgColor === 0) {
        pm.ctx.save();
        pm.ctx.globalCompositeOperation = 'destination-out';
        pm.ctx.fillStyle = '#000';
        pm.ctx.fillRect(x, y - ascent, metrics.width, ascent + descent);
        pm.ctx.restore();
      } else {
        pm.ctx.fillStyle = pixelToCssColor(bgColor);
        pm.ctx.fillRect(x, y - ascent, metrics.width, ascent + descent);
      }
    }
    if (pm.depth === 1 && fgColor === 0) {
      pm.ctx.globalCompositeOperation = 'destination-out';
      pm.ctx.fillStyle = '#000';
    } else {
      pm.ctx.fillStyle = pixelToCssColor(fgColor);
    }
    pm.ctx.fillText(text, x, y);
    pm.ctx.restore();
  }
}
