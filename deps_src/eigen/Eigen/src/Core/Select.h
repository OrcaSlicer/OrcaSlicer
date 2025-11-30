// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2008-2010 Gael Guennebaud <gael.guennebaud@inria.fr>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_SELECT_H
#define EIGEN_SELECT_H

// IWYU pragma: private
#include "./InternalHeaderCheck.h"

namespace Eigen {

/** \typedef Select
 * \ingroup Core_Module
 *
 * \brief Expression of a coefficient wise version of the C++ ternary operator ?:
 *
 * \tparam ConditionMatrixType the type of the \em condition expression which must be a boolean matrix
 * \tparam ThenMatrixType the type of the \em then expression
 * \tparam ElseMatrixType the type of the \em else expression
 *
 * This type represents an expression of a coefficient wise version of the C++ ternary operator ?:.
 * It is the return type of DenseBase::select() and most of the time this is the only way it is used.
 *
 * \sa DenseBase::select(const DenseBase<ThenDerived>&, const DenseBase<ElseDerived>&) const
 */
template <typename ConditionMatrixType, typename ThenMatrixType, typename ElseMatrixType>
using Select = CwiseTernaryOp<internal::scalar_boolean_select_op<typename DenseBase<ThenMatrixType>::Scalar,
                                                                 typename DenseBase<ElseMatrixType>::Scalar,
                                                                 typename DenseBase<ConditionMatrixType>::Scalar>,
                              ThenMatrixType, ElseMatrixType, ConditionMatrixType>;

/** \returns a matrix where each coefficient (i,j) is equal to \a thenMatrix(i,j)
 * if \c *this(i,j) != Scalar(0), and \a elseMatrix(i,j) otherwise.
 *
 * Example: \include MatrixBase_select.cpp
 * Output: \verbinclude MatrixBase_select.out
 *
 * \sa typedef Select
 */
template <typename Derived>
template <typename ThenDerived, typename ElseDerived>
inline EIGEN_DEVICE_FUNC CwiseTernaryOp<
    internal::scalar_boolean_select_op<typename DenseBase<ThenDerived>::Scalar, typename DenseBase<ElseDerived>::Scalar,
                                       typename DenseBase<Derived>::Scalar>,
    ThenDerived, ElseDerived, Derived>
DenseBase<Derived>::select(const DenseBase<ThenDerived>& thenMatrix, const DenseBase<ElseDerived>& elseMatrix) const {
  return Select<Derived, ThenDerived, ElseDerived>(thenMatrix.derived(), elseMatrix.derived(), derived());
}
/** Version of DenseBase::select(const DenseBase&, const DenseBase&) with
 * the \em else expression being a scalar value.
 *
 * \sa typedef Select
 */
template <typename Derived>
template <typename ThenDerived>
inline EIGEN_DEVICE_FUNC CwiseTernaryOp<
    internal::scalar_boolean_select_op<typename DenseBase<ThenDerived>::Scalar, typename DenseBase<ThenDerived>::Scalar,
                                       typename DenseBase<Derived>::Scalar>,
    ThenDerived, typename DenseBase<ThenDerived>::ConstantReturnType, Derived>
DenseBase<Derived>::select(const DenseBase<ThenDerived>& thenMatrix,
                           const typename DenseBase<ThenDerived>::Scalar& elseScalar) const {
  using ElseConstantType = typename DenseBase<ThenDerived>::ConstantReturnType;
  return Select<Derived, ThenDerived, ElseConstantType>(thenMatrix.derived(),
                                                        ElseConstantType(rows(), cols(), elseScalar), derived());
}
/** Version of DenseBase::select(const DenseBase&, const DenseBase&) with
 * the \em then expression being a scalar value.
 *
 * \sa typedef Select
 */
template <typename Derived>
template <typename ElseDerived>
inline EIGEN_DEVICE_FUNC CwiseTernaryOp<
    internal::scalar_boolean_select_op<typename DenseBase<ElseDerived>::Scalar, typename DenseBase<ElseDerived>::Scalar,
                                       typename DenseBase<Derived>::Scalar>,
    typename DenseBase<ElseDerived>::ConstantReturnType, ElseDerived, Derived>
DenseBase<Derived>::select(const typename DenseBase<ElseDerived>::Scalar& thenScalar,
                           const DenseBase<ElseDerived>& elseMatrix) const {
  using ThenConstantType = typename DenseBase<ElseDerived>::ConstantReturnType;
  return Select<Derived, ThenConstantType, ElseDerived>(ThenConstantType(rows(), cols(), thenScalar),
                                                        elseMatrix.derived(), derived());
}

}  // end namespace Eigen

#endif  // EIGEN_SELECT_H
