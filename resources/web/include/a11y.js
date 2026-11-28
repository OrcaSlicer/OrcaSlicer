// Shared accessibility support for the bundled web pages: one <script> include, no init call.
// Listeners are delegated on the document, so page handlers on the elements run first.

// The screen reader's language, from ?lang; remembered for the guide steps that link without it.
(function () {
	var lang = new URLSearchParams(location.search).get('lang');
	try {
		if (lang)
			localStorage.setItem('orca_ui_lang', lang);
		else
			lang = localStorage.getItem('orca_ui_lang');
	} catch (e) {}
	if (lang)
		document.documentElement.lang = lang.replace('_', '-');
})();

// Enter/Space clicks a non-native element with a widget role (the `<div onclick>` pattern).
document.addEventListener('keydown', function (e) {
	if (e.defaultPrevented || e.ctrlKey || e.metaKey || e.altKey)
		return;
	if (e.repeat)
		return;   // a native control fires once per press too
	var enter = e.key === 'Enter';
	if (!enter && e.key !== ' ')
		return;
	var t = e.target;
	if (!t || !t.closest)
		return;   // no focused element
	if (t.closest('input, textarea, select, button, a[href]') || t.isContentEditable)
		return;   // native controls handle their own keys, even inside a widget row
	var el = t.closest('[role="button"], [role="checkbox"], [role="menuitem"], [role="link"]');
	if (!el)
		return;
	if (el.getAttribute('aria-disabled') === 'true')
		return;   // aria-disabled does not stop click(), so honor it here
	if (el.getAttribute('role') === 'link' && !enter)
		return;   // links activate on Enter only
	e.preventDefault();
	el.click();
});

// Fields that take typed text, and with it their own edit chords and edit menu.
var TEXT_FIELDS = 'textarea, input:not([type="checkbox"]):not([type="radio"]):not([type="button"]):not([type="submit"]):not([type="reset"])';

// Deny the browser's own shortcuts (F-keys, Ctrl/Meta chords): these pages are app UI, not websites.
document.addEventListener('keydown', function (e) {
	if (e.defaultPrevented || !e.target || !e.target.closest)
		return;
	if (/^F\d\d?$/.test(e.key)) {
		// Alt+F-keys and F10 (the host menu bar) belong to the window system
		if (!e.altKey && e.key !== 'F10')
			e.preventDefault();
		return;
	}
	if (e.altKey)
		return;   // Alt chords and AltGr belong to the window system
	if (e.metaKey && /^Key[QWMH]$|^Comma$/.test(e.code))
		return;   // the macOS app-level Cmd keys; e.code because e.key depends on the layout
	if (!e.ctrlKey && !e.metaKey)
		return;   // plain and Shift+ keys belong to the page
	if (e.target.closest(TEXT_FIELDS) || e.target.isContentEditable)
		return;   // a text field owns its chords
	if (e.key.length > 1)
		return;   // Ctrl+named keys switch tabs or move the caret, harmless here
	if (/^Key[ACVXYZ]$/.test(e.code))
		return;   // editing chords; e.code because the browser resolves them by position
	e.preventDefault();
});

// The browser context menu is chrome too, by mouse or keyboard; text fields keep the edit menu.
document.addEventListener('contextmenu', function (e) {
	if (e.defaultPrevented || !e.target || !e.target.closest)
		return;
	if (e.target.closest(TEXT_FIELDS) || e.target.isContentEditable)
		return;
	e.preventDefault();
});

// Write state only where it changes: Chromium announces every write, even of an unchanged value.
function SetCheckedState(nodes, bChecked) {
	var List = nodes.nodeType ? [nodes] : nodes;
	for (var n = 0; n < List.length; n++)
		if (List[n].checked !== bChecked)
			List[n].checked = bChecked;
}

function SetAriaState(nodes, strName, value) {
	var List = nodes.nodeType ? [nodes] : nodes;
	var strValue = String(value);
	for (var n = 0; n < List.length; n++)
		if (List[n].getAttribute(strName) !== strValue)
			List[n].setAttribute(strName, strValue);
}

// Profile and user supplied names go into markup built as a string.
function EscAttr(strText) {
	return String(strText).replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;");
}
