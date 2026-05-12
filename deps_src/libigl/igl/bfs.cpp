#include "bfs.h"
#include "list_to_matrix.h"
#include <vector>
#include <queue>

template <
  typename AType,
  typename DerivedD,
  typename DerivedP>
IGL_INLINE void igl::bfs(
  const AType & A,
  const size_t s,
  Eigen::PlainObjectBase<DerivedD> & D,
  Eigen::PlainObjectBase<DerivedP> & P)
{
  std::vector<typename DerivedD::Scalar> vD;
  std::vector<typename DerivedP::Scalar> vP;
  bfs(A,s,vD,vP);
  list_to_matrix(vD,D);
  list_to_matrix(vP,P);
}

template <
  typename AType,
  typename DType,
  typename PType>
IGL_INLINE void igl::bfs(
  const std::vector<std::vector<AType> > & A,
  const size_t s,
  std::vector<DType> & D,
  std::vector<PType> & P)
{
  // number of nodes
  int N = s+1;
  for(const auto & Ai : A) for(const auto & a : Ai) N = std::max(N,a+1);
  std::vector<bool> seen(N,false);
  P.resize(N,-1);
  std::queue<std::pair<int,int> > Q;
  Q.push({s,-1});
  while(!Q.empty())
  {
    const int f = Q.front().first;
    const int p = Q.front().second;
    Q.pop();
    if(seen[f])
    {
      continue;
    }
    D.push_back(f);
    P[f] = p;
    seen[f] = true;
    for(const auto & n : A[f]) Q.push({n,f});
  }
}


template <
  typename AType,
  typename DType,
  typename PType>
IGL_INLINE void igl::bfs(
  const Eigen::SparseCompressedBase<AType> & A,
  const size_t s,
  std::vector<DType> & D,
  std::vector<PType> & P)
{
  // number of nodes
  int N = A.rows();
  assert(A.rows() == A.cols());
  std::vector<bool> seen(N,false);
  P.resize(N,-1);
  std::queue<std::pair<int,int> > Q;
  Q.push({s,-1});
  while(!Q.empty())
  {
    const int f = Q.front().first;
    const int p = Q.front().second;
    Q.pop();
    if(seen[f])
    {
      continue;
    }
    D.push_back(f);
    P[f] = p;
    seen[f] = true;
    for(typename AType::InnerIterator it (A,f); it; ++it)
    {
      if(it.value()) Q.push({it.index(),f});
    }
  }

}

