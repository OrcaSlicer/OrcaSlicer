#include "edges_to_path.h"
#include "dfs.h"
#include "sort.h"
#include "ismember_rows.h"
#include "unique.h"
#include "adjacency_list.h"
#include "PlainMatrix.h"

template <
  typename DerivedE,
  typename DerivedI,
  typename DerivedJ,
  typename DerivedK>
IGL_INLINE void igl::edges_to_path(
  const Eigen::MatrixBase<DerivedE> & OE,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedJ> & J,
  Eigen::PlainObjectBase<DerivedK> & K)
{
  assert(OE.rows()>=1);
  if(OE.rows() == 1)
  {
    I.resize(2);
    I(0) = OE(0);
    I(1) = OE(1);
    J.resize(1);
    J(0) = 0;
    K.resize(1);
    K(0) = 0;
  }

  // Compute on reduced graph
  DerivedI U;
  Eigen::VectorXi vE;
  {
    Eigen::VectorXi IA;
    unique(OE,U,IA,vE);
  }

  Eigen::VectorXi V = Eigen::VectorXi::Zero(vE.maxCoeff()+1);
  for(int e = 0;e<vE.size();e++)
  {
    V(vE(e))++;
    assert(V(vE(e))<=2);
  }
  // Try to find a vertex with valence = 1
  int c = 2;
  int s = vE(0);
  for(int v = 0;v<V.size();v++)
  {
    if(V(v) == 1)
    {
      c = V(v);
      s = v;
      break;
    }
  }
  assert(V(s) == c);
  assert(c == 2 || c == 1);

  // reshape E to be #E by 2
  PlainMatrix<DerivedE> E = Eigen::Map<DerivedE>(vE.data(),OE.rows(),OE.cols()).eval();
  {
    std::vector<std::vector<int> > A;
    igl::adjacency_list(E,A);
    Eigen::VectorXi P,C;
    dfs(A,s,I,P,C);
  }
  if(c == 2)
  {
    I.conservativeResize(I.size()+1);
    I(I.size()-1) = I(0);
  }

  PlainMatrix<DerivedE> sE;
  Eigen::Matrix<typename DerivedI::Scalar,Eigen::Dynamic,2> sEI;
  {
    Eigen::MatrixXi _;
    sort(E,2,true,sE,_);
    Eigen::Matrix<typename DerivedI::Scalar,Eigen::Dynamic,2> EI(I.size()-1,2);
    EI.col(0) = I.head(I.size()-1);
    EI.col(1) = I.tail(I.size()-1);
    sort(EI,2,true,sEI,_);
  }
  {
    Eigen::Array<bool,Eigen::Dynamic,1> F;
    ismember_rows(sEI,sE,F,J);
  }
  K.resize(I.size()-1);
  for(int k = 0;k<K.size();k++)
  {
    K(k) = (E(J(k),0) != I(k) ? 1 : 0);
  }

  // Map vertex indices onto original graph
  I = U(I.derived()).eval();
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::edges_to_path<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>>(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&);
template void igl::edges_to_path<Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
