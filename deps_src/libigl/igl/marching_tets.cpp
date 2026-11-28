// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Francis Williams <francis@fwilliams.info>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "marching_tets.h"

#include <unordered_map>
#include <vector>
#include <utility>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include <utility>
#include <cmath>

template <typename DerivedTV,
          typename DerivedTT,
          typename DerivedS,
          typename DerivedSV,
          typename DerivedSF,
          typename DerivedJ,
          typename BCType>
void igl::marching_tets(
    const Eigen::MatrixBase<DerivedTV>& TV,
    const Eigen::MatrixBase<DerivedTT>& TT,
    const Eigen::MatrixBase<DerivedS>& isovals,
    const typename DerivedS::Scalar isovalue,
    Eigen::PlainObjectBase<DerivedSV>& outV,
    Eigen::PlainObjectBase<DerivedSF>& outF,
    Eigen::PlainObjectBase<DerivedJ>& J,
    Eigen::SparseMatrix<BCType>& BC)
{
  using namespace std;

  // We're hashing edges to deduplicate using 64 bit ints. The upper and lower
  // 32 bits of a key are the indices of vertices in the mesh. The implication is
  // that you can only have 2^32 vertices which I have deemed sufficient for
  // anything reasonable.
  const auto make_edge_key = [](const pair<int32_t, int32_t>& p) -> std::int64_t
  {
    std::int64_t ret = 0;
    ret |= p.first;
    ret |= static_cast<std::int64_t>(p.second) << 32;
    return ret;
  };

  const int mt_cell_lookup[16][4] =
  {
    { -1, -1, -1, -1 },
    {  0,  2,  1, -1 },
    {  0,  3,  4, -1 },
    {  2,  1,  3,  4 },
    {  5,  3,  1, -1 },
    {  0,  2,  5,  3 },
    {  0,  1,  5,  4 },
    {  2,  5,  4, -1 },
    {  4,  5,  2, -1 },
    {  0,  4,  5,  1 },
    {  0,  3,  5,  2 },
    {  1,  3,  5, -1 },
    {  4,  3,  1,  2 },
    {  0,  4,  3, -1 },
    {  0,  1,  2, -1 },
    { -1, -1, -1, -1 },
  };

  const int mt_edge_lookup[6][2] =
  {
    {0, 1},
    {0, 2},
    {0, 3},
    {1, 2},
    {1, 3},
    {2, 3},
  };

  // Store the faces and the tet they are in
  vector<pair<Eigen::RowVector3i, int>> faces;

  // Store the edges in the tet mesh which we add vertices on
  // so we can deduplicate
  vector<pair<int, int>> edge_table;


  assert(TT.cols() == 4 && TT.rows() >= 1);
  assert(TV.cols() == 3 && TV.rows() >= 4);
  assert(isovals.cols() == 1);

  // For each tet
  for (int i = 0; i < TT.rows(); i++)
  {
    uint8_t key = 0;
    for (int v = 0; v < 4; v++)
    {
      const int vid = TT(i, v);
      const uint8_t flag = isovals(vid, 0) > isovalue;
      key |= flag << v;
    }

    // This will contain the index in TV of each vertex in the tet
    int v_ids[4] = {-1, -1, -1, -1};

    // Insert any vertices if the tet intersects the level surface
    for (int e = 0; e < 4 && mt_cell_lookup[key][e] != -1; e++)
    {
      const int tv1_idx = TT(i, mt_edge_lookup[mt_cell_lookup[key][e]][0]);
      const int tv2_idx = TT(i, mt_edge_lookup[mt_cell_lookup[key][e]][1]);
      const int vertex_id = edge_table.size();
      edge_table.push_back(make_pair(std::min(tv1_idx, tv2_idx), std::max(tv1_idx, tv2_idx)));
      v_ids[e] = vertex_id;
    }

    // Insert the corresponding faces
    if (v_ids[0] != -1)
    {
      bool is_quad = mt_cell_lookup[key][3] != -1;
      if (is_quad)
      {
        const Eigen::RowVector3i f1(v_ids[0], v_ids[1], v_ids[3]);
        const Eigen::RowVector3i f2(v_ids[1], v_ids[2], v_ids[3]);
        faces.push_back(make_pair(f1, i));
        faces.push_back(make_pair(f2, i));
      }
      else
      {
        const Eigen::RowVector3i f(v_ids[0], v_ids[1], v_ids[2]);
        faces.push_back(make_pair(f, i));
      }

    }
  }

  int num_unique = 0;
  outV.resize(edge_table.size(), 3);
  outF.resize(faces.size(), 3);
  J.resize(faces.size());

  // Sparse matrix triplets for BC
  vector<Eigen::Triplet<BCType>> bc_triplets;
  bc_triplets.reserve(edge_table.size());

  // Deduplicate vertices
  unordered_map<std::int64_t, int> emap;
  emap.max_load_factor(0.5);
  emap.reserve(edge_table.size());

  for (int f = 0; f < faces.size(); f++)
  {
    const int ti = faces[f].second;
    assert(ti>=0);
    assert(ti<TT.rows());
    J(f) = ti;
    for (int v = 0; v < 3; v++)
    {
      const int vi = faces[f].first[v];
      const pair<int32_t, int32_t> edge = edge_table[vi];
      const std::int64_t key = make_edge_key(edge);
      auto it = emap.find(key);
      if (it == emap.end()) // New unique vertex, insert it
      {
        // Typedef to make sure we handle floats properly
        typedef Eigen::Matrix<typename DerivedTV::Scalar, 1, 3, Eigen::RowMajor, 1, 3> RowVector;
        using Scalar = typename DerivedS::Scalar;
        const RowVector v1 =  TV.row(edge.first).template cast<Scalar>();
        const RowVector v2 = TV.row(edge.second).template cast<Scalar>();
        const Scalar a = abs(isovals(edge.first, 0) - isovalue);
        const Scalar b = abs(isovals(edge.second, 0) - isovalue);
        const Scalar w = a / (a+b);

        // Create a casted copy in case BCType is a float and we need to downcast
        const BCType bc_w = static_cast<BCType>(w);
        bc_triplets.push_back(Eigen::Triplet<BCType>(num_unique, edge.first, 1-bc_w));
        bc_triplets.push_back(Eigen::Triplet<BCType>(num_unique, edge.second, bc_w));

        // Create a casted copy in case DerivedTV::Scalar is a float and we need to downcast
        const typename DerivedTV::Scalar v_w = static_cast<typename DerivedTV::Scalar>(w);
        outV.row(num_unique) = (1-v_w)*v1 + v_w*v2;
        outF(f, v) = num_unique;

        emap.emplace(key, num_unique);
        num_unique += 1;
      } else {
        outF(f, v) = it->second;
      }
    }
  }
  outV.conservativeResize(num_unique, 3);
  BC.resize(num_unique, TV.rows());
  BC.setFromTriplets(bc_triplets.begin(), bc_triplets.end());
}


#ifdef IGL_STATIC_LIBRARY
template void igl::marching_tets<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, const double, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::SparseMatrix<double, 0, int>&);
#endif // IGL_STATIC_LIBRARY
