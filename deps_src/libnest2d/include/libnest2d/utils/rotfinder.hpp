#ifndef ROTFINDER_HPP
#define ROTFINDER_HPP

#include <libnest2d/libnest2d.hpp>
#include <libnest2d/optimizer.hpp>
#include <iterator>

namespace libnest2d {

template<class RawShape>
Radians findBestRotation(_Item<RawShape>& item) {
    opt::StopCriteria stopcr;
    stopcr.absolute_score_difference = 0.01;
    stopcr.max_iterations = 10000;
    opt::TOptimizer<opt::Method::G_GENETIC> solver(stopcr);

    auto orig_rot = item.rotation();

    auto result = solver.optimize_min([&item, &orig_rot](Radians rot){
        item.rotation(orig_rot + rot);
        auto bb = item.boundingBox();
        return std::sqrt(bb.height()*bb.width());
    }, opt::initvals(Radians(0)), opt::bound<Radians>(-Pi/2, Pi/2));

    item.rotation(orig_rot);

    return std::get<0>(result.optimum);
}

template<class Iterator>
void findMinimumBoundingBoxRotations(Iterator from, Iterator to) {
    using V = typename std::iterator_traits<Iterator>::value_type;
    std::for_each(from, to, [](V& item){
        Radians rot = findBestRotation(item);
        item.rotate(rot);
    });
}

}

#endif // ROTFINDER_HPP
