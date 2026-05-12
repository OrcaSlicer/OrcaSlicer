// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "intrinsic_delaunay_triangulation.h"
#include "is_intrinsic_delaunay.h"
#include "tan_half_angle.h"
#include "unique_edge_map.h"
#include "flip_edge.h"
#include "EPS.h"
#include "IGL_ASSERT.h"
#include <iostream>
#include <queue>
#include <map>

template <
  typename Derivedl_in,
  typename DerivedF_in,
  typename Derivedl,
  typename DerivedF>
IGL_INLINE void igl::intrinsic_delaunay_triangulation(
  const Eigen::MatrixBase<Derivedl_in> & l_in,
  const Eigen::MatrixBase<DerivedF_in> & F_in,
  Eigen::PlainObjectBase<Derivedl> & l,
  Eigen::PlainObjectBase<DerivedF> & F)
{
  typedef Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,2> MatrixX2I;
  typedef Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,1> VectorXI;
  MatrixX2I E,uE;
  VectorXI EMAP;
  std::vector<std::vector<typename DerivedF::Scalar> > uE2E;
  return intrinsic_delaunay_triangulation(l_in,F_in,l,F,E,uE,EMAP,uE2E);
}

template <
  typename Derivedl_in,
  typename DerivedF_in,
  typename Derivedl,
  typename DerivedF,
  typename DerivedE,
  typename DeriveduE,
  typename DerivedEMAP,
  typename uE2EType>
IGL_INLINE void igl::intrinsic_delaunay_triangulation(
  const Eigen::MatrixBase<Derivedl_in> & l_in,
  const Eigen::MatrixBase<DerivedF_in> & F_in,
  Eigen::PlainObjectBase<Derivedl> & l,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedE> & E,
  Eigen::PlainObjectBase<DeriveduE> & uE,
  Eigen::PlainObjectBase<DerivedEMAP> & EMAP,
  std::vector<std::vector<uE2EType> > & uE2E)
{
  igl::unique_edge_map(F_in, E, uE, EMAP, uE2E);
  // We're going to work in place
  l = l_in;
  F = F_in;
  typedef typename DerivedF::Scalar Index;
  typedef typename Derivedl::Scalar Scalar;
  const Index num_faces = F.rows();

  // Vector is faster than queue...
  std::vector<Index> Q;
  Q.reserve(uE2E.size());
  for (size_t uei=0; uei<uE2E.size(); uei++)
  {
    Q.push_back(uei);
  }
  // I tried using a "delaunay_since = iter" flag to avoid duplicates, but there
  // was no speed up.

  while(!Q.empty())
  {
#ifdef IGL_INTRINSIC_DELAUNAY_TRIANGULATION_DEBUG
    // Expensive sanity check
    {
      Eigen::Matrix<bool,Eigen::Dynamic,1> inQ(uE2E.size(),1);
      inQ.setConstant(false);
      for(const auto uei : Q)
      {
        inQ(uei) = true;
      }
      for (Index uei=0; uei<uE2E.size(); uei++)
      {
        if(!inQ(uei) && !is_intrinsic_delaunay(l,uE2E,num_faces,uei))
        {
          std::cout<<"  "<<uei<<" might never be fixed!"<<std::endl;
        }
      }
    }
#endif

    const Index uei = Q.back();
    Q.pop_back();
    if (uE2E[uei].size() == 2)
    {
      if(!is_intrinsic_delaunay(l,uE2E,num_faces,uei))
      {
        // update l just before flipping edge
        //      .        //
        //     ╱|╲       //
        //   a╱ | ╲d     //
        //   ╱  e  ╲     //
        //  ╱   |   ╲    //
        // .----|-f--.   //
        //  ╲   |   ╱    //
        //   ╲  |  ╱     //
        //   b╲α|δ╱c     //
        //     ╲|╱       //
        //      .        //
        // Annotated from flip_edge:
        // Edge to flip [v1,v2] --> [v3,v4]
        // Before:
        // F(f1,:) = [v1,v2,v4] // in some cyclic order
        // F(f2,:) = [v1,v3,v2] // in some cyclic order
        // After:
        // F(f1,:) = [v1,v3,v4] // in *this* order
        // F(f2,:) = [v2,v4,v3] // in *this* order
        //
        //          v1                 v1
        //          ╱|╲                ╱ ╲
        //        c╱ | ╲b            c╱f1 ╲b
        //     v3 ╱f2|f1╲ v4  =>  v3 ╱__f__╲ v4
        //        ╲  e  /            ╲ f2  /
        //        d╲ | /a            d╲   /a
        //          ╲|/                ╲ /
        //          v2                 v2
        //
        // Compute intrinsic length of oppposite edge
        IGL_ASSERT(uE2E[uei].size() == 2 && "edge should have 2 incident faces");
        const Index f1 = uE2E[uei][0]%num_faces;
        const Index f2 = uE2E[uei][1]%num_faces;
        const Index c1 = uE2E[uei][0]/num_faces;
        const Index c2 = uE2E[uei][1]/num_faces;
        IGL_ASSERT(c1 < 3);
        IGL_ASSERT(c2 < 3);
        IGL_ASSERT(f1 != f2);
        const Index v1 = F(f1, (c1+1)%3);
        const Index v2 = F(f1, (c1+2)%3);
        IGL_ASSERT(F(f2, (c2+2)%3) == v1);
        IGL_ASSERT(F(f2, (c2+1)%3) == v2);
        IGL_ASSERT( std::abs(l(f1,c1)-l(f2,c2)) < igl::EPS<Scalar>() );
        const Scalar e = l(f1,c1);
        const Scalar a = l(f1,(c1+1)%3);
        const Scalar b = l(f1,(c1+2)%3);
        const Scalar c = l(f2,(c2+1)%3);
        const Scalar d = l(f2,(c2+2)%3);
        // tan(α/2)
        const Scalar tan_a_2= tan_half_angle(a,b,e);
        // tan(δ/2)
        const Scalar tan_d_2 = tan_half_angle(d,e,c);
        // tan((α+δ)/2)
        const Scalar tan_a_d_2 = (tan_a_2 + tan_d_2)/(1.0-tan_a_2*tan_d_2);
        // cos(α+δ)
        const Scalar cos_a_d =
          (1.0 - tan_a_d_2*tan_a_d_2)/(1.0+tan_a_d_2*tan_a_d_2);
        const Scalar f = sqrt(b*b + c*c - 2.0*b*c*cos_a_d);
        l(f1,0) = f;
        l(f1,1) = b;
        l(f1,2) = c;
        l(f2,0) = f;
        l(f2,1) = d;
        l(f2,2) = a;
        // Important to grab these indices _before_ calling flip_edges (they
        // will be correct after)
        const size_t e_24 = f1 + ((c1 + 1) % 3) * num_faces;
        const size_t e_41 = f1 + ((c1 + 2) % 3) * num_faces;
        const size_t e_13 = f2 + ((c2 + 1) % 3) * num_faces;
        const size_t e_32 = f2 + ((c2 + 2) % 3) * num_faces;
        const size_t ue_24 = EMAP(e_24);
        const size_t ue_41 = EMAP(e_41);
        const size_t ue_13 = EMAP(e_13);
        const size_t ue_32 = EMAP(e_32);
        flip_edge(F, E, uE, EMAP, uE2E, uei);
        Q.push_back(ue_24);
        Q.push_back(ue_41);
        Q.push_back(ue_13);
        Q.push_back(ue_32);
      }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::intrinsic_delaunay_triangulation<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template void igl::intrinsic_delaunay_triangulation<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template void igl::intrinsic_delaunay_triangulation<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, int>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&);
// generated by autoexplicit.sh
template void igl::intrinsic_delaunay_triangulation<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
