#include "min.h"
#include "for_each.h"
#include "find_zero.h"

template <typename AType, typename DerivedB, typename DerivedI>
IGL_INLINE void igl::min(
  const Eigen::SparseMatrix<AType> & A,
  const int dim,
  Eigen::PlainObjectBase<DerivedB> & B,
  Eigen::PlainObjectBase<DerivedI> & I)
{
  const int n = A.cols();
  const int m = A.rows();
  B.resize(dim==1?n:m);
  B.setConstant(std::numeric_limits<typename DerivedB::Scalar>::max());
  I.resize(dim==1?n:m);
  for_each(A,[&B,&I,&dim](int i, int j,const typename DerivedB::Scalar v)
    {
      if(dim == 2)
      {
        std::swap(i,j);
      }
      // Coded as if dim == 1, assuming swap for dim == 2
      if(v < B(j))
      {
        B(j) = v;
        I(j) = i;
      }
    });
  Eigen::VectorXi Z;
  find_zero(A,dim,Z);
  for(int j = 0;j<I.size();j++)
  {
    if(Z(j) != (dim==1?m:n) && 0 < B(j))
    {
      B(j) = 0;
      I(j) = Z(j);
    }
  }
}

