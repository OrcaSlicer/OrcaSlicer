// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2024 Tobias Wood tobias@spinicist.org.uk
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_CXX11_TENSOR_TENSOR_ROLL_H
#define EIGEN_CXX11_TENSOR_TENSOR_ROLL_H
// IWYU pragma: private
#include "./InternalHeaderCheck.h"

namespace Eigen {

namespace internal {
template <typename RollDimensions, typename XprType>
struct traits<TensorRollOp<RollDimensions, XprType> > : public traits<XprType> {
  typedef typename XprType::Scalar Scalar;
  typedef traits<XprType> XprTraits;
  typedef typename XprTraits::StorageKind StorageKind;
  typedef typename XprTraits::Index Index;
  typedef typename XprType::Nested Nested;
  typedef std::remove_reference_t<Nested> Nested_;
  static constexpr int NumDimensions = XprTraits::NumDimensions;
  static constexpr int Layout = XprTraits::Layout;
  typedef typename XprTraits::PointerType PointerType;
};

template <typename RollDimensions, typename XprType>
struct eval<TensorRollOp<RollDimensions, XprType>, Eigen::Dense> {
  typedef const TensorRollOp<RollDimensions, XprType>& type;
};

template <typename RollDimensions, typename XprType>
struct nested<TensorRollOp<RollDimensions, XprType>, 1, typename eval<TensorRollOp<RollDimensions, XprType> >::type> {
  typedef TensorRollOp<RollDimensions, XprType> type;
};

}  // end namespace internal

/**
 * \ingroup CXX11_Tensor_Module
 *
 * \brief Tensor roll (circular shift) elements class.
 *
 */
template <typename RollDimensions, typename XprType>
class TensorRollOp : public TensorBase<TensorRollOp<RollDimensions, XprType>, WriteAccessors> {
 public:
  typedef TensorBase<TensorRollOp<RollDimensions, XprType>, WriteAccessors> Base;
  typedef typename Eigen::internal::traits<TensorRollOp>::Scalar Scalar;
  typedef typename Eigen::NumTraits<Scalar>::Real RealScalar;
  typedef typename XprType::CoeffReturnType CoeffReturnType;
  typedef typename Eigen::internal::nested<TensorRollOp>::type Nested;
  typedef typename Eigen::internal::traits<TensorRollOp>::StorageKind StorageKind;
  typedef typename Eigen::internal::traits<TensorRollOp>::Index Index;

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE TensorRollOp(const XprType& expr, const RollDimensions& roll_dims)
      : m_xpr(expr), m_roll_dims(roll_dims) {}

  EIGEN_DEVICE_FUNC const RollDimensions& roll() const { return m_roll_dims; }

  EIGEN_DEVICE_FUNC const internal::remove_all_t<typename XprType::Nested>& expression() const { return m_xpr; }

  EIGEN_TENSOR_INHERIT_ASSIGNMENT_OPERATORS(TensorRollOp)

 protected:
  typename XprType::Nested m_xpr;
  const RollDimensions m_roll_dims;
};

// Eval as rvalue
template <typename RollDimensions, typename ArgType, typename Device>
struct TensorEvaluator<const TensorRollOp<RollDimensions, ArgType>, Device> {
  typedef TensorRollOp<RollDimensions, ArgType> XprType;
  typedef typename XprType::Index Index;
  static constexpr int NumDims = internal::array_size<RollDimensions>::value;
  typedef DSizes<Index, NumDims> Dimensions;
  typedef typename XprType::Scalar Scalar;
  typedef typename XprType::CoeffReturnType CoeffReturnType;
  typedef typename PacketType<CoeffReturnType, Device>::type PacketReturnType;
  static constexpr int PacketSize = PacketType<CoeffReturnType, Device>::size;
  typedef StorageMemory<CoeffReturnType, Device> Storage;
  typedef typename Storage::Type EvaluatorPointerType;

  static constexpr int Layout = TensorEvaluator<ArgType, Device>::Layout;
  enum {
    IsAligned = false,
    PacketAccess = TensorEvaluator<ArgType, Device>::PacketAccess,
    BlockAccess = NumDims > 0,
    PreferBlockAccess = true,
    CoordAccess = false,  // to be implemented
    RawAccess = false
  };

  typedef internal::TensorIntDivisor<Index> IndexDivisor;

  //===- Tensor block evaluation strategy (see TensorBlock.h) -------------===//
  using TensorBlockDesc = internal::TensorBlockDescriptor<NumDims, Index>;
  using TensorBlockScratch = internal::TensorBlockScratchAllocator<Device>;
  using ArgTensorBlock = typename TensorEvaluator<const ArgType, Device>::TensorBlock;
  using TensorBlock = typename internal::TensorMaterializedBlock<CoeffReturnType, NumDims, Layout, Index>;
  //===--------------------------------------------------------------------===//

  EIGEN_STRONG_INLINE TensorEvaluator(const XprType& op, const Device& device)
      : m_impl(op.expression(), device), m_rolls(op.roll()), m_device(device) {
    EIGEN_STATIC_ASSERT((NumDims > 0), Must_Have_At_Least_One_Dimension_To_Roll);

    // Compute strides
    m_dimensions = m_impl.dimensions();
    if (static_cast<int>(Layout) == static_cast<int>(ColMajor)) {
      m_strides[0] = 1;
      for (int i = 1; i < NumDims; ++i) {
        m_strides[i] = m_strides[i - 1] * m_dimensions[i - 1];
        if (m_strides[i] > 0) m_fast_strides[i] = IndexDivisor(m_strides[i]);
      }
    } else {
      m_strides[NumDims - 1] = 1;
      for (int i = NumDims - 2; i >= 0; --i) {
        m_strides[i] = m_strides[i + 1] * m_dimensions[i + 1];
        if (m_strides[i] > 0) m_fast_strides[i] = IndexDivisor(m_strides[i]);
      }
    }
  }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE const Dimensions& dimensions() const { return m_dimensions; }

  EIGEN_STRONG_INLINE bool evalSubExprsIfNeeded(EvaluatorPointerType) {
    m_impl.evalSubExprsIfNeeded(nullptr);
    return true;
  }

#ifdef EIGEN_USE_THREADS
  template <typename EvalSubExprsCallback>
  EIGEN_STRONG_INLINE void evalSubExprsIfNeededAsync(EvaluatorPointerType, EvalSubExprsCallback done) {
    m_impl.evalSubExprsIfNeededAsync(nullptr, [done](bool) { done(true); });
  }
#endif  // EIGEN_USE_THREADS

  EIGEN_STRONG_INLINE void cleanup() { m_impl.cleanup(); }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Index roll(Index const i, Index const r, Index const n) const {
    auto const tmp = (i + r) % n;
    if (tmp < 0) {
      return tmp + n;
    } else {
      return tmp;
    }
  }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE array<Index, NumDims> rollCoords(array<Index, NumDims> const& coords) const {
    array<Index, NumDims> rolledCoords;
    for (int id = 0; id < NumDims; id++) {
      eigen_assert(coords[id] < m_dimensions[id]);
      rolledCoords[id] = roll(coords[id], m_rolls[id], m_dimensions[id]);
    }
    return rolledCoords;
  }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Index rollIndex(Index index) const {
    eigen_assert(index < dimensions().TotalSize());
    Index rolledIndex = 0;
    if (static_cast<int>(Layout) == static_cast<int>(ColMajor)) {
      EIGEN_UNROLL_LOOP
      for (int i = NumDims - 1; i > 0; --i) {
        Index idx = index / m_fast_strides[i];
        index -= idx * m_strides[i];
        rolledIndex += roll(idx, m_rolls[i], m_dimensions[i]) * m_strides[i];
      }
      rolledIndex += roll(index, m_rolls[0], m_dimensions[0]);
    } else {
      EIGEN_UNROLL_LOOP
      for (int i = 0; i < NumDims - 1; ++i) {
        Index idx = index / m_fast_strides[i];
        index -= idx * m_strides[i];
        rolledIndex += roll(idx, m_rolls[i], m_dimensions[i]) * m_strides[i];
      }
      rolledIndex += roll(index, m_rolls[NumDims - 1], m_dimensions[NumDims - 1]);
    }
    return rolledIndex;
  }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE CoeffReturnType coeff(Index index) const {
    return m_impl.coeff(rollIndex(index));
  }

  template <int LoadMode>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE PacketReturnType packet(Index index) const {
    eigen_assert(index + PacketSize - 1 < dimensions().TotalSize());
    EIGEN_ALIGN_MAX std::remove_const_t<CoeffReturnType> values[PacketSize];
    EIGEN_UNROLL_LOOP
    for (int i = 0; i < PacketSize; ++i) {
      values[i] = coeff(index + i);
    }
    PacketReturnType rslt = internal::pload<PacketReturnType>(values);
    return rslt;
  }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE internal::TensorBlockResourceRequirements getResourceRequirements() const {
    const size_t target_size = m_device.lastLevelCacheSize();
    return internal::TensorBlockResourceRequirements::skewed<Scalar>(target_size).addCostPerCoeff({0, 0, 24});
  }

  struct BlockIteratorState {
    Index stride;
    Index span;
    Index size;
    Index count;
  };

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE TensorBlock block(TensorBlockDesc& desc, TensorBlockScratch& scratch,
                                                          bool /*root_of_expr_ast*/ = false) const {
    static const bool is_col_major = static_cast<int>(Layout) == static_cast<int>(ColMajor);

    // Compute spatial coordinates for the first block element.
    array<Index, NumDims> coords;
    extract_coordinates(desc.offset(), coords);
    array<Index, NumDims> initial_coords = coords;
    Index offset = 0;  // Offset in the output block buffer.

    // Initialize output block iterator state. Dimension in this array are
    // always in inner_most -> outer_most order (col major layout).
    array<BlockIteratorState, NumDims> it;
    for (int i = 0; i < NumDims; ++i) {
      const int dim = is_col_major ? i : NumDims - 1 - i;
      it[i].size = desc.dimension(dim);
      it[i].stride = i == 0 ? 1 : (it[i - 1].size * it[i - 1].stride);
      it[i].span = it[i].stride * (it[i].size - 1);
      it[i].count = 0;
    }
    eigen_assert(it[0].stride == 1);

    // Prepare storage for the materialized generator result.
    const typename TensorBlock::Storage block_storage = TensorBlock::prepareStorage(desc, scratch);
    CoeffReturnType* block_buffer = block_storage.data();

    static const int inner_dim = is_col_major ? 0 : NumDims - 1;
    const Index inner_dim_size = it[0].size;

    while (it[NumDims - 1].count < it[NumDims - 1].size) {
      Index i = 0;
      for (; i < inner_dim_size; ++i) {
        auto const rolled = rollCoords(coords);
        auto const index = is_col_major ? m_dimensions.IndexOfColMajor(rolled) : m_dimensions.IndexOfRowMajor(rolled);
        *(block_buffer + offset + i) = m_impl.coeff(index);
        coords[inner_dim]++;
      }
      coords[inner_dim] = initial_coords[inner_dim];

      if (NumDims == 1) break;  // For the 1d tensor we need to generate only one inner-most dimension.

      // Update offset.
      for (i = 1; i < NumDims; ++i) {
        if (++it[i].count < it[i].size) {
          offset += it[i].stride;
          coords[is_col_major ? i : NumDims - 1 - i]++;
          break;
        }
        if (i != NumDims - 1) it[i].count = 0;
        coords[is_col_major ? i : NumDims - 1 - i] = initial_coords[is_col_major ? i : NumDims - 1 - i];
        offset -= it[i].span;
      }
    }

    return block_storage.AsTensorMaterializedBlock();
  }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE TensorOpCost costPerCoeff(bool vectorized) const {
    double compute_cost = NumDims * (2 * TensorOpCost::AddCost<Index>() + 2 * TensorOpCost::MulCost<Index>() +
                                     TensorOpCost::DivCost<Index>());
    for (int i = 0; i < NumDims; ++i) {
      compute_cost += 2 * TensorOpCost::AddCost<Index>();
    }
    return m_impl.costPerCoeff(vectorized) + TensorOpCost(0, 0, compute_cost, false /* vectorized */, PacketSize);
  }

  EIGEN_DEVICE_FUNC typename Storage::Type data() const { return nullptr; }

 protected:
  Dimensions m_dimensions;
  array<Index, NumDims> m_strides;
  array<IndexDivisor, NumDims> m_fast_strides;
  TensorEvaluator<ArgType, Device> m_impl;
  RollDimensions m_rolls;
  const Device EIGEN_DEVICE_REF m_device;

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void extract_coordinates(Index index, array<Index, NumDims>& coords) const {
    if (static_cast<int>(Layout) == static_cast<int>(ColMajor)) {
      for (int i = NumDims - 1; i > 0; --i) {
        const Index idx = index / m_fast_strides[i];
        index -= idx * m_strides[i];
        coords[i] = idx;
      }
      coords[0] = index;
    } else {
      for (int i = 0; i < NumDims - 1; ++i) {
        const Index idx = index / m_fast_strides[i];
        index -= idx * m_strides[i];
        coords[i] = idx;
      }
      coords[NumDims - 1] = index;
    }
  }

 private:
};

// Eval as lvalue

template <typename RollDimensions, typename ArgType, typename Device>
struct TensorEvaluator<TensorRollOp<RollDimensions, ArgType>, Device>
    : public TensorEvaluator<const TensorRollOp<RollDimensions, ArgType>, Device> {
  typedef TensorEvaluator<const TensorRollOp<RollDimensions, ArgType>, Device> Base;
  typedef TensorRollOp<RollDimensions, ArgType> XprType;
  typedef typename XprType::Index Index;
  static constexpr int NumDims = internal::array_size<RollDimensions>::value;
  typedef DSizes<Index, NumDims> Dimensions;

  static constexpr int Layout = TensorEvaluator<ArgType, Device>::Layout;
  enum {
    IsAligned = false,
    PacketAccess = TensorEvaluator<ArgType, Device>::PacketAccess,
    BlockAccess = false,
    PreferBlockAccess = false,
    CoordAccess = false,
    RawAccess = false
  };
  EIGEN_STRONG_INLINE TensorEvaluator(const XprType& op, const Device& device) : Base(op, device) {}

  typedef typename XprType::Scalar Scalar;
  typedef typename XprType::CoeffReturnType CoeffReturnType;
  typedef typename PacketType<CoeffReturnType, Device>::type PacketReturnType;
  static constexpr int PacketSize = PacketType<CoeffReturnType, Device>::size;

  //===- Tensor block evaluation strategy (see TensorBlock.h) -------------===//
  typedef internal::TensorBlockNotImplemented TensorBlock;
  //===--------------------------------------------------------------------===//

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE const Dimensions& dimensions() const { return this->m_dimensions; }

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE Scalar& coeffRef(Index index) const {
    return this->m_impl.coeffRef(this->rollIndex(index));
  }

  template <int StoreMode>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void writePacket(Index index, const PacketReturnType& x) const {
    eigen_assert(index + PacketSize - 1 < dimensions().TotalSize());
    EIGEN_ALIGN_MAX CoeffReturnType values[PacketSize];
    internal::pstore<CoeffReturnType, PacketReturnType>(values, x);
    EIGEN_UNROLL_LOOP
    for (int i = 0; i < PacketSize; ++i) {
      this->coeffRef(index + i) = values[i];
    }
  }
};

}  // end namespace Eigen

#endif  // EIGEN_CXX11_TENSOR_TENSOR_ROLL_H
