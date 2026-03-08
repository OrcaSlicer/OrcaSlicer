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
// 1. Start of wave (always 0.0; not needed if the pattern base starts here)
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
  std::vector<coordf_t> res;
  coordf_t perpOffset = abs(triWave(Zpos, gridSize) / 2.);
  coordf_t normalisedOffset = perpOffset / gridSize;
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
                              coordf_t baseLocation, coordf_t gridLength,
                              coordf_t baseLocationP, coordf_t gridLengthP,
                              coordf_t offsetBase, coordf_t perpDir, int print_dir, coordf_t linearOffset)
{
  Polyline line;
  coordf_t zCycle = fmod(Zpos + gridSize/2, gridSize * 2.) / (gridSize * 2.);
  int zFlipDirection = sgn(fmod(zCycle, 0.5) - 0.25);
  bool hitEnd = false;
  int endPi = -1;
  size_t pi = 0;
  line.points.push_back(Point(0., 0.));
  coordf_t gridMax = ceil(gridLength / (gridSize * 2.)) * gridSize * 2.;
  for (coordf_t cLoc = 0; cLoc < (gridMax + EPSILON); cLoc+= gridSize*2) {
    for(pi = 0; pi < critPoints.size(); pi++){
      coordf_t offset = troctWave(critPoints[pi], gridSize, Zpos);
      coordf_t offsetFlip = sgn(offset);
      coordf_t posFlip = floor(((pi + 1) % 4) / 2) * 2 - 1;
      coordf_t multiOffset = linearOffset * posFlip * zFlipDirection;
      coordf_t posLin = cLoc + critPoints[pi] + multiOffset * print_dir;
      coordf_t posPerp = offset * perpDir;
      line.points.push_back((print_dir == 1) ? Point(posPerp, posLin) : Point(posLin, posPerp));
    }
  }
  line.points.push_back((print_dir == 1) ? Point(0., gridMax) : Point(gridMax, 0.));
  return line;
}

// Add additional dense fill in line with the pattern direction to
// cover the top squares of the pattern
static Polylines addTops(coordf_t Zpos, coordf_t gridSize, coordf_t lengthX, coordf_t lengthY, coordf_t spacing,
                         size_t multiline_count, size_t topDistance)
{
  coordf_t zCycle = fmod(Zpos + gridSize/2, gridSize * 2.) / (gridSize * 2.);
  coordf_t zHalfCycle = fmod(zCycle, 0.5) * 2.;
  bool printVert = zCycle < 0.5;
  coordf_t offsetX = multiline_count;
  coordf_t offsetY = multiline_count;
  coordf_t perpOffset = abs(triWave(Zpos, gridSize) / 2.);
  coordf_t gridPoint = gridSize * (0. + perpOffset / gridSize);
  coordf_t topOffset = gridSize / 2.0 - abs(troctWave(gridPoint, gridSize, Zpos));
  coordf_t multilineAdjust = (sqrt(2) - 1.0) / 2.;
  Polylines lines;
  size_t pointCount = 0;
  coordf_t gridStartL = gridSize * 0.5 - topOffset;
  coordf_t gridEndL = gridSize * 0.5 + topOffset;
  if((topDistance == 0) && (multiline_count == 1)){
    // extend out a little bit on the first layer to help fuse the cover
    gridStartL -= spacing;
    gridEndL += spacing;
  } else if(multiline_count > 1) {
    // match start point to the corner edge
    gridStartL -= spacing * multiline_count * multilineAdjust;
    gridEndL += spacing * multiline_count * multilineAdjust;
  }
  // top cover extents perpendicular to the direction of travel
  coordf_t gridStartP = gridSize * 0.5 - topOffset + spacing * multiline_count / 2. + spacing / 2.;
  coordf_t gridEndP = gridSize * 0.5 + topOffset - spacing * multiline_count / 2. - spacing / 2.;
  coordf_t x, y;
  int xm, ym;
  // if the print direction needs to be rotated, then swap the extents
  if((topDistance % 2) == 0){
    std::swap(gridStartL, gridStartP);
    std::swap(gridEndL, gridEndP);
  }
  // adjust spacing so that it starts and ends on exactly the right place
  coordf_t region_count = floor((gridEndP - gridStartP) / spacing);
  spacing = (gridEndP - gridStartP) / region_count;
  for (x = offsetX, xm = 0; x <= (lengthX); x+= gridSize, xm = xm ^ 1) {
    for (y = offsetY, ym = 0; y <= (lengthY); y += gridSize, ym = ym ^ 1) {
      if(((xm ^ ym) == 1) == printVert){
        continue;
      }
      // // For debugging: remove 0,0 -> 1,1 top to help understand orientation
      // if((x <= (gridSize + EPSILON)) && (y <= (gridSize + EPSILON)) && ((y - x) < EPSILON)){
      //   continue;
      // }
      Polyline newPoints;
      int dirMod = xm ^ ym;
      if(printVert == (topDistance % 2)){
        if(y < (lengthY - spacing * multiline_count * 1.5)){
          coordf_t endPMod = std::min(lengthX - (multiline_count * (spacing + 1) / 2.), x + gridEndP) - x;
          coordf_t endLMod = std::min(lengthY - (multiline_count * (spacing + 1) / 2.), y + gridEndL) - y;
          for(coordf_t xi = gridStartP; xi < (endPMod + EPSILON); xi += spacing, dirMod = dirMod ^ 1){
            newPoints.points.push_back((dirMod == 0) ? Point(x + xi, y + gridStartL) : Point(x + xi, y + endLMod));
            newPoints.points.push_back((dirMod == 0) ? Point(x + xi, y + endLMod) : Point(x + xi, y + gridStartL));
            pointCount += 2;
          }
        }
      } else {
        if(x < (lengthX - spacing * multiline_count * 1.5)){
          coordf_t endPMod = std::min(lengthY - (multiline_count * (spacing + 1) / 2.), y + gridEndP) - y;
          coordf_t endLMod = std::min(lengthX - (multiline_count * (spacing + 1) / 2.), x + gridEndL) - x;
          for(coordf_t yi = gridStartP; yi < (endPMod + EPSILON); yi += spacing, dirMod = dirMod ^ 1){
            newPoints.points.push_back((dirMod == 0) ? Point(x + gridStartL, y + yi) : Point(x + endLMod, y + yi));
            newPoints.points.push_back((dirMod == 0) ? Point(x + endLMod, y + yi) : Point(x + gridStartL, y + yi));
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
  static Polylines makeZigZag(coordf_t Zpos, coordf_t gridSize, coordf_t lengthX, coordf_t lengthY,
                              coordf_t spacing, size_t multiline_count)
{
  Polylines lines;
  coordf_t offsetX = multiline_count;
  coordf_t offsetY = multiline_count;
  std::vector<coordf_t> critPoints = getCriticalPoints(Zpos, gridSize);
  coordf_t zCycle = fmod(Zpos + gridSize/2, gridSize * 2.) / (gridSize * 2.);
  bool printVert = zCycle < 0.5;
  if (printVert) {
    BoundingBox extents;
    int perpDir = -1;
    int perpDirPattern = -1;
    coordf_t lastX = offsetX;
    for (coordf_t x = offsetX; x <= (lengthX + EPSILON); lastX = x, x+= gridSize, perpDirPattern *= -1) {
      coordf_t xAdj = - spacing * (multiline_count - 1) / 2.0;
      for (size_t mci = 0; mci < multiline_count; mci++, xAdj += spacing){
        Polyline newPoints;
        newPoints = patternPoints(Zpos, gridSize, critPoints, 0, lengthY, 0, lengthX, x,
                                  perpDirPattern, 1, (sqrt(2) - 1) * xAdj * perpDirPattern);
        if (perpDir == 1)
          std::reverse(newPoints.points.begin(), newPoints.points.end());
        newPoints.translate(Point(x + xAdj, offsetY));
        extents.merge(newPoints.points);
        lines.push_back(newPoints);
        perpDir *= -1;
      }
    }
    // add ending straight lines, if necessary
    if(extents.size()(0) < (lengthX - spacing * multiline_count)){
      Polyline endPoints;
      endPoints.points.push_back(Point(lengthX, lengthY));
      for(int pi = 0; pi < multiline_count; pi++){
        endPoints.points.push_back(Point(lengthX - pi * spacing, (pi % 2 == 0) ? 0.0 : lengthY));
        if(pi < (multiline_count - 1)){ // connection for next line
          endPoints.points.push_back(Point(lengthX - (pi + 1) * spacing, (pi % 2 == 0) ? 0.0 : lengthY));
        }
      }
      lines.push_back(endPoints);
    }
  } else {
    BoundingBox extents;
    int perpDir = -1;
    int perpDirPattern = -1;
    coordf_t lastY = 0;
    for (coordf_t y = offsetY; y <= (lengthY + EPSILON); lastY = y, y+= gridSize, perpDirPattern *= -1) {
      coordf_t yAdj = - spacing * (multiline_count - 1) / 2.0;
      for (size_t mci = 0; mci < multiline_count; mci++, yAdj += spacing){
        Polyline newPoints;
        newPoints = patternPoints(Zpos, gridSize, critPoints, 0, lengthX, 0, lengthY, y,
                                  perpDirPattern, -1, (sqrt(2) - 1) * yAdj * perpDirPattern);
        if (perpDir == -1)
          std::reverse(newPoints.points.begin(), newPoints.points.end());
        newPoints.translate(Point(offsetX, y + yAdj));
        extents.merge(newPoints.points);
        lines.push_back(newPoints);
        perpDir *= -1;
      }
    }
    // add ending straight lines, if necessary
    if(extents.size()(1) < (lengthY - spacing * multiline_count)){
      Polyline endPoints;
      endPoints.points.push_back(Point(lengthX, lengthY));
      for(int pi = 0; pi < multiline_count; pi++){
        endPoints.points.push_back(Point((pi % 2 == 0) ? 0.0 : lengthX, lengthY - pi * spacing));
        if(pi < (multiline_count - 1)){ // connection for next line
          endPoints.points.push_back(Point((pi % 2 == 0) ? 0.0 : lengthX, lengthY - (pi + 1) * spacing));
        }
      }
      lines.push_back(endPoints);
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
                          coordf_t lengthX, coordf_t lengthY,
                          bool completeTops, coordf_t spacing, size_t multiline_count)
{
  coordf_t zCycle = fmod(z + gridSize/2, gridSize * 2.) / (gridSize * 2.);
  bool printVert = zCycle < 0.5;
  coordf_t zCycleLast = fmod(zLast + gridSize/2, gridSize * 2.) / (gridSize * 2.);
  bool printVertLast = zCycleLast < 0.5;
  Polylines result;
  Polylines polyZag = makeZigZag(z, gridSize, lengthX, lengthY, spacing, multiline_count);
  result.insert(result.end(), polyZag.begin(), polyZag.end());
  if(completeTops && (printVert != printVertLast)){
    coordf_t layer_height = (z - zLast) / multiline_count;
    size_t top_distance = 0;
    for(coordf_t zCheck = z; zCheck >= (zLast + EPSILON); zCheck -= layer_height, top_distance++){
      coordf_t zCheckCycle = fmod(zCheck + gridSize/2, gridSize * 2.) / (gridSize * 2.);
      if(printVert != (zCheckCycle < 0.5)){
        break;
      }
    }
    // only print tops for the first <multiline_count> layers in each cycle
    Polylines polytops = addTops(z, gridSize, lengthX, lengthY, spacing, multiline_count, top_distance);
    result.insert(result.end(), polytops.begin(), polytops.end());
  }
  return result;
}

// FillParams has the following useful information:
// density <0 .. 1>  [proportion of space to fill]
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

    // Reduce the bounding box inwards to avoid artifacts at the edges from clipping
    //coord_t reduceSize = 1. * scale_(this->spacing);
    //bb.offset(-reduceSize);

    // Note: with equally-scaled X/Y/Z, the pattern will create a vertically-stretched
    // truncated octahedron; so Z is pre-adjusted first by scaling by sqrt(2)
    coordf_t zScale = sqrt(2);

    // Density adjustment to account for the additional distance of
    // octagram curves. [This only strictly applies for a rectangular
    // area where the total Z travel distance is a multiple of the
    // spacing]
    // = 4 * integrate(func=4*x(sqrt(2) - 1) + 1, from=0, to=0.25)
    // = (sqrt(2) + 1) / 2 [... I think]
    // make a first guess at the preferred grid Size (in unscaled units)
    coordf_t gridSize = (scale_(this->spacing) *
                         ((zScale + 1.) / 2.) * params.multiline  / params.density);
    coordf_t layerHeight = scale_(params.layer_height);
    coordf_t layersPerModule = floor((gridSize * 2) / (zScale * layerHeight) + 0.05);
    // If a density over 42% is requested, set an exact layer pattern
    if((params.density > 0.42) || (layersPerModule < 2)){
      layersPerModule = 2;
      // re-adjust the grid size for a partial octahedral path
      // (scale of 1.1 guessed based on modeling)
      gridSize = (scale_(this->spacing) * 1.1 * params.multiline  / params.density);
      // re-adjust zScale to make layering consistent
      zScale = (gridSize * 2) / (layersPerModule * layerHeight);
    }

    // align bounding box to a multiple of the octahedron grid so that
    // layers with different starting points have matching origins
    bb.merge(align_to_grid(bb.min, Point(gridSize * 2., gridSize * 2.)));

    // Z adjustment to start at the widest point for the lowest layer
    coordf_t startOffset = gridSize / 2. + scale_(params.layer_height / 2.);
    // generate pattern
    Polylines polylines =
      makeGrid(
	       scale_(this->z) * zScale + startOffset,
	       scale_(this->z - (params.layer_height * params.multiline)) * zScale + startOffset,
	       gridSize, bb.size()(0), bb.size()(1),
	       params.infill_complete_top,
               scale_(this->spacing),
               params.multiline);

    // move pattern in place
    for (Polyline &pl : polylines){
      pl.translate(bb.min);
      //pl.simplify(5 * spacing); // simplify shouldn't be necessary for this pattern
    }

    // Note: multiline fill adjustment is carried out in this code,
    // rather than using the multiline_fill function

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
