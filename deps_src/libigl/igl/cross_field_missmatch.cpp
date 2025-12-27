// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "cross_field_missmatch.h"

#include <cmath>
#include <vector>
#include <deque>
#include <igl/comb_cross_field.h>
#include <igl/per_face_normals.h>
#include <igl/is_border_vertex.h>
#include <igl/vertex_triangle_adjacency.h>
#include <igl/triangle_triangle_adjacency.h>
#include <igl/rotation_matrix_from_directions.h>
#include <igl/PI.h>

namespace igl {
  template <typename DerivedV, typename DerivedF, typename DerivedM>
  class MissMatchCalculator
  {
  public:

    const Eigen::PlainObjectBase<DerivedV> &V;
    const Eigen::PlainObjectBase<DerivedF> &F;
    const Eigen::PlainObjectBase<DerivedV> &PD1;
    const Eigen::PlainObjectBase<DerivedV> &PD2;
    
    DerivedV N;

  private:
    // internal
    std::vector<bool> V_border; // bool
    std::vector<std::vector<int> > VF;
    std::vector<std::vector<int> > VFi;
    
    DerivedF TT;
    DerivedF TTi;


  private:
    ///compute the mismatch between 2 faces
    inline int MissMatchByCross(const int f0,
                         const int f1)
    {
      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir0 = PD1.row(f0);
      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir1 = PD1.row(f1);
      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> n0 = N.row(f0);
      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> n1 = N.row(f1);

      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir1Rot = igl::rotation_matrix_from_directions(n1,n0)*dir1;
      dir1Rot.normalize();

      // TODO: this should be equivalent to the other code below, to check!
      // Compute the angle between the two vectors
      //    double a0 = atan2(dir0.dot(B2.row(f0)),dir0.dot(B1.row(f0)));
      //    double a1 = atan2(dir1Rot.dot(B2.row(f0)),dir1Rot.dot(B1.row(f0)));
      //
      //    double angle_diff = a1-a0;   //VectToAngle(f0,dir1Rot);

      double angle_diff = atan2(dir1Rot.dot(PD2.row(f0)),dir1Rot.dot(PD1.row(f0)));

      //    std::cerr << "Dani: " << dir0(0) << " " << dir0(1) << " " << dir0(2) << " " << dir1Rot(0) << " " << dir1Rot(1) << " " << dir1Rot(2) << " " << angle_diff << std::endl;

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
  inline MissMatchCalculator(const Eigen::PlainObjectBase<DerivedV> &_V,
                      const Eigen::PlainObjectBase<DerivedF> &_F,
                      const Eigen::PlainObjectBase<DerivedV> &_PD1,
                      const Eigen::PlainObjectBase<DerivedV> &_PD2
                      ):
  V(_V),
  F(_F),
  PD1(_PD1),
  PD2(_PD2)
  {
    igl::per_face_normals(V,F,N);
    V_border = igl::is_border_vertex(V,F);
    igl::vertex_triangle_adjacency(V,F,VF,VFi);
    igl::triangle_triangle_adjacency(F,TT,TTi);
  }

  inline void calculateMissmatch(Eigen::PlainObjectBase<DerivedM> &Handle_MMatch)
  {
    Handle_MMatch.setConstant(F.rows(),3,-1);
    for (size_t i=0;i<F.rows();i++)
    {
      for (int j=0;j<3;j++)
      {
        if (((int)i)==TT(i,j) || TT(i,j) == -1)
          Handle_MMatch(i,j)=0;
        else
          Handle_MMatch(i,j) = MissMatchByCross(i,TT(i,j));
      }
    }
  }

};
}
template <typename DerivedV, typename DerivedF, typename DerivedM>
IGL_INLINE void igl::cross_field_missmatch(const Eigen::PlainObjectBase<DerivedV> &V,
                                           const Eigen::PlainObjectBase<DerivedF> &F,
                                           const Eigen::PlainObjectBase<DerivedV> &PD1,
                                           const Eigen::PlainObjectBase<DerivedV> &PD2,
                                           const bool isCombed,
                                           Eigen::PlainObjectBase<DerivedM> &missmatch)
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
  igl::MissMatchCalculator<DerivedV, DerivedF, DerivedM> sf(V, F, PD1_combed, PD2_combed);
  sf.calculateMissmatch(missmatch);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::cross_field_missmatch<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template void igl::cross_field_missmatch<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cross_field_missmatch<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);

#endif
