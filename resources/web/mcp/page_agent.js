// The MCP server's agent inside a web page: lists the page as ui_snapshot shows it and acts on its
// elements for ui_click, ui_input, ui_key, ui_mouse and dialog_respond. OrcaSlicer injects it on
// demand into any of its web views (src/slic3r/GUI/Mcp/WebPages.cpp), so no page or host includes
// it. It runs as the body of `function (V, SRC)`, V the version OrcaSlicer stamps it with and SRC
// this source, which it hands to the plugin configuration frames it relays to, and installs
// window.__orcaMcp, whose call(request) returns a JSON-able reply and never throws.
//
// Input is synthesized DOM events, which are untrusted, so the browser's own default action for a
// key (typing, moving focus, activating a button, submitting a form) or a wheel turn is done here
// when the page does not cancel it. Everything a call does happens before it returns; messages the
// page posts to OrcaSlicer meanwhile are held and sent once it has, so the reply arrives first.
//
// Plain ES2017 for Safari 15 and WebKitGTK 2.36: newer APIs are feature-detected.
'use strict';

var api = {v: V, src: SRC};
var docToken = Math.random().toString(36).slice(2) + Date.now().toString(36);
var keyOf = new WeakMap();
var byKey = new Map();
var nextKey = 1;
var MAX_KEYS = 50000;
var TEXT_MAX = 200;
var NAME_MAX = 120;
var NOTCH_PX = 100;

function keyFor(el) {
  var k = keyOf.get(el);
  if (k === undefined) {
    k = nextKey++;
    keyOf.set(el, k);
  }
  byKey.set(k, el);
  return k;
}

function prune() {
  byKey.forEach(function (el, k) {
    if (!el.isConnected)
      byKey.delete(k);
  });
  if (byKey.size > MAX_KEYS)
    byKey.clear();
}

function collapse(s) { return String(s == null ? '' : s).replace(/\s+/g, ' ').trim(); }
function clip(s, max) { return s.length > max ? s.slice(0, max - 1) + '…' : s; }
function lower(s) { return String(s).toLowerCase(); }
function viewOf(el) { return (el.ownerDocument || document).defaultView || window; }
function normalize(s) { return lower(collapse(s)).replace(/(\.\.\.|…|:)$/, '').trim(); }

function fail(code, message) {
  var e = new Error(message);
  e.code = code;
  throw e;
}

// ---- styles and visibility ------------------------------------------------------------------

var styleCache = null;
function styleOf(el) {
  if (styleCache) {
    var s = styleCache.get(el);
    if (s)
      return s;
  }
  var st = viewOf(el).getComputedStyle(el);
  if (styleCache)
    styleCache.set(el, st);
  return st;
}

function withStyles(fn) {
  var outer = styleCache;
  if (!outer)
    styleCache = new Map();
  try {
    return fn();
  } finally {
    if (!outer)
      styleCache = null;
  }
}

function inert(el) {
  if ('inert' in el)
    return el.inert;
  return el.hasAttribute('inert');
}

// Display, opacity and inert hide a whole subtree; visibility can be overridden below.
function subtreeHidden(el, st) { return st.display === 'none' || st.opacity === '0' || inert(el); }

function hasBox(el, st) {
  if (st.display === 'contents')
    return true;
  var rects = el.getClientRects();
  for (var i = 0; i < rects.length; i++)
    if (rects[i].width > 0 && rects[i].height > 0)
      return true;
  return false;
}

function shown(el, st) { return st.visibility === 'visible' && hasBox(el, st); }

// Moved left of the page's origin, as a closed slide-in panel is: no scrolling reaches it.
function offCanvas(el, st) {
  if (st.display === 'contents')
    return false;
  var r = el.getBoundingClientRect();
  if (!(r.width > 0 && r.right + (viewOf(el).scrollX || 0) <= 0))
    return false;
  // Left of a scroller scrolled to the right, it is scrolled back into view when acted on.
  for (var p = parentOf(el); p && p.nodeType === 1; p = parentOf(p))
    if (p.scrollLeft > 0)
      return false;
  return true;
}

// A control drawn as something else: an opacity-0 or clipped checkbox under a styled label.
function visuallyHidden(el, st) {
  if (st.opacity === '0' || st.visibility !== 'visible' || st.display === 'none')
    return true;
  var r = el.getBoundingClientRect();
  if (r.width <= 2 || r.height <= 2)
    return true;
  return st.clip === 'rect(0px, 0px, 0px, 0px)' || st.clipPath === 'inset(50%)';
}

function parentOf(n) {
  if (n.assignedSlot)
    return n.assignedSlot;
  if (n.parentNode && n.parentNode.nodeType === 11)
    return n.parentNode.host;
  if (n.parentNode && n.parentNode.nodeType === 9)
    return n.parentNode.defaultView && n.parentNode.defaultView.frameElement;
  return n.parentElement;
}

// Whether a person sees `el`: it and every ancestor drawn, through frames.
function hiddenFromView(el) {
  for (var n = el; n && n.nodeType === 1; n = parentOf(n))
    if (subtreeHidden(n, styleOf(n)))
      return true;
  return !shown(el, styleOf(el));
}

// ---- what an element is ------------------------------------------------------------------------

var ROLE_KINDS = {
  button: 'button', menuitem: 'button', link: 'link', textbox: 'text', searchbox: 'text', spinbutton: 'spin',
  checkbox: 'checkbox', menuitemcheckbox: 'checkbox', 'switch': 'toggle', radio: 'radio', menuitemradio: 'radio',
  combobox: 'choice', listbox: 'choice', slider: 'slider', tab: 'tab', heading: 'heading', img: 'image',
  row: 'row', option: 'row', treeitem: 'row', dialog: 'dialog', alertdialog: 'dialog'
};
var INPUT_KINDS = {
  checkbox: 'checkbox', radio: 'radio', range: 'slider', number: 'spin', button: 'button', submit: 'button',
  reset: 'button', image: 'button', file: 'file', hidden: ''
};

var CONTROLS = 'button,input,select,textarea,a[href],[role=button],[onclick]';

function roleOf(el) { return collapse(el.getAttribute('role') || '').split(' ')[0]; }

function jqueryHandles(el) {
  var jq = viewOf(el).jQuery;
  if (!jq || typeof jq._data !== 'function')
    return false;
  try {
    var ev = jq._data(el, 'events');
    return !!(ev && (ev.click || ev.mousedown || ev.mouseup || ev.dblclick || ev.pointerdown || ev.touchstart));
  } catch (e) {
    return false;
  }
}

function isFrame(el) { return el.localName === 'iframe' || el.localName === 'frame'; }

// The kind ui_snapshot lists an element as, in the vocabulary of wx widgets, or '' when it is only a
// container. `pointerAbove` says whether its parent already shows the pointer cursor or is a listed
// button or link, so a clickable element counts once, not with every child inside it.
function kindOf(el, st, pointerAbove) {
  var role = roleOf(el);
  if (role && ROLE_KINDS[role])
    return ROLE_KINDS[role];
  if (role === 'presentation' || role === 'none')
    return '';
  var tag = el.localName;
  switch (tag) {
  case 'button': case 'summary': return 'button';
  case 'a': if (el.hasAttribute('href')) return 'link'; break;
  case 'input': {
    var type = lower(el.type || 'text');
    return type in INPUT_KINDS ? INPUT_KINDS[type] : 'text';
  }
  case 'textarea': return 'text';
  case 'select': return 'choice';
  case 'canvas': return 'canvas';
  case 'h1': case 'h2': case 'h3': case 'h4': case 'h5': case 'h6': return 'heading';
  case 'img': return collapse(el.alt || el.title) ? 'image' : '';
  case 'tr': return 'row';
  case 'iframe': case 'frame': return 'frame';
  case 'dialog': return el.open ? 'dialog' : '';
  case 'label': case 'option': case 'body': case 'html': return '';
  }
  if (el.isContentEditable && !(el.parentElement && el.parentElement.isContentEditable))
    return 'text';
  if (el.hasAttribute('onclick') || typeof el.onclick === 'function' || jqueryHandles(el))
    return 'button';
  if (pointerAbove || el.querySelector(CONTROLS))
    return '';
  // Clickable by what its page gives it, focus (a tab panel holding controls is not), the pointer
  // cursor or its name (close_btn, ButtonStyleRegular), holding no control of its own: pages bind
  // handlers in script, where the walk can't see them, and a list of such items is not one button.
  if (el.hasAttribute('tabindex') && el.tabIndex >= 0)
    return 'button';
  if (st.cursor === 'pointer')
    return 'button';
  if (/btn|button/i.test(el.id + ' ' + (typeof el.className === 'string' ? el.className : '')))
    return 'button';
  return '';
}

// The control a label stands for when the control itself is drawn as the label (a hidden checkbox
// behind a styled one), so the label is listed and acted on as that control.
function labelledControl(el) {
  if (el.localName !== 'label' || !el.control)
    return null;
  var c = el.control;
  if (c.localName !== 'input' || (c.type !== 'checkbox' && c.type !== 'radio'))
    return null;
  return visuallyHidden(c, styleOf(c)) ? c : null;
}

// ---- names, values and states --------------------------------------------------------------------

function innerTextOf(el) { return collapse(typeof el.innerText === 'string' ? el.innerText : el.textContent); }

// Text of `root` without that of `skip` (a label holding its own control).
function textWithout(root, skip) {
  var out = [];
  (function visit(n) {
    if (n === skip)
      return;
    if (n.nodeType === 3)
      out.push(n.data);
    else if (n.nodeType === 1) {
      if (subtreeHidden(n, styleOf(n)))
        return;
      for (var c = n.firstChild; c; c = c.nextSibling)
        visit(c);
    }
  })(root);
  return collapse(out.join(' '));
}

function byIdText(el, ids) {
  var root = el.getRootNode ? el.getRootNode() : el.ownerDocument;
  return collapse(ids.split(/\s+/).map(function (id) {
    var t = (root.getElementById ? root.getElementById(id) : null) || el.ownerDocument.getElementById(id);
    return t ? innerTextOf(t) : '';
  }).join(' '));
}

function nameOf(el, kind) {
  var n = '';
  var tag = el.localName;
  if (el.hasAttribute('aria-labelledby'))
    n = byIdText(el, el.getAttribute('aria-labelledby'));
  if (!n)
    n = collapse(el.getAttribute('aria-label'));
  if (!n && el.labels && el.labels.length && tag !== 'button')
    n = collapse(Array.prototype.map.call(el.labels, function (l) { return textWithout(l, el); }).join(' '));
  if (!n && (tag === 'img' || (tag === 'input' && el.type === 'image')))
    n = collapse(el.alt);
  if (!n && tag === 'input' && /^(button|submit|reset)$/.test(el.type))
    n = collapse(el.value) || (el.type === 'submit' ? 'Submit' : el.type === 'reset' ? 'Reset' : '');
  if (!n && /^(button|link|tab|heading|row|checkbox|radio|toggle|dialog)$/.test(kind) && !/^(input|select|textarea)$/.test(tag)) {
    if (kind === 'row' && tag === 'tr')
      n = el.cells && el.cells[0] ? innerTextOf(el.cells[0]) : '';
    else if (kind === 'dialog') {
      var h = el.querySelector('h1,h2,h3,h4,h5,h6,[role=heading]');
      n = h ? innerTextOf(h) : '';
    } else
      n = innerTextOf(el);
  }
  if (!n) {
    var svg = el.querySelector && el.querySelector(':scope > svg > title');
    if (svg)
      n = collapse(svg.textContent);
  }
  if (!n)
    n = collapse(el.getAttribute('title'));
  if (!n && (tag === 'input' || tag === 'textarea'))
    n = collapse(el.getAttribute('placeholder'));
  if (!n && /^(input|select|textarea)$/.test(tag))
    n = cellName(el);
  return clip(n, NAME_MAX);
}

// A field with no name of its own in a table cell, a matrix's entry: "<row header> / <column
// header>", from the row's first header cell and the column's cell in the last head row (or the
// table's first row when it is all header cells).
function cellName(el) {
  var td = el.closest ? el.closest('td') : null;
  var tr = td && td.parentElement;
  var table = tr && tr.localName === 'tr' && tr.closest('table');
  if (!table)
    return '';
  var row = '';
  for (var i = 0; i < tr.cells.length && !row; i++)
    if (tr.cells[i].localName === 'th')
      row = innerTextOf(tr.cells[i]);
  var head = table.tHead && table.tHead.rows.length ? table.tHead.rows[table.tHead.rows.length - 1] : table.rows[0];
  var allHeaders = head && Array.prototype.every.call(head.cells, function (c) { return c.localName === 'th'; });
  var column = head && head !== tr && allHeaders && head.cells[td.cellIndex] ? innerTextOf(head.cells[td.cellIndex]) : '';
  return row && column ? row + ' / ' + column : row || column;
}

function checkedOf(el, kind) {
  if (kind === 'checkbox' || kind === 'radio' || kind === 'toggle') {
    if (el.localName === 'input')
      return el.checked ? 1 : 0;
    var c = labelledControl(el);
    if (c)
      return c.checked ? 1 : 0;
    var a = el.getAttribute('aria-checked');
    return a === 'true' || a === 'mixed' ? 1 : 0;
  }
  if (kind === 'tab' || (kind === 'row' && el.hasAttribute('aria-selected')))
    return el.getAttribute('aria-selected') === 'true' ? 1 : 0;
  if (kind === 'button' && el.hasAttribute('aria-pressed'))
    return el.getAttribute('aria-pressed') === 'true' ? 1 : 0;
  if (kind === 'button' && el.localName !== 'button' && el.localName !== 'input')
    return classState(el);
  return -1;
}

// The state a page shows only through class names, on a clickable element or inside it: selected
// or checked (ModelCheckBoxSelected, RegionSelected), else unchecked when it draws a checkbox.
function classState(el) {
  function classes(n) { return typeof n.className === 'string' ? n.className : ''; }
  var all = [el].concat(Array.prototype.slice.call(el.querySelectorAll('[class]'), 0, 50));
  // A class word of its own (selected, is-selected, nozzle_selected) or a camel-case one's end
  // (RegionSelected, isChecked), never unselected, un-selected, unSelected, non-selected or NotSelected.
  function selected(c) {
    return (/(^|[\s_-]|is)(selected|checked)($|[\s_-])/i.test(c) || /[a-z](Selected|Checked)($|[\s_-])/.test(c)) &&
           !/(not|non|no|un)[-_]?(selected|checked)/i.test(c);
  }
  if (all.some(function (n) { return selected(classes(n)); }))
    return 1;
  return all.some(function (n) { return /check\s*box/i.test(classes(n)); }) ? 0 : -1;
}

function selectedText(sel) {
  var out = [];
  for (var i = 0; i < sel.options.length; i++)
    if (sel.options[i].selected)
      out.push(collapse(sel.options[i].text));
  return out.join(', ');
}

var MASK = '••••••';

function valueOf(el, kind) {
  var tag = el.localName;
  if (tag === 'select')
    return selectedText(el);
  if (tag === 'input' || tag === 'textarea') {
    if (kind === 'file')
      return collapse(Array.prototype.map.call(el.files || [], function (f) { return f.name; }).join(', '));
    if (kind === 'checkbox' || kind === 'radio' || kind === 'button')
      return '';
    if (el.type === 'password')
      return el.value ? MASK : '';
    return clip(String(el.value), TEXT_MAX);
  }
  if (kind === 'text')
    return clip(innerTextOf(el), TEXT_MAX);
  if (kind === 'row' && tag === 'tr')
    return clip(Array.prototype.slice.call(el.cells || [], 1).map(innerTextOf).join(' | '), TEXT_MAX);
  if (kind === 'slider' || kind === 'spin' || kind === 'choice') {
    var v = el.getAttribute('aria-valuetext') || el.getAttribute('aria-valuenow');
    return v ? collapse(v) : '';
  }
  return '';
}

// Disabled as the browser has it (`:disabled`: its own attribute, or a disabled fieldset's, except in
// that fieldset's first legend), or by ARIA or pointer-events as a page draws it.
function browserDisabled(el) {
  try {
    return el.matches(':disabled');
  } catch (e) {
    return !!el.disabled;
  }
}

function disabledOf(el, st) {
  if (browserDisabled(el))
    return true;
  for (var n = el; n && n.nodeType === 1; n = parentOf(n))
    if (n.getAttribute('aria-disabled') === 'true')
      return true;
  return st.pointerEvents === 'none';
}

function noteOf(el, kind) {
  var notes = [];
  var tag = el.localName;
  if (tag === 'select') {
    var opts = Array.prototype.map.call(el.options, function (o) { return collapse(o.text); });
    notes.push('options: ' + opts.slice(0, 30).join(' | ') + (opts.length > 30 ? ' (+' + (opts.length - 30) + ' more)' : ''));
    if (el.multiple)
      notes.push('multiple');
  }
  if ((tag === 'input' || tag === 'textarea') && el.readOnly)
    notes.push('readonly');
  if (el.required)
    notes.push('required');
  var ph = collapse(el.getAttribute('placeholder'));
  if (ph && kind === 'text' && nameOf(el, kind) !== ph)
    notes.push('placeholder: ' + clip(ph, 60));
  var ex = el.getAttribute('aria-expanded');
  if (ex === 'true' || ex === 'false')
    notes.push(ex === 'true' ? 'expanded' : 'collapsed');
  if (kind === 'file')
    notes.push('file input: not settable');
  return notes.join(', ');
}

function clsOf(el) { return el.localName === 'input' ? 'input[type=' + lower(el.type || 'text') + ']' : el.localName; }

// An element with no name is told apart by its first class, which pages name by purpose (TabButton).
function clsWithClass(el) {
  var first = typeof el.className === 'string' ? el.className.trim().split(/\s+/)[0] : '';
  return clsOf(el) + (first ? '.' + first : '');
}

function sameOrigin(frame) {
  try {
    return !!(frame.contentDocument && frame.contentDocument.documentElement);
  } catch (e) {
    return false;
  }
}

// A frame whose document the page wrote (srcdoc) but cannot read: a plugin configuration UI, whose
// bridge (resources/web/dialog/js/plugin-config-ui.js) runs this agent on request.
function relayFrame(el) { return isFrame(el) && el.hasAttribute('srcdoc') && !sameOrigin(el) && !!el.contentWindow; }

function deepActive(d) {
  var a = d.activeElement;
  while (a) {
    if (a.shadowRoot && a.shadowRoot.activeElement)
      a = a.shadowRoot.activeElement;
    else if (isFrame(a) && sameOrigin(a) && a.contentDocument.activeElement)
      a = a.contentDocument.activeElement;
    else
      break;
  }
  return a;
}

// Where a frame's content starts in its parent's viewport.
function frameOrigin(frame) {
  var r = frame.getBoundingClientRect();
  var st = styleOf(frame);
  return {x: r.left + parseFloat(st.borderLeftWidth || '0') + parseFloat(st.paddingLeft || '0'),
          y: r.top + parseFloat(st.borderTopWidth || '0') + parseFloat(st.paddingTop || '0')};
}

// The element's rectangle in the top page's viewport, through the frames it is in.
function topRect(el) {
  var r = el.getBoundingClientRect();
  var x = r.left, y = r.top;
  for (var w = viewOf(el); w && w !== window && w.frameElement; w = w.parent) {
    var o = frameOrigin(w.frameElement);
    x += o.x;
    y += o.y;
  }
  return {x: x, y: y, w: r.width, h: r.height};
}

// ---- the walk ------------------------------------------------------------------------------

// Visits the page as ui_snapshot lists it: visit(el, info, depth) for each listed element and
// text(text, depth, block) for each run of text outside one. Hidden subtrees are left out; open
// shadow roots are walked through their slots and same-origin frames entered.
function walkPage(visit, text) {
  var root = document.body || document.documentElement;
  if (root)
    withStyles(function () { walkChildren(root, 0, false, false); });

  function childrenOf(el) {
    if (el.shadowRoot)
      return el.shadowRoot.childNodes;
    if (el.localName === 'slot') {
      var a = el.assignedNodes({flatten: true});
      return a.length ? a : el.childNodes;
    }
    return el.childNodes;
  }

  function blockOf(n) {
    for (var p = parentOf(n); p; p = parentOf(p)) {
      var d = styleOf(p).display;
      if (d.indexOf('inline') !== 0 && d !== 'contents')
        return p;
    }
    return null;
  }

  function walkChildren(el, depth, skipText, pointerAbove) {
    var kids = childrenOf(el);
    for (var i = 0; i < kids.length; i++) {
      var n = kids[i];
      if (n.nodeType === 3) {
        if (skipText)
          continue;
        var t = collapse(n.data);
        var parent = t ? parentOf(n) : null;
        if (parent && styleOf(parent).visibility === 'visible')
          text(t, depth, blockOf(n));
      } else if (n.nodeType === 1)
        walkElement(n, depth, skipText, pointerAbove);
    }
  }

  function walkElement(el, depth, skipText, pointerAbove) {
    var tag = el.localName;
    if (tag === 'script' || tag === 'style' || tag === 'template' || tag === 'noscript' || tag === 'head')
      return;
    var st = styleOf(el);
    if (subtreeHidden(el, st) || offCanvas(el, st))
      return;
    var pointer = st.cursor === 'pointer';
    var control = labelledControl(el);
    var kind = control ? (control.type === 'radio' ? 'radio' : 'checkbox') : kindOf(el, st, pointerAbove);
    if (tag === 'input' && kind && kind !== 'file' && visuallyHidden(el, st) && el.labels && el.labels.length)
      kind = ''; // its label stands for it
    if (kind && shown(el, st)) {
      visit(el, {kind: kind, st: st, control: control}, depth);
      if (kind === 'frame') {
        if (sameOrigin(el) && el.contentDocument.body)
          walkChildren(el.contentDocument.body, depth + 1, false, false);
        return;
      }
      if (/^(input|select|textarea|canvas|img)$/.test(tag) || (kind === 'text' && !el.isContentEditable))
        return;
      // A control's text is its name or value; what it holds besides is walked for controls only.
      walkChildren(el, depth + 1, true, pointer || pointerAbove || kind === 'button' || kind === 'link');
      return;
    }
    // A label whose control is listed: its text names that control.
    walkChildren(el, depth, skipText || (tag === 'label' && !!el.control), pointer || pointerAbove);
  }
}

function describe(el, info) {
  var kind = info.kind;
  var src = info.control || el;
  var node = {k: keyFor(el), kind: kind, cls: clsOf(el), label: nameOf(src, kind)};
  if (info.control && !node.label)
    node.label = clip(textWithout(el, info.control), NAME_MAX);
  if (!node.label && !el.id)
    node.cls = clsWithClass(el);
  var v = valueOf(src, kind);
  if (v && v !== node.label)
    node.value = v;
  // The id as the CSS selector that finds it, so it is not read as the element's name attribute.
  if (el.id)
    node.name = '#' + CSS.escape(el.id);
  var c = checkedOf(el, kind);
  if (c >= 0)
    node.checked = c;
  if (disabledOf(src, info.st))
    node.enabled = false;
  var note = noteOf(src, kind);
  if (kind === 'frame' && !sameOrigin(el) && !relayFrame(el))
    note = 'cross-origin page, not readable';
  if (note)
    node.note = note;
  var r = topRect(el);
  node.x = Math.round(r.x);
  node.y = Math.round(r.y);
  node.w = Math.round(r.w);
  node.h = Math.round(r.h);
  return node;
}

function matchesQuery(node, q) {
  if (!q)
    return true;
  return lower(node.label || '').indexOf(q) >= 0 || lower(node.value || '').indexOf(q) >= 0 ||
         lower(node.name || '').indexOf(q) >= 0 || lower(node.cls || '').indexOf(q) >= 0 || node.kind === q;
}

// ---- plugin configuration frames ----------------------------------------------------------------

var relay = {next: 1, replies: {}, rounds: {}};

window.addEventListener('message', function (event) {
  var d = event.data;
  if (!d || d.__orca !== 'mcp_reply' || !relay.replies.hasOwnProperty(d.id))
    return;
  var frame = relay.replies[d.id].frame;
  if (!frame.isConnected || event.source !== frame.contentWindow)
    return;
  relay.replies[d.id].reply = d.reply;
});

// Requests a frame never answered, given up once their caller stopped waiting (a second past the
// request's deadline, which a script calling the agent directly may not give: then the tools'
// default of 10 s), so a hung frame leaves nothing behind.
function prunePending() {
  var now = Date.now();
  Object.keys(relay.replies).forEach(function (id) {
    var r = relay.replies[id];
    if (now > r.expires + 1000)
      delete relay.replies[id];
  });
}

function relaySend(frame, req) {
  prunePending();
  var id = relay.next++;
  relay.replies[id] = {frame: frame, reply: null, expires: req.expires || Date.now() + 10000};
  // A frame that lost its window since it was found is never answered, as a hung one.
  var target = frame.contentWindow;
  if (target)
    target.postMessage({__orca: 'mcp', id: id, v: api.v, src: api.src, req: req}, '*');
  return id;
}

function relayTake(id) {
  var r = relay.replies[id];
  if (!r || !r.reply)
    return null;
  delete relay.replies[id];
  return r.reply;
}

// The reply of `frame` to `req` for this round: sent once per round, null until it is back.
function relayRound(frame, round, req) {
  var key = keyFor(frame);
  var slot = relay.rounds[key];
  if (!slot || slot.round !== round || slot.op !== req.op) {
    // The last round's request, if the frame never answered it, is given up; a late reply is ignored.
    if (slot)
      delete relay.replies[slot.id];
    slot = relay.rounds[key] = {round: round, op: req.op, id: relaySend(frame, req), reply: null};
  }
  if (!slot.reply)
    slot.reply = relayTake(slot.id);
  return slot.reply;
}

// ---- snapshot and find ---------------------------------------------------------------------------

function snapshot(req) {
  prune();
  var max = req.max > 0 ? req.max : 300;
  var q = lower(req.query || '');
  var nodes = [];
  var omitted = 0;
  var pending = false;
  var focus = deepActive(document);
  var run = null; // the text run being merged: {node, block, depth}
  function add(node) {
    if (!matchesQuery(node, q))
      return;
    if (nodes.length >= max) {
      ++omitted;
      return;
    }
    nodes.push(node);
  }
  function flush() {
    if (run) {
      run.node.label = clip(run.node.label, TEXT_MAX);
      add(run.node);
    }
    run = null;
  }
  walkPage(function (el, info, depth) {
    flush();
    var node = describe(el, info);
    node.depth = depth;
    if (el === focus)
      node.focused = true;
    add(node);
    if (info.kind === 'frame' && relayFrame(el)) {
      var reply = relayRound(el, req.round, {op: 'snapshot', max: max, query: req.query, expires: req.expires});
      if (!reply && req.last)
        node.note = 'its page did not answer';
      else if (!reply)
        pending = true;
      else if (reply.error)
        node.note = 'its page did not answer: ' + reply.error;
      else {
        var o = frameOrigin(el);
        reply.nodes.forEach(function (n) {
          n.depth += depth + 1;
          if (n.k)
            n.k = node.k + '/' + reply.doc + '/' + n.k;
          n.x = Math.round(n.x + o.x);
          n.y = Math.round(n.y + o.y);
          if (nodes.length < max)
            nodes.push(n);
          else
            ++omitted;
        });
        omitted += reply.omitted || 0;
      }
    }
  }, function (t, depth, block) {
    if (run && run.block === block && run.depth === depth) {
      run.node.label += ' ' + t;
      return;
    }
    flush();
    var r = block ? topRect(block) : {x: 0, y: 0, w: 0, h: 0};
    // A text run is acted on through the block it is in, so a person's click on text (a list item
    // with a handler the walk can't see) can be made.
    run = {block: block, depth: depth,
           node: {kind: 'label', label: t, depth: depth, x: Math.round(r.x), y: Math.round(r.y), w: Math.round(r.w), h: Math.round(r.h)}};
    if (block)
      run.node.k = keyFor(block);
  });
  flush();
  var out = {doc: docToken, url: String(location.href), title: document.title, ready: document.readyState,
             dpr: window.devicePixelRatio || 1, nodes: nodes, omitted: omitted};
  if (pending)
    out.pending = true;
  return out;
}

// Every listed element in page order, with what locators match on, and the relay frames. A run of
// text is listed as snapshot() lists it, a label standing for the block it is in.
function listed() {
  var out = [];
  var run = null; // the text run being merged: {block, depth, label}
  function flush() {
    if (run && run.block)
      out.push({el: run.block, kind: 'label', label: clip(run.label, TEXT_MAX), text: true});
    run = null;
  }
  walkPage(function (el, info) {
    flush();
    var label = nameOf(info.control || el, info.kind);
    if (info.control && !label)
      label = textWithout(el, info.control);
    out.push({el: el, kind: info.kind, label: label});
  }, function (t, depth, block) {
    if (run && run.block === block && run.depth === depth) {
      run.label += ' ' + t;
      return;
    }
    flush();
    run = {block: block, depth: depth, label: t};
  });
  flush();
  return out;
}

function cssMatches(css) {
  var set = new Set();
  var roots = [document];
  for (var i = 0; i < roots.length; i++) {
    var hits = roots[i].querySelectorAll(css);
    for (var j = 0; j < hits.length; j++)
      set.add(hits[j]);
    var all = roots[i].querySelectorAll('*');
    for (var k = 0; k < all.length; k++) {
      if (all[k].shadowRoot)
        roots.push(all[k].shadowRoot);
      if (isFrame(all[k]) && sameOrigin(all[k]))
        roots.push(all[k].contentDocument);
    }
  }
  return set;
}

function filterLocator(items, loc) {
  if (loc.css) {
    var set;
    try {
      set = cssMatches(loc.css);
    } catch (e) {
      fail('locator', 'Invalid CSS selector ' + JSON.stringify(loc.css));
    }
    items = items.filter(function (it) { return set.has(it.el); });
  }
  if (loc.kind)
    items = items.filter(function (it) { return it.kind === loc.kind; });
  // The label exactly, else case aside, else as part of the label (a card listed by all its text).
  if (loc.web !== undefined && loc.web !== null) {
    var want = normalize(loc.web);
    var hits = items.filter(function (it) { return it.label === String(loc.web); });
    if (!hits.length)
      hits = items.filter(function (it) { return normalize(it.label) === want; });
    if (!hits.length && want)
      hits = items.filter(function (it) { return normalize(it.label).indexOf(want) >= 0; });
    items = hits;
  }
  // A run of text answers only when no element does, so a sentence repeating a button's label never
  // makes the button ambiguous, and the runs of one block are one match.
  var elements = items.filter(function (it) { return !it.text; });
  if (elements.length)
    return elements;
  var blocks = new Set();
  return items.filter(function (it) {
    if (blocks.has(it.el))
      return false;
    blocks.add(it.el);
    return true;
  });
}

// The listed elements a locator {css?, web?, kind?} names, in page order, the relay frames' own
// matches in their place. Null while a frame has yet to answer this round.
function find(req) {
  var loc = req.locator || {};
  var items = listed();
  var matches = [];
  var pending = false;
  var silent = 0; // relayed frames silent on the last round or failing the search: they may hold more matches
  var own = new Set(filterLocator(items, loc));
  items.forEach(function (it) {
    if (own.has(it))
      matches.push({k: keyFor(it.el), kind: it.kind, label: it.label});
    if (it.kind === 'frame' && relayFrame(it.el)) {
      var reply = relayRound(it.el, req.round, {op: 'find', locator: loc, expires: req.expires});
      if (!reply && !req.last)
        pending = true;
      else if (!reply || reply.error)
        ++silent;
      else if (reply.matches) {
        var fk = keyFor(it.el);
        reply.matches.forEach(function (m) { matches.push({k: fk + '/' + reply.doc + '/' + m.k, kind: m.kind, label: m.label}); });
      }
    }
  });
  var out = {doc: docToken, matches: matches};
  if (pending)
    out.pending = true;
  if (silent)
    out.unanswered = silent;
  return out;
}

function cssPath(el) {
  var parts = [];
  for (var n = el; n && n.nodeType === 1 && n.localName !== 'html'; n = n.parentElement) {
    if (n.id && n.ownerDocument.querySelectorAll('#' + CSS.escape(n.id)).length === 1) {
      parts.unshift('#' + CSS.escape(n.id));
      break;
    }
    var i = 1;
    for (var s = n.previousElementSibling; s; s = s.previousElementSibling)
      if (s.localName === n.localName)
        ++i;
    parts.unshift(n.localName + ':nth-of-type(' + i + ')');
  }
  return parts.join(' > ');
}

// The locator that finds `el` again within this page, and its index among its matches (-1 alone).
function locate(el, kind, label) {
  var loc;
  if (el.id && el.ownerDocument === document && document.querySelectorAll('#' + CSS.escape(el.id)).length === 1)
    loc = {css: '#' + CSS.escape(el.id)};
  else if (label)
    loc = {web: label, kind: kind};
  else
    loc = {css: cssPath(el)};
  var matches = filterLocator(listed(), loc);
  var index = -1;
  if (matches.length > 1)
    for (var i = 0; i < matches.length; i++)
      if (matches[i].el === el)
        index = i;
  return {loc: loc, index: index};
}

// ---- events ---------------------------------------------------------------------------------

function modifiersOf(m) {
  m = m || {};
  return {ctrlKey: !!m.ctrl, shiftKey: !!m.shift, altKey: !!m.alt, metaKey: !!m.meta};
}

var held = null; // a press `down` left: {button, mods, el, at, press}, held until its `up`; `press` is the tools' id for it
var POINTER_ID = 7801; // the synthetic pointer's id, unlike any real one

// Pointer capture for the synthetic pointer: a constructed pointer event makes no pointer active, so
// the browser's setPointerCapture() would throw for its id. While a press is held the element
// prototype's capture methods answer for that id as the Pointer Events capture rules say, and pass
// any other id to the originals.
var capture = null; // {proto, orig, target, pending}

function captureStart(view) {
  if (capture || !view.Element || !view.Element.prototype.setPointerCapture)
    return;
  var proto = view.Element.prototype;
  var orig = {set: proto.setPointerCapture, has: proto.hasPointerCapture, release: proto.releasePointerCapture};
  capture = {proto: proto, orig: orig, target: null, pending: null};
  proto.setPointerCapture = function (id) {
    if (id !== POINTER_ID)
      return orig.set.apply(this, arguments);
    if (capture)
      capture.pending = this;
  };
  proto.hasPointerCapture = function (id) {
    if (id !== POINTER_ID)
      return orig.has.apply(this, arguments);
    return !!capture && (capture.pending ? capture.pending === this : capture.target === this);
  };
  proto.releasePointerCapture = function (id) {
    if (id !== POINTER_ID)
      return orig.release.apply(this, arguments);
    if (capture && (capture.pending === this || capture.target === this))
      capture.pending = false;
  };
}

function captureEvent(el, type) {
  var view = viewOf(el);
  var Ctor = view.PointerEvent || view.MouseEvent;
  el.dispatchEvent(new Ctor(type, {bubbles: true, composed: true, pointerId: POINTER_ID, pointerType: 'mouse', isPrimary: true}));
}

// The capture rules' "process pending pointer capture", before each pointer event.
function captureProcess() {
  if (!capture || capture.pending === null)
    return;
  var next = capture.pending || null;
  capture.pending = null;
  if (next === capture.target)
    return;
  var old = capture.target;
  capture.target = next;
  if (old)
    captureEvent(old, 'lostpointercapture');
  if (next)
    captureEvent(next, 'gotpointercapture');
}

// After the press's pointerup: the capture is released and the wrappers removed.
function captureEnd() {
  if (!capture)
    return;
  capture.pending = false;
  captureProcess();
  capture.proto.setPointerCapture = capture.orig.set;
  capture.proto.hasPointerCapture = capture.orig.has;
  capture.proto.releasePointerCapture = capture.orig.release;
  capture = null;
}

function mouse(el, type, at, button, detail, mods) {
  var view = viewOf(el);
  var pointer = type.indexOf('pointer') === 0;
  var boundary = /over$|out$|enter$|leave$/.test(type);
  if (capture && !boundary && type !== 'click' && type !== 'contextmenu' && type !== 'auxclick' && type !== 'dblclick') {
    if (pointer)
      captureProcess();
    if (capture.target)
      el = capture.target;
  }
  var Ctor = pointer && view.PointerEvent ? view.PointerEvent : view.MouseEvent;
  var pressed = held ? (held.button === 2 ? 2 : held.button === 1 ? 4 : 1) : 0;
  var buttons = /down$/.test(type) ? (button === 2 ? 2 : button === 1 ? 4 : 1) : /up$/.test(type) ? 0 : pressed;
  var init = {bubbles: !/enter$|leave$/.test(type), cancelable: true, composed: true, view: view, detail: detail,
              clientX: at.x, clientY: at.y, screenX: at.x, screenY: at.y, button: button, buttons: buttons};
  var m = modifiersOf(mods);
  for (var k in m)
    init[k] = m[k];
  if (pointer) {
    init.pointerId = POINTER_ID;
    init.pointerType = 'mouse';
    init.isPrimary = true;
  }
  return el.dispatchEvent(new Ctor(type, init));
}

function focusable(n) {
  if (browserDisabled(n))
    return false;
  if (n.isContentEditable)
    return true;
  if (n.hasAttribute && n.hasAttribute('tabindex'))
    return true;
  return (n.localName === 'a' && n.hasAttribute('href')) || /^(button|input|select|textarea|summary|iframe)$/.test(n.localName);
}

function focusFor(el) {
  for (var n = el; n && n.nodeType === 1; n = parentOf(n))
    if (focusable(n) && typeof n.focus === 'function') {
      n.focus({preventScroll: true});
      return n;
    }
  var a = deepActive(el.ownerDocument);
  if (a && a !== el.ownerDocument.body && typeof a.blur === 'function')
    a.blur();
  return null;
}

function composedContains(outer, n) {
  for (; n; n = n.parentNode || n.host)
    if (n === outer)
      return true;
  return false;
}

function infoOf(el) {
  return withStyles(function () {
    var st = styleOf(el);
    var control = labelledControl(el);
    var p = parentOf(el);
    var pointerAbove = !!(p && p.nodeType === 1 && styleOf(p).cursor === 'pointer');
    var kind = control ? (control.type === 'radio' ? 'radio' : 'checkbox') : kindOf(el, st, pointerAbove) || 'button';
    var label = nameOf(control || el, kind);
    if (control && !label)
      label = clip(textWithout(el, control), NAME_MAX);
    return {kind: kind, st: st, control: control, label: label};
  });
}

// What a message calls an element: its kind and label when it has them, and its tag and id.
function brief(el) {
  return withStyles(function () {
    var p    = parentOf(el);
    var kind = kindOf(el, styleOf(el), !!(p && p.nodeType === 1 && styleOf(p).cursor === 'pointer'));
    var name = kind ? nameOf(el, kind) : '';
    return (kind ? kind + ' ' : '') + (name ? '"' + clip(name, 40) + '" ' : '') + '(' + (el.id ? el.localName + '#' + el.id : clsWithClass(el)) + ')';
  });
}

function reveal(el) {
  if (typeof el.scrollIntoView === 'function')
    el.scrollIntoView({block: 'nearest', inline: 'nearest'});
}

// The point a pointer action lands on, in `el`'s own frame: its centre or (dx, dy) from its top
// left, after scrolling it into view. With `hitTest`, refused when something else is drawn there.
function aimAt(el, dx, dy, hitTest) {
  reveal(el);
  var r = el.getBoundingClientRect();
  // What a press lands on must be the element; a move or release may be anywhere (a drag's).
  if (hitTest && ((dx !== undefined && (dx < 0 || dx > r.width)) || (dy !== undefined && (dy < 0 || dy > r.height))))
    fail('point', 'The point x, y is outside the element: they count from its top left, within the width and height ' +
         'ui_snapshot lists for it');
  var at = {x: dx === undefined ? r.left + r.width / 2 : r.left + dx, y: dy === undefined ? r.top + r.height / 2 : r.top + dy};
  if (hitTest) {
    var root = el.getRootNode ? el.getRootNode() : el.ownerDocument;
    var hit = (root.elementFromPoint ? root : el.ownerDocument).elementFromPoint(at.x, at.y);
    if (!hit)
      fail('covered', 'The element is outside the page\'s visible area even after scrolling');
    if (!composedContains(el, hit) && !(hit.localName === 'label' && hit.control === el))
      fail('covered', 'The element is covered by ' + brief(hit) + ' there; close or scroll what is above it first');
  }
  return at;
}

function clickEvents(el, at, opts) {
  var button = opts.button === 'right' ? 2 : opts.button === 'middle' ? 1 : 0;
  var mods = opts.modifiers;
  mouse(el, 'pointerover', at, 0, 0, mods);
  mouse(el, 'pointerenter', at, 0, 0, mods);
  mouse(el, 'mouseover', at, 0, 0, mods);
  mouse(el, 'mouseenter', at, 0, 0, mods);
  mouse(el, 'pointermove', at, 0, 0, mods);
  mouse(el, 'mousemove', at, 0, 0, mods);
  for (var i = 1; i <= (opts.double ? 2 : 1); i++) {
    captureStart(viewOf(el));
    if (mouse(el, 'pointerdown', at, button, i, mods) && mouse(el, 'mousedown', at, button, i, mods) && i === 1)
      focusFor(el);
    mouse(el, 'pointerup', at, button, i, mods);
    mouse(el, 'mouseup', at, button, i, mods);
    captureEnd();
    if (button === 0)
      mouse(el, 'click', at, 0, i, mods);
    else if (button === 1)
      mouse(el, 'auxclick', at, 1, i, mods);
  }
  if (button === 2)
    mouse(el, 'contextmenu', at, 2, 1, mods);
  if (opts.double && button === 0)
    mouse(el, 'dblclick', at, 0, 2, mods);
}

function fire(el, type, init) {
  var view = viewOf(el);
  var Ctor = (type === 'input' || type === 'beforeinput') && view.InputEvent ? view.InputEvent : view.Event;
  init = init || {};
  init.bubbles = true;
  init.cancelable = type !== 'input' && type !== 'change';
  init.composed = true;
  return el.dispatchEvent(new Ctor(type, init));
}

function setNative(el, value) {
  var view = viewOf(el);
  var proto = el.localName === 'textarea' ? view.HTMLTextAreaElement.prototype :
              el.localName === 'select' ? view.HTMLSelectElement.prototype : view.HTMLInputElement.prototype;
  var desc = Object.getOwnPropertyDescriptor(proto, 'value');
  if (desc && desc.set)
    desc.set.call(el, value);
  else
    el.value = value;
}

function isTextField(el) {
  if (!el)
    return false;
  if (el.localName === 'textarea')
    return true;
  return el.localName === 'input' && !/^(checkbox|radio|range|button|submit|reset|image|file|hidden|color)$/.test(lower(el.type));
}

function centre(el) {
  var r = el.getBoundingClientRect();
  return {x: r.left + r.width / 2, y: r.top + r.height / 2};
}

// ---- input -----------------------------------------------------------------------------------

function setValue(el, info, value, submit) {
  var target = info.control || el;
  var tag = target.localName;
  var kind = info.kind;
  if (kind === 'file')
    fail('refused', 'A file input cannot be set from automation');
  if (kind === 'checkbox' || kind === 'radio' || kind === 'toggle') {
    if (typeof value !== 'boolean')
      fail('value', 'A ' + kind + ' takes true or false');
    var now = checkedOf(el, kind) === 1;
    if (kind === 'radio' && !value && now)
      fail('value', 'A radio button is cleared by choosing another one in its group');
    if (now !== value)
      clickEvents(el, aimAt(el, undefined, undefined, true), {});
    return;
  }
  reveal(target);
  if (tag === 'select') {
    var index = -1;
    if (typeof value === 'number')
      index = value;
    else {
      var want = String(value);
      for (var i = 0; i < target.options.length && index < 0; i++)
        if (collapse(target.options[i].text) === want || target.options[i].value === want)
          index = i;
      for (var j = 0; j < target.options.length && index < 0; j++)
        if (normalize(target.options[j].text) === normalize(want))
          index = j;
    }
    if (index < 0 || index >= target.options.length)
      fail('value', 'No option ' + JSON.stringify(value) + '; the options are ' +
           Array.prototype.map.call(target.options, function (o) { return JSON.stringify(collapse(o.text)); }).join(', '));
    if (target.options[index].disabled)
      fail('value', 'The option ' + JSON.stringify(collapse(target.options[index].text)) + ' is disabled');
    focusFor(target);
    target.selectedIndex = index;
    fire(target, 'input');
    fire(target, 'change');
    if (submit)
      target.blur();
    return;
  }
  if (tag === 'input' || tag === 'textarea') {
    if (target.readOnly)
      fail('refused', 'The field is read-only');
    var text = String(value);
    focusFor(target);
    if (isTextField(target) && typeof target.select === 'function')
      target.select();
    setNative(target, text);
    fire(target, 'input', {inputType: 'insertText', data: text});
    fire(target, 'change');
    if (submit)
      target.blur();
    return;
  }
  if (target.isContentEditable) {
    var t = String(value);
    focusFor(target);
    var sel = viewOf(target).getSelection();
    var range = target.ownerDocument.createRange();
    range.selectNodeContents(target);
    sel.removeAllRanges();
    sel.addRange(range);
    var done = false;
    try {
      done = target.ownerDocument.execCommand('insertText', false, t);
    } catch (e) {
    }
    if (!done || innerTextOf(target) !== collapse(t)) {
      target.textContent = t;
      fire(target, 'input', {inputType: 'insertText', data: t});
    }
    if (submit)
      target.blur();
    return;
  }
  fail('refused', 'This ' + kind + ' is not a form control; click or drag it instead');
}

// ---- keys ------------------------------------------------------------------------------------

function keyEvent(el, type, k, mods) {
  var view = viewOf(el);
  var init = {key: k.key, code: k.code, bubbles: true, cancelable: true, composed: true, view: view};
  var m = modifiersOf(mods);
  for (var p in m)
    init[p] = m[p];
  var e = new view.KeyboardEvent(type, init);
  var code = type === 'keypress' ? k.charCode : k.keyCode;
  try {
    Object.defineProperty(e, 'keyCode', {get: function () { return code; }});
    Object.defineProperty(e, 'which', {get: function () { return code; }});
    if (type === 'keypress')
      Object.defineProperty(e, 'charCode', {get: function () { return k.charCode; }});
  } catch (err) {
  }
  return el.dispatchEvent(e);
}

// Takes typed text: a field that is not read-only or disabled, or editable content.
function editable(el) { return (isTextField(el) && !el.readOnly && !el.disabled) || !!(el && el.isContentEditable); }

// The selection in editable content, extended by one character backwards or forwards when it is
// collapsed (for a delete), or null without one.
function editRange(el, extend) {
  var sel = viewOf(el).getSelection ? viewOf(el).getSelection() : null;
  if (!sel || !sel.rangeCount)
    return null;
  if (extend && sel.isCollapsed) {
    if (typeof sel.modify === 'function')
      sel.modify('extend', extend, 'character');
    else
      return charBeside(el, sel.getRangeAt(0), extend === 'backward') || sel.getRangeAt(0);
  }
  return sel.getRangeAt(0);
}

// Without Selection.modify: the range of the character before (or after) the caret `at` in `el`'s
// text, across its text nodes, a surrogate pair whole; null at an end.
function charBeside(el, at, backward) {
  var doc = el.ownerDocument;
  var texts = [];
  var walker = doc.createTreeWalker(el, 4); // NodeFilter.SHOW_TEXT
  for (var n = walker.nextNode(); n; n = walker.nextNode())
    texts.push(n);
  if (backward)
    texts.reverse();
  for (var i = 0; i < texts.length; i++) {
    var t = texts[i], len = t.data.length, offset;
    if (t === at.startContainer)
      offset = at.startOffset;
    else if (backward)
      offset = at.comparePoint(t, len) <= 0 ? len : -1; // wholly before the caret
    else
      offset = at.comparePoint(t, 0) >= 0 ? 0 : -1; // wholly after it
    var span = offset < 0 ? null : charSpan(t.data, offset, backward ? -1 : 1);
    if (span && span[1] > span[0]) {
      var r = doc.createRange();
      r.setStart(t, span[0]);
      r.setEnd(t, span[1]);
      return r;
    }
  }
  return null;
}

// The [from, to] UTF-16 offsets of the character before (`dir` -1) or after (1) offset `at` of `s`:
// a surrogate pair whole, also when `at` falls between its halves; empty at an end.
function charSpan(s, at, dir) {
  var high = at > 0 && at < s.length && s.charCodeAt(at - 1) >= 0xD800 && s.charCodeAt(at - 1) <= 0xDBFF &&
             s.charCodeAt(at) >= 0xDC00 && s.charCodeAt(at) <= 0xDFFF;
  if (high)
    return [at - 1, at + 1];
  return dir < 0 ? [at - charLength(s, at, -1), at] : [at, at + charLength(s, at, 1)];
}

// The UTF-16 units of the character before (`dir` -1) or after (1) offset `at` of `s`: 2 for a
// surrogate pair, 0 at an end.
function charLength(s, at, dir) {
  if (dir < 0) {
    if (at <= 0)
      return 0;
    var low = s.charCodeAt(at - 1);
    return at >= 2 && low >= 0xDC00 && low <= 0xDFFF && s.charCodeAt(at - 2) >= 0xD800 && s.charCodeAt(at - 2) <= 0xDBFF ? 2 : 1;
  }
  if (at >= s.length)
    return 0;
  var high = s.charCodeAt(at);
  return high >= 0xD800 && high <= 0xDBFF && at + 1 < s.length && s.charCodeAt(at + 1) >= 0xDC00 && s.charCodeAt(at + 1) <= 0xDFFF ? 2 : 1;
}

function insertText(el, t) {
  if (el.isContentEditable) {
    var ok = false;
    try {
      ok = el.ownerDocument.execCommand('insertText', false, t);
    } catch (e) {
    }
    // Where the command is unavailable or refuses, the text goes in at the selection as the command would.
    if (!ok && fire(el, 'beforeinput', {inputType: 'insertText', data: t})) {
      var range = editRange(el);
      if (range) {
        range.deleteContents();
        var node = el.ownerDocument.createTextNode(t);
        range.insertNode(node);
        range.setStartAfter(node);
        range.collapse(true);
        var sel = viewOf(el).getSelection();
        sel.removeAllRanges();
        sel.addRange(range);
      } else
        el.appendChild(el.ownerDocument.createTextNode(t));
      fire(el, 'input', {inputType: 'insertText', data: t});
    }
    return;
  }
  if (!fire(el, 'beforeinput', {inputType: 'insertText', data: t}))
    return;
  var start = el.selectionStart, end = el.selectionEnd;
  if (typeof start === 'number' && typeof el.setRangeText === 'function')
    el.setRangeText(t, start, end, 'end');
  else
    setNative(el, String(el.value) + t);
  fire(el, 'input', {inputType: 'insertText', data: t});
}

function deleteText(el, forward) {
  if (el.isContentEditable) {
    var ok = false;
    try {
      ok = el.ownerDocument.execCommand(forward ? 'forwardDelete' : 'delete');
    } catch (e) {
    }
    var type = forward ? 'deleteContentForward' : 'deleteContentBackward';
    if (!ok && fire(el, 'beforeinput', {inputType: type})) {
      var range = editRange(el, forward ? 'forward' : 'backward');
      if (range && !range.collapsed) {
        range.deleteContents();
        fire(el, 'input', {inputType: type});
      }
    }
    return;
  }
  var start = el.selectionStart, end = el.selectionEnd;
  if (typeof start !== 'number')
    return;
  // One character, a surrogate pair (an emoji) whole.
  if (start === end) {
    var span = charSpan(String(el.value), start, forward ? 1 : -1);
    start = span[0];
    end = span[1];
  }
  if (start === end)
    return;
  var type = forward ? 'deleteContentForward' : 'deleteContentBackward';
  if (!fire(el, 'beforeinput', {inputType: type}))
    return;
  el.setRangeText('', start, end, 'end');
  fire(el, 'input', {inputType: type});
}

// The page's focus order, as Tab follows it: positive tabindex first, then the composed tree's order
// (an open shadow root's content where its host is, slotted elements where their slot is), into
// same-origin frames where they are. Disabled controls, a disabled fieldset's included, take no focus.
function tabbables(d) {
  var all = [];
  (function collect(node) {
    var kids = node.shadowRoot ? node.shadowRoot.childNodes : node.localName === 'slot' && node.assignedNodes({flatten: true}).length ?
               node.assignedNodes({flatten: true}) : node.childNodes;
    Array.prototype.forEach.call(kids, function (n) {
      if (n.nodeType !== 1)
        return;
      if (isFrame(n)) {
        if (sameOrigin(n) && n.contentDocument.body && !hiddenFromView(n))
          collect(n.contentDocument.body);
        return;
      }
      // Only what the browser itself disables: aria-disabled and pointer-events leave Tab alone.
      if (focusable(n) && n.tabIndex >= 0 && !hiddenFromView(n) && !browserDisabled(n))
        all.push(n);
      collect(n);
    });
  })(d.body || d.documentElement);
  var positive = all.filter(function (n) { return n.tabIndex > 0; }).sort(function (a, b) { return a.tabIndex - b.tabIndex; });
  return positive.concat(all.filter(function (n) { return n.tabIndex === 0; }));
}

function moveFocus(from, back) {
  var list = tabbables(document);
  if (!list.length)
    return;
  var at = list.indexOf(from);
  var next = at < 0 ? (back ? list.length - 1 : 0) : (at + (back ? list.length - 1 : 1)) % list.length;
  list[next].focus();
}

function activates(el) {
  var tag = el.localName;
  return tag === 'button' || tag === 'summary' || (tag === 'a' && el.hasAttribute('href')) ||
         (tag === 'input' && /^(button|submit|reset|image|checkbox|radio)$/.test(lower(el.type)));
}

// HTML's implicit submission: Enter in a form field clicks the form's default button; a form
// without one is submitted when it has only one field that blocks implicit submission.
function implicitSubmit(field) {
  var form = field.form;
  if (!form)
    return;
  var button = null;
  for (var i = 0; i < form.elements.length && !button; i++) {
    var e = form.elements[i];
    if ((e.localName === 'button' && lower(e.type || 'submit') === 'submit') || (e.localName === 'input' && /^(submit|image)$/.test(lower(e.type))))
      button = e;
  }
  if (button) {
    if (!button.disabled)
      clickEvents(button, centre(button), {});
    return;
  }
  var blocking = Array.prototype.filter.call(form.elements, function (e) {
    return e.localName === 'input' && /^(text|search|url|tel|email|password|date|month|week|time|datetime-local|number)$/.test(lower(e.type || 'text'));
  });
  if (blocking.length > 1)
    return;
  if (typeof form.requestSubmit === 'function')
    form.requestSubmit();
  else {
    // Without requestSubmit (Safari 15): its constraint validation first, as an implicit submission
    // validates and form.submit() would not.
    if (!form.noValidate && typeof form.reportValidity === 'function' && !form.reportValidity())
      return;
    if (form.dispatchEvent(new (viewOf(form).Event)('submit', {bubbles: true, cancelable: true})))
      form.submit();
  }
}

function stepValue(el, up) {
  // A disabled control, or a read-only number field, keeps its value (taken as handled).
  if (el.disabled || (el.localName === 'input' && el.type === 'number' && el.readOnly))
    return /^(select|input)$/.test(el.localName);
  if (el.localName === 'select') {
    var i = el.selectedIndex + (up ? -1 : 1);
    if (i >= 0 && i < el.options.length) {
      el.selectedIndex = i;
      fire(el, 'input');
      fire(el, 'change');
    }
    return true;
  }
  if (el.localName === 'input' && (el.type === 'range' || el.type === 'number')) {
    try {
      if (up) el.stepUp(); else el.stepDown();
    } catch (e) {
      return true;
    }
    fire(el, 'input');
    fire(el, 'change');
    return true;
  }
  if (el.localName === 'input' && el.type === 'radio' && el.name) {
    var group = Array.prototype.filter.call(el.ownerDocument.getElementsByName(el.name), function (n) {
      return n.type === 'radio' && !n.disabled;
    });
    var next = group[(group.indexOf(el) + (up ? group.length - 1 : 1)) % group.length];
    if (next && next !== el) {
      next.focus();
      clickEvents(next, centre(next), {});
    }
    return true;
  }
  return false;
}

// One key press on `el`: keydown, keypress for characters and Enter, the browser's default action
// when the page does not cancel them, keyup, and a button's activation by Space after it.
function pressKey(el, k, mods) {
  var plain = !mods || (!mods.ctrl && !mods.alt && !mods.meta);
  if (!keyEvent(el, 'keydown', k, mods)) {
    keyEvent(el, 'keyup', k, mods);
    return;
  }
  var printable = k.text && plain;
  if (printable || k.key === 'Enter') {
    var charCode = printable ? k.charCode : 13;
    if (!keyEvent(el, 'keypress', {key: k.key, code: k.code, keyCode: charCode, charCode: charCode}, mods)) {
      keyEvent(el, 'keyup', k, mods);
      return;
    }
  }
  if (printable) {
    if (editable(el) && !(k.key === ' ' && activates(el)))
      insertText(el, k.text);
  } else {
    switch (k.key) {
    case 'Enter':
      if (el.localName === 'textarea') {
        if (editable(el))
          insertText(el, '\n');
      } else if (el.isContentEditable)
        el.ownerDocument.execCommand('insertParagraph');
      else if (isTextField(el) || (el.localName === 'input' && el.form && !activates(el)))
        implicitSubmit(el);
      else if (activates(el) && !(el.localName === 'input' && /^(checkbox|radio)$/.test(el.type)))
        clickEvents(el, centre(el), {});
      break;
    case 'Tab':
      moveFocus(el, !!(mods && mods.shift));
      break;
    case 'Backspace': case 'Delete':
      if (editable(el))
        deleteText(el, k.key === 'Delete');
      break;
    case 'ArrowUp': case 'ArrowLeft': case 'ArrowDown': case 'ArrowRight': {
      var up = k.key === 'ArrowUp' || k.key === 'ArrowLeft';
      // A selection collapses to its start (Left) or end (Right); a caret moves by a character, so it
      // never lands inside a surrogate pair (an emoji).
      if (!stepValue(el, up) && isTextField(el) && typeof el.selectionStart === 'number' && /Left|Right/.test(k.key)) {
        var p;
        if (el.selectionStart !== el.selectionEnd)
          p = up ? el.selectionStart : el.selectionEnd;
        else {
          var span = charSpan(String(el.value), el.selectionStart, up ? -1 : 1);
          p = up ? span[0] : span[1];
        }
        el.setSelectionRange(p, p);
      }
      break;
    }
    case 'Home': case 'End':
      if (isTextField(el)) {
        var q = k.key === 'Home' ? 0 : String(el.value).length;
        el.setSelectionRange(q, q);
      }
      break;
    case 'a': case 'A':
      if (mods && (mods.ctrl || mods.meta) && isTextField(el))
        el.select();
      break;
    }
  }
  keyEvent(el, 'keyup', k, mods);
  if (k.key === ' ' && plain && activates(el))
    clickEvents(el, centre(el), {});
}

function charKey(c) {
  var upper = c.toUpperCase();
  var code = /^[a-z]$/i.test(c) ? 'Key' + upper : /^[0-9]$/.test(c) ? 'Digit' + c : c === ' ' ? 'Space' : '';
  var keyCode = /^[a-z0-9]$/i.test(c) ? upper.charCodeAt(0) : c === ' ' ? 32 : 0;
  return {key: c, code: code, keyCode: keyCode, charCode: c.codePointAt(0), text: c};
}

// ---- gestures ---------------------------------------------------------------------------------

function scroller(el) {
  for (var n = el; n && n.nodeType === 1; n = parentOf(n)) {
    var st = styleOf(n);
    if (/(auto|scroll|overlay)/.test(st.overflowY + st.overflowX) && (n.scrollHeight > n.clientHeight || n.scrollWidth > n.clientWidth))
      return n;
  }
  return el.ownerDocument.scrollingElement || el.ownerDocument.documentElement;
}

function underPointer(el, at) { return el.ownerDocument.elementFromPoint(at.x, at.y) || el; }

// CSS pixels per logical pixel of OrcaSlicer's window: `scale` is the window's content scale factor,
// which OrcaSlicer sends with a point.
function cssPerLogical(args) { return (args.scale || 1) / (window.devicePixelRatio || 1); }

// Ends the press a `down` left, as a gesture that replaces it does: its button comes up where it was
// pressed, with its modifiers.
function releaseHeld() {
  if (!held)
    return;
  var h = held;
  held = null;
  if (h.el && h.el.isConnected) {
    mouse(h.el, 'pointerup', h.at, h.button, 1, h.mods);
    mouse(h.el, 'mouseup', h.at, h.button, 1, h.mods);
  }
  captureEnd();
}

function gesture(el, args) {
  var action = args.action || 'click';
  var css = cssPerLogical(args);
  var button = args.button === 'right' ? 2 : args.button === 'middle' ? 1 : 0;
  if (action !== 'move' && action !== 'up')
    releaseHeld();
  // A `down`'s modifiers stay pressed with its button, so later moves and its `up` carry them too.
  var mods = args.modifiers || {};
  if (held && held.mods)
    mods = {ctrl: !!(mods.ctrl || held.mods.ctrl), shift: !!(mods.shift || held.mods.shift), alt: !!(mods.alt || held.mods.alt),
            meta: !!(mods.meta || held.mods.meta)};
  var start = aimAt(el, args.x === undefined ? undefined : args.x * css, args.y === undefined ? undefined : args.y * css,
                    action !== 'move' && action !== 'up' && action !== 'wheel');
  var r = el.getBoundingClientRect();
  function point(p) { return {x: r.left + p[0] * css, y: r.top + p[1] * css}; }
  if (action === 'click') {
    clickEvents(el, start, {button: args.button, double: args.double, modifiers: mods});
    return start;
  }
  if (action === 'move') {
    var over = underPointer(el, start);
    mouse(over, 'pointermove', start, 0, 0, mods);
    mouse(over, 'mousemove', start, 0, 0, mods);
    return start;
  }
  if (action === 'down') {
    captureStart(viewOf(el));
    mouse(el, 'pointerdown', start, button, 1, mods);
    mouse(el, 'mousedown', start, button, 1, mods);
    focusFor(el);
    held = {button: button, mods: mods, el: el, at: start, press: args.press || 0};
    return start;
  }
  if (action === 'up') {
    var target = underPointer(el, start);
    var b = held ? held.button : button;
    held = null;
    mouse(target, 'pointerup', start, b, 1, mods);
    mouse(target, 'mouseup', start, b, 1, mods);
    captureEnd();
    return start;
  }
  if (action === 'wheel') {
    var notches = args.delta || 0;
    var dy = -notches * NOTCH_PX;
    var hit = underPointer(el, start);
    var view = viewOf(el);
    var Wheel = view.WheelEvent || view.MouseEvent;
    var init = {bubbles: true, cancelable: true, composed: true, view: view, clientX: start.x, clientY: start.y, deltaX: 0, deltaY: dy, deltaMode: 0};
    var m = modifiersOf(mods);
    for (var k in m)
      init[k] = m[k];
    if (hit.dispatchEvent(new Wheel('wheel', init)))
      scroller(hit).scrollBy(0, dy);
    return start;
  }
  if (action === 'drag') {
    var stops = (args.path || []).concat(args.to ? [args.to] : []).map(point);
    if (!stops.length)
      fail('value', 'A drag needs `to`');
    var steps = Math.max(1, Math.min(200, args.steps || 10));
    captureStart(viewOf(el));
    mouse(el, 'pointerdown', start, button, 1, mods);
    mouse(el, 'mousedown', start, button, 1, mods);
    focusFor(el);
    held = {button: button, mods: mods};
    var from = start, at = start;
    stops.forEach(function (to) {
      for (var i = 1; i <= steps; i++) {
        at = {x: from.x + (to.x - from.x) * i / steps, y: from.y + (to.y - from.y) * i / steps};
        var over = underPointer(el, at);
        mouse(over, 'pointermove', at, button, 0, mods);
        mouse(over, 'mousemove', at, button, 0, mods);
      }
      from = to;
    });
    var end = underPointer(el, at);
    held = null;
    mouse(end, 'pointerup', at, button, 1, mods);
    mouse(end, 'mouseup', at, button, 1, mods);
    captureEnd();
    if (end === el || composedContains(el, end))
      mouse(el, 'click', at, button, 1, mods);
    return at;
  }
  fail('value', 'Unknown gesture ' + action);
}

// ---- holding the page's messages ------------------------------------------------------------------

// The objects the page posts to OrcaSlicer through: window.chrome.webview on WebView2 (which
// window.wx and every other handler name are), and on WebKit each window.webkit.messageHandlers.*
// that a window property names (window.wx, window.wipingDialog, ...).
function channels() {
  var out = [];
  function add(o) {
    if (o && typeof o.postMessage === 'function' && o !== window && out.indexOf(o) < 0)
      out.push(o);
  }
  try {
    add(window.chrome && window.chrome.webview);
  } catch (e) {
  }
  try {
    if (window.webkit && window.webkit.messageHandlers) {
      add(window.webkit.messageHandlers.wx);
      Object.keys(window).forEach(function (name) {
        var v = window[name];
        if (v && typeof v === 'object' && v.constructor && v.constructor.name === 'UserMessageHandler')
          add(v);
      });
    }
  } catch (e) {
  }
  try {
    add(window.wx);
  } catch (e) {
  }
  return out;
}

var flushes = {};
var nextFlush = 1;

// Runs fn with the page's messages held: {value, held, flush}, flush the id `flushed` answers for.
function holding(fn) {
  var queued = [];
  var patched = [];
  channels().forEach(function (ch) {
    var own = Object.prototype.hasOwnProperty.call(ch, 'postMessage');
    var orig = ch.postMessage;
    try {
      ch.postMessage = function (m) { queued.push([ch, orig, m]); };
      patched.push([ch, own, orig]);
    } catch (e) {
    }
  });
  var flush = 0;
  try {
    var value = fn();
    if (queued.length)
      flush = nextFlush++;
    return {value: value, held: queued.length, flush: flush};
  } finally {
    patched.forEach(function (p) {
      try {
        if (p[1])
          p[0].postMessage = p[2];
        else
          delete p[0].postMessage;
      } catch (e) {
      }
    });
    if (queued.length) {
      var id = flush || nextFlush++;
      flushes[id] = false;
      setTimeout(function () {
        queued.forEach(function (h) {
          try {
            h[1].call(h[0], h[2]);
          } catch (e) {
          }
        });
        flushes[id] = true;
      }, 0);
    }
  }
}

// ---- what an action changed ----------------------------------------------------------------------

var CHANGES_MAX = 20;
var WATCHES_MAX = 16;
var watches = {}; // outlines taken before actions, by the tools' id for the action: {at, lines}

// The page as ui_snapshot lists it, a line per element (kind, label, value, state and notes) and per
// run of text, and the configuration frames, whose agents outline their own pages.
function outline() {
  var lines = [];
  var frames = [];
  var run = null;
  function flush() {
    if (run)
      lines.push('label "' + clip(run.t, TEXT_MAX) + '"');
    run = null;
  }
  walkPage(function (el, info) {
    flush();
    var src = info.control || el;
    var label = nameOf(src, info.kind) || (info.control ? clip(textWithout(el, info.control), NAME_MAX) : '');
    var line = info.kind + (label ? ' "' + label + '"' : '');
    var v = valueOf(src, info.kind);
    if (v && v !== label)
      line += ' = "' + v + '"';
    var c = checkedOf(el, info.kind);
    if (c >= 0)
      line += c ? ' [checked]' : ' [unchecked]';
    if (disabledOf(src, info.st))
      line += ' [disabled]';
    var note = noteOf(src, info.kind);
    if (note)
      line += ' [' + note + ']';
    lines.push(line);
    if (info.kind === 'frame' && relayFrame(el))
      frames.push(el);
  }, function (t, depth, block) {
    if (run && run.block === block && run.depth === depth) {
      run.t += ' ' + t;
      return;
    }
    flush();
    run = {block: block, depth: depth, t: t};
  });
  flush();
  return {lines: lines, frames: frames};
}

// Outlines the page before the action req.watch, for `changes` to compare it with afterwards, and has
// each configuration frame outline its own page: a frame's page changes only by a message or by
// loading another document, and this request reaches it before any message the action sends it. An
// outline is kept until it is compared, a minute at most, and only the newest WATCHES_MAX are kept.
function watch(req) {
  if (!req.watch)
    return;
  var now = Date.now();
  Object.keys(watches).forEach(function (k) {
    if (now - watches[k].at > 60000)
      delete watches[k];
  });
  var ids = Object.keys(watches).sort(function (a, b) { return watches[a].at - watches[b].at; });
  while (ids.length >= WATCHES_MAX)
    delete watches[ids.shift()];
  var o = outline();
  watches[req.watch] = {at: now, lines: o.lines};
  o.frames.forEach(function (frame) { relaySend(frame, {op: 'watch', watch: req.watch, expires: req.expires}); });
}

// The lines of `after` that `before` lacks, a repeated line counted as often as it shows, in order.
function linesOnlyIn(after, before) {
  var count = new Map();
  before.forEach(function (l) { count.set(l, (count.get(l) || 0) + 1); });
  return after.filter(function (l) {
    var n = count.get(l) || 0;
    if (n > 0)
      count.set(l, n - 1);
    return n === 0;
  });
}

// What the page and its configuration frames list now and did not before the action req.watch
// (added), and what they no longer list (removed), at most CHANGES_MAX lines each with the number left
// out. Text that moved or an element rebuilt as it was is no change. Partial when a frame could not be
// compared: it did not answer, failed, or holds another document than the one it outlined. No lists
// from a page with no outline for the action; pending while a frame has yet to answer.
function changes(req) {
  var w = watches[req.watch];
  if (!w)
    return {doc: docToken};
  var now = outline();
  var added = linesOnlyIn(now.lines, w.lines);
  var removed = linesOnlyIn(w.lines, now.lines);
  var pending = false;
  var partial = false;
  var left = 0; // what the frames left out of theirs
  now.frames.forEach(function (frame) {
    var reply = relayRound(frame, req.round, {op: 'changes', watch: req.watch, expires: req.expires});
    if (!reply && !req.last)
      pending = true;
    else if (!reply || reply.error || !reply.added)
      partial = true;
    else {
      added = added.concat(reply.added);
      removed = removed.concat(reply.removed);
      left += reply.more || 0;
      if (reply.partial)
        partial = true;
    }
  });
  if (pending)
    return {doc: docToken, pending: true};
  delete watches[req.watch];
  var out = {doc: docToken, added: added.slice(0, CHANGES_MAX), removed: removed.slice(0, CHANGES_MAX)};
  var more = left + Math.max(0, added.length - CHANGES_MAX) + Math.max(0, removed.length - CHANGES_MAX);
  if (more)
    out.more = more;
  if (partial)
    out.partial = true;
  return out;
}

// ---- the calls ---------------------------------------------------------------------------------

function element(req) {
  if (req.doc !== docToken)
    fail('stale', 'The page changed (it navigated or reloaded); take a new ui_snapshot');
  var el = byKey.get(req.k);
  if (!el || !el.isConnected)
    fail('stale', 'The element is gone from the page; take a new ui_snapshot');
  return el;
}

function stateOf(el) {
  if (!el.isConnected)
    return {gone: true};
  var info = infoOf(el);
  var out = {};
  var c = checkedOf(el, info.kind);
  if (c >= 0)
    out.checked = c === 1;
  var src = info.control || el;
  if (src.localName === 'select')
    out.value = selectedText(src);
  else if (src.localName === 'input' || src.localName === 'textarea') {
    if (!/^(checkbox|radio|button|submit|reset|image|file)$/.test(lower(src.type || '')))
      out.value = src.type === 'password' ? (src.value ? MASK : '') : String(src.value);
  } else if (src.isContentEditable)
    out.value = clip(innerTextOf(src), TEXT_MAX);
  return out;
}

// An action on an element of a plugin configuration frame, forwarded to the frame's agent; the
// reply is read with `relay`.
function forward(req) {
  var parts = String(req.k).split('/');
  var frame = byKey.get(Number(parts[0]));
  if (!frame || !frame.isConnected || !relayFrame(frame))
    fail('stale', 'The element\'s frame is gone; take a new ui_snapshot');
  var inner = {};
  for (var p in req)
    inner[p] = req[p];
  inner.doc = parts[1];
  inner.k = Number(parts[2]);
  var r = topRect(frame);
  return {relay: relaySend(frame, inner), frame: {x: r.x, y: r.y}};
}

// Ends the press `press` (the tools' id for it) a `down` left in the document `doc`: this page's own,
// or with `frame` ("<frame key>/<frame document>") a configuration frame's, whose agent answers by
// relay. A press the page took since is another, and stays; a document that is gone holds nothing.
function release(req) {
  if (req.doc && req.doc !== docToken)
    return {released: true};
  if (!req.frame) {
    if (held && held.press === req.press)
      releaseHeld();
    return {released: true};
  }
  var parts = String(req.frame).split('/');
  var frame = byKey.get(Number(parts[0]));
  if (!frame || !frame.isConnected || !relayFrame(frame))
    return {released: true};
  return {relay: relaySend(frame, {op: 'release', doc: parts[1], press: req.press, expires: req.expires})};
}

function act(req) {
  if (typeof req.k === 'string' && req.k.indexOf('/') > 0) {
    if (req.doc !== docToken)
      fail('stale', 'The page changed (it navigated or reloaded); take a new ui_snapshot');
    watch(req);
    return forward(req);
  }
  var args = req.args || {};
  var el = req.k ? element(req) : null;
  if (req.action === 'key') {
    if (el && hiddenFromView(el))
      fail('hidden', 'The element is not shown; take a new ui_snapshot');
    // A disabled control (its fieldset's included) takes no focus, so keys would go elsewhere.
    if (el) {
      var keyed = infoOf(el);
      if (disabledOf(keyed.control || el, keyed.st))
        fail('disabled', 'The ' + keyed.kind + ' "' + clip(keyed.label, 40) + '" is disabled');
    }
    var keys = [];
    if (args.key)
      keys.push({k: args.key, mods: args.mods});
    // By code point, so a character outside the BMP (an emoji) is one key, not two halves.
    Array.from(String(args.text || '')).forEach(function (c) { keys.push({k: charKey(c), mods: null}); });
    watch(req);
    if (el) {
      reveal(el);
      focusFor(el);
    }
    var first = el ? (deepActive(el.ownerDocument) || el) : (deepActive(document) || document.body);
    keys.forEach(function (one) {
      var t = el ? (deepActive(el.ownerDocument) || el) : (deepActive(document) || document.body);
      pressKey(t, one.k, one.mods);
    });
    var out = {to: brief(first)};
    if (el) {
      var info0 = infoOf(el);
      out.kind = info0.kind;
      out.label = info0.label;
      out.state = stateOf(el);
      var found0 = locate(el, info0.kind, info0.label);
      out.loc = found0.loc;
      out.index = found0.index;
    }
    return out;
  }
  if (!el)
    fail('target', 'No element');
  if (hiddenFromView(el))
    fail('hidden', 'The element is not shown; take a new ui_snapshot');
  var info = infoOf(el);
  if (disabledOf(info.control || el, info.st))
    fail('disabled', 'The ' + info.kind + ' "' + clip(info.label, 40) + '" is disabled');
  var found = locate(el, info.kind, info.label);
  var res = {kind: info.kind, label: info.label, loc: found.loc, index: found.index};
  watch(req);
  if (req.action === 'click') {
    var c = cssPerLogical(args);
    releaseHeld();
    clickEvents(el, aimAt(el, args.x === undefined ? undefined : args.x * c, args.y === undefined ? undefined : args.y * c, true), args);
  } else if (req.action === 'input')
    setValue(el, info, args.value, args.submit !== false);
  else if (req.action === 'mouse')
    res.at = gesture(el, args);
  else
    fail('action', 'Unknown action ' + req.action);
  res.state = stateOf(el);
  return res;
}

// A dialog page's button or link by its label (case-insensitive), clicked when shown and enabled.
function press(req) {
  var want = normalize(req.label || '');
  var labels = [];
  var hit = null;
  listed().forEach(function (it) {
    if (it.kind !== 'button' && it.kind !== 'link')
      return;
    labels.push(it.label);
    if (!hit && normalize(it.label) === want && !disabledOf(it.el, styleOf(it.el)))
      hit = it;
  });
  if (!hit)
    return {pressed: null, buttons: labels, ready: document.readyState, doc: docToken};
  // Refused as a click is when something covers the button, rather than clicked through.
  releaseHeld();
  watch(req);
  clickEvents(hit.el, aimAt(hit.el, undefined, undefined, true), {});
  return {pressed: hit.label, doc: docToken};
}

function evalScript(req) {
  if (typeof req.k === 'string')
    fail('relayed', 'The element is in a plugin configuration frame, which webview_eval does not reach; use the ui_* tools');
  var el = req.k ? element(req) : undefined;
  // A page whose Content-Security-Policy forbids eval refuses to build the runner, before any of the
  // script runs; the agent itself, installed as inline code, still works there.
  var run;
  try {
    run = new Function('el', 'return eval(arguments[1]);');
  } catch (e) {
    fail('csp', 'This page\'s Content-Security-Policy forbids eval, which webview_eval runs scripts with; the ui_* tools ' +
         'still work on it');
  }
  var res = run(el, String(req.script));
  return {value: typeof res === 'object' ? JSON.stringify(res) : typeof res === 'undefined' ? 'undefined' : String(res)};
}

function relayResult(req) {
  var reply = relayTake(req.rid);
  return reply ? {done: true, reply: reply} : {done: false};
}

api.call = function (req) {
  try {
    if (req.expires && Date.now() > req.expires)
      return {error: 'The request reached the page after its deadline, so it was not run', code: 'expired'};
    if (req.op === 'flushed') {
      var done = flushes[req.id] !== false;
      if (done)
        delete flushes[req.id];
      return {done: done};
    }
    var href0 = String(location.href);
    var h = holding(function () {
      switch (req.op) {
      case 'snapshot': return snapshot(req);
      case 'find': return find(req);
      case 'act': return act(req);
      case 'press': return press(req);
      case 'relay': return relayResult(req);
      case 'eval': return evalScript(req);
      case 'release': return release(req);
      case 'changes': return changes(req);
      case 'watch': watch(req); return {watching: true};
      // `relaying`: requests still waiting on a frame's reply whose caller still waits.
      case 'doc':
        prunePending();
        return {doc: docToken, href: String(location.href),
                relaying: Object.keys(relay.replies).filter(function (id) { return !relay.replies[id].reply; }).length};
      }
      return {error: 'Unknown call ' + req.op, code: 'call'};
    });
    if (h.held && h.value && typeof h.value === 'object') {
      h.value.held = h.held;
      h.value.flush = h.flush;
    }
    // What a change of page is told by: the URL before and after (a fragment change, which WebView2
    // may not report), and this document's token, which a later `doc` call compares (a reload).
    if (/^(act|press|eval)$/.test(req.op) && h.value && typeof h.value === 'object' && !h.value.error) {
      h.value.href0 = href0;
      h.value.href = String(location.href);
      h.value.doc = docToken;
    }
    return h.value;
  } catch (e) {
    return {error: String(e && e.message || e), code: e && e.code || 'error'};
  }
};

Object.defineProperty(window, '__orcaMcp', {value: api, configurable: true, enumerable: false, writable: true});
