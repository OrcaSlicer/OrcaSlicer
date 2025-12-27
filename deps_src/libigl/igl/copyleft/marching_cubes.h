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
    // marching_cubes( values, points, x_res, y_res, z_res, vertices, faces )
    //
    // performs marching cubes reconstruction on the grid defined by values, and
    // points, and generates vertices and faces
    //
    // Input:
    //  values  #number_of_grid_points x 1 array -- the scalar values of an
    //    implicit function defined on the grid points (<0 in the inside of the
    //    surface, 0 on the border, >0 outside)
    //  points  #number_of_grid_points x 3 array -- 3-D positions of the grid
    //    points, ordered in x,y,z order:
    //      points[index] = the point at (x,y,z) where :
    //      x = (index % (xres -1),
    //      y = (index / (xres-1)) %(yres-1),
    //      z = index / (xres -1) / (yres -1) ).
    //      where x,y,z index x, y, z dimensions
    //      i.e. index = x + y*xres + z*xres*yres
    //  xres  resolutions of the grid in x dimension
    //  yres  resolutions of the grid in y dimension
    //  zres  resolutions of the grid in z dimension
    // Output:
    //   vertices  #V by 3 list of mesh vertex positions
    //   faces  #F by 3 list of mesh triangle indices
    //
    template <
      typename Derivedvalues, 
      typename Derivedpoints, 
      typename Derivedvertices, 
      typename DerivedF>
      IGL_INLINE void marching_cubes(
        const Eigen::PlainObjectBase<Derivedvalues> &values,
        const Eigen::PlainObjectBase<Derivedpoints> &points,
        const unsigned x_res,
        const unsigned y_res,
        const unsigned z_res,
        Eigen::PlainObjectBase<Derivedvertices> &vertices,
        Eigen::PlainObjectBase<DerivedF> &faces);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "marching_cubes.cpp"
#endif

#endif
