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

Status: original size limits and missing swapped-column grip coverage were reproduced before their fixes. Five related native suites and 36 Python checks pass. The real isolated package passed 32 runtime checks, including tall/minimum bands, shrink/restore, both bands together, compact/enlarged covers, five themes, swapped column resizing, and restart persistence. The swapped-column fixture is loaded from saved settings, then resized through real application-owned input. Technical verification is complete; front-end confirmation by the user remains pending. No release or shared-branch merge is included in this follow-up.
