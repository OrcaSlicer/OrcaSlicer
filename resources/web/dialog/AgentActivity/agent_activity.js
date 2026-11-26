// The Agent Activity pane's page. The host (AgentActivityPanel) sends the activity log's entries as they change, with
// their titles, targets and source labels in the UI's language; this page draws them as a step rail, filters them, and
// asks the host to clear the list or copy a request or response.

const M = AgentActivityModel;
const MASK = "\u2022\u2022\u2022";

const S = {
  store: M.createStore(),
  open: new Set(),       // the entries the person opened
  show: "all",           // all | changes | failures
  source: "all",         // a sourceKey(), or all
  find: "",
  following: true,       // the list follows the newest call until the person scrolls up
  newBelow: 0,           // calls that came while the person read further up
  tree: { top: [], children: new Map() },
};
const rows = new Map();  // entry id -> its <li>

const ICON = {
  agent: '<svg viewBox="0 0 16 16"><path d="M8 1.5c.5 3.2 1.8 5 5.5 6.5-3.7 1.5-5 3.3-5.5 6.5-.5-3.2-1.8-5-5.5-6.5C6.2 6.5 7.5 4.7 8 1.5z" fill="currentColor"/></svg>',
  action_replay: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M3.4 8.6A4.7 4.7 0 1 0 5 4.4"/><path d="M2.8 2.2v3.3h3.3"/></svg>',
  run_script: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><rect x="1.5" y="2.5" width="13" height="11" rx="2"/><path d="M4.5 6.5 6.5 8l-2 1.5M8.2 10h3.3"/></svg>',
  command_line: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M2.5 4.5 6 8l-3.5 3.5M7.5 12h6"/></svg>',
  plugin: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"><path d="M6.2 2.6a1.6 1.6 0 0 1 3.2 0V4h3.1v3.1h-1.3a1.6 1.6 0 0 0 0 3.2h1.3v3.2H3.3v-3.2h1.2a1.6 1.6 0 0 0 0-3.2H3.3V4h2.9z"/></svg>',
  dialog: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"><path d="M1.8 2.2h8.4v5.6H5.4L3 9.8V7.8H1.8z"/></svg>',
};

function $(id) { return document.getElementById(id); }

function send(command, extra) { SendWXMessage(JSON.stringify(Object.assign({ command }, extra || {}))); }

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

// An icon from the fixed set above, never from an entry.
function icon(name) {
  const span = el("span", "icon");
  span.innerHTML = ICON[name] || ICON.agent;
  return span;
}

// ---- JSON, laid out as the Run Script console lays out a result ---------------------------------------------------

function jsonBlock(value) {
  const pre = el("pre", "raw");
  if (M.truncated(value)) {
    pre.className = "raw is-note";
    pre.textContent = M.tr("too_large", "Too large to show here (%s bytes).", value.bytes);
    return pre;
  }
  const shown = RunScriptModel.formatJson(JSON.stringify(value));
  const tokens = RunScriptModel.jsonTokens(shown);
  if (!tokens) {
    pre.textContent = shown;
    return pre;
  }
  for (const token of tokens) {
    if (token.type === "space") pre.append(document.createTextNode(token.text));
    else pre.append(el("span", "j-" + token.type + (token.text === '"' + MASK + '"' ? " mask" : ""), token.text));
  }
  return pre;
}

// ---- a step -------------------------------------------------------------------------------------------------------

function makeRow(entry) {
  const row = el("li", "step");
  row.dataset.id = entry.id;
  row.append(el("span", "dot"));
  const main = el("div", "step-main");
  const head = el("button", "step-head");
  head.type = "button";
  head.setAttribute("aria-expanded", "false");
  head.append(el("span", "step-title"), el("span", "step-what"));
  head.addEventListener("click", () => toggle(entry.id));
  const nested = el("ol", "rail nested");
  nested.hidden = true;
  const detail = el("div", "step-detail");
  detail.hidden = true;
  main.append(head, el("div", "step-tags"), el("div", "failure"), nested, detail);
  row.append(main, el("div", "step-side"));
  return row;
}

function nestedOf(row) { return row.querySelector(":scope > .step-main > .rail.nested"); }

function anyRunning(entry) {
  return entry.status === "running" || (S.tree.children.get(entry.id) || []).some(anyRunning);
}

function renderRow(entry) {
  const row = rows.get(entry.id);
  const open = S.open.has(entry.id);
  const children = S.tree.children.get(entry.id) || [];
  const status = { running: "is-running", done: "is-done", failed: "is-fail" }[entry.status] || "is-done";
  row.className = "step " + status + (entry.status !== "running" ? " is-ended" : "") + (entry.read ? " is-read" : "") +
                  (entry.kind === "dialog" ? " is-dialog" : "") + (open ? " is-open" : "") + (row.classList.contains("is-hidden") ? " is-hidden" : "") +
                  (row.classList.contains("is-last") ? " is-last" : "");

  const dot = row.querySelector(":scope > .dot");
  dot.textContent = "";
  if (entry.kind === "dialog") dot.append(icon("dialog"));
  else dot.textContent = entry.number ? String(entry.number) : "";
  dot.title = entry.kind === "dialog" ? M.tr("dialog_answered", "A dialog a rule answered")
            : entry.read ? M.tr("read_call", "A call that only reads") : M.tr("change_call", "A call that changes something");

  const head = row.querySelector(":scope > .step-main > .step-head");
  head.setAttribute("aria-expanded", String(open));
  head.querySelector(".step-title").textContent = entry.title || entry.tool || "";
  const what = head.querySelector(".step-what");
  what.textContent = entry.what || "";
  if (entry.kind === "dialog" && entry.request && entry.request.button) what.append(el("span", "pressed", entry.request.button));
  if (entry.status === "running" && entry.job && !entry.read)
    what.append(el("span", "res is-run", (entry.what ? " " : "") + M.tr("running_as_job", "running as job %s", entry.job)));

  const tags = row.querySelector(":scope > .step-main > .step-tags");
  tags.textContent = "";
  // What a call does beyond the open project, labelled by the host as Action Replay's steps are ("flag_" + id).
  for (const id of entry.flags || []) tags.append(el("span", "tag", M.tr("flag_" + id, id)));
  tags.hidden = !tags.childNodes.length;

  const failure = row.querySelector(":scope > .step-main > .failure");
  failure.hidden = entry.status !== "failed" || !entry.error;
  failure.textContent = entry.status === "failed" ? (entry.error || "").split("\n")[0] : "";

  // A run's own steps, while it runs or once opened.
  const nested = nestedOf(row);
  nested.hidden = !children.length || !(open || anyRunning(entry));

  const detail = row.querySelector(":scope > .step-main > .step-detail");
  detail.hidden = !open;
  if (open) fillDetail(detail, entry);

  const side = row.querySelector(":scope > .step-side");
  side.textContent = "";
  if (!entry.parent || !S.store.has(entry.parent)) {
    const src = el("span", "src");
    src.title = entry.source_label || "";
    src.append(el("span", "src-name", entry.source_label || ""), icon(entry.source));
    side.append(src, el("span", "clock", M.clock(entry.started)));
  }
  side.append(el("span", "time", entry.kind === "dialog" ? "" : M.duration(M.elapsed(entry, Date.now()))));
}

function fillDetail(detail, entry) {
  detail.textContent = "";
  if (entry.kind === "dialog") {
    const d = entry.request || {};
    detail.append(el("div", "kv-head", M.tr("dialog", "Dialog")));
    const dl = el("dl", "args plain");
    for (const [key, fallback, value] of [["dialog_title", "Title", d.title], ["dialog_message", "Message", d.message], ["pressed", "Pressed", d.button]])
      if (value) dl.append(el("dt", "", M.tr(key, fallback)), el("dd", "", value));
    detail.append(dl);
    const rule = M.ruleSetter(entry, S.store);
    const ruleHead = el("div", "kv-head", M.tr("rule", "Rule"));
    if (rule) {
      ruleHead.append(el("span", "meta", M.tr("set_by", "set by call %s, %s", rule.number, rule.title || rule.tool)));
      const show = el("button", "link", M.tr("show_call", "Show that call"));
      show.type = "button";
      show.addEventListener("click", () => reveal(rule.id));
      ruleHead.append(show);
    } else ruleHead.append(el("span", "meta", M.tr("rule_gone", "the call that set it isn't listed")));
    detail.append(ruleHead);
    const rl = el("dl", "args");
    // The rule's own arguments, by their names.
    rl.append(el("dt", "", "match"), el("dd", "", d.match || ""), el("dt", "", "button"), el("dd", "", d.button || ""));
    detail.append(rl);
    return;
  }
  const parent = S.store.get(entry.parent);
  detail.append(section(M.tr("request", "Request"), M.requestNote(entry, parent && (parent.title || parent.tool)), entry.request,
                        M.tr("copy_request", "Copy request")));
  const state = M.responseState(entry);
  detail.append(section(M.tr("response", "Response"), M.responseNote(entry, Date.now()), state === "value" ? entry.response : undefined,
                        M.tr("copy_response", "Copy response"), state === "withheld"));
  // The files its result names, as Action Replay lists a step's: a screenshot opens in the system's viewer, and each
  // file shows in its folder. The host finds a file by the entry and its place in the list.
  const files = entry.files || [];
  if (files.length) {
    const box = el("div", "files");
    if (entry.thumb) {
      const img = el("img", "thumb");
      img.src = entry.thumb;
      img.alt = files[0];
      img.addEventListener("click", () => send("open_file", { id: entry.id, index: 0 }));
      box.append(img);
    }
    files.forEach((name, index) => {
      const link = el("button", "link", name);
      link.type = "button";
      link.title = M.tr("show_in_folder", "Show in folder");
      link.addEventListener("click", () => send("show_file", { id: entry.id, index }));
      box.append(link);
    });
    detail.append(box);
  }
}

// A titled JSON block with its note and a Copy link; "Nothing yet." while a call has no response, and a note in place of
// a response the host withheld.
function section(title, note, value, copyLabel, withheld) {
  const wrap = el("div", "kv");
  const head = el("div", "kv-head", title);
  head.append(el("span", "meta", note));
  if (value !== undefined && !M.truncated(value)) {
    const copy = el("button", "link", copyLabel);
    copy.type = "button";
    copy.addEventListener("click", (e) => {
      e.stopPropagation();
      send("copy", { text: JSON.stringify(value, null, 2) });
      copy.textContent = M.tr("copied", "Copied");
      setTimeout(() => { copy.textContent = copyLabel; }, 1200);
    });
    head.append(copy);
  }
  wrap.append(head, withheld ? el("p", "none", M.tr("withheld", "Can't be shown: it repeats a call whose record the pane no longer has."))
                  : value === undefined ? el("p", "none", M.tr("nothing_yet_response", "Nothing yet."))
                  : jsonBlock(value));
  return wrap;
}

function toggle(id) {
  if (S.open.has(id)) S.open.delete(id);
  else S.open.add(id);
  const entry = S.store.get(id);
  if (entry) renderRow(entry);
  refresh();
}

// Opens an entry and scrolls to it: the call a dialog's rule came from.
function reveal(id) {
  const entry = S.store.get(id);
  if (!entry) return;
  S.open.add(id);
  const above = ancestors(entry);
  for (const up of above) S.open.add(up.id);
  for (const shown of [entry].concat(above)) renderRow(shown);
  refresh();
  S.following = false;
  const row = rows.get(id);
  const list = $("list");
  if (row) list.scrollTop = row.getBoundingClientRect().top - list.getBoundingClientRect().top + list.scrollTop - 8;
}

function ancestors(entry) {
  const out = [];
  for (let up = S.store.get(entry.parent); up; up = S.store.get(up.parent)) out.push(up);
  return out;
}

function atBottom(list) { return list.scrollHeight - list.scrollTop - list.clientHeight < 30; }

// ---- the list -----------------------------------------------------------------------------------------------------

function onEntries(msg) {
  const list = $("list");
  const wasAtBottom = atBottom(list);
  const before = S.tree.top.length;
  if (msg.reset) {
    rows.clear();
    $("rail").textContent = "";
    S.open.clear();
  }
  for (const id of msg.removed || []) {
    const row = rows.get(id);
    if (row) row.remove();
    rows.delete(id);
    S.open.delete(id);
  }
  M.apply(S.store, msg);
  S.tree = M.tree(S.store);
  // A new row goes last under its parent, or at the end of the list: ids only grow.
  const place = (entry) => {
    if (!rows.has(entry.id)) rows.set(entry.id, makeRow(entry));
    const row = rows.get(entry.id);
    const host = entry.parent && rows.has(entry.parent) ? nestedOf(rows.get(entry.parent)) : $("rail");
    if (row.parentNode !== host) host.append(row);
  };
  const changed = new Set((msg.entries || []).map((e) => e.id));
  const walk = (entries) => {
    for (const entry of entries) {
      if (msg.reset || changed.has(entry.id) || !rows.has(entry.id)) place(entry);
      walk(S.tree.children.get(entry.id) || []);
    }
  };
  walk(S.tree.top);
  // Each changed entry and, once each, the runs above them, whose running state and counts follow their steps.
  const render = new Set();
  for (const id of changed) {
    const entry = S.store.get(id);
    if (!entry) continue;
    render.add(entry);
    for (const up of ancestors(entry)) render.add(up);
  }
  for (const entry of render) renderRow(entry);
  const following = S.following || wasAtBottom || msg.reset;
  S.newBelow = following ? 0 : S.newBelow + Math.max(0, S.tree.top.length - before);
  refresh();
  if (following) list.scrollTop = list.scrollHeight;
}

// Which rows show, the thread's end, the source names that repeat, the toolbar.
function refresh() {
  const { shown, last } = M.visibility(S.tree, { show: S.show, source: S.source, find: S.find });
  for (const [id, row] of rows) {
    row.classList.toggle("is-hidden", !shown.has(id));
    row.classList.toggle("is-last", last.has(id));
  }
  let previous = null;
  for (const entry of S.tree.top) {
    if (!shown.has(entry.id)) continue;
    const src = rows.has(entry.id) ? rows.get(entry.id).querySelector(":scope > .step-side .src") : null;
    if (src) src.classList.toggle("same", entry.source_label === previous);
    previous = entry.source_label;
  }
  const empty = $("empty");
  empty.hidden = S.tree.top.some((e) => shown.has(e.id));
  empty.textContent = S.tree.top.length ? M.tr("no_match", "No call matches.")
                                        : M.tr("empty", "Calls from agents, Action Replay, Run Script and plugins appear here as they run.");
  $("latest").hidden = !S.newBelow;
  $("latest-n").textContent = M.tr("new_calls", "New calls: %s", S.newBelow);
  renderToolbar();
}

function renderToolbar() {
  document.querySelectorAll(".seg button").forEach((b) => b.setAttribute("aria-pressed", String(b.dataset.show === S.show)));
  const sources = M.sources(S.tree.top);
  const picked = sources.find((s) => s.key === S.source);
  if (S.source !== "all" && !picked) S.source = "all";
  $("src-label").textContent = picked ? picked.label : M.tr("everyone", "Everyone");

  const status = M.status(S.tree.top);
  const counts = M.counts(S.tree.top);
  const box = $("status");
  box.textContent = "";
  const who = el("span", status.busy ? "is-busy" : "");
  who.append(el("span", "sdot"), el("span", "who", status.text));
  box.append(who, el("span", "counts", M.tr("calls", "Calls: %s", counts.calls)));
  if (counts.failed) {
    const failed = el("button", "nfail", M.tr("failed", "Failed: %s", counts.failed));
    failed.type = "button";
    failed.addEventListener("click", () => setShow("failures"));
    box.append(failed);
  }
}

function setShow(show) {
  S.show = show;
  refresh();
}

function renderSourceMenu() {
  const menu = $("src-menu");
  menu.textContent = "";
  const items = [{ key: "all", label: M.tr("everyone", "Everyone"), count: S.tree.top.length }].concat(M.sources(S.tree.top));
  for (const item of items) {
    const b = el("button");
    b.type = "button";
    b.setAttribute("role", "menuitemradio");
    b.setAttribute("aria-checked", String(S.source === item.key));
    b.append(el("span", "tick", S.source === item.key ? "✓" : ""));
    if (item.source) b.append(icon(item.source));
    b.append(el("span", "label", item.label), el("span", "mc", String(item.count)));
    b.addEventListener("click", () => {
      S.source = item.key;
      closeSourceMenu();
      refresh();
    });
    menu.append(b);
  }
}

function closeSourceMenu() {
  $("src-menu").hidden = true;
  $("src-btn").setAttribute("aria-expanded", "false");
}

// The running calls' durations, four times a second.
function tick() {
  const now = Date.now();
  for (const [id, row] of rows) {
    const entry = S.store.get(id);
    if (!entry || entry.status !== "running") continue;
    const time = row.querySelector(":scope > .step-side .time");
    if (time) time.textContent = M.duration(now - entry.started);
  }
}

// ---- host messages ------------------------------------------------------------------------------------------------

function HandleStudio(msg) {
  switch (msg.command) {
  case "entries": onEntries(msg); break;
  }
}

function OnInit() {
  TranslatePage();
  $("clear").addEventListener("click", () => send("clear"));
  document.querySelectorAll(".seg button").forEach((b) => b.addEventListener("click", () => setShow(b.dataset.show)));
  $("src-btn").addEventListener("click", (e) => {
    e.stopPropagation();
    const menu = $("src-menu");
    if (!menu.hidden) return closeSourceMenu();
    renderSourceMenu();
    menu.hidden = false;
    $("src-btn").setAttribute("aria-expanded", "true");
  });
  document.addEventListener("click", (e) => { if (!$("src-menu").contains(e.target)) closeSourceMenu(); });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") closeSourceMenu(); });
  $("find").addEventListener("input", () => {
    S.find = $("find").value;
    refresh();
  });
  const list = $("list");
  list.addEventListener("scroll", () => {
    S.following = atBottom(list);
    if (S.following && S.newBelow) {
      S.newBelow = 0;
      $("latest").hidden = true;
    }
  });
  $("latest").addEventListener("click", () => {
    S.following = true;
    S.newBelow = 0;
    list.scrollTop = list.scrollHeight;
    $("latest").hidden = true;
  });
  setInterval(tick, 250);
  refresh();
  send("ready");
}
