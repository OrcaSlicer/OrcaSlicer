#include <cstddef>
#include <algorithm>
#include <numeric>
#include <vector>
#include <string>
#include <regex>
#include <future>
#include <GL/glew.h>
#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Utils.hpp"

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "Widgets/Label.hpp"
#include "2DBed.hpp"
#include "3DBed.hpp"
#include "PartPlate.hpp"
#include "Camera.hpp"
#include "GUI_Colors.hpp"
#include "GUI_ObjectList.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/Utils/FileHelp.hpp"
#include <imgui/imgui_internal.h>
#include <wx/dcgraph.h>
using boost::optional;
namespace fs = boost::filesystem;

static const float GROUND_Z = -0.03f;
static const float GROUND_Z_GRIDLINE = -0.26f;
static const float GRABBER_X_FACTOR = 0.20f;
static const float GRABBER_Y_FACTOR = 0.03f;
static const float GRABBER_Z_VALUE = 0.5f;
static unsigned int GLOBAL_PLATE_INDEX = 0;

static const double LOGICAL_PART_PLATE_GAP = 1. / 5.;
static const int PARTPLATE_ICON_SIZE = 16;
static const int PARTPLATE_EDIT_PLATE_NAME_ICON_SIZE = 9; // ORCA this also scales height of plate name
static const int PARTPLATE_ICON_GAP_TOP = 3;
static const int PARTPLATE_ICON_GAP_LEFT = 3;
static const int PARTPLATE_ICON_GAP_Y = 5;
static const int PARTPLATE_TEXT_OFFSET_X1 = 3;
static const int PARTPLATE_TEXT_OFFSET_X2 = 1;
static const int PARTPLATE_TEXT_OFFSET_Y = 1;
static const int PARTPLATE_PLATENAME_OFFSET_Y  = 10;

const float WIPE_TOWER_DEFAULT_X_POS = 165.;
const float WIPE_TOWER_DEFAULT_Y_POS = 250.;  // Max y

const float I3_WIPE_TOWER_DEFAULT_X_POS = 0.;
const float I3_WIPE_TOWER_DEFAULT_Y_POS = 250.; // Max y

std::array<unsigned char, 4>  PlateTextureForeground = {0x0, 0xae, 0x42, 0xff};

namespace Slic3r {
namespace GUI {

class Bed3D;

ColorRGBA PartPlate::SELECT_COLOR		= { 0.2666f, 0.2784f, 0.2784f, 1.0f }; //{ 0.4196f, 0.4235f, 0.4235f, 1.0f };
ColorRGBA PartPlate::UNSELECT_COLOR		= { 0.82f, 0.82f, 0.82f, 1.0f };
ColorRGBA PartPlate::UNSELECT_DARK_COLOR		= { 0.384f, 0.384f, 0.412f, 1.0f };
ColorRGBA PartPlate::DEFAULT_COLOR		= { 0.5f, 0.5f, 0.5f, 1.0f };
ColorRGBA PartPlate::LINE_TOP_COLOR		= { 0.89f, 0.89f, 0.89f, 1.0f };
ColorRGBA PartPlate::LINE_TOP_DARK_COLOR		= { 0.431f, 0.431f, 0.463f, 1.0f };
ColorRGBA PartPlate::LINE_TOP_SEL_COLOR  = { 0.5294f, 0.5451, 0.5333f, 1.0f};
ColorRGBA PartPlate::LINE_TOP_SEL_DARK_COLOR = { 0.298f, 0.298f, 0.3333f, 1.0f};
ColorRGBA PartPlate::LINE_BOTTOM_COLOR	= { 0.8f, 0.8f, 0.8f, 0.4f };
ColorRGBA PartPlate::HEIGHT_LIMIT_TOP_COLOR		= { 0.6f, 0.6f, 1.0f, 1.0f };
ColorRGBA PartPlate::HEIGHT_LIMIT_BOTTOM_COLOR	= { 0.4f, 0.4f, 1.0f, 1.0f };

// get text extent with wxMemoryDC
void get_text_extent(const wxString &msg, wxCoord &w, wxCoord &h, wxFont *font)
{
  wxMemoryDC memDC;
  if (font)
    memDC.SetFont(*font);
  memDC.GetTextExtent(msg, &w, &h);
}


wxFont* find_font(const std::string& text_str, int max_size = 32)
{
  auto is_font_suitable = [](std::string str, wxFont &font, int max_size) {
    wxString msg(str);
	wxCoord w, h;
	get_text_extent(msg, w, h, &font);

    if (w <= max_size)
      return true;
    else
      return false;
  };
  wxFont *font = nullptr;
  if (is_font_suitable(text_str, Label::Head_24, max_size))
    font = &Label::Head_24;
  else if (is_font_suitable(text_str, Label::Head_20, max_size))
    font = &Label::Head_20;
  else if (is_font_suitable(text_str, Label::Head_18, max_size))
    font = &Label::Head_18;
  else if (is_font_suitable(text_str, Label::Head_16, max_size))
    font = &Label::Head_16;
  else if (is_font_suitable(text_str, Label::Head_14, max_size))
    font = &Label::Head_14;
  else
    font = &Label::Head_12;

  return font;
}
void PartPlate::update_render_colors()
{
	PartPlate::SELECT_COLOR			= ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Plate_Selected]);
    PartPlate::UNSELECT_COLOR		= ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Plate_Unselected]);
    PartPlate::DEFAULT_COLOR        = ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Plate_Default]);
    PartPlate::LINE_TOP_COLOR       = ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Plate_Line_Top]);
    PartPlate::LINE_BOTTOM_COLOR    = ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Plate_Line_Bottom]);
}

void PartPlate::load_render_colors()
{
    RenderColor::colors[RenderCol_Plate_Selected]    = ImGuiWrapper::to_ImVec4(SELECT_COLOR);
    RenderColor::colors[RenderCol_Plate_Unselected]  = ImGuiWrapper::to_ImVec4(UNSELECT_COLOR);
    RenderColor::colors[RenderCol_Plate_Default]     = ImGuiWrapper::to_ImVec4(DEFAULT_COLOR);
    RenderColor::colors[RenderCol_Plate_Line_Top]    = ImGuiWrapper::to_ImVec4(LINE_TOP_COLOR);
    RenderColor::colors[RenderCol_Plate_Line_Bottom] = ImGuiWrapper::to_ImVec4(LINE_BOTTOM_COLOR);
}


PartPlate::PartPlate()
	: ObjectBase(-1), m_plater(nullptr), m_model(nullptr), m_quadric(nullptr)
{
	assert(this->id().invalid());
	init();
}

PartPlate::PartPlate(PartPlateList *partplate_list, Vec3d origin, int width, int depth, int height, Plater* platerObj, Model* modelObj, bool printable, PrinterTechnology tech)
	:m_partplate_list(partplate_list), m_plater(platerObj), m_model(modelObj), printer_technology(tech), m_origin(origin), m_width(width), m_depth(depth), m_height(height),  m_printable(printable)
{
	init();
}

PartPlate::~PartPlate()
{
	clear();
	//if (m_quadric != nullptr)
	//	::gluDeleteQuadric(m_quadric);

	//boost::nowide::remove(m_tmp_gcode_path.c_str());
}

void PartPlate::init()
{
	m_locked = false;
	m_ready_for_slice = true;
	m_slice_result_valid = false;
	m_slice_percent = 0.0f;
	m_hover_id = -1;
	m_selected = false;
	//m_quadric = ::gluNewQuadric();
	//if (m_quadric != nullptr)
	//	::gluQuadricDrawStyle(m_quadric, GLU_FILL);

	m_print_index = -1;
	m_print = nullptr;
	m_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = FilamentMapMode::fmmAutoForFlush;
}

BedType PartPlate::get_bed_type(bool load_from_project) const
{
	std::string bed_type_key = "curr_bed_type";

	if (m_config.has(bed_type_key)) {
		BedType bed_type = m_config.opt_enum<BedType>(bed_type_key);
		return bed_type;
	}

	if (!load_from_project || !m_plater || !wxGetApp().preset_bundle)
		return btDefault;

	DynamicConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
	if (proj_cfg.has(bed_type_key))
		return proj_cfg.opt_enum<BedType>(bed_type_key);
	return btDefault;
}

void PartPlate::set_bed_type(BedType bed_type)
{
    std::string bed_type_key = "curr_bed_type";

    // should be called in GUI context
    assert(m_plater != nullptr);

    // update slice state
    BedType old_real_bed_type = get_bed_type();
    if (old_real_bed_type == btDefault) {
        DynamicConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
        if (proj_cfg.has(bed_type_key))
            old_real_bed_type = proj_cfg.opt_enum<BedType>(bed_type_key);
    }
    BedType new_real_bed_type = bed_type;
    if (bed_type == BedType::btDefault) {
        DynamicConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
        if (proj_cfg.has(bed_type_key))
            new_real_bed_type = proj_cfg.opt_enum<BedType>(bed_type_key);
    }
    if (old_real_bed_type != new_real_bed_type) {
        update_slice_result_valid_state(false);
    }

    if (bed_type == BedType::btDefault)
        m_config.erase(bed_type_key);
    else
        m_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(bed_type));
}

void PartPlate::reset_bed_type()
{
    m_config.erase("curr_bed_type");
}

void PartPlate::reset_skirt_start_angle()
{
    m_config.erase("skirt_start_angle");
}

void PartPlate::set_print_seq(PrintSequence print_seq)
{
    std::string print_seq_key = "print_sequence";

    // should be called in GUI context
    assert(m_plater != nullptr);

    // update slice state
    PrintSequence old_real_print_seq = get_print_seq();
    if (old_real_print_seq == PrintSequence::ByDefault) {
        auto curr_preset_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (curr_preset_config.has(print_seq_key))
            old_real_print_seq = curr_preset_config.option<ConfigOptionEnum<PrintSequence>>(print_seq_key)->value;
    }

    PrintSequence new_real_print_seq = print_seq;

    if (print_seq == PrintSequence::ByDefault) {
        auto curr_preset_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (curr_preset_config.has(print_seq_key))
            new_real_print_seq = curr_preset_config.option<ConfigOptionEnum<PrintSequence>>(print_seq_key)->value;
    }

    if (old_real_print_seq != new_real_print_seq) {
        update_slice_result_valid_state(false);
    }

    //print_seq_same_global = same_global;
    if (print_seq == PrintSequence::ByDefault)
        m_config.erase(print_seq_key);
    else
        m_config.set_key_value(print_seq_key, new ConfigOptionEnum<PrintSequence>(print_seq));
}

PrintSequence PartPlate::get_print_seq() const
{
    std::string print_seq_key = "print_sequence";

    if (m_config.has(print_seq_key)) {
        PrintSequence print_seq = m_config.opt_enum<PrintSequence>(print_seq_key);
        return print_seq;
    }

    return PrintSequence::ByDefault;
}

PrintSequence PartPlate::get_real_print_seq(bool* plate_same_as_global) const
{
	PrintSequence global_print_seq = wxGetApp().global_print_sequence();
    PrintSequence curr_plate_seq = get_print_seq();
    if (curr_plate_seq == PrintSequence::ByDefault) {
		curr_plate_seq = global_print_seq;
    }

	if(plate_same_as_global)
		*plate_same_as_global = (curr_plate_seq == global_print_seq);

    return curr_plate_seq;
}

std::vector<int> PartPlate::get_real_filament_maps(const DynamicConfig& g_config, bool* use_global_param) const
{
	auto maps = get_filament_maps();
	if (!maps.empty()) {
		if (use_global_param) { *use_global_param = false; }
		return maps;
	}
	auto g_maps = g_config.option<ConfigOptionInts>("filament_map")->values;
	if (use_global_param) { *use_global_param = true; }
	return g_maps;
}

FilamentMapMode PartPlate::get_real_filament_map_mode(const DynamicConfig& g_config, bool* use_global_param) const
{
	auto mode = get_filament_map_mode();
	if (FilamentMapMode::fmmDefault != mode) {
		if (use_global_param) { *use_global_param = false; };
		return mode;
	}

	auto g_mode = g_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode")->value;
	if (use_global_param) { *use_global_param = true; }
	return g_mode;
}


bool PartPlate::has_spiral_mode_config() const
{
	std::string key = "spiral_mode";
	return m_config.has(key);
}

bool PartPlate::get_spiral_vase_mode() const
{
	std::string key = "spiral_mode";
	if (m_config.has(key)) {
		return m_config.opt_bool(key);
	}
	else {
		DynamicPrintConfig* global_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;
		if (global_config->has(key))
			return global_config->opt_bool(key);
	}
	return false;
}

std::vector<Vec2d> PartPlate::get_plate_wrapping_detection_area() const
{
    DynamicPrintConfig  gconfig                  = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    ConfigOptionPoints *wrapping_exclude_area_opt = gconfig.option<ConfigOptionPoints>("wrapping_exclude_area");
    if (wrapping_exclude_area_opt) {
        std::vector<Vec2d> wrapping_area = wrapping_exclude_area_opt->values;
        for (Vec2d& pt : wrapping_area) {
            pt += Vec2d(m_origin.x(), m_origin.y());
        }
        return wrapping_area;
    }

    return std::vector<Vec2d>();
}

void PartPlate::set_spiral_vase_mode(bool spiral_mode, bool as_global)
{
	std::string key = "spiral_mode";
	if (as_global)
		m_config.erase(key);
	else {
		if (spiral_mode) {
			if (get_spiral_vase_mode())
				return;
			// Secondary confirmation
			auto answer = static_cast<TabPrintPlate*>(wxGetApp().plate_tab)->show_spiral_mode_settings_dialog(false);
			if (answer == wxID_YES) {
				m_config.set_key_value(key, new ConfigOptionBool(true));
				set_vase_mode_related_object_config();
			}
		}
		else
			m_config.set_key_value(key, new ConfigOptionBool(false));
	}
}

bool PartPlate::valid_instance(int obj_id, int instance_id)
{
	if ((obj_id >= 0) && (obj_id < m_model->objects.size()))
	{
		ModelObject* object = m_model->objects[obj_id];
		if ((instance_id >= 0) && (instance_id < object->instances.size()))
			return true;
	}

	return false;
}

void PartPlate::calc_bounding_boxes() const {
	BoundingBoxf3* bounding_box = const_cast<BoundingBoxf3*>(&m_bounding_box);
	*bounding_box = BoundingBoxf3();
	for (const Vec2d& p : m_shape) {
		bounding_box->merge({ p(0), p(1), 0.0 });
	}

	BoundingBoxf3* extended_bounding_box = const_cast<BoundingBoxf3*>(&m_extended_bounding_box);
	*extended_bounding_box = m_bounding_box;

	double half_x = bounding_box->size().x() * GRABBER_X_FACTOR;
	double half_y = bounding_box->size().y() * 1.0f * GRABBER_Y_FACTOR;
	double half_z = GRABBER_Z_VALUE;
	Vec3d center(bounding_box->center().x(), bounding_box->min(1) -half_y, GROUND_Z);
	m_grabber_box.min = Vec3d(center.x() - half_x, center.y() - half_y, center.z() - half_z);
	m_grabber_box.max = Vec3d(center.x() + half_x, center.y() + half_y, center.z() + half_z);
	m_grabber_box.defined = true;
	extended_bounding_box->merge(m_grabber_box);

    //calc exclude area bounding box
    m_exclude_bounding_box.clear();
    BoundingBoxf3 exclude_bb;
    for (int index = 0; index < m_exclude_area.size(); index ++) {
		const Vec2d& p = m_exclude_area[index];

		if (index % 4 == 0)
			exclude_bb = BoundingBoxf3();

		exclude_bb.merge({ p(0), p(1), 0.0 });

		if (index % 4 == 3)
		{
			exclude_bb.max(2) = m_depth;
			exclude_bb.min(2) = GROUND_Z;
			m_exclude_bounding_box.emplace_back(exclude_bb);
		}
	}
}

void PartPlate::calc_triangles(const ExPolygon &poly)
{
    m_triangles.reset();

    if (!init_model_from_poly(m_triangles.model, poly, GROUND_Z))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":Unable to create plate triangles\n";
}

void PartPlate::calc_exclude_triangles(const ExPolygon &poly)
{
    m_exclude_triangles.reset();

    if (!init_model_from_poly(m_exclude_triangles, poly, GROUND_Z))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":Unable to create exclude triangles\n";
}

void PartPlate::calc_triangles_from_polygon(const ExPolygon &poly, GLModel &render_model){
    if (poly.empty()) {
        render_model.reset();
        return;
    }
    render_model.reset();
    if (!init_model_from_poly(render_model, poly, GROUND_Z)) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "calc_triangles_from_polygon fail";
    }
}

static bool init_model_from_lines(GLModel &model, const Lines &lines, float z)
{

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(2 * lines.size());
    init_data.reserve_indices(2 * lines.size());

    for (const auto &l : lines) {
        init_data.add_vertex(Vec3f(unscale<float>(l.a.x()), unscale<float>(l.a.y()), z));
        init_data.add_vertex(Vec3f(unscale<float>(l.b.x()), unscale<float>(l.b.y()), z));
        const unsigned int vertices_counter = (unsigned int)init_data.vertices_count();
        init_data.add_line(vertices_counter - 2, vertices_counter - 1);
    }

    model.init_from(std::move(init_data));

    return true;
}

static bool init_model_from_lines(GLModel &model, const Lines3 &lines)
{

    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(2 * lines.size());
    init_data.reserve_indices(2 * lines.size());

    for (const auto &l : lines) {
        init_data.add_vertex(Vec3f(unscale<float>(l.a.x()), unscale<float>(l.a.y()), unscale<float>(l.a.z())));
        init_data.add_vertex(Vec3f(unscale<float>(l.b.x()), unscale<float>(l.b.y()), unscale<float>(l.b.z())));
        const unsigned int vertices_counter = (unsigned int) init_data.vertices_count();
        init_data.add_line(vertices_counter - 2, vertices_counter - 1);
    }

    model.init_from(std::move(init_data));

    return true;
}

static void init_raycaster_from_model(PickingModel& model)
{
    assert(model.mesh_raycaster == nullptr);

	const GLModel::Geometry &geometry = model.model.get_geometry();

    indexed_triangle_set its;
    its.vertices.reserve(geometry.vertices_count());
    for (size_t i = 0; i < geometry.vertices_count(); ++i) {
        its.vertices.emplace_back(geometry.extract_position_3(i));
    }
    its.indices.reserve(geometry.indices_count() / 3);
    for (size_t i = 0; i < geometry.indices_count() / 3; ++i) {
        const size_t tri_id = i * 3;
        its.indices.emplace_back(geometry.extract_index(tri_id), geometry.extract_index(tri_id + 1), geometry.extract_index(tri_id + 2));
    }

    model.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
}

void PartPlate::calc_gridlines(const ExPolygon& poly, const BoundingBox& pp_bbox) {
    m_gridlines.reset();
    m_gridlines_bolder.reset();

    // calculate and generate grid
    int   step          = Bed_2D::calculate_grid_step(pp_bbox, scale_(1.00));
    Vec2d scaled_origin = Vec2d(scale_(m_origin.x()),scale_(m_origin.y()));
    auto  grid_lines    = Bed_2D::generate_grid(poly, pp_bbox, scaled_origin, scale_(step), SCALED_EPSILON);

    Lines lines_thin = to_lines(grid_lines[0]);
	Lines lines_bold = to_lines(grid_lines[1]);

	// append bed contours
	Lines contour_lines = to_lines(poly);
	std::copy(contour_lines.begin(), contour_lines.end(), std::back_inserter(lines_thin));

	if (!init_model_from_lines(m_gridlines       , lines_thin, GROUND_Z_GRIDLINE))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Unable to create bed grid lines\n";

	if (!init_model_from_lines(m_gridlines_bolder, lines_bold, GROUND_Z_GRIDLINE))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Unable to create bed grid lines\n";
}

void PartPlate::calc_height_limit() {
    m_height_limit_common.reset();
    m_height_limit_bottom.reset();
    m_height_limit_top.reset();

	Lines3 bottom_h_lines, top_lines, top_h_lines, common_lines;
	int shape_count = m_shape.size();
	float first_z = 0.02f;
	for (int i = 0; i < shape_count; i++) {
		auto &cur_p = m_shape[i];
		Vec3crd p1(scale_(cur_p.x()), scale_(cur_p.y()), scale_(first_z));
		Vec3crd p2(scale_(cur_p.x()), scale_(cur_p.y()), scale_(m_height_to_rod));
		Vec3crd p3(scale_(cur_p.x()), scale_(cur_p.y()), scale_(m_height_to_lid));

		common_lines.emplace_back(p1, p2);
		top_lines.emplace_back(p2, p3);

		Vec2d next_p;
		if (i < (shape_count - 1)) {
			next_p = m_shape[i+1];

		}
		else {
			next_p = m_shape[0];
		}
		Vec3crd p4(scale_(cur_p.x()), scale_(cur_p.y()), scale_(m_height_to_rod));
		Vec3crd p5(scale_(next_p.x()), scale_(next_p.y()), scale_(m_height_to_rod));
		bottom_h_lines.emplace_back(p4, p5);

		Vec3crd p6(scale_(cur_p.x()), scale_(cur_p.y()), scale_(m_height_to_lid));
		Vec3crd p7(scale_(next_p.x()), scale_(next_p.y()), scale_(m_height_to_lid));
		top_h_lines.emplace_back(p6, p7);
	}
	//std::copy(bottom_lines.begin(), bottom_lines.end(), std::back_inserter(bottom_h_lines));
	std::copy(top_lines.begin(), top_lines.end(), std::back_inserter(top_h_lines));

	if (!init_model_from_lines(m_height_limit_common, common_lines))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Unable to create height limit bottom lines\n";

	if (!init_model_from_lines(m_height_limit_bottom, bottom_h_lines))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Unable to create height limit bottom lines\n";

	if (!init_model_from_lines(m_height_limit_top, top_h_lines))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Unable to create height limit top lines\n";
}

void PartPlate::calc_vertex_for_number(int index, bool one_number, GLModel &buffer)
{
    buffer.reset();

	ExPolygon poly;
#if 0 //in the up area
	Vec2d& p = m_shape[2];
	float offset_x = one_number?PARTPLATE_TEXT_OFFSET_X1: PARTPLATE_TEXT_OFFSET_X2;

	poly.contour.append({ scale_(p(0) + PARTPLATE_ICON_GAP + offset_x), scale_(p(1) - index * (PARTPLATE_ICON_SIZE + PARTPLATE_ICON_GAP) - PARTPLATE_ICON_GAP - PARTPLATE_ICON_SIZE + PARTPLATE_TEXT_OFFSET_Y) });
	poly.contour.append({ scale_(p(0) + PARTPLATE_ICON_GAP + PARTPLATE_ICON_SIZE - offset_x), scale_(p(1) - index * (PARTPLATE_ICON_SIZE + PARTPLATE_ICON_GAP)- PARTPLATE_ICON_GAP - PARTPLATE_ICON_SIZE + PARTPLATE_TEXT_OFFSET_Y) });
	poly.contour.append({ scale_(p(0) + PARTPLATE_ICON_GAP + PARTPLATE_ICON_SIZE - offset_x), scale_(p(1) - index * (PARTPLATE_ICON_SIZE + PARTPLATE_ICON_GAP)- PARTPLATE_ICON_GAP - PARTPLATE_TEXT_OFFSET_Y)});
	poly.contour.append({ scale_(p(0) + PARTPLATE_ICON_GAP + offset_x), scale_(p(1) - index * (PARTPLATE_ICON_SIZE + PARTPLATE_ICON_GAP)- PARTPLATE_ICON_GAP - PARTPLATE_TEXT_OFFSET_Y) });
#else //in the bottom
    auto bed_ext   = get_extents(m_shape);
    Vec2d p        = bed_ext[1];
    float factor   = bed_ext.size()(1) / 200.0;
    float size     = PARTPLATE_ICON_SIZE     * factor;
    float offset_y = PARTPLATE_TEXT_OFFSET_Y * factor;
    float offset_x = (one_number?PARTPLATE_TEXT_OFFSET_X1: PARTPLATE_TEXT_OFFSET_X2) * factor;
    float gap_left = PARTPLATE_ICON_GAP_LEFT * factor;
    p += Vec2d(gap_left,0);

    poly.contour.append({ scale_(p(0) + offset_x)       , scale_(p(1) + offset_y) });
    poly.contour.append({ scale_(p(0) + size - offset_x), scale_(p(1) + offset_y) });
    poly.contour.append({ scale_(p(0) + size - offset_x), scale_(p(1) + size - offset_y) });
    poly.contour.append({ scale_(p(0) + offset_x)       , scale_(p(1) + size - offset_y) });
#endif
    if (!init_model_from_poly(buffer, poly, GROUND_Z))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Unable to generate geometry buffers for icons\n";
}

void PartPlate::calc_vertex_for_plate_name_edit_icon(GLTexture *texture, int index, PickingModel &model) {
    model.reset();

    ExPolygon poly;
    auto  bed_ext  = get_extents(m_shape);
    Vec2d p        = bed_ext[3];
    float factor   = bed_ext.size()(1) / 200.0;
    float icon_sz  = factor * PARTPLATE_EDIT_PLATE_NAME_ICON_SIZE;
    float width    = icon_sz;
    float height   = icon_sz;
    float offset_y = factor * PARTPLATE_TEXT_OFFSET_Y;

    float name_width = 0.0;
    if (texture && texture->get_width() > 0 && texture->get_height())
        // original width give correct ratio in here since rendering width can be much higher because of next_highest_power_of_2 for rendering
        name_width = icon_sz * texture->m_original_width / texture->get_height(); 

    //if (m_plater && m_plater->get_build_volume_type() == BuildVolume_Type::Circle)
    //    px = scale_(bed_ext.center()(0)) + m_name_texture_width * 0.50 - height * 0.50;
    p += Vec2d(name_width, offset_y);

    poly.contour.append({ scale_(p(0)        ), scale_(p(1)         ) });
    poly.contour.append({ scale_(p(0) + width), scale_(p(1)         ) });
    poly.contour.append({ scale_(p(0) + width), scale_(p(1) + height) });
    poly.contour.append({ scale_(p(0)        ), scale_(p(1) + height) });

    if (!init_model_from_poly(model.model, poly, GROUND_Z))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Unable to generate geometry buffers for icons\n";

    init_raycaster_from_model(model);
}

void PartPlate::calc_vertex_for_icons(int index, PickingModel &model)
{
    model.reset();

    ExPolygon poly;
    auto  bed_ext  = get_extents(m_shape);
    Vec2d p        = bed_ext[2];
    auto  factor   = bed_ext.size()(1) / 200.0;
    float size     = PARTPLATE_ICON_SIZE     * factor;
    float gap_left = PARTPLATE_ICON_GAP_LEFT * factor;
    float gap_y    = PARTPLATE_ICON_GAP_Y    * factor;
    float gap_top  = PARTPLATE_ICON_GAP_TOP  * factor;
    p += Vec2d(gap_left,-1 * (index * (size + gap_y) + gap_top));

    if (m_plater && m_plater->get_build_volume_type() == BuildVolume_Type::Circle)
        p[1] -= std::max(0.0, (bed_ext.size()(1) - (size + gap_y) * 6 /* bed_icon_count */) / 2);

    poly.contour.append({ scale_(p(0))       , scale_(p(1) - size) });
    poly.contour.append({ scale_(p(0) + size), scale_(p(1) - size) });
    poly.contour.append({ scale_(p(0) + size), scale_(p(1)) });
    poly.contour.append({ scale_(p(0))       , scale_(p(1)) });

	if (!init_model_from_poly(model.model, poly, GROUND_Z))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Unable to generate geometry buffers for icons\n";

	init_raycaster_from_model(model);
}

/*
void PartPlate::calc_vertex_for_icons_background(int icon_count, GLModel &buffer)
{
    buffer.reset();

	ExPolygon poly;
	auto bed_ext = get_extents(m_shape);
    Vec2d p = bed_ext[2];

	poly.contour.append({ scale_(p(0) + PARTPLATE_ICON_GAP_LEFT), scale_(p(1) - icon_count * (PARTPLATE_ICON_SIZE + PARTPLATE_ICON_GAP_Y) - PARTPLATE_ICON_GAP_TOP) });
	poly.contour.append({ scale_(p(0) + PARTPLATE_ICON_GAP_LEFT + PARTPLATE_ICON_SIZE), scale_(p(1) - icon_count * (PARTPLATE_ICON_SIZE + PARTPLATE_ICON_GAP_Y)- PARTPLATE_ICON_GAP_TOP) });
	poly.contour.append({ scale_(p(0) + PARTPLATE_ICON_GAP_LEFT + PARTPLATE_ICON_SIZE), scale_(p(1) - PARTPLATE_ICON_GAP_TOP)});
	poly.contour.append({ scale_(p(0) + PARTPLATE_ICON_GAP_LEFT), scale_(p(1) - PARTPLATE_ICON_GAP_TOP) });

	if (!init_model_from_poly(buffer, poly, GROUND_Z))
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Unable to generate geometry buffers for icons\n";
}
*/

void PartPlate::render_background(bool force_default_color)
{
	//return directly for current plate
	if (m_selected && !force_default_color) return;

	// draw background
	glsafe(::glDepthMask(GL_FALSE));

	ColorRGBA color;
	if (!force_default_color) {
		if (m_selected) {
            color = PartPlate::SELECT_COLOR;
		}
		else {
            color = m_partplate_list->m_is_dark ? PartPlate::UNSELECT_DARK_COLOR : PartPlate::UNSELECT_COLOR;
		}
	}
	else {
        color = PartPlate::DEFAULT_COLOR;
	}
    m_triangles.model.set_color(color);
    m_triangles.model.render();
	glsafe(::glDepthMask(GL_TRUE));
}

void PartPlate::render_logo_texture(GLTexture &logo_texture, GLModel& logo_buffer, bool bottom)
{
	//check valid
	if (logo_texture.unsent_compressed_data_available()) {
		// sends to gpu the already available compressed levels of the main texture
		logo_texture.send_compressed_data_to_gpu();
	}

	if (logo_buffer.is_initialized()) {
		GLShaderProgram* shader = wxGetApp().get_shader("printbed");
		if (shader != nullptr) {
			shader->start_using();
            const Camera &camera = wxGetApp().plater()->get_camera();
            shader->set_uniform("view_model_matrix", camera.get_view_matrix());
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
			shader->set_uniform("transparent_background", 0);
			shader->set_uniform("svg_source", 0);

			//glsafe(::glEnable(GL_DEPTH_TEST));
			glsafe(::glDepthMask(GL_FALSE));

			glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

			if (bottom)
				glsafe(::glFrontFace(GL_CW));

			// show the temporary texture while no compressed data is available
			GLuint tex_id = (GLuint)logo_texture.get_id();

			glsafe(::glBindTexture(GL_TEXTURE_2D, tex_id));
            logo_buffer.render();
			glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

			if (bottom)
				glsafe(::glFrontFace(GL_CCW));

            glsafe(::glDisable(GL_BLEND));

			glsafe(::glDepthMask(GL_TRUE));

			shader->stop_using();
		}
	}
}

void PartPlate::render_logo(bool bottom, bool render_cali)
{
	if (!m_partplate_list->render_bedtype_logo) {
		// render third-party printer texture logo
		if (m_partplate_list->m_logo_texture_filename.empty()) {
			m_partplate_list->m_logo_texture.reset();
			return;
		}

		//GLTexture* temp_texture = const_cast<GLTexture*>(&m_temp_texture);

		if (m_partplate_list->m_logo_texture.get_id() == 0 || m_partplate_list->m_logo_texture.get_source() != m_partplate_list->m_logo_texture_filename) {
			m_partplate_list->m_logo_texture.reset();

			if (boost::algorithm::iends_with(m_partplate_list->m_logo_texture_filename, ".svg")) {
				/*// use higher resolution images if graphic card and opengl version allow
				GLint max_tex_size = OpenGLManager::get_gl_info().get_max_tex_size();
				if (temp_texture->get_id() == 0 || temp_texture->get_source() != m_texture_filename) {
					// generate a temporary lower resolution texture to show while no main texture levels have been compressed
					if (!temp_texture->load_from_svg_file(m_texture_filename, false, false, false, max_tex_size / 8)) {
						render_default(bottom, false);
						return;
					}
					canvas.request_extra_frame();
				}*/

				// starts generating the main texture, compression will run asynchronously
				GLint max_tex_size = OpenGLManager::get_gl_info().get_max_tex_size();
				GLint logo_tex_size = (max_tex_size < 2048) ? max_tex_size : 2048;
				if (!m_partplate_list->m_logo_texture.load_from_svg_file(m_partplate_list->m_logo_texture_filename, true, true, true, logo_tex_size)) {
					BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": load logo texture from %1% failed!") % m_partplate_list->m_logo_texture_filename;
					return;
				}
			}
			else if (boost::algorithm::iends_with(m_partplate_list->m_logo_texture_filename, ".png")) {
				// generate a temporary lower resolution texture to show while no main texture levels have been compressed
				/* if (temp_texture->get_id() == 0 || temp_texture->get_source() != m_logo_texture_filename) {
					if (!temp_texture->load_from_file(m_logo_texture_filename, false, GLTexture::None, false)) {
						render_default(bottom, false);
						return;
					}
					canvas.request_extra_frame();
				}*/

				// starts generating the main texture, compression will run asynchronously
				if (!m_partplate_list->m_logo_texture.load_from_file(m_partplate_list->m_logo_texture_filename, true, GLTexture::MultiThreaded, true)) {
					BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": load logo texture from %1% failed!") % m_partplate_list->m_logo_texture_filename;
					return;
				}
			}
			else {
				BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": can not load logo texture from %1%, unsupported format") % m_partplate_list->m_logo_texture_filename;
				return;
			}
		}
		else if (m_partplate_list->m_logo_texture.unsent_compressed_data_available()) {
			// sends to gpu the already available compressed levels of the main texture
			m_partplate_list->m_logo_texture.send_compressed_data_to_gpu();

			// the temporary texture is not needed anymore, reset it
			//if (temp_texture->get_id() != 0)
			//    temp_texture->reset();

			//canvas.request_extra_frame();
		}

		if (m_logo_triangles.is_initialized())
			render_logo_texture(m_partplate_list->m_logo_texture, m_logo_triangles, bottom);
		return;
	}

	m_partplate_list->load_bedtype_textures();
	m_partplate_list->load_cali_textures();
    m_partplate_list->load_extruder_only_area_textures();
	// btDefault should be skipped
	auto curr_bed_type = get_bed_type();
	if (curr_bed_type == btDefault) {
        DynamicConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
        if (proj_cfg.has(std::string("curr_bed_type")))
            curr_bed_type = proj_cfg.opt_enum<BedType>(std::string("curr_bed_type"));
	}
	int bed_type_idx = (int)curr_bed_type;
    auto is_single_extruder = wxGetApp().preset_bundle->get_printer_extruder_count() == 1;
    if (!is_single_extruder) {
        if (m_partplate_list->m_allow_bed_type_in_double_nozzle.find(bed_type_idx) == m_partplate_list->m_allow_bed_type_in_double_nozzle.end()) {
            bed_type_idx = 0;
        }
    }
	// render bed textures
	for (auto &part : m_partplate_list->bed_texture_info[bed_type_idx].parts) {
		if (part.texture) {
			if (part.buffer && part.buffer->is_initialized()
				//&& part.vbo_id != 0
				) {
				if (part.offset.x() != m_origin.x() || part.offset.y() != m_origin.y()) {
					part.offset = Vec2d(m_origin.x(), m_origin.y());
					part.update_buffer();
				}
				render_logo_texture(*(part.texture),
									*(part.buffer),
									bottom);
			}
		}
	}

	// render cali texture
	if (render_cali) {
		for (auto& part : m_partplate_list->cali_texture_info.parts) {
			if (part.texture) {
                if (part.buffer && part.buffer->is_initialized()) {
					if (part.offset.x() != m_origin.x() || part.offset.y() != m_origin.y()) {
						part.offset = Vec2d(m_origin.x(), m_origin.y());
						part.update_buffer();
					}
					render_logo_texture(*(part.texture),
						*(part.buffer),
						bottom);
				}
			}
		}
	}

	//render extruder_only_area_info
    bool is_zh        = wxGetApp().app_config->get("language") == "zh_CN";
    int  language_idx = (int) (is_zh ? ExtruderOnlyAreaType::Chinese:ExtruderOnlyAreaType::Engilish);
    if (!is_single_extruder) {
        for (auto &part : m_partplate_list->extruder_only_area_info[language_idx].parts) {
            if (part.texture) {
                if (part.buffer && part.buffer->is_initialized()) {
                    if (part.offset.x() != m_origin.x() || part.offset.y() != m_origin.y()) {
                        part.offset = Vec2d(m_origin.x(), m_origin.y());
                        part.update_buffer();
                    }
                    render_logo_texture(*(part.texture), *(part.buffer), bottom);
                }
            }
        }
	}
}

void PartPlate::render_wrapping_detection_area(bool force_default_color)
{
    if (force_default_color || !m_wrapping_detection_triangles.is_initialized())
        return;
    ColorRGBA select_color{0.765f, 0.7686f, 0.7686f, 1.0f};
    m_wrapping_detection_triangles.set_color(select_color);
    m_wrapping_detection_triangles.render();
}

void PartPlate::render_exclude_area(bool force_default_color) {
	if (force_default_color) //for thumbnail case
		return;

	ColorRGBA select_color{   .9f, .86f, .82f, .7f }; // ORCA
	ColorRGBA unselect_color{ .6f, .6f, .6f, .3f }; // ORCA
	//ColorRGBA default_color{ 0.9f, 0.9f, 0.9f, 1.0f };

	// draw exclude area
	glsafe(::glDepthMask(GL_FALSE));

	if (m_selected) {
		glsafe(::glColor4fv(select_color.data()));
	}
	else {
		glsafe(::glColor4fv(unselect_color.data()));
	}

	m_exclude_triangles.set_color(m_selected ? select_color : unselect_color);
    m_exclude_triangles.render();
	glsafe(::glDepthMask(GL_TRUE));
}

/*void PartPlate::render_background_for_picking(const ColorRGBA render_color) const
{
	unsigned int triangles_vcount = m_triangles.get_vertices_count();

	glsafe(::glDepthMask(GL_FALSE));
	glsafe(::glColor4fv(render_color));
	glsafe(::glNormal3d(0.0f, 0.0f, 1.0f));
	glsafe(::glVertexPointer(3, GL_FLOAT, m_triangles.get_vertex_data_size(), (GLvoid*)m_triangles.get_vertices_data()));
	glsafe(::glDrawArrays(GL_TRIANGLES, 0, (GLsizei)triangles_vcount));
	glsafe(::glDepthMask(GL_TRUE));
}*/

void PartPlate::render_grid(bool bottom) {
	//glsafe(::glEnable(GL_MULTISAMPLE));
	// draw grid
	glsafe(::glLineWidth(1.0f * m_scale_factor));

    ColorRGBA color;
	if (bottom)
        color = LINE_BOTTOM_COLOR;
	else {
		if (m_selected)
            color = m_partplate_list->m_is_dark ? LINE_TOP_SEL_DARK_COLOR : LINE_TOP_SEL_COLOR;
		else
            color = m_partplate_list->m_is_dark ? LINE_TOP_DARK_COLOR : LINE_TOP_COLOR;
	}
    m_gridlines.set_color(color);
    m_gridlines.render();

	glsafe(::glLineWidth(2.0f * m_scale_factor));
    m_gridlines_bolder.set_color(color);
    m_gridlines_bolder.render();
}

void PartPlate::render_height_limit(PartPlate::HeightLimitMode mode)
{
	if (m_print && m_print->config().print_sequence == PrintSequence::ByObject && mode != HEIGHT_LIMIT_NONE)
	{
		// draw lower limit
		glsafe(::glLineWidth(3.0f * m_scale_factor));
        m_height_limit_common.set_color(HEIGHT_LIMIT_BOTTOM_COLOR);
        m_height_limit_common.render();

		if ((mode == HEIGHT_LIMIT_BOTTOM) || (mode == HEIGHT_LIMIT_BOTH)) {
			glsafe(::glLineWidth(3.0f * m_scale_factor));
            m_height_limit_bottom.set_color(HEIGHT_LIMIT_BOTTOM_COLOR);
            m_height_limit_bottom.render();
		}

		// draw upper limit
		if ((mode == HEIGHT_LIMIT_TOP) || (mode == HEIGHT_LIMIT_BOTH)){
            glsafe(::glLineWidth(3.0f * m_scale_factor));
            m_height_limit_top.set_color(HEIGHT_LIMIT_TOP_COLOR);
            m_height_limit_top.render();
		}
	}
}

void PartPlate::render_icon_texture(GLModel &buffer, GLTexture &texture)
{
	GLuint tex_id = (GLuint)texture.get_id();
	glsafe(::glBindTexture(GL_TEXTURE_2D, tex_id));
    buffer.render();
	glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
}

void PartPlate::render_plate_name_texture()
{
	if (m_name_texture.get_id() == 0)
		generate_plate_name_texture();

	GLuint tex_id = (GLuint)m_name_texture.get_id();
	glsafe(::glBindTexture(GL_TEXTURE_2D, tex_id));
    m_plate_name_icon.render();
	glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
}

void PartPlate::show_tooltip(const std::string tooltip)
{
    const auto scale = m_plater->get_current_canvas3D()->get_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6 * scale, 3 * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, {3 * scale});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BACKGROUND);
    ImGui::PushStyleColor(ImGuiCol_Border, {0, 0, 0, 0});
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(tooltip.c_str());
    ImGui::EndTooltip();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

void PartPlate::render_icons(bool bottom, bool only_name, int hover_id)
{
	GLShaderProgram* shader = wxGetApp().get_shader("printbed");
	if (shader != nullptr) {
		shader->start_using();
        const Camera &camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("view_model_matrix", camera.get_view_matrix());
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
		shader->set_uniform("transparent_background", bottom);
		//shader->set_uniform("svg_source", boost::algorithm::iends_with(m_partplate_list->m_del_texture.get_source(), ".svg"));
		shader->set_uniform("svg_source", 0);

        //if (bottom)
        //    glsafe(::glFrontFace(GL_CW));
        glsafe(::glDepthMask(GL_FALSE));

        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

        if (!only_name) {
            if (hover_id == 1) {
                render_icon_texture(m_del_icon.model, m_partplate_list->m_del_hovered_texture);
                show_tooltip(_u8L("Remove current plate (if not last one)"));
            }
            else
                render_icon_texture(m_del_icon.model, m_partplate_list->m_del_texture);

            if (hover_id == 2) {
                render_icon_texture(m_orient_icon.model, m_partplate_list->m_orient_hovered_texture);
                show_tooltip(_u8L("Auto orient objects on current plate"));
            }
            else
                render_icon_texture(m_orient_icon.model, m_partplate_list->m_orient_texture);

            if (hover_id == 3) {
                render_icon_texture(m_arrange_icon.model, m_partplate_list->m_arrange_hovered_texture);
                show_tooltip(_u8L("Arrange objects on current plate"));
            }
            else
                render_icon_texture(m_arrange_icon.model, m_partplate_list->m_arrange_texture);

            if (hover_id == 4) {
                if (this->is_locked()) {
                    render_icon_texture(m_lock_icon.model,
                                        m_partplate_list->m_locked_hovered_texture);
                    show_tooltip(_u8L("Unlock current plate"));
                }
                else {
                    render_icon_texture(m_lock_icon.model,
                                        m_partplate_list->m_lockopen_hovered_texture);
                    show_tooltip(_u8L("Lock current plate"));
                }
            } else {
                if (this->is_locked())
                    render_icon_texture(m_lock_icon.model, m_partplate_list->m_locked_texture);
                else
                    render_icon_texture(m_lock_icon.model, m_partplate_list->m_lockopen_texture);
            }

            PresetBundle* preset = wxGetApp().preset_bundle;
            bool dual_bbl = (preset->is_bbl_vendor() && preset->get_printer_extruder_count() == 2);
            if (dual_bbl) {
                if (hover_id == PLATE_FILAMENT_MAP_ID){
                    render_icon_texture(m_plate_filament_map_icon.model, m_partplate_list->m_plate_set_filament_map_hovered_texture);
                    show_tooltip(_u8L("Filament grouping"));
                } else
                    render_icon_texture(m_plate_filament_map_icon.model, m_partplate_list->m_plate_set_filament_map_texture);
            }

			if (hover_id == 6) {
                render_icon_texture(m_plate_name_edit_icon.model, m_partplate_list->m_plate_name_edit_hovered_texture);
                show_tooltip(_u8L("Edit current plate name"));
			}
			else
                render_icon_texture(m_plate_name_edit_icon.model, m_partplate_list->m_plate_name_edit_texture);

			if (hover_id == 7) {
                render_icon_texture(m_move_front_icon.model, m_partplate_list->m_move_front_hovered_texture);
                show_tooltip(_u8L("Move plate to the front"));
            } else
                render_icon_texture(m_move_front_icon.model, m_partplate_list->m_move_front_texture);


			if (m_partplate_list->render_plate_settings) {
				bool has_plate_settings = get_bed_type() != BedType::btDefault || get_print_seq() != PrintSequence::ByDefault || !get_first_layer_print_sequence().empty() || !get_other_layers_print_sequence().empty() || has_spiral_mode_config();
                if (hover_id == 5) {
                    if (!has_plate_settings)
                        render_icon_texture(m_plate_settings_icon.model, m_partplate_list->m_plate_settings_hovered_texture);
                    else
                        render_icon_texture(m_plate_settings_icon.model, m_partplate_list->m_plate_settings_changed_hovered_texture);

                    show_tooltip(_u8L("Customize current plate"));
                } else {
                    if (!has_plate_settings)
                        render_icon_texture(m_plate_settings_icon.model, m_partplate_list->m_plate_settings_texture);
                    else
                        render_icon_texture(m_plate_settings_icon.model, m_partplate_list->m_plate_settings_changed_texture);
                }
            }

            if (m_plate_index >= 0 && m_plate_index < MAX_PLATE_COUNT) {
                render_icon_texture(m_plate_idx_icon, m_partplate_list->m_idx_textures[m_plate_index]);
            }
        }
		render_plate_name_texture();

        glsafe(::glDisable(GL_BLEND));

        //if (bottom)
        //    glsafe(::glFrontFace(GL_CCW));

        glsafe(::glDepthMask(GL_TRUE));
        shader->stop_using();
    }
}

void PartPlate::render_only_numbers(bool bottom)
{
	GLShaderProgram* shader = wxGetApp().get_shader("printbed");
	if (shader != nullptr) {
		shader->start_using();
        const Camera &camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("view_model_matrix", camera.get_view_matrix());
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
		shader->set_uniform("transparent_background", bottom);
		//shader->set_uniform("svg_source", boost::algorithm::iends_with(m_partplate_list->m_del_texture.get_source(), ".svg"));
		shader->set_uniform("svg_source", 0);

        //if (bottom)
        //    glsafe(::glFrontFace(GL_CW));
        glsafe(::glDepthMask(GL_FALSE));

        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

        if (m_plate_index >=0 && m_plate_index < MAX_PLATE_COUNT) {
            render_icon_texture(m_plate_idx_icon, m_partplate_list->m_idx_textures[m_plate_index]);
        }

        glsafe(::glDisable(GL_BLEND));

        //if (bottom)
        //    glsafe(::glFrontFace(GL_CCW));

        glsafe(::glDepthMask(GL_TRUE));
        shader->stop_using();
    }
}

/*
void PartPlate::render_label(GLCanvas3D& canvas) const {
	std::string label = (boost::format("Plate %1%") % (m_plate_index + 1)).str();
	const Camera& camera = wxGetApp().plater()->get_camera();
	Transform3d world_to_eye = camera.get_view_matrix();
	Transform3d world_to_screen = camera.get_projection_matrix() * world_to_eye;
	const std::array<int, 4>& viewport = camera.get_viewport();

	BoundingBoxf3* bounding_box = const_cast<BoundingBoxf3*>(&m_bounding_box);
	Vec3d screen_box_center = world_to_screen * bounding_box->min;

	float x = 0.0f;
	float y = 0.0f;
	if (camera.get_type() == Camera::EType::Perspective) {
		x = (0.5f + 0.001f * 0.5f * (float)screen_box_center(0)) * viewport[2];
		y = (0.5f - 0.001f * 0.5f * (float)screen_box_center(1)) * viewport[3];
	}
	else {
		x = (0.5f + 0.5f * (float)screen_box_center(0)) * viewport[2];
		y = (0.5f - 0.5f * (float)screen_box_center(1)) * viewport[3];
	}

	ImGuiWrapper& imgui = *wxGetApp().imgui();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
	imgui.set_next_window_pos(x, y, ImGuiCond_Always, 0.5f, 0.5f);
	imgui.begin(label, ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
	ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
	float win_w = ImGui::GetWindowWidth();
	float label_len = imgui.calc_text_size(label).x;
	ImGui::SetCursorPosX(0.5f * (win_w - label_len));
	ImGui::AlignTextToFramePadding();
	imgui.text(label);

	// force re-render while the windows gets to its final size (it takes several frames)
	if (ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowNextAutoFitSize(ImGui::GetCurrentWindow()).x)
		canvas.request_extra_frame();

	imgui.end();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);

}

void PartPlate::render_grabber(const ColorRGBA render_color, bool use_lighting) const
{
	BoundingBoxf3* bounding_box = const_cast<BoundingBoxf3*>(&m_bounding_box);
	const Vec3d& center = m_grabber_box.center();

	if (use_lighting)
		glsafe(::glEnable(GL_LIGHTING));
	glsafe(::glColor4fv(render_color.data()));
	glsafe(::glPushMatrix());

	glsafe(::glTranslated(center(0), center(1), center(2)));

	Vec3d angles(Vec3d::Zero());
	glsafe(::glRotated(Geometry::rad2deg(angles(2)), 0.0, 0.0, 1.0));
	glsafe(::glRotated(Geometry::rad2deg(angles(1)), 0.0, 1.0, 0.0));
	glsafe(::glRotated(Geometry::rad2deg(angles(0)), 1.0, 0.0, 0.0));

	float half_x = bounding_box->size().x() * GRABBER_X_FACTOR;
	float half_y = bounding_box->size().y() * GRABBER_Y_FACTOR;
	float half_z = GRABBER_Z_VALUE;
	// face min x
	glsafe(::glPushMatrix());
	glsafe(::glTranslatef(-(GLfloat)half_x, 0, 0.0f));
	glsafe(::glRotatef(-90.0f, 0.0f, 1.0f, 0.0f));
	render_face(half_z, half_y);
	glsafe(::glPopMatrix());

	// face max x
	glsafe(::glPushMatrix());
	glsafe(::glTranslatef((GLfloat)half_x, 0, 0.0f));
	glsafe(::glRotatef(90.0f, 0.0f, 1.0f, 0.0f));
	render_face(half_z, half_y);
	glsafe(::glPopMatrix());

	// face min y
	glsafe(::glPushMatrix());
	glsafe(::glTranslatef(0.0f, -(GLfloat)half_y, 0.0f));
	glsafe(::glRotatef(90.0f, 1.0f, 0.0f, 0.0f));
	render_face(half_x, half_z);
	glsafe(::glPopMatrix());

	// face max y
	glsafe(::glPushMatrix());
	glsafe(::glTranslatef(0.0f, (GLfloat)half_y, 0.0f));
	glsafe(::glRotatef(-90.0f, 1.0f, 0.0f, 0.0f));
	render_face(half_x, half_z);
	glsafe(::glPopMatrix());

	// face min z
	glsafe(::glPushMatrix());
	glsafe(::glTranslatef(0.0f, 0.0f, -(GLfloat)half_z));
	glsafe(::glRotatef(180.0f, 1.0f, 0.0f, 0.0f));
	render_face(half_x, half_y);
	glsafe(::glPopMatrix());

	// face max z
	glsafe(::glPushMatrix());
	glsafe(::glTranslatef(0.0f, 0.0f, (GLfloat)half_z));
	render_face(half_x, half_y);
	glsafe(::glPopMatrix());

	glsafe(::glPopMatrix());

	if (use_lighting)
		glsafe(::glDisable(GL_LIGHTING));
}

void PartPlate::render_face(float x_size, float y_size) const
{
	::glBegin(GL_TRIANGLES);
	::glNormal3f(0.0f, 0.0f, 1.0f);
	::glVertex3f(-(GLfloat)x_size, -(GLfloat)y_size, 0.0f);
	::glVertex3f((GLfloat)x_size, -(GLfloat)y_size, 0.0f);
	::glVertex3f((GLfloat)x_size, (GLfloat)y_size, 0.0f);
	::glVertex3f((GLfloat)x_size, (GLfloat)y_size, 0.0f);
	::glVertex3f(-(GLfloat)x_size, (GLfloat)y_size, 0.0f);
	::glVertex3f(-(GLfloat)x_size, -(GLfloat)y_size, 0.0f);
	glsafe(::glEnd());
}

void PartPlate::render_arrows(const ColorRGBA render_color, bool use_lighting) const
{
#if 0
	if (m_quadric == nullptr)
		return;
	double radius = m_grabber_box.size().y() * 0.5f;
	double height = radius * 2.0f;
	double position = m_grabber_box.size().x() * 0.8f;
	if (use_lighting)
		glsafe(::glEnable(GL_LIGHTING));

	glsafe(::glColor4fv(render_color));
	glsafe(::glPushMatrix());
	glsafe(::glTranslated(m_grabber_box.center().x(), m_grabber_box.center().y(), m_grabber_box.center().z()));
	glsafe(::glRotated(90.0, 0.0, 1.0, 0.0));
	glsafe(::glTranslated(0.0, 0.0, position));

	::gluQuadricOrientation(m_quadric, GLU_OUTSIDE);
	::gluCylinder(m_quadric, 0.9 * radius, 0.0, height, 36, 1);
	::gluQuadricOrientation(m_quadric, GLU_INSIDE);
	::gluDisk(m_quadric, 0.0, 0.9 * radius, 36, 1);
	glsafe(::glPopMatrix());

	glsafe(::glPushMatrix());
	glsafe(::glTranslated(m_grabber_box.center().x(), m_grabber_box.center().y(), m_grabber_box.center().z()));
	glsafe(::glRotated(-90.0, 0.0, 1.0, 0.0));
	glsafe(::glTranslated(0.0, 0.0, position));
	::gluQuadricOrientation(m_quadric, GLU_OUTSIDE);
	::gluCylinder(m_quadric, 0.9 * radius, 0.0, height, 36, 1);
	::gluQuadricOrientation(m_quadric, GLU_INSIDE);
	::gluDisk(m_quadric, 0.0, 0.9 * radius, 36, 1);
	glsafe(::glPopMatrix());

	if (use_lighting)
		glsafe(::glDisable(GL_LIGHTING));
#endif
}

void PartPlate::render_left_arrow(const ColorRGBA render_color, bool use_lighting) const
{
#if 0
	if (m_quadric == nullptr)
		return;
	double radius = m_grabber_box.size().y() * 0.5f;
	double height = radius * 2.0f;
	double position = m_grabber_box.size().x() * 0.8f;
	if (use_lighting)
		glsafe(::glEnable(GL_LIGHTING));

	glsafe(::glColor4fv(render_color));

	glsafe(::glPushMatrix());
	glsafe(::glTranslated(m_grabber_box.center().x(), m_grabber_box.center().y(), m_grabber_box.center().z()));
	glsafe(::glRotated(-90.0, 0.0, 1.0, 0.0));
	glsafe(::glTranslated(0.0, 0.0, position));
	::gluQuadricOrientation(m_quadric, GLU_OUTSIDE);
	::gluCylinder(m_quadric, 0.9 * radius, 0.0, height, 36, 1);
	::gluQuadricOrientation(m_quadric, GLU_INSIDE);
	::gluDisk(m_quadric, 0.0, 0.9 * radius, 36, 1);
	glsafe(::glPopMatrix());

	if (use_lighting)
		glsafe(::glDisable(GL_LIGHTING));
#endif
}
void PartPlate::render_right_arrow(const ColorRGBA render_color, bool use_lighting) const
{
#if 0
	if (m_quadric == nullptr)
		return;
	double radius = m_grabber_box.size().y() * 0.5f;
	double height = radius * 2.0f;
	double position = m_grabber_box.size().x() * 0.8f;
	if (use_lighting)
		glsafe(::glEnable(GL_LIGHTING));

	glsafe(::glColor4fv(render_color));
	glsafe(::glPushMatrix());
	glsafe(::glTranslated(m_grabber_box.center().x(), m_grabber_box.center().y(), m_grabber_box.center().z()));
	glsafe(::glRotated(90.0, 0.0, 1.0, 0.0));
	glsafe(::glTranslated(0.0, 0.0, position));
	::gluQuadricOrientation(m_quadric, GLU_OUTSIDE);
	::gluCylinder(m_quadric, 0.9 * radius, 0.0, height, 36, 1);
	::gluQuadricOrientation(m_quadric, GLU_INSIDE);
	::gluDisk(m_quadric, 0.0, 0.9 * radius, 36, 1);
	glsafe(::glPopMatrix());

	if (use_lighting)
		glsafe(::glDisable(GL_LIGHTING));
#endif
}
*/

static void register_model_for_picking(GLCanvas3D &canvas, PickingModel &model, int id)
{
    canvas.add_raycaster_for_picking(SceneRaycaster::EType::Bed, id, *model.mesh_raycaster, Transform3d::Identity());
}

void PartPlate::register_raycasters_for_picking(GLCanvas3D &canvas)
{
    register_model_for_picking(canvas, m_triangles, picking_id_component(0));
    register_model_for_picking(canvas, m_del_icon, picking_id_component(1));
    register_model_for_picking(canvas, m_orient_icon, picking_id_component(2));
    register_model_for_picking(canvas, m_arrange_icon, picking_id_component(3));
    register_model_for_picking(canvas, m_lock_icon, picking_id_component(4));
    if (m_partplate_list->render_plate_settings)
        register_model_for_picking(canvas, m_plate_settings_icon, picking_id_component(5));

    canvas.remove_raycasters_for_picking(SceneRaycaster::EType::Bed, picking_id_component(6));
    register_model_for_picking(canvas, m_plate_name_edit_icon, picking_id_component(6));
    register_model_for_picking(canvas, m_move_front_icon, picking_id_component(7));
    register_model_for_picking(canvas, m_plate_filament_map_icon, picking_id_component(PLATE_FILAMENT_MAP_ID));
}

int PartPlate::picking_id_component(int idx) const
{
    return this->m_plate_index * GRABBER_COUNT + idx;
}

std::vector<int> PartPlate::get_extruders(bool conside_custom_gcode) const
{
	std::vector<int> plate_extruders;
    if (check_objects_empty_and_gcode3mf(plate_extruders)) {
        return plate_extruders;
    }
	// if 3mf file
	const DynamicPrintConfig& glb_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
	int glb_support_intf_extr = glb_config.opt_int("support_interface_filament");
	int glb_support_extr = glb_config.opt_int("support_filament");
	int glb_wall_extr = glb_config.opt_int("wall_filament");
	int glb_sparse_infill_extr = glb_config.opt_int("sparse_infill_filament");
	int glb_solid_infill_extr = glb_config.opt_int("solid_infill_filament");
	bool glb_support = glb_config.opt_bool("enable_support");
    glb_support |= glb_config.opt_int("raft_layers") > 0;

	for (int obj_idx = 0; obj_idx < m_model->objects.size(); obj_idx++) {
		if (!contain_instance_totally(obj_idx, 0))
			continue;

		ModelObject* mo = m_model->objects[obj_idx];
		for (ModelVolume* mv : mo->volumes) {
			std::vector<int> volume_extruders = mv->get_extruders();
			plate_extruders.insert(plate_extruders.end(), volume_extruders.begin(), volume_extruders.end());
		}

		// layer range
        for (auto layer_range : mo->layer_config_ranges) {
            if (layer_range.second.has("extruder")) {
                if (auto id = layer_range.second.option("extruder")->getInt(); id > 0)
					plate_extruders.push_back(id);
			}
		}

		bool obj_support = false;
		const ConfigOption* obj_support_opt = mo->config.option("enable_support");
        const ConfigOption *obj_raft_opt    = mo->config.option("raft_layers");
		if (obj_support_opt != nullptr || obj_raft_opt != nullptr) {
            if (obj_support_opt != nullptr)
				obj_support = obj_support_opt->getBool();
            if (obj_raft_opt != nullptr)
				obj_support |= obj_raft_opt->getInt() > 0;
        }
		else
			obj_support = glb_support;

        if (obj_support) {
            int                 obj_support_intf_extr = 0;
            const ConfigOption* support_intf_extr_opt = mo->config.option("support_interface_filament");
            if (support_intf_extr_opt != nullptr)
                obj_support_intf_extr = support_intf_extr_opt->getInt();
            if (obj_support_intf_extr != 0)
                plate_extruders.push_back(obj_support_intf_extr);
            else if (glb_support_intf_extr != 0)
                plate_extruders.push_back(glb_support_intf_extr);

            int                 obj_support_extr = 0;
            const ConfigOption* support_extr_opt = mo->config.option("support_filament");
            if (support_extr_opt != nullptr)
                obj_support_extr = support_extr_opt->getInt();
            if (obj_support_extr != 0)
                plate_extruders.push_back(obj_support_extr);
            else if (glb_support_extr != 0)
                plate_extruders.push_back(glb_support_extr);
        }

        int obj_wall_extr = 1;
		const ConfigOption* wall_opt = mo->config.option("wall_filament");
		if (wall_opt != nullptr)
			obj_wall_extr = wall_opt->getInt();
		if (obj_wall_extr != 1)
			plate_extruders.push_back(obj_wall_extr);
		else if (glb_wall_extr != 1)
			plate_extruders.push_back(glb_wall_extr);

		int obj_sparse_infill_extr = 1;
		const ConfigOption* sparse_infill_opt = mo->config.option("sparse_infill_filament");
		if (sparse_infill_opt != nullptr)
			obj_sparse_infill_extr = sparse_infill_opt->getInt();
		if (obj_sparse_infill_extr != 1)
			plate_extruders.push_back(obj_sparse_infill_extr);
		else if (glb_sparse_infill_extr != 1)
			plate_extruders.push_back(glb_sparse_infill_extr);

		int obj_solid_infill_extr = 1;
		const ConfigOption* solid_infill_opt = mo->config.option("solid_infill_filament");
		if (solid_infill_opt != nullptr)
			obj_solid_infill_extr = solid_infill_opt->getInt();
		if (obj_solid_infill_extr != 1)
			plate_extruders.push_back(obj_solid_infill_extr);
		else if (glb_solid_infill_extr != 1)
			plate_extruders.push_back(glb_solid_infill_extr);

	}

	if (conside_custom_gcode) {
		//BBS
        int nums_extruders = 0;
        if (const ConfigOptionStrings *color_option = dynamic_cast<const ConfigOptionStrings *>(wxGetApp().preset_bundle->project_config.option("filament_colour"))) {
            nums_extruders = color_option->values.size();
			if (m_model->plates_custom_gcodes.find(m_plate_index) != m_model->plates_custom_gcodes.end()) {
				for (auto item : m_model->plates_custom_gcodes.at(m_plate_index).gcodes) {
					if (item.type == CustomGCode::Type::ToolChange && item.extruder <= nums_extruders)
						plate_extruders.push_back(item.extruder);
				}
			}
		}
	}

	std::sort(plate_extruders.begin(), plate_extruders.end());
	auto it_end = std::unique(plate_extruders.begin(), plate_extruders.end());
	plate_extruders.resize(std::distance(plate_extruders.begin(), it_end));
	return plate_extruders;
}

std::vector<int> PartPlate::get_extruders_under_cli(bool conside_custom_gcode, DynamicPrintConfig& full_config) const
{
    std::vector<int> plate_extruders;

    // if 3mf file
    int glb_support_intf_extr = full_config.opt_int("support_interface_filament");
    int glb_support_extr = full_config.opt_int("support_filament");
	int glb_wall_extr = full_config.opt_int("wall_filament");
	int glb_sparse_infill_extr = full_config.opt_int("sparse_infill_filament");
	int glb_solid_infill_extr = full_config.opt_int("solid_infill_filament");

    bool glb_support = full_config.opt_bool("enable_support");
    glb_support |= full_config.opt_int("raft_layers") > 0;

    for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it)
    {
        int obj_id = it->first;
        int instance_id = it->second;

        if ((obj_id >= 0) && (obj_id < m_model->objects.size()))
        {
            ModelObject* object = m_model->objects[obj_id];
            ModelInstance* instance = object->instances[instance_id];

            if (!instance->printable)
                continue;

            for (ModelVolume* mv : object->volumes) {
                std::vector<int> volume_extruders = mv->get_extruders();
                plate_extruders.insert(plate_extruders.end(), volume_extruders.begin(), volume_extruders.end());
            }

            // layer range
            for (auto layer_range : object->layer_config_ranges) {
                if (layer_range.second.has("extruder")) {
                    if (auto id = layer_range.second.option("extruder")->getInt(); id > 0)
                        plate_extruders.push_back(id);
                }
            }

            bool obj_support = false;
            const ConfigOption* obj_support_opt = object->config.option("enable_support");
            const ConfigOption *obj_raft_opt    = object->config.option("raft_layers");
            if (obj_support_opt != nullptr || obj_raft_opt != nullptr) {
                if (obj_support_opt != nullptr)
                    obj_support = obj_support_opt->getBool();
                if (obj_raft_opt != nullptr)
                    obj_support |= obj_raft_opt->getInt() > 0;
            }
            else
                obj_support = glb_support;

            if (!obj_support)
                continue;

            int obj_support_intf_extr = 0;
            const ConfigOption* support_intf_extr_opt = object->config.option("support_interface_filament");
            if (support_intf_extr_opt != nullptr)
                obj_support_intf_extr = support_intf_extr_opt->getInt();
            if (obj_support_intf_extr != 0)
                plate_extruders.push_back(obj_support_intf_extr);
            else if (glb_support_intf_extr != 0)
                plate_extruders.push_back(glb_support_intf_extr);

            int obj_support_extr = 0;
            const ConfigOption* support_extr_opt = object->config.option("support_filament");
            if (support_extr_opt != nullptr)
                obj_support_extr = support_extr_opt->getInt();
            if (obj_support_extr != 0)
                plate_extruders.push_back(obj_support_extr);
            else if (glb_support_extr != 0)
                plate_extruders.push_back(glb_support_extr);

			int obj_wall_extr = 1;
			const ConfigOption* wall_opt = object->config.option("wall_filament");
			if (wall_opt != nullptr)
				obj_wall_extr = wall_opt->getInt();
			if (obj_wall_extr != 1)
				plate_extruders.push_back(obj_wall_extr);
			else if (glb_wall_extr != 1)
				plate_extruders.push_back(glb_wall_extr);

			int obj_sparse_infill_extr = 1;
			const ConfigOption* sparse_infill_opt = object->config.option("sparse_infill_filament");
			if (sparse_infill_opt != nullptr)
				obj_sparse_infill_extr = sparse_infill_opt->getInt();
			if (obj_sparse_infill_extr != 1)
				plate_extruders.push_back(obj_sparse_infill_extr);
			else if (glb_sparse_infill_extr != 1)
				plate_extruders.push_back(glb_sparse_infill_extr);

			int obj_solid_infill_extr = 1;
			const ConfigOption* solid_infill_opt = object->config.option("solid_infill_filament");
			if (solid_infill_opt != nullptr)
				obj_solid_infill_extr = solid_infill_opt->getInt();
			if (obj_solid_infill_extr != 1)
				plate_extruders.push_back(obj_solid_infill_extr);
			else if (glb_solid_infill_extr != 1)
				plate_extruders.push_back(glb_solid_infill_extr);
        }
    }

    if (conside_custom_gcode) {
        //BBS
        int nums_extruders = 0;
        if (const ConfigOptionStrings *color_option = dynamic_cast<const ConfigOptionStrings *>(full_config.option("filament_colour"))) {
            nums_extruders = color_option->values.size();
            if (m_model->plates_custom_gcodes.find(m_plate_index) != m_model->plates_custom_gcodes.end()) {
                for (auto item : m_model->plates_custom_gcodes.at(m_plate_index).gcodes) {
                    if (item.type == CustomGCode::Type::ToolChange && item.extruder <= nums_extruders)
                        plate_extruders.push_back(item.extruder);
                }
            }
        }
    }

    std::sort(plate_extruders.begin(), plate_extruders.end());
    auto it_end = std::unique(plate_extruders.begin(), plate_extruders.end());
    plate_extruders.resize(std::distance(plate_extruders.begin(), it_end));
    return plate_extruders;
}

bool PartPlate::check_objects_empty_and_gcode3mf(std::vector<int> &result) const
{
    if (m_model->objects.empty()) {//objects is empty
        if (wxGetApp().plater()->is_gcode_3mf()) { // if gcode.3mf file
            for (int i = 0; i < slice_filaments_info.size(); i++) {
                result.push_back(slice_filaments_info[i].id + 1);
            }
        }
        return true;
    }
    return false;
}

std::vector<int> PartPlate::get_extruders_without_support(bool conside_custom_gcode) const
{
	std::vector<int> plate_extruders;
    if (check_objects_empty_and_gcode3mf(plate_extruders)) {
        return plate_extruders;
    }
	// if 3mf file
	const DynamicPrintConfig& glb_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;

	for (int obj_idx = 0; obj_idx < m_model->objects.size(); obj_idx++) {
		if (!contain_instance_totally(obj_idx, 0))
			continue;

		ModelObject* mo = m_model->objects[obj_idx];
		for (ModelVolume* mv : mo->volumes) {
			std::vector<int> volume_extruders = mv->get_extruders();
			plate_extruders.insert(plate_extruders.end(), volume_extruders.begin(), volume_extruders.end());
		}
	}

	if (conside_custom_gcode) {
		//BBS
		int nums_extruders = 0;
		if (const ConfigOptionStrings* color_option = dynamic_cast<const ConfigOptionStrings*>(wxGetApp().preset_bundle->project_config.option("filament_colour"))) {
			nums_extruders = color_option->values.size();
			if (m_model->plates_custom_gcodes.find(m_plate_index) != m_model->plates_custom_gcodes.end()) {
				for (auto item : m_model->plates_custom_gcodes.at(m_plate_index).gcodes) {
					if (item.type == CustomGCode::Type::ToolChange && item.extruder <= nums_extruders)
						plate_extruders.push_back(item.extruder);
				}
			}
		}
	}

	std::sort(plate_extruders.begin(), plate_extruders.end());
	auto it_end = std::unique(plate_extruders.begin(), plate_extruders.end());
	plate_extruders.resize(std::distance(plate_extruders.begin(), it_end));
	return plate_extruders;
}

/* -1 is invalid, return physical extruder idx*/

/* machine has 1 extruder*/
/* logical extruder: 1-unique*/
/* physical extruder: 0-unique*/

/* machine have 2 extruders*/
/* logical extruder: 1-left, 2-right*/
/* physical extruder: 0-right, 1-left*/
int PartPlate::get_physical_extruder_by_filament_id(const DynamicConfig& g_config, int idx) const
{
	const std::vector<int>& filament_map = get_real_filament_maps(g_config);
	if (filament_map.size() < idx)
	{
		return -1;
	}

	const auto the_map = g_config.option<ConfigOptionInts>("physical_extruder_map");
	if (!the_map)
	{
		return -1;
	}

	int zero_base_logical_idx = filament_map[idx - 1] - 1;
	return the_map->values[zero_base_logical_idx];
}

std::vector<int> PartPlate::get_used_filaments()
{
	std::vector<int> used_filaments;
    if (check_objects_empty_and_gcode3mf(used_filaments)) {
        return used_filaments;
    }

	GCodeProcessorResult* result = get_slice_result();
	if (!result)
		return used_filaments;

	std::set<int> used_extruders_set;
	PrintEstimatedStatistics& ps = result->print_statistics;
	for (const auto& item : ps.total_volumes_per_extruder)
		used_extruders_set.emplace(item.first + 1);

	return std::vector(used_extruders_set.begin(), used_extruders_set.end());
}

bool PartPlate::check_filament_printable(const DynamicPrintConfig &config, wxString& error_message)
{
    error_message.clear();
    FilamentMapMode mode = this->get_real_filament_map_mode(config);
    // only check printablity if we have explicit map result
    if (mode != fmmManual)
        return true;

    std::vector<int> used_filaments = get_extruders(true);  // 1 base
    if (!used_filaments.empty()) {
        for (auto filament_idx : used_filaments) {
            int filament_id = filament_idx - 1;
            std::string filament_type = config.option<ConfigOptionStrings>("filament_type")->values.at(filament_id);
            int filament_printable_status = config.option<ConfigOptionInts>("filament_printable")->values.at(filament_id);
            std::vector<int> filament_map  = get_real_filament_maps(config);
            int extruder_idx = filament_map[filament_id] - 1;
            if (!(filament_printable_status >> extruder_idx & 1)) {
                wxString extruder_name = extruder_idx == 0 ? _L("left") : _L("right");
                error_message  = wxString::Format(_L("The %s nozzle can not print %s."), extruder_name, filament_type);
                return false;
            }
        }
    }
    return true;
}

bool PartPlate::check_tpu_printable_status(const DynamicPrintConfig & config, const std::vector<int> &tpu_filaments)
{
	// do not limit the num of tpu filament in slicing
	return true;
}

bool PartPlate::check_mixture_of_pla_and_petg(const DynamicPrintConfig &config)
{
    bool has_pla = false;
    bool has_petg = false;

    std::vector<int> used_filaments = get_extruders(true); // 1 base
    if (!used_filaments.empty()) {
        for (auto filament_idx : used_filaments) {
            int                 filament_id        = filament_idx - 1;
            if (filament_id < config.option<ConfigOptionStrings>("filament_type")->values.size()) {
                std::string filament_type = config.option<ConfigOptionStrings>("filament_type")->values.at(filament_id);
                if (filament_type == "PLA")
                    has_pla = true;
                if (filament_type == "PETG")
                    has_petg = true;
            } else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " check error:array bound";
            }
        }
    }

    if (has_pla && has_petg)
        return false;

    return true;
}

bool PartPlate::check_mixture_filament_compatible(const DynamicPrintConfig &config, std::string &error_msg)
{
    static std::unordered_map<std::string, std::unordered_set<std::string>> incompatible_filament_pairs;

    auto add_incompatibility = [&](std::string filament_type1, std::string filament_type2) {
        incompatible_filament_pairs[filament_type1].insert(filament_type2);
        incompatible_filament_pairs[filament_type2].insert(filament_type1);
    };

    if (incompatible_filament_pairs.empty()) { add_incompatibility("PVA", "PETG"); }

    std::vector<int>         used_filaments = get_extruders(true); // 1 based idx
    std::vector<std::string> filament_types;
    auto                     filament_type_opt = config.option<ConfigOptionStrings>("filament_type");
    for (auto filament : used_filaments) {
        int filament_idx = filament - 1;
        if (filament_idx >= filament_type_opt->values.size()) filament_idx = 0;
        filament_types.push_back(filament_type_opt->values[filament_idx]);
    };

    {
        std::unordered_set<std::string> seen;
        filament_types.erase(std::remove_if(filament_types.begin(), filament_types.end(), [&](const std::string &s) { return !seen.insert(s).second; }), filament_types.end());
    }

    std::vector<std::pair<std::string, std::string>> conflicts;

    for (size_t i = 0; i < filament_types.size(); i++) {
        auto it = incompatible_filament_pairs.find(filament_types[i]);
        if (it == incompatible_filament_pairs.end()) continue;
        for (size_t j = i + 1; j < filament_types.size(); ++j) {
            if (it->second.count(filament_types[j])) { conflicts.emplace_back(filament_types[i], filament_types[j]); }
        }
    }

    if (!conflicts.empty()) {
        // TODO: add the full text if has multi conflict
        auto conflict = conflicts.front();
        error_msg     = GUI::format(_L("Mixing %1% with %2% in printing is not recommended.\n"), conflict.first, conflict.second);
    }
    return conflicts.empty();
}

bool PartPlate::check_compatible_of_nozzle_and_filament(const DynamicPrintConfig &config, const std::vector<std::string> &filament_presets, std::string &error_msg)
{
    float nozzle_diameter = config.option<ConfigOptionFloats>("nozzle_diameter")->values[0];
    auto  volume_type_opt = config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");

    auto get_filament_alias = [](std::string preset_name) -> std::string {
        size_t      at_pos = preset_name.find('@');
        std::string alias  = preset_name.substr(0, at_pos);
        size_t      first  = alias.find_first_not_of(' ');
        if (first == std::string::npos) return "";
        size_t last = alias.find_last_not_of(' ');
        return alias.substr(first, last - first + 1);
    };

    bool with_same_volume_type = std::all_of(volume_type_opt->values.begin(), volume_type_opt->values.end(),
                                             [first_value = volume_type_opt->values[0]](int value) { return value == first_value; });

    std::set<std::string> selected_filament_alias;
    for (auto &filament_preset : filament_presets) { selected_filament_alias.insert(get_filament_alias(filament_preset)); }

    auto get_incompatible_selected = [&](const NozzleVolumeType volume_type) -> std::set<std::string> {
        std::vector<std::string> incompatible_filaments = Print::get_incompatible_filaments_by_nozzle(nozzle_diameter, volume_type);
        std::set<std::string>    ret;
        for (auto &filament : selected_filament_alias) {
            if (std::find(incompatible_filaments.begin(), incompatible_filaments.end(), filament) != incompatible_filaments.end()) ret.insert(filament);
        }
        return ret;
    };

    auto get_nozzle_msg = [](const float nozzle_diameter, const NozzleVolumeType volume_type) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << nozzle_diameter;
        std::string nozzle_msg = oss.str();
        ((nozzle_msg += "mm ") += _u8L(get_nozzle_volume_type_string(volume_type))) += _u8L(" nozzle");
        return nozzle_msg;
    };

    auto get_incompatible_filament_msg = [](const std::set<std::string> &incompatible_selected_filaments) -> std::string {
        std::string filament_str;
        size_t      idx = 0;
        for (const auto &filament : incompatible_selected_filaments) {
            if (idx > 0) filament_str += ',';
            filament_str += filament;
            ++idx;
        }
        return filament_str;
    };

    error_msg.clear();

    std::set<int>                                     nozzle_volumes(volume_type_opt->values.begin(), volume_type_opt->values.end());
    std::map<NozzleVolumeType, std::set<std::string>> incompatible_selected_map;

    for (auto volume_type_value : nozzle_volumes) {
        NozzleVolumeType volume_type           = static_cast<NozzleVolumeType>(volume_type_value);
        auto             incompatible_selected = get_incompatible_selected(volume_type);
        if (!incompatible_selected.empty()) incompatible_selected_map[volume_type] = incompatible_selected;
    }

    if (incompatible_selected_map.empty()) return true;

    if (incompatible_selected_map.size() == 1) {
        auto             elem                  = incompatible_selected_map.begin();
        NozzleVolumeType volume_type           = elem->first;
        auto             incompatible_selected = elem->second;
        error_msg = GUI::format(_L("It is not recommended to print the following filament(s) with %1%: %2%\n"), get_nozzle_msg(nozzle_diameter, volume_type),
                                get_incompatible_filament_msg(incompatible_selected));
    } else {
        std::string warning_msg = _u8L("It is not recommended to use the following nozzle and filament combinations:\n");
        for (auto &elem : incompatible_selected_map) {
            NozzleVolumeType volume_type           = elem.first;
            auto             incompatible_selected = elem.second;
            warning_msg += GUI::format(_L("%1% with %2%\n"),get_nozzle_msg(nozzle_diameter, volume_type), get_incompatible_filament_msg(incompatible_selected));
        }
        error_msg = warning_msg;
    }
    return false;
}

/*Vec3d PartPlate::calculate_wipe_tower_size(const DynamicPrintConfig &config, const double w, const double wipe_volume, int plate_extruder_size, bool use_global_objects) const
{
    Vec3d  wipe_tower_size;
    double layer_height = 0.08f; // hard code layer height
    double max_height   = 0.f;
    wipe_tower_size.setZero();

    const ConfigOption *layer_height_opt = config.option("layer_height");
    if (layer_height_opt)
        layer_height = layer_height_opt->getFloat();

    std::vector<int> plate_extruders = get_extruders(true);
	plate_extruder_size = plate_extruders.size();
    if (plate_extruder_size == 0)
        return wipe_tower_size;

    for (int obj_idx = 0; obj_idx < m_model->objects.size(); obj_idx++) {
        if (!use_global_objects && !contain_instance_totally(obj_idx, 0))
            continue;

        BoundingBoxf3 bbox = m_model->objects[obj_idx]->bounding_box();
        max_height         = std::max(bbox.size().z(), max_height);
    }
    wipe_tower_size(2) = max_height;

    auto timelapse_type    = config.option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
    bool timelapse_enabled = timelapse_type ? (timelapse_type->value == TimelapseType::tlSmooth) : false;
    int nozzle_nums = wxGetApp().preset_bundle->get_printer_extruder_count();
    double extra_spacing     = config.option("prime_tower_infill_gap")->getFloat() / 100.;
    double depth             = std::sqrt(wipe_volume * (nozzle_nums == 2 ? plate_extruder_size : (plate_extruder_size - 1)) / layer_height * extra_spacing);
    if (timelapse_enabled || plate_extruder_size > 1) {
        float min_wipe_tower_depth = WipeTower::get_limit_depth_by_height(max_height);
        depth = std::max((double) min_wipe_tower_depth, depth);
        wipe_tower_size(0) = wipe_tower_size(1) = depth;
    }

    return wipe_tower_size;
}*/

Vec3d PartPlate::estimate_wipe_tower_size(const DynamicPrintConfig & config, const double w, const double wipe_volume, int extruder_count, int plate_extruder_size, bool use_global_objects, bool enable_wrapping_detection) const
{
    Vec3d wipe_tower_size;
    double layer_height = 0.08f; // hard code layer height
    double max_height = 0.f;
    wipe_tower_size.setZero();

    const ConfigOption* layer_height_opt = config.option("layer_height");
    if (layer_height_opt)
        layer_height = layer_height_opt->getFloat();

    // empty plate
    if (plate_extruder_size == 0)
    {
        std::vector<int> plate_extruders = get_extruders(true);
        plate_extruder_size = plate_extruders.size();
    }
    if (plate_extruder_size == 0)
        return wipe_tower_size;

    for (int obj_idx = 0; obj_idx < m_model->objects.size(); obj_idx++) {
        if (!use_global_objects && !contain_instance_totally(obj_idx, 0))
            continue;

		BoundingBoxf3 bbox = m_model->objects[obj_idx]->bounding_box_exact();
        max_height = std::max(bbox.size().z(), max_height);
    }
    wipe_tower_size(2) = max_height;
    //const DynamicPrintConfig &dconfig = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto timelapse_type    = config.option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
    bool need_wipe_tower = (timelapse_type ? (timelapse_type->value == TimelapseType::tlSmooth) : false) | enable_wrapping_detection;
    double extra_spacing     = config.option("prime_tower_infill_gap")->getFloat() / 100.;
    const ConfigOptionEnum<WipeTowerWallType>* use_rib_wall_opt = config.option<ConfigOptionEnum<WipeTowerWallType>>("wipe_tower_wall_type");
    bool use_rib_wall = use_rib_wall_opt ? use_rib_wall_opt->value == WipeTowerWallType::wtwRib: false;
    double rib_width = config.option("wipe_tower_rib_width")->getFloat();
    double depth;
    double filament_change_volume=0.;
    {
        std::vector<double>             filament_change_lengths;
        auto                filament_change_lengths_opt = m_print->config().option<ConfigOptionFloats>("filament_change_length");
        if (filament_change_lengths_opt) filament_change_lengths = filament_change_lengths_opt->values;
        double length = filament_change_lengths.empty() ? 0 : *std::max_element(filament_change_lengths.begin(), filament_change_lengths.end());
        double diameter = 1.75;
        std::vector<double> diameters;
        auto                filament_diameter_opt = m_print->config().option<ConfigOptionFloats>("filament_diameter");
        if (filament_diameter_opt) diameters = filament_diameter_opt->values;
        diameter = diameters.empty() ? diameter : *std::max_element(diameters.begin(), diameters.end());
        filament_change_volume = length * PI * diameter * diameter / 4.;
    }
    double volume = wipe_volume * (extruder_count == 2 ? plate_extruder_size : (plate_extruder_size - 1));
    if (extruder_count == 2) volume += filament_change_volume * (int) (plate_extruder_size / 2);
    if (use_rib_wall) {
        depth = std::sqrt(volume / layer_height * extra_spacing);
        if (need_wipe_tower || plate_extruder_size > 1) {
            float min_wipe_tower_depth = WipeTower::get_limit_depth_by_height(max_height);
            double volume_depth         = depth;
            depth = std::max((double) min_wipe_tower_depth, depth);
            rib_width = std::min(rib_width, depth / 2);
            depth = rib_width / std::sqrt(2) + std::max(depth + m_print->config().wipe_tower_extra_rib_length.value, volume_depth);
            wipe_tower_size(0) = wipe_tower_size(1) = depth;
        }
    }
    else {
        depth  =  volume/ (layer_height * w) *extra_spacing;
        if (need_wipe_tower || depth > EPSILON) {
            float min_wipe_tower_depth = WipeTower::get_limit_depth_by_height(max_height);
            depth = std::max((double)min_wipe_tower_depth, depth);
        }
        wipe_tower_size(0) = w;
        wipe_tower_size(1) = depth;
    }

    return wipe_tower_size;
}

arrangement::ArrangePolygon PartPlate::estimate_wipe_tower_polygon(const DynamicPrintConfig& config, int plate_index, Vec3d& wt_pos, Vec3d& wt_size, int extruder_count, int plate_extruder_size, bool use_global_objects) const
{
	float x = dynamic_cast<const ConfigOptionFloats*>(config.option("wipe_tower_x"))->get_at(plate_index);
	float y = dynamic_cast<const ConfigOptionFloats*>(config.option("wipe_tower_y"))->get_at(plate_index);
	float w = dynamic_cast<const ConfigOptionFloat*>(config.option("prime_tower_width"))->value;
	//float a = dynamic_cast<const ConfigOptionFloat*>(config.option("wipe_tower_rotation_angle"))->value;
	float v = dynamic_cast<const ConfigOptionFloat*>(config.option("prime_volume"))->value;
    float tower_brim_width = dynamic_cast<const ConfigOptionFloat*>(config.option("prime_tower_brim_width"))->value;
    const ConfigOptionBool * wrapping_opt = dynamic_cast<const ConfigOptionBool *>(config.option("enable_wrapping_detection"));
	bool enable_wrapping = (wrapping_opt != nullptr) && wrapping_opt->value;
	wt_size = estimate_wipe_tower_size(config, w, v, extruder_count, plate_extruder_size, use_global_objects, enable_wrapping);
	int plate_width=m_width, plate_depth=m_depth;
	float depth = wt_size(1);
	float margin = WIPE_TOWER_MARGIN + tower_brim_width, wp_brim_width = 0.f;
	const ConfigOption* wipe_tower_brim_width_opt = config.option("prime_tower_brim_width");
	if (wipe_tower_brim_width_opt) {
		wp_brim_width = wipe_tower_brim_width_opt->getFloat();
        if (wp_brim_width < 0) wp_brim_width = WipeTower::get_auto_brim_by_height((float) wt_size.z());
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("arrange wipe_tower: wp_brim_width %1%") % wp_brim_width;
	}

	x = std::clamp(x, margin, (float)plate_width - w - margin - wp_brim_width);
    y = std::clamp(y, margin, (float)plate_depth - depth - margin - wp_brim_width);
    wt_pos(0) = x;
    wt_pos(1) = y;
    wt_pos(2) = 0.f;

	arrangement::ArrangePolygon wipe_tower_ap;
	Polygon ap({
		{scaled(x - wp_brim_width), scaled(y - wp_brim_width)},
		{scaled(x + w + wp_brim_width), scaled(y - wp_brim_width)},
		{scaled(x + w + wp_brim_width), scaled(y + depth + wp_brim_width)},
		{scaled(x - wp_brim_width), scaled(y + depth + wp_brim_width)}
		});
	wipe_tower_ap.bed_idx = plate_index;
	wipe_tower_ap.setter = NULL; // do not move wipe tower

	wipe_tower_ap.poly.contour = std::move(ap);
	wipe_tower_ap.translation = { scaled(0.f), scaled(0.f) };
	//wipe_tower_ap.rotation = a;
	wipe_tower_ap.name = "WipeTower";
	wipe_tower_ap.is_virt_object = true;
	wipe_tower_ap.is_wipe_tower = true;

	return wipe_tower_ap;
}

bool PartPlate::operator<(PartPlate& plate) const
{
	int index = plate.get_index();
	return (this->m_plate_index < index);
}

//set the plate's index
void PartPlate::set_index(int index)
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": plate_id update from %1% to %2%") % m_plate_index % index;

	m_plate_index = index;
	if (m_print != nullptr)
		m_print->set_plate_index(index);
}

void PartPlate::clear(bool clear_sliced_result)
{
	obj_to_instance_set.clear();
	instance_outside_set.clear();
	if (clear_sliced_result) {
		m_ready_for_slice = true;
		update_slice_result_valid_state(false);
	}
	m_name_texture.reset();
	return;
}

/* size and position related functions*/
//set position and size
void PartPlate::set_pos_and_size(Vec3d& origin, int width, int depth, int height, bool with_instance_move, bool do_clear)
{
	bool size_changed = false; //size changed means the machine changed
	bool pos_changed = false;

	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate_id %1%, before, origin {%2%,%3%,%4%}, plate_width %5%, plate_depth %6%, plate_height %7%")\
		% m_plate_index % m_origin.x() % m_origin.y() % m_origin.z() % m_width % m_depth % m_height;
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": with_instance_move %1%, after, origin {%2%,%3%,%4%}, plate_width %5%, plate_depth %6%, plate_height %7%")\
		% with_instance_move % origin.x() % origin.y() % origin.z() % width % depth % height;
	size_changed = ((width != m_width) || (depth != m_depth) || (height != m_height));
	pos_changed = (m_origin != origin);

	if ((!size_changed) && (!pos_changed))
	{
		//size and position the same with before, just return
		return;
	}

	if (with_instance_move && m_model)
	{
		for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it) {
			int obj_id = it->first;
			int instance_id = it->second;
			ModelObject* object = m_model->objects[obj_id];
			ModelInstance* instance = object->instances[instance_id];

			//move this instance into the new plate's same position
			Vec3d offset = instance->get_transformation().get_offset();
			int off_x, off_y;

			if (size_changed)
			{
				//change position due to the bed size changes
				//off_x = (width - m_width) * m_plate_index + (width - m_width) / 2;
				//off_y = (depth - m_depth) * m_plate_index + (depth - m_depth) / 2;
				off_x = origin.x() - m_origin.x() + (width - m_width) / 2;
				off_y = origin.y() - m_origin.y() + (depth - m_depth) / 2;
			}
			else
			{
				//change position due to the plate moves
				off_x = origin.x() - m_origin.x();
				off_y = origin.y() - m_origin.y();
			}
			offset.x() = offset.x() + off_x;
			offset.y() = offset.y() + off_y;

			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": object %1%, instance %2%, moved {%3%,%4%} to {%5%, %6%}")\
				% obj_id % instance_id % off_x % off_y % offset.x() % offset.y();

			instance->set_offset(offset);
			object->invalidate_bounding_box();
		}
	}
	else if (do_clear)
	{
		clear();
	}

    if (m_print)
        m_print->set_plate_origin(origin);

	m_origin = origin;
	m_width = width;
	m_depth = depth;
	m_height = height;

	return;
}

//get the plate's center point origin
Vec3d PartPlate::get_center_origin()
{
	Vec3d origin;

	origin(0) = (m_bounding_box.min(0) + m_bounding_box.max(0)) / 2;//m_origin.x() + m_width / 2;
	origin(1) = (m_bounding_box.min(1) + m_bounding_box.max(1)) / 2; //m_origin.y() + m_depth / 2;
	origin(2) = m_origin.z();

	return origin;
}

void PartPlate::generate_plate_name_texture()
{
    m_plate_name_icon.reset();

	// generate m_name_texture texture from m_name with generate_from_text_string
	m_name_texture.reset();
	auto text = m_name.empty()? _L("Untitled") : from_u8(m_name);

    // ORCA also scale font size to prevent low res texture
    int size = wxGetApp().em_unit() * PARTPLATE_EDIT_PLATE_NAME_ICON_SIZE;
    auto l = Label::sysFont(size, true);
    wxFont* font = &l;

	wxColour foreground(0xf2, 0x75, 0x4e, 0xff);
    if (!m_name_texture.generate_from_text_string(text.ToUTF8().data(), *font, *wxBLACK, foreground))
		BOOST_LOG_TRIVIAL(error) << "PartPlate::generate_plate_name_texture(): generate_from_text_string() failed";

    ExPolygon poly;
    auto  bed_ext  = get_extents(m_shape);
    Vec2d p        = bed_ext[3];
    float factor   = bed_ext.size()(1) / 200.0;
    float icon_sz  = factor * PARTPLATE_EDIT_PLATE_NAME_ICON_SIZE;
    float width    = icon_sz * m_name_texture.get_width() / m_name_texture.get_height(); // icon size * text_bb_ratio
    float height   = icon_sz; // scale with icon size to preserve ratio while system scaling
    float offset_y = factor * PARTPLATE_TEXT_OFFSET_Y;

    //if (m_plater && m_plater->get_build_volume_type() == BuildVolume_Type::Circle)
    //    px = scale_(bed_ext.center()(0)) - (width + height) / 2.00;

    p += Vec2d(0, offset_y);

    poly.contour.append({ scale_(p(0)        ), scale_(p(1)         ) });
    poly.contour.append({ scale_(p(0) + width), scale_(p(1)         ) });
    poly.contour.append({ scale_(p(0) + width), scale_(p(1) + height) });
    poly.contour.append({ scale_(p(0)        ), scale_(p(1) + height) });

    if (!init_model_from_poly(m_plate_name_icon, poly, GROUND_Z))
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "Unable to generate geometry buffers for icons\n";

	auto canvas = this->m_partplate_list->m_plater->get_view3D_canvas3D();
    canvas->remove_raycasters_for_picking(SceneRaycaster::EType::Bed, picking_id_component(6));
    calc_vertex_for_plate_name_edit_icon(&m_name_texture, 0, m_plate_name_edit_icon);
    register_model_for_picking(*canvas, m_plate_name_edit_icon, picking_id_component(6));
}
void PartPlate::set_plate_name(const std::string& name) 
{ 
	// compare if name equal to m_name, case sensitive
    if (boost::equals(m_name, name))
        return;

	m_name = name;
    if (m_print != nullptr)
        m_print->set_plate_name(name);

	generate_plate_name_texture();
}

//get the print's object, result and index
void PartPlate::get_print(PrintBase** print, GCodeResult** result, int* index)
{
	if (print && (printer_technology == PrinterTechnology::ptFFF))
		*print = m_print;

	if (result)
		*result = m_gcode_result;

	if (index)
		*index = m_print_index;

	return;
}

//set the print object, result and it's index
void PartPlate::set_print(PrintBase* print, GCodeResult* result, int index)
{
	if (printer_technology == PrinterTechnology::ptFFF)
		m_print = static_cast<Print*>(print);
	//todo, for other printers

	m_gcode_result = result;
	if (index >= 0)
		m_print_index = index;

	m_print->set_plate_origin(m_origin);

	return;
}

std::string PartPlate::get_gcode_filename()
{
	if (is_slice_result_valid() && get_slice_result()) {
		return m_gcode_result->filename;
	}
	return "";
}

bool PartPlate::is_valid_gcode_file()
{
	if (get_gcode_filename().empty())
		return false;
	boost::filesystem::path gcode_file(m_gcode_result->filename);
	if (!boost::filesystem::exists(gcode_file)) {
		BOOST_LOG_TRIVIAL(info) << "invalid gcode file, file is missing, file = " << m_gcode_result->filename;
		return false;
	}
	return true;
}

ModelObjectPtrs PartPlate::get_objects_on_this_plate() {
    ModelObjectPtrs objects_ptr;
    int obj_id;
    for (auto it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); it++) {
        obj_id = it->first;
        objects_ptr.push_back(m_model->objects[obj_id]);
    }
    return objects_ptr;
}

ModelInstance* PartPlate::get_instance(int obj_id, int instance_id)
{
	if (!contain_instance(obj_id, instance_id))
		return nullptr;
	else
		return m_model->objects[obj_id]->instances[instance_id];
}

/* instance related operations*/
//judge whether instance is bound in plate or not
bool PartPlate::contain_instance(int obj_id, int instance_id)
{
	bool result = false;
	std::set<std::pair<int, int>>::iterator it;

	it = obj_to_instance_set.find(std::pair(obj_id, instance_id));
	if (it != obj_to_instance_set.end()) {
		result = true;
	}

	return result;
}

//judge whether instance is bound in plate or not
bool PartPlate::contain_instance_totally(ModelObject* object, int instance_id) const
{
	bool result = false;
	int obj_id = -1;

	for (int index = 0; index < m_model->objects.size(); index ++)
	{
		if (m_model->objects[index] == object)
		{
			obj_id = index;
			break;
		}
	}

	if ((obj_id >= 0 ) && (obj_id < m_model->objects.size()))
		result = contain_instance_totally(obj_id, instance_id);

	return result;
}

//judge whether instance is totally included in plate or not
bool PartPlate::contain_instance_totally(int obj_id, int instance_id) const
{
	bool result = false;
	std::set<std::pair<int, int>>::iterator it;

	it = obj_to_instance_set.find(std::pair(obj_id, instance_id));
	if (it != obj_to_instance_set.end()) {
		it = instance_outside_set.find(std::pair(obj_id, instance_id));
		if (it == instance_outside_set.end())
			result = true;
	}

	return result;
}

//check whether instance is outside the plate or not
bool PartPlate::check_outside(int obj_id, int instance_id, BoundingBoxf3* bounding_box)
{
	bool outside = true;

	ModelObject* object = m_model->objects[obj_id];
	ModelInstance* instance = object->instances[instance_id];

	BoundingBoxf3 instance_box = bounding_box? *bounding_box: object->instance_convex_hull_bounding_box(instance_id);
	Polygon hull = instance->convex_hull_2d();
	BoundingBoxf3 plate_box = get_plate_box();
	if (instance_box.max.z() > plate_box.min.z())
		plate_box.min.z() += instance_box.min.z(); // not considering outsize if sinking

	if (instance_box.min.z() < SINKING_Z_THRESHOLD) {
		// Orca: For sinking object, we use a more expensive algorithm so part below build plate won't be considered
		if (plate_box.intersects(instance_box)) {
			// TODO: FIXME: this does not take exclusion area into account
            const BuildVolume build_volume(get_shape(), m_plater->build_volume().printable_height(), m_extruder_areas, m_extruder_heights);
			const auto state = instance->calc_print_volume_state(build_volume);
			outside = state == ModelInstancePVS_Partly_Outside;
		}
	}
	else
	if (plate_box.contains(instance_box))
	{
		if (m_exclude_bounding_box.size() > 0)
		{
			Polygon hull = instance->convex_hull_2d();
			int index;
			for (index = 0; index < m_exclude_bounding_box.size(); index ++)
			{
				Polygon p = m_exclude_bounding_box[index].polygon(true);  // instance convex hull is scaled, so we need to scale here
				if (intersection({ p }, { hull }).empty() == false)
				//if (m_exclude_bounding_box[index].intersects(instance_box))
				{
					break;
				}
			}
			if (index >= m_exclude_bounding_box.size())
				outside = false;
		}
		else
			outside = false;
	}

	return outside;
}

//judge whether instance is intesected with plate or not
bool PartPlate::intersect_instance(int obj_id, int instance_id, BoundingBoxf3* bounding_box)
{
	bool result = false;

	if (!valid_instance(obj_id, instance_id))
	{
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": plate_id %1%, invalid obj_id %2%, instance_id %3%") % m_plate_index % obj_id % instance_id;
		return false;
	}

	if (m_printable)
	{
		ModelObject* object = m_model->objects[obj_id];
		ModelInstance* instance = object->instances[instance_id];
		BoundingBoxf3 instance_box = bounding_box? *bounding_box: object->instance_convex_hull_bounding_box(instance_id);
		result = get_plate_box().intersects(instance_box);
	}
	else
	{
		result = is_left_top_of(obj_id, instance_id);
	}

	return result;
}

//judge whether the plate's origin is at the left of instance or not
bool PartPlate::is_left_top_of(int obj_id, int instance_id)
{
	bool result = false;

	if (!valid_instance(obj_id, instance_id))
	{
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": plate_id %1%, invalid obj_id %2%, instance_id %3%") % m_plate_index % obj_id % instance_id;
		return false;
	}

	ModelObject* object = m_model->objects[obj_id];
	ModelInstance* instance = object->instances[instance_id];
	std::pair<int, int> pair(obj_id, instance_id);
	BoundingBoxf3 instance_box = object->instance_convex_hull_bounding_box(instance_id);

	result = (m_origin.x() <= instance_box.min.x()) && (m_origin.y() >= instance_box.min.y());
	return result;
}

//add an instance into plate
int PartPlate::add_instance(int obj_id, int instance_id, bool move_position, BoundingBoxf3* bounding_box)
{
	if (!valid_instance(obj_id, instance_id))
	{
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": plate_id %1%, invalid obj_id %2%, instance_id %3%, move_position %4%") % m_plate_index % obj_id % instance_id % move_position;
		return -1;
	}

	ModelObject* object = m_model->objects[obj_id];
	ModelInstance* instance = object->instances[instance_id];
	std::pair<int, int> pair(obj_id, instance_id);

    obj_to_instance_set.insert(pair);

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": plate_id %1%, add instance obj_id %2%, instance_id %3%, move_position %4%") % m_plate_index % obj_id % instance_id % move_position;

	if (move_position)
	{
		//move this instance into the new position
		Vec3d center = get_center_origin();
		center.z() = instance->get_transformation().get_offset(Z);

		instance->set_offset(center);
		object->invalidate_bounding_box();
	}

	//need to judge whether this instance has an outer part
	bool outside = check_outside(obj_id, instance_id, bounding_box);
	if (outside)
		instance_outside_set.insert(pair);

	if (m_ready_for_slice && outside)
	{
		m_ready_for_slice = false;
	}
	else if ((obj_to_instance_set.size() == 1) && (!m_ready_for_slice) && !outside)
	{
		m_ready_for_slice = true;
	}

	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate %1% , m_ready_for_slice changes to %2%") % m_plate_index %m_ready_for_slice;
	return 0;
}

//remove instance from plate
int PartPlate::remove_instance(int obj_id, int instance_id)
{
	bool result;
	std::set<std::pair<int, int>>::iterator it;

	it = obj_to_instance_set.find(std::pair(obj_id, instance_id));
	if (it != obj_to_instance_set.end()) {
		obj_to_instance_set.erase(it);
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":plate_id %1%, found obj_id %2%, instance_id %3%") % m_plate_index % obj_id % instance_id;
		result = 0;
	}
	else {
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": plate_id %1%, can not find obj_id %2%, instance_id %3%") % m_plate_index % obj_id % instance_id;
		result = -1;
		return result;
	}

	it = instance_outside_set.find(std::pair(obj_id, instance_id));
	if (it != instance_outside_set.end()) {
		instance_outside_set.erase(it);
	}
	if (!m_ready_for_slice)
		update_states();

	return result;
}

BoundingBoxf3 PartPlate::get_objects_bounding_box()
{
    BoundingBoxf3 bbox;
    for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it)
    {
        int obj_id = it->first;
        int instance_id = it->second;

        if ((obj_id >= 0) && (obj_id < m_model->objects.size()))
        {
            ModelObject* object = m_model->objects[obj_id];
            if ((instance_id >= 0) && (instance_id < object->instances.size()))
            {
                BoundingBoxf3 instance_bbox = object->instance_bounding_box(instance_id);
                bbox.merge(instance_bbox);
            }
        }
    }
    return bbox;
}

//translate instance on the plate
void PartPlate::translate_all_instance(Vec3d position)
{
    for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it)
    {
        int obj_id = it->first;
        int instance_id = it->second;

        if ((obj_id >= 0) && (obj_id < m_model->objects.size()))
        {
            ModelObject* object = m_model->objects[obj_id];
            if ((instance_id >= 0) && (instance_id < object->instances.size()))
            {
                ModelInstance* instance = object->instances[instance_id];
                const Vec3d& offset =  instance->get_offset();
                instance->set_offset(offset + position);
            }
        }
    }
    return;
}

void PartPlate::duplicate_all_instance(unsigned int dup_count, bool need_skip, std::map<int, bool>& skip_objects)
{
    std::set<std::pair<int, int>> old_obj_list = obj_to_instance_set;
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": plate_id %1%, dup_count %2%") % m_plate_index % dup_count;
    for (std::set<std::pair<int, int>>::iterator it = old_obj_list.begin(); it != old_obj_list.end(); ++it)
    {
        int obj_id = it->first;
        int instance_id = it->second;

        if ((obj_id >= 0) && (obj_id < m_model->objects.size()))
        {
            ModelObject* object = m_model->objects[obj_id];
            ModelInstance* instance = object->instances[instance_id];

            if (need_skip)
            {
                if (skip_objects.find(instance->loaded_id) != skip_objects.end())
                {
                    instance->printable = false;
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": skipped object, loaded_id %1%, name %2%, set to unprintable, no need to duplicate") % instance->loaded_id % object->name;
                    continue;
                }
            }
            for (size_t index = 0; index < dup_count; index ++)
            {
                ModelObject* newObj = m_model->add_object(*object);
                newObj->name = object->name +"_"+ std::to_string(index+1);
                int new_obj_id = m_model->objects.size() - 1;
                for ( size_t new_instance_id = 0; new_instance_id < newObj->instances.size(); new_instance_id++ )
                {
                    obj_to_instance_set.emplace(std::pair(new_obj_id, new_instance_id));
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": duplicate object into plate: index_pair [%1%,%2%], obj_id %3%") % new_obj_id % new_instance_id % newObj->id().id;
                }
            }
        }
    }

    for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it)
    {
        int obj_id = it->first;
        int instance_id = it->second;

        if ((obj_id >= 0) && (obj_id < m_model->objects.size()))
        {
            ModelObject* object = m_model->objects[obj_id];
            ModelInstance* instance = object->instances[instance_id];

            if (instance->printable)
            {
                instance->loaded_id = instance->id().id;
                if (need_skip) {
                    while (skip_objects.find(instance->loaded_id) != skip_objects.end())
                    {
                        instance->loaded_id ++;
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": duplicated id %1% with skip, try new one %2%") %instance->id().id  % instance->loaded_id;
                    }
                }
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": set obj %1% instance %2%'s loaded_id to its id %3%, name %4%") % obj_id %instance_id %instance->loaded_id  % object->name;
            }
        }
    }

    return;
}



//update instance exclude state
void PartPlate::update_instance_exclude_status(int obj_id, int instance_id, BoundingBoxf3* bounding_box)
{
	bool outside;
	std::set<std::pair<int, int>>::iterator it;

	outside = check_outside(obj_id, instance_id, bounding_box);

	it = instance_outside_set.find(std::pair(obj_id, instance_id));
	if (it == instance_outside_set.end()) {
		if (outside)
			instance_outside_set.insert(std::pair(obj_id, instance_id));
	}
	else {
		if (!outside)
			instance_outside_set.erase(it);
	}
}

//update object's index caused by original object deleted
void PartPlate::update_object_index(int obj_idx_removed, int obj_idx_max)
{
	std::set<std::pair<int, int>> temp_set;
	std::set<std::pair<int, int>>::iterator it;
	//update the obj_to_instance_set
	for (it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it)
	{
		if (it->first >= obj_idx_removed)
			temp_set.insert(std::pair(it->first-1, it->second));
		else
			temp_set.insert(std::pair(it->first, it->second));
	}
	obj_to_instance_set.clear();
	obj_to_instance_set = temp_set;

	//update the instance_outside_set
	temp_set.clear();
	for (it = instance_outside_set.begin(); it != instance_outside_set.end(); ++it)
	{
		if (it->first >= obj_idx_removed)
			temp_set.insert(std::pair(it->first - 1, it->second));
		else
			temp_set.insert(std::pair(it->first, it->second));
	}
	instance_outside_set.clear();
	instance_outside_set = temp_set;

}

void PartPlate::set_vase_mode_related_object_config(int obj_id) {
	ModelObjectPtrs obj_ptrs;
	if (obj_id != -1) {
		ModelObject* object = m_model->objects[obj_id];
		obj_ptrs.push_back(object);
	}
	else
		obj_ptrs = get_objects_on_this_plate();

	DynamicPrintConfig* global_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;
	DynamicPrintConfig new_conf;
	new_conf.set_key_value("wall_loops", new ConfigOptionInt(1));
	new_conf.set_key_value("top_shell_layers", new ConfigOptionInt(0));
	new_conf.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
	new_conf.set_key_value("enable_support", new ConfigOptionBool(false));
	new_conf.set_key_value("enforce_support_layers", new ConfigOptionInt(0));
	new_conf.set_key_value("detect_thin_wall", new ConfigOptionBool(false));
	new_conf.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
	new_conf.set_key_value("overhang_reverse", new ConfigOptionBool(false));
	new_conf.set_key_value("wall_direction", new ConfigOptionEnum<WallDirection>(WallDirection::Auto));
	auto applying_keys = global_config->diff(new_conf);

	for (ModelObject* object : obj_ptrs) {
		ModelConfigObject& config = object->config;

		for (auto opt_key : applying_keys) {
			config.set_key_value(opt_key, new_conf.option(opt_key)->clone());
		}

		applying_keys = config.get().diff(new_conf);
		for (auto opt_key : applying_keys) {
			config.set_key_value(opt_key, new_conf.option(opt_key)->clone());
		}
	}
	//wxGetApp().obj_list()->update_selections();
}

int PartPlate::printable_instance_size()
{
    int size = 0;
    for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it) {
        int obj_id      = it->first;
        int instance_id = it->second;

        if (obj_id >= m_model->objects.size())
			continue;

        ModelObject *  object   = m_model->objects[obj_id];
        ModelInstance *instance = object->instances[instance_id];

        if ((instance->printable) && (instance_outside_set.find(std::pair(obj_id, instance_id)) == instance_outside_set.end())) {
            size++;
        }
    }
    return size;
}

//whether it is has printable instances
bool PartPlate::has_printable_instances()
{
	bool result = false;

	for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it)
	{
		int obj_id = it->first;
		int instance_id = it->second;

		if (obj_id >= m_model->objects.size())
			continue;

		ModelObject* object = m_model->objects[obj_id];
		ModelInstance* instance = object->instances[instance_id];

		if ((instance->printable)&&(instance_outside_set.find(std::pair(obj_id, instance_id)) == instance_outside_set.end()))
		{
			result = true;
			break;
		}
	}

	return result;
}

bool PartPlate::is_all_instances_unprintable()
{
    bool result = true;

    for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it) {
        int obj_id      = it->first;
        int instance_id = it->second;

        if (obj_id >= m_model->objects.size()) continue;

        ModelObject *  object   = m_model->objects[obj_id];
        ModelInstance *instance = object->instances[instance_id];

        if ((instance->printable)) {
            result = false;
            break;
        }
    }

    return result;
}

//move instances to left or right PartPlate
void PartPlate::move_instances_to(PartPlate& left_plate, PartPlate& right_plate, BoundingBoxf3* bounding_box)
{
	for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it)
	{
		int obj_id = it->first;
		int instance_id = it->second;

		if (left_plate.intersect_instance(obj_id, instance_id, bounding_box))
			left_plate.add_instance(obj_id, instance_id, false, bounding_box);
		else
			right_plate.add_instance(obj_id, instance_id, false, bounding_box);
	}

	return;
}

void PartPlate::generate_logo_polygon(ExPolygon &logo_polygon)
{
	if (m_shape.size() == 4)
	{
        bool is_bbl_vendor = false;

		if (m_plater) {
            if (auto preset_bundle = wxGetApp().preset_bundle; preset_bundle)
                is_bbl_vendor = preset_bundle->is_bbl_vendor();
		}

        //rectangle case
		for (int i = 0; i < 4; i++)
		{
			const Vec2d& p = m_shape[i];
			if ((i  == 0) || (i  == 1)) {
                logo_polygon.contour.append({scale_(p(0)), scale_(is_bbl_vendor ? p(1) - 12.f : p(1))});
            }
			else {
				logo_polygon.contour.append({ scale_(p(0)), scale_(p(1)) });
			}
		}
	}
	else {
		for (const Vec2d& p : m_shape) {
			logo_polygon.contour.append({ scale_(p(0)), scale_(p(1)) });
		}
	}
}

void PartPlate::generate_print_polygon(ExPolygon &print_polygon)
{
	auto compute_points = [&print_polygon](Vec2d& center, double radius, double start_angle, double stop_angle, int count)
	{
		double angle_steps;
		angle_steps = (stop_angle - start_angle) / (count - 1);
		for(int j = 0; j < count; j++ )
		{
			double angle = start_angle + j * angle_steps;
			double x = center(0) + ::cos(angle) * radius;
			double y = center(1) + ::sin(angle) * radius;
			print_polygon.contour.append({ scale_(x), scale_(y) });
		}
	};

	for (const Vec2d& p : m_shape) {
			print_polygon.contour.append({scale_(p(0)), scale_(p(1))});
		}
}

void PartPlate::generate_exclude_polygon(ExPolygon &exclude_polygon)
{
	auto compute_exclude_points = [&exclude_polygon](Vec2d& center, double radius, double start_angle, double stop_angle, int count)
	{
		double angle_steps;
		angle_steps = (stop_angle - start_angle) / (count - 1);
		for(int j = 0; j < count; j++ )
		{
			double angle = start_angle + j * angle_steps;
			double x = center(0) + ::cos(angle) * radius;
			double y = center(1) + ::sin(angle) * radius;
			exclude_polygon.contour.append({ scale_(x), scale_(y) });
		}
	};

	int points_count = 8;
	if (m_exclude_area.size() == 4)
	{
		//rectangle case
		for (int i = 0; i < 4; i++)
		{
			const Vec2d& p = m_exclude_area[i];
			Vec2d center;
			double start_angle, stop_angle, radius;
			radius = 1.f; // ORCA use equal rounding for all corners
			switch (i) {
				case 0: // Left-Bottom
					center(0)   = p(0) + radius;
					center(1)   = p(1) + radius;
					start_angle = 1.0 * PI; //180
					stop_angle  = 1.5 * PI; //270
					compute_exclude_points(center, radius, start_angle, stop_angle, points_count);
				break;
				case 1: // Right-Bottom
					center(0)   = p(0) - radius;
					center(1)   = p(1) + radius;
					start_angle = 1.5 * PI; //270
					stop_angle  = 2.0 * PI; //360
					compute_exclude_points(center, radius, start_angle, stop_angle, points_count);
					break;
				case 2: // Right-Top
					center(0)   = p(0) - radius;
					center(1)   = p(1) - radius;
 					start_angle = 0.0 * PI; //0
					stop_angle  = 0.5 * PI; //90
					compute_exclude_points(center, radius, start_angle, stop_angle, points_count);
					break;
				case 3: // Left-Top
					center(0)   = p(0) + radius;
					center(1)   = p(1) - radius;
					start_angle = 0.5 * PI; //90
					stop_angle  = 1.0 * PI; //180
					compute_exclude_points(center, radius, start_angle, stop_angle, points_count);
					break;
			}
		}
	}
	else {
		for (const Vec2d& p : m_exclude_area) {
			exclude_polygon.contour.append({ scale_(p(0)), scale_(p(1)) });
		}
	}

	exclude_polygon.contour.make_counter_clockwise();
}

bool PartPlate::set_shape(const Pointfs& shape, const Pointfs& exclude_areas, const std::vector<Pointfs>& extruder_areas, const std::vector<double>& extruder_heights, Vec2d position, float height_to_lid, float height_to_rod)
{
	Pointfs new_shape, new_exclude_areas;
	m_extruder_heights = extruder_heights;
	for (const Vec2d& p : shape) {
		new_shape.push_back(Vec2d(p.x() + position.x(), p.y() + position.y()));
	}

	for (const Vec2d& p : exclude_areas) {
		new_exclude_areas.push_back(Vec2d(p.x() + position.x(), p.y() + position.y()));
	}

	std::vector<Pointfs> new_extruder_areas;
	for (const Pointfs& shape : extruder_areas) {
		Pointfs new_extruder_area;
		for (const Vec2d& p : shape) {
			Vec2d point(p(0) + position.x(), p(1) + position.y());
			new_extruder_area.push_back(point);
		}
		new_extruder_areas.push_back(new_extruder_area);
	}
	m_extruder_areas = std::move(new_extruder_areas);

	if ((m_shape == new_shape)&&(m_exclude_area == new_exclude_areas)
		&&(m_height_to_lid == height_to_lid)&&(m_height_to_rod == height_to_rod)) {
		BOOST_LOG_TRIVIAL(info) << "PartPlate same shape, skip directly";
		return false;
	}

	m_height_to_lid =  height_to_lid;
	m_height_to_rod =  height_to_rod;

	if ((m_shape != new_shape) || (m_exclude_area != new_exclude_areas))
	{
		/*m_shape.clear();
		for (const Vec2d& p : shape) {
			m_shape.push_back(Vec2d(p.x() + position.x(), p.y() + position.y()));
		}

		m_exclude_area.clear();
		for (const Vec2d& p : exclude_areas) {
			m_exclude_area.push_back(Vec2d(p.x() + position.x(), p.y() + position.y()));
		}*/
		m_shape = std::move(new_shape);
		m_exclude_area = std::move(new_exclude_areas);

		calc_bounding_boxes();

		ExPolygon logo_poly;
		generate_logo_polygon(logo_poly);
		m_logo_triangles.reset();
		if (!init_model_from_poly(m_logo_triangles, logo_poly, GROUND_Z + 0.02f))
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":Unable to create logo triangles\n";

		ExPolygon poly;
		/*for (const Vec2d& p : m_shape) {
			poly.contour.append({ scale_(p(0)), scale_(p(1)) });
		}*/
		generate_print_polygon(poly);
        calc_triangles(poly);

        // reset m_wrapping_detection_triangles when change printer
        m_print_polygon = poly;
        m_wrapping_detection_triangles.reset();
        init_raycaster_from_model(m_triangles);

		ExPolygon exclude_poly;
		/*for (const Vec2d& p : m_exclude_area) {
			exclude_poly.contour.append({ scale_(p(0)), scale_(p(1)) });
		}*/
		generate_exclude_polygon(exclude_poly);
		calc_exclude_triangles(exclude_poly);

		const BoundingBox& pp_bbox = poly.contour.bounding_box();
		calc_gridlines(poly, pp_bbox);

		//calc_vertex_for_icons_background(5, m_del_and_background_icon);
		//calc_vertex_for_icons(4, m_del_icon);
		calc_vertex_for_icons(0, m_del_icon);
        calc_vertex_for_icons(1, m_orient_icon);
        calc_vertex_for_icons(2, m_arrange_icon);
        calc_vertex_for_icons(3, m_lock_icon);
        calc_vertex_for_icons(4, m_plate_settings_icon);
        // ORCA also change bed_icon_count number in calc_vertex_for_icons() after adding or removing icons for circular shaped beds that uses vertical alingment for icons
        PresetBundle* preset = wxGetApp().preset_bundle;
        bool dual_bbl = (preset->is_bbl_vendor() && preset->get_printer_extruder_count() == 2);
        calc_vertex_for_icons(dual_bbl ? 5 : 6, m_plate_filament_map_icon);
        calc_vertex_for_icons(dual_bbl ? 6 : 5, m_move_front_icon);

		//calc_vertex_for_number(0, (m_plate_index < 9), m_plate_idx_icon);
		calc_vertex_for_number(0, false, m_plate_idx_icon);
		if (m_plater) {
			// calc vertex for plate name
			generate_plate_name_texture();
		}
	}

	calc_height_limit();

	return true;
}

const BoundingBox PartPlate::get_bounding_box_crd()
{
	const auto plate_shape = Slic3r::Polygon::new_scale(m_shape);

	return plate_shape.bounding_box();
}

BoundingBoxf3 PartPlate::get_build_volume(bool use_share)
{
    auto  eps=Slic3r::BuildVolume::SceneEpsilon;
	Vec3d up_point;
	Vec3d low_point;
	if (use_share && !m_extruder_areas.empty()) {
		Polygon bed_poly = get_shared_poly(m_extruder_areas);
		BoundingBox bbox = bed_poly.bounding_box();

		up_point = Vec3d(unscale_(bbox.max.x()) + eps,  unscale_(bbox.max.y()) + eps, m_origin.z() + m_height + eps);
		low_point = Vec3d(unscale_(bbox.min.x()) - eps, unscale_(bbox.min.y()) - eps, m_origin.z() - eps);
	}
	else {
		// Orca: support non-rectangular bed
		up_point  = m_bounding_box.max + Vec3d(eps, eps, m_origin.z() + m_height + eps);
		low_point = m_bounding_box.min + Vec3d(-eps, -eps, m_origin.z() - eps);
	}
    BoundingBoxf3 plate_box(low_point, up_point);
    return plate_box;
}

bool PartPlate::contains(const Vec3d& point) const
{
	return m_bounding_box.contains(point);
}

bool PartPlate::contains(const GLVolume& v) const
{
	return m_bounding_box.contains(v.bounding_box());
}

bool PartPlate::contains(const BoundingBoxf3& bb) const
{
	// Allow the objects to protrude below the print bed
	BoundingBoxf3 print_volume(Vec3d(m_bounding_box.min(0), m_bounding_box.min(1), 0.0), Vec3d(m_bounding_box.max(0), m_bounding_box.max(1), 1e3));
	print_volume.min(2) = -1e10;
	print_volume.min(0) -= Slic3r::BuildVolume::BedEpsilon;
	print_volume.min(1) -= Slic3r::BuildVolume::BedEpsilon;
	print_volume.max(0) += Slic3r::BuildVolume::BedEpsilon;
	print_volume.max(1) += Slic3r::BuildVolume::BedEpsilon;
	return print_volume.contains(bb);
}

bool PartPlate::intersects(const BoundingBoxf3& bb) const
{
	// Allow the objects to protrude below the print bed
	BoundingBoxf3 print_volume(Vec3d(m_bounding_box.min(0), m_bounding_box.min(1), 0.0), Vec3d(m_bounding_box.max(0), m_bounding_box.max(1), 1e3));
	print_volume.min(2) = -1e10;
	print_volume.min(0) -= Slic3r::BuildVolume::BedEpsilon;
	print_volume.min(1) -= Slic3r::BuildVolume::BedEpsilon;
	print_volume.max(0) += Slic3r::BuildVolume::BedEpsilon;
	print_volume.max(1) += Slic3r::BuildVolume::BedEpsilon;
	return print_volume.intersects(bb);
}

void PartPlate::render(const Transform3d& view_matrix, const Transform3d& projection_matrix, bool bottom, bool only_body, bool force_background_color, HeightLimitMode mode, int hover_id, bool render_cali, bool show_grid)
{
    glsafe(::glEnable(GL_DEPTH_TEST));

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

        shader->set_uniform("view_model_matrix", view_matrix);
        shader->set_uniform("projection_matrix", projection_matrix);

        if (!bottom) {
            // draw background
            render_background(force_background_color);

            render_exclude_area(force_background_color);
            if(m_selected && wxGetApp().plater()->get_enable_wrapping_detection()){
                if(!m_wrapping_detection_triangles.is_initialized()){
                    auto points = get_plate_wrapping_detection_area();
                    if (points.size() > 0) {//wrapping_detection_area
                        ExPolygon temp_poly;
                        for (const Vec2d &p : points) {
                            temp_poly.contour.append({scale_(p(0)), scale_(p(1))});
                        }
                        auto      result = intersection(m_print_polygon, temp_poly);
                        if (result.size() > 0) {
                            ExPolygon wrapp_poly(result[0]);
                            calc_triangles_from_polygon(wrapp_poly, m_wrapping_detection_triangles);
                        }
                    }
                }
                render_wrapping_detection_area(force_background_color);
            }
        }

        if (show_grid)
            render_grid(bottom);

        render_height_limit(mode);

        glsafe(::glDisable(GL_BLEND));

        // if (with_label) {
        //	render_label(canvas);
        // }

        shader->stop_using();
    }

    if (!bottom && m_selected && !force_background_color) {
        if (m_partplate_list)
            render_logo(bottom, m_partplate_list->render_cali_logo && render_cali);
        else
            render_logo(bottom);
    }

    render_icons(bottom, only_body, hover_id);
    if (!force_background_color) {
        render_only_numbers(bottom);
    }

    glsafe(::glDisable(GL_DEPTH_TEST));
}

void PartPlate::set_selected() {
	m_selected = true;
}

void PartPlate::set_unselected() {
	m_selected = false;
}


/*status related functions*/
//update status
void PartPlate::update_states()
{
	//currently let judge outside partplate when plate is empty
	/*if (obj_to_instance_set.size() == 0)
	{
		m_ready_for_slice = false;
		return;
	}*/
	m_ready_for_slice = true;
	for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it) {
		int obj_id = it->first;
		int instance_id = it->second;

		//if (check_outside(obj_id, instance_id))
		if (instance_outside_set.find(std::pair(obj_id, instance_id)) != instance_outside_set.end())
		{
			m_ready_for_slice = false;
			//currently only check whether ready to slice
			break;
		}
	}

	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate %1% , m_ready_for_slice changes to %2%") % m_plate_index %m_ready_for_slice;
	return;
}

/*slice related functions*/
//invalid sliced result
void PartPlate::update_slice_result_valid_state(bool valid)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate %1% , update slice result from %2% to %3%") % m_plate_index %m_slice_result_valid %valid;
    m_slice_result_valid = valid;
    if (valid)
        m_slice_percent = 100.0f;
    else {
        m_slice_percent = -1.0f;
    }
}

//update current slice context into backgroud slicing process
void PartPlate::update_slice_context(BackgroundSlicingProcess & process)
{
	auto statuscb = [this](const Slic3r::PrintBase::SlicingStatus& status) {
		Slic3r::SlicingStatusEvent *event = new Slic3r::SlicingStatusEvent(EVT_SLICING_UPDATE, 0, status);
		//BBS: GUI refactor: add plate info befor message
		if (status.message_type == Slic3r::PrintStateBase::SlicingDefaultNotification) {
			auto temp = Slic3r::format(_u8L(" plate %1%:"), std::to_string(m_plate_index + 1));
			event->status.text = temp + event->status.text;
		}
		wxQueueEvent(m_plater, event);
	};

	process.set_fff_print(m_print);
	process.set_gcode_result(m_gcode_result);
	process.select_technology(this->printer_technology);
	process.set_current_plate(this);
	m_print->set_status_callback(statuscb);
	process.switch_print_preprocess();

	return;
}

// BBS: delay calc gcode path in backup dir
std::string PartPlate::get_tmp_gcode_path()
{
    if (m_tmp_gcode_path.empty()) {
        boost::filesystem::path temp_path(m_model->get_backup_path("Metadata"));
        temp_path /= (boost::format(".%1%.%2%.gcode") % get_current_pid() %
                      GLOBAL_PLATE_INDEX++).str();
        m_tmp_gcode_path = temp_path.string();
    }
    return m_tmp_gcode_path;
}

std::string PartPlate::get_temp_config_3mf_path()
{
	if (m_temp_config_3mf_path.empty()) {
		boost::filesystem::path temp_path(m_model->get_backup_path("Metadata"));
		temp_path /= (boost::format(".%1%.%2%_config.3mf") % get_current_pid() %
			GLOBAL_PLATE_INDEX++).str();
		m_temp_config_3mf_path = temp_path.string();

	}
	return m_temp_config_3mf_path;
}

// load gcode from file
int PartPlate::load_gcode_from_file(const std::string& filename)
{
	int ret = 0;

	// process gcode
	DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
	full_config.apply(m_config, true);
	m_print->apply(*m_model, full_config);
	//BBS: need to apply two times, for after the first apply, the m_print got its object,
	//which will affect the config when new_full_config.normalize_fdm(used_filaments);
	m_print->apply(*m_model, full_config);

	// BBS: use backup path to save temp gcode
    // auto path = get_tmp_gcode_path();
    // if (boost::filesystem::exists(boost::filesystem::path(path))) {
    //	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": file %1% exists, delete it firstly") % filename.c_str();
    //	boost::nowide::remove(path.c_str());
    //}

    // std::error_code error = rename_file(filename, path);
    // if (error) {
    //	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("Failed to rename the output G-code file from %1% to %2%, error code %3%") % filename.c_str() % path.c_str() %
    //error.message(); 	return -1;
    //}
	if (boost::filesystem::exists(filename)) {
		assert(m_tmp_gcode_path.empty());
		m_tmp_gcode_path = filename;
		m_gcode_result->filename = filename;
		m_print->set_gcode_file_ready();

		update_slice_result_valid_state(true);

		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": found valid gcode file %1%") % filename.c_str();
	}
	else {
		BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": can not find gcode file %1%") % filename.c_str();
		ret = -1;
	}

	m_ready_for_slice = true;
	return ret;
}

int PartPlate::load_thumbnail_data(std::string filename, ThumbnailData& thumb_data)
{
	bool result = true;
	wxImage img;
	if (boost::algorithm::iends_with(filename, ".png")) {
		result = img.LoadFile(wxString::FromUTF8(filename.c_str()), wxBITMAP_TYPE_PNG);
		img = img.Mirror(false);
	}
	if (result) {
		thumb_data.set(img.GetWidth(), img.GetHeight());
		for (int i = 0; i < img.GetWidth() * img.GetHeight(); i++) {
			memcpy(&thumb_data.pixels[4 * i], (unsigned char*)(img.GetData() + 3 * i), 3);
			if (img.HasAlpha()) {
				thumb_data.pixels[4 * i + 3] = *(unsigned char*)(img.GetAlpha() + i);
			}
		}
	} else {
		return -1;
	}
	return 0;
}

int PartPlate::load_pattern_thumbnail_data(std::string filename)
{
	/*bool result = true;
	wxImage img;
	result = load_image(filename, img);
	if (result) {
		cali_thumbnail_data.set(img.GetWidth(), img.GetHeight());
		for (int i = 0; i < img.GetWidth() * img.GetHeight(); i++) {
			memcpy(&cali_thumbnail_data.pixels[4 * i], (unsigned char*)(img.GetData() + 3 * i), 3);
			if (img.HasAlpha()) {
				cali_thumbnail_data.pixels[4 * i + 3] = *(unsigned char*)(img.GetAlpha() + i);
			}
		}
	}
	else {
		return -1;
	}*/
	return 0;
}

//load pattern box data from file
int PartPlate::load_pattern_box_data(std::string filename)
{
    try {
        nlohmann::json j;
        boost::nowide::ifstream ifs(filename);
        ifs >> j;

        PlateBBoxData bbox_data;
        bbox_data.from_json(j);
        cali_bboxes_data = bbox_data;
        return 0;
    }
    catch(std::exception &ex) {
        BOOST_LOG_TRIVIAL(trace) << boost::format("catch an exception %1%")%ex.what();
        return -1;
    }
}

std::vector<int> PartPlate::get_first_layer_print_sequence() const
{
    const ConfigOptionInts *op_print_sequence_1st = m_config.option<ConfigOptionInts>("first_layer_print_sequence");
    if (op_print_sequence_1st)
        return op_print_sequence_1st->values;
    else
        return std::vector<int>();
}

std::vector<LayerPrintSequence> PartPlate::get_other_layers_print_sequence() const
{
	const ConfigOptionInts* other_layers_print_sequence_op = m_config.option<ConfigOptionInts>("other_layers_print_sequence");
	const ConfigOptionInt* other_layers_print_sequence_nums_op = m_config.option<ConfigOptionInt>("other_layers_print_sequence_nums");
	if (other_layers_print_sequence_op && other_layers_print_sequence_nums_op) {
		const std::vector<int>& print_sequence = other_layers_print_sequence_op->values;
		int sequence_nums = other_layers_print_sequence_nums_op->value;
		auto other_layers_seqs = Slic3r::get_other_layers_print_sequence(sequence_nums, print_sequence);
		return other_layers_seqs;
	}
	else
		return {};
}

void PartPlate::set_first_layer_print_sequence(const std::vector<int>& sorted_filaments)
{
    if (sorted_filaments.size() > 0) {
		if (sorted_filaments.size() == 1 && sorted_filaments[0] == 0) {
            m_config.erase("first_layer_print_sequence");
        }
		else {
            ConfigOptionInts *op_print_sequence_1st = m_config.option<ConfigOptionInts>("first_layer_print_sequence");
            if (op_print_sequence_1st)
                op_print_sequence_1st->values = sorted_filaments;
            else
                m_config.set_key_value("first_layer_print_sequence", new ConfigOptionInts(sorted_filaments));
        }
    }
	else {
        m_config.erase("first_layer_print_sequence");
	}
}

void PartPlate::set_other_layers_print_sequence(const std::vector<LayerPrintSequence>& layer_seq_list)
{
	if (layer_seq_list.empty()) {
		m_config.erase("other_layers_print_sequence");
		m_config.erase("other_layers_print_sequence_nums");
		return;
	}

	int sequence_nums;
	std::vector<int> other_layers_seqs;
	Slic3r::get_other_layers_print_sequence(layer_seq_list, sequence_nums, other_layers_seqs);
	ConfigOptionInts* other_layers_print_sequence_op = m_config.option<ConfigOptionInts>("other_layers_print_sequence");
	ConfigOptionInt* other_layers_print_sequence_nums_op = m_config.option<ConfigOptionInt>("other_layers_print_sequence_nums");
	if (other_layers_print_sequence_op)
		other_layers_print_sequence_op->values = other_layers_seqs;
	else
		m_config.set_key_value("other_layers_print_sequence", new ConfigOptionInts(other_layers_seqs));
	if (other_layers_print_sequence_nums_op)
		other_layers_print_sequence_nums_op->value = sequence_nums;
	else
		m_config.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(sequence_nums));
}

void PartPlate::update_first_layer_print_sequence(size_t filament_nums)
{
	auto other_layers_seqs = get_other_layers_print_sequence();
	if (!other_layers_seqs.empty()) {
		bool need_update_data = false;
		for (auto& other_layers_seq : other_layers_seqs) {
			std::vector<int>& orders = other_layers_seq.second;
			if (orders.size() > filament_nums) {
				orders.erase(std::remove_if(orders.begin(), orders.end(), [filament_nums](int n) { return n > filament_nums; }), orders.end());
				need_update_data = true;
			}
			if (orders.size() < filament_nums) {
				for (size_t extruder_id = orders.size(); extruder_id < filament_nums; ++extruder_id) {
					orders.push_back(extruder_id + 1);
					need_update_data = true;
				}
			}
		}
		if (need_update_data)
			set_other_layers_print_sequence(other_layers_seqs);
	}


    ConfigOptionInts * op_print_sequence_1st = m_config.option<ConfigOptionInts>("first_layer_print_sequence");
    if (!op_print_sequence_1st) {
		return;
	}

    std::vector<int> &print_sequence_1st = op_print_sequence_1st->values;
    if (print_sequence_1st.size() == 0 || print_sequence_1st[0] == 0)
		return;

	if (print_sequence_1st.size() > filament_nums) {
        print_sequence_1st.erase(std::remove_if(print_sequence_1st.begin(), print_sequence_1st.end(), [filament_nums](int n) { return n > filament_nums; }),
                                 print_sequence_1st.end());
    }
	else if (print_sequence_1st.size() < filament_nums) {
        for (size_t extruder_id = print_sequence_1st.size(); extruder_id < filament_nums; ++extruder_id) {
            print_sequence_1st.push_back(extruder_id + 1);
		}
    }
}

void PartPlate::update_first_layer_print_sequence_when_delete_filament(size_t filament_id)
{
    auto other_layers_seqs = get_other_layers_print_sequence();
    if (!other_layers_seqs.empty()) {
        bool need_update_data = false;
        for (auto &other_layers_seq : other_layers_seqs) {
            std::vector<int> &orders = other_layers_seq.second;
            orders.erase(std::remove_if(orders.begin(), orders.end(), [filament_id](int n) { return n == filament_id +1; }), orders.end());
            for (auto &order : orders) {
                order = order > filament_id ? order - 1 : order;
            }
            need_update_data = true;
        }
        if (need_update_data)
            set_other_layers_print_sequence(other_layers_seqs);
    }

    ConfigOptionInts *op_print_sequence_1st = m_config.option<ConfigOptionInts>("first_layer_print_sequence");
    if (!op_print_sequence_1st)
        return;

    std::vector<int> &print_sequence_1st = op_print_sequence_1st->values;
    if (print_sequence_1st.size() == 0 || print_sequence_1st[0] == 0)
        return;

    print_sequence_1st.erase(std::remove_if(print_sequence_1st.begin(), print_sequence_1st.end(), [filament_id](int n) { return n == filament_id + 1; }), print_sequence_1st.end());
    for (auto &order : print_sequence_1st) {
        order = order > filament_id ? order - 1 : order;
    }
}

void PartPlate::print() const
{
	unsigned int count=0;

	BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << boost::format(": plate index %1%, pointer %2%, print_index %3% print pointer %4%") % m_plate_index % this % m_print_index % m_print;
	BOOST_LOG_TRIVIAL(trace) << boost::format("\t origin {%1%,%2%,%3%}, width %4%,  depth %5%, height %6%") % m_origin.x() % m_origin.y() % m_origin.z() % m_width % m_depth % m_height;
	BOOST_LOG_TRIVIAL(trace) << boost::format("\t m_printable %1%, m_locked %2%, m_ready_for_slice %3%, m_slice_result_valid %4%,  m_tmp_gcode_path %5%, set size %6%")\
		% m_printable % m_locked % m_ready_for_slice % m_slice_result_valid % m_tmp_gcode_path % obj_to_instance_set.size();
	/*for (std::set<std::pair<int, int>>::iterator it = obj_to_instance_set.begin(); it != obj_to_instance_set.end(); ++it) {
		int obj_id = it->first;
		int instance_id = it->second;

		BOOST_LOG_TRIVIAL(trace) << boost::format("\t the %1%th instance, obj_id %2%, instance id %3%") % count++ % obj_id % instance_id;
	}*/
	BOOST_LOG_TRIVIAL(trace) << boost::format("excluded instance set size %1%")%instance_outside_set.size();
	/*for (std::set<std::pair<int, int>>::iterator it = instance_outside_set.begin(); it != instance_outside_set.end(); ++it) {
		int obj_id = it->first;
		int instance_id = it->second;

		BOOST_LOG_TRIVIAL(trace) << boost::format("\t obj_id %1%, instance id %2%") % obj_id % instance_id;
	}*/

	return;
}

FilamentMapMode PartPlate::get_filament_map_mode() const
{
    std::string key = "filament_map_mode";
    if(m_config.has(key))
        return m_config.option<ConfigOptionEnum<FilamentMapMode>>(key)->value;
    return FilamentMapMode::fmmDefault;
}

void PartPlate::set_filament_map_mode(const FilamentMapMode& mode)
{
	const auto& proj_config = wxGetApp().preset_bundle->project_config;
	FilamentMapMode global_mode = proj_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode")->value;
	FilamentMapMode old_mode = get_filament_map_mode();
	FilamentMapMode old_real_mode = old_mode == fmmDefault ? global_mode : old_mode;
	FilamentMapMode new_real_mode = mode == fmmDefault ? global_mode : mode;

	if (old_real_mode != new_real_mode)
		clear_filament_map();
	if (mode == fmmDefault)
		clear_filament_map_mode();
	else
		m_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = mode;
}

std::vector<int> PartPlate::get_filament_maps() const
{
    std::string key = "filament_map";
    if (m_config.has(key))
        return m_config.option<ConfigOptionInts>(key)->values;

    return {};
}

void PartPlate::set_filament_maps(const std::vector<int>& f_maps)
{
    m_config.option<ConfigOptionInts>("filament_map", true)->values = f_maps;
}

void PartPlate::clear_filament_map()
{
    if (m_config.has("filament_map"))
        m_config.erase("filament_map");
}

void PartPlate::clear_filament_map_mode()
{
    if (m_config.has("filament_map_mode"))
        m_config.erase("filament_map_mode");
}

void PartPlate::on_extruder_count_changed(int extruder_count)
{
    if (extruder_count < 2) {
        std::vector<int> f_map = wxGetApp().plater()->get_global_filament_map();
        std::fill(f_map.begin(), f_map.end(), 1);
        wxGetApp().plater()->set_global_filament_map(f_map);
        // clear filament map and mode in single extruder mode
        clear_filament_map();
        //clear_filament_map_mode();
        // do not clear mode now, reset to default mode
        m_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = FilamentMapMode::fmmAutoForFlush;
    }
}

void PartPlate::set_filament_count(int filament_count)
{
    if (m_config.has("filament_map")) {
        std::vector<int>& filament_maps = m_config.option<ConfigOptionInts>("filament_map")->values;
        filament_maps.resize(filament_count, 1);
    }
}

void PartPlate::on_filament_added()
{
    if (m_config.has("filament_map")) {
        std::vector<int>& filament_maps = m_config.option<ConfigOptionInts>("filament_map")->values;
        filament_maps.push_back(1);
    }
}

void PartPlate::on_filament_deleted(int filament_count, int filament_id)
{
    if (m_config.has("filament_map")) {
        std::vector<int>& filament_maps = m_config.option<ConfigOptionInts>("filament_map")->values;
        filament_maps.erase(filament_maps.begin() + filament_id);
    }
    update_first_layer_print_sequence_when_delete_filament(filament_id);
}


/* PartPlate List related functions*/
PartPlateList::PartPlateList(int width, int depth, int height, Plater* platerObj, Model* modelObj, PrinterTechnology tech)
	:m_plate_width(width), m_plate_depth(depth), m_plate_height(height), m_plater(platerObj), m_model(modelObj), printer_technology(tech),
	unprintable_plate(this, Vec3d(0.0 + width * (1. + LOGICAL_PART_PLATE_GAP), 0.0, 0.0), width, depth, height, platerObj, modelObj, false, tech)
{
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":plate_width %1%, plate_depth %2%, plate_height %3%") % width % depth % height;

	init();
}
PartPlateList::PartPlateList(Plater* platerObj, Model* modelObj, PrinterTechnology tech)
	:m_plate_width(0), m_plate_depth(0), m_plate_height(0), m_plater(platerObj), m_model(modelObj), printer_technology(tech),
	unprintable_plate(this, Vec3d(0.0, 0.0, 0.0), m_plate_width, m_plate_depth, m_plate_height, platerObj, modelObj, false, tech)
{
	init();
}

PartPlateList::~PartPlateList()
{
	clear(true, true);
	release_icon_textures();
}

void PartPlateList::init()
{
	m_intialized = false;
	PartPlate* first_plate = NULL;
	first_plate = new PartPlate(this, Vec3d(0.0, 0.0, 0.0), m_plate_width, m_plate_depth, m_plate_height, m_plater, m_model, true, printer_technology);
	assert(first_plate != NULL);
	m_plate_list.push_back(first_plate);

	m_print_index = 0;
	if (printer_technology == ptFFF)
	{
		Print* print = new Print();
		GCodeResult* gcode = new GCodeResult();
		m_print_list.emplace(m_print_index, print);
		m_gcode_result_list.emplace(m_print_index, gcode);
		first_plate->set_print(print, gcode, m_print_index);
		m_print_index++;
	}
	first_plate->set_index(0);

	m_plate_count = 1;
	m_plate_cols = 1;
	m_current_plate = 0;

	if (m_plater) {
        // In GUI mode
        set_default_wipe_tower_pos_for_plate(0);
    }

	select_plate(0);
	unprintable_plate.set_index(1);

	m_intialized = true;
}

//compute the origin for printable plate with index i
Vec3d PartPlateList::compute_origin(int i, int cols)
{
	Vec3d origin;
	Vec2d pos = compute_shape_position(i, cols);
	origin    = Vec3d(pos.x(), pos.y(), 0);

	return origin;
}

//compute the origin for printable plate with index i using new width
Vec3d PartPlateList::compute_origin_using_new_size(int i, int new_width, int new_depth)
{
	Vec3d origin;
	int row, col;

	row = i / m_plate_cols;
	col = i % m_plate_cols;

	origin(0) = col * (new_width * (1. + LOGICAL_PART_PLATE_GAP));
	origin(1) = -row * (new_depth * (1. + LOGICAL_PART_PLATE_GAP));
	origin(2) = 0;

	return origin;
}


//compute the origin for printable plate with index i
Vec3d PartPlateList::compute_origin_for_unprintable()
{
	int max_count = m_plate_cols * m_plate_cols;
	if (m_plate_count == max_count)
		return compute_origin(max_count + m_plate_cols - 1, m_plate_cols + 1);
	else
		return compute_origin(m_plate_count, m_plate_cols);
}

//compute shape position
Vec2d PartPlateList::compute_shape_position(int index, int cols)
{
	Vec2d pos;
	int row, col;

	row = index / cols;
	col = index % cols;

	pos(0) = col * plate_stride_x();
	pos(1) = -row * plate_stride_y();

	return pos;
}

//generate icon textures
void PartPlateList::generate_icon_textures()
{
	// use higher resolution images if graphic card and opengl version allow
	GLint max_tex_size = OpenGLManager::get_gl_info().get_max_tex_size(), icon_size = max_tex_size / 8;
	std::string path = resources_dir() + "/images/";
	std::string file_name;

	if (icon_size > 256)
		icon_size = 256;
	//if (m_del_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_close_dark.svg" : "plate_close.svg");
		if (!m_del_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_del_hovered_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_close_hover_dark.svg" : "plate_close_hover.svg");
		if (!m_del_hovered_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	
	// if (m_move_front_texture.get_id() == 0)
    {
        file_name = path + (m_is_dark ? "plate_move_front_dark.svg" : "plate_move_front.svg");
        if (!m_move_front_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
        }
    }

    // if (m_move_front_hovered_texture.get_id() == 0)
    {
        file_name = path + (m_is_dark ? "plate_move_front_hover_dark.svg" : "plate_move_front_hover.svg");
        if (!m_move_front_hovered_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
        }
    }

	//if (m_arrange_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_arrange_dark.svg" : "plate_arrange.svg");
		if (!m_arrange_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_arrange_hovered_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_arrange_hover_dark.svg" : "plate_arrange_hover.svg");
		if (!m_arrange_hovered_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_orient_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_orient_dark.svg" : "plate_orient.svg");
		if (!m_orient_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_orient_hovered_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_orient_hover_dark.svg" : "plate_orient_hover.svg");
		if (!m_orient_hovered_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_locked_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_locked_dark.svg" : "plate_locked.svg");
		if (!m_locked_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_locked_hovered_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_locked_hover_dark.svg" : "plate_locked_hover.svg");
		if (!m_locked_hovered_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_lockopen_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_unlocked_dark.svg" : "plate_unlocked.svg");
		if (!m_lockopen_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_lockopen_hovered_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_unlocked_hover_dark.svg" : "plate_unlocked_hover.svg");
		if (!m_lockopen_hovered_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_bedtype_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_settings_dark.svg" : "plate_settings.svg");
		if (!m_plate_settings_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	{
        file_name = path + (m_is_dark ? "plate_set_filament_map_dark.svg" : "plate_set_filament_map.svg");
        if (!m_plate_set_filament_map_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
        }
    }

	{
        file_name = path + (m_is_dark ? "plate_set_filament_map_hover_dark.svg" : "plate_set_filament_map_hover.svg");
        if (!m_plate_set_filament_map_hovered_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
        }
    }

	//if (m_bedtype_changed_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_settings_changed_dark.svg" : "plate_settings_changed.svg");
		if (!m_plate_settings_changed_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_bedtype_hovered_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_settings_hover_dark.svg" : "plate_settings_hover.svg");
		if (!m_plate_settings_hovered_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	//if (m_bedtype_changed_hovered_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_settings_changed_hover_dark.svg" : "plate_settings_changed_hover.svg");
		if (!m_plate_settings_changed_hovered_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	// if (m_plate_name_edit_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_name_edit_dark.svg" : "plate_name_edit.svg");
		if (!m_plate_name_edit_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
			BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		 }
	}
    // if (m_plate_name_edit_hovered_texture.get_id() == 0)
	{
		file_name = path + (m_is_dark ? "plate_name_edit_hover_dark.svg" : "plate_name_edit_hover.svg");
		if (!m_plate_name_edit_hovered_texture.load_from_svg_file(file_name, true, false, false, icon_size)) {
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
		}
	}

	std::string text_str = "01";
    // ORCA also scale font size to prevent low res texture
    int size = wxGetApp().em_unit() * PARTPLATE_ICON_SIZE;
    auto l = Label::sysFont(int(size), true);
    wxFont* font = &l;

	for (int i = 0; i < MAX_PLATE_COUNT; i++) {
		if (m_idx_textures[i].get_id() == 0) {
			//file_name = path + (boost::format("plate_%1%.svg") % (i + 1)).str();
			if ( i < 9 )
				file_name = std::string("0") + std::to_string(i+1);
			else
				file_name = std::to_string(i+1);

			wxColour foreground(0xf2, 0x75, 0x4e, 0xff);
			if (!m_idx_textures[i].generate_from_text_string(file_name, *font, *wxBLACK, foreground)) {
				BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":load file %1% failed") % file_name;
			}
		}
	}
}

void PartPlateList::release_icon_textures()
{
	m_logo_texture.reset();
	m_del_texture.reset();
	m_del_hovered_texture.reset();
    m_move_front_hovered_texture.reset();
    m_move_front_texture.reset();
	m_arrange_texture.reset();
	m_arrange_hovered_texture.reset();
	m_orient_texture.reset();
	m_orient_hovered_texture.reset();
	m_locked_texture.reset();
	m_locked_hovered_texture.reset();
	m_lockopen_texture.reset();
	m_lockopen_hovered_texture.reset();
	m_plate_settings_texture.reset();
	m_plate_settings_hovered_texture.reset();
    m_plate_set_filament_map_texture.reset();
    m_plate_set_filament_map_hovered_texture.reset();
	m_plate_name_edit_texture.reset();
	m_plate_name_edit_hovered_texture.reset();
	for (int i = 0;i < MAX_PLATE_COUNT; i++) {
		m_idx_textures[i].reset();
	}
	//reset
	PartPlateList::is_load_bedtype_textures = false;
    PartPlateList::is_load_extruder_only_area_textures = false;
	PartPlateList::is_load_cali_texture = false;
	for (int i = 0; i < btCount; i++) {
		for (auto& part: bed_texture_info[i].parts) {
			if (part.texture) {
				part.texture->reset();
				delete part.texture;
			}
			if (part.buffer) {
				delete part.buffer;
			}
		}
	}
    for (int i = 0; i < (unsigned char)ExtruderOnlyAreaType::btAreaCount; i++) {
        for (auto &part : extruder_only_area_info[i].parts) {
            if (part.texture) {
                part.texture->reset();
                delete part.texture;
            }
            if (part.buffer) { delete part.buffer; }
        }
    }
}

void PartPlateList::set_default_wipe_tower_pos_for_plate(int plate_idx)
{
    DynamicConfig &     proj_cfg     = wxGetApp().preset_bundle->project_config;
    ConfigOptionFloats *wipe_tower_x = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x");
    ConfigOptionFloats *wipe_tower_y = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y");
    wipe_tower_x->values.resize(m_plate_list.size(), wipe_tower_x->values.front());
    wipe_tower_y->values.resize(m_plate_list.size(), wipe_tower_y->values.front());

    auto printer_structure_opt = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<PrinterStructure>>("printer_structure");
    // set the default position, the same with print config(left top)
    ConfigOptionFloat wt_x_opt(WIPE_TOWER_DEFAULT_X_POS);
    ConfigOptionFloat wt_y_opt(WIPE_TOWER_DEFAULT_Y_POS);
    if (printer_structure_opt && printer_structure_opt->value == PrinterStructure::psI3) {
        wt_x_opt = ConfigOptionFloat(I3_WIPE_TOWER_DEFAULT_X_POS);
        wt_y_opt = ConfigOptionFloat(I3_WIPE_TOWER_DEFAULT_Y_POS);
    }
    dynamic_cast<ConfigOptionFloats *>(proj_cfg.option("wipe_tower_x"))->set_at(&wt_x_opt, plate_idx, 0);
    dynamic_cast<ConfigOptionFloats *>(proj_cfg.option("wipe_tower_y"))->set_at(&wt_y_opt, plate_idx, 0);
}

//this may be happened after machine changed
void PartPlateList::reset_size(int width, int depth, int height, bool reload_objects, bool update_shapes)
{
	Vec3d origin1, origin2;

	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":before size: plate_width %1%, plate_depth %2%, plate_height %3%") % m_plate_width % m_plate_depth % m_plate_height;
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":after size: plate_width %1%, plate_depth %2%, plate_height %3%") % width % depth % height;
	if ((m_plate_width != width) || (m_plate_depth != depth) || (m_plate_height != height))
	{
		m_plate_width = width;
		m_plate_depth = depth;
		m_plate_height = height;
		update_all_plates_pos_and_size(false, false, true);
		if (update_shapes) {
			set_shapes(m_shape, m_exclude_areas, m_wrapping_exclude_areas, m_extruder_areas, m_extruder_heights, m_logo_texture_filename, m_height_to_lid, m_height_to_rod);
		}
		if (reload_objects)
			reload_all_objects();
		else
			clear(false, false, false, -1);
	}

	return;
}

//clear all the instances in the plate, but keep the plates
void PartPlateList::clear(bool delete_plates, bool release_print_list, bool except_locked, int plate_index)
{
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		PartPlate* plate = m_plate_list[i];
		assert(plate != NULL);

		if (except_locked && plate->is_locked())
			plate->clear(false);
		else if ((plate_index != -1) && (plate_index != i))
			plate->clear(false);
		else
			plate->clear();
		if (delete_plates)
			delete plate;
	}

	if (delete_plates)
	{
		//also delete print related to the plate
		m_plate_list.clear();
		m_current_plate = 0;
	}

	if (release_print_list)
	{
		for (std::map<int, PrintBase*>::iterator it = m_print_list.begin(); it != m_print_list.end(); ++it)
		{
			PrintBase* print = it->second;
			assert(print != NULL);

			delete print;
		}
		m_print_list.clear();
		for (std::map<int, GCodeResult*>::iterator it = m_gcode_result_list.begin(); it != m_gcode_result_list.end(); ++it)
		{
			GCodeResult* gcode = it->second;
			assert(gcode != NULL);

			delete gcode;
		}
		m_gcode_result_list.clear();
	}

	unprintable_plate.clear();
}

//clear all the instances in the plate, and delete the plates, only keep the first default plate
void PartPlateList::reset(bool do_init)
{
	clear(true, false);

	//m_plate_list.clear();

	if (do_init) {
		init();
		m_plate_list[0]->set_filament_count(m_filament_count);
	}

	return;
}

//reset partplate to init states
void PartPlateList::reinit()
{
	clear(true, true);

	init();

	m_plate_list[0]->set_filament_count(m_filament_count);

	//reset plate 0's position
	Vec2d pos = compute_shape_position(0, m_plate_cols);
	m_plate_list[0]->set_shape(m_shape, m_exclude_areas, m_extruder_areas, m_extruder_heights, pos, m_height_to_lid, m_height_to_rod);
	//reset unprintable plate's position
	Vec3d origin2 = compute_origin_for_unprintable();
	unprintable_plate.set_pos_and_size(origin2, m_plate_width, m_plate_depth, m_plate_height, false);
	//re-calc the bounding boxes
	calc_bounding_boxes();

	return;
}

/*basic plate operations*/
//create an empty plate, and return its index
//these model instances which are not in any plates should not be affected also

void PartPlateList::update_plates()
{
    update_all_plates_pos_and_size(true, false);
    set_shapes(m_shape, m_exclude_areas, m_wrapping_exclude_areas, m_extruder_areas, m_extruder_heights, m_logo_texture_filename, m_height_to_lid, m_height_to_rod);
}

int PartPlateList::create_plate(bool adjust_position)
{
	PartPlate* plate = NULL;
	Vec3d origin;
	int new_index;

	new_index = m_plate_list.size();
	if (new_index >= MAX_PLATES_COUNT)
		return -1;
	int cols = compute_colum_count(new_index + 1);
	int old_cols = compute_colum_count(new_index);

	origin = compute_origin(new_index, cols);
	plate = new PartPlate(this, origin, m_plate_width, m_plate_depth, m_plate_height, m_plater, m_model, true, printer_technology);
	assert(plate != NULL);

	if (printer_technology == ptFFF)
	{
		Print* print = new Print();
		GCodeResult* gcode = new GCodeResult();
		m_print_list.emplace(m_print_index, print);
		m_gcode_result_list.emplace(m_print_index, gcode);
		plate->set_print(print, gcode, m_print_index);
		m_print_index++;
	}

	plate->set_filament_count(m_filament_count);

	plate->set_index(new_index);
	Vec2d pos = compute_shape_position(new_index, cols);
	plate->set_shape(m_shape, m_exclude_areas, m_extruder_areas, m_extruder_heights, pos, m_height_to_lid, m_height_to_rod);
	m_plate_list.emplace_back(plate);
	update_plate_cols();
	if (old_cols != cols)
	{
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":old_cols %1% -> new_cols %2%") % old_cols % cols;
		//update the origin of each plate
		update_all_plates_pos_and_size(adjust_position, false);
		set_shapes(m_shape, m_exclude_areas, m_wrapping_exclude_areas, m_extruder_areas, m_extruder_heights, m_logo_texture_filename, m_height_to_lid, m_height_to_rod);

		if (m_plater) {
			Vec2d pos = compute_shape_position(m_current_plate, cols);
			m_plater->set_bed_position(pos);
		}
	}
	else
	{
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": the same cols %1%") % old_cols;
		Vec3d origin2 = compute_origin_for_unprintable();
		unprintable_plate.set_pos_and_size(origin2, m_plate_width, m_plate_depth, m_plate_height, false);

		//update bounding_boxes
		calc_bounding_boxes();
	}

	// update wipe tower config
	if (m_plater) {
		// In GUI mode
        set_default_wipe_tower_pos_for_plate(new_index);
	}

	unprintable_plate.set_index(new_index+1);

	//reload all objects here
	if (adjust_position)
		construct_objects_list_for_new_plate(new_index);

	if (m_plater) {
		// In GUI mode
		wxGetApp().obj_list()->on_plate_added(plate);
	}

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":created a new plate %1%") % new_index;
	return new_index;
}


int PartPlateList::duplicate_plate(int index)
{
    // create a new plate
    int new_plate_index = create_plate(true);
    PartPlate* old_plate = NULL;
    PartPlate* new_plate = NULL;
    old_plate = get_plate(index);
    new_plate = get_plate(new_plate_index);

    // get the offset between plate centers
    Vec3d plate_to_plate_offset = new_plate->m_origin - old_plate->m_origin;

    // iterate over all the objects in this plate
    ModelObjectPtrs obj_ptrs = old_plate->get_objects_on_this_plate();
    for (ModelObject* object : obj_ptrs){
        // copy and move the object to the same relative position in the new plate
        ModelObject* object_copy = m_model->add_object(*object);
        int new_obj_id = new_plate->m_model->objects.size() - 1;
        // go over the instances and pair with the object
        for (size_t new_instance_id = 0; new_instance_id < object_copy->instances.size(); new_instance_id++){
            new_plate->obj_to_instance_set.emplace(std::pair(new_obj_id, new_instance_id));
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": duplicate object into plate: index_pair [%1%,%2%], obj_id %3%") % new_obj_id % new_instance_id % object_copy->id().id;
        }
    }
    new_plate->translate_all_instance(plate_to_plate_offset);
    // update the plates
    wxGetApp().obj_list()->reload_all_plates();
    return new_plate_index;
}


//destroy print's objects and results
int PartPlateList::destroy_print(int print_index)
{
	int result = 0;

	if (print_index >= 0)
	{
		std::map<int, PrintBase*>::iterator it = m_print_list.find(print_index);
		if (it != m_print_list.end())
		{
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":delete Print %1% for print_index %2%") % it->second % print_index;
			delete it->second;
			m_print_list.erase(it);
		}
		else
		{
			BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":can not find Print for print_index %1%") % print_index;
			result = -1;
		}
		std::map<int, GCodeResult*>::iterator it2 = m_gcode_result_list.find(print_index);
		if (it2 != m_gcode_result_list.end())
		{
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":delete GCodeResult %1% for print_index %2%") % it2->second % print_index;
			delete it2->second;
			m_gcode_result_list.erase(it2);
		}
		else
		{
			BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":can not find GCodeResult for print_index %1%") % print_index;
			result = -1;
		}
	}

	return result;
}

//delete a plate by index
//keep its instance at origin position and add them into next plate if have
//update the plate index and position after it
int PartPlateList::delete_plate(int index)
{
	int ret = 0;
	PartPlate* plate = NULL;

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":delete plate %1%, count %2%") % index % m_plate_list.size();
	if (index >= m_plate_list.size())
	{
		BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":can not find plate");
		return -1;
	}
	if (m_plate_list.size() <= 1)
	{
		BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":only one plate left, can not delete");
		return -1;
	}

	plate = m_plate_list[index];
	if (index != plate->get_index())
	{
		BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":plate %1%, has an invalid index %2%") % index % plate->get_index();
		return -1;
	}

	if (m_plater) {
		// In GUI mode
		// BBS: add wipe tower logic
		DynamicConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
		ConfigOptionFloats* wipe_tower_x = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x");
		ConfigOptionFloats* wipe_tower_y = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y");
		// wipe_tower_x and wip_tower_y may be less than plate count in the following case:
		// 1. wipe_tower is enabled after creating new plates
		// 2. wipe tower is not enabled
		if (index < wipe_tower_x->values.size())
			wipe_tower_x->values.erase(wipe_tower_x->values.begin() + index);
		if (index < wipe_tower_y->values.size())
			wipe_tower_y->values.erase(wipe_tower_y->values.begin() + index);
	}

	int cols = compute_colum_count(m_plate_list.size() - 1);
	int old_cols = compute_colum_count(m_plate_list.size());

	m_plate_list.erase(m_plate_list.begin() + index);
	update_plate_cols();
	//update this plate
	//move this plate's instance to the end
	Vec3d current_origin;
	current_origin = compute_origin_for_unprintable();
	plate->set_pos_and_size(current_origin, m_plate_width, m_plate_depth, m_plate_height, true);

	//update the plates after it
	for (unsigned int i = index; i < (unsigned int)m_plate_list.size(); ++i)
	{
		PartPlate* plate = m_plate_list[i];
		assert(plate != NULL);

		plate->set_index(i);
		Vec3d origin = compute_origin(i, m_plate_cols);
		plate->set_pos_and_size(origin, m_plate_width, m_plate_depth, m_plate_height, true);

		//update render shapes
		Vec2d pos = compute_shape_position(i, m_plate_cols);
		plate->set_shape(m_shape, m_exclude_areas, m_extruder_areas, m_extruder_heights, pos, m_height_to_lid, m_height_to_rod);
	}

	//update current_plate if delete current
	if (m_current_plate == index && index == 0) {
		select_plate(0);
	}
	else if (m_current_plate >= index) {
		select_plate(m_current_plate - 1);
	}
	else {
		//delete the plate behind current, just need to update the position of Bed3D
		Vec2d pos = compute_shape_position(m_current_plate, m_plate_cols);
		if (m_plater)
			m_plater->set_bed_position(pos);
	}

	unprintable_plate.set_index(m_plate_list.size());

	if (old_cols != cols)
	{
		//update the origin of each plate
		update_all_plates_pos_and_size();
		set_shapes(m_shape, m_exclude_areas, m_wrapping_exclude_areas, m_extruder_areas, m_extruder_heights, m_logo_texture_filename, m_height_to_lid, m_height_to_rod);
	}
	else
	{
		//update the position of the unprintable plate
		Vec3d origin2 = compute_origin_for_unprintable();
		unprintable_plate.set_pos_and_size(origin2, m_plate_width, m_plate_depth, m_plate_height, true);

		//update bounding_boxes
		calc_bounding_boxes();
	}

	plate->move_instances_to(*(m_plate_list[m_plate_list.size()-1]), unprintable_plate);
	//destroy the print object
	int print_index;
	plate->get_print(nullptr, nullptr, &print_index);
	destroy_print(print_index);

	delete plate;

    // FIX: context of BackgroundSliceProcess and gcode preview need to be updated before ObjectList::reload_all_plates().
#if 0
	if (m_plater != nullptr) {
		// In GUI mode
		wxGetApp().obj_list()->reload_all_plates();
	}
#endif
	return ret;
}

void PartPlateList::delete_selected_plate()
{
	delete_plate(m_current_plate);
}

bool PartPlateList::check_all_plate_local_bed_type(const std::vector<BedType> &cur_bed_types)
{
    std::string bed_type_key = "curr_bed_type";
    bool        is_ok        = true;
    for (int i = 0; i < m_plate_list.size(); i++) {
        PartPlate *plate = m_plate_list[i];
        if (plate->config()  && plate->config()->has(bed_type_key)) {
            BedType bed_type = plate->config()->opt_enum<BedType>(bed_type_key);
            if (bed_type == BedType::btDefault)
                continue;
            bool    find     = false;
            for (auto tmp_type : cur_bed_types) {
                if (bed_type == tmp_type) {
                    find = true;
                    break;
                }
            }
            if (!find) {
                plate->set_bed_type(BedType::btDefault);
                is_ok = false;
            }
        }
    }
    return is_ok;
}

//get a plate pointer by index
PartPlate* PartPlateList::get_plate(int index)
{
	PartPlate* plate = NULL;

	if (index >= m_plate_list.size() || index < 0)
	{
		BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":can not find index %1%, size %2%") % index % m_plate_list.size();
		return NULL;
	}

	plate = m_plate_list[index];
	assert(plate != NULL);

	return plate;
}

PartPlate* PartPlateList::get_selected_plate()
{
	if (m_current_plate < 0 || m_current_plate >= m_plate_list.size()) {
		BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":can not find m_current_plate  %1%, size %2%") % m_current_plate % m_plate_list.size();
		return NULL;
	}
	return m_plate_list[m_current_plate];
}

std::vector<PartPlate*> PartPlateList::get_nonempty_plate_list()
{
	std::vector<PartPlate*> nonempty_plate_list;
	for (auto plate : m_plate_list){
        //if (plate->get_extruders().size() != 0) {
		if (!plate->empty()) { // ORCA counts failed slices as non empty because they have model and should be calculated on total count
			nonempty_plate_list.push_back(plate);
		}
	}
	return nonempty_plate_list;
}

std::vector<const GCodeProcessorResult*> PartPlateList::get_nonempty_plates_slice_results() {
	std::vector<const GCodeProcessorResult*> nonempty_plates_slice_result;
	for (auto plate : get_nonempty_plate_list()) {
		nonempty_plates_slice_result.push_back(plate->get_slice_result());
	}
	return nonempty_plates_slice_result;
}

std::set<int> PartPlateList::get_extruders(bool conside_custom_gcode) const
{
    int plate_count = get_plate_count();
    std::set<int> extruder_ids;

    for (size_t i = 0; i < plate_count; i++) {
        auto plate_extruders = m_plate_list[i]->get_extruders(conside_custom_gcode);
        extruder_ids.insert(plate_extruders.begin(), plate_extruders.end());
    }

    return extruder_ids;
}


//select plate
int PartPlateList::select_plate(int index)
{
	const std::lock_guard<std::mutex> local_lock(m_plates_mutex);
	if (m_plate_list.empty() || index >= m_plate_list.size()) {
		return -1;
	}

	// BBS: erase unnecessary snapshot
	if (get_curr_plate_index() != index && m_intialized) {
		if (m_plater)
			m_plater->take_snapshot("select partplate!");
	}

	std::vector<PartPlate *>::iterator it = m_plate_list.begin();
	for (it = m_plate_list.begin(); it != m_plate_list.end(); it++) {
		(*it)->set_unselected();
	}

	m_current_plate = index;
	m_plate_list[m_current_plate]->set_selected();

	//BBS
	if(m_model)
		m_model->curr_plate_index = index;

	//BBS update bed origin
	if (m_intialized && m_plater) {
		Vec2d pos = compute_shape_position(index, m_plate_cols);
        m_plater->set_bed_position(pos);
		//wxQueueEvent(m_plater, new SimpleEvent(EVT_GLCANVAS_PLATE_SELECT));
	}

	return 0;
}

void PartPlateList::set_hover_id(int id)
{
	int index = id / PartPlate::GRABBER_COUNT;
	int sub_hover_id = id % PartPlate::GRABBER_COUNT;
	m_plate_list[index]->set_hover_id(sub_hover_id);
}

void PartPlateList::reset_hover_id()
{
	const std::lock_guard<std::mutex> local_lock(m_plates_mutex);
	std::vector<PartPlate*>::iterator it = m_plate_list.begin();
	for (it = m_plate_list.begin(); it != m_plate_list.end(); it++) {
		(*it)->set_hover_id(-1);
	}
}

bool PartPlateList::intersects(const BoundingBoxf3& bb)
{
	bool result = false;
	std::vector<PartPlate*>::iterator it = m_plate_list.begin();
	for (it = m_plate_list.begin(); it != m_plate_list.end(); it++) {
		if ((*it)->intersects(bb)) {
			result = true;
		}
	}
	return result;
}

bool PartPlateList::contains(const BoundingBoxf3& bb)
{
	bool result = false;
	std::vector<PartPlate*>::iterator it = m_plate_list.begin();
	for (it = m_plate_list.begin(); it != m_plate_list.end(); it++) {
		if ((*it)->contains(bb)) {
			result = true;
		}
	}
	return result;
}

double PartPlateList::plate_stride_x()
{
	//const auto plate_shape = Slic3r::Polygon::new_scale(m_shape);
	//double plate_width = plate_shape.bounding_box().size().x();
	//return unscaled<double>((1. + LOGICAL_PART_PLATE_GAP) * plate_width);
	return m_plate_width * (1. + LOGICAL_PART_PLATE_GAP);
}

double PartPlateList::plate_stride_y()
{
	//const auto plate_shape = Slic3r::Polygon::new_scale(m_shape);
	//double plate_depth = plate_shape.bounding_box().size().y();
	//return unscaled<double>((1. + LOGICAL_PART_PLATE_GAP) * plate_depth);
	return m_plate_depth * (1. + LOGICAL_PART_PLATE_GAP);
}

//get the plate counts, not including the invalid plate
int PartPlateList::get_plate_count() const
{
	int ret = 0;

	ret = m_plate_list.size();

	return ret;
}

//update the plate cols due to plate count change
void PartPlateList::update_plate_cols()
{
	m_plate_count = m_plate_list.size();

	m_plate_cols = compute_colum_count(m_plate_count);
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":m_plate_count %1%, m_plate_cols change to %2%") % m_plate_count % m_plate_cols;
	return;
}

void PartPlateList::update_all_plates_pos_and_size(bool adjust_position, bool with_unprintable_move, bool switch_plate_type, bool do_clear)
{
	Vec3d origin1, origin2;
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		PartPlate* plate = m_plate_list[i];
		assert(plate != NULL);

		//compute origin1 for PartPlate
		origin1 = compute_origin(i, m_plate_cols);
		plate->set_pos_and_size(origin1, m_plate_width, m_plate_depth, m_plate_height, adjust_position, do_clear);

		// set default wipe pos when switch plate
        if (switch_plate_type && m_plater/* && plate->get_used_extruders().size() <= 0*/) {
			set_default_wipe_tower_pos_for_plate(i);
		}
	}

	origin2 = compute_origin_for_unprintable();
	unprintable_plate.set_pos_and_size(origin2, m_plate_width, m_plate_depth, m_plate_height, with_unprintable_move);
}

//move the plate to position index
int PartPlateList::move_plate_to_index(int old_index, int new_index)
{
	int ret = 0, delta;
	Vec3d origin;


	if (old_index == new_index)
	{
		BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":should not happen, the same index %1%") % old_index;
		return -1;
	}

	if (old_index < new_index)
	{
		delta = 1;
	}
	else
	{
		delta = -1;
	}

	PartPlate* plate = m_plate_list[old_index];
	//update the plates between old_index and new_index
	for (unsigned int i = (unsigned int)old_index; i != (unsigned int)new_index; i = i + delta)
	{
		m_plate_list[i] = m_plate_list[i + delta];
		m_plate_list[i]->set_index(i);

		origin = compute_origin(i, m_plate_cols);
		m_plate_list[i]->set_pos_and_size(origin, m_plate_width, m_plate_depth, m_plate_height, true);
	}
	origin = compute_origin(new_index, m_plate_cols);
	m_plate_list[new_index] = plate;
	plate->set_index(new_index);
	plate->set_pos_and_size(origin, m_plate_width, m_plate_depth, m_plate_height, true);

	//update the new plate index
	m_current_plate = new_index;

	return ret;
}

//lock plate
int PartPlateList::lock_plate(int index, bool state)
{
	int ret = 0;
	PartPlate* plate = NULL;

	plate = get_plate(index);
	if (!plate)
	{
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":can not get plate for index %1%, size %2%") % index % m_plate_list.size();
		return -1;
	}
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":lock plate %1%, to state %2%") % index % state;

	plate->lock(state);

	return ret;
}

//find plate by print index, return -1 if not found
int PartPlateList::find_plate_by_print_index(int print_index)
{
	int plate_index = -1;

	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		PartPlate* plate = m_plate_list[i];

		if (plate->m_print_index == print_index)
		{
			plate_index = i;
			break;
		}
	}

	return plate_index;
}

/*instance related operations*/
//find instance in which plate, return -1 when not found
//this function only judges whether it is intersect with plate
int PartPlateList::find_instance(int obj_id, int instance_id)
{
	int ret = -1;

	//update the plates after it
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		PartPlate* plate = m_plate_list[i];
		assert(plate != NULL);

		if (plate->contain_instance(obj_id, instance_id))
			return i;
	}

	//return -1 for not found
	return ret;
}

/*instance related operations*/
//find instance in which plate, return -1 when not found
//this function only judges whether it is intersect with plate
int PartPlateList::find_instance(BoundingBoxf3& bounding_box)
{
	int ret = -1;

	//update the plates after it
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		PartPlate* plate = m_plate_list[i];
		assert(plate != NULL);

		if (plate->intersects(bounding_box))
			return i;
	}

	//return -1 for not found
	return ret;
}

//this function not only judges whether it is intersect with plate, but also judges whether it is fully included in plate
//returns -1 when can not find any plate
int PartPlateList::find_instance_belongs(int obj_id, int instance_id)
{
	int ret = -1;

	//update the plates after it
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		PartPlate* plate = m_plate_list[i];
		assert(plate != NULL);

		if (plate->contain_instance_totally(obj_id, instance_id))
			return i;
	}

	//return -1 for not found
	return ret;
}

//notify instance's update, need to refresh the instance in plates
//newly added or modified
int PartPlateList::notify_instance_update(int obj_id, int instance_id, bool is_new)
{
	int ret = 0, index;
	PartPlate* plate = NULL;
	ModelObject* object = NULL;

	if ((obj_id >= 0) && (obj_id < m_model->objects.size()))
	{
		object = m_model->objects[obj_id];
	}
	else if (obj_id >= 1000 && obj_id < 1000 + m_plate_count) {
		//wipe tower updates
		PartPlate* plate = m_plate_list[obj_id - 1000];
		plate->update_slice_result_valid_state( false );
		plate->thumbnail_data.reset();
        plate->no_light_thumbnail_data.reset();
		plate->top_thumbnail_data.reset();
		plate->pick_thumbnail_data.reset();

		return 0;
	}
    else
		return -1;

	BoundingBoxf3 boundingbox = object->instance_convex_hull_bounding_box(instance_id);

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": obj_id %1%, instance_id %2%") % obj_id % instance_id;
	index = find_instance(obj_id, instance_id);
	if (index != -1)
	{
		//found it added before
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found it in previous plate %1%") % index;
		plate = m_plate_list[index];
		if (!plate->intersect_instance(obj_id, instance_id, &boundingbox))
		{
			//not include anymore, remove it from original plate
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": not in plate %1% anymore, remove it") % index;
			plate->remove_instance(obj_id, instance_id);
		}
		else
		{
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": still in original plate %1%, no need to be updated") % index;
			plate->update_instance_exclude_status(obj_id, instance_id, &boundingbox);
			plate->update_states();
			plate->update_slice_result_valid_state();
			plate->thumbnail_data.reset();
            plate->no_light_thumbnail_data.reset();
			plate->top_thumbnail_data.reset();
			plate->pick_thumbnail_data.reset();
			return 0;
		}
		plate->update_slice_result_valid_state();
		plate->thumbnail_data.reset();
        plate->no_light_thumbnail_data.reset();
		plate->top_thumbnail_data.reset();
		plate->pick_thumbnail_data.reset();
	}
	else if (unprintable_plate.contain_instance(obj_id, instance_id))
	{
		//found it in the unprintable plate
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found it in unprintable plate");
		if (!unprintable_plate.intersect_instance(obj_id, instance_id, &boundingbox))
		{
			//not include anymore, remove it from original plate
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": not in unprintable plate anymore, remove it");
			unprintable_plate.remove_instance(obj_id, instance_id);
		}
		else
		{
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": still in unprintable plate, no need to be updated");
			return 0;
		}
	}

	auto is_object_config_compatible_with_spiral_vase = [](ModelObject* object) {
		const DynamicPrintConfig& config = object->config.get();
		if (config.has("wall_loops") && config.opt_int("wall_loops") == 1 &&
			config.has("top_shell_layers") && config.opt_int("top_shell_layers") == 0 &&
			config.has("sparse_infill_density") && config.option<ConfigOptionPercent>("sparse_infill_density")->value == 0 &&
			config.has("enable_support") && !config.opt_bool("enable_support") &&
			config.has("enforce_support_layers") && config.opt_int("enforce_support_layers") == 0 &&
			config.has("ensure_vertical_shell_thickness") && config.opt_bool("ensure_vertical_shell_thickness") &&
			config.has("detect_thin_wall") && !config.opt_bool("detect_thin_wall") &&
			config.has("timelapse_type") && config.opt_enum<TimelapseType>("timelapse_type") == TimelapseType::tlTraditional)
			return true;
		else
			return false;
	};

	//try to find a new plate
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		PartPlate* plate = m_plate_list[i];
		assert(plate != NULL);

		if (plate->intersect_instance(obj_id, instance_id, &boundingbox))
		{
			//found a new plate, add it to plate
			plate->add_instance(obj_id, instance_id, false, &boundingbox);

			// spiral mode, update object setting
			if (plate->config()->has("spiral_mode") && plate->config()->opt_bool("spiral_mode") && !is_object_config_compatible_with_spiral_vase(object)) {
				if (!is_new) {
					auto answer = static_cast<TabPrintPlate*>(wxGetApp().plate_tab)->show_spiral_mode_settings_dialog(true);
					if (answer == wxID_YES) {
						plate->set_vase_mode_related_object_config(obj_id);
					}
				}
				else {
					plate->set_vase_mode_related_object_config(obj_id);
				}
			}

			plate->update_slice_result_valid_state();
			plate->thumbnail_data.reset();
            plate->no_light_thumbnail_data.reset();
			plate->top_thumbnail_data.reset();
			plate->pick_thumbnail_data.reset();
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": add it to new plate %1%") % i;
			return 0;
		}
	}

	if (unprintable_plate.intersect_instance(obj_id, instance_id, &boundingbox))
	{
		//found in unprintable plate, add it to plate
		unprintable_plate.add_instance(obj_id, instance_id, false, &boundingbox);
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": add it to unprintable plate");
		return 0;
	}

	return 0;
}

//notify instance is removed
int PartPlateList::notify_instance_removed(int obj_id, int instance_id)
{
	int ret = 0, index, instance_to_delete = instance_id;
	PartPlate* plate = NULL;

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": obj_id %1%, instance_id %2%") % obj_id % instance_id;
	if (instance_id == -1) {
		instance_to_delete = 0;
	}
	index = find_instance(obj_id, instance_to_delete);
	if (index != -1)
	{
		//found it added before
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found it in plate %1%, remove it") % index;
		plate = m_plate_list[index];
		plate->remove_instance(obj_id, instance_to_delete);
		plate->update_slice_result_valid_state();
		plate->thumbnail_data.reset();
        plate->no_light_thumbnail_data.reset();
		plate->top_thumbnail_data.reset();
		plate->pick_thumbnail_data.reset();
	}

	if (unprintable_plate.contain_instance(obj_id, instance_to_delete))
	{
		//found in unprintable plate, add it to plate
		unprintable_plate.remove_instance(obj_id, instance_to_delete);
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found it in unprintable plate, remove it");
	}

	if (instance_id == -1) {
		//update all the obj_ids which is bigger
		for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
		{
			PartPlate* plate = m_plate_list[i];
			assert(plate != NULL);

			plate->update_object_index(obj_id, m_model->objects.size());
		}
		unprintable_plate.update_object_index(obj_id, m_model->objects.size());
	}

	return 0;
}

//add instance to special plate, need to remove from the original plate
//called from the right-mouse menu when a instance selected
int PartPlateList::add_to_plate(int obj_id, int instance_id, int plate_id)
{
	int ret = 0, index;
	PartPlate* plate = NULL;

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": plate_id %1%, found obj_id %2%, instance_id %3%") % plate_id % obj_id % instance_id;
	index = find_instance(obj_id, instance_id);
	if (index != -1)
	{
		//found it added before
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found it in previous plate %1%") % index;
		if (index != plate_id)
		{
			//remove it from original plate first
			plate = m_plate_list[index];
			plate->remove_instance(obj_id, instance_id);
		}
		else
		{
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": already in this plate, no need to be added");
			return 0;
		}
	}
	else
	{
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": not added to plate before, add it to center");
	}
	plate = get_plate(plate_id);
	if (!plate)
	{
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":can not get plate for index %1%, size %2%") % index % m_plate_list.size();
		return -1;
	}
	ret = plate->add_instance(obj_id, instance_id, true);

	return ret;
}

//reload all objects
int PartPlateList::reload_all_objects(bool except_locked, int plate_index)
{
	int ret = 0;
	unsigned int i, j, k;

	clear(false, false, except_locked, plate_index);

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": m_model->objects.size() is %1%") % m_model->objects.size();
	//try to find a new plate
	for (i = 0; i < (unsigned int)m_model->objects.size(); ++i)
	{
		ModelObject* object = m_model->objects[i];
		for (j = 0; j < (unsigned int)object->instances.size(); ++j)
		{
			ModelInstance* instance = object->instances[j];
			BoundingBoxf3 boundingbox = object->instance_convex_hull_bounding_box(j);
			for (k = 0; k < (unsigned int)m_plate_list.size(); ++k)
			{
				PartPlate* plate = m_plate_list[k];
				assert(plate != NULL);

				if (plate->intersect_instance(i, j, &boundingbox))
				{
					//found a new plate, add it to plate
					plate->add_instance(i, j, false, &boundingbox);
					BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found plate_id %1%, for obj_id %2%, instance_id %3%") % k % i % j;

					//need to judge whether this instance has an outer part
					/*if (plate->check_outside(i, j))
					{
						plate->m_ready_for_slice = false;
					}*/
					break;
				}
			}

			if ((k == m_plate_list.size()) && (unprintable_plate.intersect_instance(i, j, &boundingbox)))
			{
				//found in unprintable plate, add it to plate
				unprintable_plate.add_instance(i, j, false, &boundingbox);
				BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found in unprintable plate, obj_id %1%, instance_id %2%") % i % j;
			}
		}

	}

	return ret;
}

//reload objects for newly created plate
int PartPlateList::construct_objects_list_for_new_plate(int plate_index)
{
	int ret = 0;
	unsigned int i, j, k;
	PartPlate* new_plate = m_plate_list[plate_index];
	bool already_included;

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": m_model->objects.size() is %1%") % m_model->objects.size();
	unprintable_plate.clear();
	//try to find a new plate
	for (i = 0; i < (unsigned int)m_model->objects.size(); ++i)
	{
		ModelObject* object = m_model->objects[i];
		for (j = 0; j < (unsigned int)object->instances.size(); ++j)
		{
			ModelInstance* instance = object->instances[j];
			already_included = false;

			for (k = 0; k < (unsigned int)plate_index; ++k)
			{
				PartPlate* plate = m_plate_list[k];
				if (plate->contain_instance(i, j))
				{
					already_included = true;
					break;
				}
			}

			if (already_included)
				continue;

			BoundingBoxf3 boundingbox = object->instance_convex_hull_bounding_box(j);
			if (new_plate->intersect_instance(i, j, &boundingbox))
			{
				//found a new plate, add it to plate
				ret |= new_plate->add_instance(i, j, false, &boundingbox);
				BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": added to plate_id %1%, for obj_id %2%, instance_id %3%") % plate_index % i % j;

				continue;
			}

			if ( (unprintable_plate.intersect_instance(i, j, &boundingbox)))
			{
				//found in unprintable plate, add it to plate
				unprintable_plate.add_instance(i, j, false, &boundingbox);
				BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found in unprintable plate, obj_id %1%, instance_id %2%") % i % j;
			}
		}
	}

	return ret;
}


//compute the plate index
int PartPlateList::compute_plate_index(arrangement::ArrangePolygon& arrange_polygon)
{
	int row, col;

	float col_value = (unscale<double>(arrange_polygon.translation(X))) / plate_stride_x();
	float row_value = (plate_stride_y() - unscale<double>(arrange_polygon.translation(Y))) / plate_stride_y();

	row = round(row_value);
	col = round(col_value);

	return row * m_plate_cols + col;
}

//preprocess a arrangement::ArrangePolygon, return true if it is in a locked plate
bool PartPlateList::preprocess_arrange_polygon(int obj_index, int instance_index, arrangement::ArrangePolygon& arrange_polygon, bool selected)
{
	bool locked = false;
	int lockplate_cnt = 0;

	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		if (m_plate_list[i]->contain_instance(obj_index, instance_index))
		{
			if (m_plate_list[i]->is_locked())
			{
				locked = true;
				arrange_polygon.bed_idx = i;
				arrange_polygon.row = i / m_plate_cols;
				arrange_polygon.col = i % m_plate_cols;
				arrange_polygon.translation(X) -= scaled<double>(plate_stride_x() * arrange_polygon.col);
				arrange_polygon.translation(Y) += scaled<double>(plate_stride_y() * arrange_polygon.row);
			}
			else
			{
				if (!selected)
				{
					//will be treated as fixeditem later
					arrange_polygon.bed_idx = i - lockplate_cnt;
					arrange_polygon.row = i / m_plate_cols;
					arrange_polygon.col = i % m_plate_cols;
					arrange_polygon.translation(X) -= scaled<double>(plate_stride_x() * arrange_polygon.col);
					arrange_polygon.translation(Y) += scaled<double>(plate_stride_y() * arrange_polygon.row);
				}
			}
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": obj_id %1% instance_id %2% already in plate %3%, locked %4%, row %5%, col %6%\n") % obj_index % instance_index % i % locked % arrange_polygon.row % arrange_polygon.col;
			return locked;
		}
		if (m_plate_list[i]->is_locked())
			lockplate_cnt++;
	}
	//not be contained by any plates
	if (!selected)
		arrange_polygon.bed_idx = PartPlateList::MAX_PLATES_COUNT;

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": not in any plates, bed_idx %1%, translation(x) %2%, (y) %3%") % arrange_polygon.bed_idx % unscale<double>(arrange_polygon.translation(X)) % unscale<double>(arrange_polygon.translation(Y));

	return locked;
}

//preprocess a arrangement::ArrangePolygon, return true if it is not in current plate
bool PartPlateList::preprocess_arrange_polygon_other_locked(int obj_index, int instance_index, arrangement::ArrangePolygon& arrange_polygon, bool selected)
{
	bool locked = false;

	if (selected)
	{
		//arrange_polygon.translation(X) -= scaled<double>(plate_stride_x() * m_current_plate);
	}
	else
	{
		locked = true;
		for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
		{
			if (m_plate_list[i]->contain_instance(obj_index, instance_index))
			{
				arrange_polygon.bed_idx = i;
				arrange_polygon.row = i / m_plate_cols;
				arrange_polygon.col = i % m_plate_cols;
				arrange_polygon.translation(X) -= scaled<double>(plate_stride_x() * arrange_polygon.col);
				arrange_polygon.translation(Y) += scaled<double>(plate_stride_y() * arrange_polygon.row);
				//BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": obj_id %1% instance_id %2% in plate %3%, locked %4%, row %5%, col %6%\n") % obj_index % instance_index % i % locked % arrange_polygon.row % arrange_polygon.col;
				return locked;
			}
		}
		arrange_polygon.bed_idx = PartPlateList::MAX_PLATES_COUNT;
	}
	return locked;
}

bool PartPlateList::preprocess_exclude_areas(arrangement::ArrangePolygons &unselected, bool enable_wrapping_detect, int num_plates, float inflation)
{
	bool added = false;

	// wrapping detection area
    if (enable_wrapping_detect)
	{
        if (!m_wrapping_exclude_areas.empty())
		{
			Polygon ap{};
            for (const Vec2d &p : m_wrapping_exclude_areas)
			{
                ap.append({scale_(p(0)), scale_(p(1))});
            }

			for(int j = 0; j < num_plates; j++)
			{
				arrangement::ArrangePolygon ret;
				ret.poly.contour = ap;
				ret.translation  = Vec2crd(0, 0);
				ret.rotation     = 0.0f;
				ret.is_virt_object = true;
				ret.bed_idx      = j;
				ret.height      = 1;
				ret.name = "WrappingRegion";
				ret.inflation = inflation;

				unselected.emplace_back(std::move(ret));
			}
		}
		added = true;
	}

	// excluded area
	if (m_exclude_areas.size() > 0)
	{
		//has exclude areas
		PartPlate *plate = m_plate_list[0];

		for (int index = 0; index < plate->m_exclude_bounding_box.size(); index ++)
		{
			Polygon ap({
				{scaled(plate->m_exclude_bounding_box[index].min.x()), scaled(plate->m_exclude_bounding_box[index].min.y())},
				{scaled(plate->m_exclude_bounding_box[index].max.x()), scaled(plate->m_exclude_bounding_box[index].min.y())},
				{scaled(plate->m_exclude_bounding_box[index].max.x()), scaled(plate->m_exclude_bounding_box[index].max.y())},
				{scaled(plate->m_exclude_bounding_box[index].min.x()), scaled(plate->m_exclude_bounding_box[index].max.y())}
				});

			for (int j = 0; j < num_plates; j++)
			{
				arrangement::ArrangePolygon ret;
				ret.poly.contour = ap;
				ret.translation  = Vec2crd(0, 0);
				ret.rotation     = 0.0f;
				ret.is_virt_object = true;
				ret.bed_idx      = j;
				ret.height      = 1;
				ret.name = "ExcludedRegion" + std::to_string(index);
				ret.inflation = inflation;

				unselected.emplace_back(std::move(ret));
			}
			added = true;
		}
	}

	return added;
}

bool PartPlateList::preprocess_nonprefered_areas(arrangement::ArrangePolygons& regions, int num_plates, float inflation)
{
	bool added = false;

	std::vector<BoundingBoxf> nonprefered_regions;
	nonprefered_regions.emplace_back(Vec2d{ 18,0 }, Vec2d{ 240,15 }); // new extrusion & hand-eye calibration region

	//has exclude areas
	PartPlate* plate = m_plate_list[0];
	for (int index = 0; index < nonprefered_regions.size(); index++)
	{
		Polygon ap = scaled(nonprefered_regions[index]).polygon();
		for (int j = 0; j < num_plates; j++)
		{
			arrangement::ArrangePolygon ret;
			ret.poly.contour = ap;
			ret.translation = Vec2crd(0, 0);
			ret.rotation = 0.0f;
			ret.is_virt_object = true;
            ret.is_extrusion_cali_object = true;
			ret.bed_idx = j;
			ret.height = 1;
			ret.name = "NonpreferedRegion" + std::to_string(index);
			ret.inflation = inflation;

			regions.emplace_back(std::move(ret));
		}
		added = true;
	}
	return added;
}


//postprocess an arrangement::ArrangePolygon's bed index
void PartPlateList::postprocess_bed_index_for_selected(arrangement::ArrangePolygon& arrange_polygon)
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": bed_idx %1%, locked_plate %2%, translation(x) %3%, (y) %4%") % arrange_polygon.bed_idx % arrange_polygon.locked_plate % unscale<double>(arrange_polygon.translation(X)) % unscale<double>(arrange_polygon.translation(Y));

	if (arrange_polygon.bed_idx == -1)
	{
		//outarea for large object, can not process here for the plate number maybe increased later
		BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": can not be arranged inside plate!");
		return;
	}

	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		if (m_plate_list[i]->is_locked())
		{
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found locked_plate %1%, increate index by 1") % i;
			//arrange_polygon.translation(X) += scaled<double>(plate_stride_x());
			arrange_polygon.bed_idx += 1;
			//offset_x += scaled<double>(plate_stride_x());
		}
		else
		{
			//judge whether it is at the left side of the plate border
			if (arrange_polygon.bed_idx <= i)
			{
				BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":found in plate_index %1%, bed_idx %2%") % i % arrange_polygon.bed_idx;
				return;
			}
		}
	}

	//create a new plate which can hold this arrange_polygon
	int plate_index = create_plate(false);

	while (plate_index != -1)
	{
		if (arrange_polygon.bed_idx <= plate_index)
		{
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":new plate_index %1%, matches bed_idx %2%") % plate_index % arrange_polygon.bed_idx;
			break;
		}

		plate_index = create_plate(false);
	}

	return;
}

//postprocess an arrangement::ArrangePolygon's bed index
void PartPlateList::postprocess_bed_index_for_unselected(arrangement::ArrangePolygon& arrange_polygon)
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": bed_idx %1%, locked_plate %2%, translation(x) %3%, (y) %4%") % arrange_polygon.bed_idx % arrange_polygon.locked_plate % unscale<double>(arrange_polygon.translation(X)) % unscale<double>(arrange_polygon.translation(Y));

	if (arrange_polygon.bed_idx == PartPlateList::MAX_PLATES_COUNT)
		return;

	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		if (m_plate_list[i]->is_locked())
		{
			BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found locked_plate %1%, increate index by 1") % i;
			//arrange_polygon.translation(X) += scaled<double>(plate_stride_x());
			arrange_polygon.bed_idx += 1;
			//offset_x += scaled<double>(plate_stride_x());
		}
		else
		{
			//judge whether it is at the left side of the plate border
			if (arrange_polygon.bed_idx <= i)
			{
				BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":found in plate_index %1%, bed_idx %2%") % i % arrange_polygon.bed_idx;
				return;
			}
		}
	}

	return;
}

//postprocess an arrangement::ArrangePolygon, other instances are under locked states
void PartPlateList::postprocess_bed_index_for_current_plate(arrangement::ArrangePolygon& arrange_polygon)
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": bed_idx %1%, locked_plate %2%, translation(x) %3%, (y) %4%") % arrange_polygon.bed_idx % arrange_polygon.locked_plate % unscale<double>(arrange_polygon.translation(X)) % unscale<double>(arrange_polygon.translation(Y));

	if (arrange_polygon.bed_idx == -1)
	{
		//outarea for large object
		BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": can not be arranged inside plate!");
	}
	else if (arrange_polygon.bed_idx == 0)
		arrange_polygon.bed_idx += m_current_plate;
	else
		arrange_polygon.bed_idx = m_plate_list.size();

	return;
}

//postprocess an arrangement::ArrangePolygon
void PartPlateList::postprocess_arrange_polygon(arrangement::ArrangePolygon& arrange_polygon, bool selected)
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": bed_idx %1%, selected %2%, translation(x) %3%, (y) %4%") % arrange_polygon.bed_idx % selected % unscale<double>(arrange_polygon.translation(X)) % unscale<double>(arrange_polygon.translation(Y));

	if ((selected) || (arrange_polygon.bed_idx != PartPlateList::MAX_PLATES_COUNT))
	{
		if (arrange_polygon.bed_idx == -1)
		{
			// outarea for large object
			arrange_polygon.bed_idx = m_plate_list.size();
			BoundingBox apbox = get_extents(arrange_polygon.transformed_poly());  // the item may have been rotated
			auto        apbox_size = apbox.size();

			arrange_polygon.translation(X) = 0.5 * apbox_size[0];
			arrange_polygon.translation(Y) = scaled<double>(static_cast<double>(m_plate_depth)) - 0.5 * apbox_size[1];
		}

		arrange_polygon.row = arrange_polygon.bed_idx / m_plate_cols;
		arrange_polygon.col = arrange_polygon.bed_idx % m_plate_cols;
		arrange_polygon.translation(X) += scaled<double>(plate_stride_x() * arrange_polygon.col);
		arrange_polygon.translation(Y) -= scaled<double>(plate_stride_y() * arrange_polygon.row);
	}

	return;
}

/*rendering related functions*/
//render
void PartPlateList::render(const Transform3d& view_matrix, const Transform3d& projection_matrix, bool bottom, bool only_current, bool only_body, int hover_id, bool render_cali, bool show_grid)
{
	const std::lock_guard<std::mutex> local_lock(m_plates_mutex);
	std::vector<PartPlate*>::iterator it = m_plate_list.begin();

	int plate_hover_index = -1;
	int plate_hover_action = -1;
	if (hover_id != -1) {
		plate_hover_index = hover_id / PartPlate::GRABBER_COUNT;
		plate_hover_action = hover_id % PartPlate::GRABBER_COUNT;
	}

	static bool last_dark_mode_status = m_is_dark;
	if (m_is_dark != last_dark_mode_status) {
		last_dark_mode_status = m_is_dark;
		generate_icon_textures();
	} else if(m_del_texture.get_id() == 0)
		generate_icon_textures();
	for (it = m_plate_list.begin(); it != m_plate_list.end(); it++) {
		int current_index = (*it)->get_index();
		if (only_current && (current_index != m_current_plate))
			continue;
		if (current_index == m_current_plate) {
			PartPlate::HeightLimitMode height_mode = (only_current)?PartPlate::HEIGHT_LIMIT_NONE:m_height_limit_mode;
			if (plate_hover_index == current_index)
                (*it)->render(view_matrix, projection_matrix, bottom, only_body, false, height_mode, plate_hover_action, render_cali, show_grid);
			else
                (*it)->render(view_matrix, projection_matrix, bottom, only_body, false, height_mode, -1, render_cali, show_grid);
		}
		else {
			if (plate_hover_index == current_index)
				(*it)->render(view_matrix, projection_matrix, bottom, only_body, false, PartPlate::HEIGHT_LIMIT_NONE, plate_hover_action, render_cali, show_grid);
			else
                (*it)->render(view_matrix, projection_matrix, bottom, only_body, false, PartPlate::HEIGHT_LIMIT_NONE, -1, render_cali, show_grid);
		}
	}
}

/*int PartPlateList::select_plate_by_hover_id(int hover_id)
{
	int index = hover_id / PartPlate::GRABBER_COUNT;
	int sub_hover_id = hover_id % PartPlate::GRABBER_COUNT;
	if (sub_hover_id == 0) {
		select_plate(index);
	}
	else if (sub_hover_id == 1) {
		if (m_current_plate == 0) {
			select_plate(0);
		}
		else {
			select_plate(index - 1);
		}
	}
	else if (sub_hover_id == 2) {
		if (m_current_plate == (get_plate_count() - 1)) {
			select_plate(m_current_plate);
		}
		else {
			select_plate(index + 1);
		}
	}
	else {
		return -1;
	}
	return 0;
}*/

void PartPlateList::set_render_option(bool bedtype_texture, bool plate_settings)
{
    render_bedtype_logo = bedtype_texture;
    render_plate_settings = plate_settings;
}

int PartPlateList::select_plate_by_obj(int obj_index, int instance_index)
{
	int ret = 0, index;
	PartPlate* plate = NULL;

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": obj_id %1%, instance_id %2%") % obj_index % instance_index;
	index = find_instance(obj_index, instance_index);
	if (index != -1)
	{
		//found it in plate
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": found it in plate %1%") % index;
		select_plate(index);
		return 0;
	}
	return -1;
}

void PartPlateList::calc_bounding_boxes()
{
	m_bounding_box.reset();
	std::vector<PartPlate*>::iterator it = m_plate_list.begin();
	for (it = m_plate_list.begin(); it != m_plate_list.end(); it++) {
		m_bounding_box.merge((*it)->get_bounding_box(true));
	}
}

void PartPlateList::select_plate_view()
{
	if (m_current_plate < 0 || m_current_plate >= m_plate_list.size()) return;

	Vec3d target = m_plate_list[m_current_plate]->get_bounding_box(false).center();
	Vec3d position(target.x(), target.y(), m_plater->get_camera().get_distance());
	m_plater->get_camera().look_at(position, target, Vec3d::UnitY());
	m_plater->get_camera().select_view("topfront");
}

bool PartPlateList::set_shapes(const Pointfs              &shape,
                               const Pointfs              &exclude_areas,
                               const Pointfs              &wrapping_exclude_areas,
                               const std::vector<Pointfs> &extruder_areas,
                               const std::vector<double>  &extruder_heights,
                               const std::string          &texture_filename,
                               float                       height_to_lid,
                               float                       height_to_rod)
{
	const std::lock_guard<std::mutex> local_lock(m_plates_mutex);
	m_shape = shape;
	m_exclude_areas = exclude_areas;
    m_wrapping_exclude_areas = wrapping_exclude_areas;
	m_extruder_areas = extruder_areas;
	m_extruder_heights = extruder_heights;
	m_height_to_lid = height_to_lid;
	m_height_to_rod = height_to_rod;

	double stride_x = plate_stride_x();
	double stride_y = plate_stride_y();
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		PartPlate* plate = m_plate_list[i];
		assert(plate != NULL);

		Vec2d pos;

		pos = compute_shape_position(i, m_plate_cols);
		plate->set_shape(shape, exclude_areas, extruder_areas, extruder_heights, pos, height_to_lid, height_to_rod);
	}
	is_load_bedtype_textures = false; //reload textures
    is_load_extruder_only_area_textures = false; // reload textures
	calc_bounding_boxes();

	update_logo_texture_filename(texture_filename);

	return true;
}

void PartPlateList::update_logo_texture_filename(const std::string &texture_filename)
{
    auto check_texture = [](const std::string &texture) {
        boost::system::error_code ec; // so the exists call does not throw (e.g. after a permission problem)
        return !texture.empty() && (boost::algorithm::iends_with(texture, ".png") || boost::algorithm::iends_with(texture, ".svg")) && boost::filesystem::exists(texture, ec);
    };
    if (!texture_filename.empty() && !check_texture(texture_filename)) {
		m_logo_texture_filename = "";
        BOOST_LOG_TRIVIAL(error) << "Unable to load bed texture: " << texture_filename;
    } else {
        m_logo_texture_filename = texture_filename;
        Utils::slash_to_back_slash(m_logo_texture_filename);
    }
}

/*slice related functions*/
//update current slice context into backgroud slicing process
void PartPlateList::update_slice_context_to_current_plate(BackgroundSlicingProcess& process)
{
	PartPlate* current_plate;

	current_plate = m_plate_list[m_current_plate];
	assert(current_plate != NULL);

	current_plate->update_slice_context(process);

	return;
}

//return the current fff print object
Print& PartPlateList::get_current_fff_print() const
{
	PartPlate* current_plate;
	Print* print;

	current_plate = m_plate_list[m_current_plate];
	//BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":m_current_plate %1%, current_plate %2%") % m_current_plate % current_plate;
	assert(current_plate != NULL);

	current_plate->get_print((PrintBase **)&print, nullptr, nullptr);

	return *print;
}

//return the slice result
GCodeProcessorResult* PartPlateList::get_current_slice_result() const
{
	PartPlate* current_plate;

	current_plate = m_plate_list[m_current_plate];
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":m_current_plate %1%, current_plate %2%") % m_current_plate % current_plate;
	assert(current_plate != NULL);

	return current_plate->get_slice_result();
}

//invalid all the plater's slice result
void PartPlateList::invalid_all_slice_result()
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": plates count %1%") % m_plate_list.size();
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		m_plate_list[i]->update_slice_result_valid_state(false);
	}

	return;
}

//check whether all plates's slice result valid
bool PartPlateList::is_all_slice_results_valid() const
{
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		if (!m_plate_list[i]->is_slice_result_valid())
			return false;
	}
	return true;
}

//check whether all plates's slice result valid for print
bool PartPlateList::is_all_slice_results_ready_for_print() const
{
    bool res = false;

    for (unsigned int i = 0; i < (unsigned int) m_plate_list.size(); ++i) {
        if (!m_plate_list[i]->empty()) {
            if (m_plate_list[i]->is_all_instances_unprintable()) {
				continue;
			}
            if (!m_plate_list[i]->is_slice_result_ready_for_print()) {
				return false;
			}
        }
        if (m_plate_list[i]->is_slice_result_ready_for_print()) {
			res = true;
		}
    }

    return res;
}

//check whether all plates' slice result valid for export to file
bool PartPlateList::is_all_slice_result_ready_for_export() const
{
	bool res = false;

    for (unsigned int i = 0; i < (unsigned int) m_plate_list.size(); ++i) {
        if (!m_plate_list[i]->empty()) {
            if (m_plate_list[i]->is_all_instances_unprintable()) {
				continue;
			}
            if (!m_plate_list[i]->is_slice_result_ready_for_print()) {
				return false;
			}
        }
        if (m_plate_list[i]->is_slice_result_ready_for_print()) {
			if (!m_plate_list[i]->has_printable_instances()) {
				return false;
			}
			res = true;
		}
    }

    return res;
}

//check whether all plates ready for slice
bool PartPlateList::is_all_plates_ready_for_slice() const
{
    for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		if (m_plate_list[i]->can_slice())
			return true;
	}
	return false;
}

//will create a plate and load gcode, return the plate index
int PartPlateList::create_plate_from_gcode_file(const std::string& filename)
{
	int ret = 0;

	return ret;
}

void PartPlateList::get_sliced_result(std::vector<bool>& sliced_result, std::vector<std::string>& gcode_paths)
{
	sliced_result.resize(m_plate_list.size());
	gcode_paths.resize(m_plate_list.size());

	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		sliced_result[i] = m_plate_list[i]->m_slice_result_valid;
		gcode_paths[i] = m_plate_list[i]->m_tmp_gcode_path;
	}
}
//rebuild data which are not serialized after de-serialize
int PartPlateList::rebuild_plates_after_deserialize(std::vector<bool>& previous_sliced_result, std::vector<std::string>& previous_gcode_paths)
{
	int ret = 0;

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": plates count %1%") % m_plate_list.size();
	// SoftFever: assign plater info first
    for (auto partplate : m_plate_list) {
        partplate->m_plater = this->m_plater;
        partplate->m_partplate_list = this;
        partplate->m_model = this->m_model;
        partplate->printer_technology = this->printer_technology;
    }
	update_plate_cols();
	update_all_plates_pos_and_size(false, false, false, false);
	set_shapes(m_shape, m_exclude_areas, m_wrapping_exclude_areas, m_extruder_areas, m_extruder_heights, m_logo_texture_filename, m_height_to_lid, m_height_to_rod);
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		bool need_reset_print = false;
		//check the previous sliced result
		if (m_plate_list[i]->m_slice_result_valid) {
			if ((i >= previous_sliced_result.size()) || !previous_sliced_result[i])
				m_plate_list[i]->update_slice_result_valid_state(false);
		}
		if ((i < previous_gcode_paths.size())
			&& !previous_gcode_paths[i].empty()
			&& (m_plate_list[i]->m_tmp_gcode_path != previous_gcode_paths[i])) {
			if (boost::filesystem::exists(previous_gcode_paths[i])) {
				boost::nowide::remove(previous_gcode_paths[i].c_str());
				need_reset_print = true;
			}
		}

		std::map<int, PrintBase*>::iterator it = m_print_list.find(m_plate_list[i]->m_print_index);
		std::map<int, GCodeResult*>::iterator it2 = m_gcode_result_list.find(m_plate_list[i]->m_print_index);
		if (it != m_print_list.end())
		{
			//find it
			if (it2 == m_gcode_result_list.end())
			{
				//should not happen
				assert(0);
				BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":can not find gcode result for plate %1%, print index %2%") % i % m_plate_list[i]->m_print_index;
				delete it->second;
				m_print_list.erase(it);
			}
			else
			{
				m_plate_list[i]->set_print(it->second, it2->second, m_plate_list[i]->m_print_index);
				it->second->set_plate_index(i);
				if (need_reset_print) {
					Print *print = dynamic_cast<Print*>(it->second);
					it2->second->reset();
					print->set_gcode_file_invalidated();
					if ((i == m_current_plate)&&m_plater)
						m_plater->reset_gcode_toolpaths();
				}
				continue;
			}
		}

		//can not find, create a new one
		Print* print = new Print();
		GCodeResult* gcode = new GCodeResult();
		m_print_list.emplace(m_print_index, print);
		m_gcode_result_list.emplace(m_print_index, gcode);
		m_plate_list[i]->set_print(print, gcode, m_print_index);
		print->set_plate_index(i);
		m_print_index++;
	}

	//go through the print list, and delete the one not used by plate
	std::map<int, PrintBase*>::iterator it = m_print_list.begin();
	int print_index;
	std::vector<int> delete_list;
	while (it != m_print_list.end())
	{
		print_index = it->first;

		int plate_index = find_plate_by_print_index(print_index);
		if (plate_index < 0)
		{
			delete_list.push_back(print_index);
		}
		it++;
	}

	for (unsigned int index = 0; index < delete_list.size(); index++)
	{
		destroy_print(delete_list[index]);
	}

	//update the bed's position
	Vec2d pos = compute_shape_position(m_current_plate, m_plate_cols);
	m_plater->set_bed_position(pos);

	//not used
	/*if (m_plate_width == 0)
	{
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": jump to the first init state, need to re-set size!");
		Vec3d max = m_plater->get_bed().get_bounding_box(false).max;
		Vec3d min = m_plater->get_bed().get_bounding_box(false).min;
		double z = m_plater->config()->opt_float("printable_height");
		reset_size(max.x() - min.x(), max.y() - min.y(), z);
	}*/
	return ret;
}

//retruct plates structures after auto-arrangement
int PartPlateList::rebuild_plates_after_arrangement(bool recycle_plates, bool except_locked, int plate_index)
{
	int ret = 0;

	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":before rebuild, plates count %1%, recycle_plates %2%") % m_plate_list.size() % recycle_plates;

	// sort by arrange_order
	std::sort(m_model->objects.begin(), m_model->objects.end(), [](auto a, auto b) {return a->instances[0]->arrange_order < b->instances[0]->arrange_order; });
	//for (auto object : m_model->objects)
	//	std::sort(object->instances.begin(), object->instances.end(), [](auto a, auto b) {return a->arrange_order < b->arrange_order; });

	ret = reload_all_objects(except_locked, plate_index);

	if (recycle_plates)
	{
		for (unsigned int i = (unsigned int)m_plate_list.size() - 1; i > 0; --i)
		{
			if (m_plate_list[i]->empty()
				|| !m_plate_list[i]->has_printable_instances())
			{
				//delete it
				BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":delete plate %1% for empty") % i;
				delete_plate(i);
			}
			else if (m_plate_list[i]->is_locked()) {
				continue;
			}
			else
			{
				break;
			}
		}
	}

#if 0
	if (m_plater != nullptr) {
		// In GUI mode
		wxGetApp().obj_list()->reload_all_plates();
	}
#endif

	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":after rebuild, plates count %1%") % m_plate_list.size();
	return ret;
}

int PartPlateList::store_to_3mf_structure(PlateDataPtrs& plate_data_list, bool with_slice_info, int plate_idx)
{
	int ret = 0;

	plate_data_list.clear();
	plate_data_list.reserve(m_plate_list.size());
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		PlateData* plate_data_item = new PlateData();
		// TODO: write if needed
		plate_data_item->filament_maps = m_plate_list[i]->get_filament_maps();
		plate_data_item->locked = m_plate_list[i]->m_locked;
		plate_data_item->plate_index = m_plate_list[i]->m_plate_index;
		plate_data_item->plate_name  = m_plate_list[i]->get_plate_name();
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate %1% before load, width %2%, height %3%, size %4%!")
			%(i+1) %m_plate_list[i]->thumbnail_data.width %m_plate_list[i]->thumbnail_data.height %m_plate_list[i]->thumbnail_data.pixels.size();
		plate_data_item->plate_thumbnail.load_from(m_plate_list[i]->thumbnail_data);
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate %1% after load, width %2%, height %3%, size %4%!")
			%(i+1) %plate_data_item->plate_thumbnail.width %plate_data_item->plate_thumbnail.height %plate_data_item->plate_thumbnail.pixels.size();
		plate_data_item->config.apply(*m_plate_list[i]->config());

		if (m_plate_list[i]->no_light_thumbnail_data.is_valid())
			plate_data_item->no_light_thumbnail_file = "valid_no_light";
		if (m_plate_list[i]->top_thumbnail_data.is_valid())
			plate_data_item->top_file = "valid_top";
		if (m_plate_list[i]->pick_thumbnail_data.is_valid())
			plate_data_item->pick_file = "valid_pick";

		if (m_plate_list[i]->obj_to_instance_set.size() > 0)
		{
			for (std::set<std::pair<int, int>>::iterator it = m_plate_list[i]->obj_to_instance_set.begin(); it != m_plate_list[i]->obj_to_instance_set.end(); ++it)
				plate_data_item->objects_and_instances.emplace_back(it->first, it->second);
		}

		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ <<boost::format(": plate %1%, gcode_filename=%2%, with_slice_info=%3%, slice_valid %4%, object item count %5%.")
			%i %m_plate_list[i]->m_gcode_result->filename % with_slice_info %m_plate_list[i]->is_slice_result_valid()%plate_data_item->objects_and_instances.size();

		if (with_slice_info) {
			if (m_plate_list[i]->get_slice_result() && m_plate_list[i]->is_slice_result_valid()) {
				// BBS only include current palte_idx
				if (plate_idx == i || plate_idx == PLATE_CURRENT_IDX || plate_idx == PLATE_ALL_IDX) {
					//load calibration thumbnail
					//if (m_plate_list[i]->cali_thumbnail_data.is_valid())
					//	plate_data_item->pattern_file = "valid_pattern";
					if (m_plate_list[i]->cali_bboxes_data.is_valid())
						plate_data_item->pattern_bbox_file = "valid_pattern_bbox";
					plate_data_item->gcode_file       = m_plate_list[i]->m_gcode_result->filename;
					plate_data_item->is_sliced_valid  = true;
					plate_data_item->gcode_prediction = std::to_string(
						(int) m_plate_list[i]->get_slice_result()->print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
					plate_data_item->toolpath_outside = m_plate_list[i]->m_gcode_result->toolpath_outside;
                    plate_data_item->timelapse_warning_code = m_plate_list[i]->m_gcode_result->timelapse_warning_code;
                    m_plate_list[i]->set_timelapse_warning_code(plate_data_item->timelapse_warning_code);
					plate_data_item->is_label_object_enabled = m_plate_list[i]->m_gcode_result->label_object_enabled;
                    plate_data_item->limit_filament_maps = m_plate_list[i]->m_gcode_result->limit_filament_maps;
                    plate_data_item->layer_filaments  = m_plate_list[i]->m_gcode_result->layer_filaments;
                    plate_data_item->first_layer_time = std::to_string(m_plate_list[i]->cali_bboxes_data.first_layer_time);
					Print *print                      = nullptr;
					m_plate_list[i]->get_print((PrintBase **) &print, nullptr, nullptr);
					if (print) {
						const PrintStatistics &ps = print->print_statistics();
						if (ps.total_weight != 0.0) {
							CNumericLocalesSetter locales_setter;
							plate_data_item->gcode_weight =wxString::Format("%.2f", ps.total_weight).ToStdString();
						}
						plate_data_item->is_support_used = print->is_support_used();
					} else {
						BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("print is null!");
					}
					//parse filament info
					plate_data_item->parse_filament_info(m_plate_list[i]->get_slice_result());
				} else {
					BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "slice result = " << m_plate_list[i]->get_slice_result()
										<< ", result valid = " << m_plate_list[i]->is_slice_result_valid();
				}
			}
		}

		plate_data_list.push_back(plate_data_item);
	}
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":stored %1% plates!") % m_plate_list.size();

	return ret;
}

int PartPlateList::load_from_3mf_structure(PlateDataPtrs& plate_data_list, int filament_count)
{
	int ret = 0;

	if (plate_data_list.size() <= 0)
	{
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":no plates, should not happen!");
		return -1;
	}
	clear(true, true);
	set_filament_count(filament_count);
	for (unsigned int i = 0; i < (unsigned int)plate_data_list.size(); ++i)
	{
		int index = create_plate(false);
		m_plate_list[index]->m_locked = plate_data_list[i]->locked;
		m_plate_list[index]->config()->apply(plate_data_list[i]->config);
		m_plate_list[index]->set_plate_name(plate_data_list[i]->plate_name);
		if (plate_data_list[i]->plate_index != index)
		{
			BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(":plate index %1% seems invalid, skip it")% plate_data_list[i]->plate_index;
		}
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate %1%, gcode_file %2%, is_sliced_valid %3%, toolpath_outside %4%, is_support_used %5% is_label_object_enabled %6%")
			%i %plate_data_list[i]->gcode_file %plate_data_list[i]->is_sliced_valid %plate_data_list[i]->toolpath_outside %plate_data_list[i]->is_support_used %plate_data_list[i]->is_label_object_enabled;
		//load object and instance from 3mf
		//just test for file correct or not, we will rebuild later
		/*for (std::vector<std::pair<int, int>>::iterator it = plate_data_list[i]->objects_and_instances.begin(); it != plate_data_list[i]->objects_and_instances.end(); ++it)
			m_plate_list[index]->obj_to_instance_set.insert(std::pair(it->first, it->second));*/
		if (!plate_data_list[i]->gcode_file.empty()) {
			m_plate_list[index]->m_gcode_path_from_3mf = plate_data_list[i]->gcode_file;
		}
		GCodeResult* gcode_result = nullptr;
		PrintBase* fff_print = nullptr;
		m_plate_list[index]->get_print(&fff_print, &gcode_result, nullptr);
		PrintStatistics& ps = (dynamic_cast<Print*>(fff_print))->print_statistics();
		gcode_result->print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time = atoi(plate_data_list[i]->gcode_prediction.c_str());
		ps.total_weight = atof(plate_data_list[i]->gcode_weight.c_str());
		ps.total_used_filament = 0.f;
		for (auto filament_item: plate_data_list[i]->slice_filaments_info)
		{
			ps.total_used_filament += filament_item.used_m;
		}
		ps.total_used_filament *= 1000; //koef
		gcode_result->toolpath_outside = plate_data_list[i]->toolpath_outside;
		gcode_result->label_object_enabled = plate_data_list[i]->is_label_object_enabled;
        gcode_result->timelapse_warning_code = plate_data_list[i]->timelapse_warning_code;
        m_plate_list[index]->set_timelapse_warning_code(plate_data_list[i]->timelapse_warning_code);
		m_plate_list[index]->slice_filaments_info = plate_data_list[i]->slice_filaments_info;
		gcode_result->warnings = plate_data_list[i]->warnings;
        gcode_result->filament_maps = plate_data_list[i]->filament_maps;
		if (m_plater && !plate_data_list[i]->thumbnail_file.empty()) {
			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate %1%, load thumbnail from %2%.")%(i+1) %plate_data_list[i]->thumbnail_file;
			if (boost::filesystem::exists(plate_data_list[i]->thumbnail_file)) {
				m_plate_list[index]->load_thumbnail_data(plate_data_list[i]->thumbnail_file, m_plate_list[index]->thumbnail_data);
				BOOST_LOG_TRIVIAL(info) << __FUNCTION__ <<boost::format(": plate %1% after load, width %2%, height %3%, size %4%!")
					%(i+1) %m_plate_list[index]->thumbnail_data.width %m_plate_list[index]->thumbnail_data.height %m_plate_list[index]->thumbnail_data.pixels.size();
			}
		}

		if (m_plater && !plate_data_list[i]->no_light_thumbnail_file.empty()) {
			if (boost::filesystem::exists(plate_data_list[i]->no_light_thumbnail_file)) {
				BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate %1%, load no_light_thumbnail_file from %2%.")%(i+1) %plate_data_list[i]->no_light_thumbnail_file;
				m_plate_list[index]->load_thumbnail_data(plate_data_list[i]->no_light_thumbnail_file, m_plate_list[index]->no_light_thumbnail_data);
			}
		}

		/*if (m_plater && !plate_data_list[i]->pattern_file.empty()) {
			if (boost::filesystem::exists(plate_data_list[i]->pattern_file)) {
				//no need to load pattern data currently
				//m_plate_list[index]->load_pattern_thumbnail_data(plate_data_list[i]->pattern_file);
			}
		}*/
		if (m_plater && !plate_data_list[i]->top_file.empty()) {
			if (boost::filesystem::exists(plate_data_list[i]->top_file)) {
				BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate %1%, load top_thumbnail from %2%.")%(i+1) %plate_data_list[i]->top_file;
				m_plate_list[index]->load_thumbnail_data(plate_data_list[i]->top_file, m_plate_list[index]->top_thumbnail_data);
			}
		}
		if (m_plater && !plate_data_list[i]->pick_file.empty()) {
			if (boost::filesystem::exists(plate_data_list[i]->pick_file)) {
				BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": plate %1%, load pick_thumbnail from %2%.")%(i+1) %plate_data_list[i]->pick_file;
				m_plate_list[index]->load_thumbnail_data(plate_data_list[i]->pick_file, m_plate_list[index]->pick_thumbnail_data);
			}
		}
		if (m_plater && !plate_data_list[i]->pattern_bbox_file.empty()) {
			if (boost::filesystem::exists(plate_data_list[i]->pattern_bbox_file)) {
				m_plate_list[index]->load_pattern_box_data(plate_data_list[i]->pattern_bbox_file);
			}
		}

	}
	print();
	ret = reload_all_objects();
	print();

	return ret;
}

//load gcode files
int PartPlateList::load_gcode_files()
{
	int ret = 0;

	//only do this while m_plater valid for gui mode
	if (!m_plater)
		return ret;

	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		if (!m_plate_list[i]->m_gcode_path_from_3mf.empty()) {
			//the same as plater::priv::update_print_volume_state();
			//BoundingBoxf3   print_volume = m_plate_list[i]->get_bounding_box(false);
			//print_volume.max(2) = this->m_plate_height;
			//print_volume.min(2) = -1e10;
			m_model->update_print_volume_state({m_plate_list[i]->get_shape(), (double)this->m_plate_height, m_plate_list[i]->get_extruder_areas(), m_plate_list[i]->get_extruder_heights() });

			if (!m_plate_list[i]->load_gcode_from_file(m_plate_list[i]->m_gcode_path_from_3mf))
				ret ++;
		}
	}

	BOOST_LOG_TRIVIAL(trace) << boost::format("totally got %1% gcode files") % ret;

	return ret;
}

void PartPlateList::print() const
{
	BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << boost::format("PartPlateList %1%, m_plate_count %2%, current_plate %3%, print_count %4%, current print index %5%, plate cols %6%") % this % m_plate_count % m_current_plate % m_print_list.size() % m_print_index % m_plate_cols;
	BOOST_LOG_TRIVIAL(trace) << boost::format("m_plate_width %1%, m_plate_depth %2%, m_plate_height %3%, plate count %4%\nplate list:") % m_plate_width % m_plate_depth % m_plate_height % m_plate_list.size();
	for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
	{
		BOOST_LOG_TRIVIAL(trace) << boost::format("the %1%th plate") % i;
		m_plate_list[i]->print();
	}
	BOOST_LOG_TRIVIAL(trace) << boost::format("the unprintable plate:");
	unprintable_plate.print();

	flush_logs();
	return;
}

bool PartPlateList::is_load_bedtype_textures = false;
bool PartPlateList::is_load_extruder_only_area_textures = false;
bool PartPlateList::is_load_cali_texture     = false;

void PartPlateList::BedTextureInfo::TexturePart::update_buffer()
{
	if (w == 0 || h == 0) {
		return;
	}

	Pointfs rectangle;
	rectangle.push_back(Vec2d(x, y));
	rectangle.push_back(Vec2d(x+w, y));
	rectangle.push_back(Vec2d(x+w, y+h));
	rectangle.push_back(Vec2d(x, y+h));
	ExPolygon poly;

	for (int i = 0; i < 4; i++) {
		const Vec2d & p = rectangle[i];
		for (auto& p : rectangle) {
			Vec2d pp = Vec2d(p.x() + offset.x(), p.y() + offset.y());
			poly.contour.append({ scale_(pp(0)), scale_(pp(1)) });
		}
	}

	if (!buffer)
        buffer = new GLModel();

	buffer->reset();

	if (!init_model_from_poly(*buffer, poly, GROUND_Z + 0.02f)) {
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":Unable to create buffer triangles\n";
	}
}

void PartPlateList::BedTextureInfo::TexturePart::reset()
{
    if (texture) {
        texture->reset();
        delete texture;
    }
    if (buffer)
        delete buffer;
}

void PartPlateList::BedTextureInfo::reset()
{
    for (size_t i = 0; i < parts.size(); i++)
        parts[i].reset();
}

void PartPlateList::init_bed_type_info()
{
	BedTextureInfo::TexturePart pct_part_left(10, 130,  10, 110, "orca_bed_pct_left.svg");
    BedTextureInfo::TexturePart st_part1(9, 70, 12.5, 170, "bbl_bed_st_left.svg");
    BedTextureInfo::TexturePart st_part2(74, -10, 148, 12, "bbl_bed_st_bottom.svg");
	BedTextureInfo::TexturePart pc_part1(10, 130, 10, 110, "bbl_bed_pc_left.svg");
	BedTextureInfo::TexturePart pc_part2(74, -10, 148, 12, "bbl_bed_pc_bottom.svg");
	BedTextureInfo::TexturePart ep_part1(7.5, 90, 12.5, 150, "bbl_bed_ep_left.svg");
	BedTextureInfo::TexturePart ep_part2(74, -10, 148, 12, "bbl_bed_ep_bottom.svg");
	BedTextureInfo::TexturePart pei_part1(7.5, 50, 12.5, 190, "bbl_bed_pei_left.svg");
	BedTextureInfo::TexturePart pei_part2(74, -10, 148, 12, "bbl_bed_pei_bottom.svg");
	BedTextureInfo::TexturePart pte_part1(10, 80, 10, 160, "bbl_bed_pte_left.svg");
	BedTextureInfo::TexturePart pte_part2(74, -10, 148,  12, "bbl_bed_pte_bottom.svg");
    auto bed_texture_maps        = wxGetApp().plater()->get_bed_texture_maps();
    std::string bottom_texture_end_name = bed_texture_maps.find("bottom_texture_end_name") != bed_texture_maps.end() ? bed_texture_maps["bottom_texture_end_name"] : "";
    std::string bottom_texture_rect_str = bed_texture_maps.find("bottom_texture_rect") != bed_texture_maps.end() ? bed_texture_maps["bottom_texture_rect"] : "";
    std::string middle_texture_rect_str = bed_texture_maps.find("middle_texture_rect") != bed_texture_maps.end() ? bed_texture_maps["middle_texture_rect"] : "";
    std::string use_double_extruder_default_texture = bed_texture_maps.find("use_double_extruder_default_texture") != bed_texture_maps.end() ? bed_texture_maps["use_double_extruder_default_texture"] : "";
    std::array<float, 4>        bottom_texture_rect = {0, 0, 0, 0}, middle_texture_rect = {0, 0, 0, 0};
    if (bottom_texture_rect_str.size() > 0) {
        std::vector<std::string> items;
        boost::algorithm::erase_all(bottom_texture_rect_str, " ");
        boost::split(items, bottom_texture_rect_str, boost::is_any_of(","));
        if (items.size() == 4) {
            for (int i = 0; i < items.size(); i++) {
                bottom_texture_rect[i] = std::atof(items[i].c_str());
            }
        }
    }
    if (middle_texture_rect_str.size() > 0) {
        std::vector<std::string> items;
        boost::algorithm::erase_all(middle_texture_rect_str, " ");
        boost::split(items, middle_texture_rect_str, boost::is_any_of(","));
        if (items.size() == 4) {
            for (int i = 0; i < items.size(); i++) {
                middle_texture_rect[i] = std::atof(items[i].c_str());
            }
        }
    }
    auto is_single_extruder = wxGetApp().preset_bundle->get_printer_extruder_count() == 1;
    bool use_double_extruder_texture = !is_single_extruder || use_double_extruder_default_texture == "true";
    if (use_double_extruder_texture) {
        pte_part1 = BedTextureInfo::TexturePart(57, 300, 236.12f, 10.f, "bbl_bed_pte_middle.svg");
        auto &middle_rect = middle_texture_rect;
        if (middle_rect[2] > 0.f) {
            pte_part1 = BedTextureInfo::TexturePart(middle_rect[0], middle_rect[1], middle_rect[2], middle_rect[3], "bbl_bed_pte_middle.svg");
        }
        pte_part2 = BedTextureInfo::TexturePart(45, -14.5, 70, 8, "bbl_bed_pte_left_bottom.svg");
        auto &bottom_rect = bottom_texture_rect;
        if (bottom_texture_end_name.size() > 0 && bottom_rect[2] > 0.f) {
            std::string pte_part2_name = "bbl_bed_pte_bottom_" + bottom_texture_end_name + ".svg";
            pte_part2 = BedTextureInfo::TexturePart(bottom_rect[0], bottom_rect[1], bottom_rect[2], bottom_rect[3], pte_part2_name);
        }

        pei_part1  = BedTextureInfo::TexturePart(57, 300, 236.12f, 10.f, "bbl_bed_pei_middle.svg");
        if (middle_rect[2] > 0.f) {
            pei_part1 = BedTextureInfo::TexturePart(middle_rect[0], middle_rect[1], middle_rect[2], middle_rect[3], "bbl_bed_pei_middle.svg");
        }
        pei_part2  = BedTextureInfo::TexturePart(45, -14.5, 70, 8, "bbl_bed_pei_left_bottom.svg");
        if (bottom_texture_end_name.size() > 0 && bottom_rect[2] > 0.f) {
            std::string pei_part2_name = "bbl_bed_pei_bottom_" + bottom_texture_end_name + ".svg";
            pei_part2                  = BedTextureInfo::TexturePart(bottom_rect[0], bottom_rect[1], bottom_rect[2], bottom_rect[3], pei_part2_name);
        }

        st_part1 = BedTextureInfo::TexturePart(57, 300, 236.12f, 10.f, "bbl_bed_st_middle.svg");
        if (middle_rect[2] > 0.f) {
            st_part1 = BedTextureInfo::TexturePart(middle_rect[0], middle_rect[1], middle_rect[2], middle_rect[3], "bbl_bed_st_middle.svg");
        }
        st_part2 = BedTextureInfo::TexturePart(45, -14.5, 260, 8, "bbl_bed_st_left_bottom.svg");
        if (bottom_texture_end_name.size() > 0 && bottom_rect[2] > 0.f) {
            std::string st_part2_name = "bbl_bed_st_bottom_" + bottom_texture_end_name + ".svg";
            st_part2                   = BedTextureInfo::TexturePart(bottom_rect[0], bottom_rect[1], bottom_rect[2], bottom_rect[3], st_part2_name);
        }

        ep_part1 = BedTextureInfo::TexturePart(57, 300, 236.12f, 10.f, "bbl_bed_ep_middle.svg");
        if (middle_rect[2] > 0.f) {
            ep_part1 = BedTextureInfo::TexturePart(middle_rect[0], middle_rect[1], middle_rect[2], middle_rect[3], "bbl_bed_ep_middle.svg");
        }
        ep_part2 = BedTextureInfo::TexturePart(45, -14.5, 260, 8, "bbl_bed_ep_left_bottom.svg");
        if (bottom_texture_end_name.size() > 0 && bottom_rect[2] > 0.f) {
            std::string ep_part2_name = "bbl_bed_ep_bottom_" + bottom_texture_end_name + ".svg";
            ep_part2                   = BedTextureInfo::TexturePart(bottom_rect[0], bottom_rect[1], bottom_rect[2], bottom_rect[3], ep_part2_name);
        }

        pc_part1 = BedTextureInfo::TexturePart(57, 300, 236.12f, 10.f, "bbl_bed_pc_middle.svg");
        if (middle_rect[2] > 0.f) {
            pc_part1 = BedTextureInfo::TexturePart(middle_rect[0], middle_rect[1], middle_rect[2], middle_rect[3], "bbl_bed_pc_middle.svg"); }
        pc_part2 = BedTextureInfo::TexturePart(45, -14.5, 70, 8, "bbl_bed_pc_left_bottom.svg");
        if (bottom_texture_end_name.size() > 0 && bottom_rect[2] > 0.f) {
            std::string pc_part2_name = "bbl_bed_pc_bottom_" + bottom_texture_end_name + ".svg";
            pc_part2                  = BedTextureInfo::TexturePart(bottom_rect[0], bottom_rect[1], bottom_rect[2], bottom_rect[3], pc_part2_name);
        }

        m_allow_bed_type_in_double_nozzle.clear();
        auto bed_types = wxGetApp().plater()->sidebar().get_cur_combox_bed_types();
        for (int i = 0; i < bed_types.size(); i++) {
            m_allow_bed_type_in_double_nozzle[bed_types[i]] = true;
        }
    } else {
        if (bottom_texture_end_name.size() > 0) {
            st_part2.update_file("bbl_bed_st_bottom_" + bottom_texture_end_name + ".svg");
            pc_part2.update_file("bbl_bed_pc_bottom_" + bottom_texture_end_name + ".svg");
            ep_part2.update_file("bbl_bed_ep_bottom_" + bottom_texture_end_name + ".svg");
            pei_part2.update_file("bbl_bed_pei_bottom_" + bottom_texture_end_name + ".svg");
            pte_part2.update_file("bbl_bed_pte_bottom_" + bottom_texture_end_name + ".svg");
        }
    }

	for (size_t i = 0; i < btCount; i++) {
		bed_texture_info[i].reset();
		bed_texture_info[i].parts.clear();
	}
    bed_texture_info[btSuperTack].parts.push_back(st_part1);
    bed_texture_info[btSuperTack].parts.push_back(st_part2);
	bed_texture_info[btPC].parts.push_back(pc_part1);
	bed_texture_info[btPC].parts.push_back(pc_part2);
	bed_texture_info[btPCT].parts.push_back(pct_part_left);
	bed_texture_info[btPCT].parts.push_back(pc_part2);
	bed_texture_info[btEP].parts.push_back(ep_part1);
	bed_texture_info[btEP].parts.push_back(ep_part2);
	bed_texture_info[btPEI].parts.push_back(pei_part1);
	bed_texture_info[btPEI].parts.push_back(pei_part2);
	bed_texture_info[btPTE].parts.push_back(pte_part1);
	bed_texture_info[btPTE].parts.push_back(pte_part2);

	auto  bed_ext     = get_extents(m_shape);
	int   bed_width   = bed_ext.size()(0);
	int   bed_height  = bed_ext.size()(1);
    float base_width  = 256;//standard 256*256 for single_extruder
    float base_height = 256;
    if (use_double_extruder_texture) { // standard 350*325 for double_extruder
        base_width  = bed_width;
        base_height = bed_height;
    }
    float x_rate      = bed_width / base_width;
    float y_rate      = bed_height / base_height;
    for (int i = 0; i < btCount; i++) {
		for (int j = 0; j < bed_texture_info[i].parts.size(); j++) {
			bed_texture_info[i].parts[j].x *= x_rate;
			bed_texture_info[i].parts[j].y *= y_rate;
            bed_texture_info[i].parts[j].w *= x_rate;
            bed_texture_info[i].parts[j].h *= y_rate;
            bed_texture_info[i].parts[j].update_buffer();
        }
    }
}

bool PartPlateList::calc_extruder_only_area(Rect &left_only_rect, Rect &right_only_rect)
{
    auto                convert_to_rect         = [](const Pointfs &pts, Rect &rect) {
		rect.x = pts[0].x();
        rect.y = pts[0].y();
        rect.w = pts[1].x() - pts[0].x();
        rect.h = pts[2].y() - pts[1].y();
    };
    auto is_single_extruder = wxGetApp().preset_bundle->get_printer_extruder_count() ==1;
    if (is_single_extruder) {
		return false;
	}
    if (m_extruder_areas.size() == 2) {
        Rect printable_rect, left_extruder_printable_area, right_extruder_printable_area;
        convert_to_rect(m_shape, printable_rect);
        convert_to_rect(m_extruder_areas[0], left_extruder_printable_area);
        convert_to_rect(m_extruder_areas[1], right_extruder_printable_area);
        left_only_rect.x = left_extruder_printable_area.x;
        left_only_rect.y = left_extruder_printable_area.y;
        left_only_rect.w = printable_rect.w - right_extruder_printable_area.w;
        left_only_rect.h = left_extruder_printable_area.h;

        right_only_rect.x = left_extruder_printable_area.x + left_extruder_printable_area.w;
        right_only_rect.y = right_extruder_printable_area.y;
        right_only_rect.w = printable_rect.w - left_extruder_printable_area.w;
        right_only_rect.h = right_extruder_printable_area.h;
        if (left_only_rect.w < 0 || right_only_rect.w < 0) {
			return false;
		}
        return true;
	}
    return false;
}

bool PartPlateList::init_extruder_only_area_info()
{
    Rect left_only_rect,  right_only_rect;
    auto ok = calc_extruder_only_area(left_only_rect, right_only_rect);
    if (!ok) { return false; }
    float  base_width  = 25.f;
    float  base_height = 320.f;
    float  left_x_rate = left_only_rect.w / base_width;
    float  left_y_rate = left_only_rect.h / base_height;
    bool   is_zh       = wxGetApp().app_config->get("language") == "zh_CN";
    Vec4f  base_left(-6.f, -75.f, 12.f, 150.f);
    if (is_zh) {
		base_left = Vec4f(-5.5f, -76.f, 12.f, 150.f);
    }
    base_left[0]   = base_left[0] * left_x_rate + left_only_rect.x + left_only_rect.w / 2.f;
    base_left[1]   = base_left[1] * left_y_rate + left_only_rect.y + left_only_rect.h / 2.f;
    base_left[2]   = base_left[2] * left_x_rate;
    base_left[3]   = base_left[3] * left_y_rate;
    Vec4f   base_right(-5.5f, -75.f, 12.f, 150.f);
    if (is_zh) {
        base_right = Vec4f(-4.5f, -76.f, 12.f, 150.f);
    }
    float right_x_rate = right_only_rect.w / base_width;
    float right_y_rate = right_only_rect.h / base_height;
    base_right[0]                   = base_right[0] * right_x_rate + right_only_rect.x + right_only_rect.w / 2.f;
    base_right[1]                   = base_right[1] * right_y_rate + right_only_rect.y + right_only_rect.h / 2.f;
    base_right[2]                   = base_right[2] * right_x_rate;
    base_right[3]                   = base_right[3] * right_y_rate;
    BedTextureInfo::TexturePart left_part(base_left[0], base_left[1], base_left[2], base_left[3], "left_extruder_only_area.svg");
    BedTextureInfo::TexturePart left_ch_part(base_left[0], base_left[1], base_left[2], base_left[3], "left_extruder_only_area_ch.svg");
    BedTextureInfo::TexturePart right_part(base_right[0], base_right[1], base_right[2], base_right[3], "right_extruder_only_area.svg");
    BedTextureInfo::TexturePart right_ch_part(base_right[0], base_right[1], base_right[2], base_right[3], "right_extruder_only_area_ch.svg");

    for (size_t i = 0; i < (unsigned char) ExtruderOnlyAreaType::btAreaCount; i++) {
        extruder_only_area_info[i].reset();
        extruder_only_area_info[i].parts.clear();
    }
    extruder_only_area_info[(unsigned char) ExtruderOnlyAreaType::Engilish].parts.push_back(left_part);
    if (base_right[2]>5) {//width should >5
        extruder_only_area_info[(unsigned char) ExtruderOnlyAreaType::Engilish].parts.push_back(right_part);
    }
    extruder_only_area_info[(unsigned char) ExtruderOnlyAreaType::Chinese].parts.push_back(left_ch_part);
    if (base_right[2] > 5) { // width should >5
        extruder_only_area_info[(unsigned char) ExtruderOnlyAreaType::Chinese].parts.push_back(right_ch_part);
    }

    for (int i = 0; i < (unsigned char) ExtruderOnlyAreaType::btAreaCount; i++) {
        for (int j = 0; j < extruder_only_area_info[i].parts.size(); j++) {
            extruder_only_area_info[i].parts[j].update_buffer();
        }
    }
    return true;
}

void PartPlateList::load_bedtype_textures()
{
	if (PartPlateList::is_load_bedtype_textures) return;

	init_bed_type_info();
	GLint max_tex_size = OpenGLManager::get_gl_info().get_max_tex_size();
	GLint logo_tex_size = (max_tex_size < 2048) ? max_tex_size : 2048;
	for (int i = 0; i < (unsigned int)btCount; ++i) {
		for (int j = 0; j < bed_texture_info[i].parts.size(); j++) {
			std::string filename = resources_dir() + "/images/" + bed_texture_info[i].parts[j].filename;
			if (boost::filesystem::exists(filename)) {
				PartPlateList::bed_texture_info[i].parts[j].texture = new GLTexture();
				if (!PartPlateList::bed_texture_info[i].parts[j].texture->load_from_svg_file(filename, true, true, true, logo_tex_size)) {
					BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": load logo texture from %1% failed!") % filename;
				}
			} else {
				BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": load logo texture from %1% failed!") % filename;
			}
		}
	}
	PartPlateList::is_load_bedtype_textures = true;
}

void PartPlateList::load_extruder_only_area_textures() {
    if (PartPlateList::is_load_extruder_only_area_textures) return;

    auto ok  = init_extruder_only_area_info();
    if (!ok) {
        PartPlateList::is_load_extruder_only_area_textures = true;
        return;
    }
    GLint max_tex_size  = OpenGLManager::get_gl_info().get_max_tex_size();
    GLint logo_tex_size = (max_tex_size < 2048) ? max_tex_size : 2048;
    for (int i = 0; i < (unsigned int) ExtruderOnlyAreaType::btAreaCount; ++i) {
        for (int j = 0; j < extruder_only_area_info[i].parts.size(); j++) {
            std::string filename = resources_dir() + "/images/" + extruder_only_area_info[i].parts[j].filename;
            if (boost::filesystem::exists(filename)) {
                PartPlateList::extruder_only_area_info[i].parts[j].texture = new GLTexture();
                if (!PartPlateList::extruder_only_area_info[i].parts[j].texture->load_from_svg_file(filename, true, false, false, logo_tex_size)) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": load logo texture from %1% failed!") % filename;
                }
            } else {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": load logo texture from %1% failed!") % filename;
            }
        }
    }
    PartPlateList::is_load_extruder_only_area_textures = true;
}

void PartPlateList::init_cali_texture_info()
{
	BedTextureInfo::TexturePart cali_line(18, 2, 224, 16, "bbl_cali_lines.svg");
	cali_texture_info.parts.push_back(cali_line);

	for (int j = 0; j < cali_texture_info.parts.size(); j++) {
		cali_texture_info.parts[j].update_buffer();
	}
}

void PartPlateList::load_cali_textures()
{
	if (PartPlateList::is_load_cali_texture) return;

	init_cali_texture_info();
	GLint max_tex_size = OpenGLManager::get_gl_info().get_max_tex_size();
	GLint logo_tex_size = (max_tex_size < 2048) ? max_tex_size : 2048;
	for (int i = 0; i < (unsigned int)btCount; ++i) {
		for (int j = 0; j < cali_texture_info.parts.size(); j++) {
			std::string filename = resources_dir() + "/images/" + cali_texture_info.parts[j].filename;
			if (boost::filesystem::exists(filename)) {
				PartPlateList::cali_texture_info.parts[j].texture = new GLTexture();
				if (!PartPlateList::cali_texture_info.parts[j].texture->load_from_svg_file(filename, true, true, true, logo_tex_size)) {
					BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": load cali texture from %1% failed!") % filename;
				}
			}
			else {
				BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": load cali texture from %1% failed!") % filename;
			}
		}
	}
	PartPlateList::is_load_cali_texture = true;
}

void PartPlateList::on_extruder_count_changed(int extruder_count)
{
    for (unsigned int i = 0; i < (unsigned int) m_plate_list.size(); ++i) {
        m_plate_list[i]->on_extruder_count_changed(extruder_count);
    }
    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: extruder_count=%2%")% __FUNCTION__ %extruder_count;
}

void PartPlateList::set_filament_count(int filament_count)
{
    m_filament_count = filament_count;
    for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
    {
        m_plate_list[i]->set_filament_count(filament_count);
    }
    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: filament_count=%2%")% __FUNCTION__ %filament_count;
}

void PartPlateList::on_filament_added(int filament_count)
{
    m_filament_count++;
    for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
    {
        m_plate_list[i]->on_filament_added();
    }
    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: filament_count=%2%")% __FUNCTION__ %filament_count;
}

void PartPlateList::on_filament_deleted(int filament_count, int filament_id)
{
    m_filament_count--;
    for (unsigned int i = 0; i < (unsigned int)m_plate_list.size(); ++i)
    {
        m_plate_list[i]->on_filament_deleted(filament_count, filament_id);
    }
    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: filament_count=%2%, filament_id=%3%")% __FUNCTION__ %filament_count %filament_id;
}

}//end namespace GUI
}//end namespace slic3r
