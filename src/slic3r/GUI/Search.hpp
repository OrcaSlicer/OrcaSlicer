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
#include "SettingsIndex.hpp"
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
    SettingsIndex m_index;

    std::string              search_line;
    Preset::Type             search_type = Preset::TYPE_INVALID;
    PrinterTechnology        printer_technology;
    std::vector<FoundOption> found{};

    void sort_found()
    {
        std::sort(found.begin(), found.end(),
                  [](const FoundOption &f1, const FoundOption &f2) { return f1.outScore > f2.outScore || (f1.outScore == f2.outScore && f1.label < f2.label); });
    };

    size_t found_size() const { return found.size(); }

public:
    OptionViewParameters view_params;

    SearchDialog *search_dialog{nullptr};

    OptionsSearcher();
    ~OptionsSearcher();

    SettingsIndex &      index() { return m_index; }
    const SettingsIndex &index() const { return m_index; }

    // Rebuild the catalog and re-run the current query so the cached results track it.
    void init(std::vector<InputInfo> input_values);
    void apply(DynamicPrintConfig *config, Preset::Type type, ConfigOptionMode mode);

    bool search();
    bool search(const std::string &search, bool force = false, Preset::Type type = Preset::TYPE_INVALID);

    size_t size() const { return found_size(); }

    const FoundOption &operator[](const size_t pos) const noexcept { return found[pos]; }
    const Option &     get_option(size_t pos_in_filter) const;

    const std::vector<FoundOption> &found_options() { return found; }
    std::string &                   search_string() { return search_line; }

    void set_printer_technology(PrinterTechnology pt) { printer_technology = pt; }

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

#ifdef __WXMSW__
    void MSWDismissUnfocusedPopup() override;
#endif // __WXMSW__
    void Popup(wxWindow *focus = nullptr) override;
    void OnDismiss() override;
    void Dismiss() override;
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

#ifdef __WXMSW__
    void MSWDismissUnfocusedPopup() override;
#endif // __WXMSW__
    void Popup(wxWindow *focus = nullptr) override;
    void OnDismiss() override;
    void Dismiss() override;
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
