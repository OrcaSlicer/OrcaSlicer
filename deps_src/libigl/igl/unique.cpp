// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "unique.h"
#include "sort.h"
#include "IndexComparison.h"
#include "SortableRow.h"
#include "sortrows.h"
#include "list_to_matrix.h"
#include "matrix_to_list.h"

#include <algorithm>
#include <iostream>
#include <map>

template <typename T>
IGL_INLINE void igl::unique(
  const std::vector<T> & A,
  std::vector<T> & C,
  std::vector<size_t> & IA,
  std::vector<size_t> & IC)
{
  using namespace std;
  std::vector<size_t> IM;
  std::vector<T> sortA;
  igl::sort(A,true,sortA,IM);
  // Original unsorted index map
  IA.resize(sortA.size());
  for(int i=0;i<(int)sortA.size();i++)
  {
    IA[i] = i;
  }
  IA.erase(
    std::unique(
    IA.begin(),
    IA.end(),
    igl::IndexEquals<const std::vector<T>& >(sortA)),IA.end());

  IC.resize(A.size());
  {
    int j = 0;
    for(int i = 0;i<(int)sortA.size();i++)
    {
      if(sortA[IA[j]] != sortA[i])
      {
        j++;
      }
      IC[IM[i]] = j;
    }
  }
  C.resize(IA.size());
  // Reindex IA according to IM
  for(int i = 0;i<(int)IA.size();i++)
  {
    IA[i] = IM[IA[i]];
    C[i] = A[IA[i]];
  }

}

template <typename T>
IGL_INLINE void igl::unique(
  const std::vector<T> & A,
  std::vector<T> & C)
{
  std::vector<size_t> IA,IC;
  return igl::unique(A,C,IA,IC);
}

template <
  typename DerivedA,
  typename DerivedC,
  typename DerivedIA,
  typename DerivedIC>
IGL_INLINE void igl::unique(
    const Eigen::DenseBase<DerivedA> & A,
    Eigen::PlainObjectBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedIA> & IA,
    Eigen::PlainObjectBase<DerivedIC> & IC)
{
  using namespace std;
  using namespace Eigen;
  vector<typename DerivedA::Scalar > vA;
  vector<typename DerivedC::Scalar > vC;
  vector<size_t> vIA,vIC;
  matrix_to_list(A,vA);
  unique(vA,vC,vIA,vIC);
  list_to_matrix(vC,C);
  list_to_matrix(vIA,IA);
  list_to_matrix(vIC,IC);
}

template <
  typename DerivedA,
  typename DerivedC
  >
IGL_INLINE void igl::unique(
    const Eigen::DenseBase<DerivedA> & A,
    Eigen::PlainObjectBase<DerivedC> & C)
{
  using namespace std;
  using namespace Eigen;
  vector<typename DerivedA::Scalar > vA;
  vector<typename DerivedC::Scalar > vC;
  vector<size_t> vIA,vIC;
  matrix_to_list(A,vA);
  unique(vA,vC,vIA,vIC);
  list_to_matrix(vC,C);
}

// Obsolete slow version converting to vectors
// template <typename DerivedA, typename DerivedIA, typename DerivedIC>
// IGL_INLINE void igl::unique_rows(
//   const Eigen::PlainObjectBase<DerivedA>& A,
//   Eigen::PlainObjectBase<DerivedA>& C,
//   Eigen::PlainObjectBase<DerivedIA>& IA,
//   Eigen::PlainObjectBase<DerivedIC>& IC)
// {
//   using namespace std;
//
//   typedef Eigen::Matrix<typename DerivedA::Scalar, Eigen::Dynamic, 1> RowVector;
//   vector<SortableRow<RowVector> > rows;
//   rows.resize(A.rows());
//   // Loop over rows
//   for(int i = 0;i<A.rows();i++)
//   {
//     RowVector ri = A.row(i);
//     rows[i] = SortableRow<RowVector>(ri);
//   }
//   vector<SortableRow<RowVector> > vC;
//
//   // unique on rows
//   vector<size_t> vIA;
//   vector<size_t> vIC;
//   unique(rows,vC,vIA,vIC);
//
//   // Convert to eigen
//   C.resize(vC.size(),A.cols());
//   IA.resize(vIA.size(),1);
//   IC.resize(vIC.size(),1);
//   for(int i = 0;i<C.rows();i++)
//   {
//     C.row(i) = vC[i].data;
//     IA(i) = vIA[i];
//   }
//   for(int i = 0;i<A.rows();i++)
//   {
//     IC(i) = vIC[i];
//   }
// }

// Obsolete
// template <typename DerivedA, typename DerivedIA, typename DerivedIC>
// IGL_INLINE void igl::unique_rows_many(
//   const Eigen::PlainObjectBase<DerivedA>& A,
//   Eigen::PlainObjectBase<DerivedA>& C,
//   Eigen::PlainObjectBase<DerivedIA>& IA,
//   Eigen::PlainObjectBase<DerivedIC>& IC)
// {
//   using namespace std;
//   // frequency map
//   typedef Eigen::Matrix<typename DerivedA::Scalar, Eigen::Dynamic, 1> RowVector;
//   IC.resize(A.rows(),1);
//   map<SortableRow<RowVector>, int> fm;
//   const int m = A.rows();
//   for(int i = 0;i<m;i++)
//   {
//     RowVector ri = A.row(i);
//     if(fm.count(SortableRow<RowVector>(ri)) == 0)
//     {
//       fm[SortableRow<RowVector>(ri)] = i;
//     }
//     IC(i) = fm[SortableRow<RowVector>(ri)];
//   }
//   IA.resize(fm.size(),1);
//   Eigen::VectorXi RIA(m);
//   C.resize(fm.size(),A.cols());
//   {
//     int i = 0;
//     for(typename map<SortableRow<RowVector > , int >::const_iterator fit = fm.begin();
//         fit != fm.end();
//         fit++)
//     {
//       IA(i) = fit->second;
//       RIA(fit->second) = i;
//       C.row(i) = fit->first.data;
//       i++;
//     }
//   }
//   // IC should index C
//   for(int i = 0;i<m;i++)
//   {
//     IC(i) = RIA(IC(i));
//   }
// }

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::unique<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::unique<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::unique<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::unique<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::unique<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::unique<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::unique<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::unique<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::unique<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::unique<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<long, -1, 1, 0, -1, 1>, Eigen::Matrix<long, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&);
template void igl::unique<Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::unique<double>(std::vector<double, std::allocator<double> > const&, std::vector<double, std::allocator<double> >&);
template void igl::unique<int>(std::vector<int, std::allocator<int> > const&, std::vector<int, std::allocator<int> >&);
template void igl::unique<int>(std::vector<int, std::allocator<int> > const&, std::vector<int, std::allocator<int> >&, std::vector<size_t, std::allocator<size_t> >&, std::vector<size_t, std::allocator<size_t> >&);
template void igl::unique<long>(std::vector<long, std::allocator<long> > const&, std::vector<long, std::allocator<long> >&, std::vector<size_t, std::allocator<size_t> >&, std::vector<size_t, std::allocator<size_t> >&);
#ifdef WIN32
template void igl::unique<class Eigen::Matrix<int,-1,1,0,-1,1>,class Eigen::Matrix<int,-1,1,0,-1,1>,class Eigen::Matrix<__int64,-1,1,0,-1,1>,class Eigen::Matrix<__int64,-1,1,0,-1,1> >(class Eigen::DenseBase<class Eigen::Matrix<int,-1,1,0,-1,1> > const &,class Eigen::PlainObjectBase<class Eigen::Matrix<int,-1,1,0,-1,1> > &,class Eigen::PlainObjectBase<class Eigen::Matrix<__int64,-1,1,0,-1,1> > &,class Eigen::PlainObjectBase<class Eigen::Matrix<__int64,-1,1,0,-1,1> > &);
#endif
#endif
