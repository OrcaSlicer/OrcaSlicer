// ========= Data Stores =========
const bundlesById = new Map();        // bundleId -> bundle object
const printersByBundle = new Map();   // bundleId -> Map(index -> printerName)
const filamentsByBundle = new Map();  // bundleId -> Map(index -> filamentName)
const presetsByBundle = new Map();    // bundleId -> Map(index -> presetName)
const UPDATE_TOOLTIP = "Update available";
const UNAUTHORIZED_TOOLTIP = "Unauthorized bundle";

// ========= DOM =========
let topList = null;
let bottomList = null;

let ctxMenu = null;
let contextRow = null;
let ctxMenuSubscribed = null;

let ctxMenuDelete = null;

let selectedBundleId = null;

// ========= Init =========
function OnInit() {

   topList = document.getElementById("topList");
   bottomList = document.getElementById("bottomList");
   ctxMenu = document.getElementById("ctxMenu");
   ctxMenuSubscribed = document.getElementById("unsubscribe_btn");
   ctxMenuDelete = document.getElementById("delete_btn");
  const closeBtn = document.getElementById("close_btn");
   const exportbtn = document.getElementById("export_btn");
   const refreshBtn = document.getElementById("refresh_btn");                                                                                                                                                                                                                           
  const autoUpdateToggle = document.getElementById("auto_update_toggle");

  if (!topList || !bottomList) return;
  TranslatePage();

  // If wx side needs to request bundles after page load:
    RequestBundles();


    refreshBtn?.addEventListener("click", () => {                                                                                                                                                                                                                                        
    const tSend = {                                                                                                                                                                                                                                                                    
      sequence_id: Math.round(Date.now() / 1000),                                                                                                                                                                                                                                      
      command: "refresh_bundles"                                                                                                                                                                                                                                                       
    };                                                                                                                                                                                                                                                                                 
    SendWXMessage(JSON.stringify(tSend));                                                                                                                                                                                                                                              
  });                                                                                                                                                                                                                                                                                  
                                                                                                                                                                                                                                                                                       
  autoUpdateToggle?.addEventListener("change", () => {                                                                                                                                                                                                                                 
    const tSend = {                                                                                                                                                                                                                                                                    
      sequence_id: Math.round(Date.now() / 1000),                                                                                                                                                                                                                                      
      command: "set_auto_update",                                                                                                                                                                                                                                                      
      enabled: !!autoUpdateToggle.checked                                                                                                                                                                                                                                              
    };                                                                                                                                                                                                                                                                                 
    SendWXMessage(JSON.stringify(tSend));                                                                                                                                                                                                                                              
  });
  // Hook selection on top list
  topList.addEventListener("click", (e) => {                                                                                                                                                                                                                                                                                                                     
    const cloudLink = e.target.closest(".bundle-cloud-link");
    if (cloudLink) {
      e.preventDefault();
      e.stopPropagation();

      const row = cloudLink.closest(".row");
      if (!row) return;

      selectTopRow(row);
      selectedBundleId = String(row.dataset.id || "");
      renderBottomForBundle(selectedBundleId);
      sendOpenBundleOnCloud(selectedBundleId);
      return;
    }

    const updateBtn = e.target.closest(".bundle-update-btn");                                                                                                                                                                                                                                                                                                    
    if (updateBtn) {                                                                                                                                                                                                                                                                                                                                             
      e.stopPropagation();                                                                                                                                                                                                                                                                                                                                       
      if (updateBtn.disabled) return;                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                                                                 
      const row = updateBtn.closest(".row");                                                                                                                                                                                                                                                                                                                     
      if (!row) return;                                                                                                                                                                                                                                                                                                                                          
                                                                                                                                                                                                                                                                                                                                                                 
      selectTopRow(row);                                                                                                                                                                                                                                                                                                                                         
      selectedBundleId = String(row.dataset.id || "");                                                                                                                                                                                                                                                                                                           
      renderBottomForBundle(selectedBundleId);                                                                                                                                                                                                                                                                                                                   
      sendUpdateBundleCommand(selectedBundleId);                                                                                                                                                                                                                                                                                                                 
      return;                                                                                                                                                                                                                                                                                                                                                    
    }                                                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                                                                 
    const row = e.target.closest(".row");                                                                                                                                                                                                                                                                                                                        
    if (!row) return;                                                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                                                                 
    selectTopRow(row);                                                                                                                                                                                                                                                                                                                                           
    selectedBundleId = String(row.dataset.id || "");                                                                                                                                                                                                                                                                                                             
    renderBottomForBundle(selectedBundleId);                                                                                                                                                                                                                                                                                                                     
  });

  // for top list rows if right click open context menu
  topList.addEventListener("contextmenu", (e) => {
    const row = e.target.closest(".row");
    if (!row) return; // top rows only

    const bundleType = String(row.dataset.bundleType || "").toLowerCase();
    if (bundleType !== "subscribed") return;

    e.preventDefault();
    selectTopRow(row);
    contextRow = row;
    showSubscribedMenu(e.clientX, e.clientY);
  });

  // for top list rows except subscribed if right click open regular context menu
  topList.addEventListener("contextmenu", (e) => {
    const row = e.target.closest(".row");
    if (!row) return; // top rows only
    const bundleType = String(row.dataset.bundleType || "").toLowerCase();
    if (bundleType === "subscribed") return;

    e.preventDefault();

    selectTopRow(row);
    contextRow = row;
    showMenu(e.clientX, e.clientY);
  });

  ctxMenu?.addEventListener("click", (e) => {
    const btn = e.target.closest("[data-action]");
    if (!btn || !contextRow) return;

    const tSend = {
      sequence_id: Math.round(Date.now() / 1000),
      command: "top_row_menu_action",
      action: String(btn.dataset.action || ""),
      bundle_id: String(contextRow.dataset.id || "")
    };
    SendWXMessage(JSON.stringify(tSend));
    hideMenu();
  });

  closeBtn?.addEventListener("click", () => {
    const tSend = {
      sequence_id: Math.round(Date.now() / 1000),
      command: "close_page"
    };
    SendWXMessage(JSON.stringify(tSend));
  });

  exportbtn?.addEventListener("click", () => {
    const tSend = {
      sequence_id: Math.round(Date.now() / 1000),
      command: "export_page"
    };
    SendWXMessage(JSON.stringify(tSend));
  });

  document.addEventListener("click", (e) => {
    if (!e.target.closest(".ctx")) hideMenu();
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") hideMenu();
  });
}
// ========= wx bridge requests =========

function RequestBundles() {
    var tSend={};
	  tSend['sequence_id']=Math.round(new Date() / 1000);
	  tSend['command']="request_bundles";

    SendWXMessage(JSON.stringify(tSend));
}

function HandleStudio(pVal) {

  const msg = (typeof pVal === "string") ? safeJsonParse(pVal) : pVal;
  if (!msg || typeof msg !== "object") return;

  const strCmd = msg.command;
  if (strCmd === "list_bundles") {
    unpackPayload(msg);
    renderTop();
    // auto-select first bundle if none selected
    autoSelectFirstBundle();

    const autoUpdateToggle = document.getElementById("auto_update_toggle");                                                                                                                                                                                                            
    if (autoUpdateToggle) {                                                                                                                                                                                                                                                            
      autoUpdateToggle.checked = !!msg.auto_update_enabled;
    }
  }
}

// ========= Parse / store =========
function unpackPayload(payload) {
  bundlesById.clear();
  printersByBundle.clear();
  filamentsByBundle.clear();
  presetsByBundle.clear();

  const list = payload?.data || [];
  for (const bundle of list) {
    const id = String(bundle.id ?? "");
    if (!id) continue;

    bundlesById.set(id, {                                                                                                                                                                                                                                                                                                                                          
    id,                                                                                                                                                                                                                                                                                                                                                          
    name: bundle.name ?? "",
    type: bundle.type ?? "",                                                                                                                                                                                                                                                                                                                                     
    version: bundle.version ?? "",                                                                                                                                                                                                                                                                                                                               
    path: bundle.path ?? "",                                                                                                                                                                                                                                                                                                                                     
    update_available: Boolean(bundle.update_available) ,   
    unauthorized: Boolean(bundle.unauthorized)                                                                                                                                                                                                                                                                                                           
  });  

    printersByBundle.set(id, new Map((bundle.printers || []).map((name, i) => [i, name])));
    filamentsByBundle.set(id, new Map((bundle.filaments || []).map((name, i) => [i, name])));
    presetsByBundle.set(id, new Map((bundle.presets || []).map((name, i) => [i, name])));
  }
}

// ========= Render: top =========
function renderTop() {                                                                                                                                                                                                                                                                                                                                         
    const bundles = Array.from(bundlesById.values());                                                                                                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                                                                                                                                 
    topList.innerHTML = bundles.map(b => `                                                                                                                                                                                                                                                                                                                       
      <div class="row" data-id="${escapeAttr(b.id)}" data-bundle-type="${escapeAttr(String(b.type || "").toLowerCase())}">                                                                                                                                                                                                                                       
        <div class="cell bundle-name-cell" title="${escapeAttr(b.name)}">                                                                                                                                                                                                                                                                                        
          ${b.unauthorized
            ? `<span class="bundle-status-icon bundle-status-icon-unauthorized" title="${escapeAttr(UNAUTHORIZED_TOOLTIP)}" aria-label="${escapeAttr(UNAUTHORIZED_TOOLTIP)}">!</span>`
            : b.update_available
              ? `<span class="bundle-status-icon bundle-status-icon-update" title="${escapeAttr(UPDATE_TOOLTIP)}" aria-label="${escapeAttr(UPDATE_TOOLTIP)}">&uarr;</span>`
              : `<span class="bundle-status-icon-spacer" aria-hidden="true"></span>`}                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          
          ${
            b.type === "Subscribed" ?
            `<a href="#" class="bundle-name-text bundle-cloud-link" title="Open this bundle in your browser">${escapeHtml(b.name)}</a>`
            : `<span class="bundle-name-text">${escapeHtml(b.name)}</span>`
          }                                                                                                                                                                                                                                                                                
        </div>                                                                                                                                                                                                                                                                                                                                                   
        <span title="${escapeAttr(b.type)}">${escapeHtml(b.type)}</span>                                                                                                                                                                                                                                                                                         
        <span title="${escapeAttr(b.version)}">${escapeHtml(b.version)}</span>                                                                                                                                                                                                                                                                                   
        <div class="cell bundle-update-cell">                                                                                                                                                                                                                                                                                                                    
            <button                                                                                                                                                                                                                                                               
            type="button"                                                                                                                                                                                                                                                       
            class="bundle-update-btn ${(!b.unauthorized && b.update_available) ? "is-enabled" : "is-disabled"}"                                                                                                                                                                 
            ${(!b.unauthorized && b.update_available) ? "" : "disabled"}                                                                                                                                                                                                        
            data-id="${escapeAttr(b.id)}"                                                                                                                                                                                                                                       
          >Update</button>                                                                                                                                                                                                                                                                                                                                       
        </div>                                                                                                                                                                                                                                                                                                                                                   
      </div>                                                                                                                                                                                                                                                                                                                                                     
    `).join("");                                                                                                                                                                                                                                                                                                                                                 
  }

function sendOpenBundleOnCloud(bundleId) {
  const bundle = bundlesById.get(String(bundleId || ""));
  if (!bundle) return;

  const tSend = {
    sequence_id: Math.round(Date.now() / 1000),
    command: "open_bundle_on_cloud",
    bundle_id: String(bundle.id || "")
  };
  SendWXMessage(JSON.stringify(tSend));
}

function sendUpdateBundleCommand(bundleId) {                                                                                                                                                                                                                                                                                                                   
    const bundle = bundlesById.get(String(bundleId || ""));                                                                                                                                                                                                                                                                                                      
    if (!bundle || bundle.unauthorized || !bundle.update_available) return;                                                                                                                                                                                                                                                                                                             
                                                                                                                                                                                                                                                                                                                                                                 
    const tSend = {                                                                                                                                                                                                                                                                                                                                              
      sequence_id: Math.round(Date.now() / 1000),                                                                                                                                                                                                                                                                                                                
      command: "update_bundle",                                                                                                                                                                                                                                                                                                                                  
      bundle_id: String(bundle.id || "")                                                                                                                                                                                                                                                                                                                         
    };                                                                                                                                                                                                                                                                                                                                                           
    SendWXMessage(JSON.stringify(tSend));                                                                                                                                                                                                                                                                                                                        
  }

// ========= Render: bottom (for a selected bundle) =========
function renderBottomForBundle(bundleId) {
  const key = String(bundleId || "");
  const printers = printersByBundle.get(key) || new Map();
  const filaments = filamentsByBundle.get(key) || new Map();
  const presets = presetsByBundle.get(key) || new Map();

  // Convert to a flat list of rows { typeLabel, name }
  const rows = [];

  for (const [, name] of printers) rows.push({ type: "Printer", name });
  for (const [, name] of filaments) rows.push({ type: "Filament", name });
  for (const [, name] of presets) rows.push({ type: "Preset", name });

  bottomList.innerHTML = rows.map((r, idx) => `
    <div class="row" data-id="${escapeAttr(bundleId)}" data-idx="${idx}">
      <span>${escapeHtml(r.name)}</span>
      <span title="${escapeAttr(r.type)}">${escapeHtml(r.type)}</span>
    </div>
  `).join("");
}

// ========= Selection helpers =========
function clearSelection() {
  document.querySelectorAll(".row.selected").forEach(r => r.classList.remove("selected"));
}

function selectTopRow(rowEl) {
  // only clear selection in top list, not bottom
  topList.querySelectorAll(".row.selected").forEach(r => r.classList.remove("selected"));
  rowEl.classList.add("selected");
}

function autoSelectFirstBundle() {
  if (selectedBundleId && bundlesById.has(selectedBundleId)) {
    // reselect existing
    const el = topList.querySelector(`.row[data-id="${cssEscape(selectedBundleId)}"]`);
    if (el) selectTopRow(el);
    renderBottomForBundle(selectedBundleId);
    return;
  }

  const first = topList.querySelector(".row");
  if (!first) {
    bottomList.innerHTML = "";
    selectedBundleId = null;
    return;
  }

  selectTopRow(first);
  selectedBundleId = first.dataset.id;
  renderBottomForBundle(selectedBundleId);
}

function showSubscribedMenu(x, y) {
  if (!ctxMenu) return;
  ctxMenu.style.left = `${x}px`;
  ctxMenu.style.top = `${y}px`;
  ctxMenu.hidden = false;
  ctxMenuDelete.hidden = true;
  ctxMenuSubscribed.hidden = false;
}

function showMenu(x, y) {
  if (!ctxMenu) return;
  ctxMenu.style.left = `${x}px`;
  ctxMenu.style.top = `${y}px`;
  ctxMenu.hidden = false;
  ctxMenuDelete.hidden = false;
  ctxMenuSubscribed.hidden = true;
}

function hideMenu() {
  if (!ctxMenu) return;
  ctxMenu.hidden = true;
  ctxMenuSubscribed.hidden = true;
  contextRow = null;
}
// ========= Utilities =========
function safeJsonParse(s) {
  try { return JSON.parse(s); } catch { return null; }
}

function escapeHtml(str) {
  return String(str)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function escapeAttr(str) {
  // minimal attribute escaping
  return escapeHtml(str);
}

function cssEscape(str) {
  // basic css escape for attribute selectors
  return String(str).replaceAll('"', '\\"');
}
