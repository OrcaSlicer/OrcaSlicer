// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "exterior_edges.h"
#include "oriented_facets.h"
#include "sort.h"
#include "unique_rows.h"

#include <cassert>
#include <unordered_map>
#include <utility>
#include <iostream>

//template <typename T> inline int sgn(T val) {
//      return (T(0) < val) - (val < T(0));
//}

//static void mod2(std::pair<const std::pair<const int, const int>, int>& p)
//{
//  using namespace std;
//  // Be sure that sign of mod matches sign of argument
//  p.second = p.second%2 ? sgn(p.second) : 0;
//}

//// http://stackoverflow.com/a/5517869/148668
//struct Compare
//{
//   int i;
//   Compare(const int& i) : i(i) {}
//};
//bool operator==(const std::pair<std::pair<const int, const int>,int>&p, const Compare& c)
//{
//  return c.i == p.second;
//}
//bool operator==(const Compare& c, const std::pair<std::pair<const int, const int>, int> &p)
//{
//  return c.i == p.second;
//}

IGL_INLINE void igl::exterior_edges(
  const Eigen::MatrixXi & F,
  Eigen::MatrixXi & E)
{
  using namespace Eigen;
  using namespace std;
  assert(F.cols() == 3);
  const size_t m = F.rows();
  MatrixXi all_E,sall_E,sort_order;
  // Sort each edge by index
  oriented_facets(F,all_E);
  sort(all_E,2,true,sall_E,sort_order);
  // Find unique edges
  MatrixXi uE;
  VectorXi IA,EMAP;
  unique_rows(sall_E,uE,IA,EMAP);
  VectorXi counts = VectorXi::Zero(uE.rows());
  for(size_t a = 0;a<3*m;a++)
  {
    counts(EMAP(a)) += (sort_order(a)==0?1:-1);
  }

  E.resize(all_E.rows(),2);
  {
    int e = 0;
    const size_t nue = uE.rows();
    // Append each unique edge with a non-zero amount of signed occurrences
    for(size_t ue = 0; ue<nue; ue++)
    {
      const int count = counts(ue);
      size_t i,j;
      if(count == 0)
      {
        continue;
      }else if(count < 0)
      {
        i = uE(ue,1);
        j = uE(ue,0);
      }else if(count > 0)
      {
        i = uE(ue,0);
        j = uE(ue,1);
      }
      // Append edge for every repeated entry
      const int abs_count = abs(count);
      for(int k = 0;k<abs_count;k++)
      {
        E(e,0) = i;
        E(e,1) = j;
        e++;
      }
    }
    E.conservativeResize(e,2);
  }
}

IGL_INLINE Eigen::MatrixXi igl::exterior_edges( const Eigen::MatrixXi & F)
{
  using namespace Eigen;
  MatrixXi E;
  exterior_edges(F,E);
  return E;
}
