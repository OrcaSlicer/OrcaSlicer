// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "cross_field_mismatch.h"

#include <cmath>
#include <vector>
#include <deque>
#include "comb_cross_field.h"
#include "per_face_normals.h"
#include "is_border_vertex.h"
#include "vertex_triangle_adjacency.h"
#include "triangle_triangle_adjacency.h"
#include "rotation_matrix_from_directions.h"
#include "PI.h"
#include "PlainMatrix.h"

namespace igl {
  template <typename DerivedV, typename DerivedF, typename DerivedM>
  class MismatchCalculator
  {
  public:

    const Eigen::MatrixBase<DerivedV> &V;
    const Eigen::MatrixBase<DerivedF> &F;
    const Eigen::MatrixBase<DerivedV> &PD1;
    const Eigen::MatrixBase<DerivedV> &PD2;
    
    PlainMatrix<DerivedV> N;

  private:
    // internal
    std::vector<bool> V_border; // bool
    std::vector<std::vector<int> > VF;
    std::vector<std::vector<int> > VFi;
    
    PlainMatrix<DerivedF> TT;
    PlainMatrix<DerivedF> TTi;


  private:
    ///compute the mismatch between 2 faces
    inline int mismatchByCross(const int f0,
                               const int f1)
    {
      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir1 = PD1.row(f1);
      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> n0 = N.row(f0);
      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> n1 = N.row(f1);

      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir1Rot = igl::rotation_matrix_from_directions(n1,n0)*dir1;
      dir1Rot.normalize();

      double angle_diff = atan2(dir1Rot.dot(PD2.row(f0)),dir1Rot.dot(PD1.row(f0)));

      double step=igl::PI/2.0;
      int i=(int)std::floor((angle_diff/step)+0.5);
      int k=0;
      if (i>=0)
        k=i%4;
      else
        k=(-(3*i))%4;
      return k;
    }


public:
  inline MismatchCalculator(const Eigen::MatrixBase<DerivedV> &_V,
                            const Eigen::MatrixBase<DerivedF> &_F,
                            const Eigen::MatrixBase<DerivedV> &_PD1,
                            const Eigen::MatrixBase<DerivedV> &_PD2):
  V(_V),
  F(_F),
  PD1(_PD1),
  PD2(_PD2)
  {
    igl::per_face_normals(V,F,N);
    V_border = igl::is_border_vertex(F);
    igl::vertex_triangle_adjacency(V,F,VF,VFi);
    igl::triangle_triangle_adjacency(F,TT,TTi);
  }

  inline void calculateMismatch(Eigen::PlainObjectBase<DerivedM> &Handle_MMatch)
  {
    Handle_MMatch.setConstant(F.rows(),3,-1);
    for (size_t i=0;i<F.rows();i++)
    {
      for (int j=0;j<3;j++)
      {
        if (((int)i)==TT(i,j) || TT(i,j) == -1)
          Handle_MMatch(i,j)=0;
        else
          Handle_MMatch(i,j) = mismatchByCross(i, TT(i, j));
      }
    }
  }

};
}
template <typename DerivedV, typename DerivedF, typename DerivedM>
IGL_INLINE void igl::cross_field_mismatch(const Eigen::MatrixBase<DerivedV> &V,
                                          const Eigen::MatrixBase<DerivedF> &F,
                                          const Eigen::MatrixBase<DerivedV> &PD1,
                                          const Eigen::MatrixBase<DerivedV> &PD2,
                                          const bool isCombed,
                                          Eigen::PlainObjectBase<DerivedM> &mismatch)
{
  DerivedV PD1_combed;
  DerivedV PD2_combed;

  if (!isCombed)
    igl::comb_cross_field(V,F,PD1,PD2,PD1_combed,PD2_combed);
  else
  {
    PD1_combed = PD1;
    PD2_combed = PD2;
  }
  igl::MismatchCalculator<DerivedV, DerivedF, DerivedM> sf(V, F, PD1_combed, PD2_combed);
  sf.calculateMismatch(mismatch);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::cross_field_mismatch<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const &,  Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const &,  Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const &, const bool,  Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > &);
template void igl::cross_field_mismatch<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >( Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const &, const bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > &);
template void igl::cross_field_mismatch<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const &, const bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > &);

#endif
