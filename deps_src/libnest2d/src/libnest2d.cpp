#include <libnest2d/libnest2d.hpp>

namespace libnest2d {

template class _Nester<NfpPlacer, FirstFitSelection>;
template class _Nester<BottomLeftPlacer, FirstFitSelection>;

template std::size_t _Nester<NfpPlacer, FirstFitSelection>::execute(
        std::vector<Item>::iterator, std::vector<Item>::iterator);
template std::size_t _Nester<BottomLeftPlacer, FirstFitSelection>::execute(
        std::vector<Item>::iterator, std::vector<Item>::iterator);

template std::size_t nest(std::vector<Item>::iterator from,
                          std::vector<Item>::iterator to,
                          const Box & bin,
                          Coord dist,
                          const NestConfig<NfpPlacer, FirstFitSelection> &cfg,
                          NestControl ctl);

template std::size_t nest(std::vector<Item>::iterator from,
                          std::vector<Item>::iterator to,
                          const Box & bin,
                          Coord dist,
                          const NestConfig<BottomLeftPlacer, FirstFitSelection> &cfg,
                          NestControl ctl);
}
