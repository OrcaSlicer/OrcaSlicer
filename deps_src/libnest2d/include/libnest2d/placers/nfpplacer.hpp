#ifndef NOFITPOLY_HPP
#define NOFITPOLY_HPP

#include <cassert>

// For parallel for
#include <functional>
#include <iterator>
#include <future>
#include <atomic>

#ifndef NDEBUG
#include <iostream>
#endif
#include <libnest2d/geometry_traits_nfp.hpp>
#include <libnest2d/optimizer.hpp>

#include "placer_boilerplate.hpp"

// temporary
//#include "../tools/svgtools.hpp"
//#include <iomanip> // setprecision

#include <libnest2d/parallel.hpp>

namespace libnest2d {
namespace placers {

template<class RawShape>
struct NfpPConfig {

    using ItemGroup = _ItemGroup<RawShape>;

    enum class Alignment {
        CENTER,
        BOTTOM_LEFT,
        BOTTOM_RIGHT,
        TOP_LEFT,
        TOP_RIGHT,
        DONT_ALIGN,     //!> Warning: parts may end up outside the bin with the
                        //! default object function.
        USER_DEFINED
    };

    /// Which angles to try out for better results.
    std::vector<Radians> rotations;

    /// Where to align the resulting packed pile.
    Alignment alignment;

    /// Where to start putting objects in the bin.
    Alignment starting_point;

    TPoint<RawShape> best_object_pos;

    /**
     * @brief A function object representing the fitting function in the
     * placement optimization process. (Optional)
     *
     * This is the most versatile tool to configure the placer. The fitting
     * function is evaluated many times when a new item is being placed into the
     * bin. The output should be a rated score of the new item's position.
     *
     * This is not a mandatory option as there is a default fitting function
     * that will optimize for the best pack efficiency. With a custom fitting
     * function you can e.g. influence the shape of the arranged pile.
     *
     * \param item The only parameter is the candidate item which has info
     * about its current position. Your job is to rate this position compared to
     * the already packed items.
     *
     */
    std::function<double(const _Item<RawShape>&, const ItemGroup&)> object_function;

    /**
     * @brief The quality of search for an optimal placement.
     * This is a compromise slider between quality and speed. Zero is the
     * fast and poor solution while 1.0 is the slowest but most accurate.
     */
    float accuracy = 0.65f;

    /**
     * @brief If you want to see items inside other item's holes, you have to
     * turn this switch on.
     *
     * This will only work if a suitable nfp implementation is provided.
     * The library has no such implementation right now.
     */
    bool explore_holes = false;

    /**
     * @brief If true, use all CPUs available. Run on a single core otherwise.
     */
    bool parallel = true;

    /**
     * @brief before_packing Callback that is called just before a search for
     * a new item's position is started. You can use this to create various
     * cache structures and update them between subsequent packings.
     *
     * \param merged pile A polygon that is the union of all items in the bin.
     *
     * \param pile The items parameter is a container with all the placed
     * polygons excluding the current candidate. You can for instance check the
     * alignment with the candidate item or do anything else.
     *
     * \param remaining A container with the remaining items waiting to be
     * placed. You can use some features about the remaining items to alter to
     * score of the current placement. If you know that you have to leave place
     * for other items as well, that might influence your decision about where
     * the current candidate should be placed. E.g. imagine three big circles
     * which you want to place into a box: you might place them in a triangle
     * shape which has the maximum pack density. But if there is a 4th big
     * circle than you won't be able to pack it. If you knew apriori that
     * there four circles are to be placed, you would have placed the first 3
     * into an L shape. This parameter can be used to make these kind of
     * decisions (for you or a more intelligent AI).
     */
    std::function<void(const nfp::Shapes<RawShape>&, // merged pile
                       const ItemGroup&,             // packed items
                       const ItemGroup&              // remaining items
                       )> before_packing;

    std::function<void(const ItemGroup &, NfpPConfig &config)> on_preload;

    //BBS: sort function for selector
    std::function<bool(_Item<RawShape>& i1, _Item<RawShape>& i2)> sortfunc;
    //BBS: excluded region for V4 bed
    std::vector<_Item<RawShape> > m_excluded_regions;
    _ItemGroup<RawShape> m_excluded_items;
    std::vector < _Item<RawShape> > m_nonprefered_regions;

    NfpPConfig(): rotations({0.0, Pi/2.0, Pi, 3*Pi/2}),
        alignment(Alignment::CENTER), starting_point(Alignment::CENTER) {}
};

/**
 * A class for getting a point on the circumference of the polygon (in log time)
 *
 * This is a transformation of the provided polygon to be able to pinpoint
 * locations on the circumference. The optimizer will pass a floating point
 * value e.g. within <0,1> and we have to transform this value quickly into a
 * coordinate on the circumference. By definition 0 should yield the first
 * vertex and 1.0 would be the last (which should coincide with first).
 *
 * We also have to make this work for the holes of the captured polygon.
 */
template<class RawShape> class EdgeCache {
    using Vertex = TPoint<RawShape>;
    using Coord = TCoord<Vertex>;
    using Edge = _Segment<Vertex>;

    struct ContourCache {
        mutable std::vector<double> corners;
        std::vector<Edge> emap;
        std::vector<double> distances;
        double full_distance = 0;
    } contour_;

    std::vector<ContourCache> holes_;

    double accuracy_ = 1.0;
    
    static double length(const Edge &e) 
    { 
        return std::sqrt(e.template sqlength<double>());
    }

    void createCache(const RawShape& sh) {
        {   // For the contour
            auto first = shapelike::cbegin(sh);
            auto next = std::next(first);
            auto endit = shapelike::cend(sh);

            contour_.distances.reserve(shapelike::contourVertexCount(sh));

            while(next != endit) {
                contour_.emap.emplace_back(*(first++), *(next++));
                contour_.full_distance += length(contour_.emap.back());
                contour_.distances.emplace_back(contour_.full_distance);
            }
        }

        for(auto& h : shapelike::holes(sh)) { // For the holes
            auto first = h.begin();
            auto next = std::next(first);
            auto endit = h.end();

            ContourCache hc;
            hc.distances.reserve(endit - first);

            while(next != endit) {
                hc.emap.emplace_back(*(first++), *(next++));
                hc.full_distance += length(hc.emap.back());
                hc.distances.emplace_back(hc.full_distance);
            }

            holes_.emplace_back(std::move(hc));
        }
    }

    size_t stride(const size_t N) const {
        using std::round;
        using std::pow;

        return static_cast<Coord>(
                    round(N/pow(N, pow(accuracy_, 1.0/3.0)))
                );
    }

    void fetchCorners() const {
        if(!contour_.corners.empty()) return;

        const auto N = contour_.distances.size();
        const auto S = stride(N);

        contour_.corners.reserve(N / S + 1);
        contour_.corners.emplace_back(0.0);
        auto N_1 = N-1;
        contour_.corners.emplace_back(0.0);
        for(size_t i = 0; i < N_1; i += S) {
            contour_.corners.emplace_back(
                    contour_.distances.at(i) / contour_.full_distance);
        }
    }

    void fetchHoleCorners(unsigned hidx) const {
        auto& hc = holes_[hidx];
        if(!hc.corners.empty()) return;

        const auto N = hc.distances.size();
        auto N_1 = N-1;
        const auto S = stride(N);
        hc.corners.reserve(N / S + 1);
        hc.corners.emplace_back(0.0);
        for(size_t i = 0; i < N_1; i += S) {
            hc.corners.emplace_back(
                    hc.distances.at(i) / hc.full_distance);
        }
    }

    inline Vertex coords(const ContourCache& cache, double distance) const {
        assert(distance >= .0 && distance <= 1.0);
        if (cache.distances.empty() || cache.emap.empty()) return Vertex{};
        if (distance > 1.0) distance = std::fmod(distance, 1.0);

        // distance is from 0.0 to 1.0, we scale it up to the full length of
        // the circumference
        double d = distance*cache.full_distance;

        auto& distances = cache.distances;

        // Magic: we find the right edge in log time
        auto it = std::lower_bound(distances.begin(), distances.end(), d);
        auto idx = it - distances.begin();      // get the index of the edge
        auto edge = cache.emap[idx];         // extrac the edge

        // Get the remaining distance on the target edge
        auto ed = d - (idx > 0 ? *std::prev(it) : 0 );
        auto angle = edge.angleToXaxis();
        Vertex ret = edge.first();

        // Get the point on the edge which lies in ed distance from the start
        ret += Vertex(static_cast<Coord>(std::round(ed*std::cos(angle))),
                      static_cast<Coord>(std::round(ed*std::sin(angle))));

        return ret;
    }

public:

    using iterator = std::vector<double>::iterator;
    using const_iterator = std::vector<double>::const_iterator;

    inline EdgeCache() = default;

    inline EdgeCache(const _Item<RawShape>& item)
    {
        createCache(item.transformedShape());
    }

    inline EdgeCache(const RawShape& sh)
    {
        createCache(sh);
    }

    /// Resolution of returned corners. The stride is derived from this value.
    void accuracy(double a /* within <0.0, 1.0>*/) { accuracy_ = a; }

    /**
     * @brief Get a point on the circumference of a polygon.
     * @param distance A relative distance from the starting point to the end.
     * Can be from 0.0 to 1.0 where 0.0 is the starting point and 1.0 is the
     * closing point (which should be eqvivalent with the starting point with
     * closed polygons).
     * @return Returns the coordinates of the point lying on the polygon
     * circumference.
     */
    inline Vertex coords(double distance) const {
        return coords(contour_, distance);
    }

    inline Vertex coords(unsigned hidx, double distance) const {
        assert(hidx < holes_.size());
        return coords(holes_[hidx], distance);
    }

    inline double circumference() const BP2D_NOEXCEPT {
        return contour_.full_distance;
    }

    inline double circumference(unsigned hidx) const BP2D_NOEXCEPT {
        return holes_[hidx].full_distance;
    }

    /// Get the normalized distance values for each vertex
    inline const std::vector<double>& corners() const BP2D_NOEXCEPT {
        fetchCorners();
        return contour_.corners;
    }

    /// corners for a specific hole
    inline const std::vector<double>&
    corners(unsigned holeidx) const BP2D_NOEXCEPT {
        fetchHoleCorners(holeidx);
        return holes_[holeidx].corners;
    }

    /// The number of holes in the abstracted polygon
    inline size_t holeCount() const BP2D_NOEXCEPT { return holes_.size(); }

};

template<nfp::NfpLevel lvl>
struct Lvl { static const nfp::NfpLevel value = lvl; };

template<class RawShape>
inline void correctNfpPosition(nfp::NfpResult<RawShape>& nfp,
                               const _Item<RawShape>& stationary,
                               const _Item<RawShape>& orbiter)
{
    // The provided nfp is somewhere in the dark. We need to get it
    // to the right position around the stationary shape.
    // This is done by choosing the leftmost lowest vertex of the
    // orbiting polygon to be touched with the rightmost upper
    // vertex of the stationary polygon. In this configuration, the
    // reference vertex of the orbiting polygon (which can be dragged around
    // the nfp) will be its rightmost upper vertex that coincides with the
    // rightmost upper vertex of the nfp. No proof provided other than Jonas
    // Lindmark's reasoning about the reference vertex of nfp in his thesis
    // ("No fit polygon problem" - section 2.1.9)

    auto touch_sh = stationary.rightmostTopVertex();
    auto touch_other = orbiter.leftmostBottomVertex();
    auto dtouch = touch_sh - touch_other;
    auto top_other = orbiter.rightmostTopVertex() + dtouch;
    auto dnfp = top_other - nfp.second; // nfp.second is the nfp reference point
    shapelike::translate(nfp.first, dnfp);
}

template<class RawShape>
inline void correctNfpPosition(nfp::NfpResult<RawShape>& nfp,
                               const RawShape& stationary,
                               const _Item<RawShape>& orbiter)
{
    auto touch_sh = nfp::rightmostUpVertex(stationary);
    auto touch_other = orbiter.leftmostBottomVertex();
    auto dtouch = touch_sh - touch_other;
    auto top_other = orbiter.rightmostTopVertex() + dtouch;
    auto dnfp = top_other - nfp.second;
    shapelike::translate(nfp.first, dnfp);
}

template<class RawShape, class Circle = _Circle<TPoint<RawShape>> >
Circle minimizeCircle(const RawShape& sh) {
    using Point = TPoint<RawShape>;
    using Coord = TCoord<Point>;

    auto& ctr = sl::contour(sh);
    if(ctr.empty()) return {{0, 0}, 0};

    auto bb = sl::boundingBox(sh);
    auto capprx = bb.center();
    auto rapprx = pl::distance(bb.minCorner(), bb.maxCorner());


    opt::StopCriteria stopcr;
    stopcr.max_iterations = 30;
    stopcr.relative_score_difference = 1e-3;
    opt::TOptimizer<opt::Method::L_SUBPLEX> solver(stopcr);

    std::vector<double> dists(ctr.size(), 0);

    auto result = solver.optimize_min(
        [capprx, rapprx, &ctr, &dists](double xf, double yf) {
            auto xt = Coord( std::round(getX(capprx) + rapprx*xf) );
            auto yt = Coord( std::round(getY(capprx) + rapprx*yf) );

            Point centr(xt, yt);

            unsigned i = 0;
            for(auto v : ctr) {
                dists[i++] = pl::distance(v, centr);
            }

            auto mit = std::max_element(dists.begin(), dists.end());

            assert(mit != dists.end());

            return *mit;
        },
        opt::initvals(0.0, 0.0),
        opt::bound(-1.0, 1.0), opt::bound(-1.0, 1.0)
    );

    double oxf = std::get<0>(result.optimum);
    double oyf = std::get<1>(result.optimum);
    auto xt = Coord( std::round(getX(capprx) + rapprx*oxf) );
    auto yt = Coord( std::round(getY(capprx) + rapprx*oyf) );

    Point cc(xt, yt);
    auto r = result.score;

    return {cc, r};
}

template<class RawShape>
_Circle<TPoint<RawShape>> boundingCircle(const RawShape& sh) {
    return minimizeCircle(sh);
}

template<class RawShape, class TBin = _Box<TPoint<RawShape>>>
class _NofitPolyPlacer: public PlacerBoilerplate<_NofitPolyPlacer<RawShape, TBin>,
        RawShape, TBin, NfpPConfig<RawShape>> {

    using Base = PlacerBoilerplate<_NofitPolyPlacer<RawShape, TBin>,
    RawShape, TBin, NfpPConfig<RawShape>>;

    DECLARE_PLACER(Base)

    using Box = _Box<TPoint<RawShape>>;

    using MaxNfpLevel = nfp::MaxNfpLevel<RawShape>;

public:

    using Pile = nfp::Shapes<RawShape>;

private:

    // Norming factor for the optimization function
    const double norm_;
    double score_ = 0;  // BBS: total costs of putting all the items
    int plate_id = 0;   // BBS
    Pile merged_pile_;

public:

    inline explicit _NofitPolyPlacer(const BinType& bin):
        Base(bin),
        norm_(std::sqrt(abs(sl::area(bin))))
    {
        // In order to not have items out of bin, it will be shrinked by an
        // very little empiric offset value.
        // sl::offset(bin_, 1e-5 * norm_);
    }

    _NofitPolyPlacer(const _NofitPolyPlacer&) = default;
    _NofitPolyPlacer& operator=(const _NofitPolyPlacer&) = default;

#ifndef BP2D_COMPILER_MSVC12 // MSVC2013 does not support default move ctors
    _NofitPolyPlacer(_NofitPolyPlacer&&) = default;
    _NofitPolyPlacer& operator=(_NofitPolyPlacer&&) = default;
#endif

    double score() const { return score_; }

    //BBS
    void plateID(int id) { plate_id = id; }
    int plateID() { return plate_id; }

    static inline double overfit(const Box& bb, const RawShape& bin) {
        auto bbin = sl::boundingBox(bin);
        auto d = bbin.center() - bb.center();
        _Rectangle<RawShape> rect(bb.width(), bb.height());
        rect.translate(bb.minCorner() + d);
        return sl::isInside(rect.transformedShape(), bin) ? -1.5 : 1;
    }

    static inline double overfit(const RawShape& chull, const RawShape& bin) {
        auto bbch = sl::boundingBox(chull);
        auto bbin = sl::boundingBox(bin);
        auto d = bbin.center() - bbch.center(); // move to bin center
        auto chullcpy = chull;
        sl::translate(chullcpy, d);
        return sl::isInside(chullcpy, bin) ? -1.0 : 1.0;
    }

    static inline double overfit(const RawShape& chull, const Box& bin)
    {
        auto bbch = sl::boundingBox(chull);
        return overfit(bbch, bin);
    }

    static inline double overfit(const Box& bb, const Box& bin)
    {
        auto wdiff = TCompute<RawShape>(bb.width()) - bin.width();
        auto hdiff = TCompute<RawShape>(bb.height()) - bin.height();
        double diff = .0;
        if(wdiff > 0) diff += double(wdiff);
        if(hdiff > 0) diff += double(hdiff);

        return diff;
    }

    static inline double overfit(const Box& bb, const _Circle<Vertex>& bin)
    {
        double boxr = 0.5*pl::distance(bb.minCorner(), bb.maxCorner());
        double diff = boxr - bin.radius();
        return diff;
    }

    static inline double overfit(const RawShape& chull,
                                const _Circle<Vertex>& bin)
    {
        double r = boundingCircle(chull).radius();
        double diff = r - bin.radius();
        return diff;
    }

    template<class Range = ConstItemRange<typename Base::DefaultIter>>
    PackResult trypack(Item& item,
                        const Range& remaining = Range()) {
        auto result = _trypack(item, remaining);

        // Experimental
        // if(!result) repack(item, result);

        return result;
    }

    void accept(PackResult& r) {
        if (r) {
            r.item_ptr_->translation(r.move_);
            r.item_ptr_->rotation(r.rot_);
            items_.emplace_back(*(r.item_ptr_));
            merged_pile_ = nfp::merge(merged_pile_, r.item_ptr_->transformedShape());
            score_ += r.score();
        }
    }

    ~_NofitPolyPlacer() {
        clearItems();
    }

    inline void clearItems() {
        finalAlign(bin_);
        Base::clearItems();
    }

    //clearFunc: itm will be cleared if return ture
    inline void clearItems(const std::function<bool(const Item &itm)> &clearFunc)
    {
        finalAlign(bin_);
        Base::clearItems(clearFunc);
    }

    void preload(const ItemGroup& packeditems) {
        Base::preload(packeditems);
        if (config_.on_preload)
            config_.on_preload(packeditems, config_);
    }

private:

    using Shapes = TMultiShape<RawShape>;

    Shapes calcnfp(const Item &trsh, const Box& bed ,Lvl<nfp::NfpLevel::CONVEX_ONLY>)
    {
        using namespace nfp;

        Shapes nfps(items_.size());

        // /////////////////////////////////////////////////////////////////////
        // TODO: this is a workaround and should be solved in Item with mutexes
        // guarding the mutable members when writing them.
        // /////////////////////////////////////////////////////////////////////
        trsh.transformedShape();
        trsh.referenceVertex();
        trsh.rightmostTopVertex();
        trsh.leftmostBottomVertex();

        for(Item& itm : items_) {
            itm.transformedShape();
            itm.referenceVertex();
            itm.rightmostTopVertex();
            itm.leftmostBottomVertex();
        }
        // /////////////////////////////////////////////////////////////////////

        __parallel::enumerate(items_.begin(), items_.end(),
                              [&nfps, &trsh](const Item& sh, size_t n)
        {
            auto& fixedp = sh.transformedShape();
            auto& orbp = trsh.transformedShape();
            auto subnfp_r = noFitPolygon<NfpLevel::CONVEX_ONLY>(fixedp, orbp);
            correctNfpPosition(subnfp_r, sh, trsh);
            nfps[n] = subnfp_r.first;
        });

        RawShape innerNfp = nfpInnerRectBed(bed, trsh.transformedShape()).first;
        return nfp::subtract({innerNfp}, nfps);
    }

    Shapes calcnfp(const RawShape &sliding, const Shapes &stationarys, const Box &bed, Lvl<nfp::NfpLevel::CONVEX_ONLY>)
    {
        using namespace nfp;

        Shapes nfps(stationarys.size());
        Item   slidingItem(sliding);
        slidingItem.transformedShape();
        __parallel::enumerate(stationarys.begin(), stationarys.end(), [&nfps, sliding, &slidingItem](const RawShape &stationary, size_t n) {
            auto subnfp_r = noFitPolygon<NfpLevel::CONVEX_ONLY>(stationary, sliding);
            correctNfpPosition(subnfp_r, stationary, slidingItem);
            nfps[n] = subnfp_r.first;
        });

        RawShape innerNfp = nfpInnerRectBed(bed, sliding).first;
        return nfp::subtract({innerNfp}, nfps);
    }

    template<class Level>
    Shapes calcnfp(const Item &/*trsh*/, Level)
    { // Function for arbitrary level of nfp implementation

        // TODO: implement
        return {};
    }

    struct Optimum {
        double relpos;
        unsigned nfpidx;
        int hidx;
        Optimum(double pos, unsigned nidx):
            relpos(pos), nfpidx(nidx), hidx(-1) {}
        Optimum(double pos, unsigned nidx, int holeidx):
            relpos(pos), nfpidx(nidx), hidx(holeidx) {}
    };

    class Optimizer: public opt::TOptimizer<opt::Method::L_SUBPLEX> {
    public:
        Optimizer(float accuracy = 1.f) {
            opt::StopCriteria stopcr;
            stopcr.max_iterations = unsigned(std::floor(1000 * accuracy));
            stopcr.relative_score_difference = 1e-20;
            this->stopcr_ = stopcr;
        }
    };

    using Edges = EdgeCache<RawShape>;

    template<class Range = ConstItemRange<typename Base::DefaultIter>>
    PackResult _trypack(
            Item& item,
            const Range& remaining = Range()) {

        PackResult ret;

        bool can_pack = false;
        double best_overfit = std::numeric_limits<double>::max();

        ItemGroup remlist;
        if(remaining.valid) {
            remlist.insert(remlist.end(), remaining.from, remaining.to);
        }

        double global_score = std::numeric_limits<double>::max();

        auto initial_tr = item.translation();
        auto initial_rot = item.rotation();
        Vertex final_tr = {0, 0};
        Radians final_rot = initial_rot;
        Shapes nfps;

        auto& bin = bin_;
        double norm = norm_;
        auto pbb = sl::boundingBox(merged_pile_);
        auto binbb = sl::boundingBox(bin);
        auto origin = binbb.center();
        if(config_.alignment== Config::Alignment::BOTTOM_LEFT)
            origin = binbb.minCorner();

        // This is the kernel part of the object function that is
        // customizable by the library client
        std::function<double(const Item&)> _objfunc;
        if (config_.object_function) _objfunc = [this](const Item& item) {return config_.object_function(item, this->items_); };
        else {

            // Inside check has to be strict if no alignment was enabled
            std::function<double(const Box&)> ins_check;
            if(config_.alignment == Config::Alignment::DONT_ALIGN)
                ins_check = [&binbb, norm](const Box& fullbb) {
                    double ret = 0;
                    if(!sl::isInside(fullbb, binbb))
                        ret += norm;
                    return ret;
                };
            else
                ins_check = [&bin](const Box& fullbb) {
                    double miss = overfit(fullbb, bin);
                    miss = miss > 0? miss : 0;
                    return std::pow(miss, 2);
                };
            auto alignment = config_.alignment;
            _objfunc = [norm, origin, pbb, ins_check, alignment](const Item& item)
            {
                auto ibb = item.boundingBox();
                auto fullbb = sl::boundingBox(pbb, ibb);

                double score = pl::distance(ibb.center(), origin);
                if(alignment==Config::Alignment::BOTTOM_LEFT)
                    score = std::abs(ibb.center().y() - origin.y());
                score /= norm;

                score += ins_check(fullbb);

                return score;
            };
        }

        bool first_object = std::all_of(items_.begin(), items_.end(), [&](const Item &rawShape) { return rawShape.is_virt_object && !rawShape.is_wipe_tower; });

        // item won't overlap with virtual objects if it's inside or touches NFP
        auto overlapWithVirtObject = [&]() -> double {
            if (items_.empty()) return 0;
            nfps   = calcnfp(item, binbb, Lvl<MaxNfpLevel::value>());
            auto v = item.referenceVertex();
            for (const RawShape &nfp : nfps) {
                if (sl::isInside(v, nfp) || sl::touches(v, nfp)) { return 0; }
            }
            return 1;
        };

        if (first_object) {
            setInitialPosition(item);
            auto best_tr = item.translation();
            auto best_rot = item.rotation();
            best_overfit = overfit(item.transformedShape(), bin_) + overlapWithVirtObject();

            for(auto rot : config_.rotations) {
                item.translation(initial_tr);
                item.rotation(initial_rot + rot);
                setInitialPosition(item);
                double of = 0.;
                if ((of = overfit(item.transformedShape(), bin_)) + overlapWithVirtObject() < best_overfit) {
                    best_overfit = of;
                    best_tr = item.translation();
                    best_rot = item.rotation();
                }
            }

            can_pack = best_overfit <= 0;
            if (can_pack)
                global_score = 0.2;
            item.rotation(best_rot);
            item.translation(best_tr);
        }
        if (can_pack == false) {

            Pile merged_pile = merged_pile_;

            for(auto rot : config_.rotations) {

                item.translation(initial_tr);
                item.rotation(initial_rot + rot);
                item.boundingBox(); // fill the bb cache

                // place the new item outside of the print bed to make sure
                // it is disjunct from the current merged pile
                placeOutsideOfBin(item);

                nfps = calcnfp(item, binbb, Lvl<MaxNfpLevel::value>());


                auto iv = item.referenceVertex();

                auto startpos = item.translation();

                std::vector<Edges> ecache;
                ecache.reserve(nfps.size());

                for(auto& nfp : nfps ) {
                    ecache.emplace_back(nfp);
                    ecache.back().accuracy(config_.accuracy);
                }

                // Our object function for placement
                auto rawobjfunc = [_objfunc, iv, startpos]
                        (Vertex v, Item& itm)
                {
                    auto d = (v - iv) + startpos;
                    itm.translation(d);
                    return _objfunc(itm);
                };

                auto getNfpPoint = [&ecache](const Optimum& opt)
                {
                    return opt.hidx < 0? ecache[opt.nfpidx].coords(opt.relpos) :
                            ecache[opt.nfpidx].coords(opt.hidx, opt.relpos);
                };

                auto alignment = config_.alignment;

                auto boundaryCheck = [alignment, &merged_pile, &getNfpPoint,
                        &item, &bin, &iv, &startpos] (const Optimum& o)
                {
                    auto v = getNfpPoint(o);
                    auto d = (v - iv) + startpos;
                    item.translation(d);

                    merged_pile.emplace_back(item.transformedShape());
                    auto chull = sl::convexHull(merged_pile);
                    merged_pile.pop_back();

                    double miss = 0;
                    if(alignment == Config::Alignment::DONT_ALIGN)
                       miss = sl::isInside(chull, bin) ? -1.0 : 1.0;
                    else miss = overfit(chull, bin);

                    return miss;
                };

                Optimum optimum(0, 0);
                double best_score = std::numeric_limits<double>::max();
                std::launch policy = std::launch::deferred;
                if(config_.parallel) policy |= std::launch::async;

                if(config_.before_packing)
                    config_.before_packing(merged_pile, items_, remlist);

                using OptResult = opt::Result<double>;
                using OptResults = std::vector<OptResult>;

                // Local optimization with the four polygon corners as
                // starting points
                for(unsigned ch = 0; ch < ecache.size(); ch++) {
                    auto& cache = ecache[ch];

                    OptResults results(cache.corners().size());

                    auto& rofn = rawobjfunc;
                    auto& nfpoint = getNfpPoint;
                    float accuracy = config_.accuracy;

                    __parallel::enumerate(
                                cache.corners().begin(),
                                cache.corners().end(),
                                [&results, &item, &rofn, &nfpoint, ch, accuracy]
                                (double pos, size_t n)
                    {
                        Optimizer solver(accuracy);

                        Item itemcpy = item;
                        auto contour_ofn = [&rofn, &nfpoint, ch, &itemcpy]
                                (double relpos)
                        {
                            Optimum op(relpos, ch);
                            return rofn(nfpoint(op), itemcpy);
                        };

                        try {
                            results[n] = solver.optimize_min(contour_ofn,
                                            opt::initvals<double>(pos),
                                            opt::bound<double>(0, 1.0)
                                            );
                        } catch(std::exception& e) {
                            derr() << "ERROR: " << e.what() << "\n";
                        }
                    }, policy);

                    auto resultcomp =
                            []( const OptResult& r1, const OptResult& r2 ) {
                        return r1.score < r2.score;
                    };

                    auto mr = *std::min_element(results.begin(), results.end(),
                                                resultcomp);

                    if(mr.score < best_score) {
                        Optimum o(std::get<0>(mr.optimum), ch, -1);
                        double miss = boundaryCheck(o);
                        if(miss <= 0) {
                            best_score = mr.score;
                            optimum = o;
                        } else {
                            best_overfit = std::min(miss, best_overfit);
                        }
                    }

                    for(unsigned hidx = 0; hidx < cache.holeCount(); ++hidx) {
                        results.clear();
                        results.resize(cache.corners(hidx).size());

                        // TODO : use parallel for
                        __parallel::enumerate(cache.corners(hidx).begin(),
                                      cache.corners(hidx).end(),
                                      [&results, &item, &nfpoint,
                                       &rofn, ch, hidx, accuracy]
                                      (double pos, size_t n)
                        {
                            Optimizer solver(accuracy);

                            Item itmcpy = item;
                            auto hole_ofn =
                                    [&rofn, &nfpoint, ch, hidx, &itmcpy]
                                    (double pos)
                            {
                                Optimum opt(pos, ch, hidx);
                                return rofn(nfpoint(opt), itmcpy);
                            };

                            try {
                                results[n] = solver.optimize_min(hole_ofn,
                                                opt::initvals<double>(pos),
                                                opt::bound<double>(0, 1.0)
                                                );

                            } catch(std::exception& e) {
                                derr() << "ERROR: " << e.what() << "\n";
                            }
                        }, policy);

                        auto hmr = *std::min_element(results.begin(),
                                                    results.end(),
                                                    resultcomp);

                        if(hmr.score < best_score) {
                            Optimum o(std::get<0>(hmr.optimum),
                                      ch, hidx);
                            double miss = boundaryCheck(o);
                            if(miss <= 0.0) {
                                best_score = hmr.score;
                                optimum = o;
                            } else {
                                best_overfit = std::min(miss, best_overfit);
                            }
                        }
                    }
                }

                if( best_score < global_score) {
                    auto d = (getNfpPoint(optimum) - iv) + startpos;
                    final_tr = d;
                    final_rot = initial_rot + rot;
                    can_pack = true;
                    global_score = best_score;
                }
            }

            item.translation(final_tr);
            item.rotation(final_rot);
        }

#ifdef SVGTOOLS_HPP
        svg::SVGWriter<RawShape> svgwriter;
        Box binbb2(binbb.width() * 2, binbb.height() * 2, binbb.center()); // expand bbox to allow object be drawed outside
        svgwriter.setSize(binbb2);
        svgwriter.conf_.x0 = binbb.width();
        svgwriter.conf_.y0 = -binbb.height()/2; // origin is top left corner
        svgwriter.writeShape(box2RawShape(binbb), "none", "black");
        for (int i = 0; i < nfps.size(); i++)
            svgwriter.writeShape(nfps[i], "none", "blue");
        for (int i = 0; i < items_.size(); i++)
            svgwriter.writeItem(items_[i], "none", "black");
        for (int i = 0; i < merged_pile_.size(); i++)
            svgwriter.writeShape(merged_pile_[i], "none", "yellow");
        svgwriter.writeItem(item, "none", "red", 2);

        std::stringstream ss;
        ss.setf(std::ios::fixed | std::ios::showpoint);
        ss.precision(1);
        ss << "t=" << round(item.translation().x() / 1e6) << "," << round(item.translation().y() / 1e6)
            //<< "-rot=" << round(item.rotation().toDegrees())
            << "-sco=" << round(global_score);
        svgwriter.draw_text(20, 20, ss.str(), "blue", 20);
        ss.str("");
        ss << "items.size=" << items_.size()
            << "-merged_pile.size=" << merged_pile_.size();
        svgwriter.draw_text(20, 40, ss.str(), "blue", 20);
        svgwriter.save(boost::filesystem::path("SVG")/ ("nfpplacer_" + std::to_string(plate_id) + "_" + ss.str() + "_" + item.name + ".svg"));
#endif

        if(can_pack) {
            ret = PackResult(item);
            ret.score_ = global_score;
            //merged_pile_ = nfp::merge(merged_pile_, item.transformedShape());
        } else {
            ret = PackResult(best_overfit);
        }

        return ret;
    }

    RawShape box2RawShape(Box& bbin)
    {
        RawShape binrsh;
        auto minx = getX(bbin.minCorner());
        auto miny = getY(bbin.minCorner());
        auto maxx = getX(bbin.maxCorner());
        auto maxy = getY(bbin.maxCorner());
        sl::addVertex(binrsh, {minx, miny});
        sl::addVertex(binrsh, {maxx, miny});
        sl::addVertex(binrsh, {maxx, maxy});
        sl::addVertex(binrsh, {minx, maxy});
        return binrsh;
    }
#if 0
    Box inscribedBox(ClipperLib::Polygon bin_)
    {
        Box bbin = sl::boundingBox(bin_);
        Vertex cb = bbin.center();
        auto minx = getX(bbin.minCorner());
        auto miny = getY(bbin.minCorner());
        auto maxx = getX(bbin.maxCorner());
        auto maxy = getY(bbin.maxCorner());
        for (auto pt : bin_.Contour)
        {
            if (getX(pt) < getX(cb))
                minx = std::max(minx, getX(pt));
            if (getY(pt) < getY(cb))
                miny = std::max(miny, getY(pt));
            if (getX(pt) > getX(cb))
                maxx = std::min(maxx, getX(pt));
            if (getY(pt) > getY(cb))
                maxy = std::min(maxy, getY(pt));
        }
        return Box(TPoint<RawShape>(minx, miny), TPoint<RawShape>(maxx, maxy));
    }
    Box inscribedBox(Box bin_)
    {
        Box bbin = sl::boundingBox(bin_);
        return bbin;
    }
    Box inscribedBox(_Circle<TPoint<RawShape>> bin_)
    { // TODO inscribed box of circle need to reconsider. Here it's just bounding box
        Box bbin = sl::boundingBox(bin_);
        return bbin;
    }
#endif
    inline void finalAlign(const RawShape& pbin) {
        auto bbin = sl::boundingBox(pbin);//inscribedBox(pbin);
        finalAlign(bbin);
    }

    inline void finalAlign(_Circle<TPoint<RawShape>> cbin) {
        if(items_.empty() ||
                config_.alignment == Config::Alignment::DONT_ALIGN) return;

        nfp::Shapes<RawShape> m;
        m.reserve(items_.size());
        for(Item& item : items_) m.emplace_back(item.transformedShape());

        auto c = boundingCircle(sl::convexHull(m));

        auto d = cbin.center() - c.center();
        for(Item& item : items_) item.translate(d);
    }

    inline void finalAlign(Box bbin) {
        if(items_.empty() ||
                config_.alignment == Config::Alignment::DONT_ALIGN) return;

        Box bb;
        for (Item& item : items_)
            if (!item.is_virt_object)
                bb = sl::boundingBox(item.boundingBox(), bb);

        Vertex ci, cb;

        switch(config_.alignment) {
        case Config::Alignment::CENTER: {
            ci = bb.center();
            cb = bbin.center();
            break;
        }
        case Config::Alignment::BOTTOM_LEFT: {
            ci = bb.minCorner();
            cb = bbin.minCorner();
            break;
        }
        case Config::Alignment::BOTTOM_RIGHT: {
            ci = {getX(bb.maxCorner()), getY(bb.minCorner())};
            cb = {getX(bbin.maxCorner()), getY(bbin.minCorner())};
            break;
        }
        case Config::Alignment::TOP_LEFT: {
            ci = {getX(bb.minCorner()), getY(bb.maxCorner())};
            cb = {getX(bbin.minCorner()), getY(bbin.maxCorner())};
            break;
        }
        case Config::Alignment::TOP_RIGHT: {
            ci = bb.maxCorner();
            cb = bbin.maxCorner();
            break;
        }
        case Config::Alignment::USER_DEFINED: {
            ci = bb.center();
            cb = config_.best_object_pos;
            break;
        }
        default: ; // DONT_ALIGN
        }

        auto d = cb - ci;       

        // BBS make sure the item won't clash with excluded regions
        // do we have wipe tower after arranging?
        std::set<int> extruders;
        for (const Item& item : items_) {
            if (!item.is_virt_object) { extruders.insert(item.extrude_ids.begin(), item.extrude_ids.end()); }
        }
        bool need_wipe_tower = extruders.size() > 1;

        std::vector<RawShape> objs,excludes;
        for (const Item &item : items_) {
            if (item.isFixed()) continue;
            objs.push_back(item.transformedShape());
        }
        if (objs.empty())
            return;
        { // find a best position inside NFP of fixed items (excluded regions), so the center of pile is cloest to bed center
            RawShape objs_convex_hull = sl::convexHull(objs);
            for (const Item &item : items_) {
                if (item.isFixed()) {
        		    excludes.push_back(item.transformedShape());
                }
            }

            auto   nfps = calcnfp(objs_convex_hull, excludes, bbin, Lvl<MaxNfpLevel::value>());
            if (nfps.empty()) {
                return;
            }
            Item   objs_convex_hull_item(objs_convex_hull);
            Vertex objs_convex_hull_ref = objs_convex_hull_item.referenceVertex();
            Vertex diff                 = objs_convex_hull_ref - sl::boundingBox(objs_convex_hull).center();
            Vertex ref_aligned = cb + diff;  // reference point when pile center aligned with bed center
            bool ref_aligned_is_ok = std::any_of(nfps.begin(), nfps.end(), [&ref_aligned](auto& nfp) {return sl::isInside(ref_aligned, nfp); });
            if (!ref_aligned_is_ok) {
                // ref_aligned is not good, then find a nearest point on nfp boundary
                Vertex ref_projected = projection_onto(nfps, ref_aligned);
                d +=  (ref_projected - ref_aligned);
            }
        }
        for(Item& item : items_)
            if (!item.is_virt_object)
                item.translate(d);
    }

    void setInitialPosition(Item& item) {
        Box bb = item.boundingBox();
        
        Vertex ci, cb;
        Box    bbin = sl::boundingBox(bin_);
        Vertex shrink(10, 10);
        bbin.maxCorner() -= shrink;
        bbin.minCorner() += shrink;

        switch(config_.starting_point) {
        case Config::Alignment::CENTER: {
            ci = bb.center();
            cb = bbin.center();
            break;
        }
        case Config::Alignment::BOTTOM_LEFT: {
            ci = bb.minCorner();
            cb = bbin.minCorner();
            break;
        }
        case Config::Alignment::BOTTOM_RIGHT: {
            ci = {getX(bb.maxCorner()), getY(bb.minCorner())};
            cb = {getX(bbin.maxCorner()), getY(bbin.minCorner())};
            break;
        }
        case Config::Alignment::TOP_LEFT: {
            ci = {getX(bb.minCorner()), getY(bb.maxCorner())};
            cb = {getX(bbin.minCorner()), getY(bbin.maxCorner())};
            break;
        }
        case Config::Alignment::TOP_RIGHT: {
            ci = bb.maxCorner();
            cb = bbin.maxCorner();
            break;
        }
        default:;
        }

        auto d = cb - ci;
        item.translate(d);
    }

    void placeOutsideOfBin(Item& item) {
        auto&& bb = item.boundingBox();
        Box binbb = sl::boundingBox(bin_);

        Vertex v = { getX(bb.maxCorner()), getY(bb.minCorner()) };

        Coord dx = getX(binbb.maxCorner()) - getX(v);
        Coord dy = getY(binbb.maxCorner()) - getY(v);

        item.translate({dx, dy});
    }

};


}
}

#endif // NOFITPOLY_H
