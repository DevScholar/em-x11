# Using twm in the em-x11 demo

If you opened the `twm-session` demo, right-clicked or left-clicked the
desktop, and felt like the menu was broken — it isn't. twm just predates
the menu conventions you're used to. This page walks through how to
actually drive it.

twm is **the Tab Window Manager**, the original ICCCM-era window manager
that shipped with X11R4 in 1989. Its UI was designed before the modern
"click to open menu, click to choose item" idiom existed. If you are
under ~40 you have probably never used a menu like this before. The
muscle memory you have for Windows 95 / macOS / GNOME / KDE menus will
actively work against you here.

## The press-drag-release menu (Windows 1.0 style, not Windows 3.0)

Modern menus work like this:

1. Click to open
2. Move (no button held) to the item you want
3. Click to confirm

twm menus work like this:

1. **Press the mouse button and keep holding it** — the menu pops up
2. With the button **still held**, drag down to the item you want
3. **Release** the button on the item — that's how you "click" it

If you just press-and-release on the desktop the way you would in any
other modern environment, the menu pops up on the press and immediately
closes on the release, so it looks like a flicker that does nothing.

Concretely, in the `twm-session` demo:

- **Press and hold Button1** (left mouse button) on any empty area of
  the desktop. The "TWM Windows" menu (titled `Twm`) appears.
- **Keep holding**, drag down to the item you want — the highlight
  follows the pointer.
- **Release** on the item.

This is the same model the original Macintosh menu bar used (1984): hold
the button to open, drag, release to commit. It survives in twm because
twm's UI vocabulary was frozen around the same era.

## If you're coming from Windows: translation table

The mental remap that hurts the most:

| What you want to do (Windows habit)            | The twm way                                                                                  |
| ---------------------------------------------- | -------------------------------------------------------------------------------------------- |
| Click Start menu to launch an app              | No equivalent. The demo's apps are pre-spawned by the page; the root menu is WM-ops only.    |
| Click a taskbar entry to switch windows        | Click a row in the icon manager (Button1 press-drag-release on the row)                      |
| Click `×` on the title bar to close            | Root menu → **Delete** → click the target window                                             |
| Click `−` to minimise                          | **Meta + Button2** on the window body — or root menu → **Iconify** → click target            |
| Click `□` to maximise                          | Not bound in the default config. Real twm has `f.fullzoom`, but default `system.twmrc` doesn't wire it |
| Alt+Tab to switch                              | Not bound by default. (Real twm can be configured with `f.warpring` on a key.)               |
| Win+D to show desktop                          | None. Iconify each window manually if you want to clear the screen.                          |
| Right-click empty desktop                      | **Button1** press-drag-release on desktop (twm rebinds; right-click is unbound by default)   |
| Right-click title bar for window menu          | **Button2** press-drag-release on title bar — toggles raise/lower (no menu)                  |
| Drag title bar to move                         | Same — Button1 press-drag-release on title bar                                               |
| Drag corner to resize                          | Root menu → **Resize** → click target → drag near an edge with a button held                 |
| Snap to side (Aero snap, FancyZones)           | None.                                                                                        |
| Multiple workspaces / virtual desktops         | None in default twm.                                                                         |
| Search by typing in Start                      | None. There is no global keyboard input target by default.                                   |
| Type immediately after clicking a button       | Click the application's body once first to give it keyboard focus.                           |

If a row says "None" — really none. There's no hidden hotkey we forgot
to mention. twm is small.

## Two kinds of menu items

After release, items split into two groups based on whether twm needs to
know *which window* to operate on.

### Items that execute immediately

These run as soon as you release the button on them, and the menu
disappears:

- **Restart** — restarts twm itself
- **Exit** — quits twm
- **Show Iconmgr** / **Hide Iconmgr** — toggles twm's icon manager panel
- **Unfocus** — drops keyboard focus back to the root window
- **Xterm** — would launch xterm (no-op in the demo, no xterm binary)
- **Kill** with `f.delete` semantics where the focused window is implied

### Items that wait for you to pick a target window

These need a window to act on. When you release on them, the menu
**stays open** and the cursor changes shape to a small "select target"
cursor. This is twm telling you: "I have the action queued; now click
the window you want me to apply it to."

- **Iconify** — minimise a window
- **Resize** — interactive resize
- **Move** — interactive move
- **Raise** — bring a window to the front
- **Lower** — send a window to the back
- **Focus** — give a window keyboard focus
- **Kill** — `XKillClient` (force-disconnect)
- **Delete** — polite WM_DELETE_WINDOW

The follow-up sequence is:

1. Release on, say, `Iconify`. Cursor changes; menu stays.
2. **Click** any managed window (xeyes, xcalc — the ones with a twm
   border around them). That window gets iconified, the menu disappears.

Or to abort:

3. **Click the desktop** instead. twm beeps, drops the queued action,
   the menu disappears.

This two-step interaction is twm's `DeferExecution` mechanism, not an
em-x11 quirk. Real twm on real X behaves identically.

## Why menus stay open after a deferred selection

You may expect the menu to close as soon as you release on `Iconify` and
*then* let you pick the target. twm does it the other way around: the
menu is your reminder of what action is queued. It closes only after you
either commit (click a window) or cancel (click the desktop).

If this feels backwards, that's because by 1989 standards "interactive
modes that visibly persist until you commit" was the norm — think
modal-by-default, not modeless-by-default.

## Anatomy of a managed window

When twm "manages" a window (xeyes, xcalc), it wraps it in extra
decoration:

```
┌─────────────────────────────────┐  ← frame (the outer border)
│ ▢ window title             ▢    │  ← title bar
├─────────────────────────────────┤
│                                 │
│    application content          │  ← the original client window
│    (xeyes face, xcalc keypad)   │     ("body" or "client area")
│                                 │
└─────────────────────────────────┘
```

The **title bar**, **frame**, and **client area** are three different
things to twm — most bindings care which one you press on. The
`system.twmrc` table at the bottom of this page is keyed off these.

twm has no resize handle in the corner, no minimise/maximise/close
buttons in the title bar (by default), and no taskbar. Every operation
goes through either the root menu, the title bar, or a Meta+click on
the body. There is also an **icon manager** — see below.

## Moving a window

Two ways:

- **Press-drag-release Button1 on the title bar.** The window follows
  the pointer. Release where you want it.
- **Hold Meta (Alt) + Button1** anywhere on the window body, then drag.
  Same effect — useful when the title bar is offscreen.

Both bindings actually run `f.function "move-or-raise"` (or
`move-or-lower`), which is "move if you drag more than `MoveDelta`
(3 pixels), otherwise just raise/lower". So a tiny twitch counts as a
click-to-raise; a real drag counts as a move. This is by design — twm
folds two operations onto one button to save buttons for menus.

If you press but don't move, you raise (Button1) or lower (with the
`m`+Button1 binding). The MoveDelta dead-zone is what makes this feel
predictable.

## Raising and lowering

- **Press-drag-release Button1 on the title bar without moving** raises
  the window (the `move-or-raise` no-move case).
- **Press Button2 on the title bar** runs `f.raiselower` — toggles
  between raised and lowered.
- From the root menu, **Raise** and **Lower** require selecting a
  target window after release (see the deferred-action flow above).

Browsers don't always give you a Button2 (middle button). See the
browser gotchas section.

## Resizing

twm has no corner grip. Resizing is initiated explicitly:

1. Open the root menu (press-drag-release Button1 on desktop).
2. Drag down to **Resize**, release.
3. Cursor changes; menu stays open.
4. Click the window you want to resize.
5. Move the pointer toward the edge or corner you want to drag — twm
   shows a rubber-band outline. The edge/corner nearest the pointer
   becomes "active".
6. **Press and hold a button**, drag to size, release.

Yes, that's six steps for what modern WMs do with a corner drag. This
is twm. The shortcut: `Meta + Button3` drag on the window body runs
`move-or-raise` — there is no built-in keyboard or modifier shortcut
for resize in the default `system.twmrc`. If you want one you'd have to
edit twmrc and rebuild — out of scope here.

## Iconifying and the icon manager

twm's "minimise" is **Iconify**. There are two equivalent ways:

- Root menu → **Iconify** → click the target window.
- **Meta + Button2** on the window body (the default `f.iconify`
  binding for `window|icon`).

When iconified, the window disappears from its current screen position
and an entry appears in the **icon manager** — twm's equivalent of a
taskbar. The icon manager is a small panel listing all managed windows
(iconified or not). Iconified entries appear with a different state
indicator.

To bring a window back from icon state:

- **Press-drag-release Button1 on its row in the icon manager.**

To toggle the icon manager visibility:

- Root menu → **Show Iconmgr** / **Hide Iconmgr** (these execute
  immediately, no target selection).

If the icon manager is in the way, you can move it like any other
window (Button1 on its title bar).

## Focus model

The default twm config is **click-to-focus** for input, but
**focus-follows-pointer** for highlights — i.e. the title bar
highlights when the pointer crosses, but keystrokes only go to a
window after you actually click it. xcalc's keyboard input works only
after you click on its body.

The root menu's **Focus** item lets you explicitly set focus to a
selected window; **Unfocus** drops focus back to the root.

## Closing windows

Two routes via the root menu (both deferred, so menu stays open until
you pick a target window):

- **Delete** — sends WM_DELETE_WINDOW (the polite "please quit"
  ICCCM message). The application gets to do cleanup.
- **Kill** — `XKillClient`, the X-protocol equivalent of SIGKILL.
  Severs the connection. Use only if Delete doesn't work.

xeyes / xcalc in this demo do honour WM_DELETE_WINDOW where applicable,
but note that the WASM exit path in the em-x11 demos is incomplete —
killing the client doesn't cleanly tear down the wasm process, and the
window can leave a frame behind. This is a known em-x11 limitation, not
a twm bug.

## Browser-specific gotchas

A few things real X has that the browser doesn't:

- **Middle button (Button2).** Many laptop trackpads don't expose one.
  If you have a real three-button mouse it just works. On a trackpad,
  some browsers map two-finger-click or Cmd+click to middle — varies by
  OS and browser. There is no software workaround in twm.
- **Meta / Alt key.** Browsers send Alt as the X "Mod1" mask, which is
  what twm's `m` modifier resolves to. On macOS the Option key is
  Alt-equivalent at the browser level. On Linux/Windows it's just Alt.
- **Right-click context menu.** em-x11 calls `preventDefault()` on the
  canvas's `contextmenu` event so right-click reaches twm instead of
  popping the browser's own menu. (Outside the canvas, right-click
  still does what your browser usually does.)
- **Keyboard focus into the canvas.** Click the canvas at least once
  per page load before keystrokes will reach any X client, or the
  browser routes them to the address bar / wherever its own focus is.
- **Drag off the canvas.** If you start a drag (move, resize) and
  release outside the canvas, the release still reaches twm — em-x11
  listens on `window` for `mouseup` specifically so drags terminate
  cleanly.

## Five-minute hands-on tour

Open the `twm-session` demo page. You should see xeyes (a pair of eyes
that follow the cursor) and xcalc (a calculator) somewhere on a slate
background. Do these seven things in order — at the end you'll have
exercised every essential interaction:

1. **Move xcalc.** Press Button1 on xcalc's title bar, hold, drag a
   few inches, release. The window follows.
2. **Just-raise xeyes.** Click Button1 on xeyes' title bar without
   dragging (a press shorter than 3 pixels of motion). xeyes comes to
   the front. This is the "no-drag" fallthrough of `move-or-raise`.
3. **Iconify xcalc the keyboard-shortcut way.** Hold Alt, click
   Button2 on xcalc's body (the keypad area, not the title bar).
   xcalc disappears; an entry for it appears in the icon manager.
4. **Restore xcalc from the icon manager.** Find the icon manager
   panel (usually top-right). Press Button1 on the row that says
   `xcalc`, hold, drag a tiny bit, release. xcalc comes back.
5. **Open the root menu.** Press Button1 on empty desktop and **hold**.
   The "Twm" menu appears. With the button still held, drag down the
   menu — items highlight as you cross them.
6. **Iconify via the menu (the deferred way).** With the menu open,
   drag onto **Iconify**, release. Cursor changes to a small reticle;
   the menu stays open. Now click any managed window — that window
   gets iconified. Or click empty desktop to cancel.
7. **Resize xeyes.** Open the root menu, drag to **Resize**, release.
   Click on xeyes. Now move (don't press) the pointer toward one
   corner of xeyes — twm shows a rubber-band outline tracking your
   pointer relative to the window's edges. Press Button1, drag the
   outline to the size you want, release.

After step 7 you have done: move, raise, iconify (two ways), icon-
manager interaction, root menu navigation, deferred actions, and
resize. That's basically all of twm's mouse vocabulary.

## What twm doesn't have

Setting expectations explicitly so you stop looking:

- **No application launcher.** The root menu is for window-manager
  operations, not for starting programs. The `Xterm` item in the menu
  would shell out to `exec xterm &` on a real Unix box, but our demo
  doesn't ship an xterm wasm, so it's a no-op.
- **No virtual desktops / workspaces.** There is one screen.
- **No window snapping**, no edge-magnetism, no Aero/FancyZones-style
  layouts.
- **No keyboard window switching by default.** No Alt+Tab. Real twm
  can be configured to bind `f.warpring` to a key — the default
  `system.twmrc` doesn't.
- **No title-bar buttons.** Default twm leaves the title bar empty
  except for the window name. Some twmrc dialects add an iconify
  button via `LeftTitleButton "menu12.xbm"=f.iconify`; ours doesn't.
- **No system tray, clock, notifications, or status indicators.**
- **No drag-and-drop between windows.** X has DnD protocols (XDND,
  Motif DnD) but the demo apps don't implement any.
- **No window animations.** Windows reposition instantly.
- **No "always on top" toggle.** Stacking is purely by raise/lower.
- **No close button on the title bar** — see Closing Windows above.
- **No "Show Desktop" gesture or hotkey.**

If you find yourself thinking "where's the X / minimise / button bar
on the title?" — there isn't one. That space is just the window
name. The original twm authors expected you to use the menus.

## I'm stuck — quick recovery

Common dead-ends and how to get out:

- **The cursor changed shape and clicks aren't doing what I expect.**
  You released on a deferred-action menu item (Iconify / Move / Resize
  / Raise / Lower / Focus / Kill / Delete) and twm is now waiting for
  you to pick a target window. **Click the empty desktop** to cancel —
  twm beeps and the cursor goes back to normal.
- **The root menu won't go away.** Same situation as above. Click the
  empty desktop to cancel, or click a window to commit the queued
  action.
- **I picked Resize, clicked the window, and now nothing's happening.**
  Move the pointer (without pressing anything) toward an edge or
  corner of the window — the rubber-band outline highlights the edge
  it's about to drag. *Then* press Button1 and drag. The press has to
  come after you're near the edge; just hovering shows what's about
  to be active.
- **xcalc / xeyes won't take my keystrokes.** Click on its body once
  to give it focus. Until you do, keystrokes go nowhere (or to the
  browser's address bar if you haven't clicked the canvas yet).
- **The icon manager is in my way.** It's a regular window — drag it
  with Button1 on its title bar. Or hide it via the root menu →
  **Hide Iconmgr**.
- **A window vanished and I can't find it.** Probably iconified.
  Look at the icon manager. If the icon manager is hidden, root menu
  → **Show Iconmgr**.
- **Both apps disappeared.** You probably iconified both. Same fix.
- **I dragged a window past the edge and now I can't grab its title
  bar.** Open the root menu → **Move**, click the (mostly off-screen)
  window, drag it back. The default config allows offscreen
  positioning; real twm has a `DontMoveOff` directive that prevents it,
  but ours uses defaults.
- **My right-click pops the browser context menu instead of doing
  twm stuff.** That's expected — em-x11 only suppresses the
  browser context menu *over the canvas*. Outside it, the browser
  still owns right-click.
- **I broke something visually — twm's frame got out of sync with
  the window.** Reload the page. The demo doesn't persist any state
  between page loads, so a refresh is always safe.

## Cheat sheet

Memorise this and you're done:

```
ROOT MENU         press-drag-release Button1 on empty desktop
                  → Restart / Exit / Show Iconmgr / Hide Iconmgr / Unfocus
                    execute on release; menu closes
                  → Iconify / Resize / Move / Raise / Lower / Focus /
                    Kill / Delete are deferred; menu stays, cursor
                    changes, then click target window (or desktop to
                    cancel)

WINDOW            Button1 on title bar:  press-drag-release moves;
                                         press-release-without-drag raises
                  Button2 on title bar:  click toggles raise/lower
                  Meta+Button1 on body:  drag to move, click to lower
                  Meta+Button2 on body:  click to iconify
                  Meta+Button3 on body:  drag to move, click to raise

ICON MANAGER      Button1 press-drag-release on a row:
                                         restore that window from icon

CANCEL ANYTHING   click empty desktop

KEYBOARD          click a window's body first to focus it; keys flow
                  to whoever has focus

NO HOTKEYS        no Alt+Tab, no Win key, no menu mnemonics — really
```

`Meta` = Alt on Windows/Linux, Option on macOS.

## Default key bindings reference

The default `system.twmrc` (compiled into our `twm.wasm`) binds:

```
Button1 = : root : f.menu "defops"
Button1 = : title : f.function "move-or-raise"
Button2 = : title : f.raiselower
Button1 = m : window|icon : f.function "move-or-lower"
Button2 = m : window|icon : f.iconify
Button3 = m : window|icon : f.function "move-or-raise"
```

Translated:

| Action                       | How                                              |
| ---------------------------- | ------------------------------------------------ |
| Open root menu               | Press-drag-release Button1 on empty desktop      |
| Move/raise a window          | Press-drag-release Button1 on its title bar      |
| Raise/lower a window         | Press-drag-release Button2 on its title bar      |
| Move/lower (alt grip)        | Hold Meta + Button1 anywhere on the window       |
| Iconify (alt grip)           | Hold Meta + Button2 anywhere on the window       |
| Move/raise (alt grip)        | Hold Meta + Button3 anywhere on the window       |

`m` means the **Meta** modifier. In a browser we map this to the Alt key
(left or right). On macOS browsers, Option is what the OS sends as Alt
to the page.

## Key takeaways

- **Press, drag, release.** Don't click-then-click. The first release on
  the menu item is your "select."
- **Items that need a target window keep the menu open** and change the
  cursor. Click a managed window to apply, click the desktop to cancel.
- This is **how twm has always worked**. None of it is a browser-port
  bug — porting twm to em-x11 specifically preserved the original
  semantics rather than retrofitting modern menu conventions.

If you want a menu environment that behaves like 2026 software, twm
isn't it. It's preserved here as a faithful port for the same reason
people preserve 1980s synthesizers: the historical interface is the
point.
