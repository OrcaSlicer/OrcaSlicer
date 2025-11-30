// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifdef WIN32
  #define _USE_MATH_DEFINES
#endif
#include <cmath>

#include "comb_frame_field.h"
#include "local_basis.h"
#include "PI.h"
#include "PlainMatrix.h"

template <typename DerivedV, typename DerivedF, typename DerivedP>
IGL_INLINE void igl::comb_frame_field(
  const Eigen::MatrixBase<DerivedV> &V,
  const Eigen::MatrixBase<DerivedF> &F,
  const Eigen::MatrixBase<DerivedP> &PD1,
  const Eigen::MatrixBase<DerivedP> &PD2,
  const Eigen::MatrixBase<DerivedP> &BIS1_combed,
  const Eigen::MatrixBase<DerivedP> &BIS2_combed,
  Eigen::PlainObjectBase<DerivedP> &PD1_combed,
  Eigen::PlainObjectBase<DerivedP> &PD2_combed)
{
  PlainMatrix<DerivedV,Eigen::Dynamic> B1, B2, B3;
  igl::local_basis(V,F,B1,B2,B3);

  PD1_combed.resize(BIS1_combed.rows(),3);
  PD2_combed.resize(BIS2_combed.rows(),3);

  for (unsigned i=0; i<PD1.rows();++i)
  {
    Eigen::Matrix<typename DerivedP::Scalar,4,3> DIRs;
    DIRs <<
    PD1.row(i),
    -PD1.row(i),
    PD2.row(i),
    -PD2.row(i);

    std::vector<double> a(4);


    double a_combed = atan2(B2.row(i).dot(BIS1_combed.row(i)),B1.row(i).dot(BIS1_combed.row(i)));

    // center on the combed sector center
    for (unsigned j=0;j<4;++j)
    {
      a[j] = atan2(B2.row(i).dot(DIRs.row(j)),B1.row(i).dot(DIRs.row(j))) - a_combed;
      //make it positive by adding some multiple of 2pi
      a[j] += std::ceil (std::max(0., -a[j]) / (igl::PI*2.)) * (igl::PI*2.);
      //take modulo 2pi
      a[j] = fmod(a[j], (igl::PI*2.));
    }
    // now the max is u and the min is v

    int m = std::min_element(a.begin(),a.end())-a.begin();
    int M = std::max_element(a.begin(),a.end())-a.begin();

    assert(
           ((m>=0 && m<=1) && (M>=2 && M<=3))
           ||
           ((m>=2 && m<=3) && (M>=0 && M<=1))
           );

    PD1_combed.row(i) = DIRs.row(m);
    PD2_combed.row(i) = DIRs.row(M);

  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::comb_frame_field<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template void igl::comb_frame_field<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
