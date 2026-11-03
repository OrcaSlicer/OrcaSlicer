// Localized text for the bundled web pages. The hosting window injects the page's strings, translated
// through the .po catalogs, as window.ORCA_UI_STRINGS before any page script runs
// (WebViewHostDialog::localized_strings_script). The English in the markup and in the T() calls is the
// fallback for a page opened outside the app.

// The string for `key`, else `fallback`. Each further argument fills the next %s.
function T(key, fallback, ...args) {
  const strings = (typeof ORCA_UI_STRINGS !== "undefined" && ORCA_UI_STRINGS) || {};
  let text = strings[key] !== undefined ? String(strings[key]) : fallback;
  // A replacer function, so a "$" in a value (a plugin name, say) is not read as a pattern.
  args.forEach((arg) => { text = text.replace("%s", () => String(arg)); });
  return text;
}

// Appends `template` to `node`, putting the nodes that follow in place of its markers, in order, so a
// translation can move them within the sentence. %s inserts the node as it is (a search term, say);
// <a>label</a> inserts it with the label as its text, so the translation words the link together with
// the sentence around it.
function AppendTemplate(node, template, ...nodes) {
  let end = 0;
  let next = 0;
  for (const marker of template.matchAll(/%s|<a>(.*?)<\/a>/g)) {
    if (marker.index > end)
      node.appendChild(document.createTextNode(template.slice(end, marker.index)));
    const inserted = nodes[next++];
    if (inserted) {
      if (marker[1] !== undefined)
        inserted.textContent = marker[1];
      node.appendChild(inserted);
    }
    end = marker.index + marker[0].length;
  }
  if (end < template.length)
    node.appendChild(document.createTextNode(template.slice(end)));
}

// Translates the static markup: data-i18n names the key for an element's own text (child elements
// such as icons and sort arrows are kept), data-i18n-title, data-i18n-placeholder and
// data-i18n-aria-label the key for that attribute. Translating again is harmless, so a page can call
// this after adding markup.
function TranslatePage() {
  document.querySelectorAll("[data-i18n]").forEach((el) => {
    const textNode = Array.from(el.childNodes).find((n) => n.nodeType === Node.TEXT_NODE && n.nodeValue.trim());
    const text = T(el.dataset.i18n, textNode ? textNode.nodeValue.trim() : "");
    if (textNode)
      textNode.nodeValue = text;
    else
      el.insertBefore(document.createTextNode(text), el.firstChild);
  });
  ["title", "placeholder", "aria-label"].forEach((attr) => {
    document.querySelectorAll(`[data-i18n-${attr}]`).forEach((el) => {
      el.setAttribute(attr, T(el.getAttribute(`data-i18n-${attr}`), el.getAttribute(attr) || ""));
    });
  });
}
