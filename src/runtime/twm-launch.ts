/**
 * twm launcher.
 *
 * The vendored twm in third-party/ is a verbatim X.Org tarball that gets
 * fetched at setup time and is NOT git-tracked. Patching it in-place would
 * vanish on the next fetch -- so any em-x11 specific tweaks live in the
 * runtime instead.
 *
 * The one tweak we need is `RandomPlacement`: without it twm's AddWindow
 * blocks in XMaskEvent waiting for a button press to commit the new
 * window's position (see add_window.c around line 532). On real X this
 * works because there's a human at the keyboard who clicks; in our
 * browser world the only client running while twm waits is twm itself
 * (xeyes is suspended on its redirected XMapWindow), so the wait would
 * never end. RandomPlacement makes twm pick a position automatically.
 *
 * Strategy: stage a twmrc into MEMFS via `stagedFiles`, then tell twm
 * to read it via `-f`. Same mechanism people use on real Linux
 * (`twm -f ~/.twmrc`).
 */

import type { Orchestrator } from '../worker/main-thread/orchestrator.js';
import type { ClientWorkerHandle } from '../worker/main-thread/client-proxy.js';

const TWMRC_PATH = '/em-x11.twmrc';

/* Mirrors third-party/twm/src/system.twmrc with two em-x11 specific
 * additions:
 *
 *   - `RandomPlacement`: see the file header for why interactive placement
 *     deadlocks our browser world.
 *
 *   - `OpaqueMove`: twm defaults to OpaqueMove=FALSE, which means a
 *     window drag (f.move) draws a "rubber band" XSegment outline on the
 *     root window using GXxor and only XMoveWindow's the real frame on
 *     ButtonRelease. We can't honour GXxor on Canvas 2D (a second draw
 *     can't undo the first; see native/src/drawing.c::gc_draw_disabled),
 *     so the entire drag would be visually invisible -- the window only
 *     jumps at release, with no feedback in between. OpaqueMove flips
 *     twm to live-XMoveWindow during the drag, which our MoveWindow path
 *     does render correctly. The slight cost is more frame redraws
 *     while dragging; in a browser demo that's fine.
 *
 * Kept inline (rather than as a separate asset) so the Vite build doesn't
 * have to learn about a new file type. */
const TWMRC = `
NoGrabServer
RestartPreviousState
DecorateTransients
RandomPlacement
OpaqueMove
TitleFont "-adobe-helvetica-bold-r-normal--*-120-*-*-*-*-*-*"
ResizeFont "-adobe-helvetica-bold-r-normal--*-120-*-*-*-*-*-*"
MenuFont "-adobe-helvetica-bold-r-normal--*-120-*-*-*-*-*-*"
IconFont "-adobe-helvetica-bold-r-normal--*-100-*-*-*-*-*-*"
IconManagerFont "-adobe-helvetica-bold-r-normal--*-100-*-*-*"

Color
{
    BorderColor "slategrey"
    DefaultBackground "rgb:2/a/9"
    DefaultForeground "gray85"
    TitleBackground "rgb:2/a/9"
    TitleForeground "gray85"
    MenuBackground "rgb:2/a/9"
    MenuForeground "gray85"
    MenuBorderColor "slategrey"
    MenuTitleBackground "gray70"
    MenuTitleForeground "rgb:2/a/9"
    IconBackground "rgb:2/a/9"
    IconForeground "gray85"
    IconBorderColor "gray85"
    IconManagerBackground "rgb:2/a/9"
    IconManagerForeground "gray85"
}

MoveDelta 3
Function "move-or-lower" { f.move f.deltastop f.lower }
Function "move-or-raise" { f.move f.deltastop f.raise }
Function "move-or-iconify" { f.move f.deltastop f.iconify }

Button1 = : root : f.menu "defops"

Button1 = m : window|icon : f.function "move-or-lower"
Button2 = m : window|icon : f.iconify
Button3 = m : window|icon : f.function "move-or-raise"

Button1 = : title : f.function "move-or-raise"
Button2 = : title : f.raiselower

Button1 = : icon : f.function "move-or-iconify"
Button2 = : icon : f.iconify

Button1 = : iconmgr : f.iconify
Button2 = : iconmgr : f.iconify

menu "defops"
{
"Twm"           f.title
"Iconify"       f.iconify
"Resize"        f.resize
"Move"          f.move
"Raise"         f.raise
"Lower"         f.lower
""              f.nop
"Focus"         f.focus
"Unfocus"       f.unfocus
"Show Iconmgr"  f.showiconmgr
"Hide Iconmgr"  f.hideiconmgr
""              f.nop
"Kill"          f.destroy
"Delete"        f.delete
""              f.nop
"Restart"       f.restart
"Exit"          f.quit
}
`;

export interface LaunchTwmOptions {
  /** Build artifact directory containing twm.js / twm.wasm. Defaults to
   *  /build/artifacts/twm which is what cmake produces in dev. */
  artifactBase?: string;
}

/** Spawn twm in its own Client Worker. Returns once twm has armed
 *  SubstructureRedirectMask on root, so the caller's next launchClient
 *  is guaranteed to route through twm's MapRequest intercept. Without
 *  that barrier, a racing client could map at root-local (0,0) before
 *  twm ever sees a MapRequest. */
export async function launchTwm(
  orch: Orchestrator,
  options: LaunchTwmOptions = {},
): Promise<ClientWorkerHandle> {
  const base = options.artifactBase ?? '/build/artifacts/twm';
  const handle = await orch.launchClient({
    glueUrl: `${base}/twm.js`,
    wasmUrl: `${base}/twm.wasm`,
    arguments: ['-f', TWMRC_PATH],
    stagedFiles: [{ path: TWMRC_PATH, contents: TWMRC.trimStart() }],
    name: 'emx11-twm',
  });
  await orch.waitForSubstructureRedirect(orch.getRootWindow());
  return handle;
}
