#ifndef IGL_CUT_TO_DISK_H
#define IGL_CUT_TO_DISK_H
#include "igl_inline.h"

#include <Eigen/Core>

#include <vector>

namespace igl
{
  /// Given a triangle mesh, computes a set of edge cuts sufficient to carve the 
  /// mesh into a topological disk, without disconnecting any connected components.
  /// Nothing else about the cuts (including number, total length, or smoothness)
  /// is guaranteed to be optimal.
  ///
  /// Simply-connected components without boundary (topological spheres) are left
  /// untouched (delete any edge if you really want a disk). 
  /// All other connected components are cut into disks. Meshes with boundary are
  /// supported; boundary edges will be included as cuts.
  ///
  /// The cut mesh itself can be materialized using cut_mesh().
  ///
  /// Implements the triangle-deletion approach described by Gu et al's
  /// "Geometry Images."
  ///
  /// @tparam Index  Integrable type large enough to represent the total number of faces
  ///     and edges in the surface represented by F, and all entries of F.
  /// @param[in] F  #F by 3 list of the faces (must be triangles)
  /// @param[out] cuts  List of cuts. Each cut is a sequence of vertex indices (where
  ///     pairs of consecutive vertices share a face), is simple, and is either
  ///     a closed loop (in which the first and last indices are identical) or
  ///     an open curve. Cuts are edge-disjoint.
  ///
  template <
    typename DerivedF,
    typename Index>
  IGL_INLINE void cut_to_disk(
    const Eigen::MatrixBase<DerivedF> &F,
    std::vector<std::vector<Index> > &cuts);    
};

#ifndef IGL_STATIC_LIBRARY
#include "cut_to_disk.cpp"
#endif

#endif
