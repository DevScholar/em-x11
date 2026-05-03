/**
 * Client-worker spawner. One per wasm client. Creates a MessageChannel,
 * hands one end to the Server Worker via `ConnectServerClientPort`, and
 * the other end to the Client Worker via `BootstrapClient`. After this
 * returns, Server ↔ Client traffic flows directly on the MessageChannel
 * without touching the main thread.
 */

import type { RpcChannel } from '../rpc/channel.js';
import type { BootstrapClient, ConnectServerClientPort } from '../rpc/protocol.js';
import type { SabViews } from '../rpc/sab.js';

export interface SpawnClientOpts {
  /** URL of the Emscripten-produced glue.js. Loaded via dynamic import
   *  inside the Client Worker. */
  glueUrl: string;
  /** URL of the matching .wasm. Passed to factory via `locateFile`. */
  wasmUrl: string;
  /** argv for the wasm's main(). */
  arguments?: string[];
  /** Optional human-readable name for DevTools / logs. */
  name?: string;
}

export interface ClientWorkerHandle {
  worker: Worker;
  connId: number;
  /** Terminates the worker. Call when disconnecting. */
  kill(): void;
}

export interface AllocatedConn {
  connId: number;
  xidBase: number;
  xidMask: number;
  rootWindow: number;
}

/** Spawn one Client Worker. The orchestrator allocates connId on the
 *  Server Worker first, then spawns the Client Worker with that connId
 *  baked into its BootstrapClient message. */
export function spawnClientWorker(params: {
  serverChannel: RpcChannel;
  sab: SabViews;
  conn: AllocatedConn;
  opts: SpawnClientOpts;
}): ClientWorkerHandle {
  const { serverChannel, sab, conn, opts } = params;

  /* Direct Server↔Client channel. One port to each side; neither flows
   * through main thread. */
  const { port1: serverPort, port2: clientPort } = new MessageChannel();

  /* Hand serverPort to Server Worker via its main-to-server RpcChannel,
   * tagged with connId. Server will wrap it as an RpcChannel and
   * install a PortModuleSurface on Host.connection.bindModule. */
  const connect: ConnectServerClientPort = {
    kind: 'ConnectServerClientPort',
    connId: conn.connId,
    port: serverPort,
  };
  serverChannel.send(connect, [serverPort]);

  /* Spawn Client Worker and send bootstrap. Vite's worker-import-meta-url
   * plugin needs the options to be a literal object at the call site
   * so it can pick up `{ type: 'module' }`; we can't spread or use
   * shorthand property values here. Dynamic `name` is supplied as a
   * separate field after creation isn't possible, so skip it. */
  const worker = new Worker(
    new URL('../client/main.ts', import.meta.url),
    { type: 'module' },
  );
  const boot: BootstrapClient = {
    kind: 'BootstrapClient',
    glueUrl: opts.glueUrl,
    wasmUrl: opts.wasmUrl,
    ...(opts.arguments !== undefined ? { arguments: opts.arguments } : {}),
    serverPort: clientPort,
    sab: sab.sab,
    connId: conn.connId,
    xidBase: conn.xidBase,
    xidMask: conn.xidMask,
  };
  worker.postMessage(boot, [clientPort]);

  return {
    worker,
    connId: conn.connId,
    kill: () => worker.terminate(),
  };
}
