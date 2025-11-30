#include "max.h"
#include "for_each.h"
#include "find_zero.h"

template <typename AType, typename DerivedB, typename DerivedI>
IGL_INLINE void igl::max(
  const Eigen::SparseMatrix<AType> & A,
  const int dim,
  Eigen::PlainObjectBase<DerivedB> & B,
  Eigen::PlainObjectBase<DerivedI> & I)
{
  const int n = A.cols();
  const int m = A.rows();
  B.resize(dim==1?n:m);
  B.setConstant(std::numeric_limits<typename DerivedB::Scalar>::lowest());
  I.resize(dim==1?n:m);
  for_each(A,[&B,&I,&dim](int i, int j,const typename DerivedB::Scalar v)
    {
      if(dim == 2)
      {
        std::swap(i,j);
      }
      // Coded as if dim == 1, assuming swap for dim == 2
      if(v > B(j))
      {
        B(j) = v;
        I(j) = i;
      }
    });
  Eigen::VectorXi Z;
  find_zero(A,dim,Z);
  for(int j = 0;j<I.size();j++)
  {
    if(Z(j) != (dim==1?m:n) && 0 > B(j))
    {
      B(j) = 0;
      I(j) = Z(j);
    }
  }
}

template <typename DerivedX, typename DerivedY, typename DerivedI>
IGL_INLINE void igl::max(
  const Eigen::DenseBase<DerivedX> & X,
  const int dim,
  Eigen::PlainObjectBase<DerivedY> & Y,
  Eigen::PlainObjectBase<DerivedI> & I)
{
  assert(dim==1||dim==2);

  // output size
  int n = (dim==1?X.cols():X.rows());
  // resize output
  Y.resize(n);
  I.resize(n);

  // loop over dimension opposite of dim
  for(int j = 0;j<n;j++)
  {
    typename DerivedX::Index PHONY,i;
    typename DerivedX::Scalar  m;
    if(dim==1)
    {
      m = X.col(j).maxCoeff(&i,&PHONY);
    }else
    {
      m = X.row(j).maxCoeff(&PHONY,&i);
    }
    Y(j) = m;
    I(j) = i;
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::max<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::DenseBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::max<bool, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::SparseMatrix<bool, 0, int> const&, int, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
