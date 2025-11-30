#ifndef IGL_KELVINLETS_H
#define IGL_KELVINLETS_H

#include <Eigen/Core>
#include <array>
#include "igl_inline.h"

namespace igl {

/// Kelvinlets brush type
enum class BrushType : int
{
  GRAB,
  SCALE,
  TWIST,
  PINCH,
};

/// Parameters for controling kelvinlets
template<typename Scalar>
struct KelvinletParams
{
  const Scalar epsilon;
  const int scale;
  const BrushType brushType;
  std::array<Scalar, 3> ep{}, w{};

  KelvinletParams(const Scalar& epsilon,
                  const int falloff,
                  const BrushType& type)
    : epsilon(epsilon)
    , scale(falloff)
    , brushType(type)
  {
    static constexpr std::array<Scalar, 3> brush_scaling_params{ 1.0f,
                                                                 1.1f,
                                                                 1.21f };
    for (int i = 0; i < 3; i++) {
      ep[i] = epsilon * brush_scaling_params[i];
    }
    w[0] = 1;
    w[1] = -((ep[2] * ep[2] - ep[0] * ep[0]) / (ep[2] * ep[2] - ep[1] * ep[1]));
    w[2] = (ep[1] * ep[1] - ep[0] * ep[0]) / (ep[2] * ep[2] - ep[1] * ep[1]);
  }
};

/// Implements Pixar's Regularized Kelvinlets (Pixar Technical Memo #17-03):
/// Sculpting Brushes based on Fundamental Solutions of Elasticity, a technique
/// for real-time physically based volume sculpting of virtual elastic materials
///
/// @param[in] V  #V by dim list of input points in space
/// @param[in] x0  dim-vector of brush tip
/// @param[in] f  dim-vector of brush force (translation)
/// @param[in] F  dim by dim matrix of brush force matrix  (linear)
/// @param[in] params  parameters for the kelvinlet brush like brush radius, scale etc
/// @param[out] X  #V by dim list of output points in space
template<typename DerivedV,
         typename Derivedx0,
         typename Derivedf,
         typename DerivedF,
         typename DerivedU>
IGL_INLINE void kelvinlets(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<Derivedx0>& x0,
  const Eigen::MatrixBase<Derivedf>& f,
  const Eigen::MatrixBase<DerivedF>& F,
  const KelvinletParams<typename DerivedV::Scalar>& params,
  Eigen::PlainObjectBase<DerivedU>& U);

}

#ifndef IGL_STATIC_LIBRARY
#include "kelvinlets.cpp"
#endif
#endif
