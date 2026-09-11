let term = null;
let fitAddon = null;

// ANSI palettes for the xterm theme — the standard VS Code dark/light sets, picked by
// data-orca-theme. Constant data, so defined once at module scope.
const ANSI_DARK = {
  black:"#000000", red:"#cd3131", green:"#0dbc79", yellow:"#e5e510", blue:"#2472c8",
  magenta:"#bc3fbc", cyan:"#11a8cd", white:"#e5e5e5",
  brightBlack:"#666666", brightRed:"#f14c4c", brightGreen:"#23d18b", brightYellow:"#f5f543",
  brightBlue:"#3b8eea", brightMagenta:"#d670d6", brightCyan:"#29b8db", brightWhite:"#ffffff"
};
const ANSI_LIGHT = {
  black:"#000000", red:"#cd3131", green:"#107c10", yellow:"#949800", blue:"#0451a5",
  magenta:"#bc05bc", cyan:"#0598bc", white:"#555555",
  brightBlack:"#8a8a8a", brightRed:"#cd3131", brightGreen:"#14ce14", brightYellow:"#b5ba00",
  brightBlue:"#0451a5", brightMagenta:"#bc05bc", brightCyan:"#0598bc", brightWhite:"#a5a5a5"
};

// The xterm.js theme is a plain JS object (it cannot read CSS variables), so build it
// from the host contract: background/foreground come from the injected --orca-* colors;
// the ANSI palette is the fixed light or dark set chosen by data-orca-theme.
function XtermTheme() {
  var cs = getComputedStyle(document.documentElement);
  var bg = (cs.getPropertyValue('--orca-bg') || '#1e1e1e').trim();
  var fg = (cs.getPropertyValue('--orca-fg') || '#d4d4d4').trim();
  var dark = document.documentElement.getAttribute('data-orca-theme') !== 'light';
  return Object.assign({ background: bg, foreground: fg, cursor: fg }, dark ? ANSI_DARK : ANSI_LIGHT);
}

// Re-apply the xterm theme when the host flips data-orca-theme (live re-theme).
function WatchXtermTheme() {
  var obs = new MutationObserver(function () {
    if (term) term.options.theme = XtermTheme();
  });
  obs.observe(document.documentElement, { attributes: true, attributeFilter: ['data-orca-theme'] });
}

function OnInit() {
  if (typeof TranslatePage === "function")
    TranslatePage();

  term = new Terminal({
    cursorBlink: true,
    fontSize: 13,
    fontFamily: '"Cascadia Code", "Fira Code", "JetBrains Mono", monospace',
    theme: XtermTheme(),
    allowProposedApi: true
  });

  WatchXtermTheme();

  fitAddon = new (FitAddon.FitAddon || FitAddon)();
  term.loadAddon(fitAddon);

  term.open(document.getElementById("terminal-container"));
  fitAddon.fit();

  // Focus the terminal so it captures keyboard input
  term.focus();

  // Re-focus terminal when user clicks on it
  term.element.addEventListener("click", () => term.focus());

  window.addEventListener("resize", () => fitAddon.fit());

  // Send keystrokes to C++
  term.onData((data) => {
    SendWXMessage(JSON.stringify({
      command: "write_stdin",
      data: data
    }));
  });

  document.getElementById("run-btn").addEventListener("click", onRun);
  document.getElementById("cmd-input").addEventListener("keydown", (e) => {
    if (e.key === "Enter")
      onRun();
  });

  term.writeln("Plugin terminal ready.");
}

function onRun() {
  const input = document.getElementById("cmd-input");
  const cmd = input.value.trim();
  if (!cmd)
    return;

  input.value = "";

  term.writeln("$ " + cmd);

  setRunning(true);

  SendWXMessage(JSON.stringify({
    command: "run_command",
    cmd: cmd
  }));
}

function setRunning(running) {
  document.getElementById("run-btn").disabled = running;
  document.getElementById("cmd-input").disabled = running;
}

function HandleStudio(value) {
  const payload = (typeof value === "string") ? SafeJsonParse(value) : value;
  if (!payload || typeof payload !== "object")
    return;

  switch (payload.command) {
    case "output":
      if (payload.lines && Array.isArray(payload.lines)) {
        payload.lines.forEach((line) => {
          if (line.is_stderr)
            term.writeln("\x1b[91m" + line.text + "\x1b[0m");
          else
            term.writeln(line.text);
        });
      }
      break;

    case "process_done":
      setRunning(false);
      term.writeln("\x1b[90mProcess exited with code: " + payload.exit_code + "\x1b[0m");
      break;

    case "process_error":
      setRunning(false);
      term.writeln("\x1b[91m" + payload.message + "\x1b[0m");
      break;
  }
}

function SafeJsonParse(value) {
  try {
    return JSON.parse(value);
  } catch (e) {
    return null;
  }
}
