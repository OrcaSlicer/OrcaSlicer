// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "massmatrix_intrinsic.h"
#include "edge_lengths.h"
#include "sparse.h"
#include "doublearea.h"
#include "repmat.h"
#include <Eigen/Geometry>
#include <iostream>
#include <cassert>

template <typename Derivedl, typename DerivedF, typename Scalar>
IGL_INLINE void igl::massmatrix_intrinsic(
  const Eigen::MatrixBase<Derivedl> & l, 
  const Eigen::MatrixBase<DerivedF> & F, 
  const MassMatrixType type,
  Eigen::SparseMatrix<Scalar>& M)
{
  const int n = F.maxCoeff()+1;
  return massmatrix_intrinsic(l,F,type,n,M);
}

template <typename Derivedl, typename DerivedF, typename Scalar>
IGL_INLINE void igl::massmatrix_intrinsic(
  const Eigen::MatrixBase<Derivedl> & l, 
  const Eigen::MatrixBase<DerivedF> & F, 
  const MassMatrixType type,
  const int n,
  Eigen::SparseMatrix<Scalar>& M)
{
  using namespace Eigen;
  using namespace std;
  MassMatrixType eff_type = type;
  const int m = F.rows();
  const int simplex_size = F.cols();
  // Use voronoi of for triangles by default, otherwise barycentric
  if(type == MASSMATRIX_TYPE_DEFAULT)
  {
    eff_type = (simplex_size == 3?MASSMATRIX_TYPE_VORONOI:MASSMATRIX_TYPE_BARYCENTRIC);
  }
  assert(F.cols() == 3 && "only triangles supported");
  Matrix<Scalar,Dynamic,1> dblA;
  doublearea(l,0.,dblA);
  Matrix<typename DerivedF::Scalar,Dynamic,1> MI;
  Matrix<typename DerivedF::Scalar,Dynamic,1> MJ;
  Matrix<Scalar,Dynamic,1> MV;

  switch(eff_type)
  {
    case MASSMATRIX_TYPE_BARYCENTRIC:
      // diagonal entries for each face corner
      MI.resize(m*3,1); MJ.resize(m*3,1); MV.resize(m*3,1);
      MI.block(0*m,0,m,1) = F.col(0);
      MI.block(1*m,0,m,1) = F.col(1);
      MI.block(2*m,0,m,1) = F.col(2);
      MJ = MI;
      repmat(dblA,3,1,MV);
      MV.array() /= 6.0;
      break;
    case MASSMATRIX_TYPE_VORONOI:
      {
        // diagonal entries for each face corner
        // http://www.alecjacobson.com/weblog/?p=874
        MI.resize(m*3,1); MJ.resize(m*3,1); MV.resize(m*3,1);
        MI.block(0*m,0,m,1) = F.col(0);
        MI.block(1*m,0,m,1) = F.col(1);
        MI.block(2*m,0,m,1) = F.col(2);
        MJ = MI;

        // Holy shit this needs to be cleaned up and optimized
        Matrix<Scalar,Dynamic,3> cosines(m,3);
        cosines.col(0) = 
          (l.col(2).array().pow(2)+l.col(1).array().pow(2)-l.col(0).array().pow(2))/(l.col(1).array()*l.col(2).array()*2.0);
        cosines.col(1) = 
          (l.col(0).array().pow(2)+l.col(2).array().pow(2)-l.col(1).array().pow(2))/(l.col(2).array()*l.col(0).array()*2.0);
        cosines.col(2) = 
          (l.col(1).array().pow(2)+l.col(0).array().pow(2)-l.col(2).array().pow(2))/(l.col(0).array()*l.col(1).array()*2.0);
        Matrix<Scalar,Dynamic,3> barycentric = cosines.array() * l.array();
        // Replace this: normalize_row_sums(barycentric,barycentric);
        barycentric  = (barycentric.array().colwise() / barycentric.array().rowwise().sum()).eval();

        Matrix<Scalar,Dynamic,3> partial = barycentric;
        partial.col(0).array() *= dblA.array() * 0.5;
        partial.col(1).array() *= dblA.array() * 0.5;
        partial.col(2).array() *= dblA.array() * 0.5;
        Matrix<Scalar,Dynamic,3> quads(partial.rows(),partial.cols());
        quads.col(0) = (partial.col(1)+partial.col(2))*0.5;
        quads.col(1) = (partial.col(2)+partial.col(0))*0.5;
        quads.col(2) = (partial.col(0)+partial.col(1))*0.5;

        quads.col(0) = (cosines.col(0).array()<0).select( 0.25*dblA,quads.col(0));
        quads.col(1) = (cosines.col(0).array()<0).select(0.125*dblA,quads.col(1));
        quads.col(2) = (cosines.col(0).array()<0).select(0.125*dblA,quads.col(2));

        quads.col(0) = (cosines.col(1).array()<0).select(0.125*dblA,quads.col(0));
        quads.col(1) = (cosines.col(1).array()<0).select(0.25*dblA,quads.col(1));
        quads.col(2) = (cosines.col(1).array()<0).select(0.125*dblA,quads.col(2));

        quads.col(0) = (cosines.col(2).array()<0).select(0.125*dblA,quads.col(0));
        quads.col(1) = (cosines.col(2).array()<0).select(0.125*dblA,quads.col(1));
        quads.col(2) = (cosines.col(2).array()<0).select( 0.25*dblA,quads.col(2));

        MV.block(0*m,0,m,1) = quads.col(0);
        MV.block(1*m,0,m,1) = quads.col(1);
        MV.block(2*m,0,m,1) = quads.col(2);
        
        break;
      }
    case MASSMATRIX_TYPE_FULL:
      MI.resize(m*9,1); MJ.resize(m*9,1); MV.resize(m*9,1);
      // indicies and values of the element mass matrix entries in the order
      // (0,1),(1,0),(1,2),(2,1),(2,0),(0,2),(0,0),(1,1),(2,2);
      MI<<F.col(0),F.col(1),F.col(1),F.col(2),F.col(2),F.col(0),F.col(0),F.col(1),F.col(2);
      MJ<<F.col(1),F.col(0),F.col(2),F.col(1),F.col(0),F.col(2),F.col(0),F.col(1),F.col(2);
      repmat(dblA,9,1,MV);
      MV.block(0*m,0,6*m,1) /= 24.0;
      MV.block(6*m,0,3*m,1) /= 12.0;
      break;
    default:
      assert(false && "Unknown Mass matrix eff_type");
  }
  sparse(MI,MJ,MV,n,n,M);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::massmatrix_intrinsic<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 4, 0, -1, 4>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 4, 0, -1, 4> > const&, igl::MassMatrixType, Eigen::SparseMatrix<double, 0, int>&);
template void igl::massmatrix_intrinsic<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::MassMatrixType, Eigen::SparseMatrix<double, 0, int>&);
template void igl::massmatrix_intrinsic<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, igl::MassMatrixType, Eigen::SparseMatrix<double, 0, int>&);
template void igl::massmatrix_intrinsic<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, igl::MassMatrixType, Eigen::SparseMatrix<double, 0, int>&);
#endif
