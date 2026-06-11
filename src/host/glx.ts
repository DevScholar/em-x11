/**
 * GlxManager: GLX -> WebGL1 (LEGACY_GL_EMULATION) bridge.
 *
 * Each GLXContext owns a dedicated OffscreenCanvas. emscripten's libGL
 * (linked into the demo wasm with -lGL -sLEGACY_GL_EMULATION=1) creates
 * its WebGL1 context against that canvas via emscripten_webgl_create_context,
 * resolved through Module.specialHTMLTargets[targetId]. The targetId
 * registration is done in the EM_JS body that wraps createContext, since
 * `Module` there refers to the calling wasm.
 *
 * On glXSwapBuffers we drawImage(offscreen, 0, 0) into the X window's
 * 2D backing surface, mark it dirty, and request a recompose. The
 * compositor then stitches the WebGL frame in alongside everything
 * else.
 *
 * This file does NOT touch WebGL itself -- emscripten's libGL owns the
 * GL context lifecycle. We just allocate the canvas + target string and
 * blit on swap.
 */

import type { Renderer } from './render/index.js';
import type { ManagedWindow } from './render/types.js';

interface GlxContextEntry {
  id: number;
  /** Target string handed to emscripten_webgl_create_context. The EM_JS
   *  bridge stores this OffscreenCanvas under Module.specialHTMLTargets[targetId]
   *  on the calling wasm's Module so emscripten's findEventTarget resolves it. */
  targetId: string;
  canvas: OffscreenCanvas;
  width: number;
  height: number;
}

export class GlxManager {
  private readonly renderer: Renderer;
  private readonly contexts = new Map<number, GlxContextEntry>();
  private nextId = 1;

  constructor(renderer: Renderer) {
    this.renderer = renderer;
  }

  /** glXCreateContext -> allocate the OffscreenCanvas + target string.
   *  The EM_JS bridge that calls this is responsible for
   *  Module.specialHTMLTargets[targetId] = canvas (so emscripten's
   *  WebGL setup, running in the demo wasm, can find it). */
  createContext(width: number, height: number): {
    id: number; targetId: string; canvas: OffscreenCanvas;
  } {
    const id = this.nextId++;
    const targetId = `!em-x11-glx-${id}`;
    const w = Math.max(1, width | 0);
    const h = Math.max(1, height | 0);
    const canvas = new OffscreenCanvas(w, h);
    this.contexts.set(id, { id, targetId, canvas, width: w, height: h });
    return { id, targetId, canvas };
  }

  /** Lookup used by the destroy bridge to clean up Module.specialHTMLTargets. */
  targetIdOf(id: number): string | null {
    const entry = this.contexts.get(id);
    return entry ? entry.targetId : null;
  }

  destroyContext(id: number): void {
    this.contexts.delete(id);
  }

  /** glXSwapBuffers -> blit the GL OffscreenCanvas into the X window's
   *  2D backing surface. No-op when the drawable isn't a known mapped
   *  window (Pixmap drawables not yet supported). */
  swapBuffers(ctxId: number, drawable: number): void {
    const entry = this.contexts.get(ctxId);
    if (!entry) return;
    const win = this.renderer.windows.get(drawable >>> 0) as ManagedWindow | undefined;
    if (!win) return;
    const ctx = win.backingCtx;
    ctx.save();
    ctx.drawImage(entry.canvas as unknown as CanvasImageSource, 0, 0);
    ctx.restore();
    win.backingDirty = true;
    this.renderer.markDirty();
  }

  /** glXgears reconfigures viewport on ConfigureNotify; we resize the
   *  destination canvas so emscripten's gl draws at the new size. */
  resize(ctxId: number, width: number, height: number): void {
    const entry = this.contexts.get(ctxId);
    if (!entry) return;
    const w = Math.max(1, width | 0);
    const h = Math.max(1, height | 0);
    if (w === entry.width && h === entry.height) return;
    entry.canvas.width = w;
    entry.canvas.height = h;
    entry.width = w;
    entry.height = h;
  }
}
