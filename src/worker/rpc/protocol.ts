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
  rootWindow: number;
}

/* --- Draw / window / grab / property (Server ← Client, void or reply) --- */

/* All share `id` etc. as 32-bit fields. Server Worker receives these on
 * the per-client port and dispatches to Host methods. */

export type ClientToServerVoid =
  | { kind: 'Core.Init'; screenWidth: number; screenHeight: number }
  | { kind: 'Core.Flush' }
  | { kind: 'Display.Close' }
  | { kind: 'Draw.ClearArea'; id: number; x: number; y: number; w: number; h: number }
  | { kind: 'Draw.FillRect'; id: number; x: number; y: number; w: number; h: number; color: number }
  | { kind: 'Draw.Line'; id: number; x1: number; y1: number; x2: number; y2: number; color: number; lineWidth: number }
  | { kind: 'Draw.Arc'; id: number; x: number; y: number; w: number; h: number; angle1: number; angle2: number; color: number; lineWidth: number }
  | { kind: 'Draw.FillArc'; id: number; x: number; y: number; w: number; h: number; angle1: number; angle2: number; color: number }
  | { kind: 'Draw.FillPolygon'; id: number; pts: Array<{ x: number; y: number }>; shape: number; mode: number; color: number }
  | { kind: 'Draw.Points'; id: number; pts: Array<{ x: number; y: number }>; mode: number; color: number }
  | { kind: 'Draw.String'; id: number; x: number; y: number; font: string; text: string; fg: number; bg: number; imageMode: number }
  | { kind: 'Draw.PixmapCreate'; id: number; width: number; height: number; depth: number }
  | { kind: 'Draw.PixmapDestroy'; id: number }
  | { kind: 'Draw.ShapeCombineMask'; destId: number; srcId: number; xOff: number; yOff: number; op: number }
  | { kind: 'Draw.CopyArea'; srcId: number; dstId: number; srcX: number; srcY: number; w: number; h: number; dstX: number; dstY: number }
  | { kind: 'Draw.CopyPlane'; srcId: number; dstId: number; srcX: number; srcY: number; w: number; h: number; dstX: number; dstY: number; plane: number; fg: number; bg: number; applyBg: boolean }
  | { kind: 'Draw.PutImage'; dstId: number; dstX: number; dstY: number; w: number; h: number; format: number; depth: number; bytesPerLine: number; data: Uint8Array; fg: number; bg: number }
  | { kind: 'Window.Create'; connId: number; id: number; parent: number; x: number; y: number; w: number; h: number; borderWidth: number; borderPixel: number; bgType: number; bgValue: number }
  | { kind: 'Window.SetBorder'; id: number; borderWidth: number; borderPixel: number }
  | { kind: 'Window.SetBg'; id: number; bgType: number; bgValue: number }
  | { kind: 'Window.SetBgPixmap'; id: number; pmId: number }
  | { kind: 'Window.Configure'; id: number; x: number; y: number; w: number; h: number }
  | { kind: 'Window.Map'; connId: number; id: number }
  | { kind: 'Window.Unmap'; connId: number; id: number }
  | { kind: 'Window.Destroy'; id: number }
  | { kind: 'Window.Raise'; id: number }
  | { kind: 'Window.SelectInput'; connId: number; id: number; mask: number }
  | { kind: 'Window.SetOverrideRedirect'; id: number; flag: boolean }
  | { kind: 'Window.Reparent'; id: number; parent: number; x: number; y: number }
  | { kind: 'Window.Shape'; id: number; rects: Array<{ x: number; y: number; w: number; h: number }> }
  | { kind: 'Grab.Button'; window: number; button: number; modifiers: number; ownerEvents: boolean; eventMask: number; pointerMode: number; keyboardMode: number; confineTo: number; cursor: number }
  | { kind: 'Ungrab.Button'; window: number; button: number; modifiers: number }
  | { kind: 'Property.Change'; w: number; atom: number; type: number; format: number; mode: number; data: Uint8Array }
  | { kind: 'Property.Delete'; w: number; atom: number }
  | { kind: 'Clipboard.WriteUtf8'; bytes: Uint8Array };

export type ClientToServerCall =
  | { kind: 'Atom.Intern'; seqId: number; name: string; onlyIfExists: boolean }
  | { kind: 'Atom.GetName'; seqId: number; atom: number }
  | { kind: 'Property.PeekMeta'; seqId: number; w: number; atom: number; reqType: number; longOffset: number; longLength: number }
  | { kind: 'Property.PeekData'; seqId: number; w: number; atom: number; reqType: number; longOffset: number; longLength: number; deleteFlag: boolean }
  | { kind: 'Property.ListCount'; seqId: number; w: number };

/* --- XEvent delivery (Server → Client, void) ----------------------------- */

export type ServerToClientXEvent =
  | { kind: 'XEvent.Button'; xType: number; window: number; lx: number; ly: number; x_root: number; y_root: number; button: number; state: number }
  | { kind: 'XEvent.Motion'; window: number; x: number; y: number; x_root: number; y_root: number; state: number }
  | { kind: 'XEvent.Key'; xType: number; window: number; keysym: number; state: number; x: number; y: number }
  | { kind: 'XEvent.Expose'; window: number; x: number; y: number; w: number; h: number }
  | { kind: 'XEvent.MapRequest'; parent: number; window: number }
  | { kind: 'XEvent.ReparentNotify'; window: number; parent: number; x: number; y: number };

/* SAB slot assignment: pushed after every Window.Create that originated
 * from this client so its `__EMX11_XID_TO_SLOT__` lookup stays current. */
export type ServerToClientSlot =
  | { kind: 'Slot.Assigned'; winId: number; slot: number }
  | { kind: 'Slot.Freed'; winId: number };

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
