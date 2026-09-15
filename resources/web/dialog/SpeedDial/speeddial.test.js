// Regression tests for the DOM-free Speed Dial helpers.
// Run: node resources/web/dialog/SpeedDial/speeddial.test.js
const assert = require("assert");
const fs = require("fs");
const vm = require("vm");

const ctx = {};
ctx.window = ctx;
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(__dirname + "/../../js/fuzzy-search.js", "utf8"), ctx);
vm.runInContext(fs.readFileSync(__dirname + "/speeddial.js", "utf8"), ctx);

assert.equal(typeof ctx.parseId, "undefined", "opaque action ids must never be parsed");

const duplicateActions = [
    { id: "0123456789abcdef", title: "Repair", source: "Mesh Tools" },
    { id: "fedcba9876543210", title: "Repair", source: "Mesh Tools" }
];
assert.equal(
    ctx.actionLabel(duplicateActions[0], duplicateActions),
    "Repair from Mesh Tools (0123456789abcdef)",
    "duplicate labels should use the opaque id without interpreting its contents"
);

assert.equal(ctx.shouldRenderActionList(""), false, "an empty search shows the recents+pool list");
assert.equal(ctx.shouldRenderActionList("  "), false, "whitespace-only search shows the recents+pool list");
assert.equal(ctx.shouldRenderActionList("r"), true, "typing starts rendering matching actions");

// commandList: an empty query shows recents first, then every other action; a typed query filters all.
assert.deepEqual(ctx.commandList(duplicateActions, [], ""), duplicateActions,
    "empty query + no recents shows the whole action pool");
assert.deepEqual(ctx.commandList(duplicateActions, [duplicateActions[0]], ""),
    [duplicateActions[0], duplicateActions[1]],
    "empty query shows the recent first, then the remaining actions");
assert.deepEqual(ctx.commandList(duplicateActions, [], "rep"), duplicateActions,
    "a typed query filters actions (both identical titles match) instead of showing recents");

// filterTabs (tab phase): an empty query keeps the whole list; a typed query filters by title/id.
const tabOptions = [
    { id: "home", title: "Home" },
    { id: "prepare", title: "Prepare" },
    { id: "monitor", title: "Device" },
    { id: "project", title: "Project" }
];
assert.deepEqual(ctx.filterTabs(tabOptions, ""), tabOptions,
    "empty query keeps the whole tab list");
assert.equal(ctx.filterTabs(tabOptions, "prep").length, 1,
    "a typed query filters tabs by title");
assert.equal(ctx.filterTabs(tabOptions, "Device").length, 1,
    "a typed query matches a tab title");
assert.deepEqual(ctx.filterTabs(tabOptions, "zzz"), [],
    "a typed query with no match returns an empty list");

// tabTitle: pages added with an empty title (e.g. MainFrame's Home tab) fall back to the id.
assert.equal(ctx.tabTitle({ id: "home", title: "" }), "Home",
    "an empty title falls back to the title-cased id");
assert.equal(ctx.tabTitle({ id: "home" }), "Home",
    "a missing title falls back to the title-cased id");
assert.equal(ctx.tabTitle({ id: "prepare", title: "Prepare" }), "Prepare",
    "a populated title is kept as-is");
assert.equal(ctx.tabTitle({ id: "prepare", title: " Prepare" }), "Prepare",
    "a stray leading space in a tab title is trimmed so the label shows cleanly");
assert.equal(ctx.filterTabs([{ id: "home", title: "" }], "home").length, 1,
    "an untitled tab still matches a typed query via the id/title fallback");
assert.equal(ctx.filterTabs([{ id: "prepare", title: " Prepare" }], "prepare").length, 1,
    "a leading-space tab title still matches a typed query");

// The main phase is ONE pool: commands/plugins/settings are all actions, ranked by relevance
// (no group headers, no actions-vs-settings discrimination).
const pool = [
    { id: "c1", title: "Layer Height", source: "Quality", group: "Quality : Layers", input: "" },
    { id: "s1", title: "Go to layer (percent)", source: "OrcaSlicer", group: "Commands", input: "percent" },
    { id: "c2", title: "Top Surface Layers", source: "Quality", group: "Quality : Layers", input: "" }
];
assert.deepEqual(ctx.searchActions(pool, ""), pool, "an empty query returns the pool unchanged");
assert.equal(ctx.searchActions(pool, "zzz").length, 0, "a query with no match returns nothing");
// "layer" matches multiple; the exact-titled action ranks above the loosely-matching command.
assert.equal(ctx.searchActions(pool, "layer")[0].id, "c1",
    "a title-exact match ranks above a partial match");
assert.equal(ctx.searchActions(pool, "layer").length >= 2, true,
    "both a setting and a command match the same query in the same list");
assert.equal(ctx.searchActions(pool, "surface")[0].id, "c2",
    "a later-but-precise match still ranks by relevance, not by pool type");

// A perfect match (the needle as one contiguous run) outranks a fuzzy match of the same field - and a
// contiguous GROUP/header hit ("Recent Projects") beats a scattered fuzzy TITLE hit ("Retraction Length"),
// which is what the old flat title-bonus ranking got backwards.
const perfectPool = [
    { id: "set", title: "Retraction Length", source: "Process : Quality : Retraction", group: "", input: "" },
    { id: "recent", title: "myproject.3mf", source: "/home/me/projects/myproject.3mf", group: "Recent Projects", input: "" }
];
assert.equal(ctx.searchActions(perfectPool, "recent")[0].id, "recent",
    "a contiguous header/group match ranks above a scattered fuzzy title match");
// Within a perfect match, the row-name (title) outranks the header (group): the action whose TITLE
// contains the needle perfectly beats the action whose GROUP does, both being contiguous matches.
const titleFirstPool = [
    { id: "grp", title: "Delete Selected", source: "OrcaSlicer", group: "Object", input: "" },
    { id: "t", title: "Object Preview", source: "OrcaSlicer", group: "View", input: "" }
];
assert.equal(ctx.searchActions(titleFirstPool, "object")[0].id, "t",
    "a perfect title match ranks above an equally-perfect group match");

// Highlighting: the needle is matched as a whole word / most-contiguous run, so "orient" lights up the
// whole word in "Auto-Orient" instead of the stray "o" of "Auto" plus "rient" (greedy-leftmost).
const orientPool = [
    { id: "ao", title: "Auto-Orient", source: "OrcaSlicer", group: "Object", input: "" }
];
ctx.searchActions(orientPool, "orient");
assert.deepEqual(ctx.matchIndex.ao.title, [[5, 11]],
    "a whole-word match highlights the full word, not a scattered fuzzy pick");

// A fuzzy source-only match with a very late start still counts, even though its score is negative;
// the old `score < 0` sentinel mistook it for "no field matched" and dropped the action.
const negativePool = [
    { id: "n", title: "Unrelated", source: "o" + "x".repeat(200) + "rnt", group: "", input: "" }
];
assert.equal(ctx.searchActions(negativePool, "ornt").length, 1,
    "a low-score fuzzy match is not mistaken for no match");

// actionCategory: a command/dynamic action's group is its category; a setting uses the top-level
// source segment; every plugin shares one header; a category-less action falls back to "Other".
assert.equal(ctx.actionCategory({ id: "c", group: "Help", source: "OrcaSlicer", kind: "command" }), "Help",
    "a command's group is its category");
assert.equal(ctx.actionCategory({ id: "s", group: "", source: "Process : Quality : Layers", kind: "command" }), "Process",
    "a setting's category is the top-level source segment");
assert.equal(ctx.actionCategory({ id: "s", group: "", source: "Filament : Cooling", kind: "command" }), "Filament",
    "a Filament setting groups under Filament");
assert.equal(ctx.actionCategory({ id: "plugin_script_action:Foo:bar.py", group: "", source: "Gcode Optimizer", kind: "plugin" }), "Plugins",
    "every plugin shares one Plugins header");
assert.equal(ctx.actionCategory({ id: "x", group: "", source: "", kind: "command" }), "Other",
    "a category-less action falls back to Other");

// groupActions: bucket by category, order the groups alphabetically, keep the incoming order within
// each group (the pool arrives frecency-sorted).
const groupPool = [
    { id: "q1", title: "Q1", source: "Quality", group: "Quality", kind: "command" },
    { id: "h1", title: "H1", source: "OrcaSlicer", group: "Help", kind: "command" },
    { id: "p1", title: "P1", source: "Process : A", group: "", kind: "command" },
    { id: "h2", title: "H2", source: "OrcaSlicer", group: "Help", kind: "command" }
];
assert.deepEqual(ctx.groupActions(groupPool).map(function (a) { return a.id; }), ["h1", "h2", "p1", "q1"],
    "groups are alphabetical and each group keeps its incoming order");

// commandList (the main-phase list) delegates to the ranked search for a typed query and returns
// recents + the category-grouped pool for an empty query.
const mixed = [
    { id: "cmd", title: "Slice", source: "OrcaSlicer", group: "Commands", kind: "command", input: "" },
    { id: "set", title: "Sparse Infill Density", source: "Quality", group: "Quality", kind: "command", input: "" }
];
assert.equal(ctx.commandList(mixed, [], "sli")[0].id, "cmd",
    "a typed query keeps the relevance-ranked action list (best match first)");
assert.deepEqual(ctx.commandList(mixed, mixed.slice(0, 1), "").map(function (a) { return a.id; }), ["cmd", "set"],
    "an empty query shows the recents first and de-dupes them out of the tail");
assert.deepEqual(ctx.commandList(mixed, [], "").map(function (a) { return a.id; }), ["cmd", "set"],
    "empty query + no recents shows the whole action pool");
assert.deepEqual(ctx.commandList(mixed, [mixed[1]], "").map(function (a) { return a.id; }), ["set", "cmd"],
    "the recent is hoisted above the alphabetically-ordered groups");

// commandSections: "Recent" (when recents exist) plus one header per category in the grouped list;
// a typed query or an empty list yields no headers. Uses a computed grouped list so the recents
// hoist and the category ordering are exercised together.
const sectionPool = [
    { id: "cmd", title: "Slice", source: "OrcaSlicer", group: "Commands", kind: "command" },
    { id: "help", title: "Shortcuts", source: "OrcaSlicer", group: "Help", kind: "command" },
    { id: "set", title: "Infill", source: "Quality", group: "Quality", kind: "command" }
];
const sectionList = ctx.commandList(sectionPool, [sectionPool[0]], "");
assert.deepEqual(sectionList.map(function (a) { return a.id; }), ["cmd", "help", "set"],
    "recents are hoisted, then the rest is grouped alphabetically (Commands, Help, Quality)");
assert.deepEqual(ctx.commandSections(sectionList, 1, ""), { 0: "Recent", 1: "Help", 2: "Quality" },
    "recents + grouped actions get one header per category");
assert.deepEqual(ctx.commandSections([sectionPool[0]], 1, ""), { 0: "Recent" },
    "a list that is all recents gets only the Recent header");
assert.deepEqual(ctx.commandSections(ctx.commandList(sectionPool, [], ""), 0, ""),
    { 0: "Commands", 1: "Help", 2: "Quality" },
    "with no recents the grouped list still gets category headers");
assert.equal(ctx.commandSections(sectionList, 1, "sli"), null,
    "a typed query has no section headers");
assert.equal(ctx.commandSections([], 0, ""), null,
    "an empty list has no section headers");

// selectedActionId: resolves the active list (recents+pool for an empty query, filtered list otherwise).
assert.equal(
    ctx.selectedActionId({ zone: "list", i: 0 }, ctx.commandList(duplicateActions, [], ""), []),
    "0123456789abcdef",
    "Enter with an empty query resolves the first action in the recents+pool list"
);
assert.equal(
    ctx.selectedActionId({ zone: "list", i: 0 }, ctx.commandList(duplicateActions, [], "rep"), []),
    "0123456789abcdef",
    "a typed query resolves the list selection"
);
assert.equal(
    ctx.selectedActionId({ zone: "list", i: 0 }, ctx.commandList(duplicateActions, [duplicateActions[0]], ""), []),
    "0123456789abcdef",
    "Enter with an empty query resolves the recent entry"
);
assert.equal(
    ctx.selectedActionId({ zone: "fav", i: 0 }, duplicateActions, ["fedcba9876543210"]),
    "fedcba9876543210",
    "favourites stay runnable with an empty query - the fav bar is always visible"
);

// Fav quick-launch slots: digit 1..9 -> index 0..8, digit 0 -> index 9 (the 10th), else -1.
assert.equal(ctx.favIndexForDigit("1"), 0, "digit 1 maps to the 1st favourite slot");
assert.equal(ctx.favIndexForDigit("9"), 8, "digit 9 maps to the 9th favourite slot");
assert.equal(ctx.favIndexForDigit("0"), 9, "digit 0 maps to the 10th (last) favourite slot");
assert.equal(ctx.favIndexForDigit("x"), -1, "non-digit keys are not a slot");
assert.equal(ctx.favIndexForDigit(""), -1, "an empty key is not a slot");

// Badge label per 0-based index: 0..8 -> "1".."9", index 9 -> "0", out of range -> null.
assert.equal(ctx.favSlotForIndex(0), "1", "index 0 shows badge 1");
assert.equal(ctx.favSlotForIndex(8), "9", "index 8 shows badge 9");
assert.equal(ctx.favSlotForIndex(9), "0", "index 9 shows badge 0 (the 10th slot)");
assert.equal(ctx.favSlotForIndex(10), null, "index 10 is beyond the cap");
assert.equal(ctx.favSlotForIndex(-1), null, "negative index is not a slot");
assert.equal(ctx.K_FAV_LIMIT, 10, "the slot count matches the quick-launch cap");

// favDigitFromEvent: prefer the physical code (so macOS Option+digit still maps even though e.key
// is the composed symbol), and fall back to e.key for keyboards/synthetic events without a code.
assert.equal(ctx.favDigitFromEvent({ code: "Digit1", key: "¡" }), "1", "Digit1 wins over a composed key");
assert.equal(ctx.favDigitFromEvent({ code: "Digit0", key: "0" }), "0", "Digit0 is a physical digit");
assert.equal(ctx.favDigitFromEvent({ code: "Numpad7", key: "7" }), "7", "numpad digits count");
assert.equal(ctx.favDigitFromEvent({ code: "", key: "3" }), "3", "a missing code falls back to key");
assert.equal(ctx.favDigitFromEvent({ key: "a" }), "a", "non-digit input is passed through (maps to -1)");
assert.equal(ctx.favDigitFromEvent(null), "", "a null event yields no digit");

// nextSel: arrow-nav wrapping. Down wraps at the list bottom to the first row; Up wraps at the
// list top to the last row ONLY when there's no fav bar above (else it goes to the fav bar).
assert.deepEqual(ctx.nextSel({ zone: "list", i: 2 }, "ArrowDown", 3, 0), { zone: "list", i: 0 },
    "ArrowDown at the last row wraps to the first row");
assert.deepEqual(ctx.nextSel({ zone: "list", i: 1 }, "ArrowDown", 3, 0), { zone: "list", i: 2 },
    "ArrowDown in the middle advances by one");
assert.deepEqual(ctx.nextSel({ zone: "list", i: 0 }, "ArrowUp", 3, 0), { zone: "list", i: 2 },
    "ArrowUp at the first row with no fav bar wraps to the last row");
assert.deepEqual(ctx.nextSel({ zone: "list", i: 0 }, "ArrowUp", 3, 2), { zone: "fav", i: 0 },
    "ArrowUp at the first row with a fav bar goes to the fav bar (unchanged)");
assert.deepEqual(ctx.nextSel({ zone: "list", i: 2 }, "ArrowUp", 3, 0), { zone: "list", i: 1 },
    "ArrowUp in the middle moves up by one");
assert.deepEqual(ctx.nextSel({ zone: "fav", i: 1 }, "ArrowDown", 3, 2), { zone: "list", i: 0 },
    "ArrowDown from the fav bar lands on the first list row");
assert.deepEqual(ctx.nextSel({ zone: "list", i: 0 }, "ArrowDown", 1, 0), { zone: "list", i: 0 },
    "a single-row list never wraps off the end");
assert.deepEqual(ctx.nextSel({ zone: "list", i: 0 }, "ArrowUp", 1, 0), { zone: "list", i: 0 },
    "ArrowUp on the only row stays put");

// Windowed list reveal: how many rows must be materialized to cover `fromIndex` plus `size` more,
// clamped to the total. Drives the "render the next window on scroll / arrow-nav" append.
assert.equal(ctx.revealTarget(100, 0, 100), 100, "covers the whole list when the window reaches the end");
assert.equal(ctx.revealTarget(100, 60, 100), 100, "clamps to the total at the tail");
assert.equal(ctx.revealTarget(30, 5, 100), 30, "a short list is fully covered");
assert.equal(ctx.revealTarget(100, 5, 50), 55, "reveals exactly fromIndex + size");
assert.equal(ctx.revealTarget(100, -5, 50), 50, "negative start is clamped to the first row");
assert.equal(ctx.revealTarget(200, 50, 100), 150, "a scroll viewpoint reveals a window past the current rows");
assert.equal(ctx.revealTarget(10, 0, 50), 10, "a list shorter than one window stays fully materialized");

// spacerHeight: un-rendered rows (44px) plus un-rendered section headers (30px), never negative.
assert.equal(ctx.spacerHeight(100, 50, 0, 0), 50 * 44, "the tail rows reserve their full height");
assert.equal(ctx.spacerHeight(100, 100, 0, 0), 0, "a fully-rendered list needs no spacer");
assert.equal(ctx.spacerHeight(100, 50, 3, 1), 50 * 44 + 2 * 30, "pending section headers reserve their height too");
assert.equal(ctx.spacerHeight(10, 0, 2, 0), 10 * 44 + 2 * 30, "a short list still reserves its headers");
assert.equal(ctx.spacerHeight(0, 0, 0, 0), 0, "an empty list has no spacer");
assert.equal(ctx.spacerHeight(10, 20, 0, 5), 0, "over-rendered counters clamp to zero");

// visibleFavourites: the quick-bar drops pins whose action no longer exists (plugin unloaded,
// command removed) and collapses duplicate ids, keeping the persisted pin order.
assert.deepEqual(ctx.visibleFavourites(["a", "b", "c"], [{ id: "a" }, { id: "b" }]),
    ["a", "b"], "a pin with no live action is dropped from the quick-bar");
assert.deepEqual(ctx.visibleFavourites(["b", "a", "b"], [{ id: "a" }, { id: "b" }]),
    ["b", "a"], "duplicate pins collapse to the first occurrence");
assert.deepEqual(ctx.visibleFavourites([], [{ id: "a" }]), [], "no pins renders an empty quick-bar");
assert.deepEqual(ctx.visibleFavourites(["a"], []), [], "a stale config with no actions renders nothing");

// actionIcon: the SVG base name for a tile's pictogram, or "" when the action has none (blank tile).
assert.equal(ctx.actionIcon({ id: "x", title: "Slice", icon: "media_play" }), "media_play",
    "an action's icon base name is returned verbatim");
assert.equal(ctx.actionIcon({ id: "x", title: "Go to tab...", icon: "" }), "",
    "an empty icon renders a blank tile");
assert.equal(ctx.actionIcon({ id: "x", title: "Plugin action" }), "",
    "a missing icon field renders a blank tile");
assert.equal(ctx.actionIcon(null), "",
    "a null action (tab row) renders a blank tile");

// needsModeSwitch: a setting is gated only when its required mode outranks the user's current mode.
assert.equal(ctx.needsModeSwitch({ mode: "advanced" }, "simple"), true, "Advanced is gated in Simple mode");
assert.equal(ctx.needsModeSwitch({ mode: "expert" }, "simple"), true, "Expert is gated in Simple mode");
assert.equal(ctx.needsModeSwitch({ mode: "expert" }, "advanced"), true, "Expert is gated in Advanced mode");
assert.equal(ctx.needsModeSwitch({ mode: "develop" }, "expert"), true, "Developer is gated in Expert mode");
assert.equal(ctx.needsModeSwitch({ mode: "simple" }, "simple"), false, "a Simple setting is not gated");
assert.equal(ctx.needsModeSwitch({ mode: "advanced" }, "advanced"), false, "an Advanced setting is not gated in Advanced mode");
assert.equal(ctx.needsModeSwitch({ mode: "develop" }, "develop"), false, "a Developer setting is not gated in Developer mode");
assert.equal(ctx.needsModeSwitch({}, "simple"), false, "a command with no mode is never gated");

// MODE_RANK must match the C++ ConfigOptionMode order (comSimple < comAdvanced < comExpert < comDevelop).
assert.deepEqual(ctx.MODE_RANK, { simple: 0, advanced: 1, expert: 2, develop: 3 },
    "mode rank matches the C++ ConfigOptionMode order");

// modeBadge: the tag text for gated settings, empty once the setting is available.
assert.equal(ctx.modeBadge({ mode: "advanced" }, "simple"), "Advanced", "Advanced badge text");
assert.equal(ctx.modeBadge({ mode: "expert" }, "simple"), "Expert", "Expert badge text");
assert.equal(ctx.modeBadge({ mode: "develop" }, "simple"), "Developer", "Developer badge text");
assert.equal(ctx.modeBadge({ mode: "advanced" }, "advanced"), "", "no badge when the mode already matches");

// modeFilterFromQuery: whole-word, case-insensitive mode keywords -> internal mode values. "developer"
// maps to the internal "develop"; "simple" is deliberately not a keyword; a prefix is not a match.
assert.deepEqual(ctx.modeFilterFromQuery("advanced"), ["advanced"], "Advanced is a mode keyword");
assert.deepEqual(ctx.modeFilterFromQuery("Expert"), ["expert"], "the keyword is case-insensitive");
assert.deepEqual(ctx.modeFilterFromQuery("developer"), ["develop"], "Developer maps to the develop value");
assert.deepEqual(ctx.modeFilterFromQuery("develop"), ["develop"], "the internal develop spelling also works");
assert.deepEqual(ctx.modeFilterFromQuery("expert retraction"), ["expert"], "a keyword is found among other text");
assert.deepEqual(ctx.modeFilterFromQuery("advanced expert"), ["advanced", "expert"], "multiple keywords are deduped in order");
assert.deepEqual(ctx.modeFilterFromQuery("advanced advanced"), ["advanced"], "a repeated keyword is deduped");
assert.deepEqual(ctx.modeFilterFromQuery("simple"), [], "Simple is not a mode keyword");
assert.deepEqual(ctx.modeFilterFromQuery("advance"), [], "a mode-word prefix is not a whole-word match");
assert.deepEqual(ctx.modeFilterFromQuery(""), [], "an empty query names no mode");

// searchActions mode union: a mode keyword keeps the normal text matches AND appends every setting
// requiring that mode. Not a filter - a Simple setting literally named "Advanced..." still shows, and
// commands (mode "simple") are never pulled in by a keyword.
const modePool = [
    { id: "a1", title: "Top Surface Layers", source: "Quality", group: "Quality : Layers", mode: "advanced" },
    { id: "a2", title: "Advanced Detection", source: "Quality", group: "Quality", mode: "simple" },
    { id: "a3", title: "Retraction Length", source: "Process", group: "Process : Quality", mode: "expert" },
    { id: "a4", title: "Slice", source: "OrcaSlicer", group: "Commands", mode: "simple" }
];
var adv = ctx.searchActions(modePool, "advanced");
assert.deepEqual(adv.map(function (a) { return a.id; }), ["a2", "a1"],
    "a text match (a2) ranks above the mode-only setting (a1), and no expert/command leaks in");
var expert = ctx.searchActions(modePool, "expert");
assert.deepEqual(expert.map(function (a) { return a.id; }), ["a3"], "the expert keyword pulls in the expert setting");
assert.deepEqual(ctx.searchActions(modePool, "developer"), [], "no developer settings means no mode extras");
assert.equal(ctx.searchActions(modePool, "retraction")[0].id, "a3",
    "a query with no mode keyword is unaffected by the mode union");
assert.equal(ctx.searchActions([{ id: "both", title: "Advanced", source: "Quality", group: "", mode: "advanced" }], "advanced").length,
    1, "a setting that both matches text and requires the mode appears exactly once");

// actionHasWiki: the footer's wiki link/F1 path is offered only when the action carries a wiki flag.
assert.equal(ctx.actionHasWiki({ id: "x", wiki: true }), true, "a wiki-flagged setting offers the wiki action");
assert.equal(ctx.actionHasWiki({ id: "x", wiki: false }), false, "a setting without a wiki path offers nothing");
assert.equal(ctx.actionHasWiki({ id: "x" }), false, "a missing wiki field offers nothing");
assert.equal(ctx.actionHasWiki(null), false, "no action selected offers nothing");

// actionHasDetail: the footer strip appears only when the highlighted action has a description or
// wiki link; selecting a plain command hides it.
assert.equal(ctx.actionHasDetail({ id: "a", desc: "Layer height" }), true, "a description shows the footer");
assert.equal(ctx.actionHasDetail({ id: "a", wiki: true }), true, "a wiki link shows the footer");
assert.equal(ctx.actionHasDetail({ id: "a", desc: "Layer height", wiki: true }), true, "both show the footer");
assert.equal(ctx.actionHasDetail({ id: "a", desc: "" }), false, "an empty description hides the footer");
assert.equal(ctx.actionHasDetail({ id: "a", desc: "", wiki: false }), false, "empty description and false wiki hide the footer");
assert.equal(ctx.actionHasDetail({ id: "a" }), false, "an action with neither hides the footer");
assert.equal(ctx.actionHasDetail(null), false, "no selected action hides the footer");

console.log("ok");
