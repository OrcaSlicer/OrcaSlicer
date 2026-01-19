#ifndef slic3r_SVG_hpp_
#define slic3r_SVG_hpp_

#include "libslic3r.h"
#include "clipper.hpp"
#include "ExPolygon.hpp"
#include "Line.hpp"
#include "TriangleMesh.hpp"
#include "Surface.hpp"

namespace Slic3r {

class SVG
{
public:
    bool arrows;
    std::string fill, stroke;
    Point origin;
    float height;
    bool  flipY;

    SVG() = default;
    SVG(const char* afilename) :
        arrows(false), fill("grey"), stroke("black"), filename(afilename), flipY(false)
        { open(filename); }
    SVG(const char* afilename, const BoundingBox &bbox, const coord_t bbox_offset = scale_(1.), bool flipY = true) : 
        arrows(false), fill("grey"), stroke("black"), filename(afilename), origin(bbox.min - Point(bbox_offset, bbox_offset)), flipY(flipY)
        { open(filename, bbox, bbox_offset, flipY); }
    SVG(const std::string &filename) :
        arrows(false), fill("grey"), stroke("black"), filename(filename), flipY(false)
        { open(filename); }
    SVG(const std::string &filename, const BoundingBox &bbox, const coord_t bbox_offset = scale_(1.), bool flipY = true) : 
        arrows(false), fill("grey"), stroke("black"), filename(filename), origin(bbox.min - Point(bbox_offset, bbox_offset)), flipY(flipY)
        { open(filename, bbox, bbox_offset, flipY); }
    ~SVG() { if (f != NULL) Close(); }

    bool open(const char* filename);
    bool open(const char* filename, const BoundingBox &bbox, const coord_t bbox_offset = scale_(1.), bool flipY = true);
    bool open(const std::string &filename) 
        { return open(filename.c_str()); }
    bool open(const std::string &filename, const BoundingBox &bbox, const coord_t bbox_offset = scale_(1.), bool flipY = true)
        { return open(filename.c_str(), bbox, bbox_offset, flipY); }
    bool is_opened() { return f != NULL; }

    void draw(const Line &line, std::string stroke = "black", coordf_t stroke_width = 0);
    void draw(const ThickLine &line, const std::string &fill, const std::string &stroke, coordf_t stroke_width = 0);
    void draw(const Lines &lines, std::string stroke = "black", coordf_t stroke_width = 0);
    
    void draw(const ExPolygon &expolygon, std::string fill = "grey", const float fill_opacity=1.f);
    void draw_outline(const ExPolygon &polygon, std::string stroke_outer = "black", std::string stroke_holes = "blue", coordf_t stroke_width = 0);
    void draw(const ExPolygons &expolygons, std::string fill = "grey", const float fill_opacity=1.f);
    void draw_outline(const ExPolygons &polygons, std::string stroke_outer = "black", std::string stroke_holes = "blue", coordf_t stroke_width = 0);

    void draw(const Surface &surface, std::string fill = "grey", const float fill_opacity=1.f);
    void draw_outline(const Surface &surface, std::string stroke_outer = "black", std::string stroke_holes = "blue", coordf_t stroke_width = 0);
    void draw(const Surfaces &surfaces, std::string fill = "grey", const float fill_opacity=1.f);
    void draw_outline(const Surfaces &surfaces, std::string stroke_outer = "black", std::string stroke_holes = "blue", coordf_t stroke_width = 0);
    void draw(const SurfacesPtr &surfaces, std::string fill = "grey", const float fill_opacity=1.f);
    void draw_outline(const SurfacesPtr &surfaces, std::string stroke_outer = "black", std::string stroke_holes = "blue", coordf_t stroke_width = 0);
 
    void draw(const Polygon &polygon, std::string fill = "grey");
    void draw_outline(const Polygon &polygon, std::string stroke = "black", coordf_t stroke_width = 0);
    void draw(const Polygons &polygons, std::string fill = "grey");
    void draw_outline(const Polygons &polygons, std::string stroke = "black", coordf_t stroke_width = 0);
    void draw(const Polyline &polyline, std::string stroke = "black", coordf_t stroke_width = 0);
    void draw(const Polylines &polylines, std::string stroke = "black", coordf_t stroke_width = 0);
    void draw(const ThickLines &thicklines, const std::string &fill = "lime", const std::string &stroke = "black", coordf_t stroke_width = 0);
    void draw(const ThickPolylines &polylines, const std::string &stroke = "black", coordf_t stroke_width = 0);
    void draw(const ThickPolylines &thickpolylines, const std::string &fill, const std::string &stroke, coordf_t stroke_width);
    void draw(const Point &point, std::string fill = "black", coord_t radius = 0);
    void draw(const Points &points, std::string fill = "black", coord_t radius = 0);

    // Support for rendering the ClipperLib paths
    void draw(const ClipperLib::Path  &polygon, double scale, std::string fill = "grey", coordf_t stroke_width = 0);
    void draw(const ClipperLib::Paths &polygons, double scale, std::string fill = "grey", coordf_t stroke_width = 0);

    void draw_text(const Point &pt, const char *text, const char *color, int font_size = 20);
    void draw_legend(const Point &pt, const char *text, const char *color);
    //BBS
    void draw_grid(const BoundingBox& bbox, const std::string& stroke = "black", coordf_t stroke_width = scale_(0.05), coordf_t step=scale_(1.0));
    void add_comment(const std::string comment);

    void Close();
    
    private:
    std::string filename;
    FILE* f;
    
    void path(const std::string &d, bool fill, coordf_t stroke_width, const float fill_opacity);
    std::string get_path_d(const MultiPoint &mp, bool closed = false) const;
    std::string get_path_d(const ClipperLib::Path &mp, double scale, bool closed = false) const;

public:
    static void export_expolygons(const char *path, const BoundingBox &bbox, const Slic3r::ExPolygons &expolygons, std::string stroke_outer = "black", std::string stroke_holes = "blue", coordf_t stroke_width = 0);
    static void export_expolygons(const std::string &path, const BoundingBox &bbox, const Slic3r::ExPolygons &expolygons, std::string stroke_outer = "black", std::string stroke_holes = "blue", coordf_t stroke_width = 0) 
        { export_expolygons(path.c_str(), bbox, expolygons, stroke_outer, stroke_holes, stroke_width); }
    static void export_expolygons(const char *path, const Slic3r::ExPolygons &expolygons, std::string stroke_outer = "black", std::string stroke_holes = "blue", coordf_t stroke_width = 0)
        { export_expolygons(path, get_extents(expolygons), expolygons, stroke_outer, stroke_holes, stroke_width); }
    static void export_expolygons(const std::string &path, const Slic3r::ExPolygons &expolygons, std::string stroke_outer = "black", std::string stroke_holes = "blue", coordf_t stroke_width = 0) 
        { export_expolygons(path.c_str(), get_extents(expolygons), expolygons, stroke_outer, stroke_holes, stroke_width); }

    struct ExPolygonAttributes
    {
        ExPolygonAttributes() : ExPolygonAttributes("gray", "black", "blue") {}
        ExPolygonAttributes(const std::string &color) :
            ExPolygonAttributes(color, color, color) {}

        ExPolygonAttributes(
            const std::string &color_fill,
            const std::string &color_contour,
            const std::string &color_holes,
            const coord_t      outline_width = scale_(0.05),
            const float        fill_opacity  = 0.5f,
            const std::string &color_points = "black",
            const coord_t      radius_points = 0) :
            color_fill      (color_fill),
            color_contour   (color_contour),
            color_holes     (color_holes),
            outline_width   (outline_width),
            fill_opacity    (fill_opacity),
            color_points 	(color_points),
            radius_points	(radius_points)
            {}

        ExPolygonAttributes(
            const std::string &legend,
            const std::string &color_fill,
            const std::string &color_contour,
            const std::string &color_holes,
            const coord_t      outline_width = scale_(0.05),
            const float        fill_opacity  = 0.5f,
            const std::string &color_points = "black",
            const coord_t      radius_points = 0) :
            legend          (legend),
            color_fill      (color_fill),
            color_contour   (color_contour),
            color_holes     (color_holes),
            outline_width   (outline_width),
            fill_opacity    (fill_opacity),
            color_points    (color_points),
            radius_points   (radius_points)
            {}

        ExPolygonAttributes(
            const std::string &legend,
            const std::string &color_fill,
            const float        fill_opacity) :
            legend          (legend),
            color_fill      (color_fill),
            fill_opacity    (fill_opacity)
            {}

        std::string     legend;
        std::string     color_fill;
        std::string     color_contour;
        std::string     color_holes;
        std::string   	color_points;
        coord_t         outline_width { 0 };
        float           fill_opacity;
        coord_t			radius_points { 0 };
    };

    // Paint the expolygons in the order they are presented, thus the latter overwrites the former expolygon.
    // 1) Paint all areas with the provided ExPolygonAttributes::color_fill and ExPolygonAttributes::fill_opacity.
    // 2) Optionally paint outlines of the areas if ExPolygonAttributes::outline_width > 0.
    //    Paint with ExPolygonAttributes::color_contour and ExPolygonAttributes::color_holes.
    //    If color_contour is empty, color_fill is used. If color_hole is empty, color_contour is used.
    // 3) Optionally paint points of all expolygon contours with ExPolygonAttributes::radius_points if radius_points > 0.
    // 4) Paint ExPolygonAttributes::legend into legend using the ExPolygonAttributes::color_fill if legend is not empty. 
    static void export_expolygons(const char *path, const std::vector<std::pair<Slic3r::ExPolygons, ExPolygonAttributes>> &expolygons_with_attributes);
    static void export_expolygons(const std::string &path, const std::vector<std::pair<Slic3r::ExPolygons, ExPolygonAttributes>> &expolygons_with_attributes) 
        { export_expolygons(path.c_str(), expolygons_with_attributes); }

private:
    static float    to_svg_coord(float x) throw() { return unscale<float>(x) * 10.f; }
    static float    to_svg_x(float x) throw() { return to_svg_coord(x); }
    float           to_svg_y(float x) const throw() { return flipY ? this->height - to_svg_coord(x) : to_svg_coord(x); }
};

}

#endif
