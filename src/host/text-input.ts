/**
 * Hidden <textarea> overlay -- the OS IME's anchor point in canvas-land.
 *
 * X11's XIM model assumes the application window is a real DOM-editable
 * surface. A <canvas> isn't, so we paint text on canvas but borrow a
 * 1px transparent <textarea> as the "real" focused element. Whatever the
 * user types, composes, or pastes there gets forwarded as synthetic
 * X11 KeyPress events into em-x11.
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
 * Composition flow (browser DOM → InputBridge → wasm):
 *
 *   - Plain typing: keydown.key → keysym (KeyboardEvent path) AND
 *     beforeinput.data → synthetic KeyPress with text. We let the
 *     keydown produce the keysym + utf8 in one shot when the key is
 *     a single printable character; for IME (key='Process'), we
 *     suppress the keydown's keysym and wait for compositionend.
 *   - IME composition: keydown.key='Process' (suppressed),
 *     compositionstart/update (no-op for Tier A), compositionend.data →
 *     one synthetic KeyPress carrying the composed string.
 *   - Backspace/Enter/Arrows: keydown only (no DOM editing on the
 *     hidden textarea -- preventDefault keeps it empty).
 */

import type { Host } from './index.js';

export class TextInputOverlay {
  private readonly el: HTMLTextAreaElement | null;
  private focusedWindow: number | null = null;
  /** Most recent caret spot (window-local, X11 px). null until the
   *  client calls XSetICValues(XNSpotLocation) at least once. */
  private spot: { window: number; x: number; y: number } | null = null;

  /** Public accessor so InputBridge can recognise the overlay as a
   *  legitimate focus surface (document.activeElement === overlay
   *  still counts as "focused on the X canvas" for keyboard routing). */
  get element(): HTMLTextAreaElement | null {
    return this.el;
  }

  constructor(private readonly host: Host) {
    if (host.canvas.headless || typeof document === 'undefined') {
      this.el = null;
      return;
    }
    const ta = document.createElement('textarea');
    ta.setAttribute('autocapitalize', 'off');
    ta.setAttribute('autocomplete', 'off');
    ta.setAttribute('autocorrect', 'off');
    ta.setAttribute('spellcheck', 'false');
    ta.setAttribute('aria-hidden', 'true');
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
    /* Hide the OS system caret. Windows/macOS draw a native blinking
     * caret at the focused text input even when the element is fully
     * transparent -- without this the user sees a phantom cursor that
     * drifts as the textarea's internal selection moves, separate from
     * the Tk-rendered caret in the X widget. */
    (ta.style as CSSStyleDeclaration & { caretColor?: string }).caretColor = 'transparent';
    /* The textarea itself never sees the mouse: clicks on the canvas
     * route normally. The OS IME doesn't need pointer events to anchor
     * its candidate window -- only DOM focus + caret rect. */
    ta.style.pointerEvents = 'none';
    ta.style.background = 'transparent';
    ta.style.resize = 'none';
    ta.style.zIndex = '2147483647';
    document.body.appendChild(ta);
    this.el = ta;
    this.attachListeners();
  }

  /* -- Bridge entry points (called from Host facade) ---------------------- */

  setFocus(window: number): void {
    if (!this.el) return;
    this.focusedWindow = window || null;
    if (!this.focusedWindow) {
      this.el.blur();
      this.el.style.left = '-9999px';
      return;
    }
    this.applyPosition();
    this.el.value = '';
    /* preventScroll: the page must not jump because we moved focus to
     * a 1px element off in the corner of the viewport. */
    try {
      (this.el.focus as (opts?: { preventScroll?: boolean }) => void)({
        preventScroll: true,
      });
    } catch {
      this.el.focus();
    }
  }

  clearFocus(): void {
    if (!this.el) return;
    this.focusedWindow = null;
    this.el.blur();
    this.el.style.left = '-9999px';
  }

  /** Caret moved inside the focused widget. Tk fires this on every
   *  cursor motion in entries / texts. We ignore spots from windows
   *  that aren't the current focus -- Tk pre-sets XNSpotLocation on
   *  every entry whether it's focused or not. */
  setSpot(window: number, x: number, y: number): void {
    if (!this.el) return;
    this.spot = { window, x, y };
    if (window === this.focusedWindow) this.applyPosition();
  }

  /* -- Position computation ---------------------------------------------- */

  private applyPosition(): void {
    if (!this.el || this.focusedWindow === null) return;
    const winId = this.focusedWindow;
    const origin = this.host.getWindowAbsOrigin(winId);
    if (!origin) return;
    const spotX = this.spot && this.spot.window === winId ? this.spot.x : 0;
    const spotY = this.spot && this.spot.window === winId ? this.spot.y : 0;
    /* Canvas-local CSS pixels = X root-relative pixels (1:1 backing). */
    const localX = origin.ax + spotX;
    const localY = origin.ay + spotY;
    const canvasEl = this.host.canvas.element;
    if (!canvasEl || !(canvasEl instanceof HTMLCanvasElement)) return;
    const rect = canvasEl.getBoundingClientRect();
    const dprX = rect.width / this.host.canvas.cssWidth;
    const dprY = rect.height / this.host.canvas.cssHeight;
    const vx = rect.left + localX * dprX;
    const vy = rect.top + localY * dprY;
    this.el.style.left = vx + 'px';
    this.el.style.top = vy + 'px';
  }

  /* -- DOM event handlers ------------------------------------------------ */

  private attachListeners(): void {
    if (!this.el) return;
    const ta = this.el;

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
      /* Try e.data first (Firefox tends to fill it accurately). Fall
       * back to textarea.value (xterm.js's strategy for Chromium).
       * setTimeout 0 lets the native compositionend write hit
       * textarea.value before we read it. */
      const evData = e.data ?? '';
      const flush = () => {
        let text = evData;
        if (!text && ta.value) text = ta.value;
        ta.value = '';
        if (text) this.dispatchTextKey(text);
      };
      if (evData) {
        flush();
      } else {
        setTimeout(flush, 0);
      }
    });

    /* beforeinput is our path for paste, autocomplete substitution, and
     * mobile-keyboard input -- anything that bypasses the keydown +
     * KeyboardEvent.key path. We preventDefault to keep the textarea
     * empty, then forward the data into Tk via a synthetic KeyPress.
     *
     * IME-related insertions (`insertCompositionText` /
     * `insertFromComposition`) are handled by the compositionend
     * listener above, so don't dispatch them twice here. */
    ta.addEventListener('beforeinput', (e: InputEvent) => {
      if (composing || e.inputType === 'insertCompositionText' ||
          e.inputType === 'insertFromComposition') {
        return;     /* let the textarea receive it; compositionend reads it back */
      }
      e.preventDefault();
      const t = e.inputType;
      if (t === 'insertText' || t === 'insertFromPaste') {
        const data = e.data ?? '';
        if (data) this.dispatchTextKey(data);
      }
    });

    /* keydown on the textarea: we DON'T preventDefault during composition
     * (Chromium aborts IME initiation if a Process keydown is canceled).
     * For non-composing keys, the window-level keydown handler in
     * devices.ts owns the keysym + text dispatch and preventDefaults the
     * raw keystroke; we don't need to do anything here. */
    ta.addEventListener('keydown', () => {});

    /* After IME or paste, the textarea may hold composed text. The
     * compositionend handler wipes it; keep this as a safety net for
     * cases (mobile keyboards, autocorrect) where input fires without a
     * compositionend pair. */
    ta.addEventListener('input', () => {
      if (!composing) ta.value = '';
    });

    /* Reposition on viewport reshape so candidate window stays anchored. */
    window.addEventListener('resize', () => this.applyPosition());
    window.addEventListener('scroll', () => this.applyPosition(), true);
  }

  private dispatchTextKey(text: string): void {
    if (this.focusedWindow === null) return;
    this.host.devices.pushTextKey(text);
  }
}
