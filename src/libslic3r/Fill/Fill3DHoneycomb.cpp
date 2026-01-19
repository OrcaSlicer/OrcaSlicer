#include "../ClipperUtils.hpp"
#include "../ShortestPath.hpp"
#include "../Surface.hpp"
#include "FillBase.hpp"
#include "Fill3DHoneycomb.hpp"

namespace Slic3r {

// sign function
template <typename T> int sgn(T val) {
  return (T(0) < val) - (val < T(0));
}
  
/*
Creates a contiguous sequence of points at a specified height that make
up a horizontal slice of the edges of a space filling truncated
octahedron tesselation. The octahedrons are oriented so that the
square faces are in the horizontal plane with edges parallel to the X
and Y axes.

Credits: David Eccles (gringer).
*/

// triangular wave function
// this has period (gridSize * 2), and amplitude (gridSize / 2),
// with triWave(pos = 0) = 0
static coordf_t triWave(coordf_t pos, coordf_t gridSize)
{
  float t = (pos / (gridSize * 2.)) + 0.25; // convert relative to grid size
  t = t - (int)t; // extract fractional part
  return((1. - abs(t * 8. - 4.)) * (gridSize / 4.) + (gridSize / 4.));
}

// truncated octagonal waveform, with period and offset
// as per the triangular wave function. The Z position adjusts
// the maximum offset [between -(gridSize / 4) and (gridSize / 4)], with a
// period of (gridSize * 2) and troctWave(Zpos = 0) = 0
static coordf_t troctWave(coordf_t pos, coordf_t gridSize, coordf_t Zpos)
{
  coordf_t Zcycle = triWave(Zpos, gridSize);
  coordf_t perpOffset = Zcycle / 2;
  coordf_t y = triWave(pos, gridSize);
  return((abs(y) > abs(perpOffset)) ?
	 (sgn(y) * perpOffset) :
	 (y * sgn(perpOffset)));
}

// Identify the important points of curve change within a truncated
// octahedron wave (as waveform fraction t):
// 1. Start of wave (always 0.0)
// 2. Transition to upper "horizontal" part
// 3. Transition from upper "horizontal" part
// 4. Transition to lower "horizontal" part
// 5. Transition from lower "horizontal" part
/*    o---o
 *   /     \
 * o/       \
 *           \       /
 *            \     /
 *             o---o
 */
static std::vector<coordf_t> getCriticalPoints(coordf_t Zpos, coordf_t gridSize)
{
  std::vector<coordf_t> res = {0.};
  coordf_t perpOffset = abs(triWave(Zpos, gridSize) / 2.);

  coordf_t normalisedOffset = perpOffset / gridSize;
  // // for debugging: just generate evenly-distributed points
  // for(coordf_t i = 0; i < 2; i += 0.05){
  //   res.push_back(gridSize * i);
  // }
  // note: 0 == straight line
  if(normalisedOffset > 0){
    res.push_back(gridSize * (0. + normalisedOffset));
    res.push_back(gridSize * (1. - normalisedOffset));
    res.push_back(gridSize * (1. + normalisedOffset));
    res.push_back(gridSize * (2. - normalisedOffset));
  }
  return(res);
}

// Generate an array of points that are in the same direction as the
// basic printing line (i.e. Y points for columns, X points for rows)
// Note: a negative offset only causes a change in the perpendicular
// direction
static std::vector<coordf_t> colinearPoints(const coordf_t Zpos, coordf_t gridSize, std::vector<coordf_t> critPoints,
					     const size_t baseLocation, size_t gridLength)
{
  std::vector<coordf_t> points;
  points.push_back(baseLocation);
  for (coordf_t cLoc = baseLocation; cLoc < gridLength; cLoc+= (gridSize*2)) {
    for(size_t pi = 0; pi < critPoints.size(); pi++){
      points.push_back(baseLocation + cLoc + critPoints[pi]);
    }
  }
  points.push_back(gridLength);
  return points;
}

// Generate an array of points for the dimension that is perpendicular to
// the basic printing line (i.e. X points for columns, Y points for rows)
  static std::vector<coordf_t> perpendPoints(const coordf_t Zpos, coordf_t gridSize, std::vector<coordf_t> critPoints,
					     size_t baseLocation, size_t gridLength,
                                             size_t offsetBase, coordf_t perpDir)
{
  std::vector<coordf_t> points;
  points.push_back(offsetBase);
  for (coordf_t cLoc = baseLocation; cLoc < gridLength; cLoc+= gridSize*2) {
    for(size_t pi = 0; pi < critPoints.size(); pi++){
      coordf_t offset = troctWave(critPoints[pi], gridSize, Zpos);
      points.push_back(offsetBase + (offset * perpDir));
    }
  }
  points.push_back(offsetBase);
  return points;
}

static inline Pointfs zip(const std::vector<coordf_t> &x, const std::vector<coordf_t> &y)
{
    assert(x.size() == y.size());
    Pointfs out;
    out.reserve(x.size());
    for (size_t i = 0; i < x.size(); ++ i)
        out.push_back(Vec2d(x[i], y[i]));
    return out;
}

// Generate a set of curves (array of array of 2d points) that describe a
// horizontal slice of a truncated regular octahedron.
static std::vector<Pointfs> makeActualGrid(coordf_t Zpos, coordf_t gridSize, size_t boundsX, size_t boundsY)
{
  std::vector<Pointfs> points;
  std::vector<coordf_t> critPoints = getCriticalPoints(Zpos, gridSize);
  coordf_t zCycle = fmod(Zpos + gridSize/2, gridSize * 2.) / (gridSize * 2.);
  bool printVert = zCycle < 0.5;
  if (printVert) {
    int perpDir = -1;
    for (coordf_t x = 0; x <= (boundsX); x+= gridSize, perpDir *= -1) {
      points.push_back(Pointfs());
      Pointfs &newPoints = points.back();
      newPoints = zip(
		      perpendPoints(Zpos, gridSize, critPoints, 0, boundsY, x, perpDir),
		      colinearPoints(Zpos, gridSize, critPoints, 0, boundsY));
      if (perpDir == 1)
	std::reverse(newPoints.begin(), newPoints.end());
    }
  } else {
    int perpDir = 1;
    for (coordf_t y = gridSize; y <= (boundsY); y+= gridSize, perpDir *= -1) {
      points.push_back(Pointfs());
      Pointfs &newPoints = points.back();
      newPoints = zip(
		      colinearPoints(Zpos, gridSize, critPoints, 0, boundsX),
		      perpendPoints(Zpos, gridSize, critPoints, 0, boundsX, y, perpDir));
      if (perpDir == -1)
	std::reverse(newPoints.begin(), newPoints.end());
    }
  }
  return points;
}

// Generate a set of curves (array of array of 2d points) that describe a
// horizontal slice of a truncated regular octahedron with a specified
// grid square size.
// gridWidth and gridHeight define the width and height of the bounding box respectively
static Polylines makeGrid(coordf_t z, coordf_t gridSize, coordf_t boundWidth, coordf_t boundHeight, bool fillEvenly)
{
  std::vector<Pointfs> polylines = makeActualGrid(z, gridSize, boundWidth, boundHeight);
  Polylines result;
  result.reserve(polylines.size());
  for (std::vector<Pointfs>::const_iterator it_polylines = polylines.begin();
       it_polylines != polylines.end(); ++ it_polylines) {
    result.push_back(Polyline());
    Polyline &polyline = result.back();
    for (Pointfs::const_iterator it = it_polylines->begin(); it != it_polylines->end(); ++ it)
      polyline.points.push_back(Point(coord_t((*it)(0)), coord_t((*it)(1))));
  }
  return result;
}

// FillParams has the following useful information:
// density <0 .. 1>  [proportion of space to fill]
// anchor_length     [???]
// anchor_length_max [???]
// dont_connect()    [avoid connect lines]
// dont_adjust       [avoid filling space evenly]
// monotonic         [fill strictly left to right]
// complete          [complete each loop]

void Fill3DHoneycomb::_fill_surface_single(
    const FillParams                &params,
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction,
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    // no rotation is supported for this infill pattern
    // Support infill angle 
    auto infill_angle   = float(this->angle);
    if (std::abs(infill_angle) >= EPSILON) expolygon.rotate(-infill_angle);
    BoundingBox bb = expolygon.contour.bounding_box();

    // Expand the bounding box to avoid artifacts at the edges
    coord_t expand = 5 * (scale_(this->spacing));
    bb.offset(expand); 

    // Note: with equally-scaled X/Y/Z, the pattern will create a vertically-stretched
    // truncated octahedron; so Z is pre-adjusted first by scaling by sqrt(2)
    coordf_t zScale = sqrt(2);

    // adjustment to account for the additional distance of octagram curves
    // note: this only strictly applies for a rectangular area where the total
    //       Z travel distance is a multiple of the spacing... but it should
    //       be at least better than the prevous estimate which assumed straight
    //       lines
    // = 4 * integrate(func=4*x(sqrt(2) - 1) + 1, from=0, to=0.25)
    // = (sqrt(2) + 1) / 2 [... I think]
    // make a first guess at the preferred grid Size
    coordf_t gridSize = (scale_(this->spacing) * ((zScale + 1.) / 2.) * params.multiline  / params.density);

    // This density calculation is incorrect for many values > 25%, possibly
    // due to quantisation error, so this value is used as a first guess, then the
    // Z scale is adjusted to make the layer patterns consistent / symmetric
    // This means that the resultant infill won't be an ideal truncated octahedron,
    // but it should look better than the equivalent quantised version

    coordf_t layerHeight = scale_(thickness_layers);
    // ceiling to an integer value of layers per Z
    // (with a little nudge in case it's close to perfect)
    coordf_t layersPerModule = floor((gridSize * 2) / (zScale * layerHeight) + 0.05);
    if(params.density > 0.42){ // exact layer pattern for >42% density
      layersPerModule = 2;
      // re-adjust the grid size for a partial octahedral path
      // (scale of 1.1 guessed based on modeling)
      gridSize = (scale_(this->spacing) * 1.1 * params.multiline  / params.density);
      // re-adjust zScale to make layering consistent
      zScale = (gridSize * 2) / (layersPerModule * layerHeight);
    } else {
      if(layersPerModule < 2){
	layersPerModule = 2;
      }
      // re-adjust zScale to make layering consistent
      zScale = (gridSize * 2) / (layersPerModule * layerHeight);
      // re-adjust the grid size to account for the new zScale
      gridSize = (scale_(this->spacing) * ((zScale + 1.) / 2.) * params.multiline  / params.density);
      // re-calculate layersPerModule and zScale
      layersPerModule = floor((gridSize * 2) / (zScale * layerHeight) + 0.05);
      if(layersPerModule < 2){
	layersPerModule = 2;
      }
      zScale = (gridSize * 2) / (layersPerModule * layerHeight);
    }

    // align bounding box to a multiple of our honeycomb grid module
    // (a module is 2*$gridSize since one $gridSize half-module is 
    // growing while the other $gridSize half-module is shrinking)
    bb.merge(align_to_grid(bb.min, Point(gridSize*4, gridSize*4)));

    // generate pattern
    Polylines polylines =
      makeGrid(
	       scale_(this->z) * zScale,
	       gridSize,
	       bb.size()(0),
	       bb.size()(1),
	       !params.dont_adjust);

    // move pattern in place
    for (Polyline &pl : polylines){
      pl.translate(bb.min);
      pl.simplify(5 * spacing); // simplify to 5x line width
    }

    // Apply multiline offset if needed
    multiline_fill(polylines, params, spacing);

    // clip pattern to boundaries, chain the clipped polylines
    polylines = intersection_pl(polylines, to_polygons(expolygon));

    if (! polylines.empty()) {
    // Remove very small bits, but be careful to not remove infill lines connecting thin walls!
    // The infill perimeter lines should be separated by around a single infill line width.
    const double minlength = scale_(0.8 * this->spacing);
    polylines.erase(
	std::remove_if(polylines.begin(), polylines.end(), [minlength](const Polyline &pl) { return pl.length() < minlength; }),
	polylines.end());
    }

    // copy from fliplines
    if (!polylines.empty()) {
        int infill_start_idx = polylines_out.size(); // only rotate what belongs to us.
        // connect lines
        chain_or_connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);

        // rotate back
        if (std::abs(infill_angle) >= EPSILON) {
          for (auto it = polylines_out.begin() + infill_start_idx; it != polylines_out.end(); ++it) 
            it->rotate(infill_angle);
        }
    }
}

} // namespace Slic3r