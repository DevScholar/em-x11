/**
 * initEmX11 — Layer 2 API: single-program convenience wrapper.
 *
 * Use this when you want JS-side control over the canvas, dimensions,
 * or input without the multi-process mental model of createEmX11().
 * initEmX11 creates a Host, attaches it to Module['emx11Host'] so the
 * C-side bridges can reach it, and returns the display/keyboard/debug
 * surfaces directly.
 *
 * For zero-JS usage (Layer 1), compile with -sUSE_EMX11 and omit this
 * module entirely — the port's JS library auto-creates a default Host.
 *
 * For multi-program sessions (Layer 3), use createEmX11() instead.
 *
 * Example:
 *
 *   import { initEmX11 } from '@devscholar/em-x11';
 *
 *   const x11 = await initEmX11({ canvas: myCanvas, width: 1024, height: 768 });
 *   console.log('Root window:', x11.display.rootWindowId);
 *
 *   // Load the wasm module — spread moduleOverrides so the C-side
 *   // bridges find the Host on Module['emx11Host'].
 *   const factory = (await import('./myapp.js')).default;
 *   await factory({ ...x11.moduleOverrides });
 */

import { Host } from '../host/index.js';
import type { HostOptions } from '../host/index.js';
import { DisplayNamespace } from './display.js';
import { DebugNamespace } from './debug.js';
import type { EmX11Display, EmX11Debug } from './types.js';

export interface InitEmX11Options {
  /** Canvas to paint into.  Falls back to Module['canvas'], then
   *  document.querySelector('#canvas'), then auto-creates one. */
  canvas?: HTMLCanvasElement | OffscreenCanvas;
  /** DOM parent for an auto-created canvas. */
  parent?: HTMLElement;
  /** Logical (CSS) pixel width of the X screen. Default 1024. */
  width?: number;
  /** Logical (CSS) pixel height of the X screen. Default 768. */
  height?: number;
}

export interface EmX11Session {
  readonly display: EmX11Display;
  readonly debug: EmX11Debug;
  /** Module overrides to spread into the Emscripten factory call.
   *  Passes the Host to Module['emx11Host'] and suppresses the
   *  default Host auto-start.  Usage:
   *
   *    const factory = (await import('./myapp.js')).default;
   *    await factory({ ...x11.moduleOverrides, ...otherOverrides });
   */
  readonly moduleOverrides: { emx11Host: Host; emx11NoAutoStart: true };
  /** @internal Escape hatch onto the internal Host. */
  readonly _host: Host;
  /** Tear down DOM listeners and IME overlay.  Idempotent. */
  dispose(): void;
}

export function initEmX11(options: InitEmX11Options = {}): Promise<EmX11Session> {
  const hostOptions: HostOptions = {};
  if (options.parent !== undefined) hostOptions.parent = options.parent;
  if (options.width !== undefined) hostOptions.width = options.width;
  if (options.height !== undefined) hostOptions.height = options.height;
  if (options.canvas !== undefined) {
    if (options.canvas instanceof OffscreenCanvas) {
      hostOptions.surface = options.canvas;
    } else {
      hostOptions.element = options.canvas;
    }
  }

  const host = new Host(hostOptions);
  host.attachToBridge();

  const display = new DisplayNamespace(host);
  const debug = new DebugNamespace(host);

  return Promise.resolve({
    display,
    debug,
    moduleOverrides: {
      emx11Host: host,
      emx11NoAutoStart: true,
    },
    _host: host,
    dispose: () => host.dispose(),
  });
}
