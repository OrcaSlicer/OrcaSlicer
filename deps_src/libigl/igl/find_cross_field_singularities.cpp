// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "find_cross_field_singularities.h"

#include <vector>
#include "cross_field_mismatch.h"
#include "is_border_vertex.h"
#include "vertex_triangle_adjacency.h"
#include "is_border_vertex.h"


template <typename DerivedV, typename DerivedF, typename DerivedM, typename DerivedO>
IGL_INLINE void igl::find_cross_field_singularities(const Eigen::MatrixBase<DerivedV> &V,
                                                    const Eigen::MatrixBase<DerivedF> &F,
                                                    const Eigen::MatrixBase<DerivedM> &Handle_MMatch,
                                                    Eigen::PlainObjectBase<DerivedO> &isSingularity,
                                                    Eigen::PlainObjectBase<DerivedO> &singularityIndex)
{
  std::vector<bool> V_border = igl::is_border_vertex(F);

  std::vector<std::vector<int> > VF;
  std::vector<std::vector<int> > VFi;
  igl::vertex_triangle_adjacency(V,F,VF,VFi);


  isSingularity.setZero(V.rows(),1);
  singularityIndex.setZero(V.rows(),1);
  for (unsigned int vid=0;vid<V.rows();vid++)
  {
    ///check that is on border..
    if (V_border[vid])
      continue;

    int mismatch=0;
    for (unsigned int i=0;i<VF[vid].size();i++)
    {
      // look for the vertex
      int j=-1;
      for (unsigned z=0; z<3; ++z)
        if (F(VF[vid][i],z) == vid)
          j=z;
      assert(j!=-1);

      mismatch+=Handle_MMatch(VF[vid][i],j);
    }
    mismatch=mismatch%4;

    isSingularity(vid)=(mismatch!=0);
    singularityIndex(vid)=mismatch;
  }


}

template <typename DerivedV, typename DerivedF, typename DerivedO>
IGL_INLINE void igl::find_cross_field_singularities(const Eigen::MatrixBase<DerivedV> &V,
                                                    const Eigen::MatrixBase<DerivedF> &F,
                                                    const Eigen::MatrixBase<DerivedV> &PD1,
                                                    const Eigen::MatrixBase<DerivedV> &PD2,
                                                    Eigen::PlainObjectBase<DerivedO> &isSingularity,
                                                    Eigen::PlainObjectBase<DerivedO> &singularityIndex,
                                                    bool isCombed)
{
  Eigen::Matrix<typename DerivedF::Scalar, Eigen::Dynamic, 3> Handle_MMatch;

  igl::cross_field_mismatch(V, F, PD1, PD2, isCombed, Handle_MMatch);
  igl::find_cross_field_singularities(V, F, Handle_MMatch, isSingularity, singularityIndex);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::find_cross_field_singularities<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::find_cross_field_singularities<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3>> const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3>> const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3>> const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3>> const &, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> &,
                                                                                                                                                                    Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> &, bool);
template void igl::find_cross_field_singularities<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::find_cross_field_singularities<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> &, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> &, bool);
template void igl::find_cross_field_singularities<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::find_cross_field_singularities<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
