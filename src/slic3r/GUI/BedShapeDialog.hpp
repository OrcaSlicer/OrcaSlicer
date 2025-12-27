#ifndef slic3r_BedShapeDialog_hpp_
#define slic3r_BedShapeDialog_hpp_
// The bed shape dialog.
// The dialog opens from Print Settins tab->Bed Shape : Set...

#include "GUI_Utils.hpp"
#include "2DBed.hpp"
#include "I18N.hpp"

#include <libslic3r/BuildVolume.hpp>

#include "Widgets/ComboBox.hpp"

#include <wx/dialog.h>
#include <wx/simplebook.h> // ORCA

namespace Slic3r {
namespace GUI {

class ConfigOptionsGroup;

using ConfigOptionsGroupShp = std::shared_ptr<ConfigOptionsGroup>;
using ConfigOptionsGroupWkp = std::weak_ptr<ConfigOptionsGroup>;

struct BedShape
{
    enum class PageType {
        Rectangle,
        Circle,
        Custom
    };

    enum class Parameter {
        RectSize,
        RectOrigin,
        Diameter
    };

    BedShape(const Pointfs& points);

    bool            is_custom() { return m_build_volume.type() == BuildVolume_Type::Convex || m_build_volume.type() == BuildVolume_Type::Custom; }

    static void     append_option_line(ConfigOptionsGroupShp optgroup, Parameter param);
    static wxString get_name(PageType type);

    PageType        get_page_type();

    wxString        get_full_name_with_params();
    void            apply_optgroup_values(ConfigOptionsGroupShp optgroup);

private:
    BuildVolume m_build_volume;
};

class BedShapePanel : public wxPanel
{
    static const std::string NONE;
    static const std::string EMPTY_STRING;

	Bed_2D*			   m_canvas;
    Pointfs            m_shape;
    Pointfs            m_loaded_shape;
    std::string        m_custom_texture;
    std::string        m_custom_model;

public:
    BedShapePanel(wxWindow* parent) : wxPanel(parent, wxID_ANY), m_custom_texture(NONE), m_custom_model(NONE) {}

    void build_panel(const Pointfs& default_pt, const std::string& custom_texture, const std::string& custom_model);

    // Returns the resulting bed shape polygon. This value will be stored to the ini file.
    const Pointfs&     get_shape() const { return m_shape; }
    const std::string& get_custom_texture() const { return (m_custom_texture != NONE) ? m_custom_texture : EMPTY_STRING; }
    const std::string& get_custom_model() const { return (m_custom_model != NONE) ? m_custom_model : EMPTY_STRING; }

private:
    ConfigOptionsGroupShp	init_shape_options_page(const wxString& title);
    void	    activate_options_page(ConfigOptionsGroupShp options_group);
    wxPanel*    init_texture_panel();
    wxPanel*    init_model_panel();
    void		set_shape(const Pointfs& points);
    void		update_preview();
	void		update_shape();
	void		load_stl();
    void		load_texture();
    void		load_model();

    wxSimplebook* m_shape_options_book;
    ComboBox*     m_shape_combo;
	std::vector <ConfigOptionsGroupShp>	m_optgroups;

    friend class BedShapeDialog;
};

class BedShapeDialog : public DPIDialog
{
	BedShapePanel*	m_panel;
public:
	BedShapeDialog(wxWindow* parent) : DPIDialog(parent, wxID_ANY, _(L("Bed Shape")),
        wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE) {}

    void build_dialog(const Pointfs& default_pt, const ConfigOptionString& custom_texture, const ConfigOptionString& custom_model);

    const Pointfs&     get_shape() const { return m_panel->get_shape(); }
    const std::string& get_custom_texture() const { return m_panel->get_custom_texture(); }
    const std::string& get_custom_model() const { return m_panel->get_custom_model(); }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
};

} // GUI
} // Slic3r


#endif  /* slic3r_BedShapeDialog_hpp_ */
