# Portrait dock height and compact covers

The user reported that top/bottom dock panels cannot be resized far enough on a portrait monitor and that minimum-size covers remain large. This is an adjustment to the existing audio-library desktop UI, preserving its five themes, thin borders, spacing and mouse-driven controls. No new visual design or Figma asset is needed.

## Behavior

- Let top and bottom bands grow with available workspace height instead of a fixed 320px ceiling. Keep the current readable module minimums, waveform and at least the existing file-list minimum.
- Keep the resized height through save/restart, float/redock, and temporary window shrink/restore when the layout can still fit.
- Minimum-size covers remain approximately 56px and use more than eight columns when space permits. Keep even row spacing and full-width fitting; enlarging the slider still enlarges covers.
- Retain existing snap hit areas, data access, cover loading, sidebar dimensions and theme appearance.
- Follow-up reported 2026-10-29: when Info and Filter exchange left/right positions, both outer three-dot splitters remain visible and resize their current column. Include the swapped layout and its persistence in the same isolated preview.

## Steps and evidence

1. Add failing behavior tests for tall bands, both bands, smaller-window fitting and compact cover grids.
2. Remove the fixed caps at the height renderer, drag limit and drop-preview paths, and at the cover-grid column calculation.
3. Build an isolated QA package. Exercise actual app-owned drag input and screenshot the real renderer in portrait/landscape, top/bottom, resize limits, restore and compact/enlarged covers. No shared desktop input.
4. Provide the actual package, screenshots and test results for user confirmation. This follow-up is not covered by the previous version approval; do not publish before the user sees this version.

## Implementation and results

- Height limits now follow the available workspace and preserve the file-list minimum. A drag starts from the currently displayed band height when the window has temporarily reduced it. Waveform height budgeting also includes the playlist quick bar.
- The cover grid no longer has an eight-column ceiling. Small covers use additional columns while retaining even row spacing and the existing size slider.
- Outer column grips follow occupied left/right roots instead of specific modules. Both resize the persisted column width, including when Info and Filter exchange positions. The existing home-position splitters and nested leaf splitters remain in place.
- The isolated QA pointer targets the workspace viewport for outer splitters, including after a floating panel has returned to a dock. Actual rendered cover columns and thumbnail size are exposed through the existing diagnostics.

Initial verification: original size limits and missing swapped-column grip coverage were reproduced before their fixes. Five related native suites and 36 Python checks passed. The real isolated package passed 32 runtime checks, including tall/minimum bands, shrink/restore, both bands together, compact/enlarged covers, five themes, swapped column resizing, and restart persistence. The swapped-column fixture is loaded from saved settings, then resized through real application-owned input. The user subsequently reported flicker, requiring the additional verification below. No release or shared-branch merge is included in this follow-up.

## Flicker follow-up — 2026-10-29

The user reports that the cover wall flickers. The preceding static screenshots did not establish temporal stability; acceptance and integration remain pending.

1. Capture consecutive real frames in a separate isolated profile, including grid geometry, scrollbar state and ready/placeholder counts. Reproduce at thumbnail and dock-size boundaries.
2. Add a regression that fails for the reproduced cause, then fix only that cause.
3. Verify stationary covers, resizing and scrolling over consecutive frames in all five themes, and rerun the related dock checks. Deliver a new real preview and package for confirmation.

Reproduced cause: with 48 covers, minimum thumbnail size, a 1120×1600 outer window and a 362px top band, the automatic vertical scrollbar alternates every frame. Its width changes the fitted thumbnail from 57.0769px to 58px; that changes the content height enough to reverse the scrollbar decision on the next frame. All 48 textures remain ready, so this is a layout feedback loop rather than repeated image loading.

The cover-wall child now reserves the vertical scrollbar gutter consistently using the existing ImGui flag. Transient scrollbar colors still control its visual reveal; grid sizing, cover loading and database behavior remain unchanged.

Verification: the new real-package regression failed on the original build with 179 changes in 180 consecutive frames. The fixed build passed 44 stationary scenarios (180 consecutive frames each), covering top/bottom docking, five themes, nearby height boundaries, scrolling, larger covers and window shrink/restore. Sixteen captured sequences also passed exact grid-pixel comparisons across 24 consecutive frames. The same package passed all 32 earlier dock checks. Five native suites and 36 Python checks passed again, as did a clean build with local-only QA narration/capture supplements removed. Theme reloading can expose a partial ready snapshot, so the temporal test explicitly waits for the full known 48-cover fixture before sampling.

Status: technical verification passed; new actual preview and isolated package are ready for user confirmation. The preceding approval for another released version is not acceptance of this follow-up.
