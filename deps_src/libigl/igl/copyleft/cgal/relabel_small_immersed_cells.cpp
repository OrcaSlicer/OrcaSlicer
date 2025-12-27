// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//

#include "relabel_small_immersed_cells.h"
#include "../../centroid.h"
#include "assign.h"
#include "cell_adjacency.h"

#include <vector>

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DerivedC,
  typename FT,
  typename DerivedW>
IGL_INLINE void igl::copyleft::cgal::relabel_small_immersed_cells(
    const Eigen::PlainObjectBase<DerivedV>& V,
    const Eigen::PlainObjectBase<DerivedF>& F,
    const size_t num_patches,
    const Eigen::PlainObjectBase<DerivedP>& P,
    const size_t num_cells,
    const Eigen::PlainObjectBase<DerivedC>& C,
    const FT vol_threashold,
    Eigen::PlainObjectBase<DerivedW>& W)
{
  const size_t num_vertices = V.rows();
  const size_t num_faces = F.rows();
  typedef std::tuple<typename DerivedC::Scalar, bool, size_t> CellConnection;
  std::vector<std::set<CellConnection> > cell_adj;
  igl::copyleft::cgal::cell_adjacency(C, num_cells, cell_adj);

  Eigen::MatrixXd VV;
  assign(V,VV);

  auto compute_cell_volume = [&](size_t cell_id) {
    std::vector<short> is_involved(num_patches, 0);
    for (size_t i=0; i<num_patches; i++) {
      if (C(i,0) == cell_id) {
        // cell is on positive side of patch i.
        is_involved[i] = 1;
      }
      if (C(i,1) == cell_id) {
        // cell is on negative side of patch i.
        is_involved[i] = -1;
      }
    }

    std::vector<size_t> involved_positive_faces;
    std::vector<size_t> involved_negative_faces;
    for (size_t i=0; i<num_faces; i++) {
      switch (is_involved[P[i]]) {
        case 1:
          involved_negative_faces.push_back(i);
          break;
        case -1:
          involved_positive_faces.push_back(i);
          break;
      }
    }

    const size_t num_positive_faces = involved_positive_faces.size();
    const size_t num_negative_faces = involved_negative_faces.size();
    DerivedF selected_faces(num_positive_faces + num_negative_faces, 3);
    for (size_t i=0; i<num_positive_faces; i++) {
      selected_faces.row(i) = F.row(involved_positive_faces[i]);
    }
    for (size_t i=0; i<num_negative_faces; i++) {
      selected_faces.row(num_positive_faces+i) =
        F.row(involved_negative_faces[i]).reverse();
    }

    Eigen::VectorXd c(3);
    double vol;
    igl::centroid(VV, selected_faces, c, vol);
    return vol;
  };

  std::vector<typename DerivedV::Scalar> cell_volumes(num_cells);
  for (size_t i=0; i<num_cells; i++) {
    cell_volumes[i] = compute_cell_volume(i);
  }

  std::vector<typename DerivedW::Scalar> cell_values(num_cells);
  for (size_t i=0; i<num_faces; i++) {
    cell_values[C(P[i], 0)] = W(i, 0);
    cell_values[C(P[i], 1)] = W(i, 1);
  }

  for (size_t i=1; i<num_cells; i++) {
    std::cout << cell_volumes[i] << std::endl;
    if (cell_volumes[i] >= vol_threashold) continue;
    std::set<typename DerivedW::Scalar> neighbor_values;
    const auto neighbors = cell_adj[i];
    for (const auto& entry : neighbors) {
      const auto& j = std::get<0>(entry);
      neighbor_values.insert(cell_values[j]);
    }
    // If cell i is immersed, assign its value to be the immersed value.
    if (neighbor_values.size() == 1) {
      cell_values[i] = *neighbor_values.begin();
    }
  }

  for (size_t i=0; i<num_faces; i++) {
    W(i,0) = cell_values[C(P[i], 0)];
    W(i,1) = cell_values[C(P[i], 1)];
  }
}

