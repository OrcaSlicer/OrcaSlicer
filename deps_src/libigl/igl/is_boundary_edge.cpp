// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "is_boundary_edge.h"
#include "unique_rows.h"
#include "sort.h"

template <
  typename DerivedF,
  typename DerivedE,
  typename DerivedB>
void igl::is_boundary_edge(
  const Eigen::PlainObjectBase<DerivedE> & E,
  const Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedB> & B)
{
  using namespace Eigen;
  using namespace std;
  // Should be triangles
  assert(F.cols() == 3);
  // Should be edges
  assert(E.cols() == 2);
  // number of faces
  const int m = F.rows();
  // Collect all directed edges after E
  MatrixXi EallE(E.rows()+3*m,2);
  EallE.block(0,0,E.rows(),E.cols()) = E;
  for(int e = 0;e<3;e++)
  {
    for(int f = 0;f<m;f++)
    {
      for(int c = 0;c<2;c++)
      {
        // 12 20 01
        EallE(E.rows()+m*e+f,c) = F(f,(c+1+e)%3);
      }
    }
  }
  // sort directed edges into undirected edges
  MatrixXi sorted_EallE,_;
  sort(EallE,2,true,sorted_EallE,_);
  // Determine unique undirected edges E and map to directed edges EMAP
  MatrixXi uE;
  VectorXi EMAP;
  unique_rows(sorted_EallE,uE,_,EMAP);
  // Counts of occurrences
  VectorXi N = VectorXi::Zero(uE.rows());
  for(int e = 0;e<EMAP.rows();e++)
  {
    N(EMAP(e))++;
  }
  B.resize(E.rows());
  // Look of occurrences of 2: one for original and another for boundary
  for(int e = 0;e<E.rows();e++)
  {
    B(e) = (N(EMAP(e)) == 2);
  }
}


template <
  typename DerivedF,
  typename DerivedE,
  typename DerivedB,
  typename DerivedEMAP>
void igl::is_boundary_edge(
  const Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedB> & B,
  Eigen::PlainObjectBase<DerivedE> & E,
  Eigen::PlainObjectBase<DerivedEMAP> & EMAP)
{
  using namespace Eigen;
  using namespace std;
  // Should be triangles
  assert(F.cols() == 3);
  // number of faces
  const int m = F.rows();
  // Collect all directed edges after E
  MatrixXi allE(3*m,2);
  for(int e = 0;e<3;e++)
  {
    for(int f = 0;f<m;f++)
    {
      for(int c = 0;c<2;c++)
      {
        // 12 20 01
        allE(m*e+f,c) = F(f,(c+1+e)%3);
      }
    }
  }
  // sort directed edges into undirected edges
  MatrixXi sorted_allE,_;
  sort(allE,2,true,sorted_allE,_);
  // Determine unique undirected edges E and map to directed edges EMAP
  unique_rows(sorted_allE,E,_,EMAP);
  // Counts of occurrences
  VectorXi N = VectorXi::Zero(E.rows());
  for(int e = 0;e<EMAP.rows();e++)
  {
    N(EMAP(e))++;
  }
  B.resize(E.rows());
  // Look of occurrences of 1
  for(int e = 0;e<E.rows();e++)
  {
    B(e) = N(e) == 1;
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation:
template void igl::is_boundary_edge<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::is_boundary_edge<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<bool, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<bool, -1, 1, 0, -1, 1> >&);
template void igl::is_boundary_edge<Eigen::Matrix<unsigned int, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<bool, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<unsigned int, -1, -1, 1, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<bool, -1, 1, 0, -1, 1> >&);
template void igl::is_boundary_edge<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<bool, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<bool, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::is_boundary_edge<Eigen::Matrix<unsigned int, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<bool, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<unsigned int, -1, -1, 1, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<bool, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::is_boundary_edge<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
