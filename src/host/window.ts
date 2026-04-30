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
    this.host.renderer.addWindow(HOST_ROOT_ID, 0, 0, 0, w, h, 0, 0, 0xFFFFFF);
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
    background: number,
  ): void {
    this.host.connection.recordOwnership(connId, id);
    this.host.renderer.addWindow(
      id, parent, x, y, width, height, borderWidth, borderPixel, background,
    );
  }

  onSetBorder(id: number, borderWidth: number, borderPixel: number): void {
    this.host.renderer.setWindowBorder(id, borderWidth, borderPixel);
  }

  onSetBg(id: number, background: number): void {
    this.host.renderer.setWindowBackground(id, background);
  }

  onSetBgPixmap(id: number, pmId: number): void {
    this.host.renderer.setWindowBackgroundPixmap(id, pmId);
  }

  onConfigure(id: number, x: number, y: number, w: number, h: number): void {
    this.host.renderer.configureWindow(id, x, y, w, h);
    /* Configuring a mapped window erases its old content (we have no
     * per-window backing store), so the owner needs an Expose to redraw.
     * Routes via the window's owner module rather than the calling
     * connection: a WM resizing a managed client's shell is a cross-
     * connection call. attrsOf() reflects the post-configure state, so
     * we read mapped from there.
     *
     * Subtree Expose: paintWindowSubtree (host/render/paint.ts) wipes
     * this window AND every mapped descendant back to its background.
     * A TWM frame has the title bar (title_w), iconify button, and
     * resize handles as its children; without an Expose to each of
     * them, their content stays wiped and the frame looks like a solid
     * teal rectangle. Descendants can belong to a different owner than
     * the configured window (TWM resizes its own frame but the shell
     * inside belongs to the client), so pushExposeForWindow routes
     * each via its own owner module. */
    const attrs = this.host.renderer.attrsOf(id);
    if (attrs?.mapped) {
      this.host.events.pushExposeForWindow(id, null);
      for (const descendant of this.host.renderer.mappedDescendants(id)) {
        this.host.events.pushExposeForWindow(descendant, null);
      }
    }
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
    this.host.renderer.mapWindow(id);
    /* Expose synthesis: route to the window's actual owner, not the
     * calling connection. C-side XMapWindow used to push directly into
     * the caller's queue, which only worked for self-mapped windows --
     * a WM mapping a managed client's shell is the canonical case where
     * caller != owner, and the previous shape sent Expose to the WM
     * instead of the client whose shell just appeared on screen.
     *
     * pushExposeForWindow handles both bound and unbound owner modules:
     * during the initial XOpenDisplay -> XMapWindow burst, the owner's
     * Module reference isn't installed yet (launchClient awaits the
     * factory which only resolves after main() suspends in XNextEvent's
     * emscripten_sleep). Those Exposes get queued in pendingExposes and
     * drained from launchClient once `conn.module` lands.
     *
     * Subtree Expose: if this window has already-mapped descendants
     * (e.g. a reparented shell under a freshly-mapped WM frame), they
     * also got wiped by paintWindowSubtree and need Expose to redraw.
     *
     * Viewability gate: skip Expose when the window isn't actually
     * viewable (ancestor chain still contains an unmapped node). X
     * servers never send Expose to a non-viewable window; doing so
     * would let the client paint at coordinates the renderer
     * deliberately skipped, which produces the (0,0) ghost-paint
     * artefact when Xt maps children before their shell. */
    if (!this.host.renderer.isViewable(id)) return;
    this.host.events.pushExposeForWindow(id, null);
    for (const descendant of this.host.renderer.mappedDescendants(id)) {
      this.host.events.pushExposeForWindow(descendant, null);
    }
  }

  onUnmap(_connId: number, id: number): void {
    /* Unmap isn't redirected in X -- only Map / Configure / Circulate
     * go through SubstructureRedirect. Unmap is always immediate.
     *
     * Expose synthesis on overpaint: when our renderer wipes the rect
     * the unmapped window vacated, every mapped window that intersects
     * gets its bg repainted (wiping any application drawings inside the
     * intersection). Real X uses per-window backing store + DamageReport
     * to drive Expose to those siblings; we emit the Exposes explicitly
     * here. Without this, twm's icon-manager-driven deiconify (which
     * unmaps the iconified placeholder window) leaves xcalc / xeyes
     * widget pixels wiped under that placeholder's old footprint, and
     * the visible buttons all go blank until re-hovered. */
    const oldRect = this.host.renderer.absBoundingRect(id);
    this.host.renderer.unmapWindow(id);
    if (oldRect) {
      const affected = this.host.renderer.mappedWindowsIntersecting(
        oldRect.ax, oldRect.ay, oldRect.w, oldRect.h, id,
      );
      for (const wId of affected) {
        this.host.events.pushExposeForWindow(wId, null);
      }
    }
  }

  onDestroy(id: number): void {
    /* Same Expose-on-overpaint dance as onUnmap: capture the old rect
     * before destroyWindow mutates the tree, then re-expose any
     * intersecting mapped sibling whose pixels were wiped. */
    const oldRect = this.host.renderer.absBoundingRect(id);
    this.host.connection.dropOwnership(id);
    this.host.events.forgetWindow(id);
    this.overrideRedirect.delete(id);
    this.host.property.deleteAllForWindow(id);   /* dix/property.c::DeleteAllWindowProperties */
    this.host.renderer.destroyWindow(id);
    if (oldRect) {
      const affected = this.host.renderer.mappedWindowsIntersecting(
        oldRect.ax, oldRect.ay, oldRect.w, oldRect.h, id,
      );
      for (const wId of affected) {
        this.host.events.pushExposeForWindow(wId, null);
      }
    }
  }

  onSetOverrideRedirect(id: number, flag: boolean): void {
    if (flag) this.overrideRedirect.set(id, true);
    else this.overrideRedirect.delete(id);
  }

  onReparent(id: number, parent: number, x: number, y: number): void {
    this.host.renderer.reparentWindow(id, parent, x, y);
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
     * dix/events.c::DeliverEvents. See project_emx11_mask_gating.md for
     * the adjacent gate that landed alongside this. */
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
    /* After reparent the renderer repainted background at the new
     * position but did not restore pixel content; the reparented window
     * and its mapped descendants need Expose to redraw. In the TWM case
     * this is how xeyes learns that its shell has been moved under a
     * frame and has to repaint its eyes inside the new absolute area --
     * without it, the canvas only shows the teal frame background and
     * no client-drawn content. attrsOf tells us whether the moved
     * window is actually visible (an unmapped reparent doesn't need
     * Expose; only a mapped one). */
    const attrs = this.host.renderer.attrsOf(id);
    if (attrs?.mapped) {
      this.host.events.pushExposeForWindow(id, null);
      for (const descendant of this.host.renderer.mappedDescendants(id)) {
        this.host.events.pushExposeForWindow(descendant, null);
      }
    }
  }

  /** Used by ConnectionManager.closeDisplay to drop per-window
   *  override-redirect state without going through onDestroy (which
   *  would also touch the renderer that's being torn down). */
  forgetWindow(id: number): void {
    this.overrideRedirect.delete(id);
  }
}
