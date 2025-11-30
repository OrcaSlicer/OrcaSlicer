// IWYU pragma: private
#include "../../InternalHeaderCheck.h"

namespace Eigen {
namespace internal {

#ifndef EIGEN_LSX_GEBP_NR
#define EIGEN_LSX_GEBP_NR 8
#endif

template <>
struct gebp_traits<float, float, false, false, Architecture::LSX, GEBPPacketFull>
    : gebp_traits<float, float, false, false, Architecture::Generic, GEBPPacketFull> {
  enum { nr = EIGEN_LSX_GEBP_NR };
};

template <>
struct gebp_traits<double, double, false, false, Architecture::LSX, GEBPPacketFull>
    : gebp_traits<double, double, false, false, Architecture::Generic, GEBPPacketFull> {
  enum { nr = EIGEN_LSX_GEBP_NR };
};
}  // namespace internal
}  // namespace Eigen
