// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "dihedral_angles.h"
#include <cassert>

template <
  typename DerivedV, 
  typename DerivedT, 
  typename Derivedtheta,
  typename Derivedcos_theta>
IGL_INLINE void igl::dihedral_angles(
  Eigen::PlainObjectBase<DerivedV>& V,
  Eigen::PlainObjectBase<DerivedT>& T,
  Eigen::PlainObjectBase<Derivedtheta>& theta,
  Eigen::PlainObjectBase<Derivedcos_theta>& cos_theta)
{
  using namespace Eigen;
  assert(T.cols() == 4);
  Matrix<typename Derivedtheta::Scalar,Dynamic,6> l;
  edge_lengths(V,T,l);
  Matrix<typename Derivedtheta::Scalar,Dynamic,4> s;
  face_areas(l,s);
  return dihedral_angles_intrinsic(l,s,theta,cos_theta);
}

template <
  typename DerivedL, 
  typename DerivedA, 
  typename Derivedtheta,
  typename Derivedcos_theta>
IGL_INLINE void igl::dihedral_angles_intrinsic(
  Eigen::PlainObjectBase<DerivedL>& L,
  Eigen::PlainObjectBase<DerivedA>& A,
  Eigen::PlainObjectBase<Derivedtheta>& theta,
  Eigen::PlainObjectBase<Derivedcos_theta>& cos_theta)
{
  using namespace Eigen;
  const int m = L.rows();
  assert(m == A.rows());
  // Law of cosines
  // http://math.stackexchange.com/a/49340/35376
  Matrix<typename Derivedtheta::Scalar,Dynamic,6> H_sqr(m,6);
  H_sqr.col(0) = (1./16.) * (4. * L.col(3).array().square() * L.col(0).array().square() - 
                                ((L.col(1).array().square() + L.col(4).array().square()) -
                                 (L.col(2).array().square() + L.col(5).array().square())).square());
  H_sqr.col(1) = (1./16.) * (4. * L.col(4).array().square() * L.col(1).array().square() - 
                                ((L.col(2).array().square() + L.col(5).array().square()) -
                                 (L.col(3).array().square() + L.col(0).array().square())).square());
  H_sqr.col(2) = (1./16.) * (4. * L.col(5).array().square() * L.col(2).array().square() - 
                                ((L.col(3).array().square() + L.col(0).array().square()) -
                                 (L.col(4).array().square() + L.col(1).array().square())).square());
  H_sqr.col(3) = (1./16.) * (4. * L.col(0).array().square() * L.col(3).array().square() - 
                                ((L.col(4).array().square() + L.col(1).array().square()) -
                                 (L.col(5).array().square() + L.col(2).array().square())).square());
  H_sqr.col(4) = (1./16.) * (4. * L.col(1).array().square() * L.col(4).array().square() - 
                                ((L.col(5).array().square() + L.col(2).array().square()) -
                                 (L.col(0).array().square() + L.col(3).array().square())).square());
  H_sqr.col(5) = (1./16.) * (4. * L.col(2).array().square() * L.col(5).array().square() - 
                                ((L.col(0).array().square() + L.col(3).array().square()) -
                                 (L.col(1).array().square() + L.col(4).array().square())).square());
  cos_theta.resize(m,6);
  cos_theta.col(0) = (H_sqr.col(0).array() - 
      A.col(1).array().square() - A.col(2).array().square()).array() / 
      (-2.*A.col(1).array() * A.col(2).array());
  cos_theta.col(1) = (H_sqr.col(1).array() - 
      A.col(2).array().square() - A.col(0).array().square()).array() / 
      (-2.*A.col(2).array() * A.col(0).array());
  cos_theta.col(2) = (H_sqr.col(2).array() - 
      A.col(0).array().square() - A.col(1).array().square()).array() / 
      (-2.*A.col(0).array() * A.col(1).array());
  cos_theta.col(3) = (H_sqr.col(3).array() - 
      A.col(3).array().square() - A.col(0).array().square()).array() / 
      (-2.*A.col(3).array() * A.col(0).array());
  cos_theta.col(4) = (H_sqr.col(4).array() - 
      A.col(3).array().square() - A.col(1).array().square()).array() / 
      (-2.*A.col(3).array() * A.col(1).array());
  cos_theta.col(5) = (H_sqr.col(5).array() - 
      A.col(3).array().square() - A.col(2).array().square()).array() / 
      (-2.*A.col(3).array() * A.col(2).array());

  theta = cos_theta.array().acos();

  cos_theta.resize(m,6);

}
#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::dihedral_angles_intrinsic<Eigen::Matrix<double, -1, 6, 0, -1, 6>, Eigen::Matrix<double, -1, 4, 0, -1, 4>, Eigen::Matrix<double, -1, 6, 0, -1, 6>, Eigen::Matrix<double, -1, 6, 0, -1, 6> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 6, 0, -1, 6> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 4, 0, -1, 4> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 6, 0, -1, 6> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 6, 0, -1, 6> >&);
#endif
