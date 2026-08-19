// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "arap_linear_block.h"
#include "verbose.h"
#include "cotmatrix_entries.h"
#include <Eigen/Dense>

template <typename MatV, typename MatF, typename MatK>
IGL_INLINE void igl::arap_linear_block(
  const MatV & V,
  const MatF & F,
  const int d,
  const igl::ARAPEnergyType energy,
  MatK & Kd)
{
  switch(energy)
  {
    case ARAP_ENERGY_TYPE_SPOKES:
      return igl::arap_linear_block_spokes(V,F,d,Kd);
      break;
    case ARAP_ENERGY_TYPE_SPOKES_AND_RIMS:
      return igl::arap_linear_block_spokes_and_rims(V,F,d,Kd);
      break;
    case ARAP_ENERGY_TYPE_ELEMENTS:
      return igl::arap_linear_block_elements(V,F,d,Kd);
      break;
    default:
      verbose("Unsupported energy type: %d\n",energy);
      assert(false);
  }
}


template <typename MatV, typename MatF, typename MatK>
IGL_INLINE void igl::arap_linear_block_spokes(
  const MatV & V,
  const MatF & F,
  const int d,
  MatK & Kd)
{
  typedef typename MatK::Scalar Scalar;

  using namespace std;
  using namespace Eigen;
  // simplex size (3: triangles, 4: tetrahedra)
  int simplex_size = F.cols();
  // Number of elements
  int m = F.rows();
  // Temporary output
  Matrix<int,Dynamic,2> edges;
  Kd.resize(V.rows(), V.rows());
  vector<Triplet<Scalar> > Kd_IJV;
  if(simplex_size == 3)
  {
    // triangles
    Kd.reserve(7*V.rows());
    Kd_IJV.reserve(7*V.rows());
    edges.resize(3,2);
    edges << 
      1,2,
      2,0,
      0,1;
  }else if(simplex_size == 4)
  {
    // tets
    Kd.reserve(17*V.rows());
    Kd_IJV.reserve(17*V.rows());
    edges.resize(6,2);
    edges << 
      1,2,
      2,0,
      0,1,
      3,0,
      3,1,
      3,2;
  }
  // gather cotangent weights
  Matrix<Scalar,Dynamic,Dynamic> C;
  cotmatrix_entries(V,F,C);
  // should have weights for each edge
  assert(C.cols() == edges.rows());
  // loop over elements
  for(int i = 0;i<m;i++)
  {
    // loop over edges of element
    for(int e = 0;e<edges.rows();e++)
    {
      int source = F(i,edges(e,0));
      int dest = F(i,edges(e,1));
      double v = 0.5*C(i,e)*(V(source,d)-V(dest,d));
      Kd_IJV.push_back(Triplet<Scalar>(source,dest,v));
      Kd_IJV.push_back(Triplet<Scalar>(dest,source,-v));
      Kd_IJV.push_back(Triplet<Scalar>(source,source,v));
      Kd_IJV.push_back(Triplet<Scalar>(dest,dest,-v));
    }
  }
  Kd.setFromTriplets(Kd_IJV.begin(),Kd_IJV.end());
  Kd.makeCompressed();
}

template <typename MatV, typename MatF, typename MatK>
IGL_INLINE void igl::arap_linear_block_spokes_and_rims(
  const MatV & V,
  const MatF & F,
  const int d,
  MatK & Kd)
{
  typedef typename MatK::Scalar Scalar;

  using namespace std;
  using namespace Eigen;
  // simplex size (3: triangles, 4: tetrahedra)
  int simplex_size = F.cols();
  // Number of elements
  int m = F.rows();
  // Temporary output
  Kd.resize(V.rows(), V.rows());
  vector<Triplet<Scalar> > Kd_IJV;
  Matrix<int,Dynamic,2> edges;
  if(simplex_size == 3)
  {
    // triangles
    Kd.reserve(7*V.rows());
    Kd_IJV.reserve(7*V.rows());
    edges.resize(3,2);
    edges << 
      1,2,
      2,0,
      0,1;
  }else if(simplex_size == 4)
  {
    // tets
    Kd.reserve(17*V.rows());
    Kd_IJV.reserve(17*V.rows());
    edges.resize(6,2);
    edges << 
      1,2,
      2,0,
      0,1,
      3,0,
      3,1,
      3,2;
    // Not implemented yet for tets
    assert(false);
  }
  // gather cotangent weights
  Matrix<Scalar,Dynamic,Dynamic> C;
  cotmatrix_entries(V,F,C);
  // should have weights for each edge
  assert(C.cols() == edges.rows());
  // loop over elements
  for(int i = 0;i<m;i++)
  {
    // loop over edges of element
    for(int e = 0;e<edges.rows();e++)
    {
      int source = F(i,edges(e,0));
      int dest = F(i,edges(e,1));
      double v = C(i,e)*(V(source,d)-V(dest,d))/3.0;
      // loop over edges again
      for(int f = 0;f<edges.rows();f++)
      {
        int Rs = F(i,edges(f,0));
        int Rd = F(i,edges(f,1));
        if(Rs == source && Rd == dest)
        {
          Kd_IJV.push_back(Triplet<Scalar>(Rs,Rd,v));
          Kd_IJV.push_back(Triplet<Scalar>(Rd,Rs,-v));
        }else if(Rd == source)
        {
          Kd_IJV.push_back(Triplet<Scalar>(Rd,Rs,v));
        }else if(Rs == dest)
        {
          Kd_IJV.push_back(Triplet<Scalar>(Rs,Rd,-v));
        }
      }
      Kd_IJV.push_back(Triplet<Scalar>(source,source,v));
      Kd_IJV.push_back(Triplet<Scalar>(dest,dest,-v));
    }
  }
  Kd.setFromTriplets(Kd_IJV.begin(),Kd_IJV.end());
  Kd.makeCompressed();
}

template <typename MatV, typename MatF, typename MatK>
IGL_INLINE void igl::arap_linear_block_elements(
  const MatV & V,
  const MatF & F,
  const int d,
  MatK & Kd)
{
  typedef typename MatK::Scalar Scalar;
  using namespace std;
  using namespace Eigen;
  // simplex size (3: triangles, 4: tetrahedra)
  int simplex_size = F.cols();
  // Number of elements
  int m = F.rows();
  // Temporary output
  Kd.resize(V.rows(), F.rows());
  vector<Triplet<Scalar> > Kd_IJV;
  Matrix<int,Dynamic,2> edges;
  if(simplex_size == 3)
  {
    // triangles
    Kd.reserve(7*V.rows());
    Kd_IJV.reserve(7*V.rows());
    edges.resize(3,2);
    edges << 
      1,2,
      2,0,
      0,1;
  }else if(simplex_size == 4)
  {
    // tets
    Kd.reserve(17*V.rows());
    Kd_IJV.reserve(17*V.rows());
    edges.resize(6,2);
    edges << 
      1,2,
      2,0,
      0,1,
      3,0,
      3,1,
      3,2;
  }
  // gather cotangent weights
  Matrix<Scalar,Dynamic,Dynamic> C;
  cotmatrix_entries(V,F,C);
  // should have weights for each edge
  assert(C.cols() == edges.rows());
  // loop over elements
  for(int i = 0;i<m;i++)
  {
    // loop over edges of element
    for(int e = 0;e<edges.rows();e++)
    {
      int source = F(i,edges(e,0));
      int dest = F(i,edges(e,1));
      double v = C(i,e)*(V(source,d)-V(dest,d));
      Kd_IJV.push_back(Triplet<Scalar>(source,i,v));
      Kd_IJV.push_back(Triplet<Scalar>(dest,i,-v));
    }
  }
  Kd.setFromTriplets(Kd_IJV.begin(),Kd_IJV.end());
  Kd.makeCompressed();
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::arap_linear_block<Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >, Eigen::SparseMatrix<double, 0, int> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, igl::ARAPEnergyType, Eigen::SparseMatrix<double, 0, int>&);
template void igl::arap_linear_block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::SparseMatrix<double, 0, int> >(Eigen::Matrix<double, -1, -1, 0, -1, -1> const&, Eigen::Matrix<int, -1, -1, 0, -1, -1> const&, int, igl::ARAPEnergyType, Eigen::SparseMatrix<double, 0, int>&);
#endif
