#include "SyncMergeDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Search.hpp"
#include "Tab.hpp"
#include "Field.hpp"

#include "libslic3r/PrintConfig.hpp"
#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/button.h>

#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>

#include <ctime>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

namespace Slic3r { namespace GUI {

// Copied from UnsavedChangesDialog.cpp — static helpers for option formatting
static std::string get_pure_opt_key(std::string opt_key)
{
    const int pos = opt_key.find("#");
    if (pos > 0)
        boost::erase_tail(opt_key, opt_key.size() - pos);
    return opt_key;
}

static wxString get_full_label(std::string opt_key, const DynamicPrintConfig& config)
{
    opt_key = get_pure_opt_key(opt_key);
    auto option = config.option(opt_key);
    if (!option || option->is_nil())
        return _L("N/A");
    const ConfigOptionDef* opt = config.def()->get(opt_key);
    return opt->full_label.empty() ? opt->label : opt->full_label;
}

static wxString get_string_from_enum(const std::string& opt_key, const DynamicPrintConfig& config, bool is_infill = false, int idx = -1)
{
    const ConfigOptionDef& def = config.def()->options.at(opt_key);
    const std::vector<std::string>& names = def.enum_labels;
    int val = 0;
    if (idx >= 0)
        val = dynamic_cast<const ConfigOptionInts*>(config.option(opt_key))->get_at(idx);
    else
        val = config.option(opt_key)->getInt();

    if (is_infill) {
        for (auto key_val : *def.enum_keys_map)
            if (int(key_val.second) == val) {
                auto it = std::find(def.enum_values.begin(), def.enum_values.end(), key_val.first);
                if (it == def.enum_values.end())
                    return "";
                return from_u8(_utf8(names[it - def.enum_values.begin()]));
            }
        return _L("Undef");
    }
    return from_u8(_utf8(names[val]));
}

static wxString get_string_value(std::string opt_key, const DynamicPrintConfig& config)
{
    int orig_opt_idx = -1;
    int opt_idx = -1;
    int pos = opt_key.find("#");
    std::string temp_str = opt_key;
    if (pos > 0) {
        boost::erase_head(temp_str, pos + 1);
        orig_opt_idx = static_cast<size_t>(atoi(temp_str.c_str()));
    }
    opt_idx = orig_opt_idx >= 0 ? orig_opt_idx : 0;
    opt_key = get_pure_opt_key(opt_key);
    auto option = config.option(opt_key);
    if (!option)
        return _L("N/A");

    if ((option->is_scalar() && config.option(opt_key)->is_nil()) ||
        (option->is_vector() && dynamic_cast<const ConfigOptionVectorBase*>(config.option(opt_key))->is_nil(opt_idx)))
        return _L("N/A");

    wxString out;
    const ConfigOptionDef* opt = config.def()->get(opt_key);
    bool is_nullable = opt->nullable;

    switch (opt->type) {
    case coInt:
        return from_u8((boost::format("%1%") % config.opt_int(opt_key)).str());
    case coInts: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionIntsNullable>(opt_key);
            if (opt_idx < values->size())
                return from_u8((boost::format("%1%") % values->get_at(opt_idx)).str());
        } else {
            auto values = config.opt<ConfigOptionInts>(opt_key);
            if (orig_opt_idx >= 0 && orig_opt_idx < values->size()) {
                return from_u8((boost::format("%1%") % values->get_at(opt_idx)).str());
            } else {
                std::string value_str;
                for (int i = 0; i < values->size(); i++) {
                    value_str += std::to_string(values->get_at(i));
                    if (i != values->size() - 1) value_str += ",";
                }
                return from_u8(value_str);
            }
        }
        return _L("Undef");
    }
    case coBool:
        return config.opt_bool(opt_key) ? "true" : "false";
    case coBools: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionBoolsNullable>(opt_key);
            if (opt_idx < values->size())
                return values->get_at(opt_idx) ? "true" : "false";
        } else {
            auto values = config.opt<ConfigOptionBools>(opt_key);
            if (opt_idx < values->size())
                return values->get_at(opt_idx) ? "true" : "false";
        }
        return _L("Undef");
    }
    case coPercent:
        return from_u8((boost::format("%1%%%") % int(config.optptr(opt_key)->getFloat())).str());
    case coPercents: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionPercentsNullable>(opt_key);
            if (opt_idx < values->size())
                return from_u8((boost::format("%1%%%") % values->get_at(opt_idx)).str());
        } else {
            auto values = config.opt<ConfigOptionPercents>(opt_key);
            if (opt_idx < values->size())
                return from_u8((boost::format("%1%%%") % values->get_at(opt_idx)).str());
        }
        return _L("Undef");
    }
    case coFloat:
        return double_to_string(config.opt_float(opt_key));
    case coFloats: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionFloatsNullable>(opt_key);
            if (opt_idx < values->size())
                return double_to_string(values->get_at(opt_idx));
        } else {
            auto values = config.opt<ConfigOptionFloats>(opt_key);
            if (values && opt_idx < values->size())
                return double_to_string(values->get_at(opt_idx));
        }
        return _L("Undef");
    }
    case coString:
        return from_u8(config.opt_string(opt_key));
    case coStrings: {
        const ConfigOptionStrings* strings = config.opt<ConfigOptionStrings>(opt_key);
        if (strings) {
            if (opt_key == "compatible_printers" || opt_key == "compatible_prints") {
                if (strings->empty()) return _L("All");
                for (size_t id = 0; id < strings->size(); id++)
                    out += from_u8(strings->get_at(id)) + "\n";
                out.RemoveLast(1);
                return out;
            }
            if (!strings->empty() && opt_idx < strings->values.size())
                return from_u8(strings->get_at(opt_idx));
        }
        break;
    }
    case coFloatOrPercent: {
        const ConfigOptionFloatOrPercent* fop = config.opt<ConfigOptionFloatOrPercent>(opt_key);
        if (fop)
            out = double_to_string(fop->value) + (fop->percent ? "%" : "");
        return out;
    }
    case coEnum:
        return get_string_from_enum(opt_key, config,
            opt_key == "top_surface_pattern" || opt_key == "bottom_surface_pattern" ||
            opt_key == "internal_solid_infill_pattern" || opt_key == "sparse_infill_pattern" ||
            opt_key == "ironing_pattern" || opt_key == "support_ironing_pattern" ||
            opt_key == "support_pattern" || opt_key == "support_interface_pattern");
    case coEnums:
        return get_string_from_enum(opt_key, config,
            opt_key == "top_surface_pattern" || opt_key == "bottom_surface_pattern" ||
            opt_key == "internal_solid_infill_pattern" || opt_key == "sparse_infill_pattern" ||
            opt_key == "ironing_pattern" || opt_key == "support_ironing_pattern" ||
            opt_key == "support_pattern" || opt_key == "support_interface_pattern",
            opt_idx);
    case coPoint: {
        Vec2d val = config.opt<ConfigOptionPoint>(opt_key)->value;
        return from_u8((boost::format("[%1%]") % ConfigOptionPoint(val).serialize()).str());
    }
    case coPoints: {
        if (opt_key == "printable_area" || opt_key == "thumbnails") {
            ConfigOptionPoints points = *config.option<ConfigOptionPoints>(opt_key);
            return get_thumbnails_string(points.values);
        } else if (opt_key == "bed_exclude_area" || opt_key == "head_wrap_detect_zone" || opt_key == "wrapping_exclude_area") {
            return get_thumbnails_string(config.option<ConfigOptionPoints>(opt_key)->values);
        }
        Vec2d val = config.opt<ConfigOptionPoints>(opt_key)->get_at(opt_idx);
        return from_u8((boost::format("[%1%]") % ConfigOptionPoint(val).serialize()).str());
    }
    default:
        break;
    }
    return out;
}

// ---- SyncMergeDialog implementation ----

SyncMergeDialog::SyncMergeDialog(wxWindow* parent, const SyncConflict& conflict)
    : DPIDialog(parent, wxID_ANY, _L("Merge Sync Changes"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_preset_type(conflict.preset_type)
    , m_local_json_content(conflict.local_content)
{
    m_parse_ok = parse_configs(conflict);
    build_ui(conflict);
    if (m_parse_ok)
        populate_tree();
    CenterOnParent();
}

std::string SyncMergeDialog::format_time(long long timestamp)
{
    if (timestamp <= 0)
        return "Unknown";
    std::time_t t = static_cast<std::time_t>(timestamp);
    std::tm* tm   = std::localtime(&t);
    if (!tm) return "Unknown";
    std::ostringstream ss;
    ss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

bool SyncMergeDialog::parse_configs(const SyncConflict& conflict)
{
    // Detect deletion conflicts (one side is empty)
    if (conflict.local_content.empty()) {
        m_local_deleted = true;
        m_is_deletion_conflict = true;
    }
    if (conflict.remote_content.empty()) {
        m_remote_deleted = true;
        m_is_deletion_conflict = true;
    }

    ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::EnableSilent);
    std::map<std::string, std::string> kv;
    std::string reason;

    if (!m_local_deleted) {
        BOOST_LOG_TRIVIAL(debug) << "SyncMergeDialog: parsing local content (" << conflict.local_content.size() << " bytes)";
        int rc = m_local_config.load_from_json_string(conflict.local_content, ctx, false, kv, reason);
        if (rc != 0) {
            BOOST_LOG_TRIVIAL(error) << "SyncMergeDialog: failed to parse local config: " << reason;
            return false;
        }
        BOOST_LOG_TRIVIAL(debug) << "SyncMergeDialog: local config has " << m_local_config.keys().size() << " keys";
        for (const auto& k : m_local_config.keys())
            BOOST_LOG_TRIVIAL(debug) << "SyncMergeDialog:   local key: " << k << " = " << m_local_config.opt_serialize(k);
    }

    if (!m_remote_deleted) {
        ctx.substitutions.clear();
        kv.clear();
        BOOST_LOG_TRIVIAL(debug) << "SyncMergeDialog: parsing remote content (" << conflict.remote_content.size() << " bytes)";
        int rc = m_remote_config.load_from_json_string(conflict.remote_content, ctx, false, kv, reason);
        if (rc != 0) {
            BOOST_LOG_TRIVIAL(error) << "SyncMergeDialog: failed to parse remote config: " << reason;
            return false;
        }
        BOOST_LOG_TRIVIAL(debug) << "SyncMergeDialog: remote config has " << m_remote_config.keys().size() << " keys";
        for (const auto& k : m_remote_config.keys())
            BOOST_LOG_TRIVIAL(debug) << "SyncMergeDialog:   remote key: " << k << " = " << m_remote_config.opt_serialize(k);
    }

    return true;
}

void SyncMergeDialog::populate_tree()
{
    if (!m_tree)
        return;

    // Check if tab exists (may not if sidebar isn't initialized)
    Tab* tab = wxGetApp().get_tab(m_preset_type);
    if (!tab) {
        BOOST_LOG_TRIVIAL(error) << "SyncMergeDialog: get_tab(" << m_preset_type << ") returned null";
        return;
    }

    Search::OptionsSearcher& searcher = wxGetApp().sidebar().get_searcher();
    searcher.sort_options_by_key();

    m_tree->model->AddPreset(m_preset_type, _L("Local") + " vs " + _L("Remote"), ptFFF);

    // For deletion conflicts, show all keys from the non-deleted side
    t_config_option_keys diff_keys;
    if (m_is_deletion_conflict) {
        const auto& populated = m_local_deleted ? m_remote_config : m_local_config;
        diff_keys = populated.keys();
    } else {
        diff_keys = m_local_config.diff(m_remote_config);
    }
    BOOST_LOG_TRIVIAL(debug) << "SyncMergeDialog: diff_keys.size() = " << diff_keys.size()
                             << ", preset_type = " << static_cast<int>(m_preset_type)
                             << ", is_deletion = " << m_is_deletion_conflict;

    const std::map<wxString, std::string>& category_icon_map = tab->get_category_icon_map();
    auto get_category_icon = [&category_icon_map](const wxString& key) {
        auto it = category_icon_map.find(key);
        return it != category_icon_map.end() ? it->second : std::string();
    };

    for (const auto& opt_key : diff_keys) {
        BOOST_LOG_TRIVIAL(debug) << "SyncMergeDialog: processing diff key: " << opt_key;
        wxString local_val  = m_local_deleted  ? _L("(Deleted)") : get_string_value(opt_key, m_local_config);
        wxString remote_val = m_remote_deleted ? _L("(Deleted)") : get_string_value(opt_key, m_remote_config);

        const std::string lookup_key = get_pure_opt_key(opt_key);
        Search::Option option = searcher.get_option(lookup_key, m_preset_type);
        if (get_pure_opt_key(option.opt_key()) != lookup_key)
            option = searcher.get_option(opt_key, get_full_label(opt_key, m_local_config), m_preset_type);
        if (get_pure_opt_key(option.opt_key()) != lookup_key) {
            // Metadata keys (e.g. *_settings_id, compatible_*, inherits) are not
            // real settings — skip them in the merge UI.
            continue;
        }

        m_tree->Append(opt_key, m_preset_type, option.category_local, option.group_local, option.label_local,
                       local_val, remote_val, get_category_icon(option.category));
        m_has_visible_diffs = true;
    }

    searcher.sort_options_by_label();
}

std::string SyncMergeDialog::build_merged_json(const std::vector<std::string>& selected_remote_keys,
                                                const std::string& base_json_content)
{
    try {
        json j = json::parse(base_json_content);

        for (const auto& key : selected_remote_keys) {
            const ConfigOption* opt = m_remote_config.option(key);
            if (!opt)
                continue;

            if (opt->is_scalar()) {
                if (opt->type() == coString)
                    j[key] = dynamic_cast<const ConfigOptionString*>(opt)->value;
                else
                    j[key] = opt->serialize();
            } else {
                const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(opt);
                if (vec) {
                    auto serialized = vec->vserialize();
                    j[key] = json(serialized);
                }
            }
        }

        return j.dump(4);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "SyncMergeDialog: failed to build merged JSON: " << e.what();
        return base_json_content;
    }
}

void SyncMergeDialog::build_ui(const SyncConflict& conflict)
{
    SetBackgroundColour(*wxWHITE);
    int em = em_unit();

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    // Title
    auto* title = new wxStaticText(this, wxID_ANY, _L("Merge Sync Changes"));
    title->SetFont(title->GetFont().Bold().Scaled(1.2f));
    main_sizer->Add(title, 0, wxALL | wxALIGN_LEFT, FromDIP(15));

    // File path
    auto* path_label = new wxStaticText(this, wxID_ANY,
        _L("File") + ": " + wxString::FromUTF8(conflict.path));
    main_sizer->Add(path_label, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(15));

    // Time info grid
    auto* grid = new wxFlexGridSizer(2, 2, FromDIP(8), FromDIP(20));
    grid->AddGrowableCol(1);

    auto add_info = [&](const wxString& label, const wxString& value) {
        auto* lbl = new wxStaticText(this, wxID_ANY, label);
        lbl->SetFont(lbl->GetFont().Bold());
        grid->Add(lbl, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(this, wxID_ANY, value), 0, wxALIGN_LEFT);
    };

    add_info(_L("Local version modified"),
             wxString::FromUTF8(format_time(conflict.local_time)));
    add_info(_L("Remote version modified"),
             wxString::FromUTF8(format_time(conflict.remote_time)));

    main_sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(15));

    // Separator
    main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

    // Info text — different message for deletion conflicts
    wxString info_text;
    if (m_remote_deleted)
        info_text = _L("This preset was deleted remotely but modified locally.");
    else if (m_local_deleted)
        info_text = _L("This preset was deleted locally but modified remotely.");
    else
        info_text = _L("Check options to take remote value, uncheck to keep local value.");
    auto* info = new wxStaticText(this, wxID_ANY, info_text);
    info->SetForegroundColour(wxColour(100, 100, 100));
    main_sizer->Add(info, 0, wxALL, FromDIP(15));

    // DiffViewCtrl tree
    m_tree = new DiffViewCtrl(this, wxSize(80 * em, 30 * em));
    m_tree->AppendToggleColumn_(L"\u2714",        DiffModel::colToggle,   6);
    m_tree->AppendBmpTextColumn("",               DiffModel::colIconText, 35, true);
    m_tree->AppendBmpTextColumn(_L("Local Value"),  DiffModel::colOldValue, 15);
    m_tree->AppendBmpTextColumn(_L("Remote Value"), DiffModel::colNewValue, 15);
    main_sizer->Add(m_tree, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

    // Buttons
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto* btn_merge = new wxButton(this, wxID_ANY, _L("Apply Merge"));
    btn_merge->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        auto selected = m_tree->options(m_preset_type, true); // checked = take remote
        m_result.resolution     = ConflictResolution::Merge;
        m_result.merged_content = build_merged_json(selected, m_local_json_content);
        EndModal(wxID_OK);
    });

    auto* btn_local = new wxButton(this, wxID_ANY, _L("Keep All Local"));
    btn_local->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_result.resolution = ConflictResolution::KeepLocal;
        EndModal(wxID_OK);
    });

    auto* btn_remote = new wxButton(this, wxID_ANY, _L("Keep All Remote"));
    btn_remote->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_result.resolution = ConflictResolution::KeepRemote;
        EndModal(wxID_OK);
    });

    auto* btn_skip = new wxButton(this, wxID_CANCEL, _L("Skip"));
    btn_skip->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_result.resolution = ConflictResolution::Skip;
        EndModal(wxID_CANCEL);
    });

    // Disable merge/remote buttons if configs couldn't be parsed — pressing
    // "Apply Merge" with an empty tree would silently produce an empty merge.
    if (!m_parse_ok) {
        btn_merge->Enable(false);
        btn_remote->Enable(false);
    }
    // Disable "Apply Merge" for deletion conflicts — can't cherry-pick fields from a deleted side
    if (m_is_deletion_conflict) {
        btn_merge->Enable(false);
    }

    // Update "Apply Merge" state when user toggles checkboxes —
    // merge with zero selected options is meaningless.
    auto update_merge_btn = [this, btn_merge]() {
        if (!m_parse_ok) return;
        auto selected = m_tree->options(m_preset_type, true);
        btn_merge->Enable(!selected.empty());
    };

    // event.Skip() lets DiffViewCtrl::item_value_changed() propagate parent/child checkbox toggles
    m_tree->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [update_merge_btn](wxDataViewEvent& event) {
        event.Skip();
        update_merge_btn();
    });

    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(btn_merge,  0, wxRIGHT, FromDIP(8));
    btn_sizer->Add(btn_local,  0, wxRIGHT, FromDIP(8));
    btn_sizer->Add(btn_remote, 0, wxRIGHT, FromDIP(8));
    btn_sizer->Add(btn_skip,   0);

    main_sizer->Add(btn_sizer, 0, wxALL | wxEXPAND, FromDIP(15));

    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);

    SetMinSize(wxSize(FromDIP(700), FromDIP(500)));
    Fit();
}

void SyncMergeDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    if (m_tree)
        m_tree->Rescale();
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
