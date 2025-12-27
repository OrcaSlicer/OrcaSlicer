#ifndef slic3r_SearchComboBox_hpp_
#define slic3r_SearchComboBox_hpp_

#include <vector>
#include <map>

#include <boost/nowide/convert.hpp>

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/listctrl.h>

#include <wx/combo.h>

#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/srchctrl.h>

#include "wxExtensions.hpp"
#include "GUI_Utils.hpp"
#include "libslic3r/Preset.hpp"
#include "Widgets/ScrolledWindow.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/PopupWindow.hpp"
#include "GUI_ObjectList.hpp"

namespace Slic3r {

wxDECLARE_EVENT(wxCUSTOMEVT_JUMP_TO_OPTION, wxCommandEvent);
wxDECLARE_EVENT(wxCUSTOMEVT_EXIT_SEARCH, wxCommandEvent);
wxDECLARE_EVENT(wxCUSTOMEVT_JUMP_TO_OBJECT, wxCommandEvent);

namespace Search {

class SearchDialog;

struct InputInfo
{
    DynamicPrintConfig *config{nullptr};
    Preset::Type        type{Preset::TYPE_INVALID};
    ConfigOptionMode    mode{comSimple};
};

struct GroupAndCategory
{
    wxString group;
    wxString category;
};

struct Option
{
    //    bool operator<(const Option& other) const { return other.label > this->label; }
    bool operator<(const Option &other) const { return other.key > this->key; }

    // Fuzzy matching works at a character level. Thus matching with wide characters is a safer bet than with short characters,
    // though for some languages (Chinese?) it may not work correctly.
    std::wstring key;
    Preset::Type type{Preset::TYPE_INVALID};
    std::wstring label;
    std::wstring label_local;
    std::wstring group;
    std::wstring group_local;
    std::wstring category;
    std::wstring category_local;
    bool multi_category { false };

    std::string opt_key() const;
};

struct FoundOption
{
    // UTF8 encoding, to be consumed by ImGUI by reference.
    std::string label;
    std::string marked_label;
    std::string tooltip;
    size_t      option_idx{0};
    int         outScore{0};

    // Returning pointers to contents of std::string members, to be used by ImGUI for rendering.
    void get_marked_label_and_tooltip(const char **label, const char **tooltip) const;
};

struct OptionViewParameters
{
    bool category{false};
    bool english{false};

    int hovered_id{0};
};

class OptionsSearcher
{
    std::string                             search_line;
    Preset::Type                            search_type = Preset::TYPE_INVALID;

    std::map<std::string, GroupAndCategory> groups_and_categories;
    PrinterTechnology                       printer_technology;

    std::vector<Option>      options{};
    std::vector<FoundOption> found{};

    void append_options(DynamicPrintConfig *config, Preset::Type type, ConfigOptionMode mode);

    void sort_options();
    void sort_found()
    {
        std::sort(found.begin(), found.end(),
                  [](const FoundOption &f1, const FoundOption &f2) { return f1.outScore > f2.outScore || (f1.outScore == f2.outScore && f1.label < f2.label); });
    };

    size_t options_size() const { return options.size(); }
    size_t found_size() const { return found.size(); }

public:
    OptionViewParameters view_params;

    SearchDialog *search_dialog{nullptr};

    OptionsSearcher();
    ~OptionsSearcher();

    void init(std::vector<InputInfo> input_values);
    void apply(DynamicPrintConfig *config, Preset::Type type, ConfigOptionMode mode);
    bool search();
    bool search(const std::string &search, bool force = false, Preset::Type type = Preset::TYPE_INVALID);

    void add_key(const std::string &opt_key, Preset::Type type, const wxString &group, const wxString &category);

    size_t size() const { return found_size(); }

    const FoundOption &operator[](const size_t pos) const noexcept { return found[pos]; }
    const Option &     get_option(size_t pos_in_filter) const;
    const Option &     get_option(const std::string &opt_key, Preset::Type type) const;
    Option             get_option(const std::string &opt_key, const wxString &label, Preset::Type type) const;

    const std::vector<FoundOption> &found_options() { return found; }
    const GroupAndCategory &        get_group_and_category(const std::string &opt_key) { return groups_and_categories[opt_key]; }
    std::string &                   search_string() { return search_line; }

    void set_printer_technology(PrinterTechnology pt) { printer_technology = pt; }

    void sort_options_by_key()
    {
        std::sort(options.begin(), options.end(), [](const Option &o1, const Option &o2) { return o1.key < o2.key; });
    }
    void sort_options_by_label() { sort_options(); }

    void show_dialog(Preset::Type type, wxWindow *parent, TextInput *input, wxWindow *ssearch_btn);
    void dlg_sys_color_changed();
    void dlg_msw_rescale();
};

//------------------------------------------
//          SearchDialog
//------------------------------------------
class SearchDialog;
class SearchObjectDialog;
class SearchItem : public wxWindow
{
public:
    wxString      m_text;
    int           m_index;
    SearchDialog* m_sdialog{ nullptr };
    SearchObjectDialog* m_search_object_dialog{ nullptr };
    GUI::ObjectDataViewModelNode* m_item{ nullptr };

    SearchItem(wxWindow *parent, wxString text, int index, SearchDialog *sdialog = nullptr, SearchObjectDialog* search_dialog = nullptr, wxString tooltip = "");
    ~SearchItem(){};

    wxSize DrawTextString(wxDC &dc, const wxString &text, const wxPoint &pt, bool bold);
    void   OnPaint(wxPaintEvent &event);
    void   on_mouse_enter(wxMouseEvent &evt);
    void   on_mouse_leave(wxMouseEvent &evt);
    void   on_mouse_left_down(wxMouseEvent &evt);
    void   on_mouse_left_up(wxMouseEvent &evt);
};

//------------------------------------------
//          SearchDialog
//------------------------------------------
class SearchListModel;
class SearchDialog : public PopupWindow
{
public:
    wxColour m_bg_colour;
    wxColour m_thumb_color;

    wxBoxSizer *m_sizer_body{nullptr};
    wxBoxSizer *m_sizer_main{nullptr};
    wxBoxSizer *m_sizer_border{nullptr};

    wxWindow *m_border_panel{nullptr};
    wxWindow *m_client_panel{nullptr};

    wxWindow *m_event_tag{nullptr};
    wxWindow *m_search_item_tag{nullptr};

    int       em;
    const int POPUP_WIDTH  = 38;
    const int POPUP_HEIGHT = 40;

    TextInput *  search_line{nullptr};
    wxTextCtrl *  search_line2{nullptr};
    Preset::Type     search_type = Preset::TYPE_INVALID;

    ScrolledWindow * m_scrolledWindow{nullptr};

    OptionsSearcher *searcher{nullptr};

    void OnInputText(wxCommandEvent &event);
    void OnLeftUpInTextCtrl(wxEvent &event);

    void update_list();

public:
    SearchDialog(OptionsSearcher *searcher, Preset::Type type, wxWindow *parent, TextInput *input, wxWindow *search_btn);
    ~SearchDialog();

    void MSWDismissUnfocusedPopup();
    void Popup(wxPoint position = wxDefaultPosition);
    void OnDismiss();
    void Dismiss();
    void Die();
    void msw_rescale();

};

// ----------------------------------------------------------------------------
// SearchListModel
// ----------------------------------------------------------------------------

class SearchListModel : public wxDataViewVirtualListModel
{
    std::vector<std::pair<wxString, int>> m_values;
    ScalableBitmap                        m_icon[5];

public:
    enum { colIcon, colMarkedText, colMax };

    SearchListModel(wxWindow *parent);

    // helper methods to change the model

    void Clear();
    void Prepend(const std::string &text);
    void msw_rescale();

    // implementation of base class virtuals to define model

    unsigned int GetColumnCount() const override { return colMax; }
    wxString     GetColumnType(unsigned int col) const override;
    void         GetValueByRow(wxVariant &variant, unsigned int row, unsigned int col) const override;
    bool         GetAttrByRow(unsigned int row, unsigned int col, wxDataViewItemAttr &attr) const override { return true; }
    bool         SetValueByRow(const wxVariant &variant, unsigned int row, unsigned int col) override { return false; }
};

class SearchObjectDialog : public PopupWindow
{
public:
    SearchObjectDialog(GUI::ObjectList* object_list, wxWindow* parent, TextInput* input);
    ~SearchObjectDialog();

    void MSWDismissUnfocusedPopup();
    void Popup(wxPoint position = wxDefaultPosition);
    void OnDismiss();
    void Dismiss();
    void Die();

    void OnInputText(wxCommandEvent& event);
    void OnLeftUpInTextCtrl(wxEvent& event);

    void update_list();

public:
    GUI::ObjectList* m_object_list{ nullptr };

    int       em;
    const int POPUP_WIDTH = 41;
    const int POPUP_HEIGHT = 45;

    TextInput*  search_line{nullptr};
    wxTextCtrl* search_line2{nullptr};

    ScrolledWindow* m_scrolledWindow{ nullptr };

    wxColour m_bg_color;
    wxColour m_thumb_color;

    wxBoxSizer* m_sizer_body{ nullptr };
    wxBoxSizer* m_sizer_main{ nullptr };
    wxBoxSizer* m_sizer_border{ nullptr };

    wxWindow* m_border_panel{ nullptr };
    wxWindow* m_client_panel{ nullptr };

private:
    bool m_is_dismissing{ false };
};

} // namespace Search
} // namespace Slic3r

#endif // slic3r_SearchComboBox_hpp_
