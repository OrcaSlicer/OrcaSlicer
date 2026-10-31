# Plugin tab ordering GUI regression checks

Use an isolated Orca profile with at least three Pages capabilities. Give their
tabs distinct names and include two pages from one package if available. Record
the operating system, source commit and theme for native results. These checks
complement the `PluginPageOrder` tests; automated ordering tests do not establish
native mouse-capture or browser behaviour.

1. With all plugin pages visible, open one page and change a control in it.
   Drag another plugin tab left and right, dropping on each half of a target.
   Verify the resulting order, unchanged built-in order, unchanged selection and
   retained control value. Clicking and keyboard navigation must still select
   their usual tabs.
2. Attempt to drag built-in tabs, drop a plugin on a built-in tab, release outside
   the strip, and press Escape during a drag. Verify no order change or unwanted
   selection. Begin a drag and interrupt capture by switching windows; verify
   ordinary clicking and dragging work afterwards.
3. Save a distinctive order, quit normally and relaunch. Verify the order after
   all plugins finish loading. Repeat with different plugin load timings and
   select a page before an earlier saved page finishes loading; the selected page
   must stay visible if it moves into overflow. Then disable and re-enable a page
   and reload its package. Its position must return.
   Verify newly installed page identities appear after known ones.
4. Leave a page disabled, reorder the remaining pages, quit and relaunch, then
   enable the page. Its remembered identity must still be in the saved sequence.
   Changing another page's title must not accidentally inherit that identity.
5. Set the visible-page count below the number of pages. Select a hidden page
   from overflow and drag it before the first plugin tab. Verify it becomes a
   fixed visible tab, the overflow list follows the new full order, and selection
   alone does not reorder anything. Move a fixed tab after the displayed overflow
   page and verify the moved tab stays visible in the last slot when the active
   page is still in a fixed slot. Repeat with the overflow page selected and
   verify that page remains selected, even if the dragged tab becomes hidden in
   the menu. Quit and verify the full order again after relaunch.
6. At a visible-page count of one, verify overflow selection still works and no
   invalid drag destination is accepted. Restore a larger count and verify the
   saved order is retained. Repeat the drag checks in a narrow window with compact
   icon-only labels and in light and dark themes. The drop marker must be visible.
7. Reload or disable a plugin during a drag, then verify there is no crash or
   stuck mouse capture. Rebuild the main window through a language change and
   verify order restoration. Reordering must not create browsers for unvisited
   pages; an opened page must retain its state.

Repeat the native drag/capture and restart checks on Windows, Linux and macOS
when those runtimes are available. Report unavailable platforms as untested.
