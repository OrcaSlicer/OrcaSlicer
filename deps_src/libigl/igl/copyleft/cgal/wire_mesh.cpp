#include "wire_mesh.h"

#include "../../list_to_matrix.h"
#include "../../PI.h"
#include "../../placeholders.h"
#include "convex_hull.h"
#include "coplanar.h"
#include "mesh_boolean.h"
#include <Eigen/Geometry>
#include <vector>

template <
  typename DerivedWV,
  typename DerivedWE,
  typename Derivedth,
  typename DerivedV,
  typename DerivedF,
  typename DerivedJ>
IGL_INLINE void igl::copyleft::cgal::wire_mesh(
  const Eigen::MatrixBase<DerivedWV> & WV,
  const Eigen::MatrixBase<DerivedWE> & WE,
  const Eigen::MatrixBase<Derivedth> & th,
  const int poly_size,
  const bool solid,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  assert((th.size()==1 || th.size()==WE.rows()) && 
    "th should be scalar or size of WE");

  typedef typename DerivedWV::Scalar Scalar;
  // Canonical polygon to place at each endpoint
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,3> MatrixX3S;
  MatrixX3S PV(poly_size,3);
  for(int p =0;p<PV.rows();p++)
  {
    const Scalar phi = (Scalar(p)/Scalar(PV.rows()))*2.*igl::PI;
    PV(p,0) = 0.5*cos(phi);
    PV(p,1) = 0.5*sin(phi);
    PV(p,2) = 0;
  }

  V.resize(WV.rows() + PV.rows() * 2 * WE.rows(),3);
  V.topLeftCorner(WV.rows(),3) = WV;
  // Signed adjacency list
  std::vector<std::vector<std::pair<int,int> > > A(WV.rows());
  // Inputs:
  //   e  index of edge
  //   c  index of endpoint [0,1]
  //   p  index of polygon vertex
  // Returns index of corresponding vertex in V
  const auto index = 
    [&PV,&WV](const int e, const int c, const int p)->int
  {
    return WV.rows() + e*2*PV.rows() + PV.rows()*c + p;
  };
  //const auto unindex = 
  //  [&PV,&WV](int v, int & e, int & c, int & p)
  //{
  //  assert(v>=WV.rows());
  //  v = v-WV.rows();
  //  e = v/(2*PV.rows());
  //  v = v-e*(2*PV.rows());
  //  c = v/(PV.rows());
  //  v = v-c*(PV.rows());
  //  p = v;
  //};

  // Count each vertex's indicident edges.
  std::vector<int> nedges(WV.rows(), 0);
  for(int e = 0;e<WE.rows();e++)
  {
    ++nedges[WE(e, 0)];
    ++nedges[WE(e, 1)];
  }

  // loop over all edges
  for(int e = 0;e<WE.rows();e++)
  {
    // Fill in adjacency list as we go
    A[WE(e,0)].emplace_back(e,0);
    A[WE(e,1)].emplace_back(e,1);
    typedef Eigen::Matrix<Scalar,1,3> RowVector3S;
    const RowVector3S ev = WV.row(WE(e,1))-WV.row(WE(e,0));
    const Scalar len = ev.norm();
    // Unit edge vector
    const RowVector3S uv = ev.normalized();
    Eigen::Quaternion<Scalar> q;
    q = q.FromTwoVectors(RowVector3S(0,0,1),uv);
    // loop over polygon vertices
    for(int p = 0;p<PV.rows();p++)
    {
      RowVector3S qp = q*(PV.row(p)*th(e%th.size()));
      // loop over endpoints
      for(int c = 0;c<2;c++)
      {
        // Direction moving along edge vector
        const Scalar dir = c==0?1:-1;
        // Amount (distance) to move along edge vector
        // Start with factor of thickness;
        // Max out amount at 1/3 of edge length so that there's always some
        // amount of edge
        // Zero out if vertex is incident on only one edge
        Scalar dist = 
          std::min(1.*th(e%th.size()),len/3.0)*(nedges[WE(e,c)] > 1);
        // Move to endpoint, offset by amount
        V.row(index(e,c,p)) = 
          qp+WV.row(WE(e,c)) + dist*dir*uv;
      }
    }
  }

  std::vector<std::vector<typename DerivedF::Index> > vF;
  std::vector<int> vJ;
  const auto append_hull = 
    [&V,&vF,&vJ](const Eigen::VectorXi & I, const int j)
  {
    MatrixX3S Vv = V(I,igl::placeholders::all);

    if(coplanar(Vv))
    {
      return;
    }
    Eigen::MatrixXi Fv;
    convex_hull(Vv,Fv);
    for(int f = 0;f<Fv.rows();f++)
    {
      const Eigen::Array<int,1,3> face(I(Fv(f,0)), I(Fv(f,1)), I(Fv(f,2)));
      //const bool on_vertex = (face<WV.rows()).any();
      //if(!on_vertex)
      //{
      //  // This correctly prunes fcaes on the "caps" of convex hulls around
      //  // edges, but for convex hulls around vertices this will only work if
      //  // the incoming edges are not overlapping.
      //  //
      //  // Q: For convex hulls around vertices, is the correct thing to do:
      //  // check if all corners of face lie *on or _outside_* of plane of "cap"?
      //  // 
      //  // H: Maybe, but if there's an intersection then the boundary of the
      //  // incoming convex hulls around edges is still not going to match up
      //  // with the boundary on the convex hull around the vertices.
      //  //
      //  // Might have to bite the bullet and always call self-union.
      //  bool all_same = true;
      //  int e0,c0,p0;
      //  unindex(face(0),e0,c0,p0);
      //  for(int i = 1;i<3;i++)
      //  {
      //    int ei,ci,pi;
      //    unindex(face(i),ei,ci,pi);
      //    all_same = all_same && (e0==ei && c0==ci);
      //  }
      //  if(all_same)
      //  {
      //    // don't add this face
      //    continue;
      //  }
      //}
      vF.push_back( { face(0),face(1),face(2)});
      vJ.push_back(j);
    }
  };
  // loop over each vertex
  for(int v = 0;v<WV.rows();v++)
  {
    // Gather together this vertex and the polygon vertices of all incident
    // edges
    Eigen::VectorXi I(1+A[v].size()*PV.rows());
    // This vertex
    I(0) = v;
    for(int n = 0;n<A[v].size();n++)
    {
      for(int p = 0;p<PV.rows();p++)
      {
        const int e = A[v][n].first;
        const int c = A[v][n].second;
        I(1+n*PV.rows()+p) = index(e,c,p);
      }
    }
    append_hull(I,v);
  }
  // loop over each edge
  for(int e = 0;e<WE.rows();e++)
  {
    // Gether together polygon vertices of both endpoints
    Eigen::VectorXi I(PV.rows()*2);
    for(int c = 0;c<2;c++)
    {
      for(int p = 0;p<PV.rows();p++)
      {
        I(c*PV.rows()+p) = index(e,c,p);
      }
    }
    append_hull(I,WV.rows()+e);
  }

  list_to_matrix(vF,F);
  if(solid)
  {
    // Self-union to clean up 
    igl::copyleft::cgal::mesh_boolean(
      Eigen::MatrixXd(V),Eigen::MatrixXi(F),Eigen::MatrixXd(),Eigen::MatrixXi(),
      "union",
      V,F,J);
    for(int j=0;j<J.size();j++) J(j) = vJ[J(j)];
  }else
  {
    list_to_matrix(vJ,J);
  }
}

template <
  typename DerivedWV,
  typename DerivedWE,
  typename DerivedV,
  typename DerivedF,
  typename DerivedJ>
IGL_INLINE void igl::copyleft::cgal::wire_mesh(
  const Eigen::MatrixBase<DerivedWV> & WV,
  const Eigen::MatrixBase<DerivedWE> & WE,
  const double th,
  const int poly_size,
  const bool solid,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  return wire_mesh(
    WV,WE,(Eigen::VectorXd(1,1)<<th).finished(),poly_size,solid,V,F,J);
}

template <
  typename DerivedWV,
  typename DerivedWE,
  typename DerivedV,
  typename DerivedF,
  typename DerivedJ>
IGL_INLINE void igl::copyleft::cgal::wire_mesh(
  const Eigen::MatrixBase<DerivedWV> & WV,
  const Eigen::MatrixBase<DerivedWE> & WE,
  const double th,
  const int poly_size,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  return wire_mesh(WV,WE,th,poly_size,true,V,F,J);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation 
template void igl::copyleft::cgal::wire_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, double, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
