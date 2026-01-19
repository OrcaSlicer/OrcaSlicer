#ifndef IGL_FOR_EACH_H
#define IGL_FOR_EACH_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  // FOR_EACH  Call a given function for each non-zero (i.e., explicit value
  // might actually be ==0) in a Sparse Matrix A _in order (of storage)_. This is
  // useless unless func has _side-effects_.
  //
  // Inputs:
  //   A  m by n SparseMatrix
  //   func  function handle with prototype "compatible with" `void (Index i,
  //     Index j, Scalar & v)`. Return values will be ignored.
  //
  // See also: std::for_each
  template <typename AType, typename Func>
  inline void for_each(
    const Eigen::SparseMatrix<AType> & A,
    const Func & func);
  template <typename DerivedA, typename Func>
  inline void for_each(
    const Eigen::DenseBase<DerivedA> & A,
    const Func & func);
}

// Implementation

template <typename AType, typename Func>
inline void igl::for_each(
  const Eigen::SparseMatrix<AType> & A,
  const Func & func)
{
  // Can **not** use parallel for because this must be _in order_
  // Iterate over outside
  for(int k=0; k<A.outerSize(); ++k)
  {
    // Iterate over inside
    for(typename Eigen::SparseMatrix<AType>::InnerIterator it (A,k); it; ++it)
    {
      func(it.row(),it.col(),it.value());
    }
  }
}

template <typename DerivedA, typename Func>
inline void igl::for_each(
  const Eigen::DenseBase<DerivedA> & A,
  const Func & func)
{
  // Can **not** use parallel for because this must be _in order_
  if(A.IsRowMajor)
  {
    for(typename DerivedA::Index i = 0;i<A.rows();i++)
    {
      for(typename DerivedA::Index j = 0;j<A.cols();j++)
      {
        func(i,j,A(i,j));
      }
    }
  }else
  {
    for(typename DerivedA::Index j = 0;j<A.cols();j++)
    {
      for(typename DerivedA::Index i = 0;i<A.rows();i++)
      {
        func(i,j,A(i,j));
      }
    }
  }
  
}

//#ifndef IGL_STATIC_LIBRARY
//#  include "for_each.cpp"
//#endif
#endif
