// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_MARCHINGCUBES_H
#define IGL_COPYLEFT_MARCHINGCUBES_H
#include "../igl_inline.h"

#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    /// Performs marching cubes reconstruction on a grid defined by values, and
    /// points, and generates a mesh defined by vertices and faces
    ///
    /// @param[in] values  #number_of_grid_points x 1 array -- the scalar values of an
    ///    implicit function defined on the grid points (<0 in the inside of the
    ///    surface, 0 on the border, >0 outside)
    /// @param[in] points  #number_of_grid_points x 3 array -- 3-D positions of the grid
    ///    points, ordered in x,y,z order:
    ///      points[index] = the point at (x,y,z) where :
    ///      x = (index % (xres -1),
    ///      y = (index / (xres-1)) %(yres-1),
    ///      z = index / (xres -1) / (yres -1) ).
    ///      where x,y,z index x, y, z dimensions
    ///      i.e. index = x + y*xres + z*xres*yres
    /// @param[in] xres  resolutions of the grid in x dimension
    /// @param[in] yres  resolutions of the grid in y dimension
    /// @param[in] zres  resolutions of the grid in z dimension
    /// @param[in] isovalue  the isovalue of the surface to reconstruct
    /// @param[out] vertices  #V by 3 list of mesh vertex positions
    /// @param[out] faces  #F by 3 list of mesh triangle indices
    ///
    /// \see igl::marching_cubes
    template <typename DerivedValues, typename DerivedPoints, typename DerivedVertices, typename DerivedFaces>
    IGL_INLINE void marching_cubes(
        const Eigen::MatrixBase<DerivedValues> &values,
        const Eigen::MatrixBase<DerivedPoints> &points,
        const unsigned x_res,
        const unsigned y_res,
        const unsigned z_res,
        const double isovalue,
        Eigen::PlainObjectBase<DerivedVertices> &vertices,
        Eigen::PlainObjectBase<DerivedFaces> &faces);
    /// \overload
    /// Overload of the above function where the isovalue defaults to 0.0
    template <typename DerivedValues, typename DerivedPoints, typename DerivedVertices, typename DerivedFaces>
    IGL_INLINE void marching_cubes(
      const Eigen::MatrixBase<DerivedValues> &values,
      const Eigen::MatrixBase<DerivedPoints> &points,
      const unsigned x_res,
      const unsigned y_res,
      const unsigned z_res,
      Eigen::PlainObjectBase<DerivedVertices> &vertices,
      Eigen::PlainObjectBase<DerivedFaces> &faces);
    /// \overload
    /// @param[in] value_fun a function that takes a 3D point and returns a scalar value
    template <
      typename DerivedValue, 
      typename DerivedPoint,
      typename DerivedPoints, 
      typename DerivedVertices, 
      typename DerivedFaces>
    IGL_INLINE void marching_cubes(
        const std::function< DerivedValue(const DerivedPoint & ) > & value_fun,
        const Eigen::MatrixBase<DerivedPoints> &points,
        const unsigned x_res,
        const unsigned y_res,
        const unsigned z_res,
        const double isovalue, 
        Eigen::PlainObjectBase<DerivedVertices> &vertices,
        Eigen::PlainObjectBase<DerivedFaces> &faces);
    /// Perform marching cubes reconstruction on the sparse grid cells defined by (indices, points).
    /// The indices parameter is an nx8 dense array of index values into the points and values arrays.
    /// Each row of indices represents a cube for which to generate vertices and faces over.
    ///
    /// @param[in] values  #number_of_grid_points x 1 array -- the scalar values of an
    ///    implicit function defined on the grid points (<0 in the inside of the
    ///    surface, 0 on the border, >0 outside)
    /// @param[in] points  #number_of_grid_points x 3 array -- 3-D positions of the grid
    ///    points, ordered in x,y,z order:
    /// @param[in] indices  #cubes x 8 array -- one row for each cube where each value is
    ///    the index of a vertex in points and a scalar in values.
    ///    i.e. points[indices[i, j]] = the position of the j'th vertex of the i'th cube
    /// @param[out] vertices  #V by 3 list of mesh vertex positions
    /// @param[out] faces  #F by 3 list of mesh triangle indices
    ///
    /// \note The winding direction of the cube indices will affect the output winding of the faces
    ///
    template <typename DerivedValues, typename DerivedPoints, typename DerivedVertices, typename DerivedIndices, typename DerivedFaces>
    IGL_INLINE void marching_cubes(
      const Eigen::MatrixBase<DerivedValues> &values,
      const Eigen::MatrixBase<DerivedPoints> &points,
      const Eigen::MatrixBase<DerivedIndices> &indices,
      const double isovalue,
      Eigen::PlainObjectBase<DerivedVertices> &vertices,
      Eigen::PlainObjectBase<DerivedFaces> &faces);
    /// \overload
    /// \brief  isovalue defaults to 0.0
    template <typename DerivedValues, typename DerivedPoints, typename DerivedVertices, typename DerivedIndices, typename DerivedFaces>
    IGL_INLINE void marching_cubes(
      const Eigen::MatrixBase<DerivedValues> &values,
      const Eigen::MatrixBase<DerivedPoints> &points,
      const Eigen::MatrixBase<DerivedIndices> &indices,
      Eigen::PlainObjectBase<DerivedVertices> &vertices,
      Eigen::PlainObjectBase<DerivedFaces> &faces);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "marching_cubes.cpp"
#endif

#endif
