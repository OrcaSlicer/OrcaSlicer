// Script runner page. The host (ScriptRunnerDialog) parses, describes and runs scripts and words what
// it reports; this page shows the steps as a rail that fills in as the script runs, and asks.

const FLAG_LABELS = {
  writes_file: ["flag_writes_file", "Writes a file"],
  changes_presets: ["flag_changes_presets", "Changes presets"],
  replaces_project: ["flag_replaces_project", "Replaces the project"],
  sends_to_printer: ["flag_sends_to_printer", "Sends to a printer"],
};

const state = {
  ready: false,
  origin: "",          // "file" or "paste"
  fileName: "",
  loadedText: null,    // the opened file's text, to tell when the source is edited
  parseSeq: 0,
  parseTimer: 0,
  parsed: null,        // the host's last description: {ok, error, name, steps, count_text}
  stale: false,        // the text has changed since the host described it; Run waits for the new description
  pending: false,      // Run was pressed and the host hasn't started or refused the run yet
  running: false,
  single: null,        // the step of a single-step run
  total: 0,
  started: new Map(),  // step index -> start time of a running step
  results: new Map(),  // step index -> what the host reported when it finished
  skipped: new Set(),
  open: new Set(),     // step indexes opened to their details
  outcome: null,       // the last run's {kind, text, detail}
  outDir: "",
  hasRun: false,
  message: null,       // a refusal or error to show in the footer: {kind, text, detail}
};

const rows = new Map(); // step index -> its <li>
let ticker = 0;

function $(id) { return document.getElementById(id); }

function send(command, extra) {
  SendWXMessage(JSON.stringify(Object.assign({ command }, extra || {})));
}

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined && text !== null) node.textContent = text;
  return node;
}

function seconds(ms) { return T("seconds", "%s s", (ms / 1000).toFixed(1)); }

// ---- source -----------------------------------------------------------------------------------

function sourceText() { return $("source").value; }

function scheduleParse(delay) {
  clearTimeout(state.parseTimer);
  // Numbered now, so a description of the text before this change is not taken for this one.
  const seq = ++state.parseSeq;
  state.stale = true;
  state.parseTimer = setTimeout(() => {
    const text = sourceText();
    if (!text.trim()) {
      onParsed({ seq, ok: false, empty: true });
      return;
    }
    send("parse", { text, seq });
  }, delay);
}

function onSource(msg) {
  state.origin = msg.origin;
  state.fileName = msg.file_name || "";
  state.loadedText = msg.origin === "file" ? msg.text : null;
  $("source").value = msg.text;
  if (msg.origin === "paste") setSectionOpen("source-section", true);
  resetRun();
  scheduleParse(0);
}

function onSourceInput() {
  if (!state.origin) state.origin = "paste";
  scheduleParse(250);
  render();
}

function renderChip() {
  const text = sourceText();
  const shown = state.origin === "file" || text.trim() !== "";
  $("chip").hidden = !shown;
  if (!shown) return;
  $("chip-name").textContent = state.origin === "file" ? state.fileName : T("pasted_script", "Pasted script");
  $("chip-edited").hidden = !(state.origin === "file" && state.loadedText !== null && text !== state.loadedText);
}

// ---- sections ---------------------------------------------------------------------------------

function setSectionOpen(id, open) {
  const section = $(id);
  section.classList.toggle("is-open", open);
  const head = section.querySelector(".section-head");
  head.setAttribute("aria-expanded", String(open));
  $(head.getAttribute("aria-controls")).hidden = !open;
}

function toggleSection(id) { setSectionOpen(id, !$(id).classList.contains("is-open")); }

// ---- the step list ----------------------------------------------------------------------------

function onParsed(msg) {
  if (msg.seq !== state.parseSeq) return; // an answer to text that has changed since
  // Run waits for the latest description and the text is frozen from the click on, so no answer can
  // arrive for a run's script; none may reset a run's log either way.
  if (busy()) return;
  state.stale = false;
  state.parsed = msg;
  resetRun();
  buildRail();
  render();
}

function resetRun() {
  state.results.clear();
  state.skipped.clear();
  state.started.clear();
  state.open.clear();
  state.outcome = null;
  state.message = null;
  state.hasRun = false;
}

function buildRail() {
  const rail = $("steps");
  rail.textContent = "";
  rows.clear();
  const steps = state.parsed && state.parsed.ok ? state.parsed.steps : [];
  for (const step of steps) {
    const row = makeRow(step);
    rows.set(step.index, row);
    rail.appendChild(row);
  }
}

function makeRow(step) {
  const row = el("li", "step is-wait");
  row.dataset.index = step.index;

  row.appendChild(el("span", "dot", String(step.index)));

  const main = el("div", "step-main");
  const head = el("button", "step-head");
  head.type = "button";
  head.setAttribute("aria-expanded", "false");
  head.appendChild(el("span", "step-title", step.title));
  if (step.what_text) head.appendChild(el("span", "step-what", step.what_text));
  if (step.param_text) head.appendChild(el("span", "step-param", step.param_text));
  const tags = el("span", "step-tags");
  for (const id of step.flags || []) {
    const label = FLAG_LABELS[id];
    if (label) tags.appendChild(el("span", "tag", T(label[0], label[1])));
  }
  if (step.checks_result) tags.appendChild(el("span", "tag is-check", T("checks_result", "Checks the result")));
  if (tags.childNodes.length) head.appendChild(tags);
  head.addEventListener("click", () => toggleStep(step.index));
  main.appendChild(head);

  const detail = el("div", "step-detail");
  detail.hidden = true;
  main.appendChild(detail);
  row.appendChild(main);

  const side = el("div", "step-side");
  side.appendChild(el("span", "time"));
  const play = el("button", "play", "▷");
  play.type = "button";
  play.title = T("run_this_step", "Run this step");
  play.setAttribute("aria-label", play.title);
  play.addEventListener("click", () => runStep(step.index));
  side.appendChild(play);
  row.appendChild(side);
  return row;
}

function stepOf(index) {
  return state.parsed && state.parsed.ok ? state.parsed.steps.find((s) => s.index === index) : null;
}

function toggleStep(index) {
  if (state.open.has(index)) state.open.delete(index);
  else state.open.add(index);
  renderRow(index);
}

function renderRow(index) {
  const row = rows.get(index);
  const step = stepOf(index);
  if (!row || !step) return;
  const result = state.results.get(index);
  let status = "wait";
  if (state.started.has(index)) status = "running";
  else if (result) status = result.cancelled ? "stopped" : !result.ok ? "fail" : result.warning_text ? "warn" : "done";
  else if (state.skipped.has(index)) status = "skip";
  row.className = "step is-" + status + (state.open.has(index) ? " is-open" : "");

  const dot = row.querySelector(".dot");
  dot.textContent = status === "done" ? "✓" : status === "fail" || status === "stopped" || status === "warn" ? "!" : String(index);

  const time = row.querySelector(".time");
  if (status === "running") time.textContent = seconds(Date.now() - state.started.get(index));
  else if (result) time.textContent = seconds(result.duration_ms || 0);
  else if (status === "skip") time.textContent = T("skipped", "Skipped");
  else time.textContent = "";

  row.querySelector(".play").disabled = !canRun();
  const head = row.querySelector(".step-head");
  head.setAttribute("aria-expanded", String(state.open.has(index)));
  const detail = row.querySelector(".step-detail");
  detail.hidden = !state.open.has(index);
  if (!detail.hidden) fillDetail(detail, step, result);
}

function fillDetail(detail, step, result) {
  detail.textContent = "";
  const args = step.args || {};
  const keys = Object.keys(args);
  if (keys.length) {
    const dl = el("dl", "args");
    for (const key of keys) {
      dl.appendChild(el("dt", "", key));
      const value = args[key];
      dl.appendChild(el("dd", "", typeof value === "string" ? value : JSON.stringify(value)));
    }
    detail.appendChild(dl);
  }
  if (step.checks && step.checks.length) {
    // A failed check is told in the failure box; after a pass, each one is marked as met.
    const checks = el("ul", "checks");
    for (const text of step.checks) checks.appendChild(el("li", result && result.ok ? "is-met" : "", text));
    detail.appendChild(checks);
  }
  if (result) {
    const problem = result.failure_text || result.warning_text;
    if (problem) {
      const box = el("div", "failure", problem);
      const more = result.failure_detail || result.warning_detail;
      if (more) box.appendChild(el("span", "failure-detail", more));
      detail.appendChild(box);
    }
    for (const line of result.result_lines || []) detail.appendChild(el("div", "result-line", line));
    if (result.thumbnail || (result.files && result.files.length)) {
      const files = el("div", "files");
      if (result.thumbnail && result.files && result.files.length) {
        const img = el("img", "thumb");
        img.src = result.thumbnail;
        img.alt = result.files[0].name;
        img.addEventListener("click", () => send("open_image", { path: result.files[0].path }));
        files.appendChild(img);
      }
      for (const file of result.files || []) {
        const link = el("button", "link", file.name);
        link.type = "button";
        link.title = T("show_in_folder", "Show in folder");
        link.addEventListener("click", () => send("open_location", { path: file.path }));
        files.appendChild(link);
      }
      detail.appendChild(files);
    }
  }
  const actions = el("div", "actions");
  const run = el("button", "btn small", "▷ " + T("run_this_step", "Run this step"));
  run.type = "button";
  run.disabled = !canRun();
  run.addEventListener("click", () => runStep(step.index));
  actions.appendChild(run);
  if (result && result.raw) {
    const raw = el("pre", "raw", JSON.stringify(result.raw, null, 2));
    raw.hidden = true;
    const toggle = el("button", "link", T("show_raw", "Show raw result"));
    toggle.type = "button";
    toggle.addEventListener("click", () => {
      raw.hidden = !raw.hidden;
      toggle.textContent = raw.hidden ? T("show_raw", "Show raw result") : T("hide_raw", "Hide raw result");
    });
    actions.appendChild(toggle);
    detail.appendChild(actions);
    detail.appendChild(raw);
  } else {
    detail.appendChild(actions);
  }
}

// ---- running ----------------------------------------------------------------------------------

function busy() { return state.pending || state.running; }

function canRun() { return state.ready && !busy() && !state.stale && state.parsed && state.parsed.ok; }

function requestRun(extra) {
  if (!canRun()) return;
  // Frozen at once: the host may ask about unsaved changes first, and the script must not change meanwhile.
  state.pending = true;
  send("run", Object.assign({ text: sourceText() }, extra));
  render();
}

function runAll() { requestRun({}); }

function runStep(index) { requestRun({ step: index }); }

function onRunStarted(msg) {
  state.pending = false;
  state.running = true;
  state.single = msg.step || null;
  state.total = state.parsed && state.parsed.ok ? state.parsed.steps.length : 0;
  state.message = null;
  if (state.single) {
    state.results.delete(state.single);
    state.skipped.delete(state.single);
  } else {
    state.results.clear();
    state.skipped.clear();
    state.outcome = null;
  }
  clearInterval(ticker);
  ticker = setInterval(() => state.started.forEach((_, index) => renderRow(index)), 200);
  render();
}

function onStepStarted(msg) {
  state.started.set(msg.index, Date.now());
  renderRow(msg.index);
  renderFooter();
  const row = rows.get(msg.index);
  if (row) row.scrollIntoView({ block: "nearest" });
}

function onStepFinished(msg) {
  state.started.delete(msg.index);
  state.results.set(msg.index, msg);
  if (!msg.ok || msg.warning_text) state.open.add(msg.index);
  renderRow(msg.index);
}

function onRunFinished(msg) {
  state.pending = false;
  state.running = false;
  state.started.clear();
  clearInterval(ticker);
  state.outDir = msg.out_dir || state.outDir;
  if (!msg.single) {
    state.hasRun = true;
    // A run that ended early leaves the steps after it unrun.
    if (!msg.passed && state.parsed && state.parsed.ok)
      for (const step of state.parsed.steps)
        if (!state.results.has(step.index)) state.skipped.add(step.index);
  }
  if (msg.outcome_text) state.outcome = { kind: msg.outcome_kind, text: msg.outcome_text, detail: msg.error_detail || "" };
  render();
}

function onRunRefused(msg) {
  state.pending = false;
  state.running = false;
  clearInterval(ticker);
  state.message = msg.message ? { kind: "fail", text: msg.message, detail: msg.detail || "" } : null;
  render();
}

// ---- footer and header --------------------------------------------------------------------------

function render() {
  const text = sourceText();
  const hasSteps = state.parsed && state.parsed.ok;
  $("empty").hidden = text.trim() !== "";
  $("steps").hidden = !hasSteps;
  renderChip();
  rows.forEach((_, index) => renderRow(index));
  renderSummary();
  renderFooter();

  for (const id of ["open-file", "paste", "empty-open", "empty-paste"]) $(id).disabled = busy();
  $("source").readOnly = busy();
}

function runningText() {
  const index = [...state.started.keys()].pop() || state.single || state.results.size + 1;
  return state.single ? T("running_one", "Running step %s", index) : T("running_step", "Running step %s of %s", index, state.total);
}

function renderSummary() {
  const summary = $("steps-summary");
  summary.classList.remove("is-error");
  if (state.running) summary.textContent = runningText();
  else if (state.outcome) summary.textContent = state.outcome.text;
  else if (state.parsed && state.parsed.ok) summary.textContent = state.parsed.count_text || "";
  else if (state.parsed && !state.parsed.empty && state.parsed.error) {
    summary.textContent = state.parsed.error;
    summary.classList.add("is-error");
  } else summary.textContent = "";
}

function renderFooter() {
  const outcome = $("outcome");
  const detail = $("outcome-detail");
  let shown = null;
  if (!state.ready) shown = { kind: "quiet", text: T("starting", "OrcaSlicer is still starting…"), detail: "" };
  else if (state.running) shown = { kind: "quiet", text: runningText(), detail: T("hands_off", "Don't use OrcaSlicer until the script finishes.") };
  else if (state.message) shown = state.message;
  else if (state.outcome) shown = state.outcome;
  outcome.className = shown ? "is-" + shown.kind : "";
  outcome.textContent = shown ? shown.text : "";
  detail.textContent = shown ? shown.detail : "";
  detail.title = detail.textContent;

  $("run").hidden = state.running;
  $("run").disabled = !canRun();
  $("run").textContent = state.hasRun ? T("run_again", "Run again") : T("run", "Run");
  $("stop").hidden = !state.running;
  $("open-output").hidden = state.running || !state.outDir;
  renderSummary();
}

// ---- host messages ------------------------------------------------------------------------------

function HandleStudio(msg) {
  switch (msg.command) {
  case "service":
    state.ready = !!msg.ready;
    render();
    break;
  case "source": onSource(msg); break;
  case "parsed": onParsed(msg); break;
  case "run_started": onRunStarted(msg); break;
  case "step_started": onStepStarted(msg); break;
  case "step_finished": onStepFinished(msg); break;
  case "run_finished": onRunFinished(msg); break;
  case "run_refused": onRunRefused(msg); break;
  case "error":
    state.pending = false;
    state.message = { kind: "fail", text: msg.message, detail: "" };
    render();
    break;
  }
}

function OnInit() {
  TranslatePage();
  $("open-file").addEventListener("click", () => send("open_file"));
  $("empty-open").addEventListener("click", () => send("open_file"));
  $("paste").addEventListener("click", () => send("paste"));
  $("empty-paste").addEventListener("click", () => send("paste"));
  $("run").addEventListener("click", runAll);
  $("stop").addEventListener("click", () => send("stop"));
  $("close").addEventListener("click", () => send("close_page"));
  $("open-output").addEventListener("click", () => send("open_output"));
  for (const id of ["steps-section", "source-section"])
    $(id).querySelector(".section-head").addEventListener("click", () => toggleSection(id));
  const source = $("source");
  source.addEventListener("input", onSourceInput);
  source.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      runAll();
    }
  });
  render();
  send("init");
}
