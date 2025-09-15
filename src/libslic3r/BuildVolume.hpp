#ifndef slic3r_BuildVolume_hpp_
#define slic3r_BuildVolume_hpp_

#include "Point.hpp"
#include "Geometry/Circle.hpp"
#include "Polygon.hpp"
#include "BoundingBox.hpp"
#include <admesh/stl.h>

#include <string_view>

namespace Slic3r {

struct GCodeProcessorResult;
enum class BuildVolume_Type : unsigned char {
  // Not set yet or undefined.
  Invalid,
  // Rectangular print bed. Most common, cheap to work with.
  Rectangle,
  // Circular print bed. Common on detals, cheap to work with.
  Circle,
  // Convex print bed. Complex to process.
  Convex,
  // Some non convex shape.
  Custom
};
// For collision detection of objects and G-code (extrusion paths) against the build volume.
class BuildVolume
{
public:


    // Initialized to empty, all zeros, Invalid.
    BuildVolume() {}
    // Initialize from PrintConfig::printable_area and PrintConfig::printable_height
    BuildVolume(const std::vector<Vec2d> &printable_area, const double printable_height);

    // Source data, unscaled coordinates.
    const std::vector<Vec2d>&   printable_area()         const { return m_bed_shape; }
    double                      printable_height()  const { return m_max_print_height; }
    
    // Derived data
    BuildVolume_Type                        type()              const { return m_type; }
    // Format the type for console output.
    static std::string_view     type_name(BuildVolume_Type type);
    std::string_view            type_name()         const { return type_name(m_type); }
    bool                        valid()             const { return m_type != BuildVolume_Type::Invalid; }
    // Same as printable_area(), but scaled coordinates.
    const Polygon&              polygon()           const { return m_polygon; }
    // Bounding box of polygon(), scaled.
    const BoundingBox&          bounding_box()      const { return m_bbox; }
    // Bounding volume of printable_area(), printable_height(), unscaled.
    const BoundingBoxf3&        bounding_volume()   const { return m_bboxf; }
    BoundingBoxf                bounding_volume2d() const { return { to_2d(m_bboxf.min), to_2d(m_bboxf.max) }; }
    indexed_triangle_set        bounding_mesh(bool scale=true) const;

    // Center of the print bed, unscaled.
    Vec2d                       bed_center()        const { return to_2d(m_bboxf.center()); }
    // Convex hull of polygon(), scaled.
    const Polygon&              convex_hull()       const { return m_convex_hull; }
    // Smallest enclosing circle of polygon(), scaled.
    const Geometry::Circled&    circle()            const { return m_circle; }

    enum class ObjectState : unsigned char
    {
        // Inside the build volume, thus printable.
        Inside,
        // Colliding with the build volume boundary, thus not printable and error is shown.
        Colliding,
        // Outside of the build volume means the object is ignored: Not printed and no error is shown.
        Outside,
        // Completely below the print bed. The same as Outside, but an object with one printable part below the print bed 
        // and at least one part above the print bed is still printable.
        Below,
    };

    // 1) Tests called on the plater.
    // Using SceneEpsilon for all tests.
    static constexpr const double SceneEpsilon = EPSILON;
    // Called by Plater to update Inside / Colliding / Outside state of ModelObjects before slicing.
    // Called from Model::update_print_volume_state() -> ModelObject::update_instances_print_volume_state()
    // Using SceneEpsilon
    ObjectState  object_state(const indexed_triangle_set &its, const Transform3f &trafo, bool may_be_below_bed, bool ignore_bottom = true) const;
    // Called by GLVolumeCollection::check_outside_state() after an object is manipulated with gizmos for example.
    // Called for a rectangular bed:
    ObjectState  volume_state_bbox(const BoundingBoxf3& volume_bbox, bool ignore_bottom = true) const;

    // 2) Test called on G-code paths.
    // Using BedEpsilon for all tests.
    static constexpr const double BedEpsilon = 3. * EPSILON;
    // Called on final G-code paths.
    //FIXME The test does not take the thickness of the extrudates into account!
    bool         all_paths_inside(const GCodeProcessorResult& paths, const BoundingBoxf3& paths_bbox, bool ignore_bottom = true) const;

    const std::pair<std::vector<Vec2d>, std::vector<Vec2d>>& top_bottom_convex_hull_decomposition_scene() const { return m_top_bottom_convex_hull_decomposition_scene; }
    const std::pair<std::vector<Vec2d>, std::vector<Vec2d>>& top_bottom_convex_hull_decomposition_bed() const { return m_top_bottom_convex_hull_decomposition_bed; }

private:
    // Source definition of the print bed geometry (PrintConfig::printable_area)
    std::vector<Vec2d>  m_bed_shape;
    // Source definition of the print volume height (PrintConfig::printable_height)
    double              m_max_print_height { 0.f };

    // Derived values.
    BuildVolume_Type                m_type { BuildVolume_Type::Invalid };
    // Geometry of the print bed, scaled copy of m_bed_shape.
    Polygon             m_polygon;
    // Scaled snug bounding box around m_polygon.
    BoundingBox         m_bbox;
    // 3D bounding box around m_shape, m_max_print_height.
    BoundingBoxf3       m_bboxf;
    // Area of m_polygon, scaled.
    double              m_area { 0. };
    // Convex hull of m_polygon, scaled.
    Polygon             m_convex_hull;
    // For collision detection against a convex build volume. Only filled in for m_type == Convex or Custom.
    // Variant with SceneEpsilon applied.
    std::pair<std::vector<Vec2d>, std::vector<Vec2d>>   m_top_bottom_convex_hull_decomposition_scene;
    // Variant with BedEpsilon applied.
    std::pair<std::vector<Vec2d>, std::vector<Vec2d>>   m_top_bottom_convex_hull_decomposition_bed;
    // Smallest enclosing circle of m_polygon, scaled.
    Geometry::Circled   m_circle { Vec2d::Zero(), 0 };
};

} // namespace Slic3r

#endif // slic3r_BuildVolume_hpp_
