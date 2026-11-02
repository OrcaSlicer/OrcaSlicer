# Plugin page order

Users can drag the main window's plugin tabs before or after another plugin tab.
Built-in tabs stay in their existing positions. Plugins have no ordering or
priority API: the order is a preference owned by the user.

`Notebook` recognises drags on plugin tab buttons and reports the source, target
and drop side to `PluginPages`. The host resolves those buttons to active page
capabilities and changes its full page order. Invalid targets, cancelled drags
and ordinary clicks do not change the preference. The existing page windows and
their browser instances survive reordering, and the selected page is retained.

## Persistence and page lifecycle

The `plugin_pages_order` AppConfig value is a JSON array of objects containing
`plugin_key` and `name`. These are the existing package and page capability
identities, not translated labels or positions. Separate fields avoid ambiguity
when an identity contains punctuation.

The saved sequence includes pages which are currently disabled, unloaded or
missing. Removing a page does not remove its remembered position. Registration
sorts active pages against that sequence, so asynchronous startup and reloading a
plugin restore the user's order. Newly encountered identities append after the
known ones. Renaming a capability or changing its package identity produces a
new identity.

The host writes the preference only after an actual user move. AppConfig marks
it dirty and uses the normal idle or exit save. Startup, shutdown, relayout,
overflow selection and enable-state changes do not overwrite the saved order.
Malformed settings fall back to registration order; invalid or duplicate entries
within an otherwise valid array are ignored.

## Overflow

The existing visible-page limit still applies. The first slots follow the full
order, and the last slot displays one page from the remaining overflow sequence.
The overflow menu lists that remaining sequence in the same order.

To promote an overflow page, select it from the menu and drag its tab before a
fixed plugin tab. Drops are relative to the actual source and target identities
in the full order, including any hidden pages between them. A selected page which
ends in overflow stays displayed in the last slot. Otherwise, a moved page which
ends in overflow is displayed there. Preserving the active page takes priority
when both pages would need that slot. Choosing an overflow page by itself never
changes the saved order. With only one visible plugin slot there is
no second tab to drag against; increasing the visible-page preference makes
reordering available.
