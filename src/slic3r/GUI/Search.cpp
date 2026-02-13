#include "Search.hpp"

#include <vector>
#include <cstddef>
#include <string>
#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <boost/nowide/convert.hpp>

#include "wx/dataview.h"
#include "wx/numformatter.h"
#include "Widgets/Label.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

#define FTS_FUZZY_MATCH_IMPLEMENTATION
#include "fts_fuzzy_match.h"

#include "imgui/imconfig.h"

using boost::optional;

namespace Slic3r {

wxDEFINE_EVENT(wxCUSTOMEVT_JUMP_TO_OPTION, wxCommandEvent);
wxDEFINE_EVENT(wxCUSTOMEVT_EXIT_SEARCH, wxCommandEvent);
wxDEFINE_EVENT(wxCUSTOMEVT_JUMP_TO_OBJECT, wxCommandEvent);

using GUI::from_u8;
using GUI::into_u8;

namespace Search {

static char marker_by_type(Preset::Type type, PrinterTechnology pt)
{
    switch (type) {
    case Preset::TYPE_PRINT:
    case Preset::TYPE_SLA_PRINT: return ImGui::PrintIconMarker;
    case Preset::TYPE_FILAMENT: return ImGui::FilamentIconMarker;
    case Preset::TYPE_SLA_MATERIAL: return ImGui::MaterialIconMarker;
    case Preset::TYPE_PRINTER: return pt == ptSLA ? ImGui::PrinterSlaIconMarker : ImGui::PrinterIconMarker;
    default: return ' ';
    }
}

std::string Option::opt_key() const { return into_u8(key).substr(2); }

void FoundOption::get_marked_label_and_tooltip(const char **label_, const char **tooltip_) const
{
    *label_   = marked_label.c_str();
    *tooltip_ = tooltip.c_str();
}

template<class T>
// void change_opt_key(std::string& opt_key, DynamicPrintConfig* config)
void change_opt_key(std::string &opt_key, DynamicPrintConfig *config, int &cnt)
{
    T *opt_cur = static_cast<T *>(config->option(opt_key));
    cnt        = opt_cur->values.size();
    return;

    if (opt_cur->values.size() > 0) opt_key += "#" + std::to_string(0);
}

static std::string get_key(const std::string &opt_key, Preset::Type type) { return std::to_string(int(type)) + ";" + opt_key; }

void OptionsSearcher::append_options(DynamicPrintConfig *config, Preset::Type type, ConfigOptionMode mode)
{
    auto emplace = [this, type](const std::string key, const wxString &label) {
        const GroupAndCategory &gc = groups_and_categories[key];
        if (gc.group.IsEmpty() || gc.category.IsEmpty()) return;

        wxString suffix;
        wxString suffix_local;
        if (gc.category == "Machine limits") {
            //suffix       = key.back() == '1' ? L("Stealth") : L("Normal");
            suffix       = key.back() == '1' ? wxEmptyString : wxEmptyString;
            suffix_local = " " + _(suffix);
            suffix       = " " + suffix;
        }

        if (!label.IsEmpty())
            options.emplace_back(Option{boost::nowide::widen(key), type, (label + suffix).ToStdWstring(), (_(label) + suffix_local).ToStdWstring(), gc.group.ToStdWstring(),
                                        _(gc.group).ToStdWstring(), gc.category.ToStdWstring(), GUI::Tab::translate_category(gc.category, type).ToStdWstring()});
    };

    for (std::string opt_key : config->keys()) {
        const ConfigOptionDef &opt = config->def()->options.at(opt_key);
        if (opt.mode > mode) continue;

        int cnt = 0;

        if ((type == Preset::TYPE_SLA_MATERIAL || type == Preset::TYPE_PRINTER) && opt_key != "printable_area")
            switch (config->option(opt_key)->type()) {
            case coInts: change_opt_key<ConfigOptionInts>(opt_key, config, cnt); break;
            case coBools: change_opt_key<ConfigOptionBools>(opt_key, config, cnt); break;
            case coFloats: change_opt_key<ConfigOptionFloats>(opt_key, config, cnt); break;
            case coStrings: change_opt_key<ConfigOptionStrings>(opt_key, config, cnt); break;
            case coPercents: change_opt_key<ConfigOptionPercents>(opt_key, config, cnt); break;
            case coPoints: change_opt_key<ConfigOptionPoints>(opt_key, config, cnt); break;
            // BBS
            case coEnums: change_opt_key<ConfigOptionInts>(opt_key, config, cnt); break;
            default: break;
            }

        wxString label = opt.full_label.empty() ? opt.label : opt.full_label;

        std::string key = get_key(opt_key, type);
        if (cnt == 0)
            emplace(key, label);
        else
            for (int i = 0; i < cnt; ++i)
                // ! It's very important to use "#". opt_key#n is a real option key used in GroupAndCategory
                emplace(key + "#" + std::to_string(i), label);
    }
}

inline void OptionsSearcher::sort_options()
{
    std::sort(options.begin(), options.end(), [](const Option &o1, const Option &o2) { return o1.label < o2.label; });
    Option * last = nullptr;
    for (auto& opt : options) {
        if (last && last->label == opt.label && last->group == opt.group && last->type == opt.type && last->category != opt.category) {
            last->multi_category = true;
            opt.multi_category = true;
        }
        last = &opt;
    }
}

// Mark a string using ColorMarkerStart and ColorMarkerEnd symbols
static std::wstring mark_string(const std::wstring &str, const std::vector<uint16_t> &matches, Preset::Type type, PrinterTechnology pt)
{
    std::wstring out;
    out += marker_by_type(type, pt);
    if (matches.empty())
        out += str;
    else {
        out.reserve(str.size() * 2);
        if (matches.front() > 0) out += str.substr(0, matches.front());
        for (size_t i = 0;;) {
            // Find the longest string of successive indices.
            size_t j = i + 1;
            while (j < matches.size() && matches[j] == matches[j - 1] + 1) ++j;
            out += ImGui::ColorMarkerStart;
            out += str.substr(matches[i], matches[j - 1] - matches[i] + 1);
            out += ImGui::ColorMarkerEnd;
            if (j == matches.size()) {
                out += str.substr(matches[j - 1] + 1);
                break;
            }
            out += str.substr(matches[j - 1] + 1, matches[j] - matches[j - 1] - 1);
            i = j;
        }
    }
    return out;
}

bool OptionsSearcher::search() { return search(search_line, true); }

static bool fuzzy_match(const std::wstring &search_pattern, const std::wstring &label, int &out_score, std::vector<uint16_t> &out_matches)
{
    uint16_t matches[fts::max_matches + 1]; // +1 for the stopper
    int      score;
    if (fts::fuzzy_match(search_pattern.c_str(), label.c_str(), score, matches)) {
        size_t cnt = 0;
        for (; matches[cnt] != fts::stopper; ++cnt)
            ;
        out_matches.assign(matches, matches + cnt);
        out_score = score;
        return true;
    } else
        return false;
}

bool OptionsSearcher::search(const std::string &search, bool force /* = false*/, Preset::Type type/* = Preset::TYPE_INVALID*/)
{
    if (search_line == search && search_type == type && !force) return false;

    found.clear();

    bool         full_list = search.empty();
    std::wstring sep       = L" : ";

    auto get_label = [this, &sep](const Option &opt, bool marked = true) {
        std::wstring out;
        if (marked) out += marker_by_type(opt.type, printer_technology);
        const std::wstring *prev = nullptr;
        for (const std::wstring *const s : {view_params.category || opt.multi_category ? &opt.category_local : nullptr, &opt.group_local, &opt.label_local})
            if (s != nullptr && (prev == nullptr || *prev != *s)) {
                if (out.size() > 2) out += sep;
                out += *s;
                prev = s;
            }
        return out;
    };

    auto get_label_english = [this, &sep](const Option &opt, bool marked = true) {
        std::wstring out;
        if (marked) out += marker_by_type(opt.type, printer_technology);
        const std::wstring *prev = nullptr;
        for (const std::wstring *const s : {view_params.category || opt.multi_category ? &opt.category : nullptr, &opt.group, &opt.label})
            if (s != nullptr && (prev == nullptr || *prev != *s)) {
                if (out.size() > 2) out += sep;
                out += *s;
                prev = s;
            }
        return out;
    };

    auto get_tooltip = [this, &sep](const Option &opt) {
        return marker_by_type(opt.type, printer_technology) + opt.category_local + sep + opt.group_local + sep + opt.label_local;
    };

    std::vector<uint16_t> matches, matches2;
    for (size_t i = 0; i < options.size(); i++) {
        const Option &opt = options[i];
        if (full_list) {
            std::string label = into_u8(get_label(opt));
            //all
            if (type == Preset::TYPE_INVALID) { 
                found.emplace_back(FoundOption{label, label, into_u8(get_tooltip(opt)), i, 0});
            } else if (type == opt.type){
                found.emplace_back(FoundOption{label, label, into_u8(get_tooltip(opt)), i, 0});
            }
            
            continue;
        }

        std::wstring wsearch = boost::nowide::widen(search);
        boost::trim_left(wsearch);
        std::wstring label         = get_label(opt, false);
        std::wstring label_english = get_label_english(opt, false);
        int          score         = std::numeric_limits<int>::min();
        int          score2;
        matches.clear();
        fuzzy_match(wsearch, label, score, matches);
        // bbs hide the contents in parentheses
        /* if (fuzzy_match(wsearch, opt.key, score2, matches2) && score2 > score) {
             for (fts::pos_type &pos : matches2) pos += label.size() + 1;
             label += L"(" + opt.key + L")";
             append(matches, matches2);
             score = score2;
         }*/
        if (view_params.english && fuzzy_match(wsearch, label_english, score2, matches2) && score2 > score) {
            label   = std::move(label_english);
            matches = std::move(matches2);
            score   = score2;
        }
        if (score > 90 /*std::numeric_limits<int>::min()*/) {
            label = mark_string(label, matches, opt.type, printer_technology);
            //label += L"  [" + std::to_wstring(score) + L"]"; // add score value
            std::string label_u8    = into_u8(label);
            std::string label_plain = label_u8;

#ifdef SUPPORTS_MARKUP
            boost::replace_all(label_plain, std::string(1, char(ImGui::ColorMarkerStart)), "<b>");
            boost::replace_all(label_plain, std::string(1, char(ImGui::ColorMarkerEnd)), "</b>");
#else
            boost::erase_all(label_plain, std::string(1, char(ImGui::ColorMarkerStart)));
            boost::erase_all(label_plain, std::string(1, char(ImGui::ColorMarkerEnd)));
#endif

            if (type == Preset::TYPE_INVALID) {
                found.emplace_back(FoundOption{label_plain, label_u8, into_u8(get_tooltip(opt)), i, score});
            } else if (type == opt.type) {
                found.emplace_back(FoundOption{label_plain, label_u8, into_u8(get_tooltip(opt)), i, score});
            }
            
        }
    }

    if (!full_list) sort_found();

    if (search_line != search) search_line = search;
    if (search_type != type) search_type = type;

    return true;
}

OptionsSearcher::OptionsSearcher() {}

OptionsSearcher::~OptionsSearcher() {}

void OptionsSearcher::init(std::vector<InputInfo> input_values)
{
    options.clear();
    for (auto i : input_values) append_options(i.config, i.type, i.mode);
    sort_options();

    search(search_line, true, search_type);
}

void OptionsSearcher::apply(DynamicPrintConfig *config, Preset::Type type, ConfigOptionMode mode)
{
    if (options.empty()) return;

    options.erase(std::remove_if(options.begin(), options.end(), [type](Option opt) { return opt.type == type; }), options.end());

    append_options(config, type, mode);

    sort_options();

    search(search_line, true, search_type);
}

const Option &OptionsSearcher::get_option(size_t pos_in_filter) const
{
    assert(pos_in_filter != size_t(-1) && found[pos_in_filter].option_idx != size_t(-1));
    return options[found[pos_in_filter].option_idx];
}

const Option &OptionsSearcher::get_option(const std::string &opt_key, Preset::Type type) const
{
    auto it = std::lower_bound(options.begin(), options.end(), Option({boost::nowide::widen(get_key(opt_key, type))}));
    // BBS: return the 0th option when not found in searcher caused by mode difference
    // assert(it != options.end());
    if (it == options.end()) return options[0];

    return options[it - options.begin()];
}

static Option create_option(const std::string &opt_key, const wxString &label, Preset::Type type, const GroupAndCategory &gc)
{
    wxString suffix;
    wxString suffix_local;
    if (gc.category == "Machine limits") {
        //suffix       = opt_key.back() == '1' ? L("Stealth") : L("Normal");
        suffix       = opt_key.back() == '1' ? wxEmptyString : wxEmptyString;
        suffix_local = " " + _(suffix);
        suffix       = " " + suffix;
    }

    wxString category = gc.category;
    if (type == Preset::TYPE_PRINTER && category.Contains("Extruder ")) {
        std::string opt_idx = opt_key.substr(opt_key.find("#") + 1);
        category            = wxString::Format("%s %d", "Extruder", atoi(opt_idx.c_str()) + 1);
    }

    return Option{boost::nowide::widen(get_key(opt_key, type)),
                  type,
                  (label + suffix).ToStdWstring(),
                  (_(label) + suffix_local).ToStdWstring(),
                  gc.group.ToStdWstring(),
                  _(gc.group).ToStdWstring(),
                  gc.category.ToStdWstring(),
                  GUI::Tab::translate_category(category, type).ToStdWstring()};
}

Option OptionsSearcher::get_option(const std::string &opt_key, const wxString &label, Preset::Type type) const
{
    std::string key = get_key(opt_key, type);
    auto        it  = std::lower_bound(options.begin(), options.end(), Option({boost::nowide::widen(key)}));
    // BBS: return the 0th option when not found in searcher caused by mode difference
    if (it == options.end()) return options[0];
    if (it->key == boost::nowide::widen(key)) return options[it - options.begin()];
    if (groups_and_categories.find(key) == groups_and_categories.end()) {
        size_t pos = key.find('#');
        if (pos == std::string::npos) return options[it - options.begin()];

        std::string zero_opt_key = key.substr(0, pos + 1) + "0";

        if (groups_and_categories.find(zero_opt_key) == groups_and_categories.end()) return options[it - options.begin()];

        return create_option(opt_key, label, type, groups_and_categories.at(zero_opt_key));
    }

    const GroupAndCategory &gc = groups_and_categories.at(key);
    if (gc.group.IsEmpty() || gc.category.IsEmpty()) return options[it - options.begin()];

    return create_option(opt_key, label, type, gc);
}

void OptionsSearcher::show_dialog(Preset::Type type, wxWindow *parent, TextInput *input, wxWindow* ssearch_btn)
{
    if (parent == nullptr || input == nullptr) return;
    auto    search_dialog = new SearchDialog(this, type, parent, input, ssearch_btn);
    wxPoint pos = input->GetParent()->ClientToScreen(wxPoint(0, 0));
#ifndef __WXGTK__
    pos.y += input->GetParent()->GetRect().height;
#else
    input->GetParent()->Hide();
#endif
    search_dialog->SetPosition(pos);
    search_dialog->Popup();
}

void OptionsSearcher::dlg_sys_color_changed()
{
    /*if (search_dialog)
        search_dialog->on_sys_color_changed();*/
}

void OptionsSearcher::dlg_msw_rescale()
{
    if (search_dialog) search_dialog->msw_rescale();
}

void OptionsSearcher::add_key(const std::string &opt_key, Preset::Type type, const wxString &group, const wxString &category)
{
    groups_and_categories[get_key(opt_key, type)] = GroupAndCategory{group, category};
}
//------------------------------------------
//          SearchItem
//------------------------------------------

SearchItem::SearchItem(wxWindow *parent, wxString text, int index, SearchDialog* sdialog, SearchObjectDialog* search_dialog, wxString tooltip)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->GetSize().GetWidth(), 3 * GUI::wxGetApp().em_unit()))
{
    m_sdialog = sdialog;
    m_search_object_dialog = search_dialog;
    m_text  = text;
    m_index = index;

    this->SetToolTip(tooltip);

    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    Bind(wxEVT_ENTER_WINDOW, &SearchItem::on_mouse_enter, this);
    Bind(wxEVT_LEAVE_WINDOW, &SearchItem::on_mouse_leave, this);
    Bind(wxEVT_LEFT_DOWN, &SearchItem::on_mouse_left_down, this);
    Bind(wxEVT_LEFT_UP, &SearchItem::on_mouse_left_up, this);
    Bind(wxEVT_PAINT, &SearchItem::OnPaint, this);
}

wxSize SearchItem::DrawTextString(wxDC &dc, const wxString &text, const wxPoint &pt, bool bold)
{
    if (bold) {
        dc.SetFont(Label::Head_14);
    } else {
        dc.SetFont(Label::Body_14);
    }

    dc.SetBackgroundMode(wxTRANSPARENT);
    dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#323A3C")));
    dc.DrawText(text, pt);
    return dc.GetTextExtent(text);
}

void SearchItem::OnPaint(wxPaintEvent &event)
{
    wxPaintDC dc(this);
    auto      top  = 5;
    int       left = 20;

    auto bold_pair = std::vector<std::pair<int, int>>();
    
    auto index     = 0;

    auto b_first_list  = std::vector<int>();
    auto b_second_list = std::vector<int>();

    auto position      = 0;
    while ((position = m_text.find("<b>", position)) != wxString::npos) {
        b_first_list.push_back(position);
        position++;
    }

    position = 0;
    while ((position = m_text.find("</b>", position)) != wxString::npos) {
        b_second_list.push_back(position + 3);
        position++;
    }

    if (b_first_list.size() != b_second_list.size()) { return; }

    for (auto i = 0; i < b_first_list.size(); i++) {
        auto pair = std::make_pair(b_first_list[i], b_second_list[i]);
        bold_pair.push_back(pair);
    }

    //DrawTextString(dc, m_text, wxPoint(left, top), false);
    /*if (bold_pair.size() <= 0) {
        DrawTextString(dc, m_text, wxPoint(left, top), false);
    } else {
        auto index = 0;
        for (auto i = 0; i < bold_pair.size(); i++) { DrawTextString(dc, m_text.SubString(index, bold_pair[i].second), wxPoint(left, top), true); }
    }*/
    auto str = wxString("");
    for (auto c = 0; c < m_text.length(); c++) {
        str = m_text[c];

        auto inset = false;
        auto pair_index = 0;
        for (auto o = 0; o < bold_pair.size(); o++) {
            if (c >= bold_pair[o].first && c <= bold_pair[o].second) { 
                pair_index = o;
                inset = true;
                break;
            }
        }

        if (!inset) { 
            left += DrawTextString(dc, str, wxPoint(left, top), false).GetWidth();
        } else {
            //str = str.erase(bold_pair[pair_index].first, 3);
            //str = str.erase(bold_pair[pair_index].second, 4);
            if (c - bold_pair[pair_index].first >= 3 && bold_pair[pair_index].second - c > 3) { 
                left += DrawTextString(dc, str, wxPoint(left, top), true).GetWidth();
            }
        }
    }
}

void SearchItem::on_mouse_enter(wxMouseEvent &evt)
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#BFE1DE"))); // ORCA color with %25 opacity
    Refresh();
}

void SearchItem::on_mouse_leave(wxMouseEvent &evt)
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour(255, 255, 255)));
    Refresh();
}

void SearchItem::on_mouse_left_down(wxMouseEvent &evt)
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#BFE1DE"))); // ORCA color with %25 opacity
    Refresh();
}

void SearchItem::on_mouse_left_up(wxMouseEvent &evt)
{

    //if (m_sdialog->prevent_list_events) return;
    // if (wxGetMouseState().LeftIsDown())
    if (m_sdialog) {
        m_sdialog->Die();
        wxCommandEvent event(wxCUSTOMEVT_JUMP_TO_OPTION);
        event.SetInt(m_index);
        wxPostEvent(GUI::wxGetApp().plater(), event);
    }

    if (m_search_object_dialog) {
        m_search_object_dialog->Die();
        wxCommandEvent event(wxCUSTOMEVT_JUMP_TO_OBJECT);
        event.SetClientData(m_item);
        wxPostEvent(GUI::wxGetApp().plater(), event);
    }
}

//------------------------------------------
//          SearchDialog
//------------------------------------------

static const std::map<const char, int> icon_idxs = {
    {ImGui::PrintIconMarker, 0}, {ImGui::PrinterIconMarker, 1}, {ImGui::PrinterSlaIconMarker, 2}, {ImGui::FilamentIconMarker, 3}, {ImGui::MaterialIconMarker, 4},
};

SearchDialog::SearchDialog(OptionsSearcher *searcher, Preset::Type type, wxWindow *parent, TextInput *input, wxWindow *search_btn) 
    : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS), searcher(searcher)
{
    m_event_tag       = parent;
    search_line       = input;
    search_type       = type;

    m_search_item_tag = search_btn;

    // set border color
    Freeze();
    SetBackgroundColour(wxColour(238, 238, 238));

    em = GUI::wxGetApp().em_unit();

    m_bg_colour    = wxColour(255, 255, 255);
    m_thumb_color  = wxColour(196, 196, 196);

    SetFont(GUI::wxGetApp().normal_font());
    SetSizeHints(wxDefaultSize, wxDefaultSize);

    m_sizer_border = new wxBoxSizer(wxVERTICAL);
    m_sizer_main   = new wxBoxSizer(wxVERTICAL);
    m_sizer_body   = new wxBoxSizer(wxVERTICAL);

    // border
    m_border_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(POPUP_WIDTH * em, POPUP_HEIGHT * em), wxTAB_TRAVERSAL);
    m_border_panel->SetBackgroundColour(m_bg_colour);

    // client
    m_client_panel = new wxPanel(m_border_panel, wxID_ANY, wxDefaultPosition, wxSize(POPUP_WIDTH * em, POPUP_HEIGHT * em), wxTAB_TRAVERSAL);
    m_client_panel->SetBackgroundColour(m_bg_colour);

    // search line
    //search_line = new wxTextCtrl(m_client_panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
#ifdef __WXGTK__
    search_line = new TextInput(m_client_panel, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    search_line->SetBackgroundColour(wxColour(238, 238, 238));
    search_line->SetForegroundColour(wxColour(43, 52, 54));
    search_line->SetFont(GUI::wxGetApp().bold_font());
#endif

    search_line->Bind(wxEVT_TEXT, &SearchDialog::OnInputText, this);
    search_line->Bind(wxEVT_LEFT_UP, &SearchDialog::OnLeftUpInTextCtrl, this);
    search_line2 = search_line->GetTextCtrl();

    // scroll window
    m_scrolledWindow = new ScrolledWindow(m_client_panel, wxID_ANY, wxDefaultPosition, wxSize(POPUP_WIDTH * em - (em + em /2), POPUP_HEIGHT * em), wxVSCROLL, 6, 6);
    m_scrolledWindow->SetMarginColor(m_bg_colour);
    m_scrolledWindow->SetScrollbarColor(m_thumb_color);
    m_scrolledWindow->SetBackgroundColour(m_bg_colour);

    auto m_listsizer = new wxBoxSizer(wxVERTICAL);
    auto m_listPanel = new wxWindow(m_scrolledWindow->GetPanel(), -1);
    m_listPanel->SetBackgroundColour(m_bg_colour);
    m_listPanel->SetSize(wxSize(m_scrolledWindow->GetSize().GetWidth(), -1));

    m_listPanel->SetSizer(m_listsizer);
    m_listPanel->Fit();
    m_scrolledWindow->SetScrollbars(1, 1, 0, m_listPanel->GetSize().GetHeight());

#ifdef __WXGTK__
    m_sizer_body->Add(search_line, 0, wxEXPAND | wxALL, em / 2);
    search_line = input;
#endif
    m_sizer_body->Add(m_scrolledWindow, 0, wxEXPAND | wxALL, em);

    m_client_panel->SetSizer(m_sizer_body);
    m_client_panel->Layout();
    m_sizer_body->Fit(m_client_panel);
    m_sizer_main->Add(m_client_panel, 1, wxEXPAND, 0);

    m_border_panel->SetSizer(m_sizer_main);
    m_border_panel->Layout();
    m_sizer_border->Add(m_border_panel, 1, wxEXPAND | wxALL, 1);

    SetSizer(m_sizer_border);
    Layout();
    m_sizer_border->Fit(this);
    Thaw();

    // check_category->Bind(wxEVT_CHECKBOX, &SearchDialog::OnCheck, this);
    // Bind(wxEVT_MOTION, &SearchDialog::OnMotion, this);
    // Bind(wxEVT_LEFT_DOWN, &SearchDialog::OnLeftDown, this);

    // SetSizer(topSizer);
    // topSizer->SetSizeHints(this);
    GUI::wxGetApp().UpdateDarkUIWin(this);
}

SearchDialog::~SearchDialog() {}

void SearchDialog::Popup(wxPoint position /*= wxDefaultPosition*/)
{
    /* const std::string& line = searcher->search_string();
     search_line->SetValue(line.empty() ? default_string : from_u8(line));

     update_list();

     const OptionViewParameters& params = searcher->view_params;
     check_category->SetValue(params.category);*/

    //const std::string &line = searcher->search_string();
    //search_line->SetValue(line.empty() ? default_string : from_u8(line));
    search_line2->SetValue(wxString(""));
    //const std::string &line = searcher->search_string();
    //searcher->search(into_u8(line), true);
    PopupWindow::Popup();
    search_line2->SetFocus();
    update_list();
}


void SearchDialog::MSWDismissUnfocusedPopup()
{
    Dismiss();
    OnDismiss();
}

void SearchDialog::OnDismiss() { }

void SearchDialog::Dismiss()
{
    auto pos = wxGetMousePosition();
    auto focus_window = wxWindow::FindFocus();
    if (!focus_window)
        Die();
    else if (!m_event_tag->GetScreenRect().Contains(pos) && !this->GetScreenRect().Contains(pos) && !m_search_item_tag->GetScreenRect().Contains(pos)) {
        Die();
    }
}

void SearchDialog::Die() 
{
    PopupWindow::Dismiss();
    wxCommandEvent event(wxCUSTOMEVT_EXIT_SEARCH);
    wxPostEvent(search_line, event);
}

void SearchDialog::OnInputText(wxCommandEvent &)
{
    search_line2->SetInsertionPointEnd();
    wxString input_string = search_line2->GetValue();
    if (input_string == wxEmptyString) input_string.Clear();
    searcher->search(into_u8(input_string), true, search_type);
    update_list();
}

void SearchDialog::OnLeftUpInTextCtrl(wxEvent &event)
{
    if (search_line2->GetValue() == wxEmptyString) search_line2->SetValue("");
    event.Skip();
}

void SearchDialog::update_list()
{
#ifndef __WXGTK__
    Freeze();
#endif
    m_scrolledWindow->Destroy();

    m_scrolledWindow = new ScrolledWindow(m_client_panel, wxID_ANY, wxDefaultPosition, wxSize(POPUP_WIDTH * em - (em + em / 2), POPUP_HEIGHT * em - em), wxVSCROLL, 6, 6);
    m_scrolledWindow->SetMarginColor(StateColor::darkModeColorFor(m_bg_colour));
    m_scrolledWindow->SetScrollbarColor(StateColor::darkModeColorFor(m_thumb_color));
    m_scrolledWindow->SetBackgroundColour(StateColor::darkModeColorFor(m_bg_colour));

    auto m_listsizer = new wxBoxSizer(wxVERTICAL);
    auto m_listPanel = new wxWindow(m_scrolledWindow->GetPanel(), -1);
    m_listPanel->SetBackgroundColour(StateColor::darkModeColorFor(m_bg_colour));
    m_listPanel->SetSize(wxSize(m_scrolledWindow->GetSize().GetWidth(), -1));

    const std::vector<FoundOption> &filters = searcher->found_options();
    auto                            index   = 0;
    for (const FoundOption &item : filters) {
        wxString str = from_u8(item.label).Remove(0, 1);
        auto     tmp = new SearchItem(m_listPanel, str, index, this);
        m_listsizer->Add(tmp, 0, wxEXPAND, 0);
        index++;
    }

    m_listPanel->SetSizer(m_listsizer);
    m_listPanel->Fit();
    m_scrolledWindow->SetScrollbars(1, 1, 0, m_listPanel->GetSize().GetHeight());

    m_sizer_body->Add(m_scrolledWindow, 0, wxEXPAND | wxALL, em);
    m_sizer_body->Fit(m_client_panel);
    m_sizer_body->Layout();
#ifndef __WXGTK__
    Thaw();
#endif
}

void SearchDialog::msw_rescale()
{
}

// ----------------------------------------------------------------------------
// SearchListModel
// ----------------------------------------------------------------------------

SearchListModel::SearchListModel(wxWindow *parent) : wxDataViewVirtualListModel(0)
{
    int icon_id = 0;
    for (const std::string icon : {"cog", "printer", "printer", "spool", "blank_16"}) m_icon[icon_id++] = ScalableBitmap(parent, icon);
}

void SearchListModel::Clear()
{
    m_values.clear();
    Reset(0);
}

void SearchListModel::Prepend(const std::string &label)
{
    const char icon_c   = label.at(0);
    int        icon_idx = icon_idxs.at(icon_c);
    wxString   str      = from_u8(label).Remove(0, 1);

    m_values.emplace_back(str, icon_idx);

    RowPrepended();
}

void SearchListModel::msw_rescale()
{
    for (ScalableBitmap &bmp : m_icon) bmp.msw_rescale();
}

wxString SearchListModel::GetColumnType(unsigned int col) const
{
    if (col == colIcon) return "wxBitmap";
    return "string";
}

void SearchListModel::GetValueByRow(wxVariant &variant, unsigned int row, unsigned int col) const
{
    switch (col) {
    case colIcon: variant << m_icon[m_values[row].second].bmp(); break;
    case colMarkedText: variant = m_values[row].first; break;
    case colMax: wxFAIL_MSG("invalid column");
    default: break;
    }
}

SearchObjectDialog::SearchObjectDialog(GUI::ObjectList* object_list, wxWindow* parent, TextInput* input)
    : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS), m_object_list(object_list)
{
    search_line = input;

    Freeze();
    SetBackgroundColour(wxColour(238, 238, 238));

    em = GUI::wxGetApp().em_unit();

    m_bg_color = wxColour(255, 255, 255);
    m_thumb_color = wxColour(196, 196, 196);

    SetFont(GUI::wxGetApp().normal_font());
    SetSizeHints(wxDefaultSize, wxDefaultSize);

    m_sizer_border = new wxBoxSizer(wxVERTICAL);
    m_sizer_main = new wxBoxSizer(wxVERTICAL);
    m_sizer_body = new wxBoxSizer(wxVERTICAL);

    // border
    m_border_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(POPUP_WIDTH * em, POPUP_HEIGHT * em), wxTAB_TRAVERSAL);
    m_border_panel->SetBackgroundColour(m_bg_color);

    // client
    m_client_panel = new wxPanel(m_border_panel, wxID_ANY, wxDefaultPosition, wxSize(POPUP_WIDTH * em, POPUP_HEIGHT * em), wxTAB_TRAVERSAL);
    m_client_panel->SetBackgroundColour(m_bg_color);

    // search line
#ifdef __WXGTK__
    search_line = new TextInput(m_client_panel, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    search_line->SetBackgroundColour(wxColour(238, 238, 238));
    search_line->SetForegroundColour(wxColour(43, 52, 54));
    search_line->SetFont(GUI::wxGetApp().bold_font());
#endif

    search_line->Bind(wxEVT_TEXT, &SearchObjectDialog::OnInputText, this);
    search_line->Bind(wxEVT_LEFT_UP, &SearchObjectDialog::OnLeftUpInTextCtrl, this);
    search_line2 = search_line->GetTextCtrl();


    // scroll window
    m_scrolledWindow = new ScrolledWindow(m_client_panel, wxID_ANY, wxDefaultPosition, wxSize(POPUP_WIDTH * em - (em + em / 2), POPUP_HEIGHT * em), wxVSCROLL, 6, 6);
    m_scrolledWindow->SetMarginColor(m_bg_color);
    m_scrolledWindow->SetScrollbarColor(m_thumb_color);
    m_scrolledWindow->SetBackgroundColour(m_bg_color);
    auto m_listsizer = new wxBoxSizer(wxVERTICAL);
    auto m_listPanel = new wxWindow(m_scrolledWindow->GetPanel(), -1);
    m_listPanel->SetBackgroundColour(m_bg_color);
    m_listPanel->SetSize(wxSize(m_scrolledWindow->GetSize().GetWidth(), -1));

    m_listPanel->SetSizer(m_listsizer);
    m_listPanel->Fit();
    m_scrolledWindow->SetScrollbars(1, 1, 0, m_listPanel->GetSize().GetHeight());

#ifdef __WXGTK__
    m_sizer_body->Add(search_line, 0, wxEXPAND | wxALL, em / 2);
    search_line = input;
#endif
    m_sizer_body->Add(m_scrolledWindow, 0, wxEXPAND | wxALL, em);

    m_client_panel->SetSizer(m_sizer_body);
    m_client_panel->Layout();
    m_sizer_body->Fit(m_client_panel);
    m_sizer_main->Add(m_client_panel, 1, wxEXPAND, 0);

    m_border_panel->SetSizer(m_sizer_main);
    m_border_panel->Layout();
    m_sizer_border->Add(m_border_panel, 1, wxEXPAND | wxALL, 1);

    SetSizer(m_sizer_border);
    Layout();
    m_sizer_border->Fit(this);
    Thaw();

    GUI::wxGetApp().UpdateDarkUIWin(this);
}

SearchObjectDialog::~SearchObjectDialog() {}

void SearchObjectDialog::Popup(wxPoint position /*= wxDefaultPosition*/)
{
    if (m_is_dismissing || this->IsShown()) {
        return;
    }
    search_line2->SetValue(wxString(""));
#ifdef __WXOSX__
    // On macOS we need to remove the focus from the text input before popping up the
    // dropdown list, otherwise the text input won't be usable
    m_object_list->SetFocus();
#endif
    PopupWindow::Popup();
    search_line2->SetFocus();

    m_object_list->assembly_plate_object_name();
    m_object_list->GetModel()->search_object(wxEmptyString);
    update_list();
}

void SearchObjectDialog::MSWDismissUnfocusedPopup()
{
    Dismiss();
    OnDismiss();
}

void SearchObjectDialog::OnDismiss() {}

void SearchObjectDialog::Dismiss()
{
    auto pos = wxGetMousePosition();
    auto focus_window = wxWindow::FindFocus();
    if (!focus_window)
        Die();
    else if (!search_line->GetScreenRect().Contains(pos) && !this->GetScreenRect().Contains(pos)) {
        Die();
    }
}

void SearchObjectDialog::Die()
{
    m_is_dismissing = true;
    m_object_list->SetFocus();
    PopupWindow::Dismiss();
    wxCommandEvent event(wxCUSTOMEVT_EXIT_SEARCH);
    wxPostEvent(m_object_list, event);
    m_is_dismissing = false;
}

void SearchObjectDialog::OnInputText(wxCommandEvent&)
{
    search_line2->SetInsertionPointEnd();
    wxString input_string = search_line2->GetValue();
    if (input_string == wxEmptyString)
        input_string.Clear();

    m_object_list->assembly_plate_object_name();
    m_object_list->GetModel()->search_object(input_string);

    update_list();
}

void SearchObjectDialog::OnLeftUpInTextCtrl(wxEvent& event)
{
    if (search_line2->GetValue() == wxEmptyString)
        search_line2->SetValue("");
    event.Skip();
}

void SearchObjectDialog::update_list()
{
#ifndef __WXGTK__
    Freeze();
#endif
    m_scrolledWindow->Destroy();

    m_scrolledWindow = new ScrolledWindow(m_client_panel, wxID_ANY, wxDefaultPosition, wxSize(POPUP_WIDTH * em - (em + em / 2), POPUP_HEIGHT * em - em), wxVSCROLL, 6, 6);
    m_scrolledWindow->SetMarginColor(StateColor::darkModeColorFor(m_bg_color));
    m_scrolledWindow->SetScrollbarColor(StateColor::darkModeColorFor(m_thumb_color));
    m_scrolledWindow->SetBackgroundColour(StateColor::darkModeColorFor(m_bg_color));

    auto m_listsizer = new wxBoxSizer(wxVERTICAL);
    auto m_listPanel = new wxWindow(m_scrolledWindow->GetPanel(), -1);
    m_listPanel->SetBackgroundColour(StateColor::darkModeColorFor(m_bg_color));
    m_listPanel->SetSize(wxSize(m_scrolledWindow->GetSize().GetWidth(), -1));

    const std::vector<std::tuple<GUI::ObjectDataViewModelNode*, wxString, wxString>>& found = m_object_list->GetModel()->get_found_list();
    auto                            index = 0;
    for (const auto& [model_node, name, tip] : found) {
        auto     tmp = new SearchItem(m_listPanel, name, index, nullptr, this, tip);
        tmp->m_item = model_node;
        m_listsizer->Add(tmp, 0, wxEXPAND, 0);
        index++;
    }

    m_listPanel->SetSizer(m_listsizer);
    m_listPanel->Fit();
    m_scrolledWindow->SetScrollbars(1, 1, 0, m_listPanel->GetSize().GetHeight());

    m_sizer_body->Add(m_scrolledWindow, 0, wxEXPAND | wxALL, em);
    m_sizer_body->Fit(m_client_panel);
    m_sizer_body->Layout();
#ifndef __WXGTK__
    Thaw();
#endif
}

} // namespace Search

} // namespace Slic3r
