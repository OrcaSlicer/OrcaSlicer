var g_profile = {
    machines: [],
    filaments: [],
    presets: []
};

var g_search = {
    machine: "",
    filament: "",
    preset: ""
};

function OnInit()
{
    if (typeof TranslatePage === "function")
        TranslatePage();

    InstallInputSafeKeydown();
    BindSearchInputs();
    BindClearIcons();
    BindBottomButtons();

    // Always load demo data first so the page works without C++ backend.
    // LoadDemoProfile();
    RequestProfile();
}

function InstallInputSafeKeydown()
{
    // common.js blocks all key events globally; allow typing in text inputs.
    document.onkeydown = function (event) {
        var e = event || window.event || arguments.callee.caller.arguments[0];
        var target = e && e.target ? e.target : null;
        var tag = target && target.tagName ? String(target.tagName).toUpperCase() : "";
        var type = target && target.type ? String(target.type).toLowerCase() : "";

        var editable =
            !!(target && target.isContentEditable) ||
            tag === "TEXTAREA" ||
            (tag === "INPUT" && type !== "checkbox" && type !== "radio" && type !== "button" && type !== "submit");

        if (editable)
            return true;

        if (e && e.keyCode === 27 && typeof ClosePage === "function")
            ClosePage();

        if (window.event) {
            try { e.keyCode = 0; } catch (err) { }
            e.returnValue = false;
        }

        if (e && typeof e.preventDefault === "function")
            e.preventDefault();

        return false;
    };
}

function RequestProfile()
{
    SendMessage("request_export_preset_profile", {});
}

function HandleStudio(pVal)
{
    var payload = (typeof pVal === "string") ? SafeJsonParse(pVal) : pVal;
    if (!payload || typeof payload !== "object")
        return;

    var cmd = String(payload.command || "");
    if (cmd === "response_export_preset_profile" ) {
        ApplyProfile(payload.data);
    }
}

function ApplyProfile(profile)
{

    g_profile.machines = BuildNameRows(profile.printers);
    g_profile.filaments = BuildNameRows(profile.filaments);
    g_profile.presets = BuildNameRows(profile.process);

    RenderColumn("MachineList", g_profile.machines, "mode", "MachineClick");
    RenderColumn("FilatypeList", g_profile.filaments, "filatype", "FilaClick");
    RenderColumn("PresetList", g_profile.presets, "preset", "PresetClick");

    ApplyColumnSearch("MachineList", g_search.machine);
    ApplyColumnSearch("FilatypeList", g_search.filament);
    ApplyColumnSearch("PresetList", g_search.preset);
}

function BuildNameRows(names)
{
    var src = Array.isArray(names) ? names : [];
    var out = [];

    for (var n = 0; n < src.length; n++) {
        var row = src[n];
        if (row === undefined || row === null)
            continue;

        var name = String(row);
        out.push({ id: name, label: name, checked: false });
    }

    return out;
}

function RenderColumn(listId, items, attrName, onChangeFn)
{
    var root = $("#" + listId + " .CValues");
    if (!root.length)
        return;

    root.find("label:gt(0)").remove();

    var html = "";
    for (var n = 0; n < items.length; n++) {
        var one = items[n];
        html += '<label data-dynamic="1">' +
            '<input type="checkbox" data-key="' + EscapeAttr(one.id) + '" ' + attrName + '="' + EscapeAttr(one.id) + '"' +
            (one.checked ? ' checked="checked"' : "") +
            ' onChange="' + onChangeFn + '()" />' +
            '<span title="' + EscapeAttr(one.label) + '">' + EscapeHtml(one.label) + '</span>' +
            '</label>';
    }

    root.append(html);
    SyncMasterCheckbox(listId);
    ToggleNoItems(listId, items.length === 0);
}

function ChooseAllMachine()
{
    var checked = !!$("#MachineList .CValues input:first").prop("checked");
    $("#MachineList .CValues input:gt(0)").prop("checked", checked);
    SyncListFromDom("MachineList", g_profile.machines);
}

function MachineClick()
{
    SyncMasterCheckbox("MachineList");
    SyncListFromDom("MachineList", g_profile.machines);
}

function ChooseAllFilament()
{
    var checked = !!$("#FilatypeList .CValues input:first").prop("checked");
    $("#FilatypeList .CValues input:gt(0)").prop("checked", checked);
    SyncListFromDom("FilatypeList", g_profile.filaments);
}

function FilaClick()
{
    SyncMasterCheckbox("FilatypeList");
    SyncListFromDom("FilatypeList", g_profile.filaments);
}

function ChooseAllPreset()
{
    var checked = !!$("#PresetList .CValues input:first").prop("checked");
    $("#PresetList .CValues input:gt(0)").prop("checked", checked);
    SyncListFromDom("PresetList", g_profile.presets);
}

function PresetClick()
{
    SyncMasterCheckbox("PresetList");
    SyncListFromDom("PresetList", g_profile.presets);
}

function SyncMasterCheckbox(listId)
{
    var all = $("#" + listId + " .CValues input:gt(0)");
    var master = $("#" + listId + " .CValues input:first");

    if (!all.length) {
        master.prop("checked", false);
        return;
    }

    master.prop("checked", all.length === all.filter(":checked").length);
}

function SyncListFromDom(listId, store)
{
    var map = {};
    for (var n = 0; n < store.length; n++)
        map[store[n].id] = store[n];

    $("#" + listId + " .CValues input:gt(0)").each(function () {
        var id = String($(this).attr("data-key") || "");
        if (map[id])
            map[id].checked = !!$(this).prop("checked");
    });
}

function BindSearchInputs()
{
    var inputs = document.querySelectorAll(".cbr-search-bar");

    if (inputs.length > 0) {
        inputs[0].addEventListener("input", function () {
            g_search.machine = String(this.value || "").toLowerCase();
            ApplyColumnSearch("MachineList", g_search.machine);
        });
    }

    if (inputs.length > 1) {
        inputs[1].addEventListener("input", function () {
            g_search.filament = String(this.value || "").toLowerCase();
            ApplyColumnSearch("FilatypeList", g_search.filament);
        });
    }

    if (inputs.length > 2) {
        inputs[2].addEventListener("input", function () {
            g_search.preset = String(this.value || "").toLowerCase();
            ApplyColumnSearch("PresetList", g_search.preset);
        });
    }
}

function ApplyColumnSearch(listId, query)
{
    var rows = $("#" + listId + " .CValues label:gt(0)");
    var visibleCount = 0;

    rows.each(function () {
        var row = $(this);
        var text = String(row.text() || "").toLowerCase();
        var key = String(row.find("input").attr("data-key") || "").toLowerCase();

        if (!query || text.indexOf(query) >= 0 || key.indexOf(query) >= 0) {
            row.show();
            visibleCount++;
        }
        else {
            row.hide();
        }
    });

    ToggleNoItems(listId, visibleCount === 0);
}

function ToggleNoItems(listId, show)
{
    var node = $("#" + listId + " .cbr-no-items");
    if (!node.length)
        return;

    if (show)
        node.addClass("show");
    else
        node.removeClass("show");
}

function BindClearIcons()
{
    var icons = document.querySelectorAll(".clear-icon");

    for (var n = 0; n < icons.length; n++) {
        icons[n].addEventListener("click", function () {
            var parent = this.parentElement;
            if (!parent)
                return;

            var input = parent.querySelector("input[type='text']");
            if (!input)
                return;

            input.value = "";
            input.dispatchEvent(new Event("input", { bubbles: true }));
            input.focus();
        });
    }
}

function BindBottomButtons()
{
    var backBtn = document.getElementById("back_btn");
    var exportCloud = document.getElementById("export_cloud_btn")
    var exportLocal = document.getElementById("export_local_btn");
    var closeBtn = document.getElementById("close_btn");

    backBtn?.addEventListener("click", function () {
        SendMessage("navigate_back", {});
    });


    exportLocal?.addEventListener("click", function () {
        SendMessage("export_local", BuildResultPayload());
    });
    

    closeBtn?.addEventListener("click", () => {
        const tSend = {
        sequence_id: Math.round(Date.now() / 1000),
        command: "close_page"
        };
        SendWXMessage(JSON.stringify(tSend));
    });
}

function BuildResultPayload()
{
    return {
        machines: g_profile.machines.filter(function (x) { return x.checked; }).map(function (x) { return x.id; }),
        filaments: g_profile.filaments.filter(function (x) { return x.checked; }).map(function (x) { return x.id; }),
        presets: g_profile.presets.filter(function (x) { return x.checked; }).map(function (x) { return x.id; })
    };
}

function LoadDemoProfile()
{
    ApplyProfile({
        machines: [
            { id: "printer_x1c_04", name: "X1 Carbon 0.4 nozzle", selected: 1 },
            { id: "printer_p1s_04", name: "P1S 0.4 nozzle", selected: 1 },
            { id: "printer_a1_04", name: "A1 0.4 nozzle", selected: 0 },
            { id: "printer_prusa_mk4_04", name: "Prusa MK4 0.4 nozzle", selected: 1 }
        ],
        filaments: [
            { id: "filament_generic_pla", name: "Generic PLA", selected: 1 },
            { id: "filament_generic_petg", name: "Generic PETG", selected: 1 },
            { id: "filament_bambu_abs", name: "Bambu ABS", selected: 0 },
            { id: "filament_esun_pla_plus", name: "eSUN PLA+", selected: 1 }
        ],
        presets: [
            { id: "preset_quality_020", name: "Quality 0.20mm", selected: 1 },
            { id: "preset_quality_012", name: "Quality 0.12mm", selected: 0 },
            { id: "preset_speed_024", name: "Speed 0.24mm", selected: 1 },
            { id: "preset_draft_028", name: "Draft 0.28mm", selected: 0 }
        ]
    });
}

function SendMessage(command, data)
{
    var msg = {};
    msg.sequence_id = Math.round(new Date() / 1000);
    msg.command = command;
    if (data && typeof data === "object")
        msg.data = data;

    if (typeof SendWXMessage === "function")
        SendWXMessage(JSON.stringify(msg));
}

function SafeJsonParse(str)
{
    try {
        return JSON.parse(str);
    }
    catch (err) {
        return null;
    }
}

function EscapeHtml(str)
{
    return String(str)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/\"/g, "&quot;")
        .replace(/'/g, "&#039;");
}

function EscapeAttr(str)
{
    return EscapeHtml(str);
}
