/**
 * SharedArrayBuffer layout + seqlock for tear-free reads.
 *
 * The Server Worker publishes hot state here; every Client Worker reads
 * from it directly in its SAB-backed EM_JS bridges. Avoids an
 * await-round-trip per XQueryPointer / XGetWindowAttributes call, which
 * at 50ms × multiple clients × 20-per-tick polling is a LOT of round
 * trips to amortise.
 *
 * Layout (all Int32 views, LE):
 *
 *   0x000  GEN_COUNTER   seqlock: even = stable, odd = writer in flight
 *   0x004  POINTER_X
 *   0x008  POINTER_Y
 *   0x00C  MAX_WINDOW_SLOTS  (=128, one-shot at header write)
 *   0x010  WIN_ATTR_STRIDE   (=8,   one-shot)
 *   0x014  ABS_ORIGIN_STRIDE (=3,   one-shot)
 *   0x018  reserved (40 bytes)
 *   0x040  WinAttrTable[128 * 8]   — PRESENT, X, Y, W, H, MAPPED, OVERRIDE, BW
 *   0x1040 AbsOriginTable[128 * 3] — PRESENT, AX, AY
 *   0x1640 slack to page boundary
 *
 * The "slot index" is allocated per window on Window.Create (server-
 * side slot allocator). Slot 0 is reserved as "not yet mirrored" -- a
 * reader that can't resolve xid→slot should fall back to the EM_ASYNC_JS
 * round-trip path.
 *
 * Seqlock discipline (writers):
 *   Atomics.add(i32, GEN_IDX, 1)   // gen becomes odd
 *   ...mutate slot fields...
 *   Atomics.add(i32, GEN_IDX, 1)   // gen becomes even
 *
 * Seqlock discipline (readers):
 *   do {
 *     g0 = Atomics.load(i32, GEN_IDX)
 *     if (g0 & 1) retry;     // writer in flight
 *     ...snapshot fields...
 *     g1 = Atomics.load(i32, GEN_IDX)
 *   } while (g0 !== g1);
 *
 * Single writer (Server Worker) means no write-write contention. The
 * seqlock just guards against readers racing with in-progress writes.
 */

/* --- Layout constants --------------------------------------------------- */

/** GEN_COUNTER at i32[0]. */
export const SAB_GEN_IDX = 0;
export const SAB_POINTER_X_IDX = 1;
export const SAB_POINTER_Y_IDX = 2;
export const SAB_MAX_SLOTS_IDX = 3;
export const SAB_WIN_ATTR_STRIDE_IDX = 4;
export const SAB_ABS_ORIGIN_STRIDE_IDX = 5;

/** Header size in Int32 elements: 6 fields + 10 reserved = 16 ints = 64 bytes. */
export const SAB_HEADER_INTS = 16;

export const SAB_MAX_WINDOW_SLOTS = 128;
export const SAB_WIN_ATTR_STRIDE = 8;    /* matches emx11_meta_layout.h */
export const SAB_ABS_ORIGIN_STRIDE = 3;  /* matches emx11_meta_layout.h */

/* Per-slot offsets within WinAttr entry */
export const WIN_ATTR_PRESENT = 0;
export const WIN_ATTR_X = 1;
export const WIN_ATTR_Y = 2;
export const WIN_ATTR_W = 3;
export const WIN_ATTR_H = 4;
export const WIN_ATTR_MAPPED = 5;
export const WIN_ATTR_OVERRIDE = 6;
export const WIN_ATTR_BORDER_WIDTH = 7;

/* Per-slot offsets within AbsOrigin entry */
export const ABS_ORIGIN_PRESENT = 0;
export const ABS_ORIGIN_AX = 1;
export const ABS_ORIGIN_AY = 2;

/* --- Derived sizes ------------------------------------------------------ */

const WIN_ATTR_TABLE_INTS = SAB_MAX_WINDOW_SLOTS * SAB_WIN_ATTR_STRIDE;
const ABS_ORIGIN_TABLE_INTS = SAB_MAX_WINDOW_SLOTS * SAB_ABS_ORIGIN_STRIDE;

/** Total SAB size, in bytes. Page-aligned (8192). */
export const SAB_TOTAL_BYTES = 8192;

/** Offset of WinAttrTable in Int32 elements. */
export const SAB_WIN_ATTR_TABLE_IDX = SAB_HEADER_INTS;
/** Offset of AbsOriginTable in Int32 elements. */
export const SAB_ABS_ORIGIN_TABLE_IDX =
  SAB_WIN_ATTR_TABLE_IDX + WIN_ATTR_TABLE_INTS;

/* Validate we fit: (16 header + 1024 attr + 384 origin) * 4 = 5696 bytes < 8192 */
const _fits = SAB_ABS_ORIGIN_TABLE_IDX + ABS_ORIGIN_TABLE_INTS;
if (_fits * 4 > SAB_TOTAL_BYTES) {
  /* Compile-time assertion via runtime throw at module load. Tiny cost; only fires if someone mis-tunes the constants. */
  throw new Error(
    `em-x11: SAB layout overflow (${_fits * 4} > ${SAB_TOTAL_BYTES})`,
  );
}

/* --- Views + allocation ------------------------------------------------- */

export interface SabViews {
  readonly sab: SharedArrayBuffer;
  readonly i32: Int32Array;
  readonly u32: Uint32Array;
}

export function createSab(): SabViews {
  const sab = new SharedArrayBuffer(SAB_TOTAL_BYTES);
  const views = attachSab(sab);
  /* One-shot header constants so Clients can sanity-check. */
  views.i32[SAB_MAX_SLOTS_IDX] = SAB_MAX_WINDOW_SLOTS;
  views.i32[SAB_WIN_ATTR_STRIDE_IDX] = SAB_WIN_ATTR_STRIDE;
  views.i32[SAB_ABS_ORIGIN_STRIDE_IDX] = SAB_ABS_ORIGIN_STRIDE;
  return views;
}

export function attachSab(sab: SharedArrayBuffer): SabViews {
  return {
    sab,
    i32: new Int32Array(sab),
    u32: new Uint32Array(sab),
  };
}

/* --- Writer helpers (Server Worker only) --------------------------------- */

/** Begin a seqlock write critical section. Pair with `endWrite`. */
export function beginWrite(v: SabViews): void {
  Atomics.add(v.i32, SAB_GEN_IDX, 1);      /* odd = writer in flight */
}

export function endWrite(v: SabViews): void {
  Atomics.add(v.i32, SAB_GEN_IDX, 1);      /* even = stable */
}

export function writePointer(v: SabViews, x: number, y: number): void {
  beginWrite(v);
  v.i32[SAB_POINTER_X_IDX] = x | 0;
  v.i32[SAB_POINTER_Y_IDX] = y | 0;
  endWrite(v);
}

/** Write an attr slot. Slot 0 is reserved as "not mirrored" — do not
 *  use it for real windows. */
export function writeWinAttr(
  v: SabViews,
  slot: number,
  present: number,
  x: number,
  y: number,
  w: number,
  h: number,
  mapped: number,
  override: number,
  borderWidth: number,
): void {
  if (slot <= 0 || slot >= SAB_MAX_WINDOW_SLOTS) return;
  const base = SAB_WIN_ATTR_TABLE_IDX + slot * SAB_WIN_ATTR_STRIDE;
  beginWrite(v);
  v.i32[base + WIN_ATTR_PRESENT] = present;
  v.i32[base + WIN_ATTR_X] = x | 0;
  v.i32[base + WIN_ATTR_Y] = y | 0;
  v.i32[base + WIN_ATTR_W] = w | 0;
  v.i32[base + WIN_ATTR_H] = h | 0;
  v.i32[base + WIN_ATTR_MAPPED] = mapped;
  v.i32[base + WIN_ATTR_OVERRIDE] = override;
  v.i32[base + WIN_ATTR_BORDER_WIDTH] = borderWidth | 0;
  endWrite(v);
}

export function writeAbsOrigin(
  v: SabViews,
  slot: number,
  present: number,
  ax: number,
  ay: number,
): void {
  if (slot <= 0 || slot >= SAB_MAX_WINDOW_SLOTS) return;
  const base = SAB_ABS_ORIGIN_TABLE_IDX + slot * SAB_ABS_ORIGIN_STRIDE;
  beginWrite(v);
  v.i32[base + ABS_ORIGIN_PRESENT] = present;
  v.i32[base + ABS_ORIGIN_AX] = ax | 0;
  v.i32[base + ABS_ORIGIN_AY] = ay | 0;
  endWrite(v);
}

/** Mark a slot as not-present. Used on window destroy. */
export function clearWinSlot(v: SabViews, slot: number): void {
  if (slot <= 0 || slot >= SAB_MAX_WINDOW_SLOTS) return;
  const attrBase = SAB_WIN_ATTR_TABLE_IDX + slot * SAB_WIN_ATTR_STRIDE;
  const origBase = SAB_ABS_ORIGIN_TABLE_IDX + slot * SAB_ABS_ORIGIN_STRIDE;
  beginWrite(v);
  v.i32[attrBase + WIN_ATTR_PRESENT] = 0;
  v.i32[origBase + ABS_ORIGIN_PRESENT] = 0;
  endWrite(v);
}
