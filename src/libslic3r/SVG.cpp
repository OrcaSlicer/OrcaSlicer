#include "SVG.hpp"
#include <iostream>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>

namespace Slic3r {

void create_dir(const std::string& filePath)
{
    boost::filesystem::path path(filePath);
    boost::filesystem::path dir = path.parent_path();

    if (!dir.empty()) {
        if (!boost::filesystem::exists(dir)) {
            try {
                boost::filesystem::create_directories(dir);
            } catch (const boost::filesystem::filesystem_error& e) {
                std::cerr << e.what() << std::endl;
            }
        }
    }
}

bool SVG::open(const char* afilename)
{
    create_dir(afilename);
    this->filename = afilename;
    this->f = boost::nowide::fopen(afilename, "w");
    if (this->f == NULL)
        return false;
    fprintf(this->f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.0//EN\" \"http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd\">\n"
        "<svg height=\"2000\" width=\"2000\" xmlns=\"http://www.w3.org/2000/svg\" xmlns:svg=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">\n"
        "   <marker id=\"endArrow\" markerHeight=\"8\" markerUnits=\"strokeWidth\" markerWidth=\"10\" orient=\"auto\" refX=\"1\" refY=\"5\" viewBox=\"0 0 10 10\">\n"
        "      <polyline fill=\"darkblue\" points=\"0,0 10,5 0,10 1,5\" />\n"
        "   </marker>\n"
        );
    fprintf(this->f, "<rect fill='white' stroke='none' x='0' y='0' width='%f' height='%f'/>\n", 2000.f, 2000.f);
    return true;
}

bool SVG::open(const char* afilename, const BoundingBox &bbox, const coord_t bbox_offset, bool aflipY)
{
    create_dir(afilename);
    this->filename = afilename;
    this->origin   = bbox.min - Point(bbox_offset, bbox_offset);
    this->flipY    = aflipY;
    this->f        = boost::nowide::fopen(afilename, "w");
    if (f == NULL)
        return false;
    float w = to_svg_coord(bbox.max(0) - bbox.min(0) + 2 * bbox_offset);
    float h = to_svg_coord(bbox.max(1) - bbox.min(1) + 2 * bbox_offset);
    this->height   = h;
    fprintf(this->f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.0//EN\" \"http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd\">\n"
        "<svg height=\"%f\" width=\"%f\" xmlns=\"http://www.w3.org/2000/svg\" xmlns:svg=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">\n"
        "   <marker id=\"endArrow\" markerHeight=\"8\" markerUnits=\"strokeWidth\" markerWidth=\"10\" orient=\"auto\" refX=\"1\" refY=\"5\" viewBox=\"0 0 10 10\">\n"
        "      <polyline fill=\"darkblue\" points=\"0,0 10,5 0,10 1,5\" />\n"
        "   </marker>\n",
        h, w);
    fprintf(this->f, "<rect fill='white' stroke='none' x='0' y='0' width='%f' height='%f'/>\n", w, h);
    return true;
}

void SVG::draw(const Line &line, std::string stroke, coordf_t stroke_width)
{
    fprintf(this->f,
        "   <line x1=\"%f\" y1=\"%f\" x2=\"%f\" y2=\"%f\" style=\"stroke: %s; stroke-width: %f\"",
        to_svg_x(line.a(0) - origin(0)), to_svg_y(line.a(1) - origin(1)), to_svg_x(line.b(0) - origin(0)), to_svg_y(line.b(1) - origin(1)), stroke.c_str(), (stroke_width == 0) ? 1.f : to_svg_coord(stroke_width));
    if (this->arrows)
        fprintf(this->f, " marker-end=\"url(#endArrow)\"");
    fprintf(this->f, "/>\n");
}

void SVG::draw(const ThickLine &line, const std::string &fill, const std::string &stroke, coordf_t stroke_width)
{
    Vec2d dir(line.b(0)-line.a(0), line.b(1)-line.a(1));
    Vec2d perp(-dir(1), dir(0));
    coordf_t len = sqrt(perp(0)*perp(0) + perp(1)*perp(1));
    coordf_t da  = coordf_t(0.5)*line.a_width/len;
    coordf_t db  = coordf_t(0.5)*line.b_width/len;
    fprintf(this->f,
        "   <polygon points=\"%f,%f %f,%f %f,%f %f,%f\" style=\"fill:%s; stroke: %s; stroke-width: %f\"/>\n",
        to_svg_x(line.a(0)-da*perp(0)-origin(0)),
        to_svg_y(line.a(1)-da*perp(1)-origin(1)),
        to_svg_x(line.b(0)-db*perp(0)-origin(0)),
        to_svg_y(line.b(1)-db*perp(1)-origin(1)),
        to_svg_x(line.b(0)+db*perp(0)-origin(0)),
        to_svg_y(line.b(1)+db*perp(1)-origin(1)),
        to_svg_x(line.a(0)+da*perp(0)-origin(0)),
        to_svg_y(line.a(1)+da*perp(1)-origin(1)),
        fill.c_str(), stroke.c_str(),
        (stroke_width == 0) ? 1.f : to_svg_coord(stroke_width));
}

void SVG::draw(const Lines &lines, std::string stroke, coordf_t stroke_width)
{
    for (const Line &l : lines)
        this->draw(l, stroke, stroke_width);
}

void SVG::draw(const ExPolygon &expolygon, std::string fill, const float fill_opacity)
{
    this->fill = fill;
    
    std::string d;
    for (const Polygon &p : to_polygons(expolygon))
        d += this->get_path_d(p, true) + " ";
    this->path(d, true, 0, fill_opacity);
}

void SVG::draw_outline(const ExPolygon &expolygon, std::string stroke_outer, std::string stroke_holes, coordf_t stroke_width)
{
    draw_outline(expolygon.contour, stroke_outer, stroke_width);
    for (Polygons::const_iterator it = expolygon.holes.begin(); it != expolygon.holes.end(); ++ it) {
        draw_outline(*it, stroke_holes, stroke_width);
    }
}

void SVG::draw(const ExPolygons &expolygons, std::string fill, const float fill_opacity)
{
    for (ExPolygons::const_iterator it = expolygons.begin(); it != expolygons.end(); ++it)
        this->draw(*it, fill, fill_opacity);
}

void SVG::draw_outline(const ExPolygons &expolygons, std::string stroke_outer, std::string stroke_holes, coordf_t stroke_width)
{
    for (ExPolygons::const_iterator it = expolygons.begin(); it != expolygons.end(); ++ it)
        draw_outline(*it, stroke_outer, stroke_holes, stroke_width);
}

void SVG::draw(const Surface &surface, std::string fill, const float fill_opacity)
{
    draw(surface.expolygon, fill, fill_opacity);
}

void SVG::draw_outline(const Surface &surface, std::string stroke_outer, std::string stroke_holes, coordf_t stroke_width)
{
    draw_outline(surface.expolygon, stroke_outer, stroke_holes, stroke_width);
}

void SVG::draw(const Surfaces &surfaces, std::string fill, const float fill_opacity)
{
    for (Surfaces::const_iterator it = surfaces.begin(); it != surfaces.end(); ++it)
        this->draw(*it, fill, fill_opacity);
}

void SVG::draw_outline(const Surfaces &surfaces, std::string stroke_outer, std::string stroke_holes, coordf_t stroke_width)
{
    for (Surfaces::const_iterator it = surfaces.begin(); it != surfaces.end(); ++ it)
        draw_outline(*it, stroke_outer, stroke_holes, stroke_width);
}

void SVG::draw(const SurfacesPtr &surfaces, std::string fill, const float fill_opacity)
{
    for (SurfacesPtr::const_iterator it = surfaces.begin(); it != surfaces.end(); ++it)
        this->draw(*(*it), fill, fill_opacity);
}

void SVG::draw_outline(const SurfacesPtr &surfaces, std::string stroke_outer, std::string stroke_holes, coordf_t stroke_width)
{
    for (SurfacesPtr::const_iterator it = surfaces.begin(); it != surfaces.end(); ++ it)
        draw_outline(*(*it), stroke_outer, stroke_holes, stroke_width);
}

void SVG::draw(const Polygon &polygon, std::string fill)
{
    this->fill = fill;
    this->path(this->get_path_d(polygon, true), !fill.empty(), 0, 1.f);
}

void SVG::draw(const Polygons &polygons, std::string fill)
{
    for (Polygons::const_iterator it = polygons.begin(); it != polygons.end(); ++it) {
        // BBS
        if (it->is_counter_clockwise())
            this->draw(*it, fill);
        else
            this->draw(*it, "white");
    }
}

void SVG::draw(const Polyline &polyline, std::string stroke, coordf_t stroke_width)
{
    this->stroke = stroke;
    this->path(this->get_path_d(polyline, false), false, stroke_width, 1.f);
}

void SVG::draw(const Polylines &polylines, std::string stroke, coordf_t stroke_width)
{
    for (Polylines::const_iterator it = polylines.begin(); it != polylines.end(); ++it)
        this->draw(*it, stroke, stroke_width);
}

void SVG::draw(const ThickLines &thicklines, const std::string &fill, const std::string &stroke, coordf_t stroke_width)
{
    for (ThickLines::const_iterator it = thicklines.begin(); it != thicklines.end(); ++it)
        this->draw(*it, fill, stroke, stroke_width);
}

void SVG::draw(const ThickPolylines &polylines, const std::string &stroke, coordf_t stroke_width)
{
    for (ThickPolylines::const_iterator it = polylines.begin(); it != polylines.end(); ++it)
        this->draw((Polyline)*it, stroke, stroke_width);
}

void SVG::draw(const ThickPolylines &thickpolylines, const std::string &fill, const std::string &stroke, coordf_t stroke_width)
{
    for (ThickPolylines::const_iterator it = thickpolylines.begin(); it != thickpolylines.end(); ++ it)
        draw(it->thicklines(), fill, stroke, stroke_width);
}

void SVG::draw(const Point &point, std::string fill, coord_t iradius)
{
    float radius = (iradius == 0) ? 3.f : to_svg_coord(iradius);
    std::ostringstream svg;
    svg << "   <circle cx=\"" << to_svg_x(point(0) - origin(0)) << "\" cy=\"" << to_svg_y(point(1) - origin(1))
        << "\" r=\"" << radius << "\" "
        << "style=\"stroke: none; fill: " << fill << "\" />";
    
    fprintf(this->f, "%s\n", svg.str().c_str());
}

void SVG::draw(const Points &points, std::string fill, coord_t radius)
{
    for (Points::const_iterator it = points.begin(); it != points.end(); ++it)
        this->draw(*it, fill, radius);
}

void SVG::draw(const ClipperLib::Path &polygon, double scale, std::string stroke, coordf_t stroke_width)
{
    this->stroke = stroke;
    this->path(this->get_path_d(polygon, scale, true), false, stroke_width, 1.f);
}

void SVG::draw(const ClipperLib::Paths &polygons, double scale, std::string stroke, coordf_t stroke_width)
{
    for (ClipperLib::Paths::const_iterator it = polygons.begin(); it != polygons.end(); ++ it)
        draw(*it, scale, stroke, stroke_width);
}

void SVG::draw_outline(const Polygon &polygon, std::string stroke, coordf_t stroke_width)
{
    this->stroke = stroke;
    this->path(this->get_path_d(polygon, true), false, stroke_width, 1.f);
}

void SVG::draw_outline(const Polygons &polygons, std::string stroke, coordf_t stroke_width)
{
    for (Polygons::const_iterator it = polygons.begin(); it != polygons.end(); ++ it)
        draw_outline(*it, stroke, stroke_width);
}

void SVG::path(const std::string &d, bool fill, coordf_t stroke_width, const float fill_opacity)
{
    float lineWidth = 0.f;
    if (! fill)
        lineWidth = (stroke_width == 0) ? 2.f : to_svg_coord(stroke_width);

    fprintf(
        this->f,
        "   <path d=\"%s\" style=\"fill: %s; stroke: %s; stroke-width: %f; fill-type: evenodd\" %s fill-opacity=\"%f\" />\n",
        d.c_str(),
        fill ? this->fill.c_str() : "none",
        this->stroke.c_str(),
        lineWidth, 
        (this->arrows && !fill) ? " marker-end=\"url(#endArrow)\"" : "",
        fill_opacity
    );
}

std::string SVG::get_path_d(const MultiPoint &mp, bool closed) const
{
    std::ostringstream d;
    d << "M ";
    for (Points::const_iterator p = mp.points.begin(); p != mp.points.end(); ++p) {
        d << to_svg_x((*p)(0) - origin(0)) << " ";
        d << to_svg_y((*p)(1) - origin(1)) << " ";
    }
    if (closed) d << "z";
    return d.str();
}

std::string SVG::get_path_d(const ClipperLib::Path &path, double scale, bool closed) const
{
    std::ostringstream d;
    d << "M ";
    for (ClipperLib::Path::const_iterator p = path.begin(); p != path.end(); ++p) {
        d << to_svg_x(scale * p->x() - origin(0)) << " ";
        d << to_svg_y(scale * p->y() - origin(1)) << " ";
    }
    if (closed) d << "z";
    return d.str();
}

// font_size: font-size={font_size*10}px
void SVG::draw_text(const Point &pt, const char *text, const char *color, int font_size)
{
    fprintf(this->f,
        "<text x=\"%f\" y=\"%f\" font-family=\"sans-serif\" font-size=\"%dpx\" fill=\"%s\">%s</text>",
        to_svg_x(pt(0)-origin(0)),
        to_svg_y(pt(1)-origin(1)),
        font_size*10, color, text);
}

void SVG::draw_legend(const Point &pt, const char *text, const char *color)
{
    fprintf(this->f,
        "<circle cx=\"%f\" cy=\"%f\" r=\"10\" fill=\"%s\"/>",
        to_svg_x(pt(0)-origin(0)),
        to_svg_y(pt(1)-origin(1)),
        color);
    fprintf(this->f,
        "<text x=\"%f\" y=\"%f\" font-family=\"sans-serif\" font-size=\"10px\" fill=\"%s\">%s</text>",
        to_svg_x(pt(0)-origin(0)) + 20.f,
        to_svg_y(pt(1)-origin(1)),
        "black", text);
}

//BBS
void SVG::draw_grid(const BoundingBox& bbox, const std::string& stroke, coordf_t stroke_width, coordf_t step)
{
    // draw grid
    Point bbox_size = bbox.size();
    if (bbox_size(0) < step || bbox_size(1) < step)
        return;

    Point start_pt(bbox.min(0), bbox.min(1));
    Point end_pt(bbox.max(1), bbox.min(1));
    for (coordf_t y = bbox.min(1); y <= bbox.max(1); y += step) {
        start_pt(1) = y;
        end_pt(1) = y;
        draw(Line(start_pt, end_pt), stroke, stroke_width);
    }

    start_pt(1) = bbox.min(1);
    end_pt(1) = bbox.max(1);
    for (coordf_t x = bbox.min(0); x <= bbox.max(0); x += step) {
        start_pt(0) = x;
        end_pt(0) = x;
        draw(Line(start_pt, end_pt), stroke, stroke_width);
    }
}

void SVG::add_comment(const std::string comment)
{
    fprintf(this->f, "<!-- %s -->\n", comment.c_str());
}

void SVG::Close()
{
    fprintf(this->f, "</svg>\n");
    fclose(this->f);
    this->f = NULL;
//    printf("SVG written to %s\n", this->filename.c_str());
}

void SVG::export_expolygons(const char *path, const BoundingBox &bbox, const Slic3r::ExPolygons &expolygons, std::string stroke_outer, std::string stroke_holes, coordf_t stroke_width)
{
    SVG svg(path, bbox);
    svg.draw(expolygons);
    svg.draw_outline(expolygons, stroke_outer, stroke_holes, stroke_width);
    svg.Close();
}

// Paint the expolygons in the order they are presented, thus the latter overwrites the former expolygon.
// 1) Paint all areas with the provided ExPolygonAttributes::color_fill and ExPolygonAttributes::fill_opacity.
// 2) Optionally paint outlines of the areas if ExPolygonAttributes::outline_width > 0.
//    Paint with ExPolygonAttributes::color_contour and ExPolygonAttributes::color_holes.
//    If color_contour is empty, color_fill is used. If color_hole is empty, color_contour is used.
// 3) Optionally paint points of all expolygon contours with ExPolygonAttributes::radius_points if radius_points > 0.
// 4) Paint ExPolygonAttributes::legend into legend using the ExPolygonAttributes::color_fill if legend is not empty.
void SVG::export_expolygons(const char *path, const std::vector<std::pair<Slic3r::ExPolygons, ExPolygonAttributes>> &expolygons_with_attributes)
{
    if (expolygons_with_attributes.empty())
        return;

    size_t num_legend = std::count_if(expolygons_with_attributes.begin(), expolygons_with_attributes.end(), [](const auto &v){ return ! v.second.legend.empty(); });
    // Format in num_columns.
    size_t num_columns = 3;
    // Width of the column.
    coord_t step_x = scale_(20.);
    Point legend_size(scale_(1.) + num_columns * step_x, scale_(0.4 + 1.3 * (num_legend + num_columns - 1) / num_columns));

    BoundingBox bbox = get_extents(expolygons_with_attributes.front().first);
    for (size_t i = 0; i < expolygons_with_attributes.size(); ++ i)
        bbox.merge(get_extents(expolygons_with_attributes[i].first));
    // Legend y.
    coord_t pos_y  = bbox.max.y() + scale_(1.5);
    bbox.merge(Point(std::max(bbox.min.x() + legend_size.x(), bbox.max.x()), bbox.max.y() + legend_size.y()));

    SVG svg(path, bbox);
    for (const auto &exp_with_attr : expolygons_with_attributes)
        svg.draw(exp_with_attr.first, exp_with_attr.second.color_fill, exp_with_attr.second.fill_opacity);
    for (const auto &exp_with_attr : expolygons_with_attributes) {
        if (exp_with_attr.second.outline_width > 0) {
            std::string color_contour = exp_with_attr.second.color_contour;
            if (color_contour.empty())
                color_contour = exp_with_attr.second.color_fill;
            std::string color_holes = exp_with_attr.second.color_holes;
            if (color_holes.empty())
                color_holes = color_contour;
            svg.draw_outline(exp_with_attr.first, color_contour, color_holes, exp_with_attr.second.outline_width);
        }
    }
    for (const auto &exp_with_attr : expolygons_with_attributes)
    	if (exp_with_attr.second.radius_points > 0)
			for (const ExPolygon &expoly : exp_with_attr.first)
    			svg.draw(to_points(expoly), exp_with_attr.second.color_points, exp_with_attr.second.radius_points);

    // Export legend.
    // 1st row
    coord_t pos_x0 = bbox.min.x() + scale_(1.);
    coord_t pos_x  = pos_x0;
    size_t  i_legend = 0;
    for (const auto &exp_with_attr : expolygons_with_attributes) {
        if (! exp_with_attr.second.legend.empty()) {
            svg.draw_legend(Point(pos_x, pos_y), exp_with_attr.second.legend.c_str(), exp_with_attr.second.color_fill.c_str());
            if ((++ i_legend) % num_columns == 0) {
                pos_x  = pos_x0;
                pos_y += scale_(1.3);
            } else {
                pos_x += step_x;
            }
        }
    }
    svg.Close();
}

}
