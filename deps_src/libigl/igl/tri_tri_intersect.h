// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2021 Vladimir S. FONOV <vladimir.fonov@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/

/*
*  
*  C++ version based on the routines published in    
*  "Fast and Robust Triangle-Triangle Overlap Test      
*   Using Orientation Predicates"  P. Guigue - O. Devillers
*  
*  Works with Eigen data structures instead of plain C arrays
*  returns bool values
* 
*  Code is rewritten to get rid of the macros and use C++ lambda and 
*  inline functions instead
*  
*  Original notice: 
*
*  Triangle-Triangle Overlap Test Routines        
*  July, 2002                                                          
*  Updated December 2003
*
*  Updated by Vladimir S. FONOV 
*  March, 2023                                             
*                                                                       
*  This file contains C implementation of algorithms for                
*  performing two and three-dimensional triangle-triangle intersection test 
*  The algorithms and underlying theory are described in                    
*                                                                           
* "Fast and Robust Triangle-Triangle Overlap Test 
*  Using Orientation Predicates"  P. Guigue - O. Devillers
*                                                 
*  Journal of Graphics Tools, 8(1), 2003                                    
*                                                                           
*  Several geometric predicates are defined.  Their parameters are all      
*  points.  Each point is an array of two or three double precision         
*  floating point numbers. The geometric predicates implemented in          
*  this file are:                                                            
*                                                                           
*    int tri_tri_overlap_test_3d(p1,q1,r1,p2,q2,r2)                         
*    int tri_tri_overlap_test_2d(p1,q1,r1,p2,q2,r2)                         
*                                                                           
*    int tri_tri_intersection_test_3d(p1,q1,r1,p2,q2,r2,
*                                     coplanar,source,target)               
*                                                                           
*       is a version that computes the segment of intersection when            
*       the triangles overlap (and are not coplanar)                        
*                                                                           
*    each function returns 1 if the triangles (including their              
*    boundary) intersect, otherwise 0                                       
*                                                                           
*                                                                           
*  Other information are available from the Web page                        
*  http://www.acm.org/jgt/papers/GuigueDevillers03/                         
*                                                                           
*/

/*
Original MIT License (from https://github.com/erich666/jgt-code)
Copyright (c) <year> <copyright holders>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software
and associated documentation files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#pragma once
#ifndef IGL_TRI_TRI_INTERSECT_H
#define IGL_TRI_TRI_INTERSECT_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl {


// Three-dimensional Triangle-Triangle overlap test
//   if triangles are co-planar
//
// Input:
//   p1,q1,r1  - vertices of the 1st triangle (3D)
//   p2,q2,r2  - vertices of the 2nd triangle (3D)
//
// Output:
// 
//   Return true if two triangles overlap
template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2> 
IGL_INLINE bool tri_tri_overlap_test_3d(
  const Eigen::MatrixBase<DerivedP1> &  p1, 
  const Eigen::MatrixBase<DerivedQ1> &  q1, 
  const Eigen::MatrixBase<DerivedR1> &  r1, 
  const Eigen::MatrixBase<DerivedP2> &  p2, 
  const Eigen::MatrixBase<DerivedQ2> &  q2, 
  const Eigen::MatrixBase<DerivedR2> &  r2);


// Three-dimensional Triangle-Triangle Intersection Test
// additionaly computes the segment of intersection of the two triangles if it exists. 
// coplanar returns whether the triangles are coplanar, 
// source and target are the endpoints of the line segment of intersection 
//
// Input:
//   p1,q1,r1  - vertices of the 1st triangle (3D)
//   p2,q2,r2  - vertices of the 2nd triangle (3D)
//
// Output:
//   coplanar - flag if two triangles are coplanar
//   source - 1st point of intersection (if exists)
//   target - 2nd point in intersection (if exists)
//
//   Return true if two triangles intersect
template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2,
typename DerivedS,typename DerivedT>
IGL_INLINE bool tri_tri_intersection_test_3d(
    const Eigen::MatrixBase<DerivedP1> & p1, const Eigen::MatrixBase<DerivedQ1> & q1, const Eigen::MatrixBase<DerivedR1> & r1, 
    const Eigen::MatrixBase<DerivedP2> & p2, const Eigen::MatrixBase<DerivedQ2> & q2, const Eigen::MatrixBase<DerivedR2> & r2,
    bool & coplanar, 
    Eigen::MatrixBase<DerivedS> & source, 
    Eigen::MatrixBase<DerivedT> & target );



// Two dimensional Triangle-Triangle Overlap Test
// Input:
//   p1,q1,r1  - vertices of the 1st triangle (2D)
//   p2,q2,r2  - vertices of the 2nd triangle (2D)
//
// Output:
//   Return true if two triangles overlap
template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2>
IGL_INLINE bool tri_tri_overlap_test_2d(
  const Eigen::MatrixBase<DerivedP1> &p1, const Eigen::MatrixBase<DerivedQ1> &q1, const Eigen::MatrixBase<DerivedR1> &r1,
  const Eigen::MatrixBase<DerivedP2> &p2, const Eigen::MatrixBase<DerivedQ2> &q2, const Eigen::MatrixBase<DerivedR2> &r2);


};

#ifndef IGL_STATIC_LIBRARY
#  include "tri_tri_intersect.cpp"
#endif

#endif // IGL_TRI_TRI_INTERSECT_H
