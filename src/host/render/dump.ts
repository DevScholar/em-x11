/**
 * Debug dump: convert per-window backing surfaces / shape masks / root
 * canvas crops into viewable images. Designed for a frontend canvas
 * developer's mental model: "show me what's on this OffscreenCanvas right
 * now."
 *
 * All output goes to console.log with <img> elements — Chrome DevTools
 * renders them inline and expandable. No new tabs, no popups. Blob URLs
 * are returned so callers can also use them programmatically.
 *
 * Auto-snapshot: a ring buffer of {backing, mask, rootCrop} triggered
 * by window-tree structural events (configure, shape, map, raise).
 * Controlled via Module['emx11Debug'].autoSnapshot.
 */

import type { RendererState, ManagedWindow } from './types.js';
import { absOrigin } from './window-tree.js';

/* ------------------------------------------------------------------ */
/*  Core canvas → viewable helpers                                     */
/* ------------------------------------------------------------------ */

/** OffscreenCanvas → PNG Blob. */
export async function canvasToBlob(c: OffscreenCanvas): Promise<Blob> {
  return c.convertToBlob({ type: 'image/png' });
}

/** Create a blob URL and log it to console as an <img> element.
 *  Returns the URL for programmatic use. */
function logImage(blob: Blob, label: string, borderColor: string): string {
  const url = URL.createObjectURL(blob);
  logImageFromUrl(url, label, borderColor);
  return url;
}

/** Log an existing blob URL as an <img> to console. */
function logImageFromUrl(url: string, label: string, borderColor: string): void {
  if (typeof document !== 'undefined') {
    const img = document.createElement('img');
    img.src = url;
    img.style.maxWidth = '400px';
    img.style.border = `2px solid ${borderColor}`;
    console.log(label, img);
  } else {
    console.log(`[emx11] ${label}: ${url}`);
  }
}

/* ------------------------------------------------------------------ */
/*  Window dump                                                        */
/* ------------------------------------------------------------------ */

export interface WindowDumpMeta {
  id: number;
  parent: number;
  ax: number;
  ay: number;
  width: number;
  height: number;
  borderWidth: number;
  mapped: boolean;
  bgType: string;
  shape: string;
  clipList: string;
}

export interface WindowDump {
  backingUrl: string;
  maskUrl: string | null;
  rootCropUrl: string;
  meta: WindowDumpMeta;
}

/** Dump one window: backing + shapeMask + root canvas crop.
 *  Logs images to console, returns URLs. */
export async function dumpWindow(r: RendererState, id: number): Promise<WindowDump | null> {
  const win = r.windows.get(id);
  if (!win) {
    console.warn(`[emx11] dumpWindow: window #${id} not found`);
    return null;
  }

  const [backingBlob, maskBlob, rootCrop] = await Promise.all([
    canvasToBlob(win.backingSurface),
    win.shapeMask ? canvasToBlob(win.shapeMask) : Promise.resolve(null),
    Promise.resolve(cropRootCanvasToWindow(r, win)),
  ]);

  const rootCropBlob = rootCrop ? await canvasToBlob(rootCrop) : null;

  const meta = buildMeta(r, win);

  console.group(`Window #${id} dump`);
  console.table([meta]);

  const backingUrl = logImage(backingBlob, 'backing', '#666');
  const maskUrl = maskBlob ? logImage(maskBlob, 'shapeMask', '#f0f') : null;
  const rootCropUrl = rootCropBlob ? logImage(rootCropBlob, 'rootCrop', '#0f0') : '';

  console.groupEnd();
  return { backingUrl, maskUrl, rootCropUrl, meta };
}

/* ------------------------------------------------------------------ */
/*  Root canvas dump                                                   */
/* ------------------------------------------------------------------ */

/** Dump the full root canvas as a PNG. Logs to console, returns URL. */
export async function dumpComposite(r: RendererState): Promise<string> {
  const blob = await rootCanvasToBlob(r);
  return logImage(blob, 'root canvas', '#ff0');
}

async function rootCanvasToBlob(r: RendererState): Promise<Blob> {
  const surface = r.canvas.surface;
  if (typeof OffscreenCanvas !== 'undefined' && surface instanceof OffscreenCanvas) {
    return surface.convertToBlob({ type: 'image/png' });
  }
  // HTMLCanvasElement
  return new Promise((resolve, reject) => {
    (surface as HTMLCanvasElement).toBlob((b) => {
      if (b) resolve(b);
      else reject(new Error('canvas.toBlob returned null'));
    }, 'image/png');
  });
}

/* ------------------------------------------------------------------ */
/*  Side-by-side comparison: backing vs root canvas crop               */
/* ------------------------------------------------------------------ */

/** Crop the root canvas to a window's absolute bounding region. */
export function cropRootCanvasToWindow(
  r: RendererState,
  win: ManagedWindow,
): OffscreenCanvas | null {
  const { ax, ay } = absOrigin(r, win);
  const dpr = r.canvas.dpr;
  const crop = new OffscreenCanvas(Math.max(1, win.width), Math.max(1, win.height));
  const cctx = crop.getContext('2d')!;
  cctx.imageSmoothingEnabled = false;
  cctx.drawImage(
    r.canvas.surface as unknown as CanvasImageSource,
    (ax * dpr) | 0, (ay * dpr) | 0,
    (win.width * dpr) | 0, (win.height * dpr) | 0,
    0, 0, win.width, win.height,
  );
  return crop;
}

/** Side-by-side comparison: backing (left) | root canvas crop (right).
 *  Logs the combined image to console, returns URL. */
export async function dumpWindowCompare(r: RendererState, id: number): Promise<string | null> {
  const win = r.windows.get(id);
  if (!win) {
    console.warn(`[emx11] dumpWindowCompare: window #${id} not found`);
    return null;
  }

  const rootCrop = cropRootCanvasToWindow(r, win);
  if (!rootCrop) return null;

  const gap = 4;
  const labelH = 16;
  const w = win.width, h = win.height;
  const combined = new OffscreenCanvas(w * 2 + gap, h + labelH);
  const cctx = combined.getContext('2d')!;
  cctx.imageSmoothingEnabled = false;

  cctx.font = '12px monospace';
  cctx.fillStyle = '#ccc';
  cctx.fillText('Backing', 2, 12);
  cctx.fillText('Root Canvas', w + gap + 2, 12);

  cctx.drawImage(win.backingSurface as unknown as CanvasImageSource, 0, labelH);
  cctx.fillStyle = '#f00';
  cctx.fillRect(w, 0, gap, combined.height);
  cctx.drawImage(rootCrop as unknown as CanvasImageSource, w + gap, labelH);

  const blob = await canvasToBlob(combined);
  const meta = buildMeta(r, win);
  console.group(`Window #${id} compare (backing | root)`);
  console.table([meta]);
  console.groupEnd();
  return logImage(blob, `compare #${id}`, '#f00');
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

function buildMeta(r: RendererState, win: ManagedWindow): WindowDumpMeta {
  const { ax, ay } = absOrigin(r, win);
  return {
    id: win.id,
    parent: win.parent,
    ax,
    ay,
    width: win.width,
    height: win.height,
    borderWidth: win.borderWidth,
    mapped: win.mapped,
    bgType: win.bgType,
    shape: win.shape ? `${win.shape.length} rects` : 'none',
    clipList: `${win.clipList.length} rects`,
  };
}

/* ------------------------------------------------------------------ */
/*  Auto-snapshot ring buffer                                          */
/* ------------------------------------------------------------------ */

export type SnapshotEvent = 'configure' | 'shape' | 'map' | 'raise' | 'compose' | 'manual';

export interface Snapshot {
  time: number;
  event: SnapshotEvent;
  windowId: number;
  backingUrl: string;
  maskUrl: string | null;
  rootCropUrl: string;
}

export interface AutoSnapshotConfig {
  enabled: boolean;
  events: SnapshotEvent[];
  maxSnapshots: number;
}

const DEFAULT_AUTO_CONFIG: AutoSnapshotConfig = {
  enabled: false,
  events: ['configure', 'shape'],
  maxSnapshots: 20,
};

export class AutoSnapshotManager {
  config: AutoSnapshotConfig = { ...DEFAULT_AUTO_CONFIG };
  private ring: Snapshot[] = [];

  /** Synchronously clone canvases and schedule async blob conversion.
   *  Safe to fire-and-forget from sync hot paths. */
  scheduleCapture(
    r: RendererState,
    event: SnapshotEvent,
    windowId: number,
  ): void {
    if (!this.config.enabled) return;
    if (!this.config.events.includes(event)) return;

    const win = r.windows.get(windowId);
    if (!win) return;

    const backingClone = cloneCanvas(win.backingSurface, win.width, win.height);
    const maskClone = win.shapeMask
      ? cloneCanvas(win.shapeMask, win.width, win.height)
      : null;
    const rootCrop = cropRootCanvasToWindow(r, win);

    this._convertAndStore(event, windowId, backingClone, maskClone, rootCrop);
  }

  private async _convertAndStore(
    event: SnapshotEvent,
    windowId: number,
    backing: OffscreenCanvas,
    mask: OffscreenCanvas | null,
    rootCrop: OffscreenCanvas | null,
  ): Promise<void> {
    const [backingBlob, maskBlob, rootCropBlob] = await Promise.all([
      canvasToBlob(backing),
      mask ? canvasToBlob(mask) : Promise.resolve(null),
      rootCrop ? canvasToBlob(rootCrop) : Promise.resolve(null),
    ]);

    const backingUrl = URL.createObjectURL(backingBlob);
    const maskUrl = maskBlob ? URL.createObjectURL(maskBlob) : null;
    const rootCropUrl = rootCropBlob ? URL.createObjectURL(rootCropBlob) : '';

    this.ring.push({
      time: Date.now(),
      event,
      windowId,
      backingUrl,
      maskUrl,
      rootCropUrl,
    });

    // Log images directly to console so they appear inline
    console.group(`[auto] ${event} #${windowId} @${this.ring.length}`);
    logImageFromUrl(backingUrl, 'backing', '#666');
    if (maskUrl) logImageFromUrl(maskUrl, 'shapeMask', '#f0f');
    logImageFromUrl(rootCropUrl, 'rootCrop', '#0f0');
    console.groupEnd();

    while (this.ring.length > this.config.maxSnapshots) {
      const old = this.ring.shift()!;
      URL.revokeObjectURL(old.backingUrl);
      if (old.maskUrl) URL.revokeObjectURL(old.maskUrl);
      URL.revokeObjectURL(old.rootCropUrl);
    }
  }

  get snapshots(): readonly Snapshot[] { return this.ring; }

  /** Manual one-shot snapshot. */
  async snapshotNow(r: RendererState, windowId: number): Promise<Snapshot> {
    const win = r.windows.get(windowId);
    if (!win) throw new Error(`window #${windowId} not found`);

    const [backingBlob, maskBlob, rootCrop] = await Promise.all([
      canvasToBlob(win.backingSurface),
      win.shapeMask ? canvasToBlob(win.shapeMask) : Promise.resolve(null),
      Promise.resolve(cropRootCanvasToWindow(r, win)),
    ]);
    const rootCropBlob = rootCrop ? await canvasToBlob(rootCrop) : null;

    const snap: Snapshot = {
      time: Date.now(),
      event: 'manual',
      windowId,
      backingUrl: URL.createObjectURL(backingBlob),
      maskUrl: maskBlob ? URL.createObjectURL(maskBlob) : null,
      rootCropUrl: rootCropBlob ? URL.createObjectURL(rootCropBlob) : '',
    };

    this.ring.push(snap);
    return snap;
  }

  /** Re-log all stored snapshots as images to the console. */
  show(): void {
    const count = this.ring.length;
    if (count === 0) {
      console.log('[auto-snapshot] no snapshots yet');
      return;
    }
    console.group(`[auto-snapshot] ${count} snapshots`);
    for (let i = 0; i < count; i++) {
      const s = this.ring[i]!;
      console.group(`#${i + 1} ${s.event} window=${s.windowId} @${s.time}`);
      logImageFromUrl(s.backingUrl, 'backing', '#666');
      if (s.maskUrl) logImageFromUrl(s.maskUrl, 'shapeMask', '#f0f');
      logImageFromUrl(s.rootCropUrl, 'rootCrop', '#0f0');
      console.groupEnd();
    }
    console.groupEnd();
  }

  clear(): void {
    for (const s of this.ring) {
      URL.revokeObjectURL(s.backingUrl);
      if (s.maskUrl) URL.revokeObjectURL(s.maskUrl);
      URL.revokeObjectURL(s.rootCropUrl);
    }
    this.ring.length = 0;
  }
}

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

function cloneCanvas(src: OffscreenCanvas, w: number, h: number): OffscreenCanvas {
  const dst = new OffscreenCanvas(Math.max(1, w), Math.max(1, h));
  const ctx = dst.getContext('2d')!;
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(src as unknown as CanvasImageSource, 0, 0);
  return dst;
}
