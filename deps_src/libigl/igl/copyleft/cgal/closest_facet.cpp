// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "closest_facet.h"

#include <vector>
#include <stdexcept>
#include <unordered_map>

#include "order_facets_around_edge.h"
#include "submesh_aabb_tree.h"
#include "../../vertex_triangle_adjacency.h"
#include "../../PlainMatrix.h"
#include "../../LinSpaced.h"
//#include "../../writePLY.h"

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedI,
  typename DerivedP,
  typename DerivedEMAP,
  typename DeriveduEC,
  typename DeriveduEE,
  typename Kernel,
  typename DerivedR,
  typename DerivedS >
IGL_INLINE void igl::copyleft::cgal::closest_facet(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedI>& I,
    const Eigen::MatrixBase<DerivedP>& P,
    const Eigen::MatrixBase<DerivedEMAP>& EMAP,
    const Eigen::MatrixBase<DeriveduEC>& uEC,
    const Eigen::MatrixBase<DeriveduEE>& uEE,
    const std::vector<std::vector<size_t> > & VF,
    const std::vector<std::vector<size_t> > & VFi,
    const CGAL::AABB_tree<
      CGAL::AABB_traits<
        Kernel, 
        CGAL::AABB_triangle_primitive<
          Kernel, typename std::vector<
            typename Kernel::Triangle_3 >::iterator > > > & tree,
    const std::vector<typename Kernel::Triangle_3 > & triangles,
    const std::vector<bool> & in_I,
    Eigen::PlainObjectBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedS>& S)
{
  typedef typename Kernel::Point_3 Point_3;
  typedef typename Kernel::Plane_3 Plane_3;
  typedef typename Kernel::Segment_3 Segment_3;
  typedef typename Kernel::Triangle_3 Triangle;
  typedef typename std::vector<Triangle>::iterator Iterator;
  typedef typename CGAL::AABB_triangle_primitive<Kernel, Iterator> Primitive;
  typedef typename CGAL::AABB_traits<Kernel, Primitive> AABB_triangle_traits;
  typedef typename CGAL::AABB_tree<AABB_triangle_traits> Tree;

  if (F.rows() <= 0 || I.rows() <= 0) {
    throw std::runtime_error(
        "Closest facet cannot be computed on empty mesh.");
  }

  auto on_the_positive_side = [&](size_t fid, const Point_3& p) -> bool
  {
    const auto& f = F.row(fid).eval();
    Point_3 v0(V(f[0], 0), V(f[0], 1), V(f[0], 2));
    Point_3 v1(V(f[1], 0), V(f[1], 1), V(f[1], 2));
    Point_3 v2(V(f[2], 0), V(f[2], 1), V(f[2], 2));
    auto ori = CGAL::orientation(v0, v1, v2, p);
    switch (ori) {
      case CGAL::POSITIVE:
        return true;
      case CGAL::NEGATIVE:
        return false;
      case CGAL::COPLANAR:
        // Warning:
        // This can only happen if fid contains a boundary edge.
        // Categorized this ambiguous case as negative side.
        return false;
      default:
        throw std::runtime_error("Unknown CGAL state.");
    }
    return false;
  };

  auto get_orientation = [&](size_t fid, size_t s, size_t d) -> bool 
  {
    const auto& f = F.row(fid);
    if      ((size_t)f[0] == s && (size_t)f[1] == d) return false;
    else if ((size_t)f[1] == s && (size_t)f[2] == d) return false;
    else if ((size_t)f[2] == s && (size_t)f[0] == d) return false;
    else if ((size_t)f[0] == d && (size_t)f[1] == s) return true;
    else if ((size_t)f[1] == d && (size_t)f[2] == s) return true;
    else if ((size_t)f[2] == d && (size_t)f[0] == s) return true;
    else {
      throw std::runtime_error(
          "Cannot compute orientation due to incorrect connectivity");
      return false;
    }
  };
  auto index_to_signed_index = [&](size_t index, bool ori) -> int{
    return (index+1) * (ori? 1:-1);
  };
  //auto signed_index_to_index = [&](int signed_index) -> size_t {
  //    return abs(signed_index) - 1;
  //};

  enum ElementType { VERTEX, EDGE, FACE };
  auto determine_element_type = [&](const Point_3& p, const size_t fid,
      size_t& element_index) -> ElementType {
    const auto& tri = triangles[fid];
    const Point_3 p0 = tri[0];
    const Point_3 p1 = tri[1];
    const Point_3 p2 = tri[2];

    if (p == p0) { element_index = 0; return VERTEX; }
    if (p == p1) { element_index = 1; return VERTEX; }
    if (p == p2) { element_index = 2; return VERTEX; }
    if (CGAL::collinear(p0, p1, p)) { element_index = 2; return EDGE; }
    if (CGAL::collinear(p1, p2, p)) { element_index = 0; return EDGE; }
    if (CGAL::collinear(p2, p0, p)) { element_index = 1; return EDGE; }

    element_index = 0;
    return FACE;
  };

  auto process_edge_case = [&](
      size_t query_idx,
      const size_t s, const size_t d,
      size_t preferred_facet,
      bool& orientation) -> size_t 
  {
    Point_3 query_point(
      P(query_idx, 0),
      P(query_idx, 1),
      P(query_idx, 2));

    size_t corner_idx = std::numeric_limits<size_t>::max();
    if ((s == F(preferred_facet, 0) && d == F(preferred_facet, 1)) ||
        (s == F(preferred_facet, 1) && d == F(preferred_facet, 0))) 
    {
      corner_idx = 2;
    } else if ((s == F(preferred_facet, 0) && d == F(preferred_facet, 2)) ||
        (s == F(preferred_facet, 2) && d == F(preferred_facet, 0))) 
    {
      corner_idx = 1;
    } else if ((s == F(preferred_facet, 1) && d == F(preferred_facet, 2)) ||
        (s == F(preferred_facet, 2) && d == F(preferred_facet, 1))) 
    {
      corner_idx = 0;
    } else 
    {
      // Should never happen.
      //std::cerr << "s: " << s << "\t d:" << d << std::endl;
      //std::cerr << F.row(preferred_facet) << std::endl;
      throw std::runtime_error(
          "Invalid connectivity, edge does not belong to facet");
    }

    auto ueid = EMAP(preferred_facet + corner_idx * F.rows());
    std::vector<size_t> intersected_face_indices;
    //auto eids = uE2E[ueid];
    //for (auto eid : eids) 
    for(size_t j = uEC(ueid);j<uEC(ueid+1);j++)
    {
      const size_t eid = uEE(j);
      const size_t fid = eid % F.rows();
      if (in_I[fid]) 
      {
        intersected_face_indices.push_back(fid);
      }
    }

    const size_t num_intersected_faces = intersected_face_indices.size();
    std::vector<int> intersected_face_signed_indices(num_intersected_faces);
    std::transform(
        intersected_face_indices.begin(),
        intersected_face_indices.end(),
        intersected_face_signed_indices.begin(),
        [&](size_t index) {
        return index_to_signed_index(
          index, get_orientation(index, s,d));
        });

    assert(num_intersected_faces >= 1);
    if (num_intersected_faces == 1) 
    {
      // The edge must be a boundary edge.  Thus, the orientation can be
      // simply determined by checking if the query point is on the
      // positive side of the facet.
      const size_t fid = intersected_face_indices[0];
      orientation = on_the_positive_side(fid, query_point);
      return fid;
    }

    Eigen::VectorXi order;
    PlainMatrix<DerivedP,1> pivot = P.row(query_idx).eval();
    igl::copyleft::cgal::order_facets_around_edge(V, F, s, d,
      intersected_face_signed_indices,
      pivot, order);

    // Although first and last are equivalent, make the choice based on
    // preferred_facet.
    const size_t first = order[0];
    const size_t last = order[num_intersected_faces-1];
    if (intersected_face_indices[first] == preferred_facet) {
      orientation = intersected_face_signed_indices[first] < 0;
      return intersected_face_indices[first];
    } else if (intersected_face_indices[last] == preferred_facet) {
      orientation = intersected_face_signed_indices[last] > 0;
      return intersected_face_indices[last];
    } else {
      orientation = intersected_face_signed_indices[order[0]] < 0;
      return intersected_face_indices[order[0]];
    }
  };

  auto process_face_case = [&F,&I,&process_edge_case](
    const size_t query_idx, 
    const size_t fid, 
    bool& orientation) -> size_t 
  {
    const auto& f = F.row(I(fid, 0));
    return process_edge_case(query_idx, f[0], f[1], I(fid, 0), orientation);
  };

  // Given that the closest point to query point P(query_idx,:) on (V,F(I,:))
  // is the vertex at V(s,:) which is incident at least on triangle
  // F(preferred_facet,:), determine a facet incident on V(s,:) that is
  // _exposed_ to the query point and determine whether that facet is facing
  // _toward_ or _away_ from the query point.
  //
  // Inputs:
  //   query_idx  index into P of query point
  //   s  index into V of closest point at vertex
  //   preferred_facet  facet incident on s
  // Outputs:
  //   orientation  whether returned face is facing toward or away from
  //     query (parity unclear)
  // Returns face guaranteed to be "exposed" to P(query_idx,:)
  auto process_vertex_case = [&](
    const size_t query_idx, 
    size_t s,
    bool& orientation) -> size_t
  {
    const Point_3 query_point(
        P(query_idx, 0), P(query_idx, 1), P(query_idx, 2));
    const Point_3 closest_point(V(s, 0), V(s, 1), V(s, 2));
    std::vector<size_t> adj_faces;
    std::vector<size_t> adj_face_corners;
    {
      // Gather adj faces to s within I.
      const auto& all_adj_faces = VF[s];
      const auto& all_adj_face_corners = VFi[s];
      const size_t num_all_adj_faces = all_adj_faces.size();
      for (size_t i=0; i<num_all_adj_faces; i++) 
      {
        const size_t fid = all_adj_faces[i];
        // Shouldn't this always be true if I is a full connected component?
        if (in_I[fid]) 
        {
          adj_faces.push_back(fid);
          adj_face_corners.push_back(all_adj_face_corners[i]);
        }
      }
    }
    const size_t num_adj_faces = adj_faces.size();
    assert(num_adj_faces > 0);

    std::set<size_t> adj_vertices_set;
    std::unordered_multimap<size_t, size_t> v2f;
    for (size_t i=0; i<num_adj_faces; i++) 
    {
      const size_t fid = adj_faces[i];
      const size_t cid = adj_face_corners[i];
      const auto& f = F.row(adj_faces[i]);
      const size_t next = f[(cid+1)%3];
      const size_t prev = f[(cid+2)%3];
      adj_vertices_set.insert(next);
      adj_vertices_set.insert(prev);
      v2f.insert({{next, fid}, {prev, fid}});
    }
    const size_t num_adj_vertices = adj_vertices_set.size();
    std::vector<size_t> adj_vertices(num_adj_vertices);
    std::copy(adj_vertices_set.begin(), adj_vertices_set.end(),
        adj_vertices.begin());

    std::vector<Point_3> adj_points;
    for (size_t vid : adj_vertices) 
    {
      adj_points.emplace_back(V(vid,0), V(vid,1), V(vid,2));
    }

    // A plane is on the exterior if all adj_points lies on or to
    // one side of the plane.
    auto is_on_exterior = [&](const Plane_3& separator) -> bool{
      size_t positive=0;
      size_t negative=0;
      for (const auto& point : adj_points) {
        switch(separator.oriented_side(point)) {
          case CGAL::ON_POSITIVE_SIDE:
            positive++;
            break;
          case CGAL::ON_NEGATIVE_SIDE:
            negative++;
            break;
          case CGAL::ON_ORIENTED_BOUNDARY:
            break;
          default:
            throw "Unknown plane-point orientation";
        }
      }
      auto query_orientation = separator.oriented_side(query_point);
      if (query_orientation == CGAL::ON_ORIENTED_BOUNDARY &&
          (positive == 0 && negative == 0)) {
        // All adj vertices and query point are coplanar.
        // In this case, all separators are equally valid.
        return true;
      } else {
        bool r = (positive == 0 && query_orientation == CGAL::POSITIVE)
          || (negative == 0 && query_orientation == CGAL::NEGATIVE);
        return r;
      }
    };

    size_t d = std::numeric_limits<size_t>::max();
    for (size_t i=0; i<num_adj_vertices; i++) {
      const size_t vi = adj_vertices[i];
      for (size_t j=i+1; j<num_adj_vertices; j++) {
        Plane_3 separator(closest_point, adj_points[i], adj_points[j]);
        if (separator.is_degenerate()) {
          continue;
        }
        if (is_on_exterior(separator)) {
          if (!CGAL::collinear(
                query_point, adj_points[i], closest_point)) {
            d = vi;
            break;
          } else {
            d = adj_vertices[j];
            assert(!CGAL::collinear(
                  query_point, adj_points[j], closest_point));
            break;
          }
        }
      }
    }
    if (d == std::numeric_limits<size_t>::max()) {
      //PlainMatrix<DerivedV,Eigen::Dynamic> tmp_vertices(V.rows(), V.cols());
      //for (size_t i=0; i<V.rows(); i++) {
      //  for (size_t j=0; j<V.cols(); j++) {
      //    tmp_vertices(i,j) = CGAL::to_double(V(i,j));
      //  }
      //}
      //PlainMatrix<DerivedF,Eigen::Dynamic,3> tmp_faces(adj_faces.size(), 3);
      //for (size_t i=0; i<adj_faces.size(); i++) {
      //  tmp_faces.row(i) = F.row(adj_faces[i]);
      //}
      //igl::writePLY("debug.ply", tmp_vertices, tmp_faces, false);
      throw std::runtime_error("Invalid vertex neighborhood");
    }
    const auto itr = v2f.equal_range(d);
    assert(itr.first != itr.second);

    return process_edge_case(query_idx, s, d, itr.first->second, orientation);
  };

  const size_t num_queries = P.rows();
  R.resize(num_queries, 1);
  S.resize(num_queries, 1);
  for (size_t i=0; i<num_queries; i++) {
    const Point_3 query(P(i,0), P(i,1), P(i,2));
    auto projection = tree.closest_point_and_primitive(query);
    const Point_3 closest_point = projection.first;
    size_t fid = projection.second - triangles.begin();
    bool fid_ori = false;

    // Gether all facets sharing the closest point.
    typename std::vector<typename Tree::Primitive_id> intersected_faces;
    tree.all_intersected_primitives(Segment_3(closest_point, query),
        std::back_inserter(intersected_faces));
    const size_t num_intersected_faces = intersected_faces.size();
    std::vector<size_t> intersected_face_indices(num_intersected_faces);
    std::transform(intersected_faces.begin(),
        intersected_faces.end(),
        intersected_face_indices.begin(),
        [&](const typename Tree::Primitive_id& itr) -> size_t
        { return I(itr-triangles.begin(), 0); });

    size_t element_index;
    auto element_type = determine_element_type(closest_point, fid,
        element_index);
    switch(element_type) {
      case VERTEX:
        {
          const auto& f = F.row(I(fid, 0));
          const size_t s = f[element_index];
          fid = process_vertex_case(i, s, fid_ori);
        }
        break;
      case EDGE:
        {
          const auto& f = F.row(I(fid, 0));
          const size_t s = f[(element_index+1)%3];
          const size_t d = f[(element_index+2)%3];
          fid = process_edge_case(i, s, d, I(fid, 0), fid_ori);
        }
        break;
      case FACE:
        {
          fid = process_face_case(i, fid, fid_ori);
        }
        break;
      default:
        throw std::runtime_error("Unknown element type.");
    }


    R(i,0) = fid;
    S(i,0) = fid_ori;
  }
}

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedI,
  typename DerivedP,
  typename DerivedEMAP,
  typename DeriveduEC,
  typename DeriveduEE,
  typename DerivedR,
  typename DerivedS >
IGL_INLINE void igl::copyleft::cgal::closest_facet(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedI>& I,
    const Eigen::MatrixBase<DerivedP>& P,
    const Eigen::MatrixBase<DerivedEMAP>& EMAP,
    const Eigen::MatrixBase<DeriveduEC>& uEC,
    const Eigen::MatrixBase<DeriveduEE>& uEE,
    Eigen::PlainObjectBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedS>& S)
{

  typedef CGAL::Exact_predicates_exact_constructions_kernel Kernel;
  typedef Kernel::Triangle_3 Triangle;
  typedef std::vector<Triangle>::iterator Iterator;
  typedef CGAL::AABB_triangle_primitive<Kernel, Iterator> Primitive;
  typedef CGAL::AABB_traits<Kernel, Primitive> AABB_triangle_traits;
  typedef CGAL::AABB_tree<AABB_triangle_traits> Tree;

  if (F.rows() <= 0 || I.rows() <= 0) {
    throw std::runtime_error(
        "Closest facet cannot be computed on empty mesh.");
  }

  std::vector<std::vector<size_t> > VF, VFi;
  igl::vertex_triangle_adjacency(V.rows(), F, VF, VFi);
  std::vector<bool> in_I;
  std::vector<Triangle> triangles;
  Tree tree;
  submesh_aabb_tree(V,F,I,tree,triangles,in_I);

  return closest_facet(
    V,F,I,P,EMAP,uEC,uEE,VF,VFi,tree,triangles,in_I,R,S);
}

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DerivedEMAP,
  typename DeriveduEC,
  typename DeriveduEE,
  typename DerivedR,
  typename DerivedS >
IGL_INLINE void igl::copyleft::cgal::closest_facet(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedP>& P,
    const Eigen::MatrixBase<DerivedEMAP>& EMAP,
    const Eigen::MatrixBase<DeriveduEC>& uEC,
    const Eigen::MatrixBase<DeriveduEE>& uEE,
    Eigen::PlainObjectBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedS>& S) {
  const size_t num_faces = F.rows();
  Eigen::VectorXi I = igl::LinSpaced<Eigen::VectorXi>(num_faces, 0, num_faces-1);
  igl::copyleft::cgal::closest_facet(V, F, I, P, EMAP, uEC, uEE, R, S);
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::copyleft::cgal::closest_facet<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, CGAL::Epeck, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>>(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 1, -1, 3>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, std::vector<std::vector<size_t, std::allocator<size_t>>, std::allocator<std::vector<size_t, std::allocator<size_t>>>> const&, std::vector<std::vector<size_t, std::allocator<size_t>>, std::allocator<std::vector<size_t, std::allocator<size_t>>>> const&, CGAL::AABB_tree<CGAL::AABB_traits<CGAL::Epeck, CGAL::AABB_triangle_primitive<CGAL::Epeck, std::vector<CGAL::Epeck::Triangle_3, std::allocator<CGAL::Epeck::Triangle_3>>::iterator, CGAL::Boolean_tag<false>>, CGAL::Default>> const&, std::vector<CGAL::Epeck::Triangle_3, std::allocator<CGAL::Epeck::Triangle_3>> const&, std::vector<bool, std::allocator<bool>> const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&);
// generated by autoexplicit.sh
template void igl::copyleft::cgal::closest_facet<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, CGAL::Epeck, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>>(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, std::vector<std::vector<size_t, std::allocator<size_t>>, std::allocator<std::vector<size_t, std::allocator<size_t>>>> const&, std::vector<std::vector<size_t, std::allocator<size_t>>, std::allocator<std::vector<size_t, std::allocator<size_t>>>> const&, CGAL::AABB_tree<CGAL::AABB_traits<CGAL::Epeck, CGAL::AABB_triangle_primitive<CGAL::Epeck, std::vector<CGAL::Epeck::Triangle_3, std::allocator<CGAL::Epeck::Triangle_3>>::iterator, CGAL::Boolean_tag<false>>, CGAL::Default>> const&, std::vector<CGAL::Epeck::Triangle_3, std::allocator<CGAL::Epeck::Triangle_3>> const&, std::vector<bool, std::allocator<bool>> const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&);
// generated by autoexplicit.sh
template void igl::copyleft::cgal::closest_facet<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, CGAL::Epeck, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(
    Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, 
    Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, 
    Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, 
    Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, 
    Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, 
    Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, 
    Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, std::vector<std::vector<size_t, std::allocator<size_t> >, std::allocator<std::vector<size_t, std::allocator<size_t> > > > const&, std::vector<std::vector<size_t, std::allocator<size_t> >, std::allocator<std::vector<size_t, std::allocator<size_t> > > > const&, CGAL::AABB_tree<CGAL::AABB_traits<CGAL::Epeck, CGAL::AABB_triangle_primitive<CGAL::Epeck, std::vector<CGAL::Epeck::Triangle_3, std::allocator<CGAL::Epeck::Triangle_3> >::iterator, CGAL::Boolean_tag<false> >, CGAL::Default> > const&, std::vector<CGAL::Epeck::Triangle_3, std::allocator<CGAL::Epeck::Triangle_3> > const&, std::vector<bool, std::allocator<bool> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#include <cstdint>
template void igl::copyleft::cgal::closest_facet<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, CGAL::Epeck, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(
Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> > const&, 
Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, 
Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, 
Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> > const&, 
Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, 
Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, 
Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, std::vector<std::vector<size_t, std::allocator<size_t> >, std::allocator<std::vector<size_t, std::allocator<size_t> > > > const&, std::vector<std::vector<size_t, std::allocator<size_t> >, std::allocator<std::vector<size_t, std::allocator<size_t> > > > const&, CGAL::AABB_tree<CGAL::AABB_traits<CGAL::Epeck, CGAL::AABB_triangle_primitive<CGAL::Epeck, std::vector<CGAL::Epeck::Triangle_3, std::allocator<CGAL::Epeck::Triangle_3> >::iterator, CGAL::Boolean_tag<false> >, CGAL::Default> > const&, std::vector<CGAL::Epeck::Triangle_3, std::allocator<CGAL::Epeck::Triangle_3> > const&, std::vector<bool, std::allocator<bool> > const&, 
Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, 
Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);

#endif
