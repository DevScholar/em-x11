/**
 * RpcChannel: thin wrapper over a `MessagePort` / `Worker` endpoint.
 *
 *   - `send(msg)`   — void fire-and-forget. Used for draw RPCs, window
 *                     updates, DOM event relay, XEvent delivery.
 *   - `call(msg)`   — reply-expected. Issues a fresh seqId, stashes
 *                     a {resolve, reject} in the pending map, rejects
 *                     on timeout. Used for sync-return bridges
 *                     (Atom.Intern, Property.Peek*, Font.Measure etc.).
 *   - `on(kind, h)` — register a handler for a specific message `kind`.
 *                     Returns a disposer.
 *
 * One-shot adaptation for DedicatedWorkerGlobalScope (Server Worker's
 * bootstrap port) vs MessagePort (all post-bootstrap channels) done by
 * accepting either in the constructor -- both expose the subset we use
 * (`postMessage`, `addEventListener`, `start()` on MessagePort).
 *
 * Mirrors the role of xlib's xcb_connection_t dispatcher: structured
 * reply routing on a shared transport.
 */

import type { Reply } from './protocol.js';
import { RPC_DEFAULT_TIMEOUT_MS, RPC_FIRST_SEQ_ID } from './protocol.js';

export type RpcEndpoint =
  | MessagePort
  | Worker
  | DedicatedWorkerGlobalScope;

interface Pending {
  resolve: (value: unknown) => void;
  reject: (reason: unknown) => void;
  timer: ReturnType<typeof setTimeout>;
}

type Handler<T = unknown> = (msg: T) => void;

export class RpcChannel {
  private readonly endpoint: RpcEndpoint;
  private seqCounter = RPC_FIRST_SEQ_ID;
  private readonly pending = new Map<number, Pending>();
  private readonly handlers = new Map<string, Set<Handler>>();
  private readonly timeoutMs: number;

  constructor(endpoint: RpcEndpoint, opts: { timeoutMs?: number } = {}) {
    this.endpoint = endpoint;
    this.timeoutMs = opts.timeoutMs ?? RPC_DEFAULT_TIMEOUT_MS;
    /* MessagePort requires explicit start() before messages flow when not
     * using the implicit-start `onmessage` setter. Safe to call on Worker
     * / DedicatedWorkerGlobalScope too -- those ignore it. */
    const port = endpoint as MessagePort;
    if (typeof port.start === 'function') port.start();
    endpoint.addEventListener(
      'message',
      (e: Event) => this.dispatch((e as MessageEvent).data),
    );
  }

  /** Void fire-and-forget. Caller must not await anything. */
  send(msg: object, transfer: Transferable[] = []): void {
    /* postMessage signatures differ between Worker (2-arg) and
     * MessagePort (2-arg) -- both accept (message, transferList). */
    (this.endpoint as MessagePort).postMessage(msg, transfer);
  }

  /** Reply-expected send. Adds a seqId, waits for matching Reply. */
  call<T = unknown>(
    msg: object,
    transfer: Transferable[] = [],
  ): Promise<T> {
    const seqId = this.seqCounter++;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        if (this.pending.delete(seqId)) {
          const kind = (msg as { kind?: string }).kind ?? '(unknown)';
          reject(new Error(`RPC timeout: ${kind} (seqId=${seqId})`));
        }
      }, this.timeoutMs);
      this.pending.set(seqId, {
        resolve: resolve as (v: unknown) => void,
        reject,
        timer,
      });
      (this.endpoint as MessagePort).postMessage(
        { ...msg, seqId },
        transfer,
      );
    });
  }

  /** Register a handler for incoming messages of a given `kind`. */
  on<T = unknown>(kind: string, handler: Handler<T>): () => void {
    let set = this.handlers.get(kind);
    if (!set) {
      set = new Set();
      this.handlers.set(kind, set);
    }
    set.add(handler as Handler);
    return () => {
      set!.delete(handler as Handler);
      if (set!.size === 0) this.handlers.delete(kind);
    };
  }

  /** Send a reply to a previously-received call. Used by the receiver
   *  side of sync RPCs (server → client, or server → main). */
  reply(seqId: number, value: unknown, transfer: Transferable[] = []): void {
    const msg: Reply = { kind: 'Reply', seqId, ok: true, value };
    (this.endpoint as MessagePort).postMessage(msg, transfer);
  }

  /** Send an error reply. */
  replyError(seqId: number, error: string): void {
    const msg: Reply = { kind: 'Reply', seqId, ok: false, error };
    (this.endpoint as MessagePort).postMessage(msg);
  }

  private dispatch(msg: unknown): void {
    if (!msg || typeof msg !== 'object') return;
    const anyMsg = msg as { kind?: string; seqId?: number };

    /* Reply path: resolve / reject the matching pending call. */
    if (anyMsg.kind === 'Reply' && typeof anyMsg.seqId === 'number') {
      const reply = anyMsg as Reply;
      const pending = this.pending.get(reply.seqId);
      if (!pending) return;           /* stale / already timed out */
      this.pending.delete(reply.seqId);
      clearTimeout(pending.timer);
      if (reply.ok) pending.resolve(reply.value);
      else pending.reject(new Error(reply.error));
      return;
    }

    /* Normal dispatch: call every handler registered for this kind. */
    if (typeof anyMsg.kind !== 'string') return;
    const set = this.handlers.get(anyMsg.kind);
    if (!set) return;
    for (const h of set) {
      try {
        h(anyMsg as unknown);
      } catch (err) {
        console.error(`[rpc] handler for ${anyMsg.kind} threw:`, err);
      }
    }
  }
}
