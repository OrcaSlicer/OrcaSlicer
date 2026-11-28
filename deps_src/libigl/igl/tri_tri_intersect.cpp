// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2021 Vladimir S. FONOV <vladimir.fonov@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

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

#ifndef IGL_TRI_TRI_INTERSECT_CPP
#define IGL_TRI_TRI_INTERSECT_CPP

#include "tri_tri_intersect.h"
#include "EPS.h"
#include <Eigen/Geometry>

// helper functions
namespace igl {

namespace internal {

template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2,
typename DerivedN1>
IGL_INLINE bool coplanar_tri_tri3d(
  const Eigen::MatrixBase<DerivedP1> &p1, const Eigen::MatrixBase<DerivedQ1> &q1, const Eigen::MatrixBase<DerivedR1> &r1,
  const Eigen::MatrixBase<DerivedP2> &p2, const Eigen::MatrixBase<DerivedQ2> &q2, const Eigen::MatrixBase<DerivedR2> &r2,
  const Eigen::MatrixBase<DerivedN1> &normal_1);

template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2>
IGL_INLINE bool ccw_tri_tri_intersection_2d(
  const Eigen::MatrixBase<DerivedP1> &p1, const Eigen::MatrixBase<DerivedQ1> &q1, const Eigen::MatrixBase<DerivedR1> &r1,
  const Eigen::MatrixBase<DerivedP2> &p2, const Eigen::MatrixBase<DerivedQ2> &q2, const Eigen::MatrixBase<DerivedR2> &r2);  

  template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
            typename DerivedP2,typename DerivedQ2,typename DerivedR2>
  inline bool _IGL_CHECK_MIN_MAX(
    const Eigen::MatrixBase<DerivedP1> &p1, const Eigen::MatrixBase<DerivedQ1> &q1, const Eigen::MatrixBase<DerivedR1> &r1,
    const Eigen::MatrixBase<DerivedP2> &p2, const Eigen::MatrixBase<DerivedQ2> &q2, const Eigen::MatrixBase<DerivedR2> &r2)
  {
    using Scalar    = typename DerivedP1::Scalar;
    using RowVector = typename Eigen::Matrix<Scalar, 1, 3>;

    RowVector v1=p2-q1;
    RowVector v2=p1-q1;
    RowVector N1=v1.cross(v2);
    v1=q2-q1;

    if (v1.dot(N1) > 0.0) return false;

    v1=p2-p1;
    v2=r1-p1;
    N1=v1.cross(v2);
    v1=r2-p1;

    if (v1.dot(N1) > 0.0) return false;
    else return true;
  }



/* Permutation in a canonical form of T2's vertices */

  template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
            typename DerivedP2,typename DerivedQ2,typename DerivedR2,
            typename DP2,typename DQ2,typename DR2,
            typename DerivedN1>
  inline bool _IGL_TRI_TRI_3D(
    const Eigen::MatrixBase<DerivedP1> &p1, const Eigen::MatrixBase<DerivedQ1> &q1, const Eigen::MatrixBase<DerivedR1> &r1,
    const Eigen::MatrixBase<DerivedP2> &p2, const Eigen::MatrixBase<DerivedQ2> &q2, const Eigen::MatrixBase<DerivedR2> &r2,
    DP2 dp2, DQ2 dq2,DR2 dr2,
    const Eigen::MatrixBase<DerivedN1> &N1)
  {
    if (dp2 > 0.0) { 
      if (dq2 > 0.0) return _IGL_CHECK_MIN_MAX(p1,r1,q1,r2,p2,q2);
      else if (dr2 > 0.0) return _IGL_CHECK_MIN_MAX(p1,r1,q1,q2,r2,p2);
      else return _IGL_CHECK_MIN_MAX(p1,q1,r1,p2,q2,r2); }
    else if (dp2 < 0.0) { 
      if (dq2 < 0.0) return _IGL_CHECK_MIN_MAX(p1,q1,r1,r2,p2,q2);
      else if (dr2 < 0.0) return _IGL_CHECK_MIN_MAX(p1,q1,r1,q2,r2,p2);
      else return _IGL_CHECK_MIN_MAX(p1,r1,q1,p2,q2,r2);
    } else { 
      if (dq2 < 0.0) { 
        if (dr2 >= 0.0)  return  _IGL_CHECK_MIN_MAX(p1,r1,q1,q2,r2,p2);
        else return _IGL_CHECK_MIN_MAX(p1,q1,r1,p2,q2,r2);
      }
      else if (dq2 > 0.0) { 
        if (dr2 > 0.0) return _IGL_CHECK_MIN_MAX(p1,r1,q1,p2,q2,r2);
        else  return _IGL_CHECK_MIN_MAX(p1,q1,r1,q2,r2,p2);
      } 
      else  { 
        if (dr2 > 0.0) return _IGL_CHECK_MIN_MAX(p1,q1,r1,r2,p2,q2);
        else if (dr2 < 0.0) return _IGL_CHECK_MIN_MAX(p1,r1,q1,r2,p2,q2);
        else return coplanar_tri_tri3d(p1,q1,r1,p2,q2,r2,N1);
      }}
  }


  
} //igl

} // internal
/*
*
*  Three-dimensional Triangle-Triangle Overlap Test
*
*/

template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2> 
IGL_INLINE bool igl::tri_tri_overlap_test_3d(
  const Eigen::MatrixBase<DerivedP1> &  p1, 
  const Eigen::MatrixBase<DerivedQ1> &  q1, 
  const Eigen::MatrixBase<DerivedR1> &  r1, 
  const Eigen::MatrixBase<DerivedP2> &  p2, 
  const Eigen::MatrixBase<DerivedQ2> &  q2, 
  const Eigen::MatrixBase<DerivedR2> &  r2)
{
  using Scalar    = typename DerivedP1::Scalar;
  using RowVector = typename Eigen::Matrix<Scalar, 1, 3>;
 
  Scalar dp1, dq1, dr1, dp2, dq2, dr2;
  RowVector v1, v2;
  RowVector N1, N2; 
  
  /* Compute distance signs  of p1, q1 and r1 to the plane of
     triangle(p2,q2,r2) */


  v1=p2-r2;
  v2=q2-r2;
  N2=v1.cross(v2);

  v1=p1-r2;
  dp1 = v1.dot(N2);
  v1=q1-r2;
  dq1 = v1.dot(N2);
  v1=r1-r2;
  dr1 = v1.dot(N2);
  
  if (((dp1 * dq1) > 0.0) && ((dp1 * dr1) > 0.0))  return false; 

  /* Compute distance signs  of p2, q2 and r2 to the plane of
     triangle(p1,q1,r1) */

  v1=q1-p1;
  v2=r1-p1;
  N1=v1.cross(v2);

  v1=p2-r1;
  dp2 = v1.dot(N1);
  v1=q2-r1;
  dq2 = v1.dot(N1);
  v1=r2-r1;
  dr2 = v1.dot(N1);
  
  if (((dp2 * dq2) > 0.0) && ((dp2 * dr2) > 0.0)) return false;

  /* Permutation in a canonical form of T1's vertices */

  if (dp1 > 0.0) {
    if (dq1 > 0.0) return internal::_IGL_TRI_TRI_3D(r1,p1,q1,p2,r2,q2,dp2,dr2,dq2,N1);
    else if (dr1 > 0.0) return internal::_IGL_TRI_TRI_3D(q1,r1,p1,p2,r2,q2,dp2,dr2,dq2,N1);
    else return internal::_IGL_TRI_TRI_3D(p1,q1,r1,p2,q2,r2,dp2,dq2,dr2,N1);
  } else if (dp1 < 0.0) {
    if (dq1 < 0.0) return internal::_IGL_TRI_TRI_3D(r1,p1,q1,p2,q2,r2,dp2,dq2,dr2,N1);
    else if (dr1 < 0.0) return internal::_IGL_TRI_TRI_3D(q1,r1,p1,p2,q2,r2,dp2,dq2,dr2,N1);
    else return internal::_IGL_TRI_TRI_3D(p1,q1,r1,p2,r2,q2,dp2,dr2,dq2,N1);
  } else {
    if (dq1 < 0.0) {
      if (dr1 >= 0.0) return internal::_IGL_TRI_TRI_3D(q1,r1,p1,p2,r2,q2,dp2,dr2,dq2,N1);
      else return internal::_IGL_TRI_TRI_3D(p1,q1,r1,p2,q2,r2,dp2,dq2,dr2,N1);
    }
    else if (dq1 > 0.0) {
      if (dr1 > 0.0) return internal::_IGL_TRI_TRI_3D(p1,q1,r1,p2,r2,q2,dp2,dr2,dq2,N1);
      else return internal::_IGL_TRI_TRI_3D(q1,r1,p1,p2,q2,r2,dp2,dq2,dr2,N1);
    }
    else  {
      if (dr1 > 0.0) return internal::_IGL_TRI_TRI_3D(r1,p1,q1,p2,q2,r2,dp2,dq2,dr2,N1);
      else if (dr1 < 0.0) return internal::_IGL_TRI_TRI_3D(r1,p1,q1,p2,r2,q2,dp2,dr2,dq2,N1);
      else return internal::coplanar_tri_tri3d(p1,q1,r1,p2,q2,r2,N1);
    }
  }
};



template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2,
typename DerivedN1>
IGL_INLINE bool igl::internal::coplanar_tri_tri3d(
  const Eigen::MatrixBase<DerivedP1> &p1, const Eigen::MatrixBase<DerivedQ1> &q1, const Eigen::MatrixBase<DerivedR1> &r1,
  const Eigen::MatrixBase<DerivedP2> &p2, const Eigen::MatrixBase<DerivedQ2> &q2, const Eigen::MatrixBase<DerivedR2> &r2,
  const Eigen::MatrixBase<DerivedN1> &normal_1)
{

  using Scalar= typename DerivedP1::Scalar;
  using RowVector2D = typename Eigen::Matrix<Scalar,1,2>;
 
  RowVector2D P1,Q1,R1;
  RowVector2D P2,Q2,R2;

  Scalar n_x, n_y, n_z;

  n_x = ((normal_1[0]<0.0)?-normal_1[0]:normal_1[0]);
  n_y = ((normal_1[1]<0.0)?-normal_1[1]:normal_1[1]);
  n_z = ((normal_1[2]<0.0)?-normal_1[2]:normal_1[2]);


  /* Projection of the triangles in 3D onto 2D such that the area of
     the projection is maximized. */


  if (( n_x > n_z ) && ( n_x >= n_y )) {
    // Project onto plane YZ

      P1[0] = q1[2]; P1[1] = q1[1];
      Q1[0] = p1[2]; Q1[1] = p1[1];
      R1[0] = r1[2]; R1[1] = r1[1]; 
    
      P2[0] = q2[2]; P2[1] = q2[1];
      Q2[0] = p2[2]; Q2[1] = p2[1];
      R2[0] = r2[2]; R2[1] = r2[1]; 

  } else if (( n_y > n_z ) && ( n_y >= n_x )) {
    // Project onto plane XZ

    P1[0] = q1[0]; P1[1] = q1[2];
    Q1[0] = p1[0]; Q1[1] = p1[2];
    R1[0] = r1[0]; R1[1] = r1[2]; 
 
    P2[0] = q2[0]; P2[1] = q2[2];
    Q2[0] = p2[0]; Q2[1] = p2[2];
    R2[0] = r2[0]; R2[1] = r2[2]; 
    
  } else {
    // Project onto plane XY

    P1[0] = p1[0]; P1[1] = p1[1]; 
    Q1[0] = q1[0]; Q1[1] = q1[1]; 
    R1[0] = r1[0]; R1[1] = r1[1]; 
    
    P2[0] = p2[0]; P2[1] = p2[1]; 
    Q2[0] = q2[0]; Q2[1] = q2[1]; 
    R2[0] = r2[0]; R2[1] = r2[1]; 
  }

  return tri_tri_overlap_test_2d(P1,Q1,R1,P2,Q2,R2);
    
};


namespace igl 
{
  namespace internal {
/*
*                                                                
*  Three-dimensional Triangle-Triangle Intersection              
*
*/

/*
   This macro is called when the triangles surely intersect
   It constructs the segment of intersection of the two triangles
   if they are not coplanar.
*/

// NOTE: a faster, but possibly less precise, method of computing
// point B is described here: https://github.com/erich666/jgt-code/issues/5

template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2,
typename DerivedS,typename DerivedT,
typename DerivedN1,typename DerivedN2>
bool _IGL_CONSTRUCT_INTERSECTION(
  const Eigen::MatrixBase<DerivedP1> &p1, const Eigen::MatrixBase<DerivedQ1> &q1, const Eigen::MatrixBase<DerivedR1> &r1,
  const Eigen::MatrixBase<DerivedP2> &p2, const Eigen::MatrixBase<DerivedQ2> &q2, const Eigen::MatrixBase<DerivedR2> &r2,
        Eigen::MatrixBase<DerivedS>  &source, Eigen::MatrixBase<DerivedT> &target,
  const Eigen::MatrixBase<DerivedN1> &N1,const Eigen::MatrixBase<DerivedN2> &N2)
{
  using Scalar = typename DerivedP1::Scalar;
  using RowVector3D = typename Eigen::Matrix<Scalar,1,3>;

  RowVector3D v,v1,v2,N;

  v1=q1-p1;
  v2=r2-p1;
  N=v1.cross(v2);
  v=p2-p1;
  if (v.dot(N) > 0.0) {
    v1=r1-p1;
    N=v1.cross(v2);
    if (v.dot(N) <= 0.0) {
      v2=q2-p1;
      N=v1.cross(v2);
      if (v.dot(N) > 0.0) {
        v1=p1-p2;
        v2=p1-r1;
        Scalar alpha = v1.dot(N2) / v2.dot(N2);
        v1=v2*alpha;
        source=p1-v1;
        v1=p2-p1;
        v2=p2-r2;
        alpha = v1.dot(N1) / v2.dot(N1);
        v1=v2*alpha;
        target=p2-v1;
        return true;
      } else { 
        v1=p2-p1;
        v2=p2-q2;
        Scalar alpha = v1.dot(N1) / v2.dot(N1);
        v1=v2*alpha;
        source=p2-v1;
        v1=p2-p1;
        v2=p2-r2;
        alpha = v1.dot(N1) / v2.dot(N1);
        v1=v2*alpha;
        target=p2-v1;
        return true;
      } 
    } else {
      return false;
    } 
  } else { 
    v2=q2-p1;
    N=v1.cross(v2);
    if (v.dot(N) < 0.0) {
      return false;
    } else {
      v1=r1-p1;
      N=v1.cross(v2);
      if (v.dot(N) >= 0.0) { 
        v1=p1-p2;
        v2=p1-r1;
        Scalar alpha = v1.dot(N2) / v2.dot(N2);
        v1=v2*alpha;
        source=p1-v1;
        v1=p1-p2;
        v2=p1-q1;
        alpha = v1.dot(N2) / v2.dot(N2);
        v1=v2*alpha;
        target=p1-v1 ;
        return true; 
      } else { 
        v1=p2-p1 ;
        v2=p2-q2 ;
        Scalar alpha = v1.dot(N1) / v2.dot(N1);
        v1=v2*alpha;
        source=p2-v1;
        v1=p1-p2;
        v2=p1-q1;
        alpha = v1.dot(N2) / v2.dot(N2);
        v1=v2*alpha;
        target=p1-v1 ;
        return true;
    }}}
}



// #define _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,p2,q2,r2) { \
//   _IGL_SUB(v1,q1,p1) \
//   _IGL_SUB(v2,r2,p1) \
//   _IGL_CROSS(N,v1,v2) \
//   _IGL_SUB(v,p2,p1) \
//   if (_IGL_DOT(v,N) > 0.0) {\
//     _IGL_SUB(v1,r1,p1) \
//     _IGL_CROSS(N,v1,v2) \
//     if (_IGL_DOT(v,N) <= 0.0) { \
//       _IGL_SUB(v2,q2,p1) \
//       _IGL_CROSS(N,v1,v2) \
//       if (_IGL_DOT(v,N) > 0.0) { \
//   _IGL_SUB(v1,p1,p2) \
//   _IGL_SUB(v2,p1,r1) \
//   alpha = _IGL_DOT(v1,N2) / _IGL_DOT(v2,N2); \
//   _IGL_SCALAR(v1,alpha,v2) \
//   _IGL_SUB(source,p1,v1) \
//   _IGL_SUB(v1,p2,p1) \
//   _IGL_SUB(v2,p2,r2) \
//   alpha = _IGL_DOT(v1,N1) / _IGL_DOT(v2,N1); \
//   _IGL_SCALAR(v1,alpha,v2) \
//   _IGL_SUB(target,p2,v1) \
//   return true; \
//       } else { \
//   _IGL_SUB(v1,p2,p1) \
//   _IGL_SUB(v2,p2,q2) \
//   alpha = _IGL_DOT(v1,N1) / _IGL_DOT(v2,N1); \
//   _IGL_SCALAR(v1,alpha,v2) \
//   _IGL_SUB(source,p2,v1) \
//   _IGL_SUB(v1,p2,p1) \
//   _IGL_SUB(v2,p2,r2) \
//   alpha = _IGL_DOT(v1,N1) / _IGL_DOT(v2,N1); \
//   _IGL_SCALAR(v1,alpha,v2) \
//   _IGL_SUB(target,p2,v1) \
//   return true; \
//       } \
//     } else { \
//       return false; \
//     } \
//   } else { \
//     _IGL_SUB(v2,q2,p1) \
//     _IGL_CROSS(N,v1,v2) \
//     if (_IGL_DOT(v,N) < 0.0) { \
//       return false; \
//     } else { \
//       _IGL_SUB(v1,r1,p1) \
//       _IGL_CROSS(N,v1,v2) \
//       if (_IGL_DOT(v,N) >= 0.0) { \
//   _IGL_SUB(v1,p1,p2) \
//   _IGL_SUB(v2,p1,r1) \
//   alpha = _IGL_DOT(v1,N2) / _IGL_DOT(v2,N2); \
//   _IGL_SCALAR(v1,alpha,v2) \
//   _IGL_SUB(source,p1,v1) \
//   _IGL_SUB(v1,p1,p2) \
//   _IGL_SUB(v2,p1,q1) \
//   alpha = _IGL_DOT(v1,N2) / _IGL_DOT(v2,N2); \
//   _IGL_SCALAR(v1,alpha,v2) \
//   _IGL_SUB(target,p1,v1) \
//   return true; \
//       } else { \
//   _IGL_SUB(v1,p2,p1) \
//   _IGL_SUB(v2,p2,q2) \
//   alpha = _IGL_DOT(v1,N1) / _IGL_DOT(v2,N1); \
//   _IGL_SCALAR(v1,alpha,v2) \
//   _IGL_SUB(source,p2,v1) \
//   _IGL_SUB(v1,p1,p2) \
//   _IGL_SUB(v2,p1,q1) \
//   alpha = _IGL_DOT(v1,N2) / _IGL_DOT(v2,N2); \
//   _IGL_SCALAR(v1,alpha,v2) \
//   _IGL_SUB(target,p1,v1) \
//   return true; \
//       }}}}


template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
            typename DerivedP2,typename DerivedQ2,typename DerivedR2,
            typename DP2,typename DQ2,typename DR2,
            typename DerivedS,typename DerivedT,
            typename DerivedN1,typename DerivedN2
            >
  inline bool _IGL_TRI_TRI_INTER_3D(
    const Eigen::MatrixBase<DerivedP1> &p1, const Eigen::MatrixBase<DerivedQ1> &q1, const Eigen::MatrixBase<DerivedR1> &r1,
    const Eigen::MatrixBase<DerivedP2> &p2, const Eigen::MatrixBase<DerivedQ2> &q2, const Eigen::MatrixBase<DerivedR2> &r2,
    DP2 dp2, DQ2 dq2,DR2 dr2,
    bool & coplanar,
    Eigen::MatrixBase<DerivedS> &source, Eigen::MatrixBase<DerivedT> &target,
    const Eigen::MatrixBase<DerivedN1> &N1,const Eigen::MatrixBase<DerivedN2> &N2
    )
  {
    if (dp2 > 0.0) { 
     if (dq2 > 0.0) return _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,r2,p2,q2,source,target,N1,N2);
     else if (dr2 > 0.0) return _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,q2,r2,p2,source,target,N1,N2);
          else return _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,p2,q2,r2,source,target,N1,N2); }
    else if (dp2 < 0.0) { 
      if (dq2 < 0.0) return _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,r2,p2,q2,source,target,N1,N2);
      else if (dr2 < 0.0) return _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,q2,r2,p2,source,target,N1,N2);
          else return _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,p2,q2,r2,source,target,N1,N2);
    } else { 
      if (dq2 < 0.0) { 
        if (dr2 >= 0.0)  return _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,q2,r2,p2,source,target,N1,N2);
        else return _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,p2,q2,r2,source,target,N1,N2);
    } 
    else if (dq2 > 0.0) {
      if (dr2 > 0.0) return _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,p2,q2,r2,source,target,N1,N2);
      else  return _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,q2,r2,p2,source,target,N1,N2);
    } 
    else  {
      if (dr2 > 0.0) return _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,r2,p2,q2,source,target,N1,N2);
      else if (dr2 < 0.0) return _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,r2,p2,q2,source,target,N1,N2);
      else { 
        coplanar = true;
        return igl::internal::coplanar_tri_tri3d(p1,q1,r1,p2,q2,r2,N1);
     }
  }}

  }

// #define _IGL_TRI_TRI_INTER_3D(p1,q1,r1,p2,q2,r2,dp2,dq2,dr2) { \
//   if (dp2 > 0.0) { \
//      if (dq2 > 0.0) _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,r2,p2,q2) \
//      else if (dr2 > 0.0) _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,q2,r2,p2)\
//      else _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,p2,q2,r2) }\
//   else if (dp2 < 0.0) { \
//     if (dq2 < 0.0) _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,r2,p2,q2)\
//     else if (dr2 < 0.0) _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,q2,r2,p2)\
//     else _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,p2,q2,r2)\
//   } else { \
//     if (dq2 < 0.0) { \
//       if (dr2 >= 0.0)  _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,q2,r2,p2)\
//       else _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,p2,q2,r2)\
//     } \
//     else if (dq2 > 0.0) { \
//       if (dr2 > 0.0) _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,p2,q2,r2)\
//       else  _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,q2,r2,p2)\
//     } \
//     else  { \
//       if (dr2 > 0.0) _IGL_CONSTRUCT_INTERSECTION(p1,q1,r1,r2,p2,q2)\
//       else if (dr2 < 0.0) _IGL_CONSTRUCT_INTERSECTION(p1,r1,q1,r2,p2,q2)\
//       else { \
//         coplanar = true; \
//   return coplanar_tri_tri3d(p1,q1,r1,p2,q2,r2,N1);\
//      } \
//   }} }
  
  } //internal 
} //igl

/*
   The following version computes the segment of intersection of the
   two triangles if it exists. 
   coplanar returns whether the triangles are coplanar
   source and target are the endpoints of the line segment of intersection 
*/

template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2,
typename DerivedS,typename DerivedT>
IGL_INLINE bool igl::tri_tri_intersection_test_3d(
    const Eigen::MatrixBase<DerivedP1> & p1, const Eigen::MatrixBase<DerivedQ1> & q1, const Eigen::MatrixBase<DerivedR1> & r1, 
    const Eigen::MatrixBase<DerivedP2> & p2, const Eigen::MatrixBase<DerivedQ2> & q2, const Eigen::MatrixBase<DerivedR2> & r2,
    bool & coplanar,
    Eigen::MatrixBase<DerivedS> & source, Eigen::MatrixBase<DerivedT> & target )
{
  using Scalar = typename DerivedP1::Scalar;
  using RowVector3D = typename Eigen::Matrix<Scalar, 1, 3>;

  Scalar dp1, dq1, dr1, dp2, dq2, dr2;
  RowVector3D v1, v2, v;
  RowVector3D N1, N2, N;
  // Compute distance signs  of p1, q1 and r1 
  // to the plane of triangle(p2,q2,r2)

  v1=p2-r2;
  v2=q2-r2;
  N2=v1.cross(v2);

  v1=p1-r2;
  dp1 = v1.dot(N2);
  v1=q1-r2;
  dq1 = v1.dot(N2);
  v1=r1-r2;
  dr1 = v1.dot(N2);
  
  coplanar = false;

  if (((dp1 * dq1) > 0.0) && ((dp1 * dr1) > 0.0))  return false; 

  // Compute distance signs  of p2, q2 and r2 
  // to the plane of triangle(p1,q1,r1)

  
  v1=q1-p1;
  v2=r1-p1;
  N1=v1.cross(v2);

  v1=p2-r1;
  dp2 = v1.dot(N1);
  v1=q2-r1;
  dq2 = v1.dot(N1);
  v1=r2-r1;
  dr2 = v1.dot(N1);

  
  if (((dp2 * dq2) > 0.0) && ((dp2 * dr2) > 0.0)) return false;
  // Alec: it's hard to believe this will ever be perfectly robust, but checking
  // 1e-22 against zero seems like a recipe for bad logic.
  // Switching all these 0.0s to epsilons makes other tests fail. My claim is
  // that the series of logic below is a bad way of determining coplanarity, so
  // instead just check for it right away.
  const Scalar eps = igl::EPS<Scalar>();
  using std::abs;
  if(
      abs(dp1) < eps && abs(dq1) < eps && abs(dr1) < eps &&
      abs(dp2) < eps && abs(dq2) < eps && abs(dr2) < eps)
  { 
    coplanar = true;
    return internal::coplanar_tri_tri3d(p1,q1,r1,p2,q2,r2,N1);
  };


  // Permutation in a canonical form of T1's vertices
  if (dp1 > 0.0) {
    if (dq1 > 0.0) return internal::_IGL_TRI_TRI_INTER_3D(r1,p1,q1,p2,r2,q2,dp2,dr2,dq2,coplanar,source,target,N1,N2);
    else if (dr1 > 0.0) return internal::_IGL_TRI_TRI_INTER_3D(q1,r1,p1,p2,r2,q2,dp2,dr2,dq2,coplanar,source,target,N1,N2);
  
    else return internal::_IGL_TRI_TRI_INTER_3D(p1,q1,r1,p2,q2,r2,dp2,dq2,dr2,coplanar,source,target,N1,N2);
  } else if (dp1 < 0.0) {
    if (dq1 < 0.0) return internal::_IGL_TRI_TRI_INTER_3D(r1,p1,q1,p2,q2,r2,dp2,dq2,dr2,coplanar,source,target,N1,N2);
    else if (dr1 < 0.0) return internal::_IGL_TRI_TRI_INTER_3D(q1,r1,p1,p2,q2,r2,dp2,dq2,dr2,coplanar,source,target,N1,N2);
    else return internal::_IGL_TRI_TRI_INTER_3D(p1,q1,r1,p2,r2,q2,dp2,dr2,dq2,coplanar,source,target,N1,N2);
  } else {
    if (dq1 < 0.0) {
      if (dr1 >= 0.0) return internal::_IGL_TRI_TRI_INTER_3D(q1,r1,p1,p2,r2,q2,dp2,dr2,dq2,coplanar,source,target,N1,N2);
      else return internal::_IGL_TRI_TRI_INTER_3D(p1,q1,r1,p2,q2,r2,dp2,dq2,dr2,coplanar,source,target,N1,N2);
    }
    else if (dq1 > 0.0) {
      if (dr1 > 0.0) return internal::_IGL_TRI_TRI_INTER_3D(p1,q1,r1,p2,r2,q2,dp2,dr2,dq2,coplanar,source,target,N1,N2);
      else return internal::_IGL_TRI_TRI_INTER_3D(q1,r1,p1,p2,q2,r2,dp2,dq2,dr2,coplanar,source,target,N1,N2);
    }
    else  {
      if (dr1 > 0.0) return internal::_IGL_TRI_TRI_INTER_3D(r1,p1,q1,p2,q2,r2,dp2,dq2,dr2,coplanar,source,target,N1,N2);
      else if (dr1 < 0.0) return internal::_IGL_TRI_TRI_INTER_3D(r1,p1,q1,p2,r2,q2,dp2,dr2,dq2,coplanar,source,target,N1,N2);
      else {
        // triangles are co-planar (should have been caught above).

        coplanar = true;
        return internal::coplanar_tri_tri3d(p1,q1,r1,p2,q2,r2,N1);
      }
    }
  }
};



namespace igl {
  namespace internal {
/*
*
*  Two dimensional Triangle-Triangle Overlap Test    
*
*/


/* some 2D macros */

//#define _IGL_ORIENT_2D(a, b, c)  ((a[0]-c[0])*(b[1]-c[1])-(a[1]-c[1])*(b[0]-c[0]))
template <typename DerivedA,typename DerivedB,typename DerivedC>
  inline typename Eigen::MatrixBase<DerivedA>::Scalar _IGL_ORIENT_2D(
    const Eigen::MatrixBase<DerivedA> &  a, 
    const Eigen::MatrixBase<DerivedB> &  b,
    const Eigen::MatrixBase<DerivedC> &  c)
    {
      return  ((a[0]-c[0])*(b[1]-c[1])-(a[1]-c[1])*(b[0]-c[0]));
    }


template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
          typename DerivedP2,typename DerivedQ2,typename DerivedR2>
bool _IGL_INTERSECTION_TEST_VERTEX(
  const Eigen::MatrixBase<DerivedP1> & P1, const Eigen::MatrixBase<DerivedQ1> & Q1, const Eigen::MatrixBase<DerivedR1> & R1,  
  const Eigen::MatrixBase<DerivedP2> & P2, const Eigen::MatrixBase<DerivedQ2> & Q2, const Eigen::MatrixBase<DerivedR2> & R2
)
{
  if (_IGL_ORIENT_2D(R2,P2,Q1) >= 0.0)
    if (_IGL_ORIENT_2D(R2,Q2,Q1) <= 0.0)
      if (_IGL_ORIENT_2D(P1,P2,Q1) > 0.0) {
        if (_IGL_ORIENT_2D(P1,Q2,Q1) <= 0.0) return true;
        else return false;} 
      else {
        if (_IGL_ORIENT_2D(P1,P2,R1) >= 0.0)
          if (_IGL_ORIENT_2D(Q1,R1,P2) >= 0.0) return true; 
          else return false;
        else return false;
      }
      else 
        if (_IGL_ORIENT_2D(P1,Q2,Q1) <= 0.0)
          if (_IGL_ORIENT_2D(R2,Q2,R1) <= 0.0)
            if (_IGL_ORIENT_2D(Q1,R1,Q2) >= 0.0) return true; 
            else return false;
          else return false;
        else return false;
      else
        if (_IGL_ORIENT_2D(R2,P2,R1) >= 0.0) 
          if (_IGL_ORIENT_2D(Q1,R1,R2) >= 0.0)
            if (_IGL_ORIENT_2D(P1,P2,R1) >= 0.0) return true;
            else return false;
          else 
            if (_IGL_ORIENT_2D(Q1,R1,Q2) >= 0.0) {
              if (_IGL_ORIENT_2D(R2,R1,Q2) >= 0.0) return true; 
              else return false; 
            }
        else return false; 
  else  return false; 
}

// #define INTERSECTION_TEST_VERTEX(P1, Q1, R1, P2, Q2, R2) {\
//   if (_IGL_ORIENT_2D(R2,P2,Q1) >= 0.0)\
//     if (_IGL_ORIENT_2D(R2,Q2,Q1) <= 0.0)\
//       if (_IGL_ORIENT_2D(P1,P2,Q1) > 0.0) {\
//   if (_IGL_ORIENT_2D(P1,Q2,Q1) <= 0.0) return true; \
//   else return false;} else {\
//   if (_IGL_ORIENT_2D(P1,P2,R1) >= 0.0)\
//     if (_IGL_ORIENT_2D(Q1,R1,P2) >= 0.0) return true; \
//     else return false;\
//   else return false;}\
//     else \
//       if (_IGL_ORIENT_2D(P1,Q2,Q1) <= 0.0)\
//   if (_IGL_ORIENT_2D(R2,Q2,R1) <= 0.0)\
//     if (_IGL_ORIENT_2D(Q1,R1,Q2) >= 0.0) return true; \
//     else return false;\
//   else return false;\
//       else return false;\
//   else\
//     if (_IGL_ORIENT_2D(R2,P2,R1) >= 0.0) \
//       if (_IGL_ORIENT_2D(Q1,R1,R2) >= 0.0)\
//   if (_IGL_ORIENT_2D(P1,P2,R1) >= 0.0) return true;\
//   else return false;\
//       else \
//   if (_IGL_ORIENT_2D(Q1,R1,Q2) >= 0.0) {\
//     if (_IGL_ORIENT_2D(R2,R1,Q2) >= 0.0) return true; \
//     else return false; }\
//   else return false; \
//     else  return false; \
//  };

template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
          typename DerivedP2,typename DerivedQ2,typename DerivedR2>
bool _IGL_INTERSECTION_TEST_EDGE(
  const Eigen::MatrixBase<DerivedP1> & P1, const Eigen::MatrixBase<DerivedQ1> & Q1, const Eigen::MatrixBase<DerivedR1> & R1,  
  const Eigen::MatrixBase<DerivedP2> & P2, const Eigen::MatrixBase<DerivedQ2> & /*Q2*/, const Eigen::MatrixBase<DerivedR2> & R2
)
{
  if (_IGL_ORIENT_2D(R2,P2,Q1) >= 0.0) {
    if (_IGL_ORIENT_2D(P1,P2,Q1) >= 0.0) {
        if (_IGL_ORIENT_2D(P1,Q1,R2) >= 0.0) return true;
        else return false;} else { 
      if (_IGL_ORIENT_2D(Q1,R1,P2) >= 0.0){ 
  if (_IGL_ORIENT_2D(R1,P1,P2) >= 0.0) return true; else return false;} 
      else return false; } 
  } else {
    if (_IGL_ORIENT_2D(R2,P2,R1) >= 0.0) {
      if (_IGL_ORIENT_2D(P1,P2,R1) >= 0.0) {
  if (_IGL_ORIENT_2D(P1,R1,R2) >= 0.0) return true;  
  else {
    if (_IGL_ORIENT_2D(Q1,R1,R2) >= 0.0) return true; else return false;}}
      else  return false; }
    else return false; }
}


// #define _IGL_INTERSECTION_TEST_EDGE(P1, Q1, R1, P2, Q2, R2) { \
//   if (_IGL_ORIENT_2D(R2,P2,Q1) >= 0.0) {\
//     if (_IGL_ORIENT_2D(P1,P2,Q1) >= 0.0) { \
//         if (_IGL_ORIENT_2D(P1,Q1,R2) >= 0.0) return true; \
//         else return false;} else { \
//       if (_IGL_ORIENT_2D(Q1,R1,P2) >= 0.0){ \
//   if (_IGL_ORIENT_2D(R1,P1,P2) >= 0.0) return true; else return false;} \
//       else return false; } \
//   } else {\
//     if (_IGL_ORIENT_2D(R2,P2,R1) >= 0.0) {\
//       if (_IGL_ORIENT_2D(P1,P2,R1) >= 0.0) {\
//   if (_IGL_ORIENT_2D(P1,R1,R2) >= 0.0) return true;  \
//   else {\
//     if (_IGL_ORIENT_2D(Q1,R1,R2) >= 0.0) return true; else return false;}}\
//       else  return false; }\
//     else return false; }}




template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2>
IGL_INLINE bool ccw_tri_tri_intersection_2d(
  const Eigen::MatrixBase<DerivedP1> &p1, const Eigen::MatrixBase<DerivedQ1> &q1, const Eigen::MatrixBase<DerivedR1> &r1,
  const Eigen::MatrixBase<DerivedP2> &p2, const Eigen::MatrixBase<DerivedQ2> &q2, const Eigen::MatrixBase<DerivedR2> &r2) 
{
  if ( _IGL_ORIENT_2D(p2,q2,p1) >= 0.0 ) {
    if ( _IGL_ORIENT_2D(q2,r2,p1) >= 0.0 ) {
      if ( _IGL_ORIENT_2D(r2,p2,p1) >= 0.0 ) return true;
      else return _IGL_INTERSECTION_TEST_EDGE(p1,q1,r1,p2,q2,r2);
    } else {  
      if ( _IGL_ORIENT_2D(r2,p2,p1) >= 0.0 ) 
      return _IGL_INTERSECTION_TEST_EDGE(p1,q1,r1,r2,p2,q2);
      else return _IGL_INTERSECTION_TEST_VERTEX(p1,q1,r1,p2,q2,r2);}}
  else {
    if ( _IGL_ORIENT_2D(q2,r2,p1) >= 0.0 ) {
      if ( _IGL_ORIENT_2D(r2,p2,p1) >= 0.0 ) 
        return _IGL_INTERSECTION_TEST_EDGE(p1,q1,r1,q2,r2,p2);
      else  return _IGL_INTERSECTION_TEST_VERTEX(p1,q1,r1,q2,r2,p2);}
    else return _IGL_INTERSECTION_TEST_VERTEX(p1,q1,r1,r2,p2,q2);}
};

  }//internal
} //igl


template <typename DerivedP1,typename DerivedQ1,typename DerivedR1,
typename DerivedP2,typename DerivedQ2,typename DerivedR2>
IGL_INLINE bool igl::tri_tri_overlap_test_2d(
  const Eigen::MatrixBase<DerivedP1> &p1, const Eigen::MatrixBase<DerivedQ1> &q1, const Eigen::MatrixBase<DerivedR1> &r1,
  const Eigen::MatrixBase<DerivedP2> &p2, const Eigen::MatrixBase<DerivedQ2> &q2, const Eigen::MatrixBase<DerivedR2> &r2) 
{
  if ( igl::internal::_IGL_ORIENT_2D(p1,q1,r1) < 0.0)
    if ( igl::internal::_IGL_ORIENT_2D(p2,q2,r2) < 0.0)
      return igl::internal::ccw_tri_tri_intersection_2d(p1,r1,q1,p2,r2,q2);
    else
      return igl::internal::ccw_tri_tri_intersection_2d(p1,r1,q1,p2,q2,r2);
  else
    if ( igl::internal::_IGL_ORIENT_2D(p2,q2,r2) < 0.0 )
      return igl::internal::ccw_tri_tri_intersection_2d(p1,q1,r1,p2,r2,q2);
    else
      return igl::internal::ccw_tri_tri_intersection_2d(p1,q1,r1,p2,q2,r2);
};

#endif //IGL_TRI_TRI_INTERSECT_CPP


#ifdef IGL_STATIC_LIBRARY
// Explicit template specialization
template bool igl::tri_tri_intersection_test_3d<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, bool&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template bool igl::tri_tri_intersection_test_3d<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Matrix<double, 1, -1, 1, 1, -1>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, bool&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template bool igl::tri_tri_intersection_test_3d<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> > const&, bool&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template bool igl::tri_tri_intersection_test_3d<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, bool&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template bool igl::tri_tri_intersection_test_3d<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, bool&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);

template bool igl::tri_tri_overlap_test_3d<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> >(Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&);
template bool igl::tri_tri_overlap_test_3d<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Matrix<double, 1, -1, 1, 1, -1>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> >(Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&);
template bool igl::tri_tri_overlap_test_3d<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> >(Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&);
#endif
