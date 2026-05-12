// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BIHARMONIC_COORDINATES_H
#define IGL_BIHARMONIC_COORDINATES_H
#include "igl_inline.h"
#include <Eigen/Dense>
#include <vector>
namespace igl
{
  /// Compute "discrete biharmonic generalized barycentric coordinates" as
  /// described in "Linear Subspace Design for Real-Time Shape Deformation"
  /// [Wang et al. 2015]. Not to be confused with "Bounded Biharmonic Weights
  /// for Real-Time Deformation" [Jacobson et al. 2011] or "Biharmonic
  /// Coordinates" (2D complex barycentric coordinates) [Weber et al. 2012].
  /// These weights minimize a discrete version of the squared Laplacian energy
  /// subject to positional interpolation constraints at selected vertices
  /// (point handles) and transformation interpolation constraints at regions
  /// (region handles).
  ///
  /// @tparam SType  should be a simple index type e.g. `int`,`size_t`
  /// @param[in] V  #V by dim list of mesh vertex positions
  /// @param[in] T  #T by dim+1 list of / triangle indices into V      if dim=2
  ///                          \ tetrahedron indices into V   if dim=3
  /// @param[in] S  #point-handles+#region-handles list of lists of selected vertices for
  ///     each handle. Point handles should have singleton lists and region
  ///     handles should have lists of size at least dim+1 (and these points
  ///     should be in general position).
  /// @param[out] W  #V by #points-handles+(#region-handles * dim+1) matrix of weights so
  ///     that columns correspond to each handles generalized barycentric
  ///     coordinates (for point-handles) or animation space weights (for region
  ///     handles).
  /// @return true only on success
  ///
  /// #### Example:
  ///
  /// \code{cpp}
  ///     MatrixXd W;
  ///     igl::biharmonic_coordinates(V,F,S,W);
  ///     const size_t dim = T.cols()-1;
  ///     MatrixXd H(W.cols(),dim);
  ///     {
  ///       int c = 0;
  ///       for(int h = 0;h<S.size();h++)
  ///       {
  ///         if(S[h].size()==1)
  ///         {
  ///           H.row(c++) = V.block(S[h][0],0,1,dim);
  ///         }else
  ///         {
  ///           H.block(c,0,dim+1,dim).setIdentity();
  ///           c+=dim+1;
  ///         }
  ///       }
  ///     }
  ///     assert( (V-(W*H)).array().maxCoeff() < 1e-7 );
  /// \endcode
  template <
    typename DerivedV,
    typename DerivedT,
    typename SType,
    typename DerivedW>
  IGL_INLINE bool biharmonic_coordinates(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedT> & T,
    const std::vector<std::vector<SType> > & S,
    Eigen::PlainObjectBase<DerivedW> & W);
  /// \overload
  /// @param[in] k  power of Laplacian (experimental)
  template <
    typename DerivedV,
    typename DerivedT,
    typename SType,
    typename DerivedW>
  IGL_INLINE bool biharmonic_coordinates(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedT> & T,
    const std::vector<std::vector<SType> > & S,
    const int k,
    Eigen::PlainObjectBase<DerivedW> & W);

};
#  ifndef IGL_STATIC_LIBRARY
#    include "biharmonic_coordinates.cpp"
#  endif
#endif
