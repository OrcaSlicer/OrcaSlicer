// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "prepare_lhs.h"
#include <algorithm>
template <typename DerivedV>
IGL_INLINE void igl::matlab::prepare_lhs_double(
  const Eigen::DenseBase<DerivedV> & V,
  mxArray *plhs[])
{
  using namespace std;
  using namespace Eigen;
  const auto m = V.rows();
  const auto n = V.cols();
  plhs[0] = mxCreateDoubleMatrix(m,n, mxREAL);
  Eigen::Map< Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic> >
    map(mxGetPr(plhs[0]),m,n);
  map = V.template cast<double>();
}

template <typename DerivedV>
IGL_INLINE void igl::matlab::prepare_lhs_logical(
  const Eigen::DenseBase<DerivedV> & V,
  mxArray *plhs[])
{
  using namespace std;
  using namespace Eigen;
  const auto m = V.rows();
  const auto n = V.cols();
  plhs[0] = mxCreateLogicalMatrix(m,n);
  Eigen::Map< Eigen::Matrix<mxLogical,Eigen::Dynamic,Eigen::Dynamic> >
    map(static_cast<mxLogical*>(mxGetData(plhs[0])),m,n);
  map = V.template cast<mxLogical>();
}

template <typename DerivedV>
IGL_INLINE void igl::matlab::prepare_lhs_index(
  const Eigen::DenseBase<DerivedV> & V,
  mxArray *plhs[])
{
  // Treat indices as reals
  const auto Vd = (V.template cast<double>().array()+1).eval();
  return prepare_lhs_double(Vd,plhs);
}

template <typename Vtype>
IGL_INLINE void igl::matlab::prepare_lhs_double(
  const Eigen::SparseMatrix<Vtype> & M,
  mxArray *plhs[])
{
  using namespace std;
  const auto m = M.rows();
  const auto n = M.cols();
  // THIS WILL NOT WORK FOR ROW-MAJOR
  assert(n==M.outerSize());
  const int nzmax = M.nonZeros();
  plhs[0] = mxCreateSparse(m, n, nzmax, mxREAL);
  mxArray * mx_data = plhs[0];
  // Copy data immediately
  double * pr = mxGetPr(mx_data);
  mwIndex * ir = mxGetIr(mx_data);
  mwIndex * jc = mxGetJc(mx_data);

  // Iterate over outside
  int k = 0;
  for(int j=0; j<M.outerSize();j++)
  {
    jc[j] = k;
    // Iterate over inside
    for(typename Eigen::SparseMatrix<Vtype>::InnerIterator it (M,j); it; ++it)
    {
      // copy (cast to double)
      pr[k] = it.value();
      ir[k] = it.row();
      k++;
    }
  }
  jc[M.outerSize()] = k;

}


template <typename Vtype>
IGL_INLINE void igl::matlab::prepare_lhs_double(
  const std::vector<Vtype> & V,
  mxArray *plhs[])
{
  plhs[0] = mxCreateCellMatrix(V.size(), 1);
  for(int  i=0; i<V.size(); i++)
  {
    const auto m = V[i].rows();
    const auto n = V[i].cols();
    mxArray * ai = mxCreateDoubleMatrix(m,n, mxREAL);
    Eigen::Map< Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic> >
      map(mxGetPr(ai),m,n);
    map = V[i].template cast<double>();
    mxSetCell(plhs[0],i,ai);
  }
}



#ifdef IGL_STATIC_LIBRARY
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::DenseBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::DenseBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_index<Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_index<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_index<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_logical<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_logical<Eigen::Matrix<bool, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<bool, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_index<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<double, -1, 3, 1, -1, 3> >(Eigen::DenseBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<int, 1, -1, 1, 1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, 1, -1, 1, 1, -1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<int, 1, 3, 1, 1, 3> >(Eigen::DenseBase<Eigen::Matrix<int, 1, 3, 1, 1, 3> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<float, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<float, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<float, -1, 3, 0, -1, 3> >(Eigen::DenseBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<double>(Eigen::SparseMatrix<double, 0, int> const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<double, -1, -1, 0, -1, -1> >(std::vector<Eigen::Matrix<double, -1, -1, 0, -1, -1>, std::allocator<Eigen::Matrix<double, -1, -1, 0, -1, -1> > > const&, mxArray_tag**);
#endif
