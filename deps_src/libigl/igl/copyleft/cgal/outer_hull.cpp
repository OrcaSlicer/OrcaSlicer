// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "outer_hull.h"
#include "extract_cells.h"
#include "remesh_self_intersections.h"
#include "assign.h"
#include "../../remove_unreferenced.h"

#include <CGAL/AABB_tree.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_triangle_primitive.h>
#include <CGAL/intersections.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedHV,
  typename DerivedHF,
  typename DerivedJ,
  typename Derivedflip>
IGL_INLINE void igl::copyleft::cgal::outer_hull(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedHV> & HV,
  Eigen::PlainObjectBase<DerivedHF> & HF,
  Eigen::PlainObjectBase<DerivedJ> & J,
  Eigen::PlainObjectBase<Derivedflip> & flip)
{
  // Exact types
  typedef CGAL::Epeck Kernel;
  typedef Kernel::FT ExactScalar;
  typedef
    Eigen::Matrix<
    ExactScalar,
    Eigen::Dynamic,
    Eigen::Dynamic,
    DerivedHV::IsRowMajor>
      MatrixXES;
  // Remesh self-intersections
  MatrixXES Vr;
  DerivedHF Fr;
  DerivedJ Jr;
  {
    RemeshSelfIntersectionsParam params;
    params.stitch_all = true;
    Eigen::VectorXi I;
    Eigen::MatrixXi IF;
    remesh_self_intersections(V, F, params, Vr, Fr, IF, Jr, I);
    // Merge coinciding vertices into non-manifold vertices.
    std::for_each(Fr.data(), Fr.data()+Fr.size(),
      [&I](typename DerivedHF::Scalar& a) { a=I[a]; });
      // Remove unreferenced vertices.
    Eigen::VectorXi UIM;
    remove_unreferenced(MatrixXES(Vr),DerivedHF(Fr), Vr, Fr, UIM);
  }
  // Extract cells for each face
  Eigen::MatrixXi C;
  extract_cells(Vr,Fr,C);
  // Extract faces on ambient cell
  int num_outer = 0;
  for(int i = 0;i<C.rows();i++)
  {
    num_outer += ( C(i,0) == 0 || C(i,1) == 0 ) ? 1 : 0;
  }
  HF.resize(num_outer,3);
  J.resize(num_outer,1);
  flip.resize(num_outer,1);
  {
    int h = 0;
    for(int i = 0;i<C.rows();i++)
    {
      if(C(i,0)==0)
      {
        HF.row(h) = Fr.row(i);
        J(h) = Jr(i);
        flip(h) = false;
        h++;
      }else if(C(i,1) == 0)
      {
        HF.row(h) = Fr.row(i).reverse();
        J(h) = Jr(i);
        flip(h) = true;
        h++;
      }
    }
    assert(h == num_outer);
  }
  // Remove unreferenced vertices and re-index faces
  {
    // Cast to output type
    DerivedHV Vr_cast;
    assign(Vr,Vr_cast);
    Eigen::VectorXi I;
    remove_unreferenced(Vr_cast,DerivedHF(HF),HV,HF,I);
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::copyleft::cgal::outer_hull<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#ifdef WIN32
#endif
#endif
