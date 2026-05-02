# xorg alignment audit (em-x11 renderer / dispatcher)

Source-of-truth references (all under `references/xserver/`):

- `dix/window.c` — structural ops (Map/Unmap/Configure/Reparent/RestackWindow)
- `mi/miwindow.c` — `miMarkOverlappedWindows`, `miHandleValidateExposures`,
  `miMoveWindow`, `miResizeWindow`, `miChangeBorderWidth`
- `mi/mivaltree.c` — `miValidateTree`, `miComputeClips`
- `mi/miexpose.c` — `miHandleExposures` (CopyArea path),
  `miSendExposures`, `miWindowExposures`, `miPaintWindow`

## The xorg pipeline for any structural change

Every restructure in xorg follows the same five-step dance. `MapWindow`,
`UnmapWindow`, `ConfigureWindow`, `ReparentWindow`, `RestackWindow`,
`miMoveWindow`, `miResizeWindow` all call into it:

```
1. (op-specific) DeliverMapNotify / DeliverUnmapNotify / etc.
2. MarkOverlappedWindows(pWin, pFirst, &pLayerWin)
   -> walks subtree, calls MarkWindow on every viewable window whose
      borderSize intersects pWin->borderSize (or the whole subtree if
      pWin == pFirst). Allocates a `valdata` struct on each marked
      window holding the pre-change abs origin + borderVisible.
3. ValidateTree(pLayerWin->parent, pFirst, kind)
   -> recomputes clipList + borderClip for every marked window.
   -> for each marked window populates valdata->after.exposed and
      valdata->after.borderExposed:
         exposed       = newClipList - oldClipList   (the area that
                         became visible)
         borderExposed = newBorderClip - newWinSize  (the strip of
                         border that became visible)
      see mivaltree.c:295-340 (miComputeClips) and 707-728
      (miValidateTree top-level).
4. HandleExposures(pLayerWin->parent)  ==  miHandleValidateExposures
   -> walks the subtree, for every window with valdata:
        a. PaintWindow(pChild, &valdata->after.borderExposed, PW_BORDER)
        b. WindowExposures(pChild, &valdata->after.exposed)
              -> miWindowExposures (miexpose.c:365):
                  paint bg via PaintWindow(..., prgn, PW_BACKGROUND)
                  if (eventMask & ExposureMask) miSendExposures(...)
        c. free valdata
      see miwindow.c:211 (miHandleValidateExposures).
5. PostValidateTree (Composite hook); WindowsRestructured.
```

The crucial property: **xorg never repaints pixels that did not
change visibility.** A sibling whose clipList shrank because something
was raised above it is **not** repainted, **not** re-exposed --
those pixels are still on the framebuffer. Only the *newly exposed*
area of the raised window has its bg painted, and only that area's
Expose is dispatched.

This is the property our renderer breaks today.

## Per-op control flow in xorg (minimal trace)

### MapWindow (window.c:2654)

```
if (pWin->mapped) return;
pWin->mapped = TRUE;
if (SubStrSend(pWin, pParent)) DeliverMapNotify(pWin);
if (!pParent->realized) return;        // mapped-but-not-viewable
RealizeTree(pWin);                     // sets viewable, RealizeWindow
if (pWin->viewable) {
  MarkOverlappedWindows(pWin, pWin, &pLayerWin);
  ValidateTree(pLayerWin->parent, pLayerWin, VTMap);
  HandleExposures(pLayerWin->parent);
}
```

### UnmapWindow (window.c:2844)

```
if (!pWin->mapped) return;
DeliverUnmapNotify(pWin, fromConfigure);
if (wasViewable && !fromConfigure) {
  pWin->valdata = UnmapValData;
  MarkOverlappedWindows(pWin, pWin->nextSib, &pLayerWin);
  MarkWindow(pLayerWin->parent);
}
pWin->mapped = FALSE;
UnrealizeTree(pWin, fromConfigure);
if (wasViewable && !fromConfigure) {
  ValidateTree(pLayerWin->parent, pWin, VTUnmap);
  HandleExposures(pLayerWin->parent);
}
```

### ConfigureWindow (window.c:2187)
Calls `pScreen->MoveWindow` / `pScreen->ResizeWindow`, both
implemented by `miMoveWindow` / `miResizeWindow` (miwindow.c:244,342)
which run the Mark/Validate/HandleExposures pipeline with kind
`VTMove` or `VTOther`. Sends ConfigureNotify after the pipeline.

### XRaiseWindow / XLowerWindow / XRestackWindow

Goes through `dix/window.c::ReflectStackChange` (not shown above)
which restacks the sibling list and runs:

```
MarkOverlappedWindows(pWin, pFirstChange, NULL);
ValidateTree(pParent, pFirstChange, VTStack);
HandleExposures(pParent);
```

`VTStack` in `miValidateTree` skips the borderClip translate path
(no movement) and only recomputes exposed = newClipList -
oldClipList, see mivaltree.c:715 switch.

### ReparentWindow (window.c:2502)

Two pipelines back-to-back: one for the old parent (window
disappearing), one for the new parent (window appearing).

## Where em-x11 diverges, by symptom

### Symptom 1: raising xeyes over xcalc shows root weave instead of xcalc through xeyes' transparent shell

**xorg flow:**
- xeyes' clipList grows (gains the area xcalc used to cover)
- xcalc's clipList shrinks
- xeyes' `valdata->after.exposed` = the newly-uncovered area
- xcalc's `valdata->after.exposed` = empty
- `HandleExposures` paints xeyes' bg (= None, no paint) on `exposed`
- Sends Expose to xeyes only
- **xcalc's pixels are never touched.** Button labels, app drawings stay.

**em-x11 today:**
- `tree.raiseWindow` calls `repaintAbsoluteRect(rax, ray, rw, rh)`
- `repaintAbsoluteRect` walks the tree, paints **every** mapped
  window's bg in stack order inside the rect
- xcalc's bg gets repainted -> button labels lost
- xeyes paints its bg over the whole rect (now bgType=None: skipped --
  but xcalc was already wiped)
- Result: root weave under xeyes' transparent shell, not xcalc

**Root divergence:** we have no per-window clipList tracking, so we
can't compute "newly exposed" -- we re-derive clip ad hoc in
`applyWindowClip` and conservatively repaint the whole rect.

### Symptom 2: configure (drag) erases siblings underneath

Same shape as Symptom 1. `tree.configureWindow` calls
`repaintAbsoluteRect` on the OLD rect (correctly -- those pixels
must be reconstructed) and `paintWindowSubtree` on the new rect.
The new-rect paint walks the moved window + its descendants but
does NOT touch siblings the new rect overlaps. So far OK.

But the OLD-rect erasure walks all siblings, repainting their bg
inside that rect. Any sibling whose pixels were partly visible
under the moved window gets its app drawings wiped within the old
rect. Without backing store, those pixels need an Expose to the
sibling -- which we don't send.

**xorg flow:** `miMoveWindow` -> Validate -> HandleExposures sends
Expose to the moved window for newly-exposed area, plus to siblings
whose clip just GREW because the moved window left. Sibling Exposes
ARE sent here (because their clip changed). xcalc would correctly
redraw its old visible area + the newly-uncovered strip.

### Symptom 3: configure leaves the moved window's old position bg-painted with no app content

This is the same architectural issue: we paint `repaintAbsoluteRect`
which fills bg from root, but never sends Expose to sibling windows
whose pixels showed in that area. Their bg is now there but their
content is gone.

`onUnmap` and `onDestroy` already do `mappedWindowsIntersecting`
+ `pushExposeForWindow` after the wipe -- this is an em-x11 specific
patch for the "we have no backing store, so re-expose siblings whose
content was wiped" problem. **`onConfigure`, `onReparent`, `onRaise`
need the same patch.** The recently-added sibling Expose in `onRaise`
is correct *as a backing-store substitute*, but it still doesn't
solve Symptom 1 because it fires after `repaintAbsoluteRect` has
already wiped xcalc, and xcalc redraws on its own schedule (inside
emscripten_sleep yield); meanwhile xeyes' transparent shell now
reveals weave.

### Symptom 4: bgType handling now correct (just landed)

The four-state backgroundState (None/ParentRelative/Pixel/Pixmap)
matches xorg as of the prior commit. ParentRelative is collapsed
to None at the bridge -- mivaltree.c:240 + miexpose.c:425-426 show
xorg walks the parent chain to find the actual tile/pixel, which
we don't yet do.

## Proposed correction: minimal-region paint

The architectural fix is to maintain per-window `clipList` (as a
bounding-rect list) and on every restructure compute `exposed`
regions via the xorg algorithm. That's the full Mark/Validate/
HandleExposures pipeline ported.

Concrete data:

```ts
interface ManagedWindow {
  // ... existing fields
  /** Absolute-canvas rectangles representing the unobscured area of
   *  this window's content rect (xorg clipList). Empty when fully
   *  obscured or unmapped. */
  clipList: Rect[];
  /** Same, including the border ring (xorg borderClip). */
  borderClip: Rect[];
}
```

Region ops needed on `Rect[]`:

- `union(a, b)` — append + canonicalize to disjoint set
- `intersect(a, b)`
- `subtract(a, b)`
- `translate(a, dx, dy)`
- `extents(a)` — bounding box

Implementable in ~200 LOC; or pull in pixman (~20K LOC C, would
need a wasm-side region API and JS wrapper). Recommend the
hand-rolled rect-list version: bounding-rect approximation is
fine for our use cases (no shaped-overlap correctness needed
beyond what we have).

Pipeline:

```ts
function restructure(kind: 'map'|'unmap'|'stack'|'move'|'resize'|...,
                    window, ...args) {
  // 1. Mark
  const marked = markOverlappedWindows(window, firstChange);
  saveBefore(marked);

  // 2. Mutate
  applyMutation(window, args);

  // 3. Validate: recompute clipList for every marked window
  for (w of marked) {
    const newClip = computeClip(w);
    w.exposed = subtract(newClip, w.clipList);
    w.borderExposed = subtract(newBorderClip, newWinSize);
    w.clipList = newClip;
    w.borderClip = newBorderClip;
  }

  // 4. HandleExposures
  for (w of marked) {
    if (!isEmpty(w.borderExposed))
      paintBorderInRegion(w, w.borderExposed);
    if (!isEmpty(w.exposed)) {
      if (w.bgType !== 'none')
        paintBgInRegion(w, w.exposed);
      pushExposeForRegion(w, w.exposed);
    }
  }
}
```

`paintBgInRegion` and `paintBorderInRegion` use Canvas 2D clip with
the rect list (current `applyWindowClip` infra), then fillRect.
`pushExposeForRegion` queues one Expose per rect in the region (or
extents-only when count > RECTLIMIT, mirroring miexpose.c:373).

## Order of work (proposed, smallest-first)

1. **Region module** (`src/host/render/region.ts`): `Rect[]`-based
   union/intersect/subtract/translate. Pure, testable.

2. **clipList tracking** in `ManagedWindow`. Compute on map/unmap/
   raise/configure/reparent. Initially: keep current pixel painting,
   just maintain `clipList` correctly + assert against current
   behaviour.

3. **Replace `repaintAbsoluteRect` with `paintBgInRegion`** at all
   call sites. Pixels outside `exposed` are no longer touched. This
   fixes Symptom 1 directly.

4. **Replace ad-hoc Expose synthesis** (`pushExposeForWindow` +
   `mappedWindowsIntersecting`) with per-window `valdata.exposed`
   conversion to Expose events, mirroring `miSendExposures`.

5. **ParentRelative**: when painting a window's bg in
   `paintBackgroundRect`, walk parent chain when bgType=='ParentRelative'
   per miexpose.c:425.

6. **borderExposed** (low priority): currently we always repaint the
   entire border ring; mivaltree's borderExposed = newBorderClip -
   newWinSize - oldBorderClip is more efficient but visually
   identical for simple cases.

## Notes / scope limits

- Shaped windows: xorg consults `wBoundingShape`/`wClipShape` inside
  Validate. Our existing `shape` handling kicks in inside Canvas
  clip; for clipList computation we approximate with bounding rect
  (already the conservative path in `getOccluderRects`).

- Backing store: xorg can DDX-OPT to backing pixmap. We don't have
  that -- the algorithm above is correct precisely BECAUSE we don't
  paint pixels we don't need to. The only place we still need to
  send Expose to siblings post-wipe (the existing `onUnmap` /
  `onDestroy` shape) is when bg painting *is* required -- e.g.
  unmap exposes the area behind the unmapped window, and per
  miUnmapWindow's flow xorg ALSO sends Expose to whatever is
  uncovered. Our current shape is consistent there.

- Multi-screen / Composite / RANDR: not applicable.
