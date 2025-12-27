#ifndef LIBNEST2D_HPP
#define LIBNEST2D_HPP

// The type of backend should be set conditionally by the cmake configuriation
// for now we set it statically to clipper backend
#ifdef LIBNEST2D_GEOMETRIES_clipper
#include <libnest2d/backends/clipper/geometries.hpp>
#endif

#ifdef LIBNEST2D_GEOMETRIES_libslic3r
#include <libnest2d/backends/libslic3r/geometries.hpp>
#endif

#ifdef LIBNEST2D_OPTIMIZER_nlopt
// We include the stock optimizers for local and global optimization
#include <libnest2d/optimizers/nlopt/subplex.hpp>     // Local subplex for NfpPlacer
#include <libnest2d/optimizers/nlopt/genetic.hpp>     // Genetic for min. bounding box
#endif

#include <libnest2d/nester.hpp>
#include <libnest2d/placers/bottomleftplacer.hpp>
#include <libnest2d/placers/nfpplacer.hpp>
#include <libnest2d/selections/firstfit.hpp>
#include <libnest2d/selections/filler.hpp>
#include <libnest2d/selections/djd_heuristic.hpp>

namespace libnest2d {

using Point = PointImpl;
using Coord = TCoord<PointImpl>;
using Box = _Box<PointImpl>;
using Segment = _Segment<PointImpl>;
using Circle = _Circle<PointImpl>;
using MultiPolygon = TMultiShape<PolygonImpl>; 

using Item = _Item<PolygonImpl>;
using Rectangle = _Rectangle<PolygonImpl>;
using PackGroup = _PackGroup<PolygonImpl>;

using FillerSelection = selections::_FillerSelection<PolygonImpl>;
using FirstFitSelection = selections::_FirstFitSelection<PolygonImpl>;
using DJDHeuristic  = selections::_DJDHeuristic<PolygonImpl>;

template<class Bin> // Generic placer for arbitrary bin types
using _NfpPlacer = placers::_NofitPolyPlacer<PolygonImpl, Bin>;

// NfpPlacer is with Box bin
using NfpPlacer = _NfpPlacer<Box>;

// This supports only box shaped bins
using BottomLeftPlacer = placers::_BottomLeftPlacer<PolygonImpl>;

#ifdef LIBNEST2D_STATIC

extern template class _Nester<NfpPlacer, FirstFitSelection>;
extern template class _Nester<BottomLeftPlacer, FirstFitSelection>;
extern template std::size_t _Nester<NfpPlacer, FirstFitSelection>::execute(
        std::vector<Item>::iterator, std::vector<Item>::iterator);
extern template std::size_t _Nester<BottomLeftPlacer, FirstFitSelection>::execute(
        std::vector<Item>::iterator, std::vector<Item>::iterator);

#endif

template<class Placer = NfpPlacer, class Selector = FirstFitSelection>
struct NestConfig {
    typename Placer::Config placer_config;
    typename Selector::Config selector_config;
    using Placement = typename Placer::Config;
    using Selection = typename Selector::Config;
    
    NestConfig() = default;
    NestConfig(const typename Placer::Config &cfg)   : placer_config{cfg} {}
    NestConfig(const typename Selector::Config &cfg) : selector_config{cfg} {}
    NestConfig(const typename Placer::Config &  pcfg,
               const typename Selector::Config &scfg)
        : placer_config{pcfg}, selector_config{scfg} {}
};

struct NestControl {
    ProgressFunction progressfn;
    StopCondition stopcond = []{ return false; };
    
    NestControl() = default;
    NestControl(ProgressFunction pr) : progressfn{std::move(pr)} {}
    NestControl(StopCondition sc) : stopcond{std::move(sc)} {}
    NestControl(ProgressFunction pr, StopCondition sc)
        : progressfn{std::move(pr)}, stopcond{std::move(sc)}
    {}
};

template<class Placer = NfpPlacer,
         class Selector = FirstFitSelection,
         class Iterator = std::vector<Item>::iterator>
std::size_t nest(Iterator from, Iterator to,
                 const typename Placer::BinType & bin,
                 Coord dist = 0,
                 const NestConfig<Placer, Selector> &cfg = {},
                 NestControl ctl = {})
{
    _Nester<Placer, Selector> nester{bin, dist, cfg.placer_config, cfg.selector_config};
    if(ctl.progressfn) nester.progressIndicator(ctl.progressfn);
    if(ctl.stopcond) nester.stopCondition(ctl.stopcond);
    return nester.execute(from, to);
}

#ifdef LIBNEST2D_STATIC

extern template class _Nester<NfpPlacer, FirstFitSelection>;
extern template class _Nester<BottomLeftPlacer, FirstFitSelection>;
extern template std::size_t nest(std::vector<Item>::iterator from,
                                 std::vector<Item>::iterator to,
                                 const Box & bin,
                                 Coord dist,
                                 const NestConfig<NfpPlacer, FirstFitSelection> &cfg,
                                 NestControl ctl);
extern template std::size_t nest(std::vector<Item>::iterator from,
                                 std::vector<Item>::iterator to,
                                 const Box & bin,
                                 Coord dist,
                                 const NestConfig<BottomLeftPlacer, FirstFitSelection> &cfg,
                                 NestControl ctl);

#endif

template<class Placer = NfpPlacer,
         class Selector = FirstFitSelection,
         class Container = std::vector<Item>>
std::size_t nest(Container&& cont,
                 const typename Placer::BinType & bin,
                 Coord dist = 0,
                 const NestConfig<Placer, Selector> &cfg = {},
                 NestControl ctl = {})
{
    return nest<Placer, Selector>(cont.begin(), cont.end(), bin, dist, cfg, ctl);
}

template<class T = double> enable_if_t<std::is_arithmetic<T>::value, TCoord<PointImpl>> mm(T val = T(1)) 
{
    return TCoord<PointImpl>(val * CoordType<PointImpl>::MM_IN_COORDS);
}

}

#endif // LIBNEST2D_HPP
