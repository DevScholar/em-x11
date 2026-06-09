/**
 * XIM text-input plumbing.
 *
 * X11's XIM model assumes the application window is a real DOM-editable
 * surface so the OS IME can attach its candidate window to a focused
 * input. A <canvas> isn't editable, so em-x11 borrows a 1px transparent
 * <textarea> as the "real" focused element. Whatever the user types,
 * composes, or pastes there gets forwarded as synthetic X11 KeyPress
 * events into the wasm side.
 *
 * The textarea is created lazily on first XSetICFocus (the wasm app has
 * a text-input widget that wants IME) and removed from the DOM on
 * XUnsetICFocus.  No permanent hidden element — only present while
 * the user is actually typing.
 *
 * Two coordinate systems involved:
 *
 *   - X11 window-local coords (XSetICValues(XNSpotLocation): caret
 *     position relative to the focused window's top-left).
 *   - Viewport CSS pixels (textarea.style.left/top): where the OS IME
 *     pulls candidate windows from.
 *
 * Translation chain: window-local → root-relative (Host.getWindowAbsOrigin)
 * → canvas viewport pixels (canvas.getBoundingClientRect()). The runtime
 * canvas is 1:1 backed (no DPR scaling), so root pixels equal canvas-CSS
 * pixels for our purposes.
 *
 * Two deployments:
 *
 *   Direct (tcldide in main thread, em-x11 in main thread): everything
 *   runs in one realm. TextInputOverlay creates the textarea itself,
 *   listens for composition, and feeds the text back via host.devices.
 *
 *   Remote (pyodide-tk: em-x11 + wasm in a Worker, DOM only on main):
 *   the host has no `document`. Construct it with a TextInputRemote
 *   that ships setFocus / setSpot / clearFocus commands across the
 *   thread boundary; the main thread owns the textarea (via
 *   createDomTextInputBridge) and posts composition results back. The
 *   main thread's bridge wires those results into emX11.display.inject.textKey.
 */

import type { Host } from './index.js';

/** Pluggable transport for hosts that don't own a DOM (worker-mode
 *  pyodide-tk). The host calls these as if it owned a textarea; a
 *  DOM-side counterpart applies the commands to a real textarea and
 *  posts text back via emX11.display.inject.textKey. */
export interface TextInputRemote {
  setFocus(window: number): void;
  clearFocus(): void;
  setSpot(window: number, x: number, y: number): void;
  /** Translate window-local caret pixels (X11 px) into root-relative
   *  pixels using the host's window tree. The host computes this via
   *  Host.getWindowAbsOrigin(window) but exposes the absolute origin to
   *  the remote so the main side doesn't need a window-tree shadow. */
  positionHint(absX: number, absY: number): void;
}

export class TextInputOverlay {
  /** The textarea exists only while a widget has XIM focus. Created on
   *  first setFocus after a clearFocus, removed from DOM on clearFocus. */
  private el: HTMLTextAreaElement | null = null;
  private focusedWindow: number | null = null;
  /** Most recent caret spot (window-local, X11 px). null until the
   *  client calls XSetICValues(XNSpotLocation) at least once. */
  private spot: { window: number; x: number; y: number } | null = null;
  private remote: TextInputRemote | null = null;
  /** Window/scroll listeners attached in direct mode, kept so dispose()
   *  can detach them when the owning Host is torn down. Without this,
   *  the closures pin `this` (and thus the previous Host) past teardown. */
  private moveHandler: (() => void) | null = null;

  /** Public accessor so InputBridge can recognise the overlay as a
   *  legitimate focus surface (document.activeElement === overlay
   *  still counts as "focused on the X canvas" for keyboard routing).
   *  Returns null when no widget has IME focus or in remote mode. */
  get element(): HTMLTextAreaElement | null {
    return this.el;
  }

  constructor(private readonly host: Host) {
    if (host.canvas.headless || typeof document === 'undefined') {
      return;
    }
    /* Reposition on viewport reshape so candidate window stays anchored.
     * applyPosition() early-returns when el or focusedWindow is null. */
    const onMove = (): void => this.applyPosition();
    this.moveHandler = onMove;
    window.addEventListener('resize', onMove);
    window.addEventListener('scroll', onMove, true);
  }

  /** Tear down DOM state owned by this overlay: detach the resize/scroll
   *  listeners and remove the hidden textarea from the document if it
   *  happens to be present. Safe to call in remote/headless mode.
   *  Idempotent. */
  dispose(): void {
    if (this.moveHandler) {
      window.removeEventListener('resize', this.moveHandler);
      window.removeEventListener('scroll', this.moveHandler, true);
      this.moveHandler = null;
    }
    this.removeTextarea();
  }

  /** Remote-mode hook. When set, setFocus/setSpot/clearFocus forward
   *  their commands across the wire INSTEAD of touching a local
   *  textarea (there isn't one in worker mode anyway). The host still
   *  computes window-tree origin → absolute pixel translations so the
   *  remote receives ready-to-position screen coordinates. */
  setRemote(remote: TextInputRemote | null): void {
    this.remote = remote;
  }

  /* -- Bridge entry points (called from Host facade) ---------------------- */

  setFocus(window: number): void {
    const target = window || null;
    /* Dedup: re-focusing the textarea on the same window resets the
     * OS IME's per-element state (Chinese mode, candidate window) to
     * default English on Windows. Tk normally only fires XSetICFocus
     * on real focus transitions, but guard here so any redundant call
     * doesn't blow away IME state mid-session. */
    if (target === this.focusedWindow && target !== null) return;
    this.focusedWindow = target;
    if (this.remote) {
      if (!this.focusedWindow) this.remote.clearFocus();
      else {
        this.remote.setFocus(this.focusedWindow);
        this.notifyRemoteSpot();
      }
      return;
    }
    if (!this.focusedWindow) {
      this.removeTextarea();
      return;
    }
    this.ensureTextarea();
    this.applyPosition();
    this.el!.value = '';
    /* preventScroll: the page must not jump because we moved focus to
     * a 1px element off in the corner of the viewport. */
    try {
      (this.el as HTMLTextAreaElement & {
        focus(opts: { preventScroll: boolean }): void;
      }).focus({ preventScroll: true });
    } catch {
      this.el!.focus();
    }
  }

  clearFocus(): void {
    this.focusedWindow = null;
    if (this.remote) {
      this.remote.clearFocus();
      return;
    }
    this.removeTextarea();
  }

  /** Caret moved inside the focused widget. Tk fires this on every
   *  cursor motion in entries / texts. We ignore spots from windows
   *  that aren't the current focus -- Tk pre-sets XNSpotLocation on
   *  every entry whether it's focused or not. */
  setSpot(window: number, x: number, y: number): void {
    this.spot = { window, x, y };
    if (window !== this.focusedWindow) return;
    if (this.remote) {
      this.remote.setSpot(window, x, y);
      this.notifyRemoteSpot();
      return;
    }
    this.applyPosition();
  }

  /* -- Position computation ---------------------------------------------- */

  private applyPosition(): void {
    if (!this.el || this.focusedWindow === null) return;
    const abs = this.absoluteCaret();
    if (!abs) return;
    const canvasEl = this.host.canvas.element;
    if (!canvasEl || !(canvasEl instanceof HTMLCanvasElement)) return;
    const rect = canvasEl.getBoundingClientRect();
    const dprX = rect.width / this.host.canvas.cssWidth;
    const dprY = rect.height / this.host.canvas.cssHeight;
    const vx = rect.left + abs.x * dprX;
    const vy = rect.top + abs.y * dprY;
    this.el.style.left = vx + 'px';
    this.el.style.top = vy + 'px';
  }

  private notifyRemoteSpot(): void {
    if (!this.remote) return;
    const abs = this.absoluteCaret();
    if (!abs) return;
    this.remote.positionHint(abs.x, abs.y);
  }

  /** Window-local spot → root-relative pixels. Returns null if no focus
   *  or the window has no origin (rare race between setFocus and the
   *  first redraw that materialises the window's tree node). */
  private absoluteCaret(): { x: number; y: number } | null {
    if (this.focusedWindow === null) return null;
    const winId = this.focusedWindow;
    const origin = this.host.getWindowAbsOrigin(winId);
    if (!origin) return null;
    const spotX = this.spot && this.spot.window === winId ? this.spot.x : 0;
    const spotY = this.spot && this.spot.window === winId ? this.spot.y : 0;
    return { x: origin.ax + spotX, y: origin.ay + spotY };
  }

  /* -- Lazy textarea helpers --------------------------------------------- */

  /** Create the textarea and inject it into the DOM. No-op if already
   *  present (consecutive setFocus calls on the same or different window
   *  without an intervening clearFocus). */
  private ensureTextarea(): void {
    if (this.el) return;
    this.el = createHiddenTextarea();
    document.body.appendChild(this.el);
    attachCompositionListeners(this.el, (text) => {
      if (this.focusedWindow !== null) this.host.devices.pushTextKey(text);
    });
  }

  /** Blur and remove the textarea from the DOM so no hidden element
   *  lingers while the user is not typing into an X11 text widget. */
  private removeTextarea(): void {
    if (!this.el) return;
    this.el.blur();
    this.el.remove();
    this.el = null;
  }
}

/* -- DOM bridge (used both by main-thread Host and main-thread side of
 *    a worker-hosted Host) ------------------------------------------------ */

function createHiddenTextarea(): HTMLTextAreaElement {
  const ta = document.createElement('textarea');
  ta.setAttribute('autocapitalize', 'off');
  ta.setAttribute('autocomplete', 'off');
  ta.setAttribute('autocorrect', 'off');
  ta.setAttribute('spellcheck', 'false');
  /* Do NOT set aria-hidden=true. Chromium logs "Blocked aria-hidden on
   * a focused element" and -- worse -- disables IME on the element
   * after the first composition session as a defensive measure (it
   * treats aria-hidden focused inputs as "not a real input"). The
   * symptom is "first Chinese 词 commits, every subsequent press falls
   * through as ASCII." Visual hiding via opacity:0 + tabindex is
   * enough. Screen readers won't read empty content anyway. */
  ta.tabIndex = -1;
  ta.style.position = 'fixed';
  ta.style.left = '-9999px';
  ta.style.top = '0px';
  ta.style.width = '1px';
  ta.style.height = '1em';
  ta.style.padding = '0';
  ta.style.margin = '0';
  ta.style.border = '0';
  ta.style.outline = 'none';
  ta.style.opacity = '0';
  /* Hide the OS system caret -- the X widget renders its own. */
  (ta.style as CSSStyleDeclaration & { caretColor?: string }).caretColor = 'transparent';
  /* The textarea itself never sees the mouse: clicks on the canvas
   * route normally. The OS IME doesn't need pointer events to anchor
   * its candidate window -- only DOM focus + caret rect. */
  ta.style.pointerEvents = 'none';
  ta.style.background = 'transparent';
  ta.style.resize = 'none';
  ta.style.zIndex = '2147483647';
  return ta;
}

function attachCompositionListeners(
  ta: HTMLTextAreaElement,
  emit: (text: string) => void,
): void {
  /* Composition strategy borrowed from xterm.js's CompositionHelper:
   *
   *   - compositionend.data is UNRELIABLE on Chromium (Korean ending
   *     consonants in particular are wrong). xterm.js reads
   *     textarea.value instead, after a setTimeout(0) so the native
   *     write has actually landed. We do the same.
   *   - We let the textarea actually receive the composed text (don't
   *     preventDefault on keydown for Process keys, don't preventDefault
   *     on beforeinput for compose insertions). After we've copied the
   *     bytes out, we wipe the textarea so it doesn't accumulate.
   *   - For ASCII keystrokes we want neither path to inject text:
   *     pushKeyDown in devices.ts already carries the byte from
   *     KeyboardEvent.key. So beforeinput's `insertText` is a no-op
   *     here, and we preventDefault it to keep the textarea empty.
   */
  let composing = false;

  ta.addEventListener('compositionstart', () => {
    composing = true;
  });
  ta.addEventListener('compositionend', (e: CompositionEvent) => {
    composing = false;
    const evData = e.data ?? '';
    /* Always defer: even when evData is non-empty, we still want to
     * wipe ta.value AFTER the browser finishes its compositionend
     * bookkeeping, otherwise the next compositionstart on Chromium
     * Windows fires but compositionupdate never follows -- the IME
     * session goes silent until the user clicks away. */
    setTimeout(() => {
      let text = evData;
      if (!text && ta.value) text = ta.value;
      ta.value = '';
      try { ta.setSelectionRange(0, 0); } catch { /* ignore */ }
      if (text) emit(text);
    }, 0);
  });

  /* beforeinput is our path for paste, autocomplete substitution, and
   * mobile-keyboard input -- anything that bypasses the keydown +
   * KeyboardEvent.key path. We preventDefault to keep the textarea
   * empty, then forward the data into Tk via a synthetic KeyPress.
   *
   * IME-related insertions (`insertCompositionText` /
   * `insertFromComposition`) are handled by the compositionend listener
   * above, so don't dispatch them twice here. */
  ta.addEventListener('beforeinput', (e: InputEvent) => {
    if (composing || e.inputType === 'insertCompositionText' ||
        e.inputType === 'insertFromComposition') {
      return;     /* let the textarea receive it; compositionend reads it back */
    }
    e.preventDefault();
    const t = e.inputType;
    if (t === 'insertText' || t === 'insertFromPaste') {
      const data = e.data ?? '';
      if (data) emit(data);
    }
  });

  /* keydown on the textarea: we DON'T preventDefault during composition
   * (Chromium aborts IME initiation if a Process keydown is canceled).
   * For non-composing keys, the window-level keydown handler in
   * devices.ts owns the keysym + text dispatch and preventDefaults the
   * raw keystroke; we don't need to do anything here. */
  ta.addEventListener('keydown', () => { /* no-op */ });

  /* After IME or paste, the textarea may hold composed text. The
   * compositionend handler wipes it; keep this as a safety net for
   * cases (mobile keyboards, autocorrect) where input fires without a
   * compositionend pair. */
  ta.addEventListener('input', () => {
    if (!composing) ta.value = '';
  });
}

/* -- Public surface: main-thread DOM bridge for worker-hosted em-x11 ---- */

export interface DomTextInputBridgeOptions {
  /** The same canvas the worker's em-x11 is painting into. Used to
   *  translate root-relative caret pixels back into viewport CSS
   *  pixels for textarea positioning. */
  canvas: HTMLCanvasElement;
  /** Logical (CSS) X-screen width passed to createEmX11. Used to
   *  derive the canvas-element-to-X-pixel scale. Defaults to
   *  canvas.width (1:1 backing). */
  rootWidth?: number;
  /** Logical (CSS) X-screen height. Defaults to canvas.height. */
  rootHeight?: number;
  /** Receive composed / pasted text. The caller routes this into the
   *  worker, which calls emX11.display.inject.textKey. */
  onText: (text: string) => void;
}

/** Main-thread companion to a worker-hosted TextInputOverlay. Owns a
 *  hidden <textarea>, applies focus/spot commands the worker sends
 *  over, and forwards composed text back via `onText`.
 *
 *  Returns a controller the caller drives from worker messages. */
export interface DomTextInputBridge {
  setFocus(window: number): void;
  clearFocus(): void;
  /** Apply a positionHint from the worker (root-relative X pixels).
   *  Translates into viewport CSS pixels and moves the textarea so the
   *  OS IME candidate window anchors there. */
  applyPosition(absX: number, absY: number): void;
  /** Tear down: blur, remove textarea from DOM, drop listeners. */
  destroy(): void;
}

export function createDomTextInputBridge(
  opts: DomTextInputBridgeOptions,
): DomTextInputBridge {
  if (typeof document === 'undefined') {
    throw new Error('em-x11: createDomTextInputBridge requires a DOM');
  }
  /** The textarea exists only while a widget has XIM focus — created on
   *  setFocus, removed from DOM on clearFocus. */
  let ta: HTMLTextAreaElement | null = null;
  let focused = false;
  let focusedWindow: number | null = null;
  let lastAbs: { x: number; y: number } | null = null;

  const ensureTa = (): HTMLTextAreaElement => {
    if (ta) return ta;
    ta = createHiddenTextarea();
    document.body.appendChild(ta);
    attachCompositionListeners(ta, opts.onText);
    return ta;
  };

  const removeTa = (): void => {
    if (!ta) return;
    ta.blur();
    ta.remove();
    ta = null;
  };

  const reposition = (): void => {
    if (!focused || !lastAbs || !ta) return;
    const rect = opts.canvas.getBoundingClientRect();
    const rootW = opts.rootWidth ?? opts.canvas.width;
    const rootH = opts.rootHeight ?? opts.canvas.height;
    const dprX = rect.width / rootW;
    const dprY = rect.height / rootH;
    ta.style.left = (rect.left + lastAbs.x * dprX) + 'px';
    ta.style.top  = (rect.top  + lastAbs.y * dprY) + 'px';
  };

  const onMove = (): void => reposition();
  window.addEventListener('resize', onMove);
  window.addEventListener('scroll', onMove, true);

  return {
    setFocus(_window: number): void {
      /* Dedup re-focus on the same window: see TextInputOverlay.setFocus
       * for why this matters (Windows IME per-element state reset). */
      if (focused && focusedWindow === _window) return;
      focused = true;
      focusedWindow = _window;
      ensureTa();
      reposition();
      ta!.value = '';
      try {
        (ta as HTMLTextAreaElement & {
          focus(opts: { preventScroll: boolean }): void;
        }).focus({ preventScroll: true });
      } catch {
        ta!.focus();
      }
    },
    clearFocus(): void {
      focused = false;
      focusedWindow = null;
      removeTa();
    },
    applyPosition(absX: number, absY: number): void {
      lastAbs = { x: absX, y: absY };
      reposition();
    },
    destroy(): void {
      window.removeEventListener('resize', onMove);
      window.removeEventListener('scroll', onMove, true);
      removeTa();
    },
  };
}
