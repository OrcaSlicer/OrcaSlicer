// The guide pages' shared notice, a modal <dialog>: it traps Tab, closes on Escape and returns focus itself.
function ShowNotice( nShow )
{
	let Notice = document.getElementById("NoticeBody");
	if (nShow == 0)
		Notice.close();
	else if (!Notice.open)
		Notice.showModal();
}

// Escape belongs to the open notice, not to the page key handlers behind it.
window.addEventListener('keydown', function (e) {
	let Notice = document.getElementById("NoticeBody");
	if (e.key === 'Escape' && Notice && Notice.open)
		e.stopPropagation();
}, true);

// The guide's window holds nothing but this page, so Tab past the last control would land on
// the bare document, a stop that says nothing. Wrap instead, like Tab in a dialog.
function TabStops(Container)
{
	return Array.from(Container.querySelectorAll('a[href], button, input, select, textarea, [tabindex]')).filter(el =>
		el.tabIndex >= 0 && !el.disabled && !el.closest('[inert]') &&
		(el.checkVisibility ? el.checkVisibility({visibilityProperty: true}) : el.getClientRects().length > 0) &&
		// a radio group is one stop: its checked radio, or the first when none is checked
		!(el.type === 'radio' && el.name && !el.checked &&
			(Container.querySelector(`input[type="radio"][name="${el.name}"]:checked`) ||
			 Container.querySelector(`input[type="radio"][name="${el.name}"]`)) !== el));
}

window.addEventListener('keydown', function (e) {
	if (e.key !== 'Tab' || e.ctrlKey || e.altKey || e.metaKey || e.defaultPrevented)
		return;
	let Notice = document.getElementById("NoticeBody");
	let Stops = TabStops(Notice && Notice.open ? Notice : document.body);
	if (Stops.length == 0) {
		e.preventDefault();
		return;
	}
	let Active = document.activeElement;
	let Edge   = e.shiftKey ? Stops[0] : Stops[Stops.length - 1];
	// from the edge control, or Shift+Tab with nothing focused yet (the page start)
	if (Active === Edge || (e.shiftKey && (!Active || Active === document.body))) {
		e.preventDefault();
		(e.shiftKey ? Stops[Stops.length - 1] : Stops[0]).focus();
	}
});

function ClosePage() {
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "close_page";
	SendWXMessage(JSON.stringify(tSend));
}

window.addEventListener('wheel', function (event) {
    if (event.ctrlKey === true || event.metaKey) {
        event.preventDefault();
    }
}, { passive: false });
