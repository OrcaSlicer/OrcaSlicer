#include "libslic3r/ClipperUtils.hpp"
#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"

#include "svg.hpp"
#include "nanosvg/nanosvg.h"

#include <string>

#include <boost/log/trivial.hpp>

#include "BRepBuilderAPI_MakeWire.hxx"
#include "BRepBuilderAPI_MakeEdge.hxx"
#include "BRepBuilderAPI_MakeFace.hxx"
#include "BRepPrimAPI_MakePrism.hxx"
#include "BRepBuilderAPI_Transform.hxx"
#include "BRepMesh_IncrementalMesh.hxx"
#include "TopoDS_Face.hxx"
#include "TopExp_Explorer.hxx"
#include "TopoDS.hxx"
#include "BRepExtrema_SelfIntersection.hxx"
#include "libslic3r/clipper.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r {
const double STEP_TRANS_CHORD_ERROR = 0.005;
const double STEP_TRANS_ANGLE_RES   = 1;

struct Element_Info
{
    std::string name;
    unsigned int color;
    TopoDS_Shape shape;
};

bool is_same_points(gp_Pnt pt1, gp_Pnt pt2) {
    return abs(pt1.X() - pt2.X()) < 0.001
        && abs(pt1.Y() - pt2.Y()) < 0.001
        && abs(pt1.Z() - pt2.Z()) < 0.001;
}

struct Point_2D
{
    Point_2D(float in_x, float in_y) : x(in_x), y(in_y) {}
    float x;
    float y;
};

void interp_v2_v2v2(float r[2], const float a[2], const float b[2], const float t)
{
    const float s = 1.0f - t;

    r[0] = s * a[0] + t * b[0];
    r[1] = s * a[1] + t * b[1];
}

void interp_v2_v2v2v2v2_cubic(float p[2], const float v1[2], const float v2[2], const float v3[2], const float v4[2], const float u)
{
    float q0[2], q1[2], q2[2], r0[2], r1[2];

    interp_v2_v2v2(q0, v1, v2, u);
    interp_v2_v2v2(q1, v2, v3, u);
    interp_v2_v2v2(q2, v3, v4, u);

    interp_v2_v2v2(r0, q0, q1, u);
    interp_v2_v2v2(r1, q1, q2, u);

    interp_v2_v2v2(p, r0, r1, u);
}

bool is_two_lines_interaction(gp_Pnt pL1, gp_Pnt pL2, gp_Pnt pR1, gp_Pnt pR2) {
    Vec3d point1(pL1.X(), pL1.Y(), 0);
    Vec3d point2(pL2.X(), pL2.Y(), 0);
    Vec3d point3(pR1.X(), pR1.Y(), 0);
    Vec3d point4(pR2.X(), pR2.Y(), 0);
  
    Vec3d line1 = point2 - point1;
    Vec3d line2 = point4 - point3;

    Vec3d line_pos1 = point1 - point3;
    Vec3d line_pos2 = point2 - point3;
    
    Vec3d line_pos3 = point3 - point1;
    Vec3d line_pos4 = point4 - point1;
    
    Vec3d cross_1 = line2.cross(line_pos1);
    Vec3d cross_2 = line2.cross(line_pos2);

    Vec3d cross_3 = line1.cross(line_pos3);
    Vec3d cross_4 = line1.cross(line_pos4);

    return (cross_1.dot(cross_2) < 0) && (cross_3.dot(cross_4) < 0);
}

bool is_profile_self_interaction(std::vector<std::pair<gp_Pnt, gp_Pnt>> profile_line_points)
{ 
    for (int i = 0; i < profile_line_points.size(); ++i) {
        for (int j = i + 2; j < profile_line_points.size(); ++j)
            if (is_two_lines_interaction(profile_line_points[i].first, profile_line_points[i].second, profile_line_points[j].first, profile_line_points[j].second))
                return true;
    }

    return false;
}

double get_profile_area(std::vector<std::pair<gp_Pnt, gp_Pnt>> profile_line_points)
{
    double min_x = 0;
    for (auto line_points : profile_line_points) {
        if (line_points.first.X() < min_x) min_x = line_points.first.X();
    }

    double area = 0;
    for (auto line_points : profile_line_points) {
        bool flag = true;
        if (line_points.second.Y() < line_points.first.Y()) flag = false;

        area += (line_points.second.X() + line_points.first.X() - 2 * min_x) * (line_points.second.Y() - line_points.first.Y()) / 2;
    }

    return abs(area);
}

bool get_svg_profile(const char *path, std::vector<Element_Info> &element_infos, std::string& message)
{
    NSVGimage *svg_data = nullptr;
    svg_data            = nsvgParseFromFile(path, "mm", 96.0f);
    if (svg_data == nullptr) {
        message = "import svg failed: could not open svg.";
        return false;
    }
    if (svg_data->shapes == nullptr) {
        message = "import svg failed: could not parse imported svg data.";
        return false;
    }

    int name_index = 1;
    for (NSVGshape *shape = svg_data->shapes; shape; shape = shape->next) {
        char *      id     = shape->id;

        int interpolation_precision = 10;  // Number of interpolation points
        float step = 1.0f / float(interpolation_precision - 1);

        // get the path point
        std::vector<std::vector<std::vector<Point_2D>>> all_path_points;  // paths<profiles<curves<points>>>
        for (NSVGpath *path = shape->paths; path; path = path->next) {
            std::vector<std::vector<Point_2D>> profile_points;
            int index = 0;
            for (int i = 0; i < path->npts - 1; i += 3) {
                float *            p = &path->pts[i * 2];
                float              a = 0.0f;
                std::vector<Point_2D> curve_points;  // points on a curve
                for (int v = 0; v < interpolation_precision; v++) {
                    float pt[2];

                    // get interpolation points of Bezier curve
                    interp_v2_v2v2v2v2_cubic(pt, &p[0], &p[2], &p[4], &p[6], a);

                    Point_2D point(pt[0], -pt[1]);
                    curve_points.push_back(point);
                    a += step;
                }

                profile_points.push_back(curve_points);   

                // keep the adjacent curves end-to-end
                if (profile_points.size() > 1) {
                    profile_points[index - 1].back() = profile_points[index].front();
                }

                index++;
            }

            if (!profile_points.empty())
                all_path_points.push_back(profile_points);
        }

        // remove duplicate points and ensure the profile is closed
        std::vector<std::vector<std::pair<gp_Pnt, gp_Pnt>>> path_line_points;
        for (auto profile_points : all_path_points) {
            std::vector<std::pair<gp_Pnt, gp_Pnt>> profile_line_points;
            for (int i = 0; i < profile_points.size(); ++i) {
                for (int j = 0; j + 1 < profile_points[i].size(); j++) {
                    gp_Pnt pt1(profile_points[i][j].x, profile_points[i][j].y, 0);
                    gp_Pnt pt2(profile_points[i][j + 1].x, profile_points[i][j + 1].y, 0);
                    if (is_same_points(pt1, pt2))
                        continue;

                    profile_line_points.push_back({pt1, pt2});
                }
            }

            if (profile_line_points.empty())
                continue;

            // keep the start and end points of profile connected
            if (shape->fill.gradient != nullptr)
                profile_line_points.back().second = profile_line_points[0].first;
            
            if (is_profile_self_interaction(profile_line_points))
                BOOST_LOG_TRIVIAL(warning) << "the profile is self interaction.";

            path_line_points.push_back(profile_line_points);
        }

        if (shape->fill.gradient == nullptr) {
            double scale_size = 1e6;
            std::vector<std::vector<std::pair<gp_Pnt, gp_Pnt>>> new_path_line_points;
            float stroke_width = shape->strokeWidth * scale_size;
            Polygons polygons;
            bool close_polygon = false;
            for (int i = 0; i < path_line_points.size(); ++i) {
                ClipperLib::Path pt_path;
                for (auto line_point : path_line_points[i]) { 
                    pt_path.push_back(ClipperLib::IntPoint(line_point.first.X() * scale_size, line_point.first.Y() * scale_size));
                }
                pt_path.push_back(ClipperLib::IntPoint(path_line_points[i].back().second.X() * scale_size, path_line_points[i].back().second.Y() * scale_size));

                ClipperLib::Paths         out_paths;
                ClipperLib::ClipperOffset co;
                if (pt_path.front() == pt_path.back()) {
                    co.AddPath(pt_path, ClipperLib::jtMiter, ClipperLib::etClosedLine);
                    close_polygon = true;
                } else {
                    co.AddPath(pt_path, ClipperLib::jtMiter, ClipperLib::etOpenSquare);
                    close_polygon = false;
                }
                co.Execute(out_paths, stroke_width / 2);

                for (auto out_path : out_paths) {
                    polygons.emplace_back(Polygon(out_path));
                }
            }

            if (!close_polygon)
                polygons = union_(polygons);

            std::vector<std::pair<gp_Pnt, gp_Pnt>> profile_line_points;
            for (auto polygon : polygons) {
                profile_line_points.clear();
                for (int i = 0; i < polygon.size() - 1; ++i) {
                    gp_Pnt pt1(double(polygon[i][0] / scale_size), double(polygon[i][1] / scale_size), 0);
                    gp_Pnt pt2(double(polygon[i + 1][0] / scale_size), double(polygon[i + 1][1] / scale_size), 0);
                    profile_line_points.push_back({pt1, pt2});
                }
                gp_Pnt pt1(double(polygon.back()[0] / scale_size), double(polygon.back()[1] / scale_size), 0);
                gp_Pnt pt2(double(polygon.front()[0] / scale_size), double(polygon.front()[1] / scale_size), 0);
                profile_line_points.push_back({pt1, pt2});

                new_path_line_points.push_back(profile_line_points);
            }

            path_line_points = new_path_line_points;
        }

        // generate all profile curves
        std::vector<TopoDS_Wire> wires;
        int index = 0;
        double                   max_area = 0;
        for (int i = 0; i < path_line_points.size(); ++i) {
            BRepBuilderAPI_MakeWire wire_build;
            for (auto point_item : path_line_points[i]) {
                TopoDS_Edge edge_build = BRepBuilderAPI_MakeEdge(point_item.first, point_item.second);
                wire_build.Add(edge_build);
            }
            TopoDS_Wire wire = wire_build.Wire();
            double profile_area = get_profile_area(path_line_points[i]);
            if (profile_area > max_area) {
                max_area = profile_area;
                index = i;
            }
            wires.emplace_back(wire);
        }

        if (wires.empty())
            continue;

        gp_Vec      dir(0, 0, 10);
        BRepBuilderAPI_MakeFace face_make(wires[index]);
        for (int i = 0; i < wires.size(); ++i) {
            if (index == i)
                continue;
            face_make.Add(wires[i]);
        }

        TopoDS_Face face = face_make.Face();
        TopoDS_Shape element_shape = BRepPrimAPI_MakePrism(face, dir, false, false).Shape();

        Element_Info element_info;
        element_info.name  = "part_" + std::to_string(name_index);
        element_info.color = shape->fill.color;
        element_info.shape = element_shape;
        element_infos.push_back(element_info);

        name_index++;
    }

    nsvgDelete(svg_data);
    return true;
}

bool load_svg(const char *path, Model *model, std::string &message)
{
    std::vector<Element_Info> namedSolids;
    if (!get_svg_profile(path, namedSolids, message))
        return false;

    std::vector<stl_file> stl;
    stl.resize(namedSolids.size());
    // todo: zhimin, Can be accelerated in parallel with tbb 
    for (size_t i = 0 ; i < namedSolids.size(); i++) {
        BRepMesh_IncrementalMesh mesh(namedSolids[i].shape, STEP_TRANS_CHORD_ERROR, false, STEP_TRANS_ANGLE_RES, true);
        // BBS: calculate total number of the nodes and triangles
        int aNbNodes     = 0;
        int aNbTriangles = 0;
        for (TopExp_Explorer anExpSF(namedSolids[i].shape, TopAbs_FACE); anExpSF.More(); anExpSF.Next()) {
            TopLoc_Location aLoc;
            Handle(Poly_Triangulation) aTriangulation = BRep_Tool::Triangulation(TopoDS::Face(anExpSF.Current()), aLoc);
            if (!aTriangulation.IsNull()) {
                aNbNodes += aTriangulation->NbNodes();
                aNbTriangles += aTriangulation->NbTriangles();
            }
        }

        if (aNbTriangles == 0 || aNbNodes == 0)
            // BBS: No triangulation on the shape.
            continue;

        stl[i].stats.type                = inmemory;
        stl[i].stats.number_of_facets    = (uint32_t) aNbTriangles;
        stl[i].stats.original_num_facets = stl[i].stats.number_of_facets;
        stl_allocate(&stl[i]);

        std::vector<Vec3f> points;
        points.reserve(aNbNodes);
        // BBS: count faces missing triangulation
        Standard_Integer aNbFacesNoTri = 0;
        // BBS: fill temporary triangulation
        Standard_Integer aNodeOffset    = 0;
        Standard_Integer aTriangleOffet = 0;
        for (TopExp_Explorer anExpSF(namedSolids[i].shape, TopAbs_FACE); anExpSF.More(); anExpSF.Next()) {
            const TopoDS_Shape &aFace = anExpSF.Current();
            TopLoc_Location     aLoc;
            Handle(Poly_Triangulation) aTriangulation = BRep_Tool::Triangulation(TopoDS::Face(aFace), aLoc);
            if (aTriangulation.IsNull()) {
                ++aNbFacesNoTri;
                continue;
            }
            // BBS: copy nodes
            gp_Trsf aTrsf = aLoc.Transformation();
            for (Standard_Integer aNodeIter = 1; aNodeIter <= aTriangulation->NbNodes(); ++aNodeIter) {
                gp_Pnt aPnt = aTriangulation->Node(aNodeIter);
                aPnt.Transform(aTrsf);
                points.emplace_back(std::move(Vec3f(aPnt.X(), aPnt.Y(), aPnt.Z())));
            }
            // BBS: copy triangles
            const TopAbs_Orientation anOrientation = anExpSF.Current().Orientation();
            Standard_Integer         anId[3];
            for (Standard_Integer aTriIter = 1; aTriIter <= aTriangulation->NbTriangles(); ++aTriIter) {
                Poly_Triangle aTri = aTriangulation->Triangle(aTriIter);

                aTri.Get(anId[0], anId[1], anId[2]);
                if (anOrientation == TopAbs_REVERSED) std::swap(anId[1], anId[2]);
                // BBS: save triangles facets
                stl_facet facet;
                facet.vertex[0] = points[anId[0] + aNodeOffset - 1].cast<float>();
                facet.vertex[1] = points[anId[1] + aNodeOffset - 1].cast<float>();
                facet.vertex[2] = points[anId[2] + aNodeOffset - 1].cast<float>();
                facet.extra[0]  = 0;
                facet.extra[1]  = 0;
                stl_normal normal;
                stl_calculate_normal(normal, &facet);
                stl_normalize_vector(normal);
                facet.normal                                      = normal;
                stl[i].facet_start[aTriangleOffet + aTriIter - 1] = facet;
            }

            aNodeOffset += aTriangulation->NbNodes();
            aTriangleOffet += aTriangulation->NbTriangles();
        }
    }

    ModelObject *new_object = model->add_object();
    // new_object->name ?
    new_object->input_file = path;
    auto stage_unit3 = stl.size() / LOAD_STEP_STAGE_UNIT_NUM + 1;
    for (size_t i = 0; i < stl.size(); i++) {
        // BBS: maybe mesh is empty from step file. Don't add
        if (stl[i].stats.number_of_facets > 0) {
            TriangleMesh triangle_mesh;
            triangle_mesh.from_stl(stl[i]);
            ModelVolume *new_volume       = new_object->add_volume(std::move(triangle_mesh));
            new_volume->name              = namedSolids[i].name;
            new_volume->source.input_file = path;
            new_volume->source.object_idx = (int) model->objects.size() - 1;
            new_volume->source.volume_idx = (int) new_object->volumes.size() - 1;
        }
    }
    return true;
}
} // namespace Slic3r
