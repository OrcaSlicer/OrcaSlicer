#include "triangle_triangle_intersect.h"
#include "triangle_triangle_intersect_shared_edge.h"
#include "triangle_triangle_intersect_shared_vertex.h"
#include "tri_tri_intersect.h"
#include <Eigen/Geometry>
#include <stdio.h>
#include <igl/unique_edge_map.h>
#include <igl/barycentric_coordinates.h>
#include <unordered_map>

//#define IGL_TRIANGLE_TRIANGLE_INTERSECT_DEBUG
#ifdef IGL_TRIANGLE_TRIANGLE_INTERSECT_DEBUG
// CGAL::Epeck
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#warning "🐌🐌🐌🐌🐌🐌🐌🐌 Slow debug mode for igl::triangle_triangle_intersect"
#endif

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE,
  typename DerivedEMAP,
  typename DerivedEF,
  typename Derivedp>
IGL_INLINE bool igl::triangle_triangle_intersect(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedE> & E,
  const Eigen::MatrixBase<DerivedEMAP> & EMAP,
  const Eigen::MatrixBase<DerivedEF> & EF,
  const int f,
  const int c,
  const Eigen::MatrixBase<Derivedp> & p,
  const int g)
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

  // I'm leaving this debug printing stuff in for a bit until I trust this
  // better. 
  constexpr bool stinker = false;
  //const bool stinker = (f==1492 && g==1554);
  if(stinker) { printf("👀\n"); }
  bool found_intersection = false;
  // So edge opposite F(f,c) is the outer edge.
  const bool share_edge_opposite_c = [&]()
  {
    const int o = EMAP(f + c*F.rows());
    return (EF(o,0) == f && EF(o,1) == g) || (EF(o,1) == f && EF(o,0) == g);
  }();
  const int o = EMAP(f + c*F.rows());
  // Do they share the edge opposite c?
  if(share_edge_opposite_c)
  {
    if(stinker) { printf("⚠️ shares an edge\n"); }
    found_intersection = triangle_triangle_intersect_shared_edge(V,F,f,c,p,g,1e-8);
  }else
  {
    if(stinker) { printf("does not share an edge\n"); }
    // Do they share a vertex?
    int sf,sg;
    bool found_shared_vertex = false;
    for(sf = 0;sf<3;sf++)
    {
      if(sf == c){ continue;}
      for(sg = 0;sg<3;sg++)
      {
        if(F(f,sf) == F(g,sg))
        {
          found_shared_vertex = true;
          break;
        }
      }
      if(found_shared_vertex) { break;} 
    }
    if(found_shared_vertex)
    {
      if(stinker) { printf("⚠️ shares a vertex\n"); }
      found_intersection = 
        triangle_triangle_intersect_shared_vertex(V,F,f,sf,c,p,g,sg,1e-14);
    }else
    {
      bool coplanar;
      Eigen::RowVector3d i1,i2;
      found_intersection = 
        igl::tri_tri_overlap_test_3d(
                V.row(F(g,0)).template cast<double>(), 
                V.row(F(g,1)).template cast<double>(), 
                V.row(F(g,2)).template cast<double>(),
                            p.template cast<double>(),
          V.row(F(f,(c+1)%3)).template cast<double>(),
          V.row(F(f,(c+2)%3)).template cast<double>()) && 
        igl::tri_tri_intersection_test_3d(
                V.row(F(g,0)).template cast<double>(), 
                V.row(F(g,1)).template cast<double>(), 
                V.row(F(g,2)).template cast<double>(),
                            p.template cast<double>(),
          V.row(F(f,(c+1)%3)).template cast<double>(),
          V.row(F(f,(c+2)%3)).template cast<double>(),
          coplanar,
          i1,i2);
      if(stinker) { printf("tri_tri_intersection_test_3d says %s\n",found_intersection?"☠️":"✅"); }
#ifdef IGL_TRIANGLE_TRIANGLE_INTERSECT_DEBUG
      if(CGAL::do_intersect(Tg,Tf))
      {
        cgal_found_intersection = true;
        printf("  ✅ sure it's anything\n");
      }
      assert(found_intersection == cgal_found_intersection);
#endif
    }
  }
  if(stinker) { printf("%s\n",found_intersection?"☠️":"✅"); }
  return found_intersection;
}


template <
  typename DerivedV, 
  typename DerivedF, 
  typename DerivedIF,
  typename DerivedEV,
  typename DerivedEE>
void igl::triangle_triangle_intersect(
    const Eigen::MatrixBase<DerivedV> & V1,
    const Eigen::MatrixBase<DerivedF> & F1,
    const Eigen::MatrixBase<DerivedV> & V2,
    const Eigen::MatrixBase<DerivedF> & F2,
    const Eigen::MatrixBase<DerivedIF> & IF,
    Eigen::PlainObjectBase<DerivedEV> & EV,
    Eigen::PlainObjectBase<DerivedEE> & EE)
{
  using Scalar = typename DerivedEV::Scalar;
  using RowVector3S = Eigen::Matrix<Scalar,1,3>;
  // We were promised that IF is a list of non-coplanar non-degenerately
  // intersecting triangle pairs. This implies that the set of intersection is a
  // line-segment whose endpoints are defined by edge-triangle intersections.
  // 
  // Each edge-triangle intersection will be stored in a map (e,f)
  //
  std::unordered_map<std::int64_t,int> uf_to_ev;
  Eigen::VectorXi EMAP1;
  Eigen::MatrixXi uE1;
  {
    Eigen::VectorXi uEE,uEC;
    Eigen::MatrixXi E;
    igl::unique_edge_map(F1,E,uE1,EMAP1,uEE,uEC);
  }
  Eigen::VectorXi EMAP2_cpy;
  Eigen::MatrixXi uE2_cpy;
  const bool self = &F1 == &F2;
  if(!self)
  {
    Eigen::VectorXi uEE,uEC;
    Eigen::MatrixXi E;
    igl::unique_edge_map(F2,E,uE2_cpy,EMAP2_cpy,uEE,uEC);
  }
  const Eigen::VectorXi & EMAP2 = self?EMAP1:EMAP2_cpy;
  const Eigen::MatrixXi & uE2 = self?uE1:uE2_cpy;

  int num_ev = 0;
  EE.resize(IF.rows(),2);
  for(int i = 0; i<IF.rows(); i++)
  {
    // Just try all 6 edges
    Eigen::Matrix<double,6,6> B;
    Eigen::Matrix<double,6,3> X;
    Eigen::Matrix<int,6,2> uf;
    for(int p = 0;p<2;p++)
    {
      const auto consider_edges = [&B,&X,&uf]
        (const int p,
         const int f1, 
         const int f2,
         const Eigen::MatrixBase<DerivedV> & V1,
         const Eigen::MatrixBase<DerivedF> & F1,
         const Eigen::VectorXi & EMAP1,
         const Eigen::MatrixXi & uE1,
         const Eigen::MatrixBase<DerivedV> & V2,
         const Eigen::MatrixBase<DerivedF> & F2)
      {
        for(int e1 = 0;e1<3;e1++)
        {
          // intersect edge (ij) opposite vertex F(f1,e1) with triangle ABC of F(f2,:)
          const int u1 = EMAP1(f1 +(EMAP1.size()/3)*e1);
          uf.row(p*3+e1) << u1,f2;
          const int i = uE1(u1,0);
          const int j = uE1(u1,1);
          // Just copy.
          const RowVector3S Vi = V1.row(i);
          const RowVector3S Vj = V1.row(j);
          const RowVector3S VA = V2.row(F2(f2,0));
          const RowVector3S VB = V2.row(F2(f2,1));
          const RowVector3S VC = V2.row(F2(f2,2));
          // Find intersection of line (Vi,Vj) with plane of triangle (A,B,C)
          const RowVector3S n = (VB-VA).template head<3>().cross((VC-VA).template head<3>());
          const Scalar d = n.dot(VA);
          const Scalar t = (d - n.dot(Vi))/(n.dot(Vj-Vi));
          const RowVector3S x = Vi + t*(Vj-Vi);
          
          // Get barycenteric coordinates (possibly negative of X)
          RowVector3S b1;
          igl::barycentric_coordinates(x,VA,VB,VC,b1);
          B.row(p*3+e1).head<3>() = b1;
          RowVector3S b2;
          igl::barycentric_coordinates(x,
              V1.row(F1(f1,0)).template head<3>().eval(),
              V1.row(F1(f1,1)).template head<3>().eval(),
              V1.row(F1(f1,2)).template head<3>().eval(),
              b2);
          B.row(p*3+e1).tail<3>() = b2;

          X.row(p*3+e1) = x;
        }
      };


      const int f1 = IF(i,p);
      const int f2 = IF(i,(p+1)%2);
      consider_edges(p,f1,f2,
        p==0?   V1:V2,
        p==0?   F1:F2,
        p==0?EMAP1:EMAP2,
        p==0?  uE1:uE2,
        p==0?   V2:V1,
        p==0?   F2:F1);
    }

    // Find the two rows in B with the largest-smallest element
    int j1,j2;
    {
      double b_min1 = -std::numeric_limits<double>::infinity();
      double b_min2 = -std::numeric_limits<double>::infinity();
      for(int j = 0;j<6;j++)
      {
        // It's not clear that using barycentric coordinates is better than
        // point_simplex distance (though that requires inequalities).
        const double bminj = B.row(j).minCoeff();
        if(bminj > b_min1)
        {
          b_min2 = b_min1;
          j2 = j1;
          b_min1 = bminj;
          j1 = j;
        }else if(bminj > b_min2)
        {
          b_min2 = bminj;
          j2 = j;
        }
      }
    }

    const auto append_or_find = [&](
      int p, int u, int f, const RowVector3S & x,
      std::unordered_map<std::int64_t,int> & uf_to_ev)->int
    {
      const std::int64_t key = (std::int64_t)u + ((std::int64_t)f << 32) + ((std::int64_t)p << 63);
      if(uf_to_ev.find(key) == uf_to_ev.end())
      {
        if(num_ev == EV.rows())
        {
          EV.conservativeResize(2*EV.rows()+1,3);
        }
        EV.row(num_ev) = x;
        uf_to_ev[key] = num_ev;
        num_ev++;
      }
      return uf_to_ev[key];
    };

    EE.row(i) <<
      append_or_find(j1>=3,uf(j1,0),uf(j1,1),X.row(j1),uf_to_ev),
      append_or_find(j2>=3,uf(j2,0),uf(j2,1),X.row(j2),uf_to_ev);
  }
  EV.conservativeResize(num_ev,3);
}

template <
  typename DerivedV, 
  typename DerivedF, 
  typename DerivedIF,
  typename DerivedEV,
  typename DerivedEE>
void igl::triangle_triangle_intersect(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedIF> & IF,
    Eigen::PlainObjectBase<DerivedEV> & EV,
    Eigen::PlainObjectBase<DerivedEE> & EE)
{
  // overload will take care of detecting reference equality
  return triangle_triangle_intersect(V,F,V,F,IF,EV,EE);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::triangle_triangle_intersect<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template void igl::triangle_triangle_intersect<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::triangle_triangle_intersect<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, int, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 1, -1, false> > const&, int);
template bool igl::triangle_triangle_intersect<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 1, -1, 1, 1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, int, Eigen::MatrixBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, int);
#endif
