// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
//   and Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "hessian_energy.h"
#include <vector>

#include "hessian.h"
#include "massmatrix.h"
#include "boundary_loop.h"


template <typename DerivedV, typename DerivedF, typename Scalar>
IGL_INLINE void igl::hessian_energy(
                                    const Eigen::MatrixBase<DerivedV> & V,
                                    const Eigen::MatrixBase<DerivedF> & F,
                                    Eigen::SparseMatrix<Scalar>& Q)
{
    typedef typename DerivedV::Scalar denseScalar;
    typedef typename Eigen::Matrix<denseScalar, Eigen::Dynamic, 1> VecXd;
    typedef typename Eigen::SparseMatrix<Scalar> SparseMat;
    typedef typename Eigen::DiagonalMatrix
                       <Scalar, Eigen::Dynamic, Eigen::Dynamic> DiagMat;

    int dim = V.cols();
    assert((dim==2 || dim==3) &&
           "The dimension of the vertices should be 2 or 3");

    SparseMat M;
    igl::massmatrix(V,F,igl::MASSMATRIX_TYPE_VORONOI,M);

    //Kill non-interior DOFs
    VecXd Mint = M.diagonal();
    std::vector<std::vector<int> > bdryLoop;
    igl::boundary_loop(F,bdryLoop);
    for(const std::vector<int>& loop : bdryLoop)
        for(const int& bdryVert : loop)
            Mint(bdryVert) = 0.;

    //Invert Mint
    for(int i=0; i<Mint.rows(); ++i)
        if(Mint(i) > 0)
            Mint(i) = 1./Mint(i);

    //Repeat Mint to form diaginal matrix
    DiagMat stackedMinv = Mint.replicate(dim*dim,1).asDiagonal();

    //Compute squared Hessian
    SparseMat H;
    igl::hessian(V,F,H);
    Q = H.transpose()*stackedMinv*H;

}



#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::hessian_energy<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int>&);
#endif
