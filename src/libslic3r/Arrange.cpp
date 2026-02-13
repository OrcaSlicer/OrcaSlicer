#include "Arrange.hpp"
#include "Print.hpp"
#include "BoundingBox.hpp"
#include "libslic3r.h"

#include <libnest2d/backends/libslic3r/geometries.hpp>
#include <libnest2d/optimizers/nlopt/subplex.hpp>
#include <libnest2d/placers/nfpplacer.hpp>
#include <libnest2d/selections/firstfit.hpp>
#include <libnest2d/utils/rotcalipers.hpp>

#include <numeric>
#include <ClipperUtils.hpp>

#include <boost/geometry/index/rtree.hpp>

#if defined(_MSC_VER) && defined(__clang__)
#define BOOST_NO_CXX17_HDR_STRING_VIEW
#endif

#include <boost/log/trivial.hpp>
#include <boost/multiprecision/integer.hpp>
#include <boost/rational.hpp>

namespace libnest2d {
#if !defined(_MSC_VER) && defined(__SIZEOF_INT128__) && !defined(__APPLE__)
using LargeInt = __int128;
#else
using LargeInt = boost::multiprecision::int128_t;
template<> struct _NumTag<LargeInt>
{
    using Type = ScalarTag;
};
#endif

template<class T> struct _NumTag<boost::rational<T>>
{
    using Type = RationalTag;
};

namespace nfp {

template<class S> struct NfpImpl<S, NfpLevel::CONVEX_ONLY>
{
    NfpResult<S> operator()(const S &sh, const S &other)
    {
        return nfpConvexOnly<S, boost::rational<LargeInt>>(sh, other);
    }
};

} // namespace nfp
} // namespace libnest2d

namespace Slic3r {

template<class Tout = double, class = FloatingOnly<Tout>, int...EigenArgs>
inline constexpr Eigen::Matrix<Tout, 2, EigenArgs...> unscaled(
    const Slic3r::ClipperLib::IntPoint &v) noexcept
{
    return Eigen::Matrix<Tout, 2, EigenArgs...>{unscaled<Tout>(v.x()),
                                                unscaled<Tout>(v.y())};
}

namespace arrangement {

using namespace libnest2d;

// Get the libnest2d types for clipper backend
using Item         = _Item<ExPolygon>;
using Box          = _Box<Point>;
using Circle       = _Circle<Point>;
using Segment      = _Segment<Point>;
using MultiPolygon = ExPolygons;

// Summon the spatial indexing facilities from boost
namespace bgi = boost::geometry::index;
using SpatElement = std::pair<Box, unsigned>;
using SpatIndex = bgi::rtree< SpatElement, bgi::rstar<16, 4> >;
using ItemGroup = std::vector<std::reference_wrapper<Item>>;

// A coefficient used in separating bigger items and smaller items.
const double BIG_ITEM_TRESHOLD = 0.02;
#define VITRIFY_TEMP_DIFF_THRSH 15  // bed temp can be higher than vitrify temp, but not higher than this thresh

void update_arrange_params(ArrangeParams& params, const DynamicPrintConfig* print_cfg, const ArrangePolygons& selected)
{
    double                             skirt_distance = get_real_skirt_dist(*print_cfg);
    // Note: skirt_distance is now defined between outermost brim and skirt, not the object and skirt.
    // So we can't do max but do adding instead.
    params.brim_skirt_distance = skirt_distance;
    params.bed_shrink_x += params.brim_skirt_distance;
    params.bed_shrink_y += params.brim_skirt_distance;
    // for sequential print, we need to inflate the bed because clearance_radius is so large
    if (params.is_seq_print) {
        params.bed_shrink_x -= params.clearance_radius / 2;
        params.bed_shrink_y -= params.clearance_radius / 2;
    }
}

void update_selected_items_inflation(ArrangePolygons& selected, const DynamicPrintConfig* print_cfg, ArrangeParams& params) {
    // do not inflate brim_width. Objects are allowed to have overlapped brim.
    Points      bedpts = get_shrink_bedpts(print_cfg, params);
    BoundingBox bedbb = Polygon(bedpts).bounding_box();
    // set obj distance for auto seq_print
    if (params.is_seq_print) {
        bool all_objects_are_short = std::all_of(selected.begin(), selected.end(), [&](ArrangePolygon& ap) { return ap.height < params.nozzle_height; });
        if (all_objects_are_short) {
            params.min_obj_distance = std::max(params.min_obj_distance, scaled(std::max(MAX_OUTER_NOZZLE_DIAMETER/2.f, params.object_skirt_offset*2)+0.001));
        }
        else
            params.min_obj_distance = std::max(params.min_obj_distance, scaled(params.clearance_radius + 0.001)); // +0.001mm to avoid clearance check fail due to rounding error
    }
    double brim_max = 0;
    bool plate_has_tree_support = false;
    std::for_each(selected.begin(), selected.end(), [&](ArrangePolygon& ap) {
        brim_max = std::max(brim_max, ap.brim_width);
        if (ap.has_tree_support) plate_has_tree_support = true; });
    std::for_each(selected.begin(), selected.end(), [&](ArrangePolygon& ap) {
        // 1. if user input a distance, use it
        // 2. if there is an object with tree support, all objects use the max tree branch radius (brim_max=branch diameter)
        // 3. otherwise, use each object's own brim width
        ap.inflation = params.min_obj_distance != 0 ? params.min_obj_distance / 2 :
            plate_has_tree_support ? scaled(brim_max / 2) : scaled(ap.brim_width);
        BoundingBox apbb = ap.poly.contour.bounding_box();
        auto        diffx = bedbb.size().x() - apbb.size().x() - 5;
        auto        diffy = bedbb.size().y() - apbb.size().y() - 5;
        if (diffx > 0 && diffy > 0) {
            auto min_diff = std::min(diffx, diffy);
            ap.inflation = std::min(min_diff / 2, ap.inflation);
        }
        });
}

void update_unselected_items_inflation(ArrangePolygons& unselected, const DynamicPrintConfig* print_cfg, const ArrangeParams& params)
{
    float exclusion_gap = 1.f;
    if (params.is_seq_print) {
        // bed_shrink_x is typically (-params.clearance_radius / 2+5) for seq_print
        exclusion_gap = std::max(exclusion_gap, params.clearance_radius / 2 + params.bed_shrink_x + 1.f);  // +1mm gap so the exclusion region is not too close
        // dont forget to move the excluded region
        for (auto& region : unselected) {
            if (region.is_virt_object) region.poly.translate(scaled(params.bed_shrink_x), scaled(params.bed_shrink_y));
        }
    }
    // For occulusion regions, inflation should be larger to prevent genrating brim on them.
    // However, extrusion cali regions are exceptional, since we can allow brim overlaps them.
    // 屏蔽区域只需要膨胀brim宽度，防止brim长过去；挤出标定区域不需要膨胀，brim可以长过去。
    // 以前我们认为还需要膨胀clearance_radius/2，这其实是不需要的，因为这些区域并不会真的摆放物体，
    // 其他物体的膨胀轮廓是可以跟它们重叠的。
    std::for_each(unselected.begin(), unselected.end(),
        [&](auto& ap) { ap.inflation = !ap.is_virt_object ? (params.min_obj_distance == 0 ? scaled(ap.brim_width) : params.min_obj_distance / 2)
        : (ap.is_extrusion_cali_object ? 0 : scale_(exclusion_gap)); });
}

void update_selected_items_axis_align(ArrangePolygons& selected, const DynamicPrintConfig* print_cfg, const ArrangeParams& params)
{
    // now only need to consider "Align to x axis"
    if (!params.align_to_y_axis)
        return;

    for (ArrangePolygon& ap : selected) {
        bool   validResult = false;
        double angle = 0.0;
        {
            const auto& pts = ap.transformed_poly().contour;
            int         lpt = pts.size();
            double      a00 = 0, a10 = 0, a01 = 0, a20 = 0, a11 = 0, a02 = 0, a30 = 0, a21 = 0, a12 = 0, a03 = 0;
            double      xi, yi, xi2, yi2, xi_1, yi_1, xi_12, yi_12, dxy, xii_1, yii_1;
            xi_1 = pts.back().x();
            yi_1 = pts.back().y();

            xi_12 = xi_1 * xi_1;
            yi_12 = yi_1 * yi_1;

            for (int i = 0; i < lpt; i++) {
                xi = pts[i].x();
                yi = pts[i].y();

                xi2 = xi * xi;
                yi2 = yi * yi;
                dxy = xi_1 * yi - xi * yi_1;
                xii_1 = xi_1 + xi;
                yii_1 = yi_1 + yi;

                a00 += dxy;
                a10 += dxy * xii_1;
                a01 += dxy * yii_1;
                a20 += dxy * (xi_1 * xii_1 + xi2);
                a11 += dxy * (xi_1 * (yii_1 + yi_1) + xi * (yii_1 + yi));
                a02 += dxy * (yi_1 * yii_1 + yi2);
                a30 += dxy * xii_1 * (xi_12 + xi2);
                a03 += dxy * yii_1 * (yi_12 + yi2);
                a21 += dxy * (xi_12 * (3 * yi_1 + yi) + 2 * xi * xi_1 * yii_1 + xi2 * (yi_1 + 3 * yi));
                a12 += dxy * (yi_12 * (3 * xi_1 + xi) + 2 * yi * yi_1 * xii_1 + yi2 * (xi_1 + 3 * xi));
                xi_1 = xi;
                yi_1 = yi;
                xi_12 = xi2;
                yi_12 = yi2;
            }

            if (std::abs(a00) > EPSILON) {
                double db1_2, db1_6, db1_12, db1_24, db1_20, db1_60;
                double m00, m10, m01, m20, m11, m02, m30, m21, m12, m03;
                if (a00 > 0) {
                    db1_2 = 0.5;
                    db1_6 = 0.16666666666666666666666666666667;
                    db1_12 = 0.083333333333333333333333333333333;
                    db1_24 = 0.041666666666666666666666666666667;
                    db1_20 = 0.05;
                    db1_60 = 0.016666666666666666666666666666667;
                }
                else {
                    db1_2 = -0.5;
                    db1_6 = -0.16666666666666666666666666666667;
                    db1_12 = -0.083333333333333333333333333333333;
                    db1_24 = -0.041666666666666666666666666666667;
                    db1_20 = -0.05;
                    db1_60 = -0.016666666666666666666666666666667;
                }
                m00 = a00 * db1_2;
                m10 = a10 * db1_6;
                m01 = a01 * db1_6;
                m20 = a20 * db1_12;
                m11 = a11 * db1_24;
                m02 = a02 * db1_12;
                m30 = a30 * db1_20;
                m21 = a21 * db1_60;
                m12 = a12 * db1_60;
                m03 = a03 * db1_20;

                double cx = m10 / m00;
                double cy = m01 / m00;

                double a = m20 / m00 - cx * cx;
                double b = m11 / m00 - cx * cy;
                double c = m02 / m00 - cy * cy;

                //if a and c are close, there is no dominant axis, then do not rotate
                // ratio is always no more than 1
                double ratio = std::abs(a) > std::abs(c) ? std::abs(c / a) :
                    std::abs(c) > 0 ? std::abs(a / c) : 0;
                if (ratio>0.66) {
                    validResult = false;
                }
                else {
                    angle = std::atan2(2 * b, (a - c)) / 2;
                    angle = PI / 2 - angle;
                    // if the angle is close to PI or -PI, it means the object is vertical, then do not rotate
                    if (std::abs(std::abs(angle) - PI) < 0.01)
                        angle = 0;
                    validResult = true;
                }
            }
        }
        if (validResult) { ap.rotation += angle; }
    }
}

//it will bed accurate after call update_params
Points get_shrink_bedpts(const DynamicPrintConfig* print_cfg, const ArrangeParams& params)
{
    Points bedpts = get_bed_shape(*print_cfg);
    // shrink bed by moving to center by dist
    auto shrinkFun = [](Points& bedpts, double dist, int direction) {
#define SGN(x) ((x) >= 0 ? 1 : -1)
        Point center = Polygon(bedpts).bounding_box().center();
        for (auto& pt : bedpts) pt[direction] += dist * SGN(center[direction] - pt[direction]);
    };
    shrinkFun(bedpts, scaled(params.bed_shrink_x), 0);
    shrinkFun(bedpts, scaled(params.bed_shrink_y), 1);
    return bedpts;
}

// Fill in the placer algorithm configuration with values carefully chosen for
// Slic3r.
template<class PConf>
void fill_config(PConf& pcfg, const ArrangeParams &params) {

        if (params.is_seq_print) {
            // Start placing the items from the center of the print bed
            pcfg.starting_point = PConf::Alignment::BOTTOM_LEFT;
        }
        else {
            // Start placing the items from the center of the print bed
            pcfg.starting_point = PConf::Alignment::TOP_RIGHT;
        }

    if (params.do_final_align) {
        // Align the arranged pile into the center of the bin
        pcfg.alignment = PConf::Alignment::CENTER;
    }else
        pcfg.alignment = PConf::Alignment::DONT_ALIGN;


    // Try 4 angles (45 degree step) and find the one with min cost
    if (params.allow_rotations)
        pcfg.rotations = {0., PI / 4., PI/2, 3. * PI / 4. };
    else
        pcfg.rotations = {0.};

    // The accuracy of optimization.
    // Goes from 0.0 to 1.0 and scales performance as well
    pcfg.accuracy = params.accuracy;

    // Allow parallel execution.
    pcfg.parallel = params.parallel;

    // BBS: excluded regions in BBS bed
    for (auto& poly : params.excluded_regions)
        process_arrangeable(poly, pcfg.m_excluded_regions);
    // BBS: nonprefered regions in BBS bed
    for (auto& poly : params.nonprefered_regions)
        process_arrangeable(poly, pcfg.m_nonprefered_regions);
    for (auto& itm : pcfg.m_excluded_regions) {
        itm.markAsFixedInBin(0);
        itm.inflate(scaled(-2. * EPSILON));
    }
}

// Apply penalty to object function result. This is used only when alignment
// after arrange is explicitly disabled (PConfig::Alignment::DONT_ALIGN)
// Also, this will only work well for Box shaped beds.
static double fixed_overfit(const std::tuple<double, Box>& result, const Box &binbb)
{
    double score = std::get<0>(result);
    Box pilebb  = std::get<1>(result);
    Box fullbb  = sl::boundingBox(pilebb, binbb);
    auto diff = double(fullbb.area()) - binbb.area();
    if(diff > 0) score += diff;

    return score;
}

// useful for arranging big circle objects
static double fixed_overfit_topright_sliding(const std::tuple<double, Box> &result, const Box &binbb, const std::vector<Box> &excluded_boxes)
{
    double score = std::get<0>(result);
    Box pilebb = std::get<1>(result);

    auto shift = binbb.maxCorner() - pilebb.maxCorner();
    shift.x() = std::max((coord_t)0, shift.x()); // do not allow left shift
    shift.y() = std::max((coord_t)0, shift.y()); // do not allow bottom shift
    pilebb.minCorner() += shift;
    pilebb.maxCorner() += shift;

    Box fullbb = sl::boundingBox(pilebb, binbb);
    auto diff = double(fullbb.area()) - binbb.area();
    if (diff > 0) score += diff;

    // excluded regions and nonprefered regions should not intersect the translated pilebb
    for (auto &bb : excluded_boxes) {
        auto area_ = pilebb.intersection(bb).area();
        if (area_ > 0) score += area_;
    }

    return score;
}

// A class encapsulating the libnest2d Nester class and extending it with other
// management and spatial index structures for acceleration.
template<class TBin>
class AutoArranger {
public:
    // Useful type shortcuts...
    using Placer = typename placers::_NofitPolyPlacer<ExPolygon, TBin>;
    using Selector = selections::_FirstFitSelection<ExPolygon>;
    using Packer   = _Nester<Placer, Selector>;
    using PConfig  = typename Packer::PlacementConfig;
    using Distance = TCoord<PointImpl>;
    std::vector<Item> m_excluded_items_in_each_plate;   // for V4 bed there are excluded regions at bottom left corner

protected:
    Packer    m_pck;
    PConfig   m_pconf; // Placement configuration
    TBin      m_bin;
    double    m_bin_area;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4267)
#endif
    SpatIndex m_rtree; // spatial index for the normal (bigger) objects
    SpatIndex m_smallsrtree;    // spatial index for only the smaller items
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    double    m_norm;           // A coefficient to scale distances
    MultiPolygon m_merged_pile; // The already merged pile (vector of items)
    Box          m_pilebb;      // The bounding box of the merged pile.
    ItemGroup m_remaining;      // Remaining items
    ItemGroup m_items;          // allready packed items
    std::vector<Box> m_excluded_and_extruCali_regions;  // excluded and extrusion calib regions
    size_t    m_item_count = 0; // Number of all items to be packed
    ArrangeParams params;

    template<class T> ArithmeticOnly<T, double> norm(T val)
    {
        return double(val) / m_norm;
    }

    // dist function for sequential print (starting_point=BOTTOM_LEFT) which is composed of
        // 1) Y distance of item corner to bed corner. Must be put above bed corner. (high weight)
        // 2) X distance of item corner to bed corner (low weight)
        // 3) item row occupancy (useful when rotation is enabled)
        // 4）需要允许往屏蔽区域的左边或下边去一点，不然很多物体可能认为摆不进去，实际上我们最后是可以做平移的
    double dist_for_BOTTOM_LEFT(Box ibb, const ClipperLib::IntPoint& origin_pack)
    {
        double dist_corner_y = ibb.minCorner().y() - origin_pack.y();
        double dist_corner_x = ibb.minCorner().x() - origin_pack.x();
        // occupy as few rows as possible if we have rotations
        double bindist       = double(ibb.maxCorner().y() - ibb.minCorner().y());
        if (dist_corner_x >= 0 && dist_corner_y >= 0)
            bindist += dist_corner_y + 1 * dist_corner_x;
        else {
            if (dist_corner_x < 0) bindist += 10 * (-dist_corner_x);
            if (dist_corner_y < 0) bindist += 10 * (-dist_corner_y);
            }
        bindist = norm(bindist);
        return bindist;
    }

    double dist_to_bin(const Box& ibb, const ClipperLib::IntPoint& origin_pack, typename Packer::PlacementConfig::Alignment starting_point_alignment)
    {
        double bindist = 0;
        if (starting_point_alignment == PConfig::Alignment::BOTTOM_LEFT)
            bindist = norm(pl::distance(ibb.minCorner(), origin_pack));
        else if (starting_point_alignment == PConfig::Alignment::TOP_RIGHT)
            bindist = norm(pl::distance(ibb.maxCorner(), origin_pack));
        else
            bindist = norm(pl::distance(ibb.center(), origin_pack));
        return bindist;
    }

    // This is "the" object function which is evaluated many times for each
    // vertex (decimated with the accuracy parameter) of each object.
    // Therefore it is upmost crucial for this function to be as efficient
    // as it possibly can be but at the same time, it has to provide
    // reasonable results.
    std::tuple<double /*score*/, Box /*farthest point from bin center*/>
    objfunc(const Item &item, const ClipperLib::IntPoint &origin_pack)
    {
        const double bin_area = m_bin_area;
        const SpatIndex& spatindex = m_rtree;
        const SpatIndex& smalls_spatindex = m_smallsrtree;

        // We will treat big items (compared to the print bed) differently
        auto isBig = [bin_area](double a) {
            return a/bin_area > BIG_ITEM_TRESHOLD ;
        };

        // Candidate item bounding box
        auto ibb = item.boundingBox();

        // Calculate the full bounding box of the pile with the candidate item
        auto fullbb = sl::boundingBox(m_pilebb, ibb);

        // The bounding box of the big items (they will accumulate in the center
        // of the pile
        Box bigbb;
        if(spatindex.empty()) bigbb = fullbb;
        else {
            auto boostbb = spatindex.bounds();
            boost::geometry::convert(boostbb, bigbb);
        }

        // Will hold the resulting score
        double score = 0;

        // Density is the pack density: how big is the arranged pile
        double density = 0;

        // Distinction of cases for the arrangement scene
        enum e_cases {
            // This branch is for big items in a mixed (big and small) scene
            // OR for all items in a small-only scene.
            BIG_ITEM,

            // This branch is for the last big item in a mixed scene
            LAST_BIG_ITEM,

            // For small items in a mixed scene.
            SMALL_ITEM
        } compute_case;

        bool bigitems = isBig(item.area()) || spatindex.empty();
        if(!params.is_seq_print && bigitems && !m_remaining.empty()) compute_case = BIG_ITEM;  // do not use so complicated logic for sequential printing
        else if (bigitems && m_remaining.empty()) compute_case = LAST_BIG_ITEM;
        else compute_case = SMALL_ITEM;

        switch (compute_case) {
        case BIG_ITEM: {
            const Point& minc = ibb.minCorner(); // bottom left corner
            const Point& maxc = ibb.maxCorner(); // top right corner

            // top left and bottom right corners
            Point top_left{getX(minc), getY(maxc)};
            Point bottom_right{getX(maxc), getY(minc)};

            // Now the distance of the gravity center will be calculated to the
            // five anchor points and the smallest will be chosen.
            std::array<double, 5> dists;
            auto cc = fullbb.center(); // The gravity center
            dists[0] = pl::distance(minc, cc);
            dists[1] = pl::distance(maxc, cc);
            dists[2] = pl::distance(ibb.center(), cc);
            dists[3] = pl::distance(top_left, cc);
            dists[4] = pl::distance(bottom_right, cc);

            // The smalles distance from the arranged pile center:
            double dist = norm(*(std::min_element(dists.begin(), dists.end())));
            if (m_pconf.starting_point == PConfig::Alignment::BOTTOM_LEFT) {
                double bindist = dist_for_BOTTOM_LEFT(ibb, origin_pack);
                score = 0.2 * dist + 0.8 * bindist;
            }
            else {
                double bindist = dist_to_bin(ibb, origin_pack, m_pconf.starting_point);
                dist = 0.8 * dist + 0.2 * bindist;


                // Prepare a variable for the alignment score.
                // This will indicate: how well is the candidate item
                // aligned with its neighbors. We will check the alignment
                // with all neighbors and return the score for the best
                // alignment. So it is enough for the candidate to be
                // aligned with only one item.
                auto alignment_score = 1.0;

                auto query = bgi::intersects(ibb);
                auto& index = isBig(item.area()) ? spatindex : smalls_spatindex;

                // Query the spatial index for the neighbors
                std::vector<SpatElement> result;
                result.reserve(index.size());

                index.query(query, std::back_inserter(result));

                // now get the score for the best alignment
                for (auto& e : result) {
                    auto idx = e.second;
                    Item& p = m_items[idx];
                    auto parea = p.area();
                    if (std::abs(1.0 - parea / item.area()) < 1e-6) {
                        auto bb = sl::boundingBox(p.boundingBox(), ibb);
                        auto bbarea = bb.area();
                        auto ascore = 1.0 - (item.area() + parea) / bbarea;

                        if (ascore < alignment_score) alignment_score = ascore;
                    }
                }

                density = std::sqrt(norm(fullbb.width()) * norm(fullbb.height()));
                double R = double(m_remaining.size()) / m_item_count;
                // alighment score is more important for rectangle items
                double alignment_weight = std::max(0.3, 0.6 * item.area() / ibb.area());

                // The final mix of the score is the balance between the
                // distance from the full pile center, the pack density and
                // the alignment with the neighbors
                if (result.empty())
                    score = 0.50 * dist + 0.50 * density;
                else
                    // Let the density matter more when fewer objects remain
                    score = (1 - 0.2 - alignment_weight) * dist + (1.0 - R) * 0.20 * density +
                    alignment_weight * alignment_score;
            }
            break;
        }
        case LAST_BIG_ITEM: {
            if (m_pconf.starting_point == PConfig::Alignment::BOTTOM_LEFT) {
                score = dist_for_BOTTOM_LEFT(ibb, origin_pack);
            }
            else {
                if (m_pilebb.defined)
                    score = 0.5 * norm(pl::distance(ibb.center(), m_pilebb.center()));
                else
                    score = 0.5 * norm(pl::distance(ibb.center(), origin_pack));
            }
            break;
        }
        case SMALL_ITEM: {
            // Here there are the small items that should be placed around the
            // already processed bigger items.
            // No need to play around with the anchor points, the center will be
            // just fine for small items
            if (m_pconf.starting_point == PConfig::Alignment::BOTTOM_LEFT)
                score = dist_for_BOTTOM_LEFT(ibb, origin_pack);
            else {
                // Align mainly around existing items
                score = 0.8 * norm(pl::distance(ibb.center(), bigbb.center()))+ 0.2*norm(pl::distance(ibb.center(), origin_pack));
                // Align to 135 degree line {calc distance to the line x+y-(xc+yc)=0}
                //auto ic = ibb.center(), bigbbc = origin_pack;// bigbb.center();
                //score = norm(std::abs(ic.x() + ic.y() - bigbbc.x() - bigbbc.y()));
            }

            break;
        }
        }


        if (params.is_seq_print) {
            double clearance_height_to_lid = params.clearance_height_to_lid;
            double clearance_height_to_rod = params.clearance_height_to_rod;
            bool hasRowHeightConflict = false;
            bool hasLidHeightConflict = false;
            auto iy1 = item.boundingBox().minCorner().y();
            auto iy2 = item.boundingBox().maxCorner().y();
            auto ix1 = item.boundingBox().minCorner().x();

            for (int i = 0; i < m_items.size(); i++) {
                Item& p = m_items[i];
                if (p.is_virt_object) continue;
                auto px1 = p.boundingBox().minCorner().x();
                auto py1 = p.boundingBox().minCorner().y();
                auto py2 = p.boundingBox().maxCorner().y();
                auto inter_min = std::max(iy1, py1); // min y of intersection
                auto inter_max = std::min(iy2, py2); // max y of intersection. length=max_y-min_y>0 means intersection exists
                // if this item is lower than existing ones, this item will be printed first, so should not exceed height_to_rod
                if (iy2 < py1) { hasRowHeightConflict |= (item.height > clearance_height_to_rod); }
                else if (inter_max - inter_min > 0) {
                    // if they inter, the one on the left will be printed first
                    double h = ix1 < px1 ? item.height : p.height;
                    hasRowHeightConflict |= (h > clearance_height_to_rod);
                }
                // only last item can be heigher than clearance_height_to_lid, so if the existing items are higher than clearance_height_to_lid, there is height conflict
                hasLidHeightConflict |= (p.height > clearance_height_to_lid);
            }

            double lambda3 = LARGE_COST_TO_REJECT*1.1;
            double lambda4 = LARGE_COST_TO_REJECT*1.2;
            for (int i = 0; i < m_items.size(); i++) {
                Item& p = m_items[i];
                if (p.is_virt_object) continue;
                //score += lambda3 * (item.bed_temp - p.vitrify_temp > VITRIFY_TEMP_DIFF_THRSH);
                if (!Print::is_filaments_compatible({item.filament_temp_type,p.filament_temp_type}))
                    score += lambda3;
            }
            //score += lambda3 * (item.bed_temp - item.vitrify_temp > VITRIFY_TEMP_DIFF_THRSH);
            score += lambda4 * hasRowHeightConflict + lambda4 * hasLidHeightConflict;
        }
        else {
            int valid_items_cnt = 0;
            double height_score = 0;
            for (int i = 0; i < m_items.size(); i++) {
                Item& p = m_items[i];
                if (!p.is_virt_object) {
                    valid_items_cnt++;
                    // 高度接近的件尽量摆到一起
                    height_score += (1- std::abs(item.height - p.height) / params.printable_height)
                        * norm(pl::distance(ibb.center(), p.boundingBox().center()));
                    //score += LARGE_COST_TO_REJECT * (item.bed_temp - p.bed_temp != 0);
                    if (!Print::is_filaments_compatible({ item.filament_temp_type,p.filament_temp_type })) {
                        score += LARGE_COST_TO_REJECT;
                        break;
                    }
                }
            }
            if (valid_items_cnt > 0)
                score += height_score / valid_items_cnt;
        }

        std::set<int> extruder_ids;
        for (int i = 0; i < m_items.size(); i++) {
            Item& p = m_items[i];
            if (p.is_virt_object) continue;
            extruder_ids.insert(p.extrude_ids.begin(),p.extrude_ids.end());
        }

        // add a large cost if not multi materials on same plate is not allowed
        if (!params.allow_multi_materials_on_same_plate) {
            // it's the first object, which can be multi-color
            bool first_object                 = extruder_ids.empty();
            // the two objects (previously packed items and the current item) are considered having same color if either one's colors are a subset of the other
            std::set<int> item_extruder_ids(item.extrude_ids.begin(), item.extrude_ids.end());
            bool same_color_with_previous_items = std::includes(item_extruder_ids.begin(), item_extruder_ids.end(), extruder_ids.begin(), extruder_ids.end())
                || std::includes(extruder_ids.begin(), extruder_ids.end(), item_extruder_ids.begin(), item_extruder_ids.end());
            if (!(first_object || same_color_with_previous_items)) score += LARGE_COST_TO_REJECT * 1.3;
        }
        // for layered printing, we want extruder change as few as possible
        // this has very weak effect, CAN NOT use a large weight
        int last_extruder_cnt = extruder_ids.size();
        extruder_ids.insert(item.extrude_ids.begin(), item.extrude_ids.end());
        int new_extruder_cnt= extruder_ids.size();
        if (!params.is_seq_print) {
            score += 1 * (new_extruder_cnt-last_extruder_cnt);
        }

        return std::make_tuple(score, fullbb);
    }

    std::function<double(const Item&, const ItemGroup&)> get_objfn();

public:
    AutoArranger(const TBin &                  bin,
                 const ArrangeParams           &params,
                 std::function<void(unsigned,std::string)> progressind,
                 std::function<bool(void)>     stopcond)
        : m_pck(bin, params.min_obj_distance)
        , m_bin(bin)
    {
        m_bin_area = abs(sl::area(bin));  // due to clockwise or anti-clockwise, the result of sl::area may be negative
        m_norm = std::sqrt(m_bin_area);
        fill_config(m_pconf, params);
        this->params = params;

        // if best object center is not bed center, specify starting point here
        if (std::abs(this->params.align_center.x() - 0.5) > 0.001 || std::abs(this->params.align_center.y() - 0.5) > 0.001) {
            auto binbb = sl::boundingBox(m_bin);
            m_pconf.best_object_pos = binbb.minCorner() + Point{ binbb.width() * this->params.align_center.x(), binbb.height() * this->params.align_center.y() };
            m_pconf.alignment = PConfig::Alignment::USER_DEFINED;
        }

        for (auto& region : m_pconf.m_excluded_regions) {
            Box  bb = region.boundingBox();
            m_excluded_and_extruCali_regions.emplace_back(bb);
        }
        for (auto& region : m_pconf.m_nonprefered_regions) {
            Box  bb = region.boundingBox();
            m_excluded_and_extruCali_regions.emplace_back(bb);
        }

        // Set up a callback that is called just before arranging starts
        // This functionality is provided by the Nester class (m_pack).
        m_pconf.before_packing =
        [this](const MultiPolygon& merged_pile,            // merged pile
               const ItemGroup& items,             // packed items
               const ItemGroup& remaining)         // future items to be packed
        {
            m_items = items;
            m_merged_pile = merged_pile;
            m_remaining = remaining;

            m_pilebb.defined = false;
            if (!merged_pile.empty())
            {
                m_pilebb = sl::boundingBox(merged_pile);
                m_pilebb.defined = true;
            }

            m_rtree.clear();
            m_smallsrtree.clear();

            // We will treat big items (compared to the print bed) differently
            auto isBig = [this](double a) {
                return a / m_bin_area > BIG_ITEM_TRESHOLD ;
            };

            for(unsigned idx = 0; idx < items.size(); ++idx) {
                Item& itm = items[idx];
                if (itm.is_virt_object) continue;
                if(isBig(itm.area())) m_rtree.insert({itm.boundingBox(), idx});
                m_smallsrtree.insert({itm.boundingBox(), idx});
            }
        };

        m_pconf.object_function = get_objfn();

        // preload fixed items (and excluded regions) on plate
        m_pconf.on_preload = [this](const ItemGroup &items, PConfig &cfg) {
            if (items.empty()) return;

            auto binbb = sl::boundingBox(m_bin);

            auto starting_point = cfg.starting_point == PConfig::Alignment::BOTTOM_LEFT ? binbb.minCorner() : binbb.center();
            // if we have wipe tower, items should be arranged around wipe tower
            for (Item itm : items) {
                if (itm.is_wipe_tower) {
                    starting_point = itm.boundingBox().center();
                    BOOST_LOG_TRIVIAL(debug) << "arrange we have wipe tower, change starting point to: " << starting_point;
                    break;
                }
            }

            cfg.object_function = [this, binbb, starting_point](const Item &item, const ItemGroup &packed_items) {
                return fixed_overfit(objfunc(item, starting_point), binbb);
            };
        };

        auto on_packed = params.on_packed;

        if (progressind || on_packed)
            m_pck.progressIndicator(
                [this, progressind, on_packed](unsigned num_finished) {
                    int last_bed = m_pck.lastPackedBinId();
                    if (last_bed >= 0) {
                        Item& last_packed = m_pck.lastResult()[last_bed].back();
                        ArrangePolygon ap;
                        ap.bed_idx = last_packed.binId();
                        ap.priority = last_packed.priority();
                        if (progressind) progressind(num_finished, last_packed.name);
                        if (on_packed)
                            on_packed(ap);
                        BOOST_LOG_TRIVIAL(debug) << "arrange " + last_packed.name + " succeed!"
                            << ", plate id=" << ap.bed_idx << ", pos=" << last_packed.translation()
                            << ", temp_type=" << last_packed.filament_temp_type;
                    }
                });

        m_pck.unfitIndicator([this](std::string name) {
            BOOST_LOG_TRIVIAL(debug) << "arrange progress: " + name;
                });

        if (stopcond) m_pck.stopCondition(stopcond);

        m_pconf.sortfunc= [&params](Item& i1, Item& i2) {
            int p1 = i1.priority(), p2 = i2.priority();
            if (p1 != p2)
                return p1 > p2;
            if (params.is_seq_print) {
                return i1.bed_temp != i2.bed_temp                ? (i1.bed_temp > i2.bed_temp) :
                       i1.height != i2.height                    ? (i1.height < i2.height) :
                       std::abs(i1.area() / i2.area() - 1) > 0.2 ? (i1.area() > i2.area()) :
                                                                   i1.extrude_ids.front() < i2.extrude_ids.front();
            }
            else {
                return i1.bed_temp != i2.bed_temp ? (i1.bed_temp > i2.bed_temp) :
                    (i1.extrude_ids != i2.extrude_ids ? (i1.extrude_ids.front() < i2.extrude_ids.front()) : (i1.area() > i2.area()));
            }
        };

        m_pck.configure(m_pconf);
    }

    template<class It> inline void operator()(It from, It to) {
        m_rtree.clear();
        m_item_count += size_t(to - from);
        m_pck.execute(from, to);
        m_item_count = 0;
    }

    PConfig& config() { return m_pconf; }
    const PConfig& config() const { return m_pconf; }

    inline void preload(std::vector<Item>& fixeditems) {
        for(unsigned idx = 0; idx < fixeditems.size(); ++idx) {
            Item& itm = fixeditems[idx];
            itm.markAsFixedInBin(itm.binId());
        }

        m_item_count += fixeditems.size();
    }
};

template<> std::function<double(const Item&, const ItemGroup&)> AutoArranger<Box>::get_objfn()
{
    auto origin_pack = m_pconf.starting_point == PConfig::Alignment::CENTER ? m_bin.center() :
        m_pconf.starting_point == PConfig::Alignment::TOP_RIGHT ? m_bin.maxCorner() : m_bin.minCorner();

    return [this, origin_pack](const Item &itm, const ItemGroup&) {
        auto result = objfunc(itm, origin_pack);

        double score = std::get<0>(result);
        auto& fullbb = std::get<1>(result);

        //if (m_pconf.starting_point == PConfig::Alignment::BOTTOM_LEFT)
        //{
        //    if (!sl::isInside(fullbb, m_bin))
        //        score += LARGE_COST_TO_REJECT;
        //}
        //else
        {
            double miss = Placer::overfit(fullbb, m_bin);
            miss = miss > 0 ? miss : 0;
            score += miss * miss;
            if (score > LARGE_COST_TO_REJECT)
                score = 1.5 * LARGE_COST_TO_REJECT;
        }

        return score;
    };
}

template<> std::function<double(const Item&, const ItemGroup&)> AutoArranger<Circle>::get_objfn()
{
    auto bb = sl::boundingBox(m_bin);
    auto origin_pack = m_pconf.starting_point == PConfig::Alignment::CENTER ? bb.center() : bb.minCorner();
    return [this, origin_pack](const Item &item, const ItemGroup&) {

        auto result = objfunc(item, origin_pack);

        double score = std::get<0>(result);

        auto isBig = [this](const Item& itm) {
            return itm.area() / m_bin_area > BIG_ITEM_TRESHOLD ;
        };

        if(isBig(item)) {
            auto mp = m_merged_pile;
            mp.push_back(item.transformedShape());
            auto chull = sl::convexHull(mp);
            double miss = Placer::overfit(chull, m_bin);
            if(miss < 0) miss = 0;
            score += miss*miss;
        }

        return score;
    };
}

// Specialization for a generalized polygon.
// Warning: this is much slower than with Box bed. Need further speedup.
template<>
std::function<double(const Item &, const ItemGroup&)> AutoArranger<ExPolygon>::get_objfn()
{
    auto bb = sl::boundingBox(m_bin);
    auto origin_pack = m_pconf.starting_point == PConfig::Alignment::CENTER ? bb.center() : bb.minCorner();
    return [this, origin_pack](const Item &itm, const ItemGroup&) {
        auto result = objfunc(itm, origin_pack);

        double score = std::get<0>(result);

        auto mp = m_merged_pile;
        mp.emplace_back(itm.transformedShape());
        auto chull = sl::convexHull(mp);
        if (m_pconf.starting_point == PConfig::Alignment::BOTTOM_LEFT)
        {
            if (!sl::isInside(chull, m_bin))
                score += LARGE_COST_TO_REJECT;
        }
        else
        {
            double miss = Placer::overfit(chull, m_bin);
            miss = miss > 0 ? miss : 0;
            score += miss * miss;
        }

        return score;
    };
}

template<class Bin> void remove_large_items(std::vector<Item> &items, Bin &&bin)
{
    auto it = items.begin();
    while (it != items.end())
    {
        //BBS: skip virtual object
        if (!it->is_virt_object && !sl::isInside(it->transformedShape(), bin))
            it = items.erase(it);
        else
            it++;
    }
}

template<class S> Radians min_area_boundingbox_rotation(const S &sh)
{
    try {
        return minAreaBoundingBox<S, TCompute<S>, boost::rational<LargeInt>>(sh)
            .angleToX();
    }
    catch (const std::exception& e) {
        // min_area_boundingbox_rotation may throw exception of dividing 0 if the object is already perfectly aligned to X
        BOOST_LOG_TRIVIAL(error) << "arranging min_area_boundingbox_rotation fails, msg=" << e.what();
        return 0.0;
    }
}

template<class S>
Radians fit_into_box_rotation(const S &sh, const _Box<TPoint<S>> &box)
{
    return fitIntoBoxRotation<S, TCompute<S>, boost::rational<LargeInt>>(sh, box);
}

template<class BinT> // Arrange for arbitrary bin type
void _arrange(
        std::vector<Item> &           shapes,
        std::vector<Item> &           excludes,
        const BinT &                  bin,
        const ArrangeParams           &params,
        std::function<void(unsigned,std::string)> progressfn,
        std::function<bool()>         stopfn)
{
    // Integer ceiling the min distance from the bed perimeters
    coord_t md = params.min_obj_distance;
    md = md / 2;

    auto corrected_bin = bin;
    //sl::offset(corrected_bin, md);
    ArrangeParams mod_params = params;
    mod_params.min_obj_distance = 0;  // items are already inflated

    AutoArranger<BinT> arranger{corrected_bin, mod_params, progressfn, stopfn};

    remove_large_items(excludes, corrected_bin);

    // If there is something on the plate
    if (!excludes.empty()) arranger.preload(excludes);

    std::vector<std::reference_wrapper<Item>> inp;
    inp.reserve(shapes.size() + excludes.size());
    for (auto &itm : shapes  ) inp.emplace_back(itm);
    for (auto &itm : excludes) inp.emplace_back(itm);

    // Use the minimum bounding box rotation as a starting point.
    // TODO: This only works for convex hull. If we ever switch to concave
    // polygon nesting, a convex hull needs to be calculated.
    if (params.allow_rotations) {
        for (auto &itm : shapes) {
            itm.rotation(min_area_boundingbox_rotation(itm.transformedShape()));

            // If the item is too big, try to find a rotation that makes it fit
            if constexpr (std::is_same_v<BinT, Box>) {
                auto bb = itm.boundingBox();
                if (bb.width() >= bin.width() || bb.height() >= bin.height())
                    itm.rotate(fit_into_box_rotation(itm.transformedShape(), bin));
            }
        }
    }

    arranger(inp.begin(), inp.end());
    for (Item &itm : inp) itm.inflation(0);
}

inline Box to_nestbin(const BoundingBox &bb) { return Box{{bb.min(X), bb.min(Y)}, {bb.max(X), bb.max(Y)}};}
inline Circle to_nestbin(const CircleBed &c) { return Circle({c.center()(0), c.center()(1)}, c.radius()); }
inline ExPolygon to_nestbin(const Polygon &p) { return ExPolygon{p}; }
inline Box to_nestbin(const InfiniteBed &bed) { return Box::infinite({bed.center.x(), bed.center.y()}); }

inline coord_t width(const BoundingBox& box) { return box.max.x() - box.min.x(); }
inline coord_t height(const BoundingBox& box) { return box.max.y() - box.min.y(); }
inline double area(const BoundingBox& box) { return double(width(box)) * height(box); }
inline double poly_area(const Points &pts) { return std::abs(Polygon::area(pts)); }
inline double distance_to(const Point& p1, const Point& p2)
{
    double dx = p2.x() - p1.x();
    double dy = p2.y() - p1.y();
    return std::sqrt(dx*dx + dy*dy);
}

static CircleBed to_circle(const Point &center, const Points& points) {
    std::vector<double> vertex_distances;
    double avg_dist = 0;

    for (auto pt : points)
    {
        double distance = distance_to(center, pt);
        vertex_distances.push_back(distance);
        avg_dist += distance;
    }

    avg_dist /= vertex_distances.size();

    CircleBed ret(center, avg_dist);
    for(auto el : vertex_distances)
    {
        if (std::abs(el - avg_dist) > 10 * SCALED_EPSILON) {
            ret = {};
            break;
        }
    }

    return ret;
}

// Create Item from Arrangeable
static void process_arrangeable(const ArrangePolygon &arrpoly,
                                std::vector<Item> &   outp)
{
    Polygon        p        = arrpoly.poly.contour;
    const Vec2crd &offs     = arrpoly.translation;
    double         rotation = arrpoly.rotation;

    if (p.is_counter_clockwise()) p.reverse();

    if (p.size() < 3)
        return;

    outp.emplace_back(std::move(p));
    Item& item = outp.back();
    item.rotation(rotation);
    item.translation({offs.x(), offs.y()});
    item.binId(arrpoly.bed_idx);
    item.priority(arrpoly.priority);
    item.itemId(arrpoly.itemid);
    item.extrude_ids = arrpoly.extrude_ids;
    item.height = arrpoly.height;
    item.name = arrpoly.name;
    //BBS: add virtual object logic
    item.is_virt_object = arrpoly.is_virt_object;
    item.is_wipe_tower = arrpoly.is_wipe_tower;
    item.bed_temp = arrpoly.first_bed_temp;
    item.print_temp = arrpoly.print_temp;
    item.vitrify_temp = arrpoly.vitrify_temp;
    item.inflation(arrpoly.inflation);
    item.filament_temp_type = arrpoly.filament_temp_type;
}

template<class Fn> auto call_with_bed(const Points &bed, Fn &&fn)
{
    if (bed.empty())
        return fn(InfiniteBed{});
    else if (bed.size() == 1)
        return fn(InfiniteBed{bed.front()});
    else {
        auto      bb    = BoundingBox(bed);
        CircleBed circ  = to_circle(bb.center(), bed);
        auto      parea = poly_area(bed);

        if ((1.0 - parea / area(bb)) < 1e-3)
            return fn(bb);
        else if (!std::isnan(circ.radius()))
            return fn(circ);
        else
            return fn(Polygon(bed));
    }
}

template<>
void arrange(ArrangePolygons &      items,
             const ArrangePolygons &excludes,
             const Points &         bed,
             const ArrangeParams &  params)
{
    call_with_bed(bed, [&](const auto &bin) {
        arrange(items, excludes, bin, params);
    });
}

template<class BedT>
void arrange(ArrangePolygons &      arrangables,
             const ArrangePolygons &excludes,
             const BedT &           bed,
             const ArrangeParams &  params)
{
    namespace clppr = Slic3r::ClipperLib;

    std::vector<Item> items, fixeditems;
    items.reserve(arrangables.size());

    for (ArrangePolygon &arrangeable : arrangables)
        process_arrangeable(arrangeable, items);

    for (const ArrangePolygon &fixed: excludes)
        process_arrangeable(fixed, fixeditems);

    for (Item &itm : fixeditems) itm.inflate(scaled(-2. * EPSILON));

    _arrange(items, fixeditems, to_nestbin(bed), params, params.progressind, params.stopcondition);

    for(size_t i = 0; i < items.size(); ++i) {
        Point tr = items[i].translation();
        arrangables[i].translation = {coord_t(tr.x()), coord_t(tr.y())};
        arrangables[i].rotation    = items[i].rotation();
        arrangables[i].bed_idx     = items[i].binId();
        arrangables[i].itemid      = items[i].itemId();  // arrange order is useful for sequential printing
    }
}

template void arrange(ArrangePolygons &items, const ArrangePolygons &excludes, const BoundingBox &bed, const ArrangeParams &params);
template void arrange(ArrangePolygons &items, const ArrangePolygons &excludes, const CircleBed &bed, const ArrangeParams &params);
template void arrange(ArrangePolygons &items, const ArrangePolygons &excludes, const Polygon &bed, const ArrangeParams &params);
template void arrange(ArrangePolygons &items, const ArrangePolygons &excludes, const InfiniteBed &bed, const ArrangeParams &params);

} // namespace arr
} // namespace Slic3r
