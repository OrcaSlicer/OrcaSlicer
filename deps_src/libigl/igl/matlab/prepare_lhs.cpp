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
  const Eigen::PlainObjectBase<DerivedV> & V,
  mxArray *plhs[])
{
  using namespace std;
  using namespace Eigen;
  const int m = V.rows();
  const int n = V.cols();
  plhs[0] = mxCreateDoubleMatrix(m,n, mxREAL);
  Eigen::Map< Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic> > 
    map(mxGetPr(plhs[0]),m,n);
  map = V.template cast<double>();
}

template <typename DerivedV>
IGL_INLINE void igl::matlab::prepare_lhs_logical(
  const Eigen::PlainObjectBase<DerivedV> & V,
  mxArray *plhs[])
{
  using namespace std;
  using namespace Eigen;
  const int m = V.rows();
  const int n = V.cols();
  plhs[0] = mxCreateLogicalMatrix(m,n);
  mxLogical * Vp = static_cast<mxLogical*>(mxGetData(plhs[0]));
  Eigen::Map< Eigen::Matrix<mxLogical,Eigen::Dynamic,Eigen::Dynamic> > 
    map(static_cast<mxLogical*>(mxGetData(plhs[0])),m,n);
  map = V.template cast<mxLogical>();
}

template <typename DerivedV>
IGL_INLINE void igl::matlab::prepare_lhs_index(
  const Eigen::PlainObjectBase<DerivedV> & V,
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
  const int m = M.rows();
  const int n = M.cols();
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

#ifdef IGL_STATIC_LIBRARY
template void igl::matlab::prepare_lhs_index<Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_index<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_index<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_logical<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_logical<Eigen::Matrix<bool, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<bool, -1, 1, 0, -1, 1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_index<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<double, -1, 3, 1, -1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<int, 1, -1, 1, 1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<int, 1, -1, 1, 1, -1> > const&, mxArray_tag**);
template void igl::matlab::prepare_lhs_double<Eigen::Matrix<int, 1, 3, 1, 1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<int, 1, 3, 1, 1, 3> > const&, mxArray_tag**);
#endif
