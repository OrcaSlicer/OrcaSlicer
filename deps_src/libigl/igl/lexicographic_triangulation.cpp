// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//                    Qingan Zhou <qnzhou@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "lexicographic_triangulation.h"
#include "sortrows.h"
#include "PlainMatrix.h"

#include <vector>
#include <list>

template<
  typename DerivedP,
  typename Orient2D,
  typename DerivedF
  >
IGL_INLINE void igl::lexicographic_triangulation(
    const Eigen::MatrixBase<DerivedP>& P,
    Orient2D orient2D,
    Eigen::PlainObjectBase<DerivedF>& F)
{
  typedef typename DerivedP::Scalar Scalar;
  const size_t num_pts = P.rows();
  if (num_pts < 3) {
    throw "At least 3 points are required for triangulation!";
  }

  // Sort points in lexicographic order.
  PlainMatrix<DerivedP> ordered_P;
  Eigen::VectorXi order;
  igl::sortrows(P, true, ordered_P, order);

  std::vector<Eigen::Vector3i> faces;
  std::list<int> boundary;
  const Scalar p0[] = {ordered_P(0, 0), ordered_P(0, 1)};
  const Scalar p1[] = {ordered_P(1, 0), ordered_P(1, 1)};
  for (size_t i=2; i<num_pts; i++) {
    const Scalar curr_p[] = {ordered_P(i, 0), ordered_P(i, 1)};
    if (faces.size() == 0) {
      // All points processed so far are collinear.
      // Check if the current point is collinear with every points before it.
      auto orientation = orient2D(p0, p1, curr_p);
      if (orientation != 0) {
        // Add a fan of triangles eminating from curr_p.
        if (orientation > 0) {
          for (size_t j=0; j<=i-2; j++) {
            faces.push_back({order[j], order[j+1], order[i]});
          }
        } else if (orientation < 0) {
          for (size_t j=0; j<=i-2; j++) {
            faces.push_back({order[j+1], order[j], order[i]});
          }
        }
        // Initialize current boundary.
        boundary.insert(boundary.end(), order.data(), order.data()+i+1);
        if (orientation < 0) {
          boundary.reverse();
        }
      }
    } else {
      const size_t bd_size = boundary.size();
      assert(bd_size >= 3);
      std::vector<short> orientations;
      for (auto itr=boundary.begin(); itr!=boundary.end(); itr++) {
        auto next_itr = std::next(itr, 1);
        if (next_itr == boundary.end()) {
          next_itr = boundary.begin();
        }
        const Scalar bd_p0[] = {P(*itr, 0), P(*itr, 1)};
        const Scalar bd_p1[] = {P(*next_itr, 0), P(*next_itr, 1)};
        auto orientation = orient2D(bd_p0, bd_p1, curr_p);
        if (orientation < 0) {
          faces.push_back({*next_itr, *itr, order[i]});
        }
        orientations.push_back(orientation);
      }

      auto left_itr = boundary.begin();
      auto right_itr = boundary.begin();
      auto curr_itr = boundary.begin();
      for (size_t j=0; j<bd_size; j++, curr_itr++) {
        size_t prev = (j+bd_size-1) % bd_size;
        if (orientations[j] >= 0 && orientations[prev] < 0) {
          right_itr = curr_itr;
        } else if (orientations[j] < 0 && orientations[prev] >= 0) {
          left_itr = curr_itr;
        }
      }
      assert(left_itr != right_itr);

      for (auto itr=left_itr; itr!=right_itr; itr++) {
        if (itr == boundary.end()) itr = boundary.begin();
        if (itr == right_itr) break;
        if (itr == left_itr) continue;
        itr = boundary.erase(itr);
        if (itr == boundary.begin()) {
            itr = boundary.end();
        }
        itr--;
      }

      if (right_itr == boundary.begin()) {
        assert(std::next(left_itr, 1) == boundary.end());
        boundary.insert(boundary.end(), order[i]);
      } else {
        assert(std::next(left_itr, 1) == right_itr);
        boundary.insert(right_itr, order[i]);
      }
    }
  }

  const size_t num_faces = faces.size();
  if (num_faces == 0) {
      // All input points are collinear.
      // Do nothing here.
  } else {
      F.resize(num_faces, 3);
      for (size_t i=0; i<num_faces; i++) {
          F.row(i) = faces[i];
      }
  }
}


#ifdef IGL_STATIC_LIBRARY
template void igl::lexicographic_triangulation<Eigen::Matrix<double, -1, -1, 0, -1, -1>, short (*)(double const*, double const*, double const*), Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, short (*)(double const*, double const*, double const*), Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
