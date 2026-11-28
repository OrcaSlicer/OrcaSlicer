// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Stefan Brugger <stefanbrugger@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PROCRUSTES_H
#define IGL_PROCRUSTES_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>

namespace igl
{
  /// Solve Procrustes problem in d dimensions.  Given two point sets X,Y in R^d
  /// find best scale s, orthogonal R  and translation t s.t. |s*X*R + t - Y|^2
  /// is minimized.
  ///
  /// @tparam DerivedV point type
  /// @tparam Scalar   scalar type
  /// @tparam DerivedR type of R
  /// @tparam DerivedT type of t
  /// @param[in] X  #V by DIM first list of points
  /// @param[in] Y  #V by DIM second list of points
  /// @param[in] includeScaling  if scaling should be allowed
  /// @param[in] includeReflections  if R is allowed to be a reflection
  /// @param[out] scale  scaling
  /// @param[out] R      orthogonal matrix
  /// @param[out] t      translation
  ///
  /// #### Example
  ///
  /// \code{cpp}
  ///     MatrixXd X, Y; (containing 3d points as rows)
  ///     double scale;
  ///     MatrixXd R;
  ///     VectorXd t;
  ///     igl::procrustes(X,Y,true,false,scale,R,t);
  ///     R *= scale;
  ///     MatrixXd Xprime = (X * R).rowwise() + t.transpose();
  /// \endcode
  ///
  template <
    typename DerivedX, 
    typename DerivedY, 
    typename Scalar, 
    typename DerivedR, 
    typename DerivedT>
  IGL_INLINE void procrustes(
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedY>& Y,
    const bool includeScaling,
    const bool includeReflections,
    Scalar& scale,
    Eigen::PlainObjectBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedT>& t);
  /// \overload
  /// \brief Same as above but returns Eigen transformation object.
  ///
  /// @param[out] T  transformation that minimizes error    
  ///
  /// #### Example
  /// \code{cpp}
  ///      MatrixXd X, Y; (containing 3d points as rows)
  ///      AffineCompact3d T;
  ///      igl::procrustes(X,Y,true,false,T);
  ///      MatrixXd Xprime = (X * T.linear()).rowwise() + T.translation().transpose();
  /// \endcode
  template <
    typename DerivedX, 
    typename DerivedY, 
    typename Scalar, 
    int DIM, 
    int TType>
  IGL_INLINE void procrustes(
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedY>& Y,
    const bool includeScaling,
    const bool includeReflections,
    Eigen::Transform<Scalar,DIM,TType>& T);
  /// \overload
  /// @param[out] S  S=scale*R, instead of scale and R separately
  template <
    typename DerivedX, 
    typename DerivedY, 
    typename DerivedR, 
    typename DerivedT>
  IGL_INLINE void procrustes(
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedY>& Y,
    const bool includeScaling,
    const bool includeReflections,
    Eigen::PlainObjectBase<DerivedR>& S,
    Eigen::PlainObjectBase<DerivedT>& t);
  /// \overload
  /// \brief Convenient wrapper for rigid case (no scaling, no reflections)
  template <
    typename DerivedX, 
    typename DerivedY, 
    typename DerivedR, 
    typename DerivedT>
  IGL_INLINE void procrustes(
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedY>& Y,
    Eigen::PlainObjectBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedT>& t);
  /// \overload
  /// \brief Convenient wrapper for 2D case.
  template <
    typename DerivedX, 
    typename DerivedY, 
    typename Scalar, 
    typename DerivedT>
  IGL_INLINE void procrustes(
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedY>& Y,
    Eigen::Rotation2D<Scalar>& R,
    Eigen::PlainObjectBase<DerivedT>& t);
}

#ifndef IGL_STATIC_LIBRARY
  #include "procrustes.cpp"
#endif

#endif
