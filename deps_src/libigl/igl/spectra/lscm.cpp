#include "lscm.h"
#include "../lscm_hessian.h"
#include "../massmatrix.h"
#include "../repdiag.h"
#include "eigs.h"
template <
  typename DerivedV, 
  typename DerivedF, 
  typename DerivedV_uv>
IGL_INLINE bool igl::spectra::lscm(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedV_uv> & V_uv)
{
  using Scalar = typename DerivedV_uv::Scalar;
  Eigen::SparseMatrix<Scalar> Q;
  igl::lscm_hessian(V,F,Q);
  Eigen::SparseMatrix<Scalar> M;
  igl::massmatrix(V,F,igl::MASSMATRIX_TYPE_DEFAULT,M);
  Eigen::SparseMatrix<Scalar> M2;
  igl::repdiag(M,2,M2);

  Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>  U;
  Eigen::Matrix<Scalar, Eigen::Dynamic, 1>  S;
  if(!igl::spectra::eigs(Q,M2,3,igl::EIGS_TYPE_SM,U,S)) {
    return false;
  }

  V_uv.resize(V.rows(),2);
  V_uv<< U.col(0).head(V.rows()),U.col(0).tail(V.rows());
  return true;
}
