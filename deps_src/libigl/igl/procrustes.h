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
  // Solve Procrustes problem in d dimensions.  Given two point sets X,Y in R^d
  // find best scale s, orthogonal R  and translation t s.t. |s*X*R + t - Y|^2
  // is minimized.
  //
  // Templates:
  //    DerivedV point type
  //    Scalar   scalar type
  //    DerivedR type of R
  //    DerivedT type of t
  // Inputs:
  //    X  #V by DIM first list of points
  //    Y  #V by DIM second list of points
  //    includeScaling  if scaling should be allowed
  //    includeReflections  if R is allowed to be a reflection
  // Outputs:
  //    scale  scaling
  //    R      orthogonal matrix
  //    t      translation
  //
  // Example:
  //   MatrixXd X, Y; (containing 3d points as rows)
  //   double scale;
  //   MatrixXd R;
  //   VectorXd t;
  //   igl::procrustes(X,Y,true,false,scale,R,t);
  //   R *= scale;
  //   MatrixXd Xprime = (X * R).rowwise() + t.transpose();
  //
  template <
    typename DerivedX, 
    typename DerivedY, 
    typename Scalar, 
    typename DerivedR, 
    typename DerivedT>
  IGL_INLINE void procrustes(
    const Eigen::PlainObjectBase<DerivedX>& X,
    const Eigen::PlainObjectBase<DerivedY>& Y,
    bool includeScaling,
    bool includeReflections,
    Scalar& scale,
    Eigen::PlainObjectBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedT>& t);
  // Same as above but returns Eigen transformation object.
  //
  // Templates:
  //    DerivedV point type
  //    Scalar   scalar type
  //    DIM      point dimension
  //    TType    type of transformation
  //             (Isometry,Affine,AffineCompact,Projective)
  // Inputs:
  //    X  #V by DIM first list of points
  //    Y  #V by DIM second list of points
  //    includeScaling  if scaling should be allowed
  //    includeReflections  if R is allowed to be a reflection
  // Outputs:
  //    T  transformation that minimizes error    
  //
  // Example:
  //   MatrixXd X, Y; (containing 3d points as rows)
  //   AffineCompact3d T;
  //   igl::procrustes(X,Y,true,false,T);
  //   MatrixXd Xprime = (X * T.linear()).rowwise() + T.translation().transpose();
  template <
    typename DerivedX, 
    typename DerivedY, 
    typename Scalar, 
    int DIM, 
    int TType>
  IGL_INLINE void procrustes(
    const Eigen::PlainObjectBase<DerivedX>& X,
    const Eigen::PlainObjectBase<DerivedY>& Y,
    bool includeScaling,
    bool includeReflections,
    Eigen::Transform<Scalar,DIM,TType>& T);


  // Convenient wrapper that returns S=scale*R instead of scale and R separately
  template <
    typename DerivedX, 
    typename DerivedY, 
    typename DerivedR, 
    typename DerivedT>
  IGL_INLINE void procrustes(
    const Eigen::PlainObjectBase<DerivedX>& X,
    const Eigen::PlainObjectBase<DerivedY>& Y,
    bool includeScaling,
    bool includeReflections,
    Eigen::PlainObjectBase<DerivedR>& S,
    Eigen::PlainObjectBase<DerivedT>& t);

  // Convenient wrapper for rigid case (no scaling, no reflections)
  template <
    typename DerivedX, 
    typename DerivedY, 
    typename DerivedR, 
    typename DerivedT>
  IGL_INLINE void procrustes(
    const Eigen::PlainObjectBase<DerivedX>& X,
    const Eigen::PlainObjectBase<DerivedY>& Y,
    Eigen::PlainObjectBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedT>& t);

  // Convenient wrapper for 2D case.
  template <
    typename DerivedX, 
    typename DerivedY, 
    typename Scalar, 
    typename DerivedT>
  IGL_INLINE void procrustes(
    const Eigen::PlainObjectBase<DerivedX>& X,
    const Eigen::PlainObjectBase<DerivedY>& Y,
    Eigen::Rotation2D<Scalar>& R,
    Eigen::PlainObjectBase<DerivedT>& t);
}

#ifndef IGL_STATIC_LIBRARY
  #include "procrustes.cpp"
#endif

#endif
