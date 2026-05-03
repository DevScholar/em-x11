/**
 * Main-thread helper: spawn the Server Worker, hand it the OffscreenCanvas
 * and the SharedArrayBuffer, wrap the worker port in an RpcChannel, and
 * return a handle for the orchestrator to use.
 *
 * `transferControlToOffscreen()` is one-shot: once called, the <canvas>
 * is permanently detached. Main thread can no longer draw to it; all
 * pixels come from the Server Worker thereafter. This matches the
 * Stage-3 design where main thread is pure input forwarder.
 *
 * COOP/COEP are required for SharedArrayBuffer -- already set in
 * vite.config.ts.
 */

import { RpcChannel } from '../rpc/channel.js';
import type { BootstrapServer } from '../rpc/protocol.js';
import { createSab, type SabViews } from '../rpc/sab.js';

export interface ServerWorkerHandle {
  worker: Worker;
  channel: RpcChannel;
  sab: SabViews;
  cssWidth: number;
  cssHeight: number;
}

export interface SpawnServerOpts {
  canvas: HTMLCanvasElement;
  cssWidth?: number;
  cssHeight?: number;
}

export function spawnServerWorker(opts: SpawnServerOpts): ServerWorkerHandle {
  const cssWidth = opts.cssWidth ?? (opts.canvas.width || 1024);
  const cssHeight = opts.cssHeight ?? (opts.canvas.height || 768);

  /* Set backing store before transfer -- the Server Worker re-applies
   * these dimensions on its side too, but setting here ensures the
   * OffscreenCanvas has sensible initial size even for a split second. */
  opts.canvas.width = cssWidth;
  opts.canvas.height = cssHeight;

  const sab = createSab();

  const worker = new Worker(
    /* vite picks this up as a worker entry and emits it as an ES module
     * per vite.config.ts `worker.format = 'es'`. */
    new URL('../server/main.ts', import.meta.url),
    { type: 'module', name: 'emx11-server' },
  );

  const offscreen = opts.canvas.transferControlToOffscreen();
  const bootstrap: BootstrapServer = {
    kind: 'BootstrapServer',
    canvas: offscreen,
    sab: sab.sab,
    cssWidth,
    cssHeight,
  };
  worker.postMessage(bootstrap, [offscreen]);

  const channel = new RpcChannel(worker);
  return { worker, channel, sab, cssWidth, cssHeight };
}
