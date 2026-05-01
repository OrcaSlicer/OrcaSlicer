function ClosePage() {
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "close_page";
	SendWXMessage(JSON.stringify(tSend));
}

document.onkeydown = function (event) {
    var e = event || window.event || arguments.callee.caller.arguments[0];

    if (window.event) {
        try { e.keyCode = 0; } catch (e) { }
        e.returnValue = false;
    }
};

window.addEventListener('wheel', function (event) {
    if (event.ctrlKey === true || event.metaKey) {
        event.preventDefault();
    }
}, { passive: false });
