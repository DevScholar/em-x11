/**
 * Orchestrator: main-thread top-level for worker mode. Replaces `Host`
 * as the coordinator that the demo harness talks to.
 *
 * Owns:
 *   - The Server Worker handle + its RpcChannel
 *   - The shared SAB
 *   - The set of Client Worker handles
 *   - DOM input forwarder lifetime
 *
 * Analog of xorg's server-main + client-process lifecycle: we spawn
 * one "server" thread (the Server Worker) that owns the rendering and
 * dispatch, plus one "process" per client (each Client Worker).
 */

import type { RpcChannel } from '../rpc/channel.js';
import type { SabViews } from '../rpc/sab.js';
import type { AllocateConnIdResp } from '../rpc/protocol.js';
import { spawnServerWorker } from './server-proxy.js';
import { attachInputForwarder } from './input-forwarder.js';
import {
  spawnClientWorker,
  type AllocatedConn,
  type ClientWorkerHandle,
  type SpawnClientOpts,
} from './client-proxy.js';

export interface OrchestratorOptions {
  canvas: HTMLCanvasElement;
  cssWidth?: number;
  cssHeight?: number;
}

export class Orchestrator {
  readonly canvas: HTMLCanvasElement;
  readonly serverWorker: Worker;
  readonly serverChannel: RpcChannel;
  readonly sab: SabViews;
  private readonly clients: ClientWorkerHandle[] = [];
  private readonly detachInput: () => void;

  constructor(opts: OrchestratorOptions) {
    this.canvas = opts.canvas;
    const handle = spawnServerWorker({
      canvas: opts.canvas,
      ...(opts.cssWidth !== undefined ? { cssWidth: opts.cssWidth } : {}),
      ...(opts.cssHeight !== undefined ? { cssHeight: opts.cssHeight } : {}),
    });
    this.serverWorker = handle.worker;
    this.serverChannel = handle.channel;
    this.sab = handle.sab;
    this.detachInput = attachInputForwarder({
      canvas: opts.canvas,
      channel: handle.channel,
    });
  }

  /** Allocate a connId via the Server Worker, then spawn a Client
   *  Worker bound to it. Returns the handle for later termination. */
  async launchClient(opts: SpawnClientOpts): Promise<ClientWorkerHandle> {
    const conn = await this.serverChannel.call<AllocateConnIdResp>({
      kind: 'AllocateConnId',
    });
    const allocated: AllocatedConn = {
      connId: conn.connId,
      xidBase: conn.xidBase,
      xidMask: conn.xidMask,
      rootWindow: conn.rootWindow,
    };
    const handle = spawnClientWorker({
      serverChannel: this.serverChannel,
      sab: this.sab,
      conn: allocated,
      opts,
    });
    this.clients.push(handle);
    return handle;
  }

  /** Kill all clients and the server worker. Not typically called
   *  outside teardown tests; page reload also handles it. */
  terminate(): void {
    for (const c of this.clients) c.kill();
    this.clients.length = 0;
    this.detachInput();
    this.serverWorker.terminate();
  }
}
