/**
 * default-host.ts — standalone IIFE entry point for Layer 1 (zero JS).
 *
 * Compiled by Vite into a self-contained IIFE that sets
 * `globalThis.EmX11DefaultHost` before the emscripten wasm starts.
 * The JS library (library_emx11.js) calls EmX11DefaultHost.create(Module)
 * during $EmX11Host.init().
 *
 * This file is NOT imported by the npm package entry (src/index.ts).
 * It is only used for the --pre-js bundle that the port injects when
 * the user compiles with -sUSE_EMX11 (no user JS needed).
 */

import { Host } from './host/index.js';
import type { HostOptions } from './host/index.js';

interface DefaultHostModule {
  create(mod: Record<string, unknown>): unknown;
}

function createDefaultHost(Module: Record<string, unknown>): unknown {
  const canvas = resolveCanvas(Module);
  const opts: HostOptions = {};

  if (canvas instanceof HTMLCanvasElement) {
    opts.element = canvas;
  } else if (canvas instanceof OffscreenCanvas) {
    opts.surface = canvas;
  }

  // Read dimensions from Module or canvas
  const w = (Module['emx11Width'] as number) ?? (canvas as HTMLCanvasElement).width ?? 1024;
  const h = (Module['emx11Height'] as number) ?? (canvas as HTMLCanvasElement).height ?? 768;
  if (w) opts.width = w;
  if (h) opts.height = h;

  // HiDPI opt-out: set Module['emx11HiDpi'] = false to revert to 1:1
  // backing store when non-integer DPR causes antialiasing artifacts
  // or layout misalignment (e.g. xcalc's Athena Toggle LCD ghosting).
  if (Module['emx11HiDpi'] === false) opts.hiDpi = false;

  const host = new Host(opts);
  host.attachToBridge();

  // Mirror the host to Module so DevTools can reach it
  Module['emx11Host'] = host;

  return host;
}

function resolveCanvas(Module: Record<string, unknown>): HTMLCanvasElement | OffscreenCanvas {
  // 1. Module['emx11Canvas'] (user override)
  const explicit = Module['emx11Canvas'];
  if (explicit instanceof HTMLCanvasElement || (typeof OffscreenCanvas !== 'undefined' && explicit instanceof OffscreenCanvas)) {
    return explicit;
  }

  // 2. Module.canvas (emscripten convention)
  const mc = Module['canvas'];
  if (mc instanceof HTMLCanvasElement || (typeof OffscreenCanvas !== 'undefined' && mc instanceof OffscreenCanvas)) {
    return mc;
  }

  // 3. document.querySelector('#canvas') (emscripten shell convention)
  if (typeof document !== 'undefined') {
    const qs = document.querySelector('#canvas');
    if (qs instanceof HTMLCanvasElement) {
      Module['canvas'] = qs;
      return qs;
    }
  }

  // 4. Auto-create a canvas (DOM only — skip in Worker context)
  if (typeof document === 'undefined') {
    throw new Error('em-x11: no canvas provided and document is not available (running in a Worker?)');
  }
  const c = document.createElement('canvas');
  c.id = 'canvas';
  c.width = (Module['emx11Width'] as number) ?? 1024;
  c.height = (Module['emx11Height'] as number) ?? 768;
  c.style.display = 'block';
  document.body.appendChild(c);
  Module['canvas'] = c;
  Module['emx11Canvas'] = c;
  return c;
}

// Expose as a global so library_emx11.js can find it during init.
(globalThis as Record<string, unknown>).EmX11DefaultHost = { create: createDefaultHost } satisfies DefaultHostModule;

// Auto-init for Layer 1: when this IIFE runs inside an emscripten
// MODULARIZE=1 factory, `Module` is a `var` in the enclosing scope.
// We call createDefaultHost directly so the EM_JS bridges (which read
// Module['emx11Host']) work immediately, without depending on the
// library_emx11.js override (which may not take effect if the EM_JS
// bodies in bridges.c are not overridden).
declare var Module: any;
if (typeof Module !== 'undefined' && !Module['emx11Host'] && !Module['emx11NoAutoStart']) {
  createDefaultHost(Module);
}
