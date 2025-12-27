// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "sort.h"

#include "SortableRow.h"
#include "reorder.h"
#include "IndexComparison.h"
#include "colon.h"
#include "parallel_for.h"

#include <cassert>
#include <algorithm>
#include <iostream>

template <typename DerivedX, typename DerivedY, typename DerivedIX>
IGL_INLINE void igl::sort(
  const Eigen::DenseBase<DerivedX>& X,
  const int dim,
  const bool ascending,
  Eigen::PlainObjectBase<DerivedY>& Y,
  Eigen::PlainObjectBase<DerivedIX>& IX)
{
  typedef typename DerivedX::Scalar Scalar;
  // get number of rows (or columns)
  int num_inner = (dim == 1 ? X.rows() : X.cols() );
  // Special case for swapping
  switch(num_inner)
  {
    default:
      break;
    case 2:
      return igl::sort2(X,dim,ascending,Y,IX);
    case 3:
      return igl::sort3(X,dim,ascending,Y,IX);
  }
  using namespace Eigen;
  // get number of columns (or rows)
  int num_outer = (dim == 1 ? X.cols() : X.rows() );
  // dim must be 2 or 1
  assert(dim == 1 || dim == 2);
  // Resize output
  Y.resizeLike(X);
  IX.resizeLike(X);
  // idea is to process each column (or row) as a std vector
  // loop over columns (or rows)
  for(int i = 0; i<num_outer;i++)
  {
    // Unsorted index map for this column (or row)
    std::vector<size_t> index_map(num_inner);
    std::vector<Scalar> data(num_inner);
    for(int j = 0;j<num_inner;j++)
    {
      if(dim == 1)
      {
        data[j] = (Scalar) X(j,i);
      }else
      {
        data[j] = (Scalar) X(i,j);
      }
    }
    // sort this column (or row)
    igl::sort( data, ascending, data, index_map);
    // Copy into Y and IX
    for(int j = 0;j<num_inner;j++)
    {
      if(dim == 1)
      {
        Y(j,i) = data[j];
        IX(j,i) = index_map[j];
      }else
      {
        Y(i,j) = data[j];
        IX(i,j) = index_map[j];
      }
    }
  }
}

template <typename DerivedX, typename DerivedY, typename DerivedIX>
IGL_INLINE void igl::sort_new(
  const Eigen::DenseBase<DerivedX>& X,
  const int dim,
  const bool ascending,
  Eigen::PlainObjectBase<DerivedY>& Y,
  Eigen::PlainObjectBase<DerivedIX>& IX)
{
  // get number of rows (or columns)
  int num_inner = (dim == 1 ? X.rows() : X.cols() );
  // Special case for swapping
  switch(num_inner)
  {
    default:
      break;
    case 2:
      return igl::sort2(X,dim,ascending,Y,IX);
    case 3:
      return igl::sort3(X,dim,ascending,Y,IX);
  }
  using namespace Eigen;
  // get number of columns (or rows)
  int num_outer = (dim == 1 ? X.cols() : X.rows() );
  // dim must be 2 or 1
  assert(dim == 1 || dim == 2);
  // Resize output
  Y.resizeLike(X);
  IX.resizeLike(X);
  // idea is to process each column (or row) as a std vector
  // loop over columns (or rows)
  for(int i = 0; i<num_outer;i++)
  {
    Eigen::VectorXi ix;
    colon(0,num_inner-1,ix);
    // Sort the index map, using unsorted for comparison
    if(dim == 1)
    {
      std::sort(
        ix.data(),
        ix.data()+ix.size(),
        igl::IndexVectorLessThan<const typename DerivedX::ConstColXpr >(X.col(i)));
    }else
    {
      std::sort(
        ix.data(),
        ix.data()+ix.size(),
        igl::IndexVectorLessThan<const typename DerivedX::ConstRowXpr >(X.row(i)));
    }
    // if not ascending then reverse
    if(!ascending)
    {
      std::reverse(ix.data(),ix.data()+ix.size());
    }
    for(int j = 0;j<num_inner;j++)
    {
      if(dim == 1)
      {
        Y(j,i) = X(ix[j],i);
        IX(j,i) = ix[j];
      }else
      {
        Y(i,j) = X(i,ix[j]);
        IX(i,j) = ix[j];
      }
    }
  }
}

template <typename DerivedX, typename DerivedY, typename DerivedIX>
IGL_INLINE void igl::sort2(
  const Eigen::DenseBase<DerivedX>& X,
  const int dim,
  const bool ascending,
  Eigen::PlainObjectBase<DerivedY>& Y,
  Eigen::PlainObjectBase<DerivedIX>& IX)
{
  using namespace Eigen;
  using namespace std;
  typedef typename DerivedY::Scalar YScalar;
  Y = X.derived().template cast<YScalar>();


  // get number of columns (or rows)
  int num_outer = (dim == 1 ? X.cols() : X.rows() );
  // get number of rows (or columns)
  int num_inner = (dim == 1 ? X.rows() : X.cols() );
  assert(num_inner == 2);(void)num_inner;
  typedef typename DerivedIX::Scalar Index;
  IX.resizeLike(X);
  if(dim==1)
  {
    IX.row(0).setConstant(0);// = DerivedIX::Zero(1,IX.cols());
    IX.row(1).setConstant(1);// = DerivedIX::Ones (1,IX.cols());
  }else
  {
    IX.col(0).setConstant(0);// = DerivedIX::Zero(IX.rows(),1);
    IX.col(1).setConstant(1);// = DerivedIX::Ones (IX.rows(),1);
  }
  // loop over columns (or rows)
  for(int i = 0;i<num_outer;i++)
  {
    YScalar & a = (dim==1 ? Y(0,i) : Y(i,0));
    YScalar & b = (dim==1 ? Y(1,i) : Y(i,1));
    Index & ai = (dim==1 ? IX(0,i) : IX(i,0));
    Index & bi = (dim==1 ? IX(1,i) : IX(i,1));
    if((ascending && a>b) || (!ascending && a<b))
    {
      std::swap(a,b);
      std::swap(ai,bi);
    }
  }
}

template <typename DerivedX, typename DerivedY, typename DerivedIX>
IGL_INLINE void igl::sort3(
  const Eigen::DenseBase<DerivedX>& X,
  const int dim,
  const bool ascending,
  Eigen::PlainObjectBase<DerivedY>& Y,
  Eigen::PlainObjectBase<DerivedIX>& IX)
{
  using namespace Eigen;
  using namespace std;
  typedef typename DerivedY::Scalar YScalar;
  Y = X.derived().template cast<YScalar>();
  Y.resizeLike(X);
  for(int j=0;j<X.cols();j++)for(int i=0;i<X.rows();i++)Y(i,j)=(YScalar)X(i,j);

  // get number of columns (or rows)
  int num_outer = (dim == 1 ? X.cols() : X.rows() );
  // get number of rows (or columns)
  int num_inner = (dim == 1 ? X.rows() : X.cols() );
  assert(num_inner == 3);(void)num_inner;
  typedef typename DerivedIX::Scalar Index;
  IX.resizeLike(X);
  if(dim==1)
  {
    IX.row(0).setConstant(0);// = DerivedIX::Zero(1,IX.cols());
    IX.row(1).setConstant(1);// = DerivedIX::Ones (1,IX.cols());
    IX.row(2).setConstant(2);// = DerivedIX::Ones (1,IX.cols());
  }else
  {
    IX.col(0).setConstant(0);// = DerivedIX::Zero(IX.rows(),1);
    IX.col(1).setConstant(1);// = DerivedIX::Ones (IX.rows(),1);
    IX.col(2).setConstant(2);// = DerivedIX::Ones (IX.rows(),1);
  }


  const auto & inner = [&IX,&Y,&dim,&ascending](const Index & i)
  {
    YScalar & a = (dim==1 ? Y(0,i) : Y(i,0));
    YScalar & b = (dim==1 ? Y(1,i) : Y(i,1));
    YScalar & c = (dim==1 ? Y(2,i) : Y(i,2));
    Index & ai = (dim==1 ? IX(0,i) : IX(i,0));
    Index & bi = (dim==1 ? IX(1,i) : IX(i,1));
    Index & ci = (dim==1 ? IX(2,i) : IX(i,2));
    if(ascending)
    {
      // 123 132 213 231 312 321
      if(a > b)
      {
        std::swap(a,b);
        std::swap(ai,bi);
      }
      // 123 132 123 231 132 231
      if(b > c)
      {
        std::swap(b,c);
        std::swap(bi,ci);
        // 123 123 123 213 123 213
        if(a > b)
        {
          std::swap(a,b);
          std::swap(ai,bi);
        }
        // 123 123 123 123 123 123
      }
    }else
    {
      // 123 132 213 231 312 321
      if(a < b)
      {
        std::swap(a,b);
        std::swap(ai,bi);
      }
      // 213 312 213 321 312 321
      if(b < c)
      {
        std::swap(b,c);
        std::swap(bi,ci);
        // 231 321 231 321 321 321
        if(a < b)
        {
          std::swap(a,b);
          std::swap(ai,bi);
        }
        // 321 321 321 321 321 321
      }
    }
  };
  parallel_for(num_outer,inner,16000);
}

template <class T>
IGL_INLINE void igl::sort(
const std::vector<T> & unsorted,
const bool ascending,
std::vector<T> & sorted,
std::vector<size_t> & index_map)
{
// Original unsorted index map
index_map.resize(unsorted.size());
for(size_t i=0;i<unsorted.size();i++)
{
  index_map[i] = i;
}
// Sort the index map, using unsorted for comparison
std::sort(
  index_map.begin(),
  index_map.end(),
  igl::IndexLessThan<const std::vector<T>& >(unsorted));

// if not ascending then reverse
if(!ascending)
{
  std::reverse(index_map.begin(),index_map.end());
}
  // make space for output without clobbering
  sorted.resize(unsorted.size());
  // reorder unsorted into sorted using index map
  igl::reorder(unsorted,index_map,sorted);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::sort<Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template void igl::sort<Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template void igl::sort<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<int>(std::vector<int, std::allocator<int> > const&, bool, std::vector<int, std::allocator<int> >&, std::vector<size_t,class std::allocator<size_t> > &);
template void igl::sort<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::sort<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::sort_new<Eigen::Matrix<int, 1, 6, 1, 1, 6>, Eigen::Matrix<int, 1, 6, 1, 1, 6>, Eigen::Matrix<int, 1, 6, 1, 1, 6> >(Eigen::DenseBase<Eigen::Matrix<int, 1, 6, 1, 1, 6> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, 1, 6, 1, 1, 6> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, 1, 6, 1, 1, 6> >&);
template void igl::sort<Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, 2, 0, -1, 2> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> >&);
template void igl::sort<Eigen::Matrix<double, -1, 4, 0, -1, 4>, Eigen::Matrix<double, -1, 4, 0, -1, 4>, Eigen::Matrix<int, -1, 4, 0, -1, 4> >(Eigen::DenseBase<Eigen::Matrix<double, -1, 4, 0, -1, 4> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 4, 0, -1, 4> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 4, 0, -1, 4> >&);
template void igl::sort<Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<long>(std::vector<long, std::allocator<long> > const&, bool, std::vector<long, std::allocator<long> >&, std::vector<size_t, std::allocator<size_t> >&);
template void igl::sort<Eigen::Matrix<double, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<double, -1, 2, 0, -1, 2>, Eigen::Matrix<double, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sort<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<long, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&);
template void igl::sort<Eigen::Matrix<double, -1, 2, 0, -1, 2>, Eigen::Matrix<double, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, 2, 0, -1, 2> >(Eigen::DenseBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> >&);
template void igl::sort<Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, int, bool, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#ifdef WIN32
template void igl::sort<class Eigen::Matrix<int,-1,1,0,-1,1>,class Eigen::Matrix<int,-1,1,0,-1,1>,class Eigen::Matrix<__int64,-1,1,0,-1,1> >(class Eigen::DenseBase<class Eigen::Matrix<int,-1,1,0,-1,1> > const &,int,bool,class Eigen::PlainObjectBase<class Eigen::Matrix<int,-1,1,0,-1,1> > &,class Eigen::PlainObjectBase<class Eigen::Matrix<__int64,-1,1,0,-1,1> > &);
#endif
#endif
