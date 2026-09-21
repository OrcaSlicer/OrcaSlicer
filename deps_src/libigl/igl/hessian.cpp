// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
//  and Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "hessian.h"
#include <vector>

#include "grad.h"
#include "igl/doublearea.h"
#include "igl/repdiag.h"



template <typename DerivedV, typename DerivedF, typename Scalar>
IGL_INLINE void igl::hessian(
                             const Eigen::MatrixBase<DerivedV> & V,
                             const Eigen::MatrixBase<DerivedF> & F,
                             Eigen::SparseMatrix<Scalar>& H)
{
    typedef typename DerivedV::Scalar denseScalar;
    typedef typename Eigen::Matrix<denseScalar, Eigen::Dynamic, 1> VecXd;
    typedef typename Eigen::SparseMatrix<Scalar> SparseMat;
    typedef typename Eigen::DiagonalMatrix
                       <Scalar, Eigen::Dynamic, Eigen::Dynamic> DiagMat;

    int dim = V.cols();
    assert((dim==2 || dim==3) &&
           "The dimension of the vertices should be 2 or 3");

    //Construct the combined gradient matric
    SparseMat G;
    igl::grad(V,
              F,
              G, false);
    SparseMat GG(F.rows(), dim*V.rows());
    GG.reserve(G.nonZeros());
    for(int i=0; i<dim; ++i)
        GG.middleCols(i*G.cols(),G.cols()) = G.middleRows(i*F.rows(),F.rows());
    SparseMat D;
    igl::repdiag(GG,dim,D);

    //Compute area matrix
    VecXd areas;
    igl::doublearea(V, F, areas);
    DiagMat A = (0.5*areas).replicate(dim,1).asDiagonal();

    //Compute FEM Hessian
    H = D.transpose()*A*G;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::hessian<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int>&);
#endif
