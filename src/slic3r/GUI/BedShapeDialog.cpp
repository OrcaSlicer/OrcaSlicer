#include "BedShapeDialog.hpp"
#include "GUI_App.hpp"
#include "OptionsGroup.hpp"

#include <wx/wx.h>
#include <wx/numformatter.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/tooltip.h>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/ClipperUtils.hpp"

#include "Widgets/LabeledStaticBox.hpp"
#include "Widgets/DialogButtons.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <algorithm>

namespace Slic3r {
namespace GUI {

BedShape::BedShape(const Pointfs& points)
{
    m_build_volume = { points, 0.f, {}, {} };
}

static std::string get_option_label(BedShape::Parameter param)
{
    switch (param) {
    case BedShape::Parameter::RectSize  : return L("Size");
    case BedShape::Parameter::RectOrigin: return L("Origin");
    case BedShape::Parameter::Diameter  : return L("Diameter");
    default:                              assert(false); return {};
    }
}

void BedShape::append_option_line(ConfigOptionsGroupShp optgroup, Parameter param)
{
    ConfigOptionDef def;
    t_config_option_key key;
    switch (param) {
    case Parameter::RectSize:
        def.type = coPoints;
        def.set_default_value(new ConfigOptionPoints{ Vec2d(200, 200) });
        def.min = 0;
        def.max = 214700;
        def.width = 10; // increase width for large scale printers with 4 digit values
        def.sidetext = "mm";	// milimeters, don't need translation
        def.label = get_option_label(param);
        def.tooltip = L("Size in X and Y of the rectangular plate.");
        key = "rect_size";
        break;
    case Parameter::RectOrigin:
        def.type = coPoints;
        def.set_default_value(new ConfigOptionPoints{ Vec2d(0, 0) });
        def.min = -107350;
        def.max = 107350;
        def.width = 10; // increase width for large scale printers with 4 digit values
        def.sidetext = "mm";	// milimeters, don't need translation
        def.label = get_option_label(param);
        def.tooltip = L("Distance of the 0,0 G-code coordinate from the front left corner of the rectangle.");
        key = "rect_origin";
        break;
    case Parameter::Diameter:
        def.type = coFloat;
        def.set_default_value(new ConfigOptionFloat(200));
        def.width = 10; // match size
        def.sidetext = "mm";	// milimeters, don't need translation
        def.label = get_option_label(param);
        def.tooltip = L("Diameter of the print bed. It is assumed that origin (0,0) is located in the center.");
        key = "diameter";
        break;
    default:
        assert(false);
    }

    optgroup->append_single_option_line({ def, std::move(key) });
}

wxString BedShape::get_name(PageType type)
{
    switch (type) {
    case PageType::Rectangle:  return _L("Rectangular");
    case PageType::Circle:     return _L("Circular");
    case PageType::Custom:     return _L("Custom");
    }
    // make visual studio happy
    assert(false);
    return {};
}

BedShape::PageType BedShape::get_page_type()
{
    switch (m_build_volume.type()) {
    case BuildVolume_Type::Rectangle:
    case BuildVolume_Type::Invalid:    return PageType::Rectangle;
    case BuildVolume_Type::Circle:     return PageType::Circle;
    case BuildVolume_Type::Convex:
    case BuildVolume_Type::Custom:     return PageType::Custom;
    }
    // make visual studio happy
    assert(false);
    return PageType::Rectangle;
}

wxString BedShape::get_full_name_with_params()
{
    wxString out = _L("Shape") + ": " + get_name(this->get_page_type());
    switch (m_build_volume.type()) {
    case BuildVolume_Type::Circle:
        out += "\n" + _L(get_option_label(Parameter::Diameter)) + ": [" + double_to_string(2. * unscaled<double>(m_build_volume.circle().radius)) + "]";
        break;
    default:
        // rectangle, convex, concave...
        out += "\n" + _(get_option_label(Parameter::RectSize)) + ": [" + ConfigOptionPoint(to_2d(m_build_volume.bounding_volume().size())).serialize() + "]";
        out += "\n" + _(get_option_label(Parameter::RectOrigin)) + ": [" + ConfigOptionPoint(- to_2d(m_build_volume.bounding_volume().min)).serialize() + "]";
        break;
    }
    return out;
}

void BedShape::apply_optgroup_values(ConfigOptionsGroupShp optgroup)
{
    switch (m_build_volume.type()) {
    case BuildVolume_Type::Circle:
        optgroup->set_value("diameter", double_to_string(2. * unscaled<double>(m_build_volume.circle().radius)));
        break;
    default:
        // rectangle, convex, concave...
        optgroup->set_value("rect_size"     , new ConfigOptionPoints{ to_2d(m_build_volume.bounding_volume().size()) });
        optgroup->set_value("rect_origin"   , new ConfigOptionPoints{ - to_2d(m_build_volume.bounding_volume().min) });
    }
}

void BedShapeDialog::build_dialog(const Pointfs& default_pt, const ConfigOptionString& custom_texture, const ConfigOptionString& custom_model)
{
    SetFont(wxGetApp().normal_font());

    SetBackgroundColour(*wxWHITE);
	m_panel = new BedShapePanel(this);
    m_panel->build_panel(default_pt, custom_texture, custom_model);

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(m_panel, 1, wxEXPAND);

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    main_sizer->Add(dlg_btns, 0, wxEXPAND);

    wxGetApp().UpdateDlgDarkUI(this);

	SetSizer(main_sizer);
	SetMinSize(GetSize());
	main_sizer->SetSizeHints(this);

    this->Bind(wxEVT_CLOSE_WINDOW, ([this](wxCloseEvent& evt) {
        EndModal(wxID_CANCEL);
    }));
}

void BedShapeDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    const int& em = em_unit();
    m_panel->m_shape_options_book->SetMinSize(wxSize(32 * em, -1));
    m_panel->m_shape_combo->Rescale();
    m_panel->m_shape_combo->SetMinSize({-1, 3 * em}); // Fix combo size on scale

    for (auto og : m_panel->m_optgroups)
        og->msw_rescale();

    const wxSize& size = wxSize(64 * em, -1);

    SetMinSize(size);
    SetSize(size);

    Refresh();
}

const std::string BedShapePanel::NONE = "None";
const std::string BedShapePanel::EMPTY_STRING = "";

void BedShapePanel::build_panel(const Pointfs& default_pt, const std::string& custom_texture, const std::string& custom_model)
{
    wxGetApp().UpdateDarkUI(this);
    m_shape = make_counter_clockwise(default_pt);
    m_custom_texture = custom_texture.empty() ? NONE : custom_texture;
    m_custom_model = custom_model.empty() ? NONE : custom_model;

    // ORCA match style of wxStaticBox between platforms
    LabeledStaticBox* stb = new LabeledStaticBox(this, _L("Shape"));
    auto sbsizer = new wxStaticBoxSizer(stb, wxVERTICAL);

	// shape options 
    m_shape_options_book = new wxSimplebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    // ORCA replace wxChoicebook wxSimplebook and add a combo box for control
    m_shape_combo = new ComboBox(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
    m_shape_combo->Append(BedShape::get_name(BedShape::PageType::Rectangle));
    m_shape_combo->Append(BedShape::get_name(BedShape::PageType::Circle));
    m_shape_combo->Append(BedShape::get_name(BedShape::PageType::Custom));
    m_shape_combo->SetSelection(int(BedShape(default_pt).get_page_type()));
    m_shape_combo->GetDropDown().Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
        if(m_shape_options_book->GetSelection() != evt.GetSelection()){
            m_shape_options_book->ChangeSelection(evt.GetSelection());
            update_shape();
        }
        evt.Skip();
    });

    sbsizer->Add(m_shape_combo, 0, wxALL | wxEXPAND, FromDIP(3));

    sbsizer->Add(m_shape_options_book, 1, wxEXPAND);

    auto optgroup = init_shape_options_page(BedShape::get_name(BedShape::PageType::Rectangle));
    BedShape::append_option_line(optgroup, BedShape::Parameter::RectSize);
    BedShape::append_option_line(optgroup, BedShape::Parameter::RectOrigin);
    activate_options_page(optgroup);

     optgroup = init_shape_options_page(BedShape::get_name(BedShape::PageType::Circle));
     BedShape::append_option_line(optgroup, BedShape::Parameter::Diameter);
     activate_options_page(optgroup);

    optgroup = init_shape_options_page(BedShape::get_name(BedShape::PageType::Custom));

	Line line{ "", "" };
	line.full_width = 1;
	line.widget = [this](wxWindow* parent) {
        Button* shape_btn = new Button(parent, _L("Load shape from STL ..."));
        shape_btn->SetStyle(ButtonStyle::Regular, ButtonType::Expanded);

        wxSizer* shape_sizer = new wxBoxSizer(wxHORIZONTAL);
        shape_sizer->Add(shape_btn, 1, wxEXPAND);

        wxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(shape_sizer, 1, wxEXPAND);

        shape_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
			load_stl();
		});

		return sizer;
	};
	optgroup->append_line(line);
    activate_options_page(optgroup);

    wxPanel* texture_panel = init_texture_panel();
    wxPanel* model_panel = init_model_panel();

    Bind(wxEVT_BOOKCTRL_PAGE_CHANGED, ([this](wxCommandEvent& e) { update_shape(); }));

	// right pane with preview canvas
	m_canvas = new Bed_2D(this);
    m_canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent& e) { m_canvas->repaint(m_shape); });
    m_canvas->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { m_canvas->Refresh(); });

    wxSizer* left_sizer = new wxBoxSizer(wxVERTICAL);
    left_sizer->Add(sbsizer, 1, wxEXPAND);
    left_sizer->Add(texture_panel, 1, wxEXPAND);
    left_sizer->Add(model_panel, 1, wxEXPAND);

    wxSizer* top_sizer = new wxBoxSizer(wxHORIZONTAL);
    top_sizer->Add(left_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, 10);
    top_sizer->Add(m_canvas, 1, wxEXPAND | wxALL, 10);

	SetSizerAndFit(top_sizer);

	set_shape(m_shape);
	update_preview();
}

// Called from the constructor.
// Create a panel for a rectangular / circular / custom bed shape.
ConfigOptionsGroupShp BedShapePanel::init_shape_options_page(const wxString& title)
{
    wxPanel* panel = new wxPanel(m_shape_options_book);
    panel->SetBackgroundColour(*wxWHITE);
    ConfigOptionsGroupShp optgroup = std::make_shared<ConfigOptionsGroup>(panel, _L("Settings"));

    optgroup->label_width = 10;
    optgroup->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
        update_shape();
    };

    m_optgroups.push_back(optgroup);
//    panel->SetSizerAndFit(optgroup->sizer);
    m_shape_options_book->AddPage(panel, title);

    return optgroup;
}

void BedShapePanel::activate_options_page(ConfigOptionsGroupShp options_group)
{
    options_group->activate();
    options_group->parent()->SetSizerAndFit(options_group->sizer);
}

wxPanel* BedShapePanel::init_texture_panel()
{
    wxPanel* panel = new wxPanel(this);
    panel->SetBackgroundColour(*wxWHITE);
    ConfigOptionsGroupShp optgroup = std::make_shared<ConfigOptionsGroup>(panel, _L("Texture"));

    optgroup->label_width = 10;
    optgroup->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
        update_shape();
    };

    Line line{ "", "" };
    line.full_width = 1;
    line.widget = [this](wxWindow* parent) {
        Button* load_btn = new Button(parent, _L("Load..."));
        load_btn->SetStyle(ButtonStyle::Regular, ButtonType::Expanded);

        wxSizer* load_sizer = new wxBoxSizer(wxHORIZONTAL);
        load_sizer->Add(load_btn, 1, wxEXPAND);

        wxStaticText* filename_lbl = new wxStaticText(parent, wxID_ANY, _(NONE));
        wxSizer* filename_sizer = new wxBoxSizer(wxHORIZONTAL);
        filename_sizer->Add(filename_lbl, 1, wxEXPAND);

        Button* remove_btn = new Button(parent, _L("Remove"));
        remove_btn->SetStyle(ButtonStyle::Regular, ButtonType::Expanded);

        wxSizer* remove_sizer = new wxBoxSizer(wxHORIZONTAL);
        remove_sizer->Add(remove_btn, 1, wxEXPAND);

        wxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(filename_sizer, 1, wxEXPAND);
        sizer->Add(load_sizer, 1, wxEXPAND);
        sizer->Add(remove_sizer, 1, wxEXPAND | wxTOP, 2);

        load_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent& e) { load_texture(); }));
        remove_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent& e) {
                m_custom_texture = NONE;
                update_shape();
            }));

        filename_lbl->Bind(wxEVT_UPDATE_UI, ([this](wxUpdateUIEvent& e) {
                e.SetText(_(boost::filesystem::path(m_custom_texture).filename().string()));
                wxStaticText* lbl = dynamic_cast<wxStaticText*>(e.GetEventObject());
                if (lbl != nullptr) {
                    bool exists = (m_custom_texture == NONE) || boost::filesystem::exists(m_custom_texture);
                    lbl->SetForegroundColour(exists ? wxGetApp().get_label_clr_default() : wxColour("#E14747")); // ORCA

                    wxString tooltip_text = "";
                    if (m_custom_texture != NONE) {
                        if (!exists)
                            tooltip_text += _L("Not found:") + " ";

                        tooltip_text += _(m_custom_texture);
                    }

                    wxToolTip* tooltip = lbl->GetToolTip();
                    if ((tooltip == nullptr) || (tooltip->GetTip() != tooltip_text))
                        lbl->SetToolTip(tooltip_text);
                }
            }));

        remove_btn->Bind(wxEVT_UPDATE_UI, ([this](wxUpdateUIEvent& e) { e.Enable(m_custom_texture != NONE); }));

        parent->SetBackgroundColour(*wxWHITE);

        return sizer;
    };
    optgroup->append_line(line);
    optgroup->activate();

    panel->SetSizerAndFit(optgroup->sizer);

    return panel;
}

wxPanel* BedShapePanel::init_model_panel()
{
    wxPanel* panel = new wxPanel(this);
    panel->SetBackgroundColour(*wxWHITE);
    ConfigOptionsGroupShp optgroup = std::make_shared<ConfigOptionsGroup>(panel, _L("Model"));

    optgroup->label_width = 10;
    optgroup->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
        update_shape();
    };


    Line line{ "", "" };
    line.full_width = 1;
    line.widget = [this](wxWindow* parent) {
        Button* load_btn = new Button(parent, _L("Load..."));
        load_btn->SetStyle(ButtonStyle::Regular, ButtonType::Expanded);

        wxSizer* load_sizer = new wxBoxSizer(wxHORIZONTAL);
        load_sizer->Add(load_btn, 1, wxEXPAND);

        wxStaticText* filename_lbl = new wxStaticText(parent, wxID_ANY, _(NONE));
        wxSizer* filename_sizer = new wxBoxSizer(wxHORIZONTAL);
        filename_sizer->Add(filename_lbl, 1, wxEXPAND);

        Button* remove_btn = new Button(parent, _L("Remove"));
        remove_btn->SetStyle(ButtonStyle::Regular, ButtonType::Expanded);

        wxSizer* remove_sizer = new wxBoxSizer(wxHORIZONTAL);
        remove_sizer->Add(remove_btn, 1, wxEXPAND);

        wxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(filename_sizer, 1, wxEXPAND);
        sizer->Add(load_sizer, 1, wxEXPAND);
        sizer->Add(remove_sizer, 1, wxEXPAND | wxTOP, 2);

        load_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent& e) { load_model(); }));

        remove_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent& e) {
                m_custom_model = NONE;
                update_shape();
            }));

        filename_lbl->Bind(wxEVT_UPDATE_UI, ([this](wxUpdateUIEvent& e) {
                e.SetText(_(boost::filesystem::path(m_custom_model).filename().string()));
                wxStaticText* lbl = dynamic_cast<wxStaticText*>(e.GetEventObject());
                if (lbl != nullptr) {
                    bool exists = (m_custom_model == NONE) || boost::filesystem::exists(m_custom_model);
                    lbl->SetForegroundColour(exists ? wxGetApp().get_label_clr_default() : wxColour("#E14747")); // ORCA

                    wxString tooltip_text = "";
                    if (m_custom_model != NONE) {
                        if (!exists)
                            tooltip_text += _L("Not found:") + " ";

                        tooltip_text += _(m_custom_model);
                    }

                    wxToolTip* tooltip = lbl->GetToolTip();
                    if ((tooltip == nullptr) || (tooltip->GetTip() != tooltip_text))
                        lbl->SetToolTip(tooltip_text);
                }
            }));

        remove_btn->Bind(wxEVT_UPDATE_UI, ([this](wxUpdateUIEvent& e) { e.Enable(m_custom_model != NONE); }));

        parent->SetBackgroundColour(*wxWHITE);

        return sizer;
    };
    optgroup->append_line(line);
    optgroup->activate();

    panel->SetSizerAndFit(optgroup->sizer);

    return panel;
}

// Called from the constructor.
// Set the initial bed shape from a list of points.
// Deduce the bed shape type(rect, circle, custom)
// This routine shall be smart enough if the user messes up
// with the list of points in the ini file directly.
void BedShapePanel::set_shape(const Pointfs& points)
{
    BedShape shape(points);

    // Copy the polygon to the canvas, make a copy of the array, if custom shape is selected
    if (shape.is_custom())
        m_loaded_shape = points;

    m_shape_options_book->SetSelection(int(shape.get_page_type()));
    shape.apply_optgroup_values(m_optgroups[int(shape.get_page_type())]);

    update_shape();

    return;
}

void BedShapePanel::update_preview()
{
	if (m_canvas) m_canvas->Refresh();
	Refresh();
}

// Update the bed shape from the dialog fields.
void BedShapePanel::update_shape()
{
	auto page_idx = m_shape_options_book->GetSelection();
    auto opt_group = m_optgroups[page_idx];

    switch (static_cast<BedShape::PageType>(page_idx)) {
    case BedShape::PageType::Rectangle:
    {
        Vec2d rect_size(Vec2d::Zero());
		Vec2d rect_origin(Vec2d::Zero());

		try { rect_size = boost::any_cast<Vec2d>(opt_group->get_value("rect_size")); }
        catch (const std::exception& /* e */) { return; }

        try { rect_origin = boost::any_cast<Vec2d>(opt_group->get_value("rect_origin")); }
		catch (const std::exception & /* e */)  { return; }
 		
		auto x = rect_size(0);
		auto y = rect_size(1);
		// empty strings or '-' or other things
		if (x == 0 || y == 0)	return;
		double x0 = 0.0;
		double y0 = 0.0;
		double x1 = x;
		double y1 = y;

		auto dx = rect_origin(0);
		auto dy = rect_origin(1);

		x0 -= dx;
		x1 -= dx;
		y0 -= dy;
		y1 -= dy;
        m_shape = { Vec2d(x0, y0),
                    Vec2d(x1, y0),
                    Vec2d(x1, y1),
                    Vec2d(x0, y1) };
        break;
    }
    case BedShape::PageType::Circle:
    {
        double diameter;
		try { diameter = boost::any_cast<double>(opt_group->get_value("diameter")); }
		catch (const std::exception & /* e */) { return; }

 		if (diameter == 0.0) return ;
		auto r = diameter / 2;
		auto twopi = 2 * PI;
        // Don't change this value without adjusting BuildVolume constructor detecting circle diameter!
        auto edges = 72;
        std::vector<Vec2d> points;
        for (int i = 1; i <= edges; ++i) {
            auto angle = i * twopi / edges;
			points.push_back(Vec2d(r*cos(angle), r*sin(angle)));
		}
        m_shape = points;
        break;
    }
    case BedShape::PageType::Custom:
        m_shape = m_loaded_shape;
        break;
    }

    update_preview();
}

// Loads an stl file, projects it to the XY plane and calculates a polygon.
void BedShapePanel::load_stl()
{
    wxFileDialog dialog(this, _L("Choose an STL file to import bed shape from:"), "", "", file_wildcards(FT_STL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    std::string file_name = dialog.GetPath().ToUTF8().data();
    if (!boost::algorithm::iends_with(file_name, ".stl")) {
        show_error(this, _L("Invalid file format."));
        return;
    }

    wxBusyCursor wait;

	Model model;
	try {
        model = Model::read_from_file(file_name);
	}
	catch (std::exception &) {
        show_error(this, _L("Error! Invalid model"));
        return;
    }

	auto mesh = model.mesh();
	auto expolygons = mesh.horizontal_projection();

	if (expolygons.size() == 0) {
		show_error(this, _L("The selected file contains no geometry."));
		return;
	}
	if (expolygons.size() > 1) {
		show_error(this, _L("The selected file contains several disjoint areas. This is not supported."));
		return;
	}

	auto polygon = expolygons[0].contour;
    polygon.make_counter_clockwise();
	std::vector<Vec2d> points;
	for (auto pt : polygon.points)
		points.push_back(unscale(pt));

    m_loaded_shape = points;
    update_shape();
}

void BedShapePanel::load_texture()
{
    wxFileDialog dialog(this, _L("Choose a file to import bed texture from (PNG/SVG):"), "", "",
        file_wildcards(FT_TEX), wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dialog.ShowModal() != wxID_OK)
        return;

    m_custom_texture = NONE;

    std::string file_name = dialog.GetPath().ToUTF8().data();
    if (!boost::algorithm::iends_with(file_name, ".png") && !boost::algorithm::iends_with(file_name, ".svg")) {
        show_error(this, _L("Invalid file format."));
        return;
    }

    wxBusyCursor wait;

    m_custom_texture = file_name;
    update_shape();
}

void BedShapePanel::load_model()
{
    wxFileDialog dialog(this, _L("Choose an STL file to import bed model from:"), "", "",
        file_wildcards(FT_STL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dialog.ShowModal() != wxID_OK)
        return;

    m_custom_model = NONE;

    std::string file_name = dialog.GetPath().ToUTF8().data();
    if (!boost::algorithm::iends_with(file_name, ".stl")) {
        show_error(this, _L("Invalid file format."));
        return;
    }

    wxBusyCursor wait;

    m_custom_model = file_name;
    update_shape();
}

} // GUI
} // Slic3r
