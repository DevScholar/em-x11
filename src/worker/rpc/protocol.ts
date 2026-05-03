/**
 * Worker RPC protocol: every message that flows between Main ↔ Server Worker
 * and Server Worker ↔ each Client Worker. Mirrors the wire shape that
 * xorg's Unix-socket protocol uses between server and client processes,
 * adapted to browser postMessage semantics.
 *
 * Three transport topologies after bootstrap:
 *   - Main ↔ Server         : DOM input, worker lifecycle, connId alloc
 *   - Main ↔ each Client    : bootstrap, preRun staging, kill
 *   - Server ↔ each Client  : draw RPCs out, XEvent deliveries in. This
 *                             is the bulk path; bypasses main thread via
 *                             MessageChannel ports (no forwarding hop).
 *
 * Envelope:
 *   Any message either has no `seqId` (fire-and-forget) or a `seqId`
 *   paired with an eventual `{ kind: 'Reply', seqId, ok, ... }`.
 *   RpcChannel.call() issues the seqId and awaits the matching Reply.
 *
 * ORDERING: single-port postMessage is ordered. Void-then-sync sequences
 * (e.g. FillRect then Atom.Intern inside one C frame) arrive in order.
 * Do NOT split across channels without re-validating that.
 */

/* --- Bootstrap ----------------------------------------------------------- */

export interface BootstrapServer {
  kind: 'BootstrapServer';
  canvas: OffscreenCanvas;
  sab: SharedArrayBuffer;
  cssWidth: number;
  cssHeight: number;
}

export interface BootstrapClient {
  kind: 'BootstrapClient';
  glueUrl: string;
  wasmUrl: string;
  arguments?: string[];
  serverPort: MessagePort;   /* direct channel to Server Worker */
  sab: SharedArrayBuffer;
  connId: number;
  xidBase: number;
  xidMask: number;
}

export interface ConnectServerClientPort {
  kind: 'ConnectServerClientPort';
  connId: number;
  port: MessagePort;         /* server's end of the Server↔Client channel */
}

export interface AllocateConnIdReq {
  kind: 'AllocateConnId';
}
export interface AllocateConnIdResp {
  connId: number;
  xidBase: number;
  xidMask: number;
}

/* --- DOM relay (main → server) ------------------------------------------ */

export interface DomMouseMove {
  kind: 'Dom.MouseMove';
  x: number;             /* canvas-local CSS px */
  y: number;
  modifiers: number;     /* X11 modifier mask */
}
export interface DomMouseDown {
  kind: 'Dom.MouseDown';
  x: number;
  y: number;
  button: number;        /* X11 button: 1 left, 2 middle, 3 right */
  modifiers: number;
}
export interface DomMouseUp {
  kind: 'Dom.MouseUp';
  x: number;
  y: number;
  button: number;
  modifiers: number;
}
export interface DomKeyDown {
  kind: 'Dom.KeyDown';
  keysym: number;
  modifiers: number;
  hasFocus: boolean;
}
export interface DomKeyUp {
  kind: 'Dom.KeyUp';
  keysym: number;
  modifiers: number;
  hasFocus: boolean;
}
export interface DomFocusChange {
  kind: 'Dom.FocusChange';
  hasFocus: boolean;     /* canvas focus state */
}

export type DomRelayMessage =
  | DomMouseMove | DomMouseDown | DomMouseUp
  | DomKeyDown | DomKeyUp | DomFocusChange;

/* --- Reply envelope ----------------------------------------------------- */

export interface ReplyOk {
  kind: 'Reply';
  seqId: number;
  ok: true;
  value: unknown;
}
export interface ReplyErr {
  kind: 'Reply';
  seqId: number;
  ok: false;
  error: string;
}
export type Reply = ReplyOk | ReplyErr;

/* --- Top-level union (M1 surface; M2+ adds Draw/Window/Atom/Property) --- */

export type MainToServer =
  | BootstrapServer
  | ConnectServerClientPort
  | DomRelayMessage
  | (AllocateConnIdReq & { seqId: number });

export type ServerToMain = Reply;

export type AnyMessage =
  | MainToServer
  | ServerToMain;

/* --- Shared constants --------------------------------------------------- */

/** Timeout for RpcChannel.call() awaiting a reply. Long enough for
 *  first-paint measure-font bursts, short enough to surface deadlocks. */
export const RPC_DEFAULT_TIMEOUT_MS = 5000;

/** Sentinel seqId: starts at 1. 0 reserved for "no reply expected". */
export const RPC_FIRST_SEQ_ID = 1;
