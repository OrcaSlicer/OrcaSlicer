// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "parse_rhs.h"
#include <algorithm>

template <typename DerivedV>
IGL_INLINE void igl::matlab::parse_rhs_double(
    const mxArray *prhs[], 
    Eigen::PlainObjectBase<DerivedV> & V)
{
  using namespace Eigen;
  // Use Eigen's map and cast to copy
  V = Map< Matrix<double,Dynamic,Dynamic> >
    (mxGetPr(prhs[0]),mxGetM(prhs[0]),mxGetN(prhs[0]))
    .cast<typename DerivedV::Scalar>();
}

template <typename DerivedV>
IGL_INLINE void igl::matlab::parse_rhs_index(
    const mxArray *prhs[], 
    Eigen::PlainObjectBase<DerivedV> & V)
{
  parse_rhs_double(prhs,V);
  V.array() -= 1;
}

template <typename MT>
IGL_INLINE void igl::matlab::parse_rhs(
  const mxArray *prhs[], 
  Eigen::SparseMatrix<MT> & M)
{
  using namespace Eigen;
  using namespace std;
  const mxArray * mx_data = prhs[0];
  // Handle boring case where matrix is actually an empty dense matrix
  if(mxGetNumberOfElements(mx_data) == 0)
  {
    M.resize(0,0);
    return;
  }
  assert(mxIsSparse(mx_data));
  assert(mxGetNumberOfDimensions(mx_data) == 2);
  //cout<<name<<": "<<mxGetM(mx_data)<<" "<<mxGetN(mx_data)<<endl;
  const int m = mxGetM(mx_data);
  const int n = mxGetN(mx_data);
  // TODO: It should be possible to directly load the data into the sparse
  // matrix without going through the triplets
  // Copy data immediately
  double * pr = mxGetPr(mx_data);
  mwIndex * ir = mxGetIr(mx_data);
  mwIndex * jc = mxGetJc(mx_data);
  vector<Triplet<MT> > MIJV;
  MIJV.reserve(mxGetNumberOfElements(mx_data));
  // Iterate over outside
  int k = 0;
  for(int j=0; j<n;j++)
  {
    // Iterate over inside
    while(k<(int)jc[j+1])
    {
      //cout<<ir[k]<<" "<<j<<" "<<pr[k]<<endl;
      assert((int)ir[k]<m);
      assert((int)j<n);
      MIJV.push_back(Triplet<MT >(ir[k],j,pr[k]));
      k++;
    }
  }
  M.resize(m,n);
  M.setFromTriplets(MIJV.begin(),MIJV.end());
}

#ifdef IGL_STATIC_LIBRARY
template void igl::matlab::parse_rhs_index<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(mxArray_tag const**, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::matlab::parse_rhs_index<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(mxArray_tag const**, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::matlab::parse_rhs_double<Eigen::Matrix<double, -1, -1, 0, -1, -1> >(mxArray_tag const**, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::matlab::parse_rhs_index<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(mxArray_tag const**, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
template void igl::matlab::parse_rhs_double<Eigen::Matrix<double, -1, 3, 1, -1, 3> >(mxArray_tag const**, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&);
#endif
