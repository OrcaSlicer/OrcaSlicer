#ifndef slic3r_CutSurface_hpp_
#define slic3r_CutSurface_hpp_

#include <vector>
#include <admesh/stl.h> // indexed_triangle_set
#include "ExPolygon.hpp"
#include "Emboss.hpp" // IProjection

namespace Slic3r{

/// <summary>
/// Represents cutted surface from object
/// Extend index triangle set by outlines
/// </summary>
struct SurfaceCut : public indexed_triangle_set
{
    // vertex indices(index to mesh vertices)
    using Index = unsigned int;
    using Contour = std::vector<Index>;
    using Contours = std::vector<Contour>;
    // list of circulated open surface
    Contours contours;
};

/// <summary>
/// Cut surface shape from models.
/// </summary>
/// <param name="shapes">Multiple shape to cut from model</param>
/// <param name="models">Multi mesh to cut, need to be in same coordinate system</param>
/// <param name="projection">Define transformation 2d shape into 3d</param>
/// <param name="projection_ratio">Define ideal ratio between front and back projection to cut
/// 0 .. means use closest to front projection
/// 1 .. means use closest to back projection
/// value from <0, 1>
/// </param>
/// <returns>Cutted surface from model</returns>
SurfaceCut cut_surface(const ExPolygons                        &shapes,
                       const std::vector<indexed_triangle_set> &models,
                       const Emboss::IProjection               &projection,
                       float projection_ratio);

/// <summary>
/// Create model from surface cuts by projection
/// </summary>
/// <param name="cut">Surface from model with outlines</param>
/// <param name="projection">Way of emboss</param>
/// <returns>Mesh</returns>
indexed_triangle_set cut2model(const SurfaceCut         &cut,
                               const Emboss::IProject3d &projection);

/// <summary>
/// Separate (A)rea (o)f (I)nterest .. AoI from model
/// NOTE: Only 2d filtration, do not filtrate by Z coordinate
/// </summary>
/// <param name="its">Input model</param>
/// <param name="bb">Bounding box to project into space</param>
/// <param name="projection">Define tranformation of BB into space</param>
/// <returns>Triangles lay at least partialy inside of projected Bounding box</returns>
indexed_triangle_set its_cut_AoI(const indexed_triangle_set &its,
                                 const BoundingBox          &bb,
                                 const Emboss::IProjection  &projection);

/// <summary>
/// Separate triangles by mask
/// </summary>
/// <param name="its">Input model</param>
/// <param name="mask">Mask - same size as its::indices</param>
/// <returns>Copy of indices by mask(with their vertices)</returns>
indexed_triangle_set its_mask(const indexed_triangle_set &its, const std::vector<bool> &mask);

bool corefine_test(const std::string &model_path, const std::string &shape_path);

} // namespace Slic3r
#endif // slic3r_CutSurface_hpp_
