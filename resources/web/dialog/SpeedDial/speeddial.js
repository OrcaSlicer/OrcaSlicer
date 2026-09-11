// Speed Dial launcher page. Static-safe module: no DOM access at load time so a
// node vm can exercise the pure helpers (filterActions / actionLabel / nextSel).

// ---- state (populated by the C++ bridge via window.HandleStudio) ----
var ACTIONS = [];        // [{id,title,source,shortcut}], already frecency-sorted by C++
var FAVS = [];           // [id...]
var query = "";
var sel = { zone: "list", i: 0 };   // zone: 'list' | 'fav'
var lastResizeHeight = 0;
var matchIndex = {};

// why: fuzzy matcher (FoldChar/Norm/FuzzyRanges) lives in shared ../../js/fuzzy-search.js, loaded before
//      this script - it is shared with the Plugins dialog. Speed dial search is always case-insensitive.

// element handles, assigned in OnInit (kept null so load-time touches no DOM)
var qEl = null, listEl = null, favEl = null, clearEl = null, eyeEl = null, countEl = null;

// ---- pure helpers (no DOM; unit-tested) -------------------------------------
function filterActions(actions, query) {
  var q = (query || "").trim();
  var list = actions || [];
  matchIndex = {};
  if (!q)
    return list.slice(0);

  var out = [];
  for (var i = 0; i < list.length; i++) {
    var a = list[i];
    var titleMatch = FuzzyRanges(a.title, q, false);
    var sourceMatch = FuzzyRanges(a.source, q, false);
    if (!titleMatch && !sourceMatch)
      continue;
    matchIndex[a.id] = { title: titleMatch, source: sourceMatch, useTitle: !!titleMatch };
    out.push(a);
  }
  return out;
}

function visibleFavourites(favourites, actions) {
  // why: a fav whose id has no live action (plugin unloaded/disabled) renders a dead
  //      monogram tile whose click run()s to a silent no-op; drop it from the quick-bar.
  var seen = {};
  (actions || []).forEach(function (a) { seen[a.id] = true; });
  return (favourites || []).filter(function (id, i, arr) {
    return seen[id] && arr.indexOf(id) === i;
  });
}

function resultCountText(total, shown, query) {
  return (query || "").trim() ? "Showing " + shown + " of " + total + " actions" : total + " actions";
}

function shouldRenderActionList(query) {
  return !!(query || "").trim();
}

// Resolve the selection cursor {zone,i} to the action id it points at: fav zone indexes the
// visible favourites, list zone the filtered actions. Pure so runSelected() shares one lookup.
function selectedActionId(sel, actions, favIds, query) {
  if (sel.zone === "fav")
    return favIds[sel.i];
  // why: search-first keeps the list blank until the user types; an empty query still
  //      filters to ALL actions, so without this gate Enter fires an action never shown.
  if (!shouldRenderActionList(query))
    return null;
  var a = actions[sel.i];
  return a && a.id;
}

function foldLabel(s) { return String(s || "").toLowerCase().replace(/[^a-z0-9]+/g, ""); }

// Title-case a source for display: "GCODE OPTIMIZER"/"iRoNiNg pRo" -> "Gcode Optimizer"/"Ironing Pro".
function prettySource(source) {
  return String(source || "").toLowerCase().replace(/\b\w/g, function (c) { return c.toUpperCase(); });
}

// Accessible label "Title from Pretty Source", disambiguated with the opaque action id when another
// action shares the same title+source (case/separator-insensitive) - so two rows never read out identically.
function actionLabel(action, actions) {
  var label = action.title + " from " + prettySource(action.source);
  if (actions && actions.length) {
    var mine = foldLabel(action.title) + "|" + foldLabel(action.source);
    var clash = actions.some(function (o) {
      return o.id !== action.id && foldLabel(o.title) + "|" + foldLabel(o.source) === mine;
    });
    if (clash)
      label += " (" + action.id + ")";
  }
  return label;
}

// Monogram code for a tile: title initial, escalated on collision by PREPENDING the source
// initial (pi+ti, e.g. "GC"), then a 1-based ordinal - so same-titled actions stay distinct.
// why: ordinal is assigned by id, not by ACTIONS order - ACTIONS is frecency-sorted and
// reshuffles as usage changes, which would otherwise flip who's "1" and who's "2" across runs.
function tileCode(action, actions) {
  var list = actions || [];
  var ti = (action.title || " ").charAt(0).toUpperCase();
  var sameTitle = list.filter(function (o) { return (o.title || " ").charAt(0).toUpperCase() === ti; });
  if (sameTitle.length <= 1)
    return ti;
  var pi = (action.source || " ").charAt(0).toUpperCase();
  var sameSource = sameTitle.filter(function (o) { return (o.source || " ").charAt(0).toUpperCase() === pi; });
  if (sameSource.length <= 1)
    return pi + ti;
  sameSource.sort(function (a, b) { return a.id < b.id ? -1 : a.id > b.id ? 1 : 0; });
  for (var i = 0; i < sameSource.length; i++)
    if (sameSource[i].id === action.id)
      return pi + ti + (i + 1);
  return pi + ti;
}

function syncClearButton() {
  if (clearEl)
    clearEl.hidden = !query;
}

function stateFromPayload(payload) {
  return {
    actions: payload.actions || [],
    favourites: payload.favourites || [],
    query: "",
    sel: { zone: "list", i: 0 },
    lastResizeHeight: 0
  };
}

function resetScrollPositions(list, doc) {
  if (list)
    list.scrollTop = 0;
  if (doc && doc.scrollingElement)
    doc.scrollingElement.scrollTop = 0;
  if (doc && doc.documentElement)
    doc.documentElement.scrollTop = 0;
  if (doc && doc.body)
    doc.body.scrollTop = 0;
}

// nextSel: pure arrow-nav transition. Down fav->list0; Down list->clamp; Up list@0->fav0;
// Up list->i-1; Left/Right clamp within fav. Returns a fresh {zone,i}.
function nextSel(sel, key, listLen, favLen) {
  var zone = sel.zone, i = sel.i;
  if (key === "ArrowDown") {
    if (zone === "fav") return { zone: "list", i: 0 };
    return { zone: "list", i: Math.min(i + 1, Math.max(0, listLen - 1)) };
  }
  if (key === "ArrowUp") {
    if (zone === "list") {
      if (i <= 0) return favLen ? { zone: "fav", i: 0 } : { zone: "list", i: 0 };
      return { zone: "list", i: i - 1 };
    }
    return { zone: zone, i: i };
  }
  if (key === "ArrowLeft" && zone === "fav") return { zone: "fav", i: Math.max(0, i - 1) };
  if (key === "ArrowRight" && zone === "fav") return { zone: "fav", i: Math.min(favLen - 1, i + 1) };
  return { zone: zone, i: i };
}

// ---- bridge ------------------------------------------------------------------
function SendMessage(msg) {
  if (typeof SendWXMessage !== "function")
    return;
  if (typeof msg === "string") msg = { command: msg };
  if (msg.sequence_id === undefined) msg.sequence_id = Date.now();
  SendWXMessage(JSON.stringify(msg));
}

// C++ pushes payloads here. Only list_actions is handled; it (re)seeds all state.
window.HandleStudio = function (payload) {
  if (!payload) return;
  if (typeof payload === "string") { try { payload = JSON.parse(payload); } catch (e) { return; } }
  if (payload.command === "list_actions") {
    var next = stateFromPayload(payload);
    ACTIONS = next.actions;
    FAVS = next.favourites;
    query = next.query;
    sel = next.sel;
    lastResizeHeight = next.lastResizeHeight;
    if (qEl) {
      qEl.value = "";
      qEl.placeholder = "Search " + ACTIONS.length + " actions";
      syncClearButton();
    }
    render({ resize: true, resetScroll: true });
    focusInput();
  }
};

// ---- DOM helpers -------------------------------------------------------------
function $(id) { return document.getElementById(id); }

function byId(id) {
  for (var i = 0; i < ACTIONS.length; i++) if (ACTIONS[i].id === id) return ACTIONS[i];
  return null;
}

function currentVisibleFavs() { return visibleFavourites(FAVS, ACTIONS); }

function hue(id) {
  var h = 0;
  for (var i = 0; i < id.length; i++)
    h = (h * 31 + id.charCodeAt(i)) >>> 0;
  return h % 360;
}

// Build a <div class=className> with the search-match ranges wrapped in <mark>. Used for both the
// title and the source eyebrow. Pure (only touches the document factory), so the node-vm test never
// calls it and load-time stays DOM-free.
function markedText(className, text, match) {
  var node = document.createElement("div");
  node.className = className; node.title = text;
  if (!match || !match.length) {
    node.textContent = text;
    return node;
  }
  var last = 0;
  for (var i = 0; i < match.length; i++) {
    var range = match[i];
    if (range[0] > last)
      node.appendChild(document.createTextNode(text.slice(last, range[0])));
    var m = document.createElement("mark");
    m.textContent = text.slice(range[0], range[1]);
    node.appendChild(m);
    last = range[1];
  }
  if (last < text.length)
    node.appendChild(document.createTextNode(text.slice(last)));
  return node;
}

function starSvg(on) {
  return '<svg width="15" height="15" viewBox="0 0 24 24" fill="' + (on ? "currentColor" : "none") +
    '" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round">' +
    '<path d="M12 3.5l2.6 5.3 5.9.9-4.3 4.1 1 5.8L12 17.9 6.8 20.6l1-5.8L3.5 9.7l5.9-.9z"/></svg>';
}

// ---- render ------------------------------------------------------------------
function renderFav() {
  favEl.innerHTML = "";
  var favs = currentVisibleFavs();
  favEl.hidden = favs.length === 0;
  if (!favs.length && sel.zone === "fav")
    sel = { zone: "list", i: 0 };
  else if (sel.zone === "fav")
    sel.i = Math.max(0, Math.min(sel.i, favs.length - 1));
  updateFavEyebrow(favs);
  favs.forEach(function (id, i) {
    var a = byId(id);
    var tile = document.createElement("button");
    tile.className = "fav-tile" + (sel.zone === "fav" && sel.i === i ? " sel" : "");
    tile.style.setProperty("--h", hue(id));
    tile.textContent = tileCode(a, ACTIONS);
    tile.title = a.title;
    tile.setAttribute("aria-label", actionLabel(a, ACTIONS));
    tile.onclick = function () { sel = { zone: "fav", i: i }; run(a); };
    tile.oncontextmenu = function (ev) {
      ev.preventDefault();
      // why: selecting shows the eyebrow, which grows the launcher - resize so the popup
      //      isn't clipped (mirrors arrow-nav). requestResize no-ops when height is unchanged.
      sel = { zone: "fav", i: i }; render({ resize: true });
      showFavMenu(ev.clientX, ev.clientY, id);
    };
    favEl.appendChild(tile);
  });
}

// ---- favourite context menu (right-click a tile) -----------------------------
var favMenuEl = null;

function hideFavMenu() { if (favMenuEl) favMenuEl.hidden = true; }

function addFavMenuItem(label, enabled, fn) {
  var item = document.createElement("button");
  item.className = "ctx-item";
  item.textContent = label;
  item.disabled = !enabled;
  item.onclick = function () { hideFavMenu(); fn(); };
  favMenuEl.appendChild(item);
}

// One reused menu node (Move left/right + Unpin), positioned at the cursor and clamped
// to the viewport. Native browser context menus can't add items, so we roll our own tiny one.
function showFavMenu(x, y, id) {
  if (!favMenuEl) {
    favMenuEl = document.createElement("div");
    favMenuEl.className = "ctx-menu";
    document.body.appendChild(favMenuEl);
  }
  favMenuEl.innerHTML = "";
  var favs = currentVisibleFavs();
  var vi = favs.indexOf(id);
  addFavMenuItem("Move left", vi > 0, function () { moveFav(id, -1); });
  addFavMenuItem("Move right", vi >= 0 && vi < favs.length - 1, function () { moveFav(id, 1); });
  addFavMenuItem("Unpin", true, function () { toggleFav(id); });
  favMenuEl.hidden = false;
  favMenuEl.style.left = Math.max(0, Math.min(x, window.innerWidth - favMenuEl.offsetWidth - 4)) + "px";
  favMenuEl.style.top = Math.max(0, Math.min(y, window.innerHeight - favMenuEl.offsetHeight - 4)) + "px";
}

// Swap a favourite with its visible neighbour (dir -1/+1) and persist the new order. Swapping
// by id inside FAVS (not the visible slice) keeps any hidden pins (no live action) in place.
function moveFav(id, dir) {
  var favs = currentVisibleFavs();
  var vi = favs.indexOf(id);
  var ni = vi + dir;
  if (vi === -1 || ni < 0 || ni >= favs.length) return;
  var a = FAVS.indexOf(id), b = FAVS.indexOf(favs[ni]);
  if (a === -1 || b === -1) return;
  FAVS[a] = favs[ni]; FAVS[b] = id;
  SendMessage({ command: "reorder_favourites", ids: FAVS.slice() });
  sel = { zone: "fav", i: ni };
  render({ resize: true });
}

// Name of the selected favourite, shown above the bar; hidden unless a fav is selected.
function updateFavEyebrow(favs) {
  if (!eyeEl) return;
  var a = sel.zone === "fav" && favs.length ? byId(favs[sel.i]) : null;
  eyeEl.textContent = a ? a.title : "";
  eyeEl.hidden = !a;
}

function renderList() {
  var q = (query || "").trim();
  var arr = filterActions(ACTIONS, query);
  if (sel.zone === "list")
    sel.i = Math.max(0, Math.min(sel.i, arr.length - 1));
  listEl.innerHTML = "";
  // why: search-first - the list stays blank until the user types; only a
  // non-empty query with zero hits earns the "No actions match" message.
  if (!shouldRenderActionList(query)) {
    listEl.className = "dial-list empty";
    if (countEl) countEl.hidden = true;
    return;
  }
  listEl.className = "dial-list" + (!arr.length ? " empty" : "");
  if (!arr.length) {
    if (countEl) countEl.hidden = true;
    var empty = document.createElement("div");
    empty.className = "dial-empty";
    empty.textContent = "No actions match (Total: " + ACTIONS.length + ")";
    listEl.appendChild(empty);
    return;
  }
  if (countEl) {
    countEl.hidden = false;
    countEl.textContent = resultCountText(ACTIONS.length, arr.length, query);
  }
  arr.forEach(function (a, i) {
    var on = FAVS.indexOf(a.id) !== -1;
    var row = document.createElement("div");
    row.className = "row" + (sel.zone === "list" && sel.i === i ? " sel" : "");
    row.setAttribute("aria-label", actionLabel(a, ACTIONS));

    var tile = document.createElement("div");
    tile.className = "tile";
    tile.style.setProperty("--h", hue(a.id));
    tile.textContent = tileCode(a, ACTIONS);

    var left = document.createElement("div");
    left.className = "row-left";
    var mi = matchIndex[a.id];
    var sourceEl = markedText("row-eyebrow", a.source, mi ? mi.source : null);
    var line = document.createElement("div");
    line.className = "row-line";
    var name = markedText("row-name", a.title, mi ? mi.title : null);
    line.appendChild(name);
    if (a.shortcut) {
      var sc = document.createElement("div");
      sc.className = "row-sc";
      a.shortcut.split("+").forEach(function (k) {
        var key = document.createElement("kbd");
        key.textContent = k;
        sc.appendChild(key);
      });
      line.appendChild(sc);
    }
    left.appendChild(sourceEl);
    left.appendChild(line);
    row.appendChild(tile);
    row.appendChild(left);

    var star = document.createElement("button");
    star.className = "star" + (on ? " on" : "");
    star.innerHTML = starSvg(on);
    star.title = on ? "Unpin from favourites" : "Pin to favourites";
    star.onclick = function (ev) { ev.stopPropagation(); toggleFav(a.id); };
    // why: two quick fav/unfav clicks must not dblclick-run the row
    star.ondblclick = function (ev) { ev.stopPropagation(); };
    row.appendChild(star);

    row.onclick = function () { sel = { zone: "list", i: i }; render(); };
    row.ondblclick = function () { sel = { zone: "list", i: i }; run(a); };
    listEl.appendChild(row);
  });
}

function render(opts) {
  renderFav();
  renderList();
  scrollSelectedIntoView();
  if (opts && opts.resetScroll)
    resetScrollPositions(listEl, document);
  if (opts && opts.resize)
    requestResize();
}

// Keep the selected item in view as arrows move it: the list scrolls vertically, the fav bar
// horizontally (arrow nav "pushes" the scrollable fav row to follow the selection).
function scrollSelectedIntoView() {
  var el = null;
  if (sel.zone === "list" && listEl)
    el = listEl.querySelector(".row.sel");
  else if (sel.zone === "fav" && favEl)
    el = favEl.querySelector(".fav-tile.sel");
  if (el && el.scrollIntoView)
    el.scrollIntoView({ block: "nearest", inline: "nearest" });
}

function requestResize() {
  if (!document.body)
    return;
  setTimeout(function () {
    var launcher = document.querySelector(".launcher");
    if (!launcher)
      return;
    var height = Math.ceil(launcher.getBoundingClientRect().height);
    if (!height || height === lastResizeHeight)
      return;
    lastResizeHeight = height;
    SendMessage({ command: "resize", height: height });
  }, 0);
}

// ---- actions -----------------------------------------------------------------
function toggleFav(id) {
  var k = FAVS.indexOf(id);
  var newState = k === -1;
  if (newState) FAVS.push(id); else FAVS.splice(k, 1);
  SendMessage({ command: "toggle_favourite", id: id, fav: newState });
  render({ resize: true });
}

// Fire the action; C++ owns the run-confirm (native dialog) + suppression, then closes the popup + toasts.
function run(a) {
  if (!a) return;
  SendMessage({ command: "run_action", id: a.id, title: a.title });
}

function runSelected() {
  var id = selectedActionId(sel, filterActions(ACTIONS, query), currentVisibleFavs(), query);
  if (id) run(byId(id));
}

function focusInput() { setTimeout(function () { qEl.focus(); }, 0); }

// ---- init --------------------------------------------------------------------
function OnInit() {
  qEl = $("q"); listEl = $("list"); favEl = $("favBar"); clearEl = $("clear"); eyeEl = $("favEyebrow"); countEl = $("count");
  syncClearButton();

  $("clear").onclick = function () {
    query = ""; qEl.value = ""; sel = { zone: "list", i: 0 }; render({ resize: true, resetScroll: true }); qEl.focus();
    syncClearButton();
  };
  qEl.addEventListener("input", function () {
    query = qEl.value; sel = { zone: "list", i: 0 }; syncClearButton(); render({ resize: true, resetScroll: true });
  });

  // why: dismiss the fav context menu on any click/scroll away from it (capture scroll to catch nested scrollers).
  document.addEventListener("click", hideFavMenu);
  document.addEventListener("scroll", hideFavMenu, true);

  document.addEventListener("keydown", function (e) {
    if (favMenuEl && !favMenuEl.hidden && e.key === "Escape") { e.preventDefault(); hideFavMenu(); return; }
    var arr = filterActions(ACTIONS, query);
    var favs = currentVisibleFavs();
    // why: Up/Down always navigate; Left/Right only navigate the fav bar. In the list zone,
    //      let Left/Right fall through so they move the caret in the focused search field.
    var lr = e.key === "ArrowLeft" || e.key === "ArrowRight";
    if (e.key === "ArrowDown" || e.key === "ArrowUp" || (lr && sel.zone === "fav")) {
      e.preventDefault();
      sel = nextSel(sel, e.key, arr.length, favs.length);
      // why: entering/leaving the fav zone toggles the eyebrow line, changing launcher height;
      // resize so the popup grows/shrinks instead of clipping. requestResize no-ops when unchanged.
      render({ resize: true });
    } else if (e.key === "Enter") {
      e.preventDefault();
      runSelected();
    } else if (e.key === "Escape") {
      e.preventDefault();
      if (query) { query = ""; qEl.value = ""; sel = { zone: "list", i: 0 }; syncClearButton(); render({ resize: true, resetScroll: true }); }
      else SendMessage({ command: "close_page" });
    }
  });

  SendMessage({ command: "request_actions" });
}
