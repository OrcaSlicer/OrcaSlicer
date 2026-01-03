#include "triangle_triangle_intersect_shared_vertex.h"
#include "ray_triangle_intersect.h"
#include "barycentric_coordinates.h"
#include "matlab_format.h"
#include <Eigen/Geometry>
#include <iostream>
#include <iomanip>
#include <stdio.h>
// std::signbit
#include <cmath>

//#define IGL_TRIANGLE_TRIANGLE_INTERSECT_SHARED_VERTEX_DEBUG
#ifdef IGL_TRIANGLE_TRIANGLE_INTERSECT_SHARED_VERTEX_DEBUG
// CGAL::Epeck
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#warning "🐌🐌🐌🐌🐌🐌🐌🐌 Slow debug mode for igl::triangle_triangle_intersect"
#endif

template <
  typename DerivedV,
  typename DerivedF,
  typename Derivedp>
IGL_INLINE bool igl::triangle_triangle_intersect_shared_vertex(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const int f,
  const int sf,
  const int c,
  const Eigen::MatrixBase<Derivedp> & p,
  const int g,
  const int sg,
  const typename DerivedV::Scalar epsilon)
{
  static_assert(
    std::is_same<typename DerivedV::Scalar,typename Derivedp::Scalar>::value, 
    "V and p should have same Scalar type");
  assert(V.cols() == 3);
  assert(p.cols() == 3);
#ifdef IGL_TRIANGLE_TRIANGLE_INTERSECT_DEBUG
  using Kernel = CGAL::Epeck;
  typedef CGAL::Point_3<Kernel>    Point_3;
  typedef CGAL::Segment_3<Kernel>  Segment_3;
  typedef CGAL::Triangle_3<Kernel> Triangle_3;
    bool cgal_found_intersection = false;
    Point_3 Vg[3];
    Point_3 Vf[3];
    for(int i = 0;i<3;i++)
    {
      Vg[i] = Point_3(V(F(g,i),0),V(F(g,i),1),V(F(g,i),2));
      if(i == c)
      {
        Vf[i] = Point_3(p(0),p(1),p(2));
      }else
      {
        Vf[i] = Point_3(V(F(f,i),0),V(F(f,i),1),V(F(f,i),2));
      }
    }
    Triangle_3 Tg(Vg[0],Vg[1],Vg[2]);
    Triangle_3 Tf(Vf[0],Vf[1],Vf[2]);
#endif
  constexpr bool stinker = false;
  bool found_intersection = false;
  // If they share a vertex and intersect, then an opposite edge must
  // stab through the other triangle.

  // Using ray_triangle_intersect now, not sure we need this copy or cast
  Eigen::RowVector3d g0 = V.row(F(g,0)).template cast<double>();
  Eigen::RowVector3d g1 = V.row(F(g,1)).template cast<double>();
  Eigen::RowVector3d g2 = V.row(F(g,2)).template cast<double>();
  Eigen::RowVector3d fs;
  if(((sf+1)%3)==c)
  {
    fs = p;
  }else
  {
    fs = V.row(F(f,(sf+1)%3));
  }
  Eigen::RowVector3d fd;
  if( ((sf+2)%3)==c )
  {
    fd = p.template cast<double>();
  }else
  {
    fd = V.row(F(f,(sf+2)%3)).template cast<double>();
  }
  Eigen::RowVector3d fdir = fd - fs;
  double t,u,v;

  if(stinker)
  {
    std::cout<<"T = ["<<g0<<";" <<g1<<";"<<g2<<"];"<<std::endl;
    std::cout<<"src = [" <<fs<<"];"<<std::endl;
    std::cout<<"dir = [" <<fdir<<"];"<<std::endl;
  }
  // p = (1-u-v)*a + u*b + v*c
  const auto bary = [](
      const Eigen::RowVector3d & p,
      const Eigen::RowVector3d & a,
      const Eigen::RowVector3d & b,
      const Eigen::RowVector3d & c,
      double & u,
      double & v)
  {
    const auto v0 = (b-a).eval();
    const auto v1 = (c-a).eval();
    const auto v2 = (p-a).eval();
    const double d00 = v0.dot(v0);
    const double d01 = v0.dot(v1);
    const double d11 = v1.dot(v1);
    const double d20 = v2.dot(v0);
    const double d21 = v2.dot(v1);
    const double denom = d00 * d11 - d01 * d01;
    u = (d11 * d20 - d01 * d21) / denom;
    v = (d00 * d21 - d01 * d20) / denom;
    // Equivalent:
    //Eigen::RowVector3d l;
    //igl::barycentric_coordinates(p,a,b,c,l);
    //u = l(1); v = l(2);
  };

  // Does the segment (A,B) intersect the triangle (0,0),(1,0),(0,1)?
  const auto intersect_unit_helper = [](
      const Eigen::RowVector2d & A,
      const Eigen::RowVector2d & B) -> bool
  {
    // Check if P is inside (0,0),(1,0),(0,1) triangle
    const auto inside_unit = []( const Eigen::RowVector2d & P) -> bool
    {
      return P(0) >= 0 && P(1) >= 0 && P(0) + P(1) <= 1;
    };
    if(inside_unit(A) || inside_unit(B))
    { 
      return true; 
    }

    const auto open_interval_contains_zero = [](
        const double a, const double b) -> bool
    {
      // handle case where either is 0.0 or -0.0
      if(a==0 || b==0) { return false; }
      return std::signbit(a) != std::signbit(b);
    };

    // Now check if the segment intersects any of the edges.
    // Does A-B intesect X-axis?
    if(open_interval_contains_zero(A(1),B(1)))
    {
      assert((A(1) - B(1)) != 0);
      // A and B are on opposite sides of the X-axis
      const double t = A(1) / (A(1) - B(1));
      const double x = A(0) + t * (B(0) - A(0));
      if(x >= 0 && x <= 1)
      {
        return true;
      }
    }
    // Does A-B intesect Y-axis?
    if(open_interval_contains_zero(A(0),B(0)))
    {
      assert((A(0) - B(0)) != 0);
      // A and B are on opposite sides of the Y-axis
      const double t = A(0) / (A(0) - B(0));
      const double y = A(1) + t * (B(1) - A(1));
      if(y >= 0 && y <= 1)
      {
        return true;
      }
    }
    // Does A-B intersect the line x+y=1?
    if(open_interval_contains_zero(A(0) + A(1) - 1.0,B(0) + B(1) - 1.0))
    {
      assert((A(0) + A(1) - 1.0) - (B(0) + B(1) - 1.0) != 0);
      // A and B are on opposite sides of the line x+y=1
      // x+y=1
      // A(0) + t * (B(0) - A(0)) + A(1) + t * (B(1) - A(1)) = 1
      // t * (B(0) - A(0) + B(1) - A(1)) = 1 - A(0) - A(1)
      const double  t = (1 - A(0) - A(1)) / (B(0) - A(0) + B(1) - A(1));
      const double y = A(1) + t * (B(1) - A(1));
      if(y >= 0 && y <= 1)
      {
        return true;
      }
    }
    return false;
  };
  const auto intersect_unit = [&intersect_unit_helper](
      const Eigen::RowVector2d & A,
      const Eigen::RowVector2d & B) -> bool
  {
#ifdef IGL_TRIANGLE_TRIANGLE_INTERSECT_SHARED_VERTEX_DEBUG
    printf("A=[%g,%g];B=[%g,%g];\n",
        A(0),A(1),B(0),B(1));
#endif
    const bool ret = intersect_unit_helper(A,B);
    return ret;
  };
  const auto point_on_plane = [&epsilon](
      const Eigen::RowVector3d & p,
      const Eigen::RowVector3d & a,
      const Eigen::RowVector3d & b,
      const Eigen::RowVector3d & c) -> bool
  {
    const auto n = (b-a).cross(c-a);
    const auto d = n.dot(p-a);
    return std::abs(d) < epsilon*n.stableNorm();
  };


  //if(intersect_triangle1(
  //      fs.data(),fdir.data(),
  //      g0.data(),g1.data(),g2.data(),
  //      &t,&u,&v))
  bool parallel = false;
  if(ray_triangle_intersect(
        fs,fdir,
        g0,g1,g2,
        epsilon,
        t,u,v,parallel))
  {
    found_intersection = t > 0.0 && t<1.0+epsilon;
  }else if(parallel)
  {
    if(stinker){ printf("    parallel\n"); }
    if(point_on_plane(fs,g0,g1,g2))
    {
      if(stinker){ printf("    coplanar\n"); }
      // deal with parallel
      Eigen::RowVector2d s2,d2;
      bary(fs,g0,g1,g2,s2(0),s2(1));
      bary(fd,g0,g1,g2,d2(0),d2(1));
      found_intersection = intersect_unit(s2,d2);
    }
  }
  if(stinker){ printf("    found_intersection: %d\n",found_intersection); }

  if(!found_intersection)
  {
    Eigen::RowVector3d fv[3];
    fv[0] = V.row(F(f,0)).template cast<double>();
    fv[1] = V.row(F(f,1)).template cast<double>();
    fv[2] = V.row(F(f,2)).template cast<double>();
    fv[c] = p.template cast<double>();
    Eigen::RowVector3d gs = V.row(F(g,(sg+1)%3)).template cast<double>();
    Eigen::RowVector3d gd = V.row(F(g,(sg+2)%3)).template cast<double>();
    Eigen::RowVector3d gdir = gd - gs;

    if(stinker)
    {
      std::cout<<"T = ["<<fv[0]<<";"<<fv[1]<<";"<<fv[2]<<"];"<<std::endl;
      std::cout<<"src = [" <<gs<<"];"<<std::endl;
      std::cout<<"dir = [" <<gdir<<"];"<<std::endl;
    }
    if(ray_triangle_intersect(
          gs,gdir,
          fv[0],fv[1],fv[2],
          epsilon,
          t,u,v,parallel))
    {
      found_intersection = t > 0 && t<1+epsilon;
    }else if(parallel)
    {
      if(stinker){ printf("    parallel2\n"); }
      if(point_on_plane(gs,fv[0],fv[1],fv[2]))
      {
        if(stinker){ printf("    coplanar\n"); }
        // deal with parallel
        //assert(false);
        Eigen::RowVector2d s2,d2;
        bary(gs,fv[0],fv[1],fv[2],s2(0),s2(1));
        bary(gd,fv[0],fv[1],fv[2],d2(0),d2(1));
        found_intersection = intersect_unit(s2,d2);
      }
    }
  }
  if(stinker){ printf("    found_intersection2: %d\n",found_intersection); }
#ifdef IGL_TRIANGLE_TRIANGLE_INTERSECT_SHARED_VERTEX_DEBUG
  if(CGAL::do_intersect(Tg,Tf))
  {
    CGAL::Object obj = CGAL::intersection(Tg,Tf);
    if(const Segment_3 *iseg = CGAL::object_cast<Segment_3 >(&obj))
    {
      printf("  ✅ sure it's a segment\n");
      cgal_found_intersection = true;
    }else if(const Point_3 *ipoint = CGAL::object_cast<Point_3 >(&obj))
    {
      printf("  ❌ it's just the point.\n");
    } else if(const Triangle_3 *itri = CGAL::object_cast<Triangle_3 >(&obj))
    {
      cgal_found_intersection = true;
      printf("  ✅ sure it's a triangle\n");
    } else if(const std::vector<Point_3 > *polyp =
        CGAL::object_cast< std::vector<Point_3 > >(&obj))
    {
      cgal_found_intersection = true;
      printf("  ✅ polygon\n");
    }else {
      printf("  🤔 da fuke?\n");
    }
  }
  printf("%d,%d  %s vs %s\n",f,c,found_intersection?"☠️":"✅",cgal_found_intersection?"☠️":"✅");
  if(found_intersection != cgal_found_intersection)
  {
    printf("Tg = [[%g,%g,%g];[%g,%g,%g];[%g,%g,%g]];\n",
        CGAL::to_double(Tg.vertex(0).x()),
        CGAL::to_double(Tg.vertex(0).y()),
        CGAL::to_double(Tg.vertex(0).z()),
        CGAL::to_double(Tg.vertex(1).x()),
        CGAL::to_double(Tg.vertex(1).y()),
        CGAL::to_double(Tg.vertex(1).z()),
        CGAL::to_double(Tg.vertex(2).x()),
        CGAL::to_double(Tg.vertex(2).y()),
        CGAL::to_double(Tg.vertex(2).z()));
    printf("Tf = [[%g,%g,%g];[%g,%g,%g];[%g,%g,%g]];\n",
        CGAL::to_double(Tf.vertex(0).x()),
        CGAL::to_double(Tf.vertex(0).y()),
        CGAL::to_double(Tf.vertex(0).z()),
        CGAL::to_double(Tf.vertex(1).x()),
        CGAL::to_double(Tf.vertex(1).y()),
        CGAL::to_double(Tf.vertex(1).z()),
        CGAL::to_double(Tf.vertex(2).x()),
        CGAL::to_double(Tf.vertex(2).y()),
        CGAL::to_double(Tf.vertex(2).z()));
  }
  assert(found_intersection == cgal_found_intersection);
#endif
  return found_intersection;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template bool igl::triangle_triangle_intersect_shared_vertex<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, int, int, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, int, int, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar);
template bool igl::triangle_triangle_intersect_shared_vertex<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 1, -1, 1, 1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, int, int, Eigen::MatrixBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, int, int, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar);
template bool igl::triangle_triangle_intersect_shared_vertex<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, int, int, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> > const&, int, int, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar);
#endif
