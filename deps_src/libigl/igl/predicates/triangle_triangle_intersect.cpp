#include "triangle_triangle_intersect.h"
#include "predicates.h"
#include <Eigen/Core>
#include <Eigen/Geometry>

template <typename Vector3D>
IGL_INLINE  bool igl::predicates::triangle_triangle_intersect(
  const Vector3D & p1,
  const Vector3D & q1,
  const Vector3D & r1,
  const Vector3D & p2,
  const Vector3D & q2,
  const Vector3D & r2,
  bool & coplanar)
{
  coplanar = false;
  // yet another translation of tri_tri_intersect.c  [Guigue & Devillers]
  exactinit();
  using Vector2D = Eigen::Matrix<typename Vector3D::Scalar,2,1>;

  constexpr Orientation COPLANAR = igl::predicates::Orientation::COPLANAR;
  constexpr Orientation NEGATIVE = igl::predicates::Orientation::NEGATIVE;
  constexpr Orientation POSITIVE = igl::predicates::Orientation::POSITIVE;

  // Determine for each vertex of one triangle if it's above, below or on the
  // plane of the other triangle.

  // SUB(v1,p2,r2)
  // SUB(v2,q2,r2)
  // CROSS(N2,v1,v2)
  // SUB(v1,p1,r2)
  // dp1 = DOT(v1,N2);
  // dp1 = (p1-r2).dot( (p2-r2).cross(q2-r2) );
  const Orientation dp1 = orient3d(p2,q2,r2,p1);
  const Orientation dq1 = orient3d(p2,q2,r2,q1);
  const Orientation dr1 = orient3d(p2,q2,r2,r1);

  const auto same_non_coplanar = [&NEGATIVE,&COPLANAR,&POSITIVE](
    const Orientation a, 
    const Orientation b, 
    const Orientation c)
  {
    return (a == POSITIVE && b == POSITIVE && c == POSITIVE) ||
           (a == NEGATIVE && b == NEGATIVE && c == NEGATIVE);
  };
  if(same_non_coplanar(dp1,dq1,dr1)) { return false; }


  const Orientation dp2 = orient3d(p1,q1,r1,p2);
  const Orientation dq2 = orient3d(p1,q1,r1,q2);
  const Orientation dr2 = orient3d(p1,q1,r1,r2);

  // Theoreticaly, this should have already been fired above
  if(same_non_coplanar(dp2,dq2,dr2)) { return false; }

  const auto tri_tri_overlap_test_2d = [&NEGATIVE,&COPLANAR,&POSITIVE](
      const Vector2D & p1,
      const Vector2D & q1,
      const Vector2D & r1,
      const Vector2D & p2,
      const Vector2D & q2,
      const Vector2D & r2)->bool
  {
    const auto ccw_tri_tri_intersection_2d = [&NEGATIVE,&COPLANAR,&POSITIVE](
      const Vector2D & p1,
      const Vector2D & q1,
      const Vector2D & r1,
      const Vector2D & p2,
      const Vector2D & q2,
      const Vector2D & r2)->bool
    {
      const auto INTERSECTION_TEST_VERTEX = [&NEGATIVE,&COPLANAR,&POSITIVE](
        const Vector2D & P1,
        const Vector2D & Q1,
        const Vector2D & R1,
        const Vector2D & P2,
        const Vector2D & Q2,
        const Vector2D & R2)->bool
      {
        if (orient2d(R2,P2,Q1) != NEGATIVE)
          if (orient2d(R2,Q2,Q1) != POSITIVE)
            if (orient2d(P1,P2,Q1) == POSITIVE) {
              if (orient2d(P1,Q2,Q1) != POSITIVE) return 1; 
              else return 0;} else {
                if (orient2d(P1,P2,R1) != NEGATIVE)
                  if (orient2d(Q1,R1,P2) != NEGATIVE) return 1; 
                  else return 0;
                else return 0;}
            else 
              if (orient2d(P1,Q2,Q1) != POSITIVE)
                if (orient2d(R2,Q2,R1) != POSITIVE)
                  if (orient2d(Q1,R1,Q2) != NEGATIVE) return 1; 
                  else return 0;
                else return 0;
              else return 0;
          else
            if (orient2d(R2,P2,R1) != NEGATIVE) 
              if (orient2d(Q1,R1,R2) != NEGATIVE)
                if (orient2d(P1,P2,R1) != NEGATIVE) return 1;
                else return 0;
              else 
                if (orient2d(Q1,R1,Q2) != NEGATIVE) {
                  if (orient2d(R2,R1,Q2) != NEGATIVE) return 1; 
                  else return 0; }
                else return 0; 
            else  return 0; 
      };
      const auto INTERSECTION_TEST_EDGE = [&NEGATIVE,&COPLANAR,&POSITIVE](
        const Vector2D & P1,
        const Vector2D & Q1,
        const Vector2D & R1,
        const Vector2D & P2,
        const Vector2D & Q2,
        const Vector2D & R2)->bool
      {
        if (orient2d(R2,P2,Q1) != NEGATIVE) {
          if (orient2d(P1,P2,Q1) != NEGATIVE) { 
            if (orient2d(P1,Q1,R2) != NEGATIVE) return 1; 
            else return 0;} else { 
              if (orient2d(Q1,R1,P2) != NEGATIVE){ 
                if (orient2d(R1,P1,P2) != NEGATIVE) return 1; else return 0;} 
              else return 0; } 
        } else {
          if (orient2d(R2,P2,R1) != NEGATIVE) {
            if (orient2d(P1,P2,R1) != NEGATIVE) {
              if (orient2d(P1,R1,R2) != NEGATIVE) return 1;  
              else {
                if (orient2d(Q1,R1,R2) != NEGATIVE) return 1; else return 0;}}
            else  return 0; }
          else return 0; }
      };
      if ( orient2d(p2,q2,p1) != NEGATIVE ) {
        if ( orient2d(q2,r2,p1) != NEGATIVE ) {
          if ( orient2d(r2,p2,p1) != NEGATIVE ) return 1;
          else return INTERSECTION_TEST_EDGE(p1,q1,r1,p2,q2,r2);
        } else {  
          if ( orient2d(r2,p2,p1) != NEGATIVE ) 
      return INTERSECTION_TEST_EDGE(p1,q1,r1,r2,p2,q2);
          else return INTERSECTION_TEST_VERTEX(p1,q1,r1,p2,q2,r2);}}
      else {
        if ( orient2d(q2,r2,p1) != NEGATIVE ) {
          if ( orient2d(r2,p2,p1) != NEGATIVE ) 
      return INTERSECTION_TEST_EDGE(p1,q1,r1,q2,r2,p2);
          else  return INTERSECTION_TEST_VERTEX(p1,q1,r1,q2,r2,p2);}
        else return INTERSECTION_TEST_VERTEX(p1,q1,r1,r2,p2,q2);}
      return false;
    };
    if ( orient2d(p1,q1,r1) == NEGATIVE )
      if ( orient2d(p2,q2,r2) == NEGATIVE )
        return ccw_tri_tri_intersection_2d(p1,r1,q1,p2,r2,q2);
      else
        return ccw_tri_tri_intersection_2d(p1,r1,q1,p2,q2,r2);
    else
      if ( orient2d(p2,q2,r2) == NEGATIVE )
        return ccw_tri_tri_intersection_2d(p1,q1,r1,p2,r2,q2);
      else
        return ccw_tri_tri_intersection_2d(p1,q1,r1,p2,q2,r2);

  };

  const auto coplanar_tri_tri3d = [&tri_tri_overlap_test_2d,&NEGATIVE,&COPLANAR,&POSITIVE](
    const Vector3D & p1,
    const Vector3D & q1,
    const Vector3D & r1,
    const Vector3D & p2,
    const Vector3D & q2,
    const Vector3D & r2)->bool
  {
    Vector3D normal_1 = (q1-p1).cross(r1-p1);
    const auto n_x = ((normal_1[0]<0)?-normal_1[0]:normal_1[0]);
    const auto n_y = ((normal_1[1]<0)?-normal_1[1]:normal_1[1]);
    const auto n_z = ((normal_1[2]<0)?-normal_1[2]:normal_1[2]);
    Vector2D P1,Q1,R1,P2,Q2,R2;
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
    exit(1);
    return false;
  };

  const auto TRI_TRI_3D = [&coplanar_tri_tri3d,&coplanar,&NEGATIVE,&COPLANAR,&POSITIVE](
    const Vector3D & p1,
    const Vector3D & q1,
    const Vector3D & r1,
    const Vector3D & p2,
    const Vector3D & q2,
    const Vector3D & r2,
    const Orientation dp2,
    const Orientation dq2,
    const Orientation dr2)->bool
  {
    const auto CHECK_MIN_MAX = [&NEGATIVE,&COPLANAR,&POSITIVE](
      const Vector3D & p1,
      const Vector3D & q1,
      const Vector3D & r1,
      const Vector3D & p2,
      const Vector3D & q2,
      const Vector3D & r2)->bool
    {
       if (orient3d(p2,p1,q1,q2) == POSITIVE) { return false; }
       if (orient3d(p2,r1,p1,r2) == POSITIVE) { return false; }
       return true;
    };



    if (dp2 == POSITIVE) { 
       if (dq2 == POSITIVE) return CHECK_MIN_MAX(p1,r1,q1,r2,p2,q2) ;
       else if (dr2 == POSITIVE) return CHECK_MIN_MAX(p1,r1,q1,q2,r2,p2);
       else return CHECK_MIN_MAX(p1,q1,r1,p2,q2,r2);
    } else if (dp2 == NEGATIVE) { 
      if (dq2 == NEGATIVE) return CHECK_MIN_MAX(p1,q1,r1,r2,p2,q2);
      else if (dr2 == NEGATIVE) return CHECK_MIN_MAX(p1,q1,r1,q2,r2,p2);
      else return CHECK_MIN_MAX(p1,r1,q1,p2,q2,r2);
    } else { 
      if (dq2 == NEGATIVE) { 
        if (dr2 == POSITIVE || dr2 == COPLANAR)  return CHECK_MIN_MAX(p1,r1,q1,q2,r2,p2);
        else return CHECK_MIN_MAX(p1,q1,r1,p2,q2,r2);
      } 
      else if (dq2 == POSITIVE) { 
        if (dr2 == POSITIVE) return CHECK_MIN_MAX(p1,r1,q1,p2,q2,r2);
        else  return CHECK_MIN_MAX(p1,q1,r1,q2,r2,p2);
      } 
      else  { 
        if (dr2 == POSITIVE) return CHECK_MIN_MAX(p1,q1,r1,r2,p2,q2);
        else if (dr2 == NEGATIVE) return CHECK_MIN_MAX(p1,r1,q1,r2,p2,q2);
        else
        {
          coplanar = coplanar_tri_tri3d(p1,q1,r1,p2,q2,r2);
          return coplanar;
        }
      }
    }
  };

  if (dp1 == POSITIVE) {
    if (dq1 == POSITIVE) return TRI_TRI_3D(r1,p1,q1,p2,r2,q2,dp2,dr2,dq2);
    else if (dr1 == POSITIVE) return TRI_TRI_3D(q1,r1,p1,p2,r2,q2,dp2,dr2,dq2)  ;
    else return TRI_TRI_3D(p1,q1,r1,p2,q2,r2,dp2,dq2,dr2);
  } else if (dp1 == NEGATIVE) {
    if (dq1 == NEGATIVE) return TRI_TRI_3D(r1,p1,q1,p2,q2,r2,dp2,dq2,dr2);
    else if (dr1 == NEGATIVE) return TRI_TRI_3D(q1,r1,p1,p2,q2,r2,dp2,dq2,dr2);
    else return TRI_TRI_3D(p1,q1,r1,p2,r2,q2,dp2,dr2,dq2);
  } else {
    if (dq1 == NEGATIVE) {
      // why COPLANAR here? It seems so haphazard.
      if (dr1 == POSITIVE || dr1 == COPLANAR) return TRI_TRI_3D(q1,r1,p1,p2,r2,q2,dp2,dr2,dq2);
      else return TRI_TRI_3D(p1,q1,r1,p2,q2,r2,dp2,dq2,dr2);
    }
    else if (dq1 == POSITIVE) {
      if (dr1 == POSITIVE) return TRI_TRI_3D(p1,q1,r1,p2,r2,q2,dp2,dr2,dq2);
      else return TRI_TRI_3D(q1,r1,p1,p2,q2,r2,dp2,dq2,dr2);
    }
    else  {
      if (dr1 == POSITIVE) return TRI_TRI_3D(r1,p1,q1,p2,q2,r2,dp2,dq2,dr2);
      else if (dr1 == NEGATIVE) return TRI_TRI_3D(r1,p1,q1,p2,r2,q2,dp2,dr2,dq2);
      else
      {
        coplanar = coplanar_tri_tri3d(p1,q1,r1,p2,q2,r2);
        return coplanar;
      }
    }
  }

}

#ifdef IGL_STATIC_LIBRARY
// Explicit template specialization
// template using Eigen::Vector3d.
template bool igl::predicates::triangle_triangle_intersect<Eigen::Vector3d>(
    const Eigen::Vector3d & p1,
    const Eigen::Vector3d & q1,
    const Eigen::Vector3d & r1,
    const Eigen::Vector3d & p2,
    const Eigen::Vector3d & q2,
    const Eigen::Vector3d & r2,
    bool & coplanar);
template bool igl::predicates::triangle_triangle_intersect<Eigen::RowVector3d>(
    const Eigen::RowVector3d & p1,
    const Eigen::RowVector3d & q1,
    const Eigen::RowVector3d & r1,
    const Eigen::RowVector3d & p2,
    const Eigen::RowVector3d & q2,
    const Eigen::RowVector3d & r2,
    bool & coplanar);
#endif
