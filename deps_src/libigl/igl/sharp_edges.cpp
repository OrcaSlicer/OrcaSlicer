#include "sharp_edges.h"
#include "unique_edge_map.h"
#include "per_face_normals.h"
#include "PI.h"
#include <Eigen/Geometry>

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedSE,
  typename DerivedE,
  typename DeriveduE,
  typename DerivedEMAP,
  typename uE2Etype,
  typename sharptype>
IGL_INLINE void igl::sharp_edges(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const typename DerivedV::Scalar angle,
  Eigen::PlainObjectBase<DerivedSE> & SE,
  Eigen::PlainObjectBase<DerivedE> & E,
  Eigen::PlainObjectBase<DeriveduE> & uE,
  Eigen::PlainObjectBase<DerivedEMAP> & EMAP,
  std::vector<std::vector<uE2Etype> > & uE2E,
  std::vector< sharptype > & sharp)
{
  typedef typename DerivedSE::Scalar Index;
  typedef typename DerivedV::Scalar Scalar;
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,3> MatrixX3S;
  typedef Eigen::Matrix<Scalar,1,3> RowVector3S;

  unique_edge_map(F,E,uE,EMAP,uE2E);
  MatrixX3S N;
  per_face_normals(V,F,N);
  // number of faces
  const Index m = F.rows();
  // Dihedral angles
  //std::vector<Eigen::Triplet<Scalar,int> > DIJV;
  sharp.clear();
  // Loop over each unique edge
  for(int u = 0;u<uE2E.size();u++)
  {
    bool u_is_sharp = false;
    // Consider every pair of incident faces
    //
    // if there are 3 faces (non-manifold) it appears to follow that the edge
    // must be sharp if angle<60. Could skip those (they're likely small number
    // anyway).
    for(int i = 0;i<uE2E[u].size();i++)
    for(int j = i+1;j<uE2E[u].size();j++)
    {
      const int ei = uE2E[u][i];
      const int fi = ei%m;
      const int ej = uE2E[u][j];
      const int fj = ej%m;
      const RowVector3S ni = N.row(fi);
      const RowVector3S nj = N.row(fj);
      // Edge vector
      // normalization might not be necessary
      const RowVector3S ev = (V.row(E(ei,1)) - V.row(E(ei,0))).normalized();
      const Scalar dij = 
        igl::PI - atan2((ni.cross(nj)).dot(ev),ni.dot(nj));
      //DIJV.emplace_back(fi,fj,dij);
      if(std::abs(dij-igl::PI) > angle)
      {
        u_is_sharp = true;
      }
    }
    if(u_is_sharp)
    {
      sharp.push_back(u);
    }
  }
  SE.resize(sharp.size(),2);
  for(int i = 0;i<SE.rows();i++)
  {
    SE(i,0) = uE(sharp[i],0);
    SE(i,1) = uE(sharp[i],1);
  }
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedSE>
IGL_INLINE void igl::sharp_edges(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const typename DerivedV::Scalar angle,
  Eigen::PlainObjectBase<DerivedSE> & SE
  )
{
  typedef typename DerivedSE::Scalar Index;
  typedef Eigen::Matrix<Index,Eigen::Dynamic,2> MatrixX2I;
  typedef Eigen::Matrix<Index,Eigen::Dynamic,1> VectorXI;
  MatrixX2I E,uE;
  VectorXI EMAP;
  std::vector<std::vector<Index> > uE2E;
  std::vector<int>  sharp;
  return sharp_edges(V,F,angle,SE,E,uE,EMAP,uE2E,sharp);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::sharp_edges<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::sharp_edges<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, int, int>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, std::vector<int, std::allocator<int> >&);
#endif
