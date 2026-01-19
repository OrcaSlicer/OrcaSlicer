#ifndef IGL_STRAIGHTEN_SEAMS_H
#define IGL_STRAIGHTEN_SEAMS_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  // STRAIGHTEN_SEAMS Given a obj-style mesh with (V,F) defining the geometric
  // surface of the mesh and (VT,FT) defining the
  // parameterization/texture-mapping of the mesh in the uv-domain, find all
  // seams and boundaries in the texture-mapping and "straighten" them,
  // remapping vertices along the boundary and in the interior. This will be
  // careful to consistently straighten multiple seams in the texture-mesh
  // corresponding to the same edge chains in the surface-mesh. 
  //
  // [UT] = straighten_seams(V,F,VT,FT)
  //
  // Inputs:
  //  V  #V by 3 list of vertices
  //  F  #F by 3 list of triangle indices
  //  VT  #VT by 2 list of texture coordinates
  //  FT  #F by 3 list of triangle texture coordinates
  //  Optional:
  //    'Tol'  followed by Ramer-Douglas-Peucker tolerance as a fraction of the
  //      curves bounding box diagonal (see dpsimplify)
  // Outputs:
  //   UE  #UE by 2 list of indices into VT of coarse output polygon edges
  //   UT  #VT by 3 list of new texture coordinates
  //   OT  #OT by 2 list of indices into VT of boundary edges 
  //
  // See also: simplify_curve, dpsimplify
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
