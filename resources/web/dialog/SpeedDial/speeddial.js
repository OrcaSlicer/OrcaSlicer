// Speed Dial launcher page. Static-safe module: no DOM access at load time so a
// node vm can exercise the pure helpers (searchActions / filterTabs / actionLabel / nextSel /
// commandList / commandSections / actionCategory / groupActions / favDigitFromEvent / spacerHeight).
//
// Cross-boundary contracts (keep in sync; the C++ side pins its half in tests):
//   - favourite cap 10            -> ActionRegistry::kFavLimit (K_FAV_LIMIT here)
//   - mode rank simple<advanced<expert<develop -> ConfigOptionMode order (MODE_RANK here)
//   - action.mode token           -> ActionRegistry::mode_key / SpeedDialDialog::mode_label
//   - action.input "percent"/"tab" -> NativeCommands catalog (phases handled in activateEntry)
//   - action.icon SVG base name   -> AppAction::icon / resources/images/<name>.svg
//   - action.desc/wiki            -> AppAction::tooltip / help_url (footer detail strip)
//   - action list is frecency-sorted -> ActionRegistry::snapshot()

// ---- state (populated by the C++ bridge via window.HandleStudio) ----
var ACTIONS = [];        // [{id,title,source,group,kind,input,icon,mode}], already frecency-sorted by C++
var FAVS = [];           // [id...]
var RECENTS = [];        // [{id,title,source,group,kind,input,icon,mode}] - last-N launched
var query = "";
var sel = { zone: "list", i: 0 };   // zone: 'list' | 'fav'
var lastResizeHeight = 0;
var matchIndex = {};

// The user's current settings mode (from the C++ payload) plus the rank order of the modes. Each
// action carries the mode it requires, so "would this need a switch?" is a rank comparison.
var USER_MODE = "simple";
var MODE_RANK = { simple: 0, advanced: 1, expert: 2, develop: 3 };

// Search ranking weights: every contiguous match must outrank every fuzzy one regardless of field,
// and title must outrank group, which outranks source.
var SCORE_CONTIGUOUS = 100000;
var SCORE_TITLE = 2000;
var SCORE_GROUP = 1000;

// Localized lookup for strings this page builds at runtime. text.js (loaded before this script)
// defines LangText; a missing entry falls back to the English literal. Extra args replace
// successive %s placeholders.
function T(key, fallback) {
    var table = (typeof LangText !== "undefined" && LangText) || null;
    var lang = "en";
    try { lang = localStorage.getItem(LANG_COOKIE_NAME) || "en"; } catch (e) {}
    var s = table && table[lang] && table[lang][key] !== undefined ? table[lang][key] :
        table && table.en && table.en[key] !== undefined ? table.en[key] : fallback;
    for (var i = 2; i < arguments.length; i++)
        s = s.replace("%s", arguments[i]);
    return s;
}

// ---- windowed list render ----------------------------------------------------
// The command list is rendered in windows (append-on-scroll) so a huge settings pool doesn't build
// the whole DOM per keystroke. Rows are exactly ROW_H tall (matches .row min-height 44px; see --row-h,
// which is documented to stay in sync). `renderEnd` is the exclusive count of rows currently in the DOM;
// a bottom spacer fills the rest of the list so the scrollbar reflects the full match count and
// "scroll past the last rendered row" reveals the next window.
var K_ROWS = 50;
var ROW_H = 44;
var renderEnd = 0;
var builtKey = "";   // phase|query|total - when it changes, rows are rebuilt from the first window [0, K_ROWS)
var spacerEl = null; // the trailing height spacer, always the last child of listEl

// Section headers in the empty-query list ("Recent" then one per category). sectionStarts maps a flat
// action index to the header label that sits above it. sectionTotal/Rendered count headers so the
// bottom spacer reserves the same vertical space the not-yet-rendered headers will occupy.
var SECTION_H = 30; // MUST match .dial-section height (30px)
var sectionStarts = null;
var sectionTotal = 0;
var sectionRendered = 0;

// search-cache: the normalized (folded+lowercased) needle for the current query pass.
var searchNeedle = "";

// Palette phase: 'commands' (one unified search over actions/commands/settings, recents on empty
// query), 'percent' ("Go to layer" second phase: enter a 0-100 percentage), 'tab' ("Go to tab..."
// second phase: pick a notebook tab).
var phase = "commands";
var tabOptions = [];       // [{id,title}] - notebook pages, fetched on entering the tab phase

// why: the fuzzy matcher (NormText/FuzzyRangesNorm) lives in shared
//      ../../js/fuzzy-search.js, loaded before this script. Search is always case-insensitive.

// element handles, assigned in OnInit (kept null so load-time touches no DOM)
var qEl = null, listEl = null, favEl = null, clearEl = null, eyeEl = null, countEl = null, detailEl = null;

// ---- pure helpers (no DOM; unit-tested) -------------------------------------
// Pre-normalized haystacks, cached on the action object. The fold is length-preserving (1:1 per
// char) so the ranges FuzzyRangesNorm returns slice the ORIGINAL title/group/source text correctly.
// The action objects arrive from C++ and are stable for the dialog's lifetime, so we compute these once.
function titleNorm(a) {
    if (a._tn === undefined)
        a._tn = NormText(a.title, false);
    return a._tn;
}
// The eyebrow (header) line shows group when present, else source. Settings keep an empty group so
// their source path is the eyebrow; commands / plates / recents carry a non-empty group. Splitting the
// two lets a match range stay aligned to whichever string the eyebrow actually renders.
function groupNorm(a) {
    if (a._gn === undefined)
        a._gn = NormText(a.group || "", false);
    return a._gn;
}
function sourceNorm(a) {
    if (a._sn === undefined)
        a._sn = NormText(a.source || "", false);
    return a._sn;
}

// Match one pre-normalized field vs the current needle. Returns {score, ranges, contiguous} when the
// needle is present, else null. wwRe is a compiled whole-word (\b-bounded) regex for the current needle.
// A whole-word hit is preferred - it highlights the full word (e.g. "orient" in "Auto-Orient", not the
// stray "o" of "Auto") and marks a perfect match. Otherwise FuzzyRangesNorm (which now prefers the
// most-contiguous run) is used. score is higher for an earlier start and fewer gaps; contiguous marks
// a perfect match - the whole needle landed as one unbroken run.
function fieldMatchScore(norm, wwRe) {
    if (!searchNeedle) return null;
    if (wwRe) {
        var m = wwRe.exec(norm || "");
        if (m)
            return { score: 1000 - m.index * 10, ranges: [[m.index, m.index + m[0].length]], contiguous: true };
    }
    var r = FuzzyRangesNorm(norm || "", searchNeedle);
    if (!r) return null;
    var gaps = 0, len = 0;
    for (var i = 0; i < r.length; i++) {
        if (i > 0)
            gaps += r[i][0] - r[i - 1][1];
        len += r[i][1] - r[i][0];
    }
    return { score: 1000 - r[0][0] * 10 - gaps * 10, ranges: r, contiguous: r.length === 1 && len === searchNeedle.length };
}

// Combine the per-field match scores into one comparable value, or null when no field matched.
// Ranking tiers, strongest first:
//   tier (contiguous/perfect vs fuzzy) > field (title > group > source) > start/gaps.
// The additive weights keep every contiguous match above every fuzzy one regardless of field.
function scoreFields(t, g, s) {
    var best = null;
    function consider(m, weight) {
        if (!m) return;
        var v = (m.contiguous ? SCORE_CONTIGUOUS : 0) + weight + m.score;
        best = best === null ? v : Math.max(best, v);
    }
    consider(t, SCORE_TITLE);
    consider(g, SCORE_GROUP);
    consider(s, 0);
    return best;
}

// The unified main-phase search: every action (command/plugin/setting) matching the query, ranked
// by relevance (not by action type). Sets matchIndex so rows highlight their match ranges. The query
// is normalized ONCE per pass - FuzzyRangesNorm then runs against each action's pre-normalized
// haystack, so per-keystroke cost is a cheap scan (no per-char normalize/regex).
function searchActions(actions, query) {
    var q = (query || "").trim();
    var list = actions || [];
    matchIndex = {};
    if (!q) { searchNeedle = ""; return list.slice(0); }
    searchNeedle = NormText(q, false);
    // Mode keywords ("advanced"/"expert"/"developer") are a union, not a filter: the normal text
    // search still runs on the full query, and every setting requiring a named mode is appended.
    var modes = modeFilterFromQuery(q);
    // Compiled once per pass, reused over every field: non-global so no exec()/lastIndex state leaks
    // between fields, and EscapeRegExp keeps regex metachars in the query literal.
    var wwRe = new RegExp("\\b" + EscapeRegExp(searchNeedle) + "\\b");

    var scored = [];
    var seen = {};
    for (var i = 0; i < list.length; i++) {
        var a = list[i];
        var t = fieldMatchScore(titleNorm(a), wwRe);
        var g = fieldMatchScore(groupNorm(a), wwRe);
        var s = fieldMatchScore(sourceNorm(a), wwRe);
        var score = scoreFields(t, g, s);
        if (score === null) continue;
        // Ranges are per-field against the ACTUAL text drawn: title for the row-name, and group (or
        // source when group is empty) for the eyebrow - so highlight offsets stay aligned to the label.
        matchIndex[a.id] = {
            title: t ? t.ranges : null,
            group: g ? g.ranges : null,
            source: s ? s.ranges : null,
            useEyebrowGroup: !!(a.group)
        };
        scored.push({ a: a, s: score });
        seen[a.id] = true;
    }
    scored.sort(function (x, y) {
        if (x.s !== y.s) return y.s - x.s;
        if (x.a.title !== y.a.title) return x.a.title < y.a.title ? -1 : 1;
        return x.a.id < y.a.id ? -1 : x.a.id > y.a.id ? 1 : 0;
    });
    var result = scored.map(function (e) { return e.a; });
    if (modes.length) {
        // Settings requiring a named mode, after the ranked text matches and without duplicates.
        var extras = [];
        for (var j = 0; j < list.length; j++) {
            if (!seen[list[j].id] && modes.indexOf(list[j].mode) !== -1)
                extras.push(list[j]);
        }
        extras.sort(function (x, y) {
            if (x.title !== y.title) return x.title < y.title ? -1 : 1;
            return x.id < y.id ? -1 : x.id > y.id ? 1 : 0;
        });
        result = result.concat(extras);
    }
    return result;
}

// Pure: how many rows must be materialized to cover the given starting index plus `size` more.
// Clamped to the total; used to decide "render the next window" on scroll / arrow-nav.
function revealTarget(total, fromIndex, size) {
    return Math.min(total, Math.max(0, fromIndex) + size);
}

// Pure: the bottom spacer's height - the un-rendered row tail plus any not-yet-rendered section
// headers, so the scrollbar reflects the full list and the last rows stay reachable.
function spacerHeight(total, rendered, totalSections, renderedSections) {
    return Math.max(0, total - rendered) * ROW_H + Math.max(0, totalSections - renderedSections) * SECTION_H;
}

// buildKey: the command-list signature that decides whether rows must be rebuilt (new search / phase)
// or just have their selection refreshed in place (arrow-nav / click). Cheap to compute.
function buildKey() { return phase + "|" + (query || "").trim(); }

function visibleFavourites(favourites, actions) {
    // why: a fav whose id has no live action (plugin unloaded/disabled) renders a dead
    //      placeholder tile whose click run()s to a silent no-op; drop it from the quick-bar.
    var seen = {};
    (actions || []).forEach(function (a) { seen[a.id] = true; });
    return (favourites || []).filter(function (id, i, arr) {
        return seen[id] && arr.indexOf(id) === i;
    });
}

// Numbered quick-launch slots (mirrors ActionRegistry::kFavLimit). Pure so the node-vm test
// can exercise the digit<->slot mapping without a DOM.
var K_FAV_LIMIT = 10;

// Badge label for a 0-based fav-bar index: 0..8 -> "1".."9", index 9 (the 10th) -> "0".
function favSlotForIndex(i) {
    if (i < 0 || i >= K_FAV_LIMIT) return null;
    return i < 9 ? String(i + 1) : "0";
}

// Digit key -> 0-based fav-bar index (1..9 -> 0..8, 0 -> 9); -1 for anything else.
function favIndexForDigit(d) {
    var c = String(d || "").charCodeAt(0);
    if (c >= 49 && c <= 57) return c - 49;
    if (c === 48) return 9;
    return -1;
}

// Physical digit for a keydown event. Use e.code so macOS Option+digit (which composes to a symbol
// in e.key, e.g. Alt+1 -> "¡") still maps to the intended slot; fall back to e.key elsewhere.
function favDigitFromEvent(e) {
    var m = /^(?:Digit|Numpad)([0-9])$/.exec((e && e.code) || "");
    return m ? m[1] : ((e && e.key) || "");
}

function resultCountText(total, shown, query) {
    var n = total + " " + T("sd_actions", "actions");
    return (query || "").trim() ? T("sd_showing", "Showing") + " " + shown + " " + T("sd_of", "of") + " " + n : n;
}

// Display label for a notebook tab. Trim any stray whitespace; pages added with an empty title
// (Home, MainFrame adds TAB_ID_HOME with "") fall back to the title-cased id ("home" -> "Home").
function tabTitle(t) {
    var title = (t && t.title) ? String(t.title).trim() : "";
    return title || prettySource((t && t.id) || "");
}

// Filter the tab list by a fuzzy title/id match. Pure so the node-vm test can exercise it.
function filterTabs(tabs, query) {
    var q = (query || "").trim();
    if (!q)
        return (tabs || []).slice(0);
    return (tabs || []).filter(function (t) {
        return FuzzyRanges(tabTitle(t), q, false) || FuzzyRanges(t.id, q, false);
    });
}

function shouldRenderActionList(query) {
    return !!((query || "").trim());
}

// Tile pictogram base path. The page lives at resources/web/dialog/SpeedDial/, so this climbs to
// resources/images/ where the same SVG icons the native GUI controls use are shipped.
var ICON_BASE = "../../../images/";

// SVG base name for an action's tile pictogram, or "" when it has none (commands without a GUI
// icon, plugins). Pure so the node-vm test can exercise it.
function actionIcon(a) {
    return (a && a.icon) ? a.icon : "";
}

// Put a pictogram into a tile (search row, favourites tile, or tab row). No icon leaves the tile
// blank. `mono` marks the white tab-strip glyphs, which the CSS recolors to the shared gray.
function fillTile(tile, a, mono) {
    tile.textContent = "";
    var icon = actionIcon(a);
    if (!icon)
        return;
    var img = document.createElement("img");
    img.className = mono ? "tile-icon tab-mono" : "tile-icon";
    img.src = ICON_BASE + icon + ".svg";
    img.alt = "";
    img.setAttribute("aria-hidden", "true");
    tile.appendChild(img);
}

// Category a row is grouped under in the empty-query list. Native commands and dynamic plate/recent
// actions carry a group; settings derive their top-level preset type from the source breadcrumb
// ("Process : Quality : Layers" -> "Process"); plugins all share one header.
function actionCategory(a) {
    if (!a) return T("sd_other", "Other");
    if (a.kind === "plugin") return T("sd_plugins", "Plugins");
    if (a.group) return a.group;
    var src = a.source || "";
    var sep = src.indexOf(" : ");
    var cat = sep === -1 ? src : src.slice(0, sep);
    return cat || T("sd_other", "Other");
}

// Stable-bucket actions by category, then order the groups alphabetically. Within a group the incoming
// (frecency) order is kept. Pure so the node-vm test can exercise grouping.
function groupActions(list) {
    var buckets = Object.create(null);
    var order = [];
    (list || []).forEach(function (a) {
        var c = actionCategory(a);
        if (!buckets[c]) { buckets[c] = []; order.push(c); }
        buckets[c].push(a);
    });
    order.sort(function (x, y) {
        var a = x.toLowerCase(), b = y.toLowerCase();
        return a < b ? -1 : a > b ? 1 : 0;
    });
    var out = [];
    order.forEach(function (c) { out = out.concat(buckets[c]); });
    return out;
}

// The active list for the main phase. A typed query ranks every action (commands/plugins/settings)
// by relevance; an empty query shows the recents first, then the rest grouped under category headers.
// The empty-query result is cached on the action/recents array identities so scrolling doesn't regroup.
var commandListCache = null;
// Typed-query result cache, keyed on the pool identity + query. searchActions populates the
// module-level matchIndex/searchNeedle; a hit restores both so a repeated call (keydown + input,
// or a scroll tick) skips the whole scan instead of recomputing it.
var searchCache = null;
function commandList(actions, recents, query) {
    var all = actions || [];
    if (shouldRenderActionList(query)) {
        if (searchCache && searchCache.actions === all && searchCache.query === query) {
            matchIndex   = searchCache.matchIndex;
            searchNeedle = searchCache.needle;
            return searchCache.list;
        }
        var found = searchActions(all, query);
        searchCache = { actions: all, query: query, list: found, matchIndex: matchIndex, needle: searchNeedle };
        return found;
    }
    var rec = recents || [];
    if (commandListCache && commandListCache.actions === all && commandListCache.recents === rec)
        return commandListCache.list;
    var recentIds = {};
    for (var i = 0; i < rec.length; i++)
        recentIds[rec[i].id] = true;
    var rest = all.filter(function (a) { return !recentIds[a.id]; });
    var list = rec.concat(groupActions(rest));
    commandListCache = { actions: all, recents: rec, list: list };
    return list;
}

// Section headers for the main list: "Recent" (when recents exist) then one per category. The list is
// already grouped, so a header is emitted whenever the category changes. A typed query has no headers.
// Returns {startIndex: label}, where startIndex is the flat list index the header sits above.
function commandSections(list, recentsLen, query) {
    if (shouldRenderActionList(query) || !list || !list.length)
        return null;
    var sections = {};
    if (recentsLen > 0)
        sections[0] = T("sd_recent", "Recent");
    var prev = null;
    for (var i = recentsLen; i < list.length; i++) {
        var c = actionCategory(list[i]);
        if (i === recentsLen || c !== prev)
            sections[i] = c;
        prev = c;
    }
    return sections;
}

// Resolve the selection cursor {zone,i} to the action id it points at: fav zone indexes the
// visible favourites, list zone the active commands list. `actions` must be the already-resolved
// list (recents for an empty query, the filtered list otherwise) - pure so runSelected() shares
// one lookup and the node-vm test can call it directly.
function selectedActionId(sel, actions, favIds) {
    if (sel.zone === "fav")
        return favIds[sel.i];
    if (!actions || !actions.length)
        return null;
    var a = actions[sel.i];
    return a && a.id;
}

function actionHasWiki(a) { return !!(a && a.wiki); }

// Whether the action has anything for the footer strip to show (a description or a wiki link).
function actionHasDetail(a) { return !!(a && ((a.desc && a.desc.length) || a.wiki)); }

function foldLabel(s) { return String(s || "").toLowerCase().replace(/[^a-z0-9]+/g, ""); }

// Title-case a source for display: "GCODE OPTIMIZER"/"iRoNiNg pRo" -> "Gcode Optimizer"/"Ironing Pro".
function prettySource(source) {
    return String(source || "").toLowerCase().replace(/\b\w/g, function (c) { return c.toUpperCase(); });
}

// True when the action's required mode is above the user's current mode, i.e. selecting it will
// prompt a mode switch. Unknown/empty modes are treated as "simple" so commands never flag.
function needsModeSwitch(item, userMode) {
    var need = MODE_RANK[(item && item.mode) || "simple"] || 0;
    var have = MODE_RANK[userMode || "simple"] || 0;
    return need > have;
}

// Short mode tag for an action that needs a switch, or "" when it is already available.
function modeBadge(item, userMode) {
    if (!needsModeSwitch(item, userMode)) return "";
    if (item.mode === "develop") return T("sd_mode_develop", "Developer");
    if (item.mode === "expert") return T("sd_mode_expert", "Expert");
    if (item.mode === "advanced") return T("sd_mode_advanced", "Advanced");
    return "";
}

// Search keywords that name a settings mode. "developer" (and the internal "develop") both select
// Developer; Simple is deliberately absent so it never floods the list with every command.
var MODE_WORDS = { advanced: "advanced", expert: "expert", developer: "develop", develop: "develop" };

// The mode values named as whole words in `query`, deduped. Case/diacritic-insensitive via Norm. A
// query with no mode keyword returns [] so the normal text search is completely unaffected.
function modeFilterFromQuery(query) {
    var norm = NormText(String(query || "").trim(), false);
    var found = [];
    norm.split(/[^a-z0-9]+/).forEach(function (word) {
        var mode = MODE_WORDS[word];
        if (mode && found.indexOf(mode) === -1)
            found.push(mode);
    });
    return found;
}

// Precomputed label parts for one action pool, keyed on the pool's array identity. The fold pass is
// O(N); doing it here instead of inside every actionLabel() call keeps row rendering O(rows), not O(rows*N).
var labelCache = null;
function labelParts(actions) {
    if (labelCache && labelCache.actions === actions)
        return labelCache;
    var sig = {}, count = {};
    (actions || []).forEach(function (o) {
        var s = foldLabel(o.title) + "|" + foldLabel(o.source || o.group || "");
        sig[o.id] = s;
        count[s] = (count[s] || 0) + 1;
    });
    labelCache = { actions: actions, sig: sig, count: count };
    return labelCache;
}

// Accessible label "Title from Pretty Source", disambiguated with the opaque action id when another
// action shares the same title+source (case/separator-insensitive) - so two rows never read out identically.
function actionLabel(action, actions) {
    var label = action.title + " from " + prettySource(action.source || action.group || "");
    if (actions && actions.length) {
        var cache = labelParts(actions);
        var mine  = cache.sig[action.id];
        // mine is undefined only for an action outside the cached pool (e.g. a transient row); scan then.
        var clash = mine !== undefined ? cache.count[mine] > 1 :
            actions.some(function (o) {
                return o.id !== action.id && foldLabel(o.title) + "|" + foldLabel(o.source || o.group || "") ===
                    foldLabel(action.title) + "|" + foldLabel(action.source || action.group || "");
            });
        if (clash)
            label += " (" + action.id + ")";
    }
    return label;
}

function syncClearButton() {
    if (clearEl)
        clearEl.hidden = !query;
}

function stateFromPayload(payload) {
    return {
        actions: payload.actions || [],
        favourites: payload.favourites || [],
        recent: payload.recent || [],
        userMode: payload.user_mode || "simple"
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

// nextSel: pure arrow-nav transition. Down fav->list0; Down list wraps at the bottom (last -> first).
// Up list wraps at the top (first -> last) only when there's no fav bar above; with a fav bar, Up at
// the list top goes to fav0 (unchanged). Left/Right clamp within fav. Returns a fresh {zone,i}.
function nextSel(sel, key, listLen, favLen) {
    var zone = sel.zone, i = sel.i;
    var last = Math.max(0, listLen - 1);
    if (key === "ArrowDown") {
        if (zone === "fav") return { zone: "list", i: 0 };
        return { zone: "list", i: i >= last ? 0 : i + 1 };
    }
    if (key === "ArrowUp") {
        if (zone === "list") {
            if (i <= 0) return favLen ? { zone: "fav", i: 0 } : { zone: "list", i: last };
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

// C++ pushes payloads here.
window.HandleStudio = function (payload) {
    if (!payload) return;
    if (typeof payload === "string") { try { payload = JSON.parse(payload); } catch (e) { return; } }
    if (payload.command === "list_actions") {
        var next = stateFromPayload(payload);
        ACTIONS = next.actions;
        FAVS = next.favourites;
        RECENTS = next.recent;
        USER_MODE = next.userMode;
        // A fresh payload re-opens the main phase; C++ never rehydrates the transient phase/query state.
        phase = "commands";
        tabOptions = [];
        query = "";
        sel = { zone: "list", i: 0 };
        lastResizeHeight = 0;
        // why: builtKey caches phase|query so renderCommandsList can skip a rebuild on arrow-nav.
        // It survives a re-open (which never goes through exitPhase), so without a reset the cached
        // empty-query key would skip the rebuild and leave stale list content.
        builtKey = "";
        if (qEl) {
            qEl.value = "";
            qEl.placeholder = T("sd_search_n", "Search %s actions", ACTIONS.length);
            syncClearButton();
        }
        render({ resize: true, resetScroll: true });
        focusInput();
    } else if (payload.command === "tab_results") {
        tabOptions = payload.tabs || [];
        if (sel.zone === "list")
            sel.i = Math.max(0, Math.min(sel.i, tabOptions.length - 1));
        render({ resize: true });
    } else if (payload.command === "favourite_full") {
        // Favourites are at the quick-launch cap - undo the optimistic pin and flash a hint.
        var fid = payload.id;
        if (fid && FAVS.indexOf(fid) !== -1) FAVS.splice(FAVS.indexOf(fid), 1);
        render({ resize: true, keepScroll: true });
        flashHint(T("sd_favs_full", "Favourites are full") + " (" + (payload.limit || K_FAV_LIMIT) + " " + T("sd_max", "max") + ")");
    }
};

// ---- DOM helpers -------------------------------------------------------------
function $(id) { return document.getElementById(id); }

function byId(id) {
    for (var i = 0; i < ACTIONS.length; i++) if (ACTIONS[i].id === id) return ACTIONS[i];
    return null;
}

function findActionByInput(input) {
    for (var i = 0; i < ACTIONS.length; i++) if (ACTIONS[i].input === input) return ACTIONS[i];
    return null;
}

function currentVisibleFavs() { return visibleFavourites(FAVS, ACTIONS); }

// Active list for the current phase (drives list rendering + arrow nav).
function currentList() {
    if (phase === "tab") return filterTabs(tabOptions, query);
    if (phase === "commands") return commandList(ACTIONS, RECENTS, query);
    return []; // percent - the input itself is the only field
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

// A bookmark glyph: outlined when unpinned, filled when saved to favourites.
function pinSvg(on) {
    return '<svg width="15" height="15" viewBox="0 0 24 24" fill="' + (on ? "currentColor" : "none") +
        '" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round">' +
        '<path d="M6 3.5A1.5 1.5 0 0 1 7.5 2h9A1.5 1.5 0 0 1 18 3.5V21l-6-4.2L6 21z"/></svg>';
}

// Sync one pin button to its favourite state. Shared by row construction and the in-place
// updatePins pass so the two can't drift.
function setPinState(pin, on) {
    pin.classList.toggle("on", on);
    pin.innerHTML = pinSvg(on);
    pin.title = on ? T("sd_unpin_fav", "Unpin from favourites (Ctrl+B)") : T("sd_pin_fav", "Pin to favourites (Ctrl+B)");
}

// ---- render ------------------------------------------------------------------
function renderFav() {
    // Only the commands phase shows the pinned quick-bar.
    if (phase !== "commands") {
        if (favEl) { favEl.innerHTML = ""; favEl.hidden = true; }
        if (eyeEl) eyeEl.hidden = true;
        return;
    }
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
        fillTile(tile, a);
        var tileBadge = modeBadge(a, USER_MODE);
        tile.title = a.title + (tileBadge ? " (" + tileBadge + ")" : "");
        tile.setAttribute("aria-label", actionLabel(a, ACTIONS));
        tile.onclick = function () { sel = { zone: "fav", i: i }; activateEntry(a); };
        // Numbered quick-launch badge (Alt/Option+digit), drawn on the corner.
        var slot = favSlotForIndex(i);
        if (slot) {
            var badge = document.createElement("span");
            badge.className = "fav-slot";
            badge.textContent = slot;
            badge.title = slot === "0" ? T("sd_favourite", "Favourite") + " 10 (Alt+0)" :
                T("sd_favourite", "Favourite") + " " + slot + " (Alt+" + slot + ")";
            tile.appendChild(badge);
        }
        // Direct removal: a hover-revealed ✕ in the tile's corner. click() stops propagation so it
        // unpins without activating the action.
        var unpin = document.createElement("button");
        unpin.className = "fav-unpin";
        unpin.title = T("sd_remove_fav", "Remove from favourites");
        unpin.setAttribute("aria-label", T("sd_remove_fav", "Remove from favourites"));
        unpin.innerHTML = '<svg width="9" height="9" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><line x1="4" y1="4" x2="12" y2="12"/><line x1="12" y1="4" x2="4" y2="12"/></svg>';
        unpin.onclick = function (ev) { ev.stopPropagation(); toggleFav(id); };
        tile.appendChild(unpin);
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
    addFavMenuItem(T("sd_move_left", "Move left"), vi > 0, function () { moveFav(id, -1); });
    addFavMenuItem(T("sd_move_right", "Move right"), vi >= 0 && vi < favs.length - 1, function () { moveFav(id, 1); });
    addFavMenuItem(T("sd_unpin", "Unpin"), true, function () { toggleFav(id); });
    favMenuEl.hidden = false;
    favMenuEl.style.left = Math.max(0, Math.min(x, window.innerWidth - favMenuEl.offsetWidth - 4)) + "px";
    favMenuEl.style.top = Math.max(0, Math.min(y, window.innerHeight - favMenuEl.offsetHeight - 4)) + "px";
}

// Swap a favourite with its visible neighbour (dir -1/+1) and persist the new order. Swapping
// by id inside FAVS (not the visible slice) keeps the persisted order stable.
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

// Row shell shared by action and tab rows: the row (selection class + aria label), the icon tile and
// the text column. Returns the pieces the caller fills in (eyebrow/name/badge/pin, handlers).
function beginRow(item, i, mono, ariaLabel) {
    var row = document.createElement("div");
    row.className = "row" + (sel.zone === "list" && sel.i === i ? " sel" : "");
    row.setAttribute("aria-label", ariaLabel);

    var tile = document.createElement("div");
    tile.className = "tile";
    fillTile(tile, item, mono);

    var left = document.createElement("div");
    left.className = "row-left";
    var line = document.createElement("div");
    line.className = "row-line";
    left.appendChild(line);
    row.appendChild(tile);
    row.appendChild(left);
    return { row: row, left: left, line: line };
}

// A command/action row - used for search results, recents, and (because settings are actions now)
// the setting options too. All rows are pinnable, so every row carries a bookmark.
function renderActionRow(a, i) {
    var on = FAVS.indexOf(a.id) !== -1;
    var shell = beginRow(a, i, false, actionLabel(a, ACTIONS));
    var mi = matchIndex[a.id];
    // The eyebrow shows group when present, else source. Highlight with the ranges of whichever of the
    // two the eyebrow actually renders (so a "Recent Projects"/"Object" header match lights up like a
    // setting path does - the offsets are computed against the same string we are marking).
    var eyebrow = a.group || a.source;
    var eyebrowMatch = mi ? (mi.useEyebrowGroup ? mi.group : mi.source) : null;
    shell.left.insertBefore(markedText("row-eyebrow", eyebrow, eyebrowMatch), shell.line);
    shell.line.appendChild(markedText("row-name", a.title, mi ? mi.title : null));
    var badge = modeBadge(a, USER_MODE);
    if (badge) {
        var tag = document.createElement("span");
        tag.className = "row-mode";
        tag.textContent = badge;
        shell.line.appendChild(tag);
    }

    var pin = document.createElement("button");
    pin.className = "pin";
    setPinState(pin, on);
    pin.onclick = function (ev) { ev.stopPropagation(); toggleFav(a.id); };
    // why: two quick fav/unfav clicks must not dblclick-run the row
    pin.ondblclick = function (ev) { ev.stopPropagation(); };
    shell.row.appendChild(pin);

    shell.row.onclick = function () { sel = { zone: "list", i: i }; render({ resize: true }); };
    shell.row.ondblclick = function () { sel = { zone: "list", i: i }; activateEntry(a); };
    return shell.row;
}

// Append rows [from, to) into listEl, always inserting before the bottom spacer so row order is
// preserved. A section header is inserted just before the first row of its section.
function appendActionRows(list, from, to) {
    var spacer = spacerEl || ensureSpacer();
    for (var i = from; i < to; i++) {
        if (sectionStarts && sectionStarts[i] !== undefined) {
            var header = document.createElement("div");
            header.className = "dial-section";
            header.textContent = sectionStarts[i];
            listEl.insertBefore(header, spacer);
            sectionRendered++;
        }
        var row = renderActionRow(list[i], i);
        row.setAttribute("data-idx", i);
        listEl.insertBefore(row, spacer);
    }
}

// Set the section map for the current list and reset the rendered-header counters (a fresh build).
function setSections(sections) {
    sectionStarts = sections || null;
    sectionTotal = sectionStarts ? Object.keys(sectionStarts).length : 0;
    sectionRendered = 0;
}

// Ensure the bottom spacer exists as the last child of listEl. It is (re)created on rebuild because
// listEl.innerHTML="" destroys the old node.
function ensureSpacer() {
    if (!spacerEl || spacerEl.parentNode !== listEl) {
        spacerEl = document.createElement("div");
        spacerEl.className = "dial-spacer-bottom";
        listEl.appendChild(spacerEl);
    }
    return spacerEl;
}

// Size the spacer to the un-rendered tail so the scrollbar reflects the full match count. Pending
// section headers reserve their own height too, so the last rows stay reachable.
function setBottomSpacer(total) {
    ensureSpacer();
    spacerEl.style.height = spacerHeight(total, renderEnd, sectionTotal, sectionRendered) + "px";
}

// Reveal rows up to `upto` (an exclusive index), appending without rebuilding the whole list. Used by
// the scroll handler (viewport + overscan) and by arrow-nav that runs off the end of the current window.
// The window is append-only and row indices are absolute, so jumping the selection to the very last row
// (ArrowUp wrap with no fav bar) necessarily materializes the whole list; the label/search caches above
// keep that one-off cost linear rather than quadratic.
function revealTo(list, upto) {
    var need = Math.min(list.length, upto);
    if (need <= renderEnd)
        return;
    appendActionRows(list, renderEnd, need);
    renderEnd = need;
    setBottomSpacer(list.length);
}

// Rebuild the list from the first window (new search / phase change), clearing stale rows.
function rebuildCommandsList(list) {
    listEl.innerHTML = "";
    listEl.className = "dial-list";
    ensureSpacer();
    renderEnd = 0;
    sectionRendered = 0;
    appendActionRows(list, 0, Math.min(list.length, K_ROWS));
    renderEnd = Math.min(list.length, K_ROWS);
    setBottomSpacer(list.length);
}

// Toggle the .sel class in place - arrow-nav/click don't rebuild the DOM, just re-highlight the row.
function updateSelection() {
    var rows = listEl ? listEl.querySelectorAll(".row") : [];
    for (var i = 0; i < rows.length; i++) {
        var idx = parseInt(rows[i].getAttribute("data-idx"), 10);
        rows[i].classList.toggle("sel", sel.zone === "list" && idx === sel.i);
    }
}

// Sync the pin buttons in place when FAVS changes but the row set doesn't (fav toggle), so a
// bookmark fills/empties without a rebuild that would reset scroll. Rows carry data-idx into the
// active list.
function updatePins(list) {
    var rows = listEl ? listEl.querySelectorAll(".row") : [];
    for (var i = 0; i < rows.length; i++) {
        var a = list[parseInt(rows[i].getAttribute("data-idx"), 10)];
        var pin = rows[i].querySelector(".pin");
        if (!a || !pin) continue;
        var on = FAVS.indexOf(a.id) !== -1;
        if (pin.classList.contains("on") !== on)
            setPinState(pin, on);
    }
}

// Replace the list with a single placeholder message (no matches / empty state). Shared by all phases.
function renderEmpty(text) {
    listEl.innerHTML = "";
    spacerEl = null;
    listEl.className = "dial-list empty";
    setSections(null);
    if (countEl) countEl.hidden = true;
    var empty = document.createElement("div");
    empty.className = "dial-empty";
    empty.textContent = text;
    listEl.appendChild(empty);
}

function renderCommandsList() {
    var list = currentList();
    var total = list.length;
    if (sel.zone === "list")
        sel.i = Math.max(0, Math.min(sel.i, total - 1));
    var showList = shouldRenderActionList(query);
    // Recents are not filtered, so clear any stale match marks from a previous typed query.
    if (!showList)
        matchIndex = {};

    if (!total) {
        renderEmpty(showList ? (T("sd_no_match", "No actions match") + " (" + T("sd_total", "Total") + ": " + ACTIONS.length + ")")
                             : T("sd_no_actions", "No actions yet"));
        renderEnd = 0;
        builtKey = buildKey() + "|0";
        return;
    }

    var key = buildKey() + "|" + total;
    if (key !== builtKey) {
        builtKey = key;
        // Headers split the empty-query list into recents + category groups; typed queries have none.
        setSections(showList ? null : commandSections(list, (RECENTS || []).length, query));
        rebuildCommandsList(list);
    } else if (sel.i >= renderEnd) {
        // Arrow-nav walked past the rendered window - reveal enough to keep the selection visible.
        revealTo(list, revealTarget(total, sel.i, K_ROWS));
    }

    listEl.className = "dial-list";
    // The empty-query list is labelled by its section headers instead.
    if (countEl) {
        countEl.hidden = !showList;
        if (showList)
            countEl.textContent = resultCountText(ACTIONS.length, total, query);
    }
    updateSelection();
    updatePins(list);
}

// A tab row: no pin/unpin (tabs aren't pinnable), and a tile that shows the page icon when the
// notebook has one (plugin pages often don't). Uses tabTitle so pages added with an empty text
// (e.g. Home) still show a label.
function renderTabRow(t, i) {
    var label = tabTitle(t);
    var shell = beginRow(t, i, true, label);
    var name = document.createElement("div");
    name.className = "row-name";
    name.textContent = label;
    shell.line.appendChild(name);
    shell.row.onclick = function () { sel = { zone: "list", i: i }; render({ resize: true }); };
    shell.row.ondblclick = function () { sel = { zone: "list", i: i }; jumpToTab(t); };
    return shell.row;
}

function renderTabList() {
    var q = (query || "").trim();
    var list = currentList();
    listEl.innerHTML = "";
    spacerEl = null; // the tab list has no windowed spacer; rebuildCommandsList recreates it

    if (!list.length) {
        renderEmpty(q ? T("sd_no_tabs_match", "No tabs match") : T("sd_no_tabs", "No tabs"));
        return;
    }
    if (sel.zone === "list")
        sel.i = Math.max(0, Math.min(sel.i, list.length - 1));
    listEl.className = "dial-list";
    if (countEl) {
        countEl.hidden = false;
        countEl.textContent = q ? list.length + " " + T("sd_matches", "matches") : list.length + " " + T("sd_tabs", "tabs");
    }
    list.forEach(function (t, i) { listEl.appendChild(renderTabRow(t, i)); });
}

function renderPercentList() {
    var q = (query || "").trim();
    renderEmpty(q ? T("sd_go_to_pct", "Go to %s% of the layer range", q) : T("sd_enter_pct", "Enter a layer percentage (0-100)"));
}

function renderList() {
    if (phase === "tab")
        renderTabList();
    else if (phase === "percent")
        renderPercentList();
    else
        renderCommandsList();
}

// The action the footer describes: the current selection resolved through the active list/fav bar.
function currentDetailAction() {
    if (phase !== "commands") return null;
    var id = selectedActionId(sel, currentList(), currentVisibleFavs());
    return id ? byId(id) : null;
}

// Footer detail strip: the selected action's description plus, when it has a wiki page, a link that
// opens it (same path as F1). Shown only when the highlighted action has something to say, so
// selecting a command with no description hides the strip.
function renderDetail() {
    if (!detailEl) return;
    var a = currentDetailAction();
    var show = phase === "commands" && actionHasDetail(a);
    detailEl.hidden = !show;
    detailEl.innerHTML = "";
    if (!show) return;
    if (a && a.desc) {
        var desc = document.createElement("div");
        desc.className = "detail-desc";
        desc.textContent = a.desc;
        detailEl.appendChild(desc);
    }
    if (a && a.wiki) {
        var link = document.createElement("button");
        link.type = "button";
        link.className = "detail-wiki";
        link.textContent = T("sd_wiki", "Wiki") + " (F1)";
        link.onclick = function (ev) { ev.stopPropagation(); SendMessage({ command: "open_wiki", id: a.id }); };
        detailEl.appendChild(link);
    }
}

function render(opts) {
    renderFav();
    renderList();
    renderDetail();
    // Pin toggles don't move the selection, so they pass keepScroll to avoid snapping the list
    // back to a row that is currently off-screen.
    if (!(opts && opts.keepScroll))
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

// Transient inline hint pinned to the top of the launcher (e.g. "favourites are full").
function flashHint(text) {
    if (!document.body) return;
    var launcher = document.querySelector(".launcher");
    if (!launcher) return;
    var hint = document.createElement("div");
    hint.className = "dial-flash";
    hint.textContent = text;
    launcher.insertBefore(hint, launcher.firstChild);
    setTimeout(function () {
        if (hint && hint.parentNode) {
            hint.parentNode.removeChild(hint);
            requestResize(); // reclaim the hint's height so the popup doesn't stay tall
        }
    }, 2500);
}

// ---- actions -----------------------------------------------------------------
function toggleFav(id) {
    var k = FAVS.indexOf(id);
    var newState = k === -1;
    if (newState) FAVS.push(id); else FAVS.splice(k, 1);
    SendMessage({ command: "toggle_favourite", id: id, fav: newState });
    render({ resize: true, keepScroll: true });
}

// Fire a command/plugin action; C++ owns the run-confirm (native dialog) + suppression, then
// closes the popup + toasts.
function run(a) {
    if (!a) return;
    SendMessage({ command: "run_action", id: a.id, title: a.title, param: "" });
}

// Activate an entry in the main phase. Two-phase commands switch the palette to their input phase
// instead of running; everything else (including a setting jump, which is a plain action now) runs.
function activateEntry(a) {
    if (!a) return;
    if (a.input === "percent") { enterPercentPhase(); return; }
    if (a.input === "tab") { enterTabsPhase(); return; }
    run(a);
}

function runSelected() {
    if (phase === "percent") {
        runJumpToLayer(query.trim());
        return;
    }
    if (phase === "tab") {
        var t = currentList()[sel.i];
        if (t) jumpToTab(t);
        return;
    }
    var list = currentList();
    var id = selectedActionId(sel, list, currentVisibleFavs());
    if (id) activateEntry(byId(id));
}

function runJumpToLayer(pct) {
    if (pct === "") return;
    var n = parseFloat(pct);
    if (!isFinite(n) || n < 0 || n > 100) return;
    var a = findActionByInput("percent");
    if (!a) return;
    SendMessage({ command: "run_action", id: a.id, title: a.title, param: String(n) });
}

function jumpToTab(t) {
    var a = findActionByInput("tab");
    if (!a || !t) return;
    SendMessage({ command: "run_action", id: a.id, title: a.title, param: t.id });
}

// Clear the shared query/cursor state when entering or leaving a phase. Callers set the
// phase-specific placeholder, then render.
function resetPhaseInput() {
    query = ""; qEl.value = ""; sel = { zone: "list", i: 0 };
    syncClearButton();
}

function enterPercentPhase() {
    phase = "percent";
    resetPhaseInput();
    qEl.placeholder = T("sd_go_layer_ph", "Go to layer % (0-100)");
    render({ resize: true, resetScroll: true });
    qEl.focus();
}

function enterTabsPhase() {
    phase = "tab"; tabOptions = [];
    resetPhaseInput();
    qEl.placeholder = T("sd_go_tab_ph", "Go to tab");
    render({ resize: true, resetScroll: true });
    qEl.focus();
    SendMessage({ command: "search_tabs" });
}

function exitPhase() {
    phase = "commands"; tabOptions = [];
    resetPhaseInput();
    // why: builtKey caches phase|query so renderCommandsList can skip a rebuild on arrow-nav/click.
    // It survives a second-phase exit (which never goes through exitPhase from the commands view),
    // so without a reset the cached empty-query key would skip the rebuild and leave stale content.
    builtKey = "";
    qEl.placeholder = T("sd_search_n", "Search %s actions", ACTIONS.length);
    render({ resize: true, resetScroll: true });
    qEl.focus();
}

function focusInput() { setTimeout(function () { if (qEl) qEl.focus(); }, 0); }

// ---- init --------------------------------------------------------------------
function OnInit() {
    qEl = $("q"); listEl = $("list"); favEl = $("favBar"); clearEl = $("clear"); eyeEl = $("favEyebrow"); countEl = $("count"); detailEl = $("detail");
    // text.js's TranslatePage() targets jQuery `.trans` nodes; this page has none and defines its own
    // `$`, so don't call it. Runtime strings go through T() instead.
    qEl.placeholder = T("sd_search", "Search actions");
    qEl.setAttribute("aria-label", T("sd_search", "Search actions"));
    if (clearEl) {
        clearEl.title = T("sd_clear", "Clear");
        clearEl.setAttribute("aria-label", T("sd_clear", "Clear"));
    }
    syncClearButton();

    $("clear").onclick = function () {
        resetPhaseInput();
        render({ resize: true, resetScroll: true });
        qEl.focus();
    };
    qEl.addEventListener("input", function () {
        query = qEl.value; sel = { zone: "list", i: 0 }; syncClearButton();
        render({ resize: true, resetScroll: true });
    });
    // Windowed reveal: as the list scrolls, materialize the next window (append-only, no rebuild) so the
    // DOM stays bounded to what's near the viewport. Guarded to the commands phase (tabs/percent are tiny).
    listEl.addEventListener("scroll", function () {
        if (phase !== "commands")
            return;
        var list = currentList();
        if (renderEnd >= list.length)
            return;
        var firstVisible = Math.max(0, Math.floor(listEl.scrollTop / ROW_H));
        // Reveal the viewport + a full window of lookahead so fast scrolling doesn't hit a blank tail.
        revealTo(list, revealTarget(list.length, firstVisible, 2 * K_ROWS));
    });

    // why: dismiss the fav context menu on any click/scroll away from it (capture scroll to catch nested scrollers).
    document.addEventListener("click", hideFavMenu);
    document.addEventListener("scroll", hideFavMenu, true);

    document.addEventListener("keydown", function (e) {
        if (favMenuEl && !favMenuEl.hidden && e.key === "Escape") { e.preventDefault(); hideFavMenu(); return; }
        // F1 opens the selected setting's wiki page. Settings without one flash a hint instead.
        if (e.key === "F1") {
            e.preventDefault();
            if (phase !== "commands") return;
            var help = currentDetailAction();
            if (actionHasWiki(help)) SendMessage({ command: "open_wiki", id: help.id });
            else flashHint(T("sd_no_wiki", "No wiki page for this action"));
            return;
        }
        // Pin/unpin the highlighted action: Ctrl/Cmd+B. Commands phase only (tabs/percent aren't pinnable).
        if (phase === "commands" && (e.ctrlKey || e.metaKey) && !e.altKey && !e.shiftKey &&
            e.key.toLowerCase() === "b") {
            e.preventDefault();
            var id = selectedActionId(sel, currentList(), currentVisibleFavs());
            if (id) toggleFav(id);
            return;
        }
        // Quick-launch a numbered favourite: Alt/Option + digit (0 = the 10th). Only in the
        // commands phase, where the pinned bar is shown.
        if (phase === "commands" && e.altKey && !e.ctrlKey && !e.metaKey) {
            var slotIdx = favIndexForDigit(favDigitFromEvent(e));
            var favIds = currentVisibleFavs();
            if (slotIdx >= 0 && slotIdx < favIds.length) {
                e.preventDefault();
                var fav = byId(favIds[slotIdx]);
                if (fav) activateEntry(fav);
                return;
            }
        }
        var list = currentList();
        // why: fav bar only exists in the commands phase; other phases are list-only, so an
        // ArrowUp at the top must not jump into a hidden fav zone.
        var favs = (phase === "commands") ? currentVisibleFavs() : [];
        // why: Up/Down always navigate; Left/Right only navigate the fav bar. In the list zone,
        //      let Left/Right fall through so they move the caret in the focused search field.
        var lr = e.key === "ArrowLeft" || e.key === "ArrowRight";
        if (e.key === "ArrowDown" || e.key === "ArrowUp" || (lr && sel.zone === "fav")) {
            // In the percent phase the input control owns the caret - arrows edit text, not rows.
            if (phase === "percent") return;
            e.preventDefault();
            sel = nextSel(sel, e.key, list.length, favs.length);
            // why: entering/leaving the fav zone toggles the eyebrow line, changing launcher height;
            // resize so the popup grows/shrinks instead of clipping. requestResize no-ops when unchanged.
            render({ resize: true });
        } else if (e.key === "Enter") {
            e.preventDefault();
            if (phase === "percent") runJumpToLayer(query.trim());
            else runSelected();
        } else if (e.key === "Escape") {
            e.preventDefault();
            if (phase !== "commands") { exitPhase(); }
            else if (query) { query = ""; qEl.value = ""; sel = { zone: "list", i: 0 }; syncClearButton(); render({ resize: true, resetScroll: true }); qEl.focus(); }
            else SendMessage({ command: "close_page" });
        }
    });

    SendMessage({ command: "request_actions" });
}
