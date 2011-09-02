#ifndef IGL_DFS_H
#define IGL_DFS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  // Traverse a **directed** graph represented by an adjacency list using
  // depth first search
  //
  // Inputs:
  //   A  #V list of adjacency lists
  //   s  starting node (index into A)
  // Outputs:
  //   D  #V list of indices into rows of A in the order in which graph nodes
  //     are discovered.
  //   P  #V list of indices into rows of A of predecessor in resulting
  //     spanning tree {-1 indicates root/not discovered), order corresponds to
  //     V **not** D.
  //   C  #V list of indices into rows of A in order that nodes are "closed"
  //     (all descendants have been discovered)
  template <
    typename AType,
    typename DerivedD,
    typename DerivedP,
    typename DerivedC>
  IGL_INLINE void dfs(
    const std::vector<std::vector<AType> > & A,
    const size_t s,
    Eigen::PlainObjectBase<DerivedD> & D,
    Eigen::PlainObjectBase<DerivedP> & P,
    Eigen::PlainObjectBase<DerivedC> & C);
  template <
    typename AType,
    typename DType,
    typename PType,
    typename CType>
  IGL_INLINE void dfs(
    const std::vector<std::vector<AType> > & A,
    const size_t s,
    std::vector<DType> & D,
    std::vector<PType> & P,
    std::vector<CType> & C);

}
#ifndef IGL_STATIC_LIBRARY
#  include "dfs.cpp"
#endif
#endif
