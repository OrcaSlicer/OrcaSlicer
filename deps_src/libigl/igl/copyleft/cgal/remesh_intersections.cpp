// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "remesh_intersections.h"
#include "assign_scalar.h"
#include "projected_cdt.h"
#include "../../get_seconds.h"
#include "../../parallel_for.h"
#include "../../LinSpaced.h"
#include "../../C_STR.h"
#include "../../STR.h"
#include "../../unique_rows.h"

#include <vector>
#include <map>
#include <queue>
#include <unordered_map>
#include <iostream>
#include <cstdio>

//#define REMESH_INTERSECTIONS_TIMING

// Helper function to invoke .exact() on CGAL::Epeck::FT and no-op on others
template <typename T> inline void exact(T & v);
template <> inline void exact(CGAL::Epeck::FT & v) { v = v.exact(); }
template <typename T> inline void exact(T & /* v */ ){}

template <
  typename DerivedV,
  typename DerivedF,
  typename Kernel,
  typename DerivedVV,
  typename DerivedFF,
  typename DerivedJ,
  typename DerivedIM>
IGL_INLINE void igl::copyleft::cgal::remesh_intersections(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const std::vector<CGAL::Triangle_3<Kernel> > & T,
  const std::map<
    typename DerivedF::Index,
    std::vector<
      std::pair<typename DerivedF::Index, CGAL::Object> > > & offending,
  bool stitch_all,
  bool slow_and_more_precise_rounding,
  Eigen::PlainObjectBase<DerivedVV> & VV,
  Eigen::PlainObjectBase<DerivedFF> & FF,
  Eigen::PlainObjectBase<DerivedJ> & J,
  Eigen::PlainObjectBase<DerivedIM> & IM)
{

#ifdef REMESH_INTERSECTIONS_TIMING
    const auto & tictoc = []() -> double
    {
      static double t_start = igl::get_seconds();
      double diff = igl::get_seconds()-t_start;
      t_start += diff;
      return diff;
    };
    const auto log_time = [&](const std::string& label) -> void {
      printf("%50s: %0.5lf\n",
        C_STR("remesh_intersections." << label),tictoc());
    };
    tictoc();
#endif

    typedef CGAL::Point_3<Kernel>    Point_3;
    typedef CGAL::Segment_3<Kernel>  Segment_3; 
    typedef CGAL::Plane_3<Kernel>    Plane_3;

    typedef typename DerivedF::Index Index;
    typedef std::pair<Index, Index> Edge;
    struct EdgeHash {
        size_t operator()(const Edge& e) const {
            return (e.first * 805306457) ^ (e.second * 201326611);
        }
    };
    typedef std::unordered_map<Edge, std::vector<Index>, EdgeHash > EdgeMap;

    const size_t num_faces = F.rows();
    const size_t num_base_vertices = V.rows();
    assert(num_faces == T.size());

    std::vector<bool> is_offending(num_faces, false);
    for (const auto itr : offending)
    {
      const auto& fid = itr.first;
      is_offending[fid] = true;
    }

    // Cluster overlaps so that co-planar clusters are resolved only once
    std::unordered_map<Index, std::vector<Index> > intersecting_and_coplanar;
    for (const auto itr : offending)
    {
      const auto& fi = itr.first;
      const auto P = T[fi].supporting_plane();
      assert(!P.is_degenerate());
      for (const auto jtr : itr.second) 
      {
        const auto& fj = jtr.first;
        const auto& tj = T[fj];
        if (P.has_on(tj[0]) && P.has_on(tj[1]) && P.has_on(tj[2]))
        {
          auto loc = intersecting_and_coplanar.find(fi);
          if (loc == intersecting_and_coplanar.end())
          {
            intersecting_and_coplanar[fi] = {fj};
          } else
          {
            loc->second.push_back(fj);
          }
        }
      }
    }
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("overlap_analysis");
#endif

    std::vector<std::vector<Index> > resolved_faces;
    std::vector<Index> source_faces;
    std::vector<Point_3> new_vertices;
    EdgeMap edge_vertices;
    // face_vertices: Given a face Index, find vertices inside the face
    std::unordered_map<Index, std::vector<Index>> face_vertices;

    // Run constraint Delaunay triangulation on the plane.
    // 
    // Inputs:
    //   P  plane to triangulate upone
    //   involved_faces  #F list of indices into triangle of involved faces
    // Outputs:
    //   vertices  #V list of vertex positions of output triangulation
    //   faces   #F list of face indices into vertices of output triangulation
    //
    auto delaunay_triangulation = [&offending, &T](
      const Plane_3& P,
      const std::vector<Index>& involved_faces,
      std::vector<Point_3>& vertices,
      std::vector<std::vector<Index> >& faces) -> void 
    {
      std::vector<CGAL::Object> objects;

      // insert each face into a common cdt
      for (const auto& fid : involved_faces)
      {
        const auto itr = offending.find(fid);
        const auto& triangle = T[fid];
        objects.push_back(CGAL::make_object(triangle));
        if (itr == offending.end())
        {
          continue;
        }
        for (const auto& index_obj : itr->second) 
        {
          //const auto& ofid = index_obj.first;
          const auto& obj = index_obj.second;
          objects.push_back(obj);
        }
      }
      projected_cdt(objects,P,vertices,faces);
    };

    // Given p on triangle indexed by ori_f, add point to list of vertices return index of p.
    //
    // Input:
    //   p  point to search for
    //   ori_f  index of triangle p is corner of
    // Returns global index of vertex (dependent on whether stitch_all flag is
    // set)
    //
    auto find_or_append_point = [&](
      const Point_3& p, 
      const size_t ori_f) -> Index 
    {
      assert(stitch_all == false);
      // Stitching triangles according to input connectivity.
      // This step is potentially costly.
      const auto& triangle = T[ori_f];
      const auto& f = F.row(ori_f).eval();

      // Check if p is one of the triangle corners.
      for (size_t i=0; i<3; i++) 
      {
        if (p == triangle[i]) return f[i];
      }

      // Check if p is on one of the edges.
      for (size_t i=0; i<3; i++) {
        const Point_3 curr_corner = triangle[i];
        const Point_3 next_corner = triangle[(i+1)%3];
        const Segment_3 edge(curr_corner, next_corner);
        if (edge.has_on(p)) {
          const Index curr = f[i];
          const Index next = f[(i+1)%3];
          Edge key;
          key.first = curr<next?curr:next;
          key.second = curr<next?next:curr;
          auto itr = edge_vertices.find(key);
          if (itr == edge_vertices.end()) {
            const Index index =
              num_base_vertices + new_vertices.size();
            edge_vertices.insert({key, {index}});
            new_vertices.push_back(p);
            return index;
          } else {
            for (const auto vid : itr->second) {
              if (p == new_vertices[vid - num_base_vertices]) {
                return vid;
              }
            }
            const size_t index = num_base_vertices + new_vertices.size();
            new_vertices.push_back(p);
            itr->second.push_back(index);
            return index;
          }
        }
      }

      // p must be in the middle of the triangle.
      auto & existing_face_vertices = face_vertices[ori_f];
      for(const auto vid : existing_face_vertices) {
        if (p == new_vertices[vid - num_base_vertices]) {
          return vid;
        }
      }
      const size_t index = num_base_vertices + new_vertices.size();
      new_vertices.push_back(p);
      existing_face_vertices.push_back(index);
      return index;
      
    };

    // Determine the vertex indices for each corner of each output triangle.
    //
    // Note: When `stitch_all == false` this produces copies of new vertices for
    // every new triangle, relying on the call to `unique` below to de-duplicate
    // them. Seems this could at least only create one copy per CDT. In general,
    // this could mean 12 copies of a new vertex rather than just 2.
    // 
    // Inputs:
    //   vertices  #V list of vertices of cdt
    //   faces  #F list of list of face indices into vertices of cdt
    //   involved_faces  list of involved faces on the plane of cdt
    // Side effects:
    //   - add faces to resolved_faces
    //   - add corresponding original face to source_faces
    //   - 
    auto post_triangulation_process = [&](
      const std::vector<Point_3>& vertices,
      const std::vector<std::vector<Index> >& faces,
      const std::vector<Index>& involved_faces) -> void 
    {
      assert(involved_faces.size() > 0);
      std::vector<Index> indices(vertices.size());
      // If we're stitching all points together then we can insert just one copy
      // of each new vertex in this cdt and reference it for each new face.
      if(stitch_all)
      {
        for(size_t i = 0;i<vertices.size();i++)
        {
          indices[i] = num_base_vertices + new_vertices.size();
          new_vertices.push_back(vertices[i]);
        }
      }
      // for all faces of the cdt
      for (const auto& f : faces) 
      {
        std::vector<Index> corners(3);
        if(stitch_all){ for(size_t i =0;i<3;i++){ corners[i] = indices[f[i]];} }
        const Point_3& v0 = vertices[f[0]];
        const Point_3& v1 = vertices[f[1]];
        const Point_3& v2 = vertices[f[2]];
        if(involved_faces.size() == 1) 
        {
          const auto& ori_f = involved_faces[0];
          // If only there is only one involved face, all sub-triangles must
          // belong to it and have the correct orientation.
          if(!stitch_all)
          {
            corners[0] = find_or_append_point(v0, ori_f);
            corners[1] = find_or_append_point(v1, ori_f);
            corners[2] = find_or_append_point(v2, ori_f);
          }
          resolved_faces.emplace_back(corners);
          source_faces.push_back(ori_f);
        }else 
        {
          // CGAL is silly about adding points together: 
          // https://stackoverflow.com/questions/46693301/why-can-i-not-add-points-in-cgal
          Point_3 center(
            (v0[0] + v1[0] + v2[0]) / 3.0,
            (v0[1] + v1[1] + v2[1]) / 3.0,
            (v0[2] + v1[2] + v2[2]) / 3.0);
          // O(n²) type loop for a large co-planar patch
          for (const auto& ori_f : involved_faces) 
          {
            const auto& triangle = T[ori_f];
            if (triangle.has_on(center)) 
            {
              const Plane_3 P = triangle.supporting_plane();
              if(!stitch_all)
              {
                corners[0] = find_or_append_point(v0, ori_f);
                corners[1] = find_or_append_point(v1, ori_f);
                corners[2] = find_or_append_point(v2, ori_f);
              }
              bool was_flipped = false;
              if(
                CGAL::orientation( P.to_2d(v0), P.to_2d(v1), P.to_2d(v2)) ==
                CGAL::RIGHT_TURN)
              {
                was_flipped = true;
                std::swap(corners[0], corners[1]);
              }
              resolved_faces.emplace_back(corners);
              // swap back
              if(was_flipped) { std::swap(corners[0], corners[1]); }
              source_faces.push_back(ori_f);
            }
          }
        }
      }
    };

    // Process un-touched faces.
    for (size_t i=0; i<num_faces; i++) 
    {
      if (!is_offending[i] && !T[i].is_degenerate()) 
      {
        resolved_faces.push_back( { F(i,0), F(i,1), F(i,2) } );
        source_faces.push_back(i);
      }
    }

    // Process self-intersecting faces.
    std::vector<bool> processed(num_faces, false);
    std::vector<std::pair<Plane_3, std::vector<Index> > > cdt_inputs;
    for (const auto itr : offending) 
    {
      const auto fid = itr.first;
      if (processed[fid]) continue;
      processed[fid] = true;

      const auto loc = intersecting_and_coplanar.find(fid);
      std::vector<Index> involved_faces;
      if (loc == intersecting_and_coplanar.end()) 
      {
        involved_faces.push_back(fid);
      } else 
      {
        std::queue<Index> Q;
        Q.push(fid);
        while (!Q.empty()) 
        {
          const auto index = Q.front();
          involved_faces.push_back(index);
          Q.pop();

          const auto overlapping_faces = intersecting_and_coplanar.find(index);
          assert(overlapping_faces != intersecting_and_coplanar.end());

          for (const auto other_index : overlapping_faces->second) 
          {
            if (processed[other_index]) continue;
            processed[other_index] = true;
            Q.push(other_index);
          }
        }
      }

      Plane_3 P = T[fid].supporting_plane();
      cdt_inputs.emplace_back(P, involved_faces);
    }
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("preprocess");
#endif

    const size_t num_cdts = cdt_inputs.size();
    std::vector<std::vector<Point_3> > cdt_vertices(num_cdts);
    std::vector<std::vector<std::vector<Index> > > cdt_faces(num_cdts);

    igl::parallel_for(num_cdts,[&](int i)
    {
      auto& vertices = cdt_vertices[i];
      auto& faces = cdt_faces[i];
      const auto& P = cdt_inputs[i].first;
      const auto& involved_faces = cdt_inputs[i].second;
      delaunay_triangulation(P, involved_faces, vertices, faces);
    } ,1000);
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("cdt");
#endif

    for (size_t i=0; i<num_cdts; i++) 
    {
      const auto& vertices = cdt_vertices[i];
      const auto& faces = cdt_faces[i];
      const auto& involved_faces = cdt_inputs[i].second;
      post_triangulation_process(vertices, faces, involved_faces);
    }
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("stitching");
#endif

    // Output resolved mesh.
    const size_t num_out_vertices = new_vertices.size() + num_base_vertices;
    // Use scratch memory if we're stitching to avoid a copy below.
    DerivedVV scratch;
    Eigen::PlainObjectBase<DerivedVV> & U = stitch_all? scratch: VV;
    U.resize(num_out_vertices, 3);

    igl::parallel_for(num_base_vertices,[&](Eigen::Index i)
    {
      assign_scalar(V(i,0), slow_and_more_precise_rounding, U(i,0));
      assign_scalar(V(i,1), slow_and_more_precise_rounding, U(i,1));
      assign_scalar(V(i,2), slow_and_more_precise_rounding, U(i,2));
    },1000);
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("assigning0");
#endif

    igl::parallel_for(num_out_vertices-num_base_vertices,[&](size_t i)
    {
      const size_t j = i+num_base_vertices;
      assign_scalar(new_vertices[i][0],slow_and_more_precise_rounding,U(j,0));
      assign_scalar(new_vertices[i][1],slow_and_more_precise_rounding,U(j,1));
      assign_scalar(new_vertices[i][2],slow_and_more_precise_rounding,U(j,2));
    },1000);
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("assigning1");
#endif

    const size_t num_out_faces = resolved_faces.size();
    FF.resize(num_out_faces, 3);
    for (size_t i=0; i<num_out_faces; i++) 
    {
      FF(i,0) = resolved_faces[i][0];
      FF(i,1) = resolved_faces[i][1];
      FF(i,2) = resolved_faces[i][2];
    }
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("FF copy");
#endif

    J.resize(num_out_faces);
    std::copy(source_faces.begin(), source_faces.end(), J.data());
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("J copy");
#endif

    // Extract unique vertex indices.
    const size_t U_size = U.rows();
    IM.resize(U_size,1);

    // Try to minimize any overhead here but avoid shinanigans trying to get
    // .exact() call into a function containing the loop called on the matrix
    // with so many Eigen parameters and possibly a type that doesn't have
    // .exact()
    igl::parallel_for(U.size(),[&](Eigen::Index &i){ exact(*(U.data()+i)); },
      std::is_same<typename DerivedVV::Scalar,CGAL::Epeck::FT>()?
        1000:U.size()+1);
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("exact");
#endif
    // Write directly into VV if stitching, else use scratch (in fact if not
    // stitching then we don't need this output at all).
    Eigen::PlainObjectBase<DerivedVV> & unique_vv = stitch_all ? VV : scratch;
    // This is not stable... So even if offending is empty V != U in
    // general...
    Eigen::VectorXi unique_to_vv, vv_to_unique;
    igl::unique_rows(U, unique_vv, unique_to_vv, vv_to_unique);
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("unique");
#endif
    if(stitch_all) 
    {
      // Merge all vertices having the same coordinates into a single vertex
      // and set IM to identity map.
      std::transform(FF.data(), FF.data() + FF.rows()*FF.cols(),
          FF.data(), [&vv_to_unique](const typename DerivedFF::Scalar& a)
          { return vv_to_unique[a]; });
#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("transform");
#endif
      IM.resize(unique_vv.rows());
      // Have to use << instead of = because Eigen's PlainObjectBase is annoying
      IM << igl::LinSpaced<
        Eigen::Matrix<typename DerivedIM::Scalar, Eigen::Dynamic,1 >
        >(unique_vv.rows(), 0, unique_vv.rows()-1);
    }else 
    {
      // Vertices with the same coordinates would be represented by one vertex.
      // The IM value of a vertex is the index of the representative vertex.
      for (Index v=0; v<(Index)U_size; v++) {
        IM(v) = unique_to_vv[vv_to_unique[v]];
      }
    }

#ifdef REMESH_INTERSECTIONS_TIMING
    log_time("store_results");
#endif
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
//
// To-do: we should really stop using Eigen::Index or anything but `int` for
// index types, it causes this bloating of templates below and chaos on windows.
//
// generated by autoexplicit.sh
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
// generated by autoexplicit.sh
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
// generated by autoexplicit.sh
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
// generated by autoexplicit.sh
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
// generated by autoexplicit.sh
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, 3, 0, -1, 3>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#include <cstdint>
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1>, Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1>, Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epeck, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epeck>, std::allocator<CGAL::Triangle_3<CGAL::Epeck> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const&, std::map<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > >, std::less<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index const, std::vector<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object>, std::allocator<std::pair<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Index, CGAL::Object> > > > > > const&, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<std::ptrdiff_t, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#ifdef WIN32
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT,-1,3,0,-1,3>,Eigen::Matrix<int,-1,3,0,-1,3>,CGAL::Epick,Eigen::Matrix<CGAL::Epeck::FT,-1,3,0,-1,3>,Eigen::Matrix<int,-1,-1,0,-1,-1>,Eigen::Matrix<int,-1,1,0,-1,1>,Eigen::Matrix<int,-1,1,0,-1,1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT,-1,3,0,-1,3> >const &,Eigen::MatrixBase<Eigen::Matrix<int,-1,3,0,-1,3> > const &,std::vector<CGAL::Triangle_3<CGAL::Epick>,std::allocator<CGAL::Triangle_3<CGAL::Epick>> > const &,std::map<__int64,std::vector<struct std::pair<__int64,CGAL::Object>,std::allocator<struct std::pair<__int64,CGAL::Object> > >,struct std::less<__int64>,std::allocator<struct std::pair<__int64 const ,std::vector<struct std::pair<__int64,CGAL::Object>,std::allocator<struct std::pair<__int64,CGAL::Object> > > > > > const &,bool,bool,Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT,-1,3,0,-1,3> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,-1,0,-1,-1> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,1,0,-1,1> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,1,0,-1,1> > &);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT,-1,3,0,-1,3>,Eigen::Matrix<int,-1,3,0,-1,3>,CGAL::Epick,Eigen::Matrix<CGAL::Epeck::FT,-1,3,1,-1,3>,Eigen::Matrix<int,-1,-1,0,-1,-1>,Eigen::Matrix<int,-1,1,0,-1,1>,Eigen::Matrix<int,-1,1,0,-1,1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT,-1,3,0,-1,3> >const &,Eigen::MatrixBase<Eigen::Matrix<int,-1,3,0,-1,3> > const &,std::vector<CGAL::Triangle_3<CGAL::Epick>,std::allocator<CGAL::Triangle_3<CGAL::Epick>> > const &,std::map<__int64,std::vector<struct std::pair<__int64,CGAL::Object>,std::allocator<struct std::pair<__int64,CGAL::Object> > >,struct std::less<__int64>,std::allocator<struct std::pair<__int64 const ,std::vector<struct std::pair<__int64,CGAL::Object>,std::allocator<struct std::pair<__int64,CGAL::Object> > > > > > const &,bool,bool,Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT,-1,3,1,-1,3> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,-1,0,-1,-1> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,1,0,-1,1> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,1,0,-1,1> > &);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT,-1,-1,1,-1,-1>,Eigen::Matrix<int,-1,-1,0,-1,-1>,CGAL::Epick,Eigen::Matrix<CGAL::Epeck::FT,-1,-1,1,-1,-1>,Eigen::Matrix<int,-1,-1,0,-1,-1>,Eigen::Matrix<int,-1,1,0,-1,1>,Eigen::Matrix<int,-1,1,0,-1,1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT,-1,-1,1,-1,-1> > const &,Eigen::MatrixBase<Eigen::Matrix<int,-1,-1,0,-1,-1> > const &,std::vector<CGAL::Triangle_3<CGAL::Epick>,std::allocator<CGAL::Triangle_3<CGAL::Epick>> > const &,std::map<__int64,std::vector<struct std::pair<__int64,CGAL::Object>,std::allocator<struct std::pair<__int64,CGAL::Object> > >,struct std::less<__int64>,std::allocator<struct std::pair<__int64 const ,std::vector<struct std::pair<__int64,CGAL::Object>,std::allocator<struct std::pair<__int64,CGAL::Object> > > > > > const &,bool,bool,Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT,-1,-1,1,-1,-1> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,-1,0,-1,-1> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,1,0,-1,1> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,1,0,-1,1> > &);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT,-1,3,0,-1,3>,Eigen::Matrix<int,-1,-1,0,-1,-1>,CGAL::Epick,Eigen::Matrix<CGAL::Epeck::FT,-1,3,0,-1,3>,Eigen::Matrix<int,-1,-1,0,-1,-1>,Eigen::Matrix<int,-1,1,0,-1,1>,Eigen::Matrix<int,-1,1,0,-1,1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT,-1,3,0,-1,3> > const &,Eigen::MatrixBase<Eigen::Matrix<int,-1,-1,0,-1,-1> > const &,std::vector<CGAL::Triangle_3<CGAL::Epick>,std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const &,std::map<__int64,std::vector<struct std::pair<__int64,CGAL::Object>,std::allocator<struct std::pair<__int64,CGAL::Object> > >,struct std::less<__int64>,std::allocator<struct std::pair<__int64 const ,std::vector<struct std::pair<__int64,CGAL::Object>,std::allocator<struct std::pair<__int64,CGAL::Object> > > > > > const &,bool,bool,Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT,-1,3,0,-1,3> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,-1,0,-1,-1> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,1,0,-1,1> > &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,1,0,-1,1> > &);
template void igl::copyleft::cgal::remesh_intersections<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, CGAL::Epick, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const &, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const &, std::vector<CGAL::Triangle_3<CGAL::Epick>, std::allocator<CGAL::Triangle_3<CGAL::Epick> > > const &, std::map<__int64, std::vector<struct std::pair<__int64, CGAL::Object>, std::allocator<struct std::pair<__int64, CGAL::Object> > >, struct std::less<__int64>, std::allocator<struct std::pair<__int64 const , std::vector<struct std::pair<__int64, CGAL::Object>, std::allocator<struct std::pair<__int64, CGAL::Object> > > > > > const &, bool, bool, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > &, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > &, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > &, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > &);
#endif
#endif
