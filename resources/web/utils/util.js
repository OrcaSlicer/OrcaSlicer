
function isEmptyString(str) {
    return !str || str.trim().length === 0;
}

function isValidURL(url) {
    const pattern = new RegExp('^(https?:\\/\\/)?' + // protocol
        '((([a-z0-9\\-]+\\.)+[a-z]{2,})|' + // domain name
        'localhost|' + // localhost
        '\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}|' + // IP address
        '\\[?[a-f0-9]*:[a-f0-9:%.~+\\-]*\\]?)' + // IPv6
        '(\\:\\d+)?(\\/[^\\s]*)?$', 'i'); // port and path
    return pattern.test(url);
}

function disableRightClickMenu() {
    // Disable right-click menu only in non-dev mode
    const isDev = GetQueryString("dev") === "true";

    if (!isDev) {
        document.addEventListener('contextmenu', (e) => {
            e.preventDefault();
        });
    }
}

function isDevMode() {
    return GetQueryString("dev") === "true";
}

function suppressConsoleInNonDevMode() {
    try {
        if (!isDevMode()) {
            // Suppress console output in non-dev mode
            console.log = function () { };
            console.info = function () { };
        }
    } catch (e) {
        console.error("suppressConsoleInNonDevMode error:", e);
    }
}
suppressConsoleInNonDevMode();