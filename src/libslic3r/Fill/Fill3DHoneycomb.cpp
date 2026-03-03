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

// Generate a polyline that describes a single path segment through
// the infill in the same direction as the basic printing line (i.e. X
// points for columns, Y points for rows)
  static Polyline patternPoints(const coordf_t Zpos, coordf_t gridSize, std::vector<coordf_t> critPoints,
                                coordf_t baseLocation, size_t gridLength,
                                coordf_t offsetBase, coordf_t perpDir, int print_dir)
{
  Polyline line;
  if(print_dir == 1){
    line.points.push_back(Point(offsetBase, baseLocation));
  } else {
    line.points.push_back(Point(baseLocation, offsetBase));
  }
  for (coordf_t cLoc = baseLocation; cLoc < gridLength; cLoc+= gridSize*2) {
    for(size_t pi = 0; pi < critPoints.size(); pi++){
      coordf_t offset = troctWave(critPoints[pi], gridSize, Zpos);
      if(print_dir == 1){
        line.points.push_back(Point(offsetBase + (offset * perpDir), baseLocation + cLoc + critPoints[pi]));
      } else {
        line.points.push_back(Point(baseLocation + cLoc + critPoints[pi], offsetBase + (offset * perpDir)));
      }
    }
  }
  if(print_dir == 1){
    line.points.push_back(Point(offsetBase, gridLength));
  } else {
    line.points.push_back(Point(gridLength, offsetBase));
  }
  return line;
}

// Add additional dense fill in line with the pattern direction to
// cover the top squares of the pattern
static Polylines addTops(coordf_t Zpos, coordf_t gridSize, size_t boundsX, size_t boundsY, coordf_t spacing,
                         size_t multiline_count, size_t topDistance)
{
  float pointsPerSquare = 10;
  coordf_t zCycle = fmod(Zpos + gridSize/2, gridSize * 2.) / (gridSize * 2.);
  coordf_t zHalfCycle = fmod(zCycle, 0.5) * 2.;
  bool printVert = zCycle < 0.5;
  coordf_t perpOffset = abs(triWave(Zpos, gridSize) / 2.);
  Polylines lines;
  size_t pointCount = 0;
  // top cover extents in the direction of travel (a bit longer, to help fuse the cover)
  coordf_t gridStartL = gridSize * 0.25 - spacing / 2.;
  coordf_t gridEndL = gridSize * 0.75 + spacing / 2.;
  // top cover extents perpendicular to the direction of travel
  coordf_t gridStartP = gridSize * 0.25 + spacing;
  coordf_t gridEndP = gridSize * 0.75 - spacing;
  coordf_t x, y;
  int xm, ym;
  // if the print direction needs to be rotated, then swap the extents
  if((topDistance % 2) == 0){
    std::swap(gridStartL, gridStartP);
    std::swap(gridEndL, gridEndP);
  }
  // adjust spacing so that it starts and ends on exactly the right place
  coordf_t region_count = floor((gridEndP - gridStartP) / (spacing * multiline_count));
  spacing = (gridEndP - gridStartP) / region_count;
  for (x = 0, xm = 0; x <= (boundsX); x+= gridSize, xm = xm ^ 1) {
    for (y = 0, ym = 0; y <= (boundsY); y += gridSize, ym = ym ^ 1) {
      if(((xm ^ ym) == 1) == printVert){
        continue;
      }
      Polyline newPoints;
      int dirMod = xm ^ ym;
      if(printVert == (topDistance % 2)){
        if(y < boundsY){
          for(coordf_t xi = gridStartP; xi < (gridEndP + EPSILON); xi += spacing, dirMod = dirMod ^ 1){
            newPoints.points.push_back((dirMod == 0) ? Point(x + xi, y + gridStartL) : Point(x + xi, y + gridEndL));
            newPoints.points.push_back((dirMod == 0) ? Point(x + xi, y + gridEndL) : Point(x + xi, y + gridStartL));
            pointCount += 2;
          }
        }
      } else {
        if(x < boundsX){
          for(coordf_t yi = gridStartP; yi < (gridEndP + EPSILON); yi += spacing, dirMod = dirMod ^ 1){
            newPoints.points.push_back((dirMod == 0) ? Point(x + gridStartL, y + yi) : Point(x + gridEndL, y + yi));
            newPoints.points.push_back((dirMod == 0) ? Point(x + gridEndL, y + yi) : Point(x + gridStartL, y + yi));
            pointCount += 2;
          }
        }
      }
      lines.push_back(newPoints);
    }
  }
  return lines;
}

// Generate a set of curves (array of array of 2d points) that describe a
// horizontal slice of a truncated regular octahedron.
static Polylines makeZigZag(coordf_t Zpos, coordf_t gridSize, size_t boundsX, size_t boundsY)
{
  Polylines lines;
  std::vector<coordf_t> critPoints = getCriticalPoints(Zpos, gridSize);
  coordf_t zCycle = fmod(Zpos + gridSize/2, gridSize * 2.) / (gridSize * 2.);
  bool printVert = zCycle < 0.5;
  if (printVert) {
    int perpDir = -1;
    for (coordf_t x = 0; x <= (boundsX); x+= gridSize, perpDir *= -1) {
      Polyline newPoints;
      newPoints = patternPoints(Zpos, gridSize, critPoints, 0, boundsY, x, perpDir, 1);
      if (perpDir == 1)
        std::reverse(newPoints.points.begin(), newPoints.points.end());
      lines.push_back(newPoints);
    }
  } else {
    int perpDir = 1;
    for (coordf_t y = gridSize; y <= (boundsY); y+= gridSize, perpDir *= -1) {
      Polyline newPoints;
      newPoints = patternPoints(Zpos, gridSize, critPoints, 0, boundsX, y, perpDir,0);
      if (perpDir == -1)
        std::reverse(newPoints.points.begin(), newPoints.points.end());
      lines.push_back(newPoints);
    }
  }
  return lines;
}

// Generate a set of curves (array of array of 2d points) that describe a
// horizontal slice of a truncated regular octahedron with a specified
// grid square size.
// gridWidth and gridHeight define the width and height of the bounding box respectively
// Note: this uses the 'complete' infill parameter to determine if the
//       square tops should be enclosed (true) or open (false). Alternatively,
//       a rotation angle of 180 degrees or greater can be used.
static Polylines makeGrid(coordf_t z, coordf_t zLast, coordf_t gridSize,
                          coordf_t boundWidth, coordf_t boundHeight,
                          bool completeTops, coordf_t spacing, size_t multiline_count)
{
  coordf_t zCycle = fmod(z + gridSize/2, gridSize * 2.) / (gridSize * 2.);
  bool printVert = zCycle < 0.5;
  coordf_t zCycleLast = fmod(zLast + gridSize/2, gridSize * 2.) / (gridSize * 2.);
  bool printVertLast = zCycleLast < 0.5;
  Polylines result = makeZigZag(z, gridSize, boundWidth, boundHeight);
  if(completeTops && (printVert != printVertLast)){
    coordf_t layer_height = (z - zLast) / multiline_count;
    size_t top_distance = 0;
    for(coordf_t zCheck = z; zCheck >= (zLast + EPSILON); zCheck -= layer_height, top_distance++){
      coordf_t zCheckCycle = fmod(zCheck + gridSize/2, gridSize * 2.) / (gridSize * 2.);
      if(printVert != (zCheckCycle < 0.5)){
        break;
      }
    }
    // only print tops for the first layer in each cycle
    Polylines polytops = addTops(z, gridSize, boundWidth, boundHeight, spacing, multiline_count, top_distance);
    result.insert(result.end(), polytops.begin(), polytops.end());
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
    // make a first guess at the preferred grid Size (in unscaled units)
    coordf_t gridSize = (scale_(this->spacing) * ((zScale + 1.) / 2.) * params.multiline  / params.density);

    // This density calculation is incorrect for many values > 25%,
    // possibly due to quantisation error, so this value is used as a
    // first guess, then the Z scale is adjusted to make the layer
    // patterns consistent / symmetric This means that the resultant
    // infill won't be an ideal truncated octahedron, but the
    // consistent repeating pattern should look better than the
    // equivalent quantised version

    coordf_t layerHeight = scale_(params.layer_height);
    // adjust the layer height to an integer value of layers per Z
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
	       scale_(this->z - (params.layer_height * params.multiline)) * zScale,
	       gridSize,
	       bb.size()(0),
	       bb.size()(1),
	       params.infill_complete_top,
               scale_(this->spacing),
               params.multiline);

    // move pattern in place
    for (Polyline &pl : polylines){
      pl.translate(bb.min);
      pl.simplify(5 * spacing); // simplify to 5x line width
    }

    // Apply multiline offset if needed
    multiline_fill(polylines, params, spacing);

    // clip pattern to boundaries, chain the clipped polylines
    polylines = intersection_pl(std::move(polylines), to_polygons(expolygon));

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
