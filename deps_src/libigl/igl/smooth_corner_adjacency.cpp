#include "smooth_corner_adjacency.h"
#include "vertex_triangle_adjacency.h"
#include "matlab_format.h"
#include "parallel_for.h"
#include "unzip_corners.h"
#include <iostream>

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedCI,
  typename DerivedCC>
void igl::smooth_corner_adjacency(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const double corner_threshold_radians,
  Eigen::PlainObjectBase<DerivedCI> & CI,
  Eigen::PlainObjectBase<DerivedCC> & CC)
{
  typedef double Scalar;
  typedef Eigen::Index Index;
  Eigen::Matrix<Index,Eigen::Dynamic,1> VF,NI;
  igl::vertex_triangle_adjacency(F,V.rows(),VF,NI);
  // unit normals
  Eigen::Matrix<Scalar,Eigen::Dynamic,3,Eigen::RowMajor> FN(F.rows(),3);
  igl::parallel_for(F.rows(),[&](const Index f)
  {
    const Eigen::Matrix<Scalar,1,3> v10 = V.row(F(f,1))-V.row(F(f,0));
    const Eigen::Matrix<Scalar,1,3> v20 = V.row(F(f,2))-V.row(F(f,0));
    const Eigen::Matrix<Scalar,1,3> n = v10.cross(v20);
    const Scalar a = n.norm();
    FN.row(f) = n/a;
  },10000);

  // number of faces
  const Index m = F.rows();
  // valence of faces
  const Index n = F.cols();
  assert(n == 3);

  CI.resize(m*n*8);
  CI.setConstant(-1);
  Index ncc = 0;
  Index ci = -1;
  // assumes that ci is strictly increasing and we're appending to CI
  const auto append_CI = [&](Index nf)
  {
    // make room
    if(ncc >= CI.size()) { CI.conservativeResize(CI.size()*2+1); }
    CI(ncc++) = nf;
    CC(ci+1)++;
  };
  CC.resize(m*3+1);
  CC.setConstant(-1);
  CC(0) = 0;

  const Scalar cos_thresh = cos(corner_threshold_radians);
  // parallelizing this probably requires map-reduce
  for(Index i = 0;i<m;i++)
  {
    // Normal of this face
    const auto & fnhat = FN.row(i);
    // loop over corners
    for(Index j = 0;j<n;j++)
    {
      // increment ci
      ci++;
      assert(ci == i*n+j);
      CC(ci+1) = CC(ci);
      const auto & v = F(i,j);
      for(int k = NI(v); k<NI(v+1); k++)
      {
        const int nf = VF(k);
        const auto & ifn = FN.row(nf);
        // dot product between face's normal and other face's normal
        const Scalar dp = fnhat.dot(ifn);
        // if difference in normal is slight then add to average
        if(dp > cos_thresh)
        {
          append_CI(nf);
        }
      }
    }
  }
  CI.conservativeResize(ncc);
}


template <
  typename DerivedFV,
  typename DerivedFN,
  typename DerivedCI,
  typename DerivedCC>
void igl::smooth_corner_adjacency(
  const Eigen::MatrixBase<DerivedFV> & FV,
  const Eigen::MatrixBase<DerivedFN> & FN,
  Eigen::PlainObjectBase<DerivedCI> & CI,
  Eigen::PlainObjectBase<DerivedCC> & CC)
{
  typedef Eigen::Index Index;
  assert(FV.rows() == FN.rows());
  assert(FV.cols() == 3);
  assert(FN.cols() == 3);
  Eigen::VectorXi J;
  Index nu = -1;
  {
    Eigen::MatrixXi U;
    Eigen::MatrixXi _;
    igl::unzip_corners<const Eigen::MatrixXi>({FV,FN},U,_,J);
    nu = U.rows();
    assert(J.maxCoeff() == nu-1);
  }
  // could use linear arrays here if every becomes bottleneck
  std::vector<std::vector<Index>> U2F(nu);
  const Index m = FV.rows();
  for(Index j = 0;j<3;j++)
  {
    for(Index i = 0;i<m;i++)
    {
      // unzip_corners uses convention i+j*#F
      const Index ci = i+j*m;
      U2F[J(ci)].emplace_back(i);
    }
  }
  CC.resize(m*3+1);
  CC.setConstant(-1);
  CC(0) = 0;
  CI.resize(m*3*8);
  int ncc = 0;
  // assumes that ci is strictly increasing and we're appending to CI
  const auto append_CI = [&CI,&CC,&ncc](Index ci, Index nf)
  {
    // make room
    if(ncc >= CI.size()) { CI.conservativeResize(CI.size()*2+1); }
    CI(ncc++) = nf;
    CC(ci+1)++;
  };
  for(Index i = 0;i<m;i++)
  {
    for(Index j = 0;j<3;j++)
    {
      const Index J_ci = i+j*m;
      // CI,CC uses convention i*3 + j
      const Index C_ci = i*3+j;
      CC(C_ci+1) = CC(C_ci);
      for(const auto & nf : U2F[J(J_ci)])
      {
        append_CI(C_ci,nf);
      }
    }
  }
  CI.conservativeResize(ncc);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiations
#endif
