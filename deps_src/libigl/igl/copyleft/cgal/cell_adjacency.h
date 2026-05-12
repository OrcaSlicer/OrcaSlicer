// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#ifndef IGL_COPYLEFT_CGAL_CELL_ADJACENCY_H
#define IGL_COPYLEFT_CGAL_CELL_ADJACENCY_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <set>
#include <tuple>
#include <vector>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Determine adjacency of cells
      ///
      /// @param[in] per_patch_cells  #P by 2 list of cell labels on each side
      ///   of each patch.  Cell labels are assumed to be continuous from 0 to #C.
      /// @param[in] num_cells        number of cells.
      /// @param[out] adjacency_list  #C array of list of adjcent cell
      ///   information.  If cell i and cell j are adjacent via patch x, where i
      ///   is on the positive side of x, and j is on the negative side.  Then,
      ///   adjacency_list[i] will contain the entry {j, false, x} and
      ///   adjacency_list[j] will contain the entry {i, true, x}
      template < typename DerivedC >
      IGL_INLINE void cell_adjacency(
          const Eigen::MatrixBase<DerivedC>& per_patch_cells,
          const size_t num_cells,
          std::vector<std::set<std::tuple<typename DerivedC::Scalar, bool, size_t> > >&
          adjacency_list);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "cell_adjacency.cpp"
#endif
#endif
