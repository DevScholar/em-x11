/**
 * Window properties. Mirrors xserver/dix/property.c:
 * dixChangeWindowProperty, ProcGetProperty, DeleteProperty,
 * ProcListProperties.
 *
 * Properties live keyed by (XID, atom) so any client can read a
 * window's properties regardless of who set them. Moving the store
 * here from per-connection C tables (the old layout) is what lets a
 * WM read the managed client's WM_NAME / WM_HINTS / WM_PROTOCOLS at
 * all -- without it, twm and xeyes had disjoint property views and
 * twm couldn't decorate xeyes' shell.
 */

import type { Host } from './index.js';

/* Per-window property value. dix/property.c holds property storage
 * on the server's WindowPtr keyed by atom; we mirror that layout at
 * Host level so any client can reach it by (XID, atom). The `data`
 * field is raw bytes -- length = nitems * format/8. */
interface PropertyEntry {
  type: number;
  format: 8 | 16 | 32;
  nitems: number;
  data: Uint8Array;
}

/* X11 property modes we branch on explicitly (X.h). Prepend (= 1) is
 * handled implicitly as the else-branch of Append in change(). */
const PropModeReplace = 0;
const PropModeAppend = 2;

/* X11 AnyPropertyType sentinel (Xatom.h). */
const AnyPropertyType = 0;

export class PropertyManager {
  private readonly properties = new Map<number, Map<number, PropertyEntry>>();

  constructor(private readonly host: Host) {}

  /** XChangeProperty -- server-authoritative path (dix/property.c
   *  dixChangeWindowProperty). Stores by (window XID, atom) so any
   *  client can read it back regardless of who wrote it.
   *  @returns true on success, false on BadMatch (format/type mismatch
   *  with existing entry under Append/Prepend). */
  change(
    window: number, atom: number, type: number,
    format: 8 | 16 | 32, mode: number,
    data: Uint8Array,
  ): boolean {
    if (!this.host.renderer.attrsOf(window)) {
      /* BadWindow -- caller returns BadWindow. In real X this goes
       * through the resource manager; here the renderer is our
       * source of truth for "does this window exist". */
      return false;
    }
    let table = this.properties.get(window);
    if (!table) {
      table = new Map();
      this.properties.set(window, table);
    }
    const existing = table.get(atom);
    if (!existing || mode === PropModeReplace) {
      const entry: PropertyEntry = {
        type,
        format,
        nitems: (data.length * 8) / format,
        data: new Uint8Array(data),
      };
      table.set(atom, entry);
      return true;
    }
    /* Append / Prepend: format and type must match existing. */
    if (existing.format !== format || existing.type !== type) return false;
    if (data.length === 0) return true;
    const merged = new Uint8Array(existing.data.length + data.length);
    if (mode === PropModeAppend) {
      merged.set(existing.data, 0);
      merged.set(data, existing.data.length);
    } else {
      /* Prepend */
      merged.set(data, 0);
      merged.set(existing.data, data.length);
    }
    existing.data = merged;
    existing.nitems = (merged.length * 8) / format;
    return true;
  }

  /** XGetWindowProperty -- server-authoritative path (dix/property.c
   *  ProcGetProperty). Returns meta + a data slice corresponding to
   *  long_offset / long_length, both in 32-bit units per X protocol.
   *  `null` means BadWindow (unknown XID).
   *  When the atom doesn't exist, returns found=false with other
   *  fields zeroed -- matches Xlib's Success with type=None.
   *  When the stored type doesn't match reqType (and reqType isn't
   *  AnyPropertyType), returns found=false BUT with actualType/format
   *  populated so the caller can report them. */
  peek(
    window: number, atom: number, reqType: number,
    longOffset: number, longLength: number, deleteFlag: boolean,
  ): {
    found: boolean; type: number; format: number;
    nitems: number; bytesAfter: number; data: Uint8Array;
  } | null {
    if (!this.host.renderer.attrsOf(window)) return null;
    const table = this.properties.get(window);
    const entry = table?.get(atom);
    if (!entry) {
      return { found: false, type: 0, format: 0, nitems: 0, bytesAfter: 0,
               data: new Uint8Array(0) };
    }
    if (reqType !== AnyPropertyType && reqType !== entry.type) {
      return { found: false, type: entry.type, format: entry.format,
               nitems: 0, bytesAfter: 0, data: new Uint8Array(0) };
    }
    /* X protocol: long_offset and long_length are in 32-bit units,
     * regardless of format. Data slice in bytes = (total_bytes - 4*offset)
     * clamped to 4*length. */
    const totalBytes = entry.data.length;
    const startByte = Math.min(Math.max(longOffset, 0) * 4, totalBytes);
    const wantBytes = Math.max(longLength, 0) * 4;
    const availBytes = totalBytes - startByte;
    const sliceBytes = Math.min(availBytes, wantBytes);
    const data = entry.data.subarray(startByte, startByte + sliceBytes);
    const bytesAfter = availBytes - sliceBytes;
    const unit = entry.format / 8;
    const nitemsReturned = unit > 0 ? sliceBytes / unit : 0;

    if (deleteFlag && bytesAfter === 0 && startByte === 0) {
      table!.delete(atom);
      if (table!.size === 0) this.properties.delete(window);
    }
    return {
      found: true, type: entry.type, format: entry.format,
      nitems: nitemsReturned, bytesAfter,
      data: new Uint8Array(data),
    };
  }

  /** XDeleteProperty (dix/property.c DeleteProperty). */
  delete(window: number, atom: number): void {
    const table = this.properties.get(window);
    if (!table) return;
    table.delete(atom);
    if (table.size === 0) this.properties.delete(window);
  }

  /** XListProperties (dix/property.c ProcListProperties). */
  list(window: number): number[] {
    const table = this.properties.get(window);
    if (!table) return [];
    return Array.from(table.keys());
  }

  /** dix/property.c::DeleteAllWindowProperties. Called by WindowManager
   *  on XDestroyWindow and by ConnectionManager on XCloseDisplay for
   *  every window the connection owned. */
  deleteAllForWindow(window: number): void {
    this.properties.delete(window);
  }
}
