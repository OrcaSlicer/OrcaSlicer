// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "lbs_matrix.h"

IGL_INLINE void igl::lbs_matrix(
  const Eigen::MatrixXd & V, 
  const Eigen::MatrixXd & W,
  Eigen::MatrixXd & M)
{
  using namespace Eigen;
  // Number of dimensions
  const int dim = V.cols();
  // Number of model points
  const int n = V.rows();
  // Number of skinning transformations/weights
  const int m = W.cols();

  // Assumes that first n rows of weights correspond to V
  assert(W.rows() >= n);

  M.resize(n,(dim+1)*m);
  for(int j = 0;j<m;j++)
  {
    VectorXd Wj = W.block(0,j,V.rows(),1);
    for(int i = 0;i<(dim+1);i++)
    {
      if(i<dim)
      {
        M.col(i + j*(dim+1)) = 
          Wj.cwiseProduct(V.col(i));
      }else
      {
        M.col(i + j*(dim+1)).array() = W.block(0,j,V.rows(),1).array();
      }
    }
  }
}

IGL_INLINE void igl::lbs_matrix_column(
  const Eigen::MatrixXd & V, 
  const Eigen::MatrixXd & W,
  Eigen::SparseMatrix<double>& M)
{
  // number of mesh vertices
  int n = V.rows();
  assert(n == W.rows());
  // dimension of mesh
  int dim = V.cols();
  // number of handles
  int m = W.cols();

  M.resize(n*dim,m*dim*(dim+1));

  // loop over coordinates of mesh vertices
  for(int x = 0; x < dim; x++)
  {
    // loop over mesh vertices
    for(int j = 0; j < n; j++)
    {
      // loop over handles
      for(int i = 0; i < m; i++)
      {
        // loop over cols of affine transformations
        for(int c = 0; c < (dim+1); c++)
        {
          double value = W(j,i);
          if(c<dim)
          {
            value *= V(j,c);
          }
          M.insert(x*n + j,x*m + c*m*dim + i) = value;
        }
      }
    }
  }

  M.makeCompressed();
}

IGL_INLINE void igl::lbs_matrix_column(
  const Eigen::MatrixXd & V, 
  const Eigen::MatrixXd & W,
  Eigen::MatrixXd & M)
{
  // number of mesh vertices
  int n = V.rows();
  assert(n == W.rows());
  // dimension of mesh
  int dim = V.cols();
  // number of handles
  int m = W.cols();
  M.resize(n*dim,m*dim*(dim+1));

  // loop over coordinates of mesh vertices
  for(int x = 0; x < dim; x++)
  {
    // loop over mesh vertices
    for(int j = 0; j < n; j++)
    {
      // loop over handles
      for(int i = 0; i < m; i++)
      {
        // loop over cols of affine transformations
        for(int c = 0; c < (dim+1); c++)
        {
          double value = W(j,i);
          if(c<dim)
          {
            value *= V(j,c);
          }
          M(x*n + j,x*m + c*m*dim + i) = value;
        }
      }
    }
  }
}

IGL_INLINE void igl::lbs_matrix_column(
  const Eigen::MatrixXd & V, 
  const Eigen::MatrixXd & W,
  const Eigen::MatrixXi & WI,
  Eigen::SparseMatrix<double>& M)
{
  // number of mesh vertices
  int n = V.rows();
  assert(n == W.rows());
  assert(n == WI.rows());
  // dimension of mesh
  int dim = V.cols();
  // number of handles
  int m = WI.maxCoeff()+1;
  // max number of influencing handles
  int k = W.cols();
  assert(k == WI.cols());

  M.resize(n*dim,m*dim*(dim+1));

  // loop over coordinates of mesh vertices
  for(int x = 0; x < dim; x++)
  {
    // loop over mesh vertices
    for(int j = 0; j < n; j++)
    {
      // loop over handles
      for(int i = 0; i < k; i++)
      {
        // loop over cols of affine transformations
        for(int c = 0; c < (dim+1); c++)
        {
          double value = W(j,i);
          if(c<dim)
          {
            value *= V(j,c);
          }
          if(value != 0)
          {
            M.insert(x*n + j,x*m + c*m*dim + WI(j,i)) = value;
          }
        }
      }
    }
  }

  M.makeCompressed();
}


IGL_INLINE void igl::lbs_matrix_column(
  const Eigen::MatrixXd & V, 
  const Eigen::MatrixXd & W,
  const Eigen::MatrixXi & WI,
  Eigen::MatrixXd & M)
{
  // Cheapskate wrapper
  using namespace Eigen;
  SparseMatrix<double> sM;
  lbs_matrix_column(V,W,WI,sM);
  M = MatrixXd(sM);
}
