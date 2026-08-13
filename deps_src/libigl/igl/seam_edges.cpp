// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Yotam Gingold <yotam@yotamgingold.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#include "seam_edges.h"
#include <unordered_map>
#include <unordered_set>
#include <cassert>

// Yotam has verified that this function produces the exact same output as
// `find_seam_fast.py` for `cow_triangled.obj`.
template <
  typename DerivedV, 
  typename DerivedTC,
  typename DerivedF, 
  typename DerivedFTC,
  typename Derivedseams,
  typename Derivedboundaries,
  typename Derivedfoldovers>
IGL_INLINE void igl::seam_edges(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedTC>& TC,
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedFTC>& FTC,
  Eigen::PlainObjectBase<Derivedseams>& seams,
  Eigen::PlainObjectBase<Derivedboundaries>& boundaries,
  Eigen::PlainObjectBase<Derivedfoldovers>& foldovers)
{
  // Assume triangles.
  assert( F.cols() == 3 );
  assert( F.cols() == FTC.cols() );
  assert( F.rows() == FTC.rows() );
    
  // Assume 2D texture coordinates (foldovers tests).
  assert( TC.cols() == 2 );
  typedef Eigen::Matrix< typename DerivedTC::Scalar, 2, 1 > Vector2S;
  // Computes the orientation of `c` relative to the line between `a` and `b`.
  // Assumes 2D vector input.
  // Based on: https://www.cs.cmu.edu/~quake/robust.html
  const auto& Orientation = []( 
    const Vector2S& a, 
    const Vector2S& b, 
    const Vector2S& c ) -> typename DerivedTC::Scalar
  {
      const Vector2S row0 = a - c;
      const Vector2S row1 = b - c;
      return row0(0)*row1(1) - row1(0)*row0(1);
  };
    
  seams     .setZero( 3*F.rows(), 4 );
  boundaries.setZero( 3*F.rows(), 2 );
  foldovers .setZero( 3*F.rows(), 4 );
    
  int num_seams = 0;
  int num_boundaries = 0;
  int num_foldovers = 0;
    
  // A map from a pair of vertex indices to the index (face and endpoints)
  // into face_position_indices.
  // The following should be true for every key, value pair:
  //    key == face_position_indices[ value ]
  // This gives us a "reverse map" so that we can look up other face
  // attributes based on position edges.
  // The value are written in the format returned by numpy.where(),
  // which stores multi-dimensional indices such as array[a0,b0], array[a1,b1]
  // as ( (a0,a1), (b0,b1) ).
    
  // We need to make a hash function for our directed edges.
  // We'll use i*V.rows() + j.
  typedef std::pair< typename DerivedF::Scalar, typename DerivedF::Scalar > 
    directed_edge;
	const int numV = V.rows();
	const int numF = F.rows();
	const auto& edge_hasher = 
    [numV]( directed_edge const& e ) { return e.first*numV + e.second; };
  // When we pass a hash function object, we also need to specify the number of
  // buckets. The Euler characteristic says that the number of undirected edges
  // is numV + numF -2*genus.
	std::unordered_map<directed_edge,std::pair<int,int>,decltype(edge_hasher) > 
    directed_position_edge2face_position_index(2*( numV + numF ), edge_hasher);
  for( int fi = 0; fi < F.rows(); ++fi ) 
  {
    for( int i = 0; i < 3; ++i )
    {
      const int j = ( i+1 ) % 3;
      directed_position_edge2face_position_index[ 
        std::make_pair( F(fi,i), F(fi,j) ) ] = std::make_pair( fi, i );
    }
  }
    
  // First find all undirected position edges (collect a canonical orientation
  // of the directed edges).
  std::unordered_set< directed_edge, decltype( edge_hasher ) > 
    undirected_position_edges( numV + numF, edge_hasher );
  for( const auto& el : directed_position_edge2face_position_index ) 
  {
    // The canonical orientation is the one where the smaller of
    // the two vertex indices is first.
    undirected_position_edges.insert( std::make_pair( 
      std::min( el.first.first, el.first.second ), 
      std::max( el.first.first, el.first.second ) ) );
  }
    
  // Now we will iterate over all position edges.
  // Seam edges are the edges whose two opposite directed edges have different
  // texcoord indices (or one doesn't exist at all in the case of a mesh
  // boundary).
  for( const auto& vp_edge : undirected_position_edges ) 
  {
    // We should only see canonical edges,
    // where the first vertex index is smaller.
    assert( vp_edge.first < vp_edge.second );
        
    const auto vp_edge_reverse = std::make_pair(vp_edge.second, vp_edge.first);
    // If it and its opposite exist as directed edges, check if their
    // texture coordinate indices match.
    if( directed_position_edge2face_position_index.count( vp_edge ) &&
      directed_position_edge2face_position_index.count( vp_edge_reverse ) ) 
    {
      const auto forwards = 
        directed_position_edge2face_position_index[ vp_edge ];
      const auto backwards = 
        directed_position_edge2face_position_index[ vp_edge_reverse ];
            
      // NOTE: They should never be equal.
      assert( forwards != backwards );
            
      // If the texcoord indices match (are similarly flipped),
      // this edge is not a seam. It could be a foldover.
      if( 
        std::make_pair( 
          FTC( forwards.first, forwards.second ), 
          FTC( forwards.first, ( forwards.second+1 ) % 3 ) )
        ==
        std::make_pair( 
          FTC( backwards.first, ( backwards.second+1 ) % 3 ), 
          FTC( backwards.first, backwards.second ) )) 
      {
        // Check for foldovers in UV space.
        // Get the edge (a,b) and the two opposite vertices's texture
        // coordinates.
        const Vector2S a = TC.row( FTC( forwards.first,  forwards.second ) );
        const Vector2S b = 
          TC.row( FTC( forwards.first, (forwards.second+1) % 3 ) );
        const Vector2S c_forwards  = 
          TC.row( FTC( forwards .first, (forwards .second+2) % 3 ) );
        const Vector2S c_backwards = 
          TC.row( FTC( backwards.first, (backwards.second+2) % 3 ) );
        // If the opposite vertices' texture coordinates fall on the same side
        // of the edge, we have a UV-space foldover.
        const auto orientation_forwards = Orientation( a, b, c_forwards );
        const auto orientation_backwards = Orientation( a, b, c_backwards );
        if( ( orientation_forwards > 0 && orientation_backwards > 0 ) ||
            ( orientation_forwards < 0 && orientation_backwards < 0 )
            ) {
            foldovers( num_foldovers, 0 ) = forwards.first;
            foldovers( num_foldovers, 1 ) = forwards.second;
            foldovers( num_foldovers, 2 ) = backwards.first;
            foldovers( num_foldovers, 3 ) = backwards.second;
            num_foldovers += 1;
        }
      }
      // Otherwise, we have a non-matching seam edge.
      else 
      {
        seams( num_seams, 0 ) = forwards.first;
        seams( num_seams, 1 ) = forwards.second;
        seams( num_seams, 2 ) = backwards.first;
        seams( num_seams, 3 ) = backwards.second;
        num_seams += 1;
      }
    }
    // Otherwise, the edge and its opposite aren't both in the directed edges.
    // One of them should be.
    else if( directed_position_edge2face_position_index.count( vp_edge ) ) 
    {
      const auto forwards = directed_position_edge2face_position_index[vp_edge];
      boundaries( num_boundaries, 0 ) = forwards.first;
      boundaries( num_boundaries, 1 ) = forwards.second;
      num_boundaries += 1;
    } else if( 
      directed_position_edge2face_position_index.count( vp_edge_reverse ) ) 
    {
      const auto backwards = 
        directed_position_edge2face_position_index[ vp_edge_reverse ];
      boundaries( num_boundaries, 0 ) = backwards.first;
      boundaries( num_boundaries, 1 ) = backwards.second;
      num_boundaries += 1;
    } else {
      // This should never happen! One of these two must have been seen.
      assert(
        directed_position_edge2face_position_index.count( vp_edge ) ||
        directed_position_edge2face_position_index.count( vp_edge_reverse )
      );
    }
  }
    
  seams     .conservativeResize( num_seams,      Eigen::NoChange_t() );
  boundaries.conservativeResize( num_boundaries, Eigen::NoChange_t() );
  foldovers .conservativeResize( num_foldovers,  Eigen::NoChange_t() );
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::seam_edges<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
