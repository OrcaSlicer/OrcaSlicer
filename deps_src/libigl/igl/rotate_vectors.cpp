// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "rotate_vectors.h"
IGL_INLINE Eigen::MatrixXd igl::rotate_vectors(
                    const Eigen::MatrixXd& V,
                    const Eigen::VectorXd& A,
                    const Eigen::MatrixXd& B1,
                    const Eigen::MatrixXd& B2)
{
  Eigen::MatrixXd RV(V.rows(),V.cols());

  for (unsigned i=0; i<V.rows();++i)
  {
    double norm = V.row(i).norm();
    
    // project onto the tangent plane and convert to angle
    double a = atan2(B2.row(i).dot(V.row(i)),B1.row(i).dot(V.row(i)));

    // rotate
    a += (A.size() == 1) ? A(0) : A(i);

    // move it back to global coordinates
    RV.row(i) = norm*cos(a) * B1.row(i) + norm*sin(a) * B2.row(i);
  }

  return RV;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
