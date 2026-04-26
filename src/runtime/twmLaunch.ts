/**
 * twm launcher with em-x11 specific config.
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
 * single-thread JS world the only client running while twm waits is
 * twm itself (xeyes is suspended on its redirected XMapWindow), so the
 * wait would never end. RandomPlacement makes twm pick a position
 * automatically and proceed straight to XCreateWindow(frame) +
 * XReparentWindow.
 *
 * Strategy: stage a twmrc into MEMFS via the Module's preRun hook, then
 * tell twm to read it via `-f`. Same mechanism people use on real Linux
 * (`twm -f ~/.twmrc`).
 */

import type { Host } from './host.js';
import type { EmscriptenModule } from '../types/emscripten.js';

const TWMRC_PATH = '/em-x11.twmrc';

/* Mirrors third-party/twm/src/system.twmrc with `RandomPlacement` added.
 * Kept inline (rather than as a separate asset) so the Vite build doesn't
 * have to learn about a new file type. */
const TWMRC = `
NoGrabServer
RestartPreviousState
DecorateTransients
RandomPlacement
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

export async function launchTwm(
  host: Host,
  options: LaunchTwmOptions = {},
): Promise<{ connId: number; module: EmscriptenModule }> {
  const base = options.artifactBase ?? '/build/artifacts/twm';
  return host.launchClient({
    glueUrl: `${base}/twm.js`,
    wasmUrl: `${base}/twm.wasm`,
    arguments: ['-f', TWMRC_PATH],
    preRun: [
      (mod) => {
        const fs = mod.FS;
        if (!fs) {
          throw new Error(
            'em-x11: twm wasm has no FS — was it built with the default ' +
              'Emscripten filesystem support?',
          );
        }
        fs.writeFile(TWMRC_PATH, TWMRC.trimStart());
      },
    ],
  });
}
