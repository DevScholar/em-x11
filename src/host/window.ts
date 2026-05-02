/**
 * Window manager. Mirrors xserver/dix/window.c structural ops -- create,
 * configure, map, unmap, destroy, reparent, ChangeWindowAttributes
 * (border / background / overrideRedirect) -- plus the shared-root
 * install that runs once at Host startup.
 *
 * Most methods are thin: the renderer holds the canonical window state
 * and does the painting. WindowManager's job is the cross-cutting glue
 * around each structural change: ownership tracking, expose synthesis,
 * SubstructureRedirect routing, property/subscription cleanup on destroy.
 */

import type { Host } from './index.js';
import { HOST_ROOT_ID, HOST_WEAVE_PIXMAP_ID } from './constants.js';

export class WindowManager {
  /** override_redirect flag per window (CWOverrideRedirect). True means
   *  "WM stays out" -- Host skips the redirect path for this window. */
  private readonly overrideRedirect = new Map<number, boolean>();

  constructor(private readonly host: Host) {}

  /** Create the single root window at Host-owned XID `HOST_ROOT_ID` and
   *  attach the classic X weave as its background_pixmap. Called once
   *  from the Host constructor; every client's XOpenDisplay then hands
   *  back this same XID rather than minting a per-connection root
   *  (which caused background clicks to hit the last-loaded wasm's
   *  root in Step 2). The renderer's flat window list still works
   *  because this root inserts first and stays at the bottom of
   *  z-order. */
  installSharedRoot(): void {
    const w = this.host.canvas.cssWidth;
    const h = this.host.canvas.cssHeight;
    this.host.renderer.addWindow(HOST_ROOT_ID, 0, 0, 0, w, h, 0, 0, 'pixel', 0xFFFFFF);
    this.host.renderer.mapWindow(HOST_ROOT_ID);
    this.host.connection.bindWindowToConn(HOST_ROOT_ID, 0); // conn_id=0 = Host

    /* Weave: 2×2 OffscreenCanvas, pure black + pure white on the
     * diagonal. Historically this dithered to gray on CRTs at the
     * period's DPI; on HiDPI displays you see the checker clearly,
     * which is authentic to the era. */
    const weave = new OffscreenCanvas(2, 2);
    const wctx = weave.getContext('2d');
    if (wctx) {
      wctx.fillStyle = '#000';
      wctx.fillRect(0, 0, 2, 2);
      wctx.fillStyle = '#FFF';
      wctx.fillRect(0, 0, 1, 1);
      wctx.fillRect(1, 1, 1, 1);
    }
    this.host.gc.installHostPixmap(HOST_WEAVE_PIXMAP_ID, weave, wctx!, 2, 2, 24);
    this.host.renderer.setWindowBackgroundPixmap(HOST_ROOT_ID, HOST_WEAVE_PIXMAP_ID);
  }

  /** XID of the one shared root window. Queried by every client's
   *  XOpenDisplay through the emx11_js_get_root_window bridge. */
  getRootWindow(): number {
    return HOST_ROOT_ID;
  }

  /** Cross-connection XGetWindowAttributes fallback. When twm queries
   *  xeyes's shell for geometry, twm's local shadow table has no entry
   *  for it (a WM doesn't mirror other clients' windows). Xlib calls
   *  the emx11_js_get_window_attrs bridge, which lands here.
   *  dix/window.c treats window state as XID-keyed server state; this
   *  accessor is how we present that to a querying client. */
  getAttrs(id: number): {
    x: number; y: number; width: number; height: number;
    mapped: boolean; overrideRedirect: boolean; borderWidth: number;
  } | null {
    const geom = this.host.renderer.attrsOf(id);
    if (!geom) return null;
    return {
      x: geom.x, y: geom.y, width: geom.width, height: geom.height,
      mapped: geom.mapped,
      overrideRedirect: this.overrideRedirect.get(id) ?? false,
      borderWidth: geom.borderWidth,
    };
  }

  onCreate(
    connId: number,
    id: number,
    parent: number,
    x: number,
    y: number,
    width: number,
    height: number,
    borderWidth: number,
    borderPixel: number,
    bgType: number,
    bgValue: number,
  ): void {
    this.host.connection.recordOwnership(connId, id);
    /* Bridge ints → renderer enum. ParentRelative (3) collapses to
     * 'none' for now -- proper impl needs parent-tile lookup at paint
     * time. xserver/dix/window.c:1212 stores ParentRelative as a
     * distinct state. */
    const type =
      bgType === 1 ? 'pixel'
      : bgType === 2 ? 'pixmap'
      : 'none';
    this.host.renderer.addWindow(
      id, parent, x, y, width, height, borderWidth, borderPixel,
      type,
      bgValue,
    );
  }

  onSetBorder(id: number, borderWidth: number, borderPixel: number): void {
    /* Width-changed path returns a non-empty exposed map (bounding rect
     * grew/shrank → clipLists shifted); color-only returns empty. */
    const exposed = this.host.renderer.setWindowBorder(id, borderWidth, borderPixel);
    this.host.events.pushExposesForRegions(exposed, null);
  }

  onSetBg(id: number, bgType: number, bgValue: number): void {
    const type =
      bgType === 1 ? 'pixel'
      : bgType === 2 ? 'pixmap'
      : 'none';
    this.host.renderer.setWindowBackground(id, type, bgValue);
  }

  onSetBgPixmap(id: number, pmId: number): void {
    this.host.renderer.setWindowBackgroundPixmap(id, pmId);
  }

  onConfigure(id: number, x: number, y: number, w: number, h: number): void {
    const exposed = this.host.renderer.configureWindow(id, x, y, w, h);
    /* Region-driven Expose: paintExposedRegions returned the per-window
     * newClip - oldClip diff (mirroring xserver miHandleValidateExposures,
     * mi/miexpose.c). Lower-z siblings whose clipList grew where this
     * window vacated, and the moved window itself for the area it now
     * covers, all get one Expose per rect. Pixels in oldClip ∩ newClip
     * are NOT in the diff so the client doesn't get a redundant
     * Expose / overpaint there. */
    this.host.events.pushExposesForRegions(exposed, null);
  }

  onMap(connId: number, id: number): void {
    /* SubstructureRedirect decision (x11protocol.txt §1592):
     *   Redirect applies iff the window's PARENT has a connection
     *   that selected SubstructureRedirectMask on it, AND the caller
     *   is a different connection, AND override_redirect is False.
     * Otherwise proceed with the actual map. The redirect path is
     * dormant in current demos -- no client subscribes to it -- but
     * the plumbing stays so a future WM (Host-embedded or another
     * X-client WM port) can light it up without touching Host. */
    const parent = this.host.renderer.parentOf(id);
    const holderConnId = parent !== 0 ? this.host.events.redirectHolderFor(parent) : null;
    const overrideRedirect = this.overrideRedirect.get(id) ?? false;
    if (
      holderConnId !== null &&
      holderConnId !== connId &&
      !overrideRedirect
    ) {
      this.host.events.dispatchMapRequest(holderConnId, parent, id);
      return;
    }
    /* Region-driven Expose: mapping a window expands its clipList from
     * empty to its new visible area; previously-higher-z siblings'
     * clipLists shrink (no expose for them, correctly). Already-mapped
     * descendants of a freshly-mapped ancestor (Xt maps children before
     * shell; mapping shell makes the whole subtree viewable for the
     * first time) get a non-empty diff via the recompute pass.
     *
     * Routing: pushExposesForRegions resolves each window's owner via
     * connOf(id). When the owner has no Module yet (initial bootstrap),
     * the call defers via deferExpose and ConnectionManager.launchClient
     * drains it once the Module binds. */
    const exposed = this.host.renderer.mapWindow(id);
    this.host.events.pushExposesForRegions(exposed, null);
  }

  onUnmap(_connId: number, id: number): void {
    /* Unmap isn't redirected in X -- only Map / Configure / Circulate
     * go through SubstructureRedirect. Unmap is always immediate.
     *
     * Region-driven Expose: the unmapped window vanishes from the
     * clipList map; lower-z siblings reclaim the vacated area through
     * newClip - oldClip > 0 and get one Expose per rect. The unmapped
     * window itself contributes empty newClip and never sees an Expose
     * (correct -- it's gone). */
    const exposed = this.host.renderer.unmapWindow(id);
    this.host.events.pushExposesForRegions(exposed, null);
  }

  onDestroy(id: number): void {
    this.host.connection.dropOwnership(id);
    this.host.events.forgetWindow(id);
    this.overrideRedirect.delete(id);
    this.host.property.deleteAllForWindow(id);   /* dix/property.c::DeleteAllWindowProperties */
    /* Same shape as onUnmap: lower-z siblings reclaim the area; we
     * Expose them. The destroyed window is gone from r.windows so it
     * receives no Expose (correct). */
    const exposed = this.host.renderer.destroyWindow(id);
    this.host.events.pushExposesForRegions(exposed, null);
  }

  onSetOverrideRedirect(id: number, flag: boolean): void {
    if (flag) this.overrideRedirect.set(id, true);
    else this.overrideRedirect.delete(id);
  }

  onRaise(id: number): void {
    /* Region-driven Expose: the raised window's clipList grows to
     * absorb the area previously-higher siblings used to cover.
     * exposed = newClip - oldClip = exactly that area. Lower-z
     * siblings whose clipLists shrank contribute empty exposed --
     * their pixels are NOT touched and they get no Expose. xorg
     * dix/window.c ReflectStackChange via VTStack runs this same
     * algorithm. */
    const exposed = this.host.renderer.raiseWindow(id);
    this.host.events.pushExposesForRegions(exposed, null);
  }

  onReparent(id: number, parent: number, x: number, y: number): void {
    const exposed = this.host.renderer.reparentWindow(id, parent, x, y);
    /* Notify the window's *owner* connection that its shadow is now stale.
     * In the TWM case the reparent is issued by twm's display, but xcalc
     * (the owner) still has its shell recorded as parent=root in its own
     * EmxWindow table. Without correcting that, the C-side window_abs_origin
     * walk lands at the pre-reparent absolute coords, ButtonPress/Motion
     * get translated with the wrong offset, hover/click hit the wrong
     * widget, and Xt's Shell widget never sees a ReparentNotify it would
     * otherwise consume. emx11_push_reparent_notify (event.c) updates
     * the local shadow unconditionally and pushes a ReparentNotify XEvent
     * gated on the StructureNotify/SubstructureNotify masks, mirroring
     * dix/events.c::DeliverEvents. */
    const ownerConnId = this.host.connection.connOf(id);
    if (ownerConnId !== undefined && ownerConnId !== 0) {
      const owner = this.host.connection.get(ownerConnId);
      if (owner?.module) {
        owner.module.ccall(
          'emx11_push_reparent_notify',
          null,
          ['number', 'number', 'number', 'number'],
          [id, parent, x, y],
        );
      }
    }
    /* Region-driven Expose: reparent = vacate old position + occupy
     * new position. The reparented window's exposed = newClip
     * (everything visible at NEW location it didn't cover before).
     * Lower-z windows at the OLD location get exposed in the freed
     * area through their own newClip - oldClip diffs. */
    this.host.events.pushExposesForRegions(exposed, null);
  }

  /** Used by ConnectionManager.closeDisplay to drop per-window
   *  override-redirect state without going through onDestroy (which
   *  would also touch the renderer that's being torn down). */
  forgetWindow(id: number): void {
    this.overrideRedirect.delete(id);
  }
}
