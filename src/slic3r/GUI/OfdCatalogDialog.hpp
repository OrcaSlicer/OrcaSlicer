#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/MixedFilamentOfd.hpp"

#include <memory>
#include <string>
#include <vector>

class wxButton;
class wxChoice;
class wxComboBox;
class wxScrolledWindow;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

class OfdCatalogDialog : public DPIDialog
{
public:
    OfdCatalogDialog(wxWindow *parent, int filament_idx);
    ~OfdCatalogDialog() override;

    const SpectrumOfdVariant &selected() const { return m_selected; }
    int filament_idx() const { return m_filament_idx; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void load_catalog();
    void rebuild_brand_filter();
    void refresh_list();
    void select_row(int idx);
    void on_apply();
    void on_refresh();
    void update_title();
    std::string seed_path() const;
    std::string user_ndjson_path() const;

    int                                 m_filament_idx{0};
    int                                 m_sel{-1};
    SpectrumOfdVariant                  m_selected;
    std::vector<SpectrumOfdVariant>     m_catalog;
    std::vector<SpectrumOfdVariant>     m_shown;
    std::shared_ptr<bool>               m_refresh_alive;

    wxChoice *        m_slot{nullptr};
    wxComboBox *      m_brand{nullptr};
    wxTextCtrl *      m_name{nullptr};
    wxScrolledWindow *m_list{nullptr};
    wxStaticText *    m_status{nullptr};
    wxButton *        m_btn_refresh{nullptr};
    wxButton *        m_btn_apply{nullptr};
};

class OfdColorChooserDialog : public DPIDialog
{
public:
    enum { ID_CATALOG = wxID_HIGHEST + 61, ID_CUSTOM };

    explicit OfdColorChooserDialog(wxWindow *parent);
    ~OfdColorChooserDialog() override = default;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
};

} // namespace GUI
} // namespace Slic3r
