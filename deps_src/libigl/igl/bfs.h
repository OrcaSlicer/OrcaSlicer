#ifndef IGL_BFS_H
#define IGL_BFS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
#include <Eigen/Sparse>
namespace igl
{
  // Traverse a **directed** graph represented by an adjacency list using
  // breadth first search
  //
  // Inputs:
  //   A  #V list of adjacency lists  or #V by #V adjacency matrix
  //   s  starting node (index into A)
  // Outputs:
  //   D  #V list of indices into rows of A in the order in which graph nodes
  //     are discovered.
  //   P  #V list of indices into rows of A of predecessor in resulting
  //     spanning tree {-1 indicates root/not discovered), order corresponds to
  //     V **not** D.
  template <
    typename AType,
    typename DerivedD,
    typename DerivedP>
  IGL_INLINE void bfs(
    const AType & A,
    const size_t s,
    Eigen::PlainObjectBase<DerivedD> & D,
    Eigen::PlainObjectBase<DerivedP> & P);

  template <
    typename AType,
    typename DType,
    typename PType>
  IGL_INLINE void bfs(
    const std::vector<std::vector<AType> > & A,
    const size_t s,
    std::vector<DType> & D,
    std::vector<PType> & P);
  template <
    typename AType,
    typename DType,
    typename PType>
  IGL_INLINE void bfs(
    const Eigen::SparseMatrix<AType> & A,
    const size_t s,
    std::vector<DType> & D,
    std::vector<PType> & P);
}
#ifndef IGL_STATIC_LIBRARY
#  include "bfs.cpp"
#endif
#endif

