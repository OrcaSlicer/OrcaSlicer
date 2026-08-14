#ifndef IGL_STRAIGHTEN_SEAMS_H
#define IGL_STRAIGHTEN_SEAMS_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Given a obj-style mesh with (V,F) defining the geometric surface of the
  /// mesh and (VT,FT) defining the parameterization/texture-mapping of the mesh
  /// in the uv-domain, find all seams and boundaries in the texture-mapping and
  /// "straighten" them, remapping vertices along the boundary and in the
  /// interior. This will be careful to consistently straighten multiple seams
  /// in the texture-mesh corresponding to the same edge chains in the
  /// surface-mesh. 
  ///
  ///
  /// @param[in] V  #V by 3 list of vertices
  /// @param[in] F  #F by 3 list of triangle indices
  /// @param[in] VT  #VT by 2 list of texture coordinates
  /// @param[in] FT  #F by 3 list of triangle texture coordinates
  /// @param[in] tol  followed by Ramer-Douglas-Peucker tolerance as a fraction
  ///   of the curves bounding box diagonal (see dpsimplify)
  /// @param[out] UE  #UE by 2 list of indices into VT of coarse output polygon edges
  /// @param[out] UT  #VT by 3 list of new texture coordinates
  /// @param[out] OT  #OT by 2 list of indices into VT of boundary edges 
  ///
  /// \see ramer_douglas_peucker
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedVT,
    typename DerivedFT,
    typename Scalar,
    typename DerivedUE,
    typename DerivedUT,
    typename DerivedOT>
  IGL_INLINE void straighten_seams(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedVT> & VT,
    const Eigen::MatrixBase<DerivedFT> & FT,
    const Scalar tol,
    Eigen::PlainObjectBase<DerivedUE> & UE,
    Eigen::PlainObjectBase<DerivedUT> & UT,
    Eigen::PlainObjectBase<DerivedOT> & OT);
}

#ifndef IGL_STATIC_LIBRARY
#  include "straighten_seams.cpp"
#endif

#endif
