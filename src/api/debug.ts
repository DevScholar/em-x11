/**
 * em.debug — toggleable trace flags + state dumpers.
 *
 * The flag fields are accessor pairs over `globalThis.emX11._debug`,
 * so toggling `em.debug.traceHit = true` and toggling
 * `globalThis.emX11._debug.traceHit = true` are equivalent — but the
 * `em.debug.*` form is the public API and the underscore form is
 * implementation detail (read by hit-test.ts and the EM_ASM gates in
 * the C bridges).
 */

import type { Host } from '../host/index.js';
import { dumpWindows as renderDumpWindows } from '../host/render/hit-test.js';
import type { EmX11Debug } from './types.js';

export class DebugNamespace implements EmX11Debug {
  constructor(private readonly host: Host) {}

  private get state() {
    /* host.attachToBridge() guarantees emX11._debug exists; the
     * non-null assertion below would only fail if a caller bypassed
     * the factory and never attached. */
    return globalThis.emX11!._debug!;
  }

  get traceHit(): boolean { return this.state.traceHit; }
  set traceHit(v: boolean) { this.state.traceHit = v; }

  get traceHitNext(): boolean { return this.state.traceHitNext; }
  set traceHitNext(v: boolean) { this.state.traceHitNext = v; }

  get traceMotion(): boolean { return this.state.traceMotion; }
  set traceMotion(v: boolean) { this.state.traceMotion = v; }

  get traceButton(): boolean { return this.state.traceButton; }
  set traceButton(v: boolean) { this.state.traceButton = v; }

  get tracePaint(): boolean { return this.state.tracePaint; }
  set tracePaint(v: boolean) { this.state.tracePaint = v; }

  get traceCBtn(): boolean { return this.state.traceCBtn; }
  set traceCBtn(v: boolean) { this.state.traceCBtn = v; }

  get traceCMot(): boolean { return this.state.traceCMot; }
  set traceCMot(v: boolean) { this.state.traceCMot = v; }

  get traceMove(): boolean { return this.state.traceMove; }
  set traceMove(v: boolean) { this.state.traceMove = v; }

  get traceQp(): boolean { return this.state.traceQp; }
  set traceQp(v: boolean) { this.state.traceQp = v; }

  dumpWindows(): void {
    renderDumpWindows(this.host.renderer);
  }

  dumpGrabs(): void {
    this.host.grabs.dump();
  }
}
