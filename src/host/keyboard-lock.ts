/**
 * Keyboard Lock -- capture OS-level shortcut keys (Alt+Tab, Super,
 * Esc, F11, ...) while the canvas is in fullscreen mode.
 *
 * Web API: https://wicg.github.io/keyboard-lock/
 *   navigator.keyboard.lock([keyCodes])  --  return Promise<void>
 *   navigator.keyboard.unlock()
 *
 * Security model: lock() only does anything while the page is in
 * fullscreen. We hook fullscreenchange on the document and engage /
 * disengage automatically when a Host opts in. There is no permission
 * prompt; the act of entering fullscreen IS the user gesture.
 *
 * Typical usage from a game-style demo:
 *
 *   host.keyboard.lock.enable(['AltLeft','MetaLeft','Tab','Escape','F11']);
 *   // ... user clicks "Enter fullscreen" button ...
 *   canvas.requestFullscreen();
 *   // lock auto-engages here; on exit, auto-disengages.
 *
 * Browsers without the Keyboard API (Firefox / Safari / all mobile)
 * silently become no-ops -- callers don't need to feature-detect.
 */

export interface KeyboardLockOptions {
  /** KeyboardEvent.code values to capture. Empty / omitted = lock ALL
   *  keys (subject to OS-level reserved combos like Ctrl-Alt-Del that
   *  Chromium can't intercept anyway). */
  keys?: string[];
}

interface KeyboardApi {
  lock: (codes?: string[]) => Promise<void>;
  unlock: () => void;
}

function getKeyboardApi(): KeyboardApi | null {
  const nav = navigator as Navigator & { keyboard?: Partial<KeyboardApi> };
  if (typeof nav.keyboard?.lock !== 'function') return null;
  if (typeof nav.keyboard.unlock !== 'function') return null;
  return nav.keyboard as KeyboardApi;
}

export class KeyboardLockManager {
  private enabled = false;
  private wantedKeys: string[] = [];
  /** True while we currently hold the OS lock. Tracked so disable() can
   *  unlock if we're locked, and so we don't double-lock on
   *  fullscreenchange races. */
  private locked = false;
  private fsListener: (() => void) | null = null;

  /** Engage keyboard lock the next time the page enters fullscreen.
   *  If already in fullscreen when enable() is called, the lock kicks
   *  in immediately. */
  enable(opts: KeyboardLockOptions = {}): void {
    this.wantedKeys = opts.keys ? [...opts.keys] : [];
    this.enabled = true;
    if (!this.fsListener) {
      this.fsListener = () => this.onFullscreenChange();
      /* `document.fullscreenchange` is the cross-vendor spelling now;
       * older Safari uses webkitfullscreenchange. Hook both for safety
       * even though the Keyboard API itself is Chromium-only -- this
       * file shouldn't be the reason the rest of the demo stops
       * working on Safari. */
      document.addEventListener('fullscreenchange', this.fsListener);
      document.addEventListener('webkitfullscreenchange', this.fsListener);
    }
    /* If we're already fullscreen at enable-time, race the listener:
     * try to lock right now. The listener will lock again on the next
     * transition; navigator.keyboard.lock is idempotent. */
    if (this.isFullscreen()) void this.engage();
  }

  /** Stop auto-engaging on fullscreen, and release the lock right now
   *  if we currently hold it. */
  disable(): void {
    this.enabled = false;
    if (this.fsListener) {
      document.removeEventListener('fullscreenchange', this.fsListener);
      document.removeEventListener('webkitfullscreenchange', this.fsListener);
      this.fsListener = null;
    }
    if (this.locked) {
      const api = getKeyboardApi();
      api?.unlock();
      this.locked = false;
    }
  }

  /** True when the browser has the Keyboard Lock API available. Demos
   *  can use this to hide a "fullscreen keyboard lock" toggle in
   *  Firefox / Safari rather than show a broken control. */
  isAvailable(): boolean {
    return getKeyboardApi() !== null;
  }

  /** Are we currently holding the OS-level lock? */
  isLocked(): boolean {
    return this.locked;
  }

  /* -- internal -------------------------------------------------------- */

  private onFullscreenChange(): void {
    if (!this.enabled) return;
    if (this.isFullscreen()) {
      void this.engage();
    } else if (this.locked) {
      /* Exiting fullscreen: navigator.keyboard.unlock() is the spec
       * contract but the browser also auto-unlocks; we still call
       * explicitly so our `locked` mirror stays accurate. */
      const api = getKeyboardApi();
      api?.unlock();
      this.locked = false;
    }
  }

  private async engage(): Promise<void> {
    const api = getKeyboardApi();
    if (!api) return;
    try {
      await api.lock(this.wantedKeys);
      this.locked = true;
    } catch (err) {
      /* lock() can reject if not in fullscreen at await-resolve time
       * (race between enable() and exit), if the user is in another
       * keyboard-grabbing context (e.g. printing dialog), or if the
       * browser rejected the key list. Stay silent -- this is an
       * opt-in best-effort feature; demos shouldn't crash because the
       * OS won't let us steal Alt-Tab right this instant. */
      void err;
    }
  }

  private isFullscreen(): boolean {
    const doc = document as Document & {
      webkitFullscreenElement?: Element | null;
    };
    return Boolean(doc.fullscreenElement ?? doc.webkitFullscreenElement);
  }
}
