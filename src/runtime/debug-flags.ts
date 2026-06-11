/**
 * Module-level debug-flag store for Host TypeScript code.
 *
 * Host TS files (hit-test, window-tree, draw, devices) are bundled by
 * Vite and run in the user's ES module scope, OUTSIDE the emscripten
 * factory.  Module (the emscripten Module object) is NOT a global in
 * that context — it only exists as a local variable inside the
 * emscripten factory.
 *
 * This module provides a dependency-free storage location that both
 * the Host render code and the public API (DebugNamespace) can access.
 * Host.attachToBridge() writes the same object reference to BOTH this
 * store AND Module['emX11Debug'] so that:
 *
 *   - JS library / EM_JS code (inside the factory) reads Module['emX11Debug']
 *   - Host TS code (outside the factory) reads getDebugFlags()
 *   - DevTools toggles on Module['emX11Debug'] are visible to both (same object)
 */

export interface EmX11DebugFlags {
  traceHit: boolean;
  traceHitNext: boolean;
  traceMotion: boolean;
  traceButton: boolean;
  tracePaint: boolean;
  traceCBtn: boolean;
  traceCMot: boolean;
  traceMove: boolean;
  traceQp: boolean;
}

const DEFAULTS: EmX11DebugFlags = {
  traceHit: false,
  traceHitNext: false,
  traceMotion: false,
  traceButton: false,
  tracePaint: false,
  traceCBtn: false,
  traceCMot: false,
  traceMove: false,
  traceQp: false,
};

let _flags: EmX11DebugFlags | undefined;

export function getDebugFlags(): EmX11DebugFlags | undefined {
  return _flags;
}

export function ensureDebugFlags(): EmX11DebugFlags {
  if (!_flags) {
    _flags = { ...DEFAULTS };
  }
  return _flags;
}

export function setDebugFlags(flags: EmX11DebugFlags): void {
  _flags = flags;
}
