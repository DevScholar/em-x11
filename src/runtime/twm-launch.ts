/**
 * twm launcher — spawns twm via the public em.spawn API.
 *
 * The vendored twm in ignored-area/third-party/ is a verbatim X.Org tarball that gets
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
 * Strategy: stage a twmrc into emX11.fs at /em-x11.twmrc, then tell twm
 * to read it via `-f`. Same mechanism people use on real Linux.
 */

import type { EmX11 } from '../api/emx11.js';
import type { Process } from '../api/types.js';

const TWMRC_PATH = '/em-x11.twmrc';

/* Mirrors ignored-area/third-party/twm/src/system.twmrc with two em-x11 specific
 * additions; see prior revision history for the full justification.
 *   RandomPlacement   — interactive placement deadlocks our browser world
 *   OpaqueMove        — GXxor rubber-band can't be undone on Canvas 2D */
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
   *  /artifacts/twm which is what the build produces. */
  artifactBase?: string;
}

/** Spawn twm in single-thread mode. Returns once twm has armed
 *  SubstructureRedirectMask on root, so the caller's next
 *  emX11.child_process.spawn is guaranteed to route through twm's
 *  MapRequest intercept. Without that barrier, a racing client could
 *  map at root-local (0,0) before twm ever sees a MapRequest. */
export async function launchTwm(
  emX11: EmX11,
  options: LaunchTwmOptions = {},
): Promise<Process> {
  const base = options.artifactBase ?? '/artifacts/twm';
  emX11.fs.writeFileSync(TWMRC_PATH, TWMRC.trimStart());
  const p = emX11.child_process.spawn(`${base}/twm`, {
    thisProgram: 'twm',
    argv: ['-f', TWMRC_PATH],
  });
  await p.ready;
  await emX11.display.waitForSubstructureRedirect(emX11.display.rootWindowId);
  return p;
}
