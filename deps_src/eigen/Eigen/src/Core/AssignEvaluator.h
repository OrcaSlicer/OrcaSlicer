// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2011 Benoit Jacob <jacob.benoit.1@gmail.com>
// Copyright (C) 2011-2014 Gael Guennebaud <gael.guennebaud@inria.fr>
// Copyright (C) 2011-2012 Jitse Niesen <jitse@maths.leeds.ac.uk>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_ASSIGN_EVALUATOR_H
#define EIGEN_ASSIGN_EVALUATOR_H

// IWYU pragma: private
#include "./InternalHeaderCheck.h"

namespace Eigen {

// This implementation is based on Assign.h

namespace internal {

/***************************************************************************
 * Part 1 : the logic deciding a strategy for traversal and unrolling       *
 ***************************************************************************/

// copy_using_evaluator_traits is based on assign_traits

template <typename DstEvaluator, typename SrcEvaluator, typename AssignFunc, int MaxPacketSize = Dynamic>
struct copy_using_evaluator_traits {
  using Src = typename SrcEvaluator::XprType;
  using Dst = typename DstEvaluator::XprType;
  using DstScalar = typename Dst::Scalar;

  static constexpr int DstFlags = DstEvaluator::Flags;
  static constexpr int SrcFlags = SrcEvaluator::Flags;

 public:
  static constexpr int DstAlignment = DstEvaluator::Alignment;
  static constexpr int SrcAlignment = SrcEvaluator::Alignment;
  static constexpr int JointAlignment = plain_enum_min(DstAlignment, SrcAlignment);
  static constexpr bool DstHasDirectAccess = bool(DstFlags & DirectAccessBit);
  static constexpr bool SrcIsRowMajor = bool(SrcFlags & RowMajorBit);
  static constexpr bool DstIsRowMajor = bool(DstFlags & RowMajorBit);
  static constexpr bool IsVectorAtCompileTime = Dst::IsVectorAtCompileTime;
  static constexpr int RowsAtCompileTime = size_prefer_fixed(Src::RowsAtCompileTime, Dst::RowsAtCompileTime);
  static constexpr int ColsAtCompileTime = size_prefer_fixed(Src::ColsAtCompileTime, Dst::ColsAtCompileTime);
  static constexpr int SizeAtCompileTime = size_at_compile_time(RowsAtCompileTime, ColsAtCompileTime);
  static constexpr int MaxRowsAtCompileTime =
      min_size_prefer_fixed(Src::MaxRowsAtCompileTime, Dst::MaxRowsAtCompileTime);
  static constexpr int MaxColsAtCompileTime =
      min_size_prefer_fixed(Src::MaxColsAtCompileTime, Dst::MaxColsAtCompileTime);
  static constexpr int MaxSizeAtCompileTime =
      min_size_prefer_fixed(Src::MaxSizeAtCompileTime, Dst::MaxSizeAtCompileTime);
  static constexpr int InnerSizeAtCompileTime = IsVectorAtCompileTime ? SizeAtCompileTime
                                                : DstIsRowMajor       ? ColsAtCompileTime
                                                                      : RowsAtCompileTime;
  static constexpr int MaxInnerSizeAtCompileTime = IsVectorAtCompileTime ? MaxSizeAtCompileTime
                                                   : DstIsRowMajor       ? MaxColsAtCompileTime
                                                                         : MaxRowsAtCompileTime;
  static constexpr int RestrictedInnerSize = min_size_prefer_fixed(MaxInnerSizeAtCompileTime, MaxPacketSize);
  static constexpr int RestrictedLinearSize = min_size_prefer_fixed(MaxSizeAtCompileTime, MaxPacketSize);
  static constexpr int OuterStride = outer_stride_at_compile_time<Dst>::ret;

  // TODO distinguish between linear traversal and inner-traversals
  using LinearPacketType = typename find_best_packet<DstScalar, RestrictedLinearSize>::type;
  using InnerPacketType = typename find_best_packet<DstScalar, RestrictedInnerSize>::type;

  static constexpr int LinearPacketSize = unpacket_traits<LinearPacketType>::size;
  static constexpr int InnerPacketSize = unpacket_traits<InnerPacketType>::size;

 public:
  static constexpr int LinearRequiredAlignment = unpacket_traits<LinearPacketType>::alignment;
  static constexpr int InnerRequiredAlignment = unpacket_traits<InnerPacketType>::alignment;

 private:
  static constexpr bool StorageOrdersAgree = DstIsRowMajor == SrcIsRowMajor;
  static constexpr bool MightVectorize = StorageOrdersAgree && bool(DstFlags & SrcFlags & ActualPacketAccessBit) &&
                                         bool(functor_traits<AssignFunc>::PacketAccess);
  static constexpr bool MayInnerVectorize = MightVectorize && (InnerSizeAtCompileTime != Dynamic) &&
                                            (InnerSizeAtCompileTime % InnerPacketSize == 0) &&
                                            (OuterStride != Dynamic) && (OuterStride % InnerPacketSize == 0) &&
                                            (EIGEN_UNALIGNED_VECTORIZE || JointAlignment >= InnerRequiredAlignment);
  static constexpr bool MayLinearize = StorageOrdersAgree && (DstFlags & SrcFlags & LinearAccessBit);
  static constexpr bool MayLinearVectorize =
      MightVectorize && MayLinearize && DstHasDirectAccess &&
      (EIGEN_UNALIGNED_VECTORIZE || (DstAlignment >= LinearRequiredAlignment) || MaxSizeAtCompileTime == Dynamic) &&
      (MaxSizeAtCompileTime == Dynamic || MaxSizeAtCompileTime >= LinearPacketSize);
  /* If the destination isn't aligned, we have to do runtime checks and we don't unroll,
     so it's only good for large enough sizes. */
  static constexpr int InnerSizeThreshold = (EIGEN_UNALIGNED_VECTORIZE ? 1 : 3) * InnerPacketSize;
  static constexpr bool MaySliceVectorize =
      MightVectorize && DstHasDirectAccess &&
      (MaxInnerSizeAtCompileTime == Dynamic || MaxInnerSizeAtCompileTime >= InnerSizeThreshold);
  /* slice vectorization can be slow, so we only want it if the slices are big, which is
     indicated by InnerMaxSize rather than InnerSize, think of the case of a dynamic block
     in a fixed-size matrix
     However, with EIGEN_UNALIGNED_VECTORIZE and unrolling, slice vectorization is still worth it */

 public:
  static constexpr int Traversal = SizeAtCompileTime == 0 ? AllAtOnceTraversal
                                   : (MayLinearVectorize && (LinearPacketSize > InnerPacketSize))
                                       ? LinearVectorizedTraversal
                                   : MayInnerVectorize  ? InnerVectorizedTraversal
                                   : MayLinearVectorize ? LinearVectorizedTraversal
                                   : MaySliceVectorize  ? SliceVectorizedTraversal
                                   : MayLinearize       ? LinearTraversal
                                                        : DefaultTraversal;
  static constexpr bool Vectorized = Traversal == InnerVectorizedTraversal || Traversal == LinearVectorizedTraversal ||
                                     Traversal == SliceVectorizedTraversal;

  using PacketType = std::conditional_t<Traversal == LinearVectorizedTraversal, LinearPacketType, InnerPacketType>;

 private:
  static constexpr int ActualPacketSize = Vectorized ? unpacket_traits<PacketType>::size : 1;
  static constexpr int UnrollingLimit = EIGEN_UNROLLING_LIMIT * ActualPacketSize;
  static constexpr int CoeffReadCost = int(DstEvaluator::CoeffReadCost) + int(SrcEvaluator::CoeffReadCost);
  static constexpr bool MayUnrollCompletely =
      (SizeAtCompileTime != Dynamic) && (SizeAtCompileTime * CoeffReadCost <= UnrollingLimit);
  static constexpr bool MayUnrollInner =
      (InnerSizeAtCompileTime != Dynamic) && (InnerSizeAtCompileTime * CoeffReadCost <= UnrollingLimit);

 public:
  static constexpr int Unrolling =
      (Traversal == InnerVectorizedTraversal || Traversal == DefaultTraversal)
          ? (MayUnrollCompletely ? CompleteUnrolling
             : MayUnrollInner    ? InnerUnrolling
                                 : NoUnrolling)
      : Traversal == LinearVectorizedTraversal
          ? (MayUnrollCompletely && (EIGEN_UNALIGNED_VECTORIZE || (DstAlignment >= LinearRequiredAlignment))
                 ? CompleteUnrolling
                 : NoUnrolling)
      : Traversal == LinearTraversal ? (MayUnrollCompletely ? CompleteUnrolling : NoUnrolling)
#if EIGEN_UNALIGNED_VECTORIZE
      : Traversal == SliceVectorizedTraversal ? (MayUnrollInner ? InnerUnrolling : NoUnrolling)
#endif
                                              : NoUnrolling;
  static constexpr bool UsePacketSegment = has_packet_segment<PacketType>::value;

#ifdef EIGEN_DEBUG_ASSIGN
  static void debug() {
    std::cerr << "DstXpr: " << typeid(typename DstEvaluator::XprType).name() << std::endl;
    std::cerr << "SrcXpr: " << typeid(typename SrcEvaluator::XprType).name() << std::endl;
    std::cerr.setf(std::ios::hex, std::ios::basefield);
    std::cerr << "DstFlags"
              << " = " << DstFlags << " (" << demangle_flags(DstFlags) << " )" << std::endl;
    std::cerr << "SrcFlags"
              << " = " << SrcFlags << " (" << demangle_flags(SrcFlags) << " )" << std::endl;
    std::cerr.unsetf(std::ios::hex);
    EIGEN_DEBUG_VAR(DstAlignment)
    EIGEN_DEBUG_VAR(SrcAlignment)
    EIGEN_DEBUG_VAR(LinearRequiredAlignment)
    EIGEN_DEBUG_VAR(InnerRequiredAlignment)
    EIGEN_DEBUG_VAR(JointAlignment)
    EIGEN_DEBUG_VAR(InnerSizeAtCompileTime)
    EIGEN_DEBUG_VAR(MaxInnerSizeAtCompileTime)
    EIGEN_DEBUG_VAR(LinearPacketSize)
    EIGEN_DEBUG_VAR(InnerPacketSize)
    EIGEN_DEBUG_VAR(ActualPacketSize)
    EIGEN_DEBUG_VAR(StorageOrdersAgree)
    EIGEN_DEBUG_VAR(MightVectorize)
    EIGEN_DEBUG_VAR(MayLinearize)
    EIGEN_DEBUG_VAR(MayInnerVectorize)
    EIGEN_DEBUG_VAR(MayLinearVectorize)
    EIGEN_DEBUG_VAR(MaySliceVectorize)
    std::cerr << "Traversal"
              << " = " << Traversal << " (" << demangle_traversal(Traversal) << ")" << std::endl;
    EIGEN_DEBUG_VAR(SrcEvaluator::CoeffReadCost)
    EIGEN_DEBUG_VAR(DstEvaluator::CoeffReadCost)
    EIGEN_DEBUG_VAR(Dst::SizeAtCompileTime)
    EIGEN_DEBUG_VAR(UnrollingLimit)
    EIGEN_DEBUG_VAR(MayUnrollCompletely)
    EIGEN_DEBUG_VAR(MayUnrollInner)
    std::cerr << "Unrolling"
              << " = " << Unrolling << " (" << demangle_unrolling(Unrolling) << ")" << std::endl;
    std::cerr << std::endl;
  }
#endif
};

/***************************************************************************
 * Part 2 : meta-unrollers
 ***************************************************************************/

/************************
*** Default traversal ***
************************/

template <typename Kernel, int Index_, int Stop>
struct copy_using_evaluator_DefaultTraversal_CompleteUnrolling {
  static constexpr int Outer = Index_ / Kernel::AssignmentTraits::InnerSizeAtCompileTime;
  static constexpr int Inner = Index_ % Kernel::AssignmentTraits::InnerSizeAtCompileTime;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    kernel.assignCoeffByOuterInner(Outer, Inner);
    copy_using_evaluator_DefaultTraversal_CompleteUnrolling<Kernel, Index_ + 1, Stop>::run(kernel);
  }
};

template <typename Kernel, int Stop>
struct copy_using_evaluator_DefaultTraversal_CompleteUnrolling<Kernel, Stop, Stop> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel&) {}
};

template <typename Kernel, int Index_, int Stop>
struct copy_using_evaluator_DefaultTraversal_InnerUnrolling {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel, Index outer) {
    kernel.assignCoeffByOuterInner(outer, Index_);
    copy_using_evaluator_DefaultTraversal_InnerUnrolling<Kernel, Index_ + 1, Stop>::run(kernel, outer);
  }
};

template <typename Kernel, int Stop>
struct copy_using_evaluator_DefaultTraversal_InnerUnrolling<Kernel, Stop, Stop> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel&, Index) {}
};

/***********************
*** Linear traversal ***
***********************/

template <typename Kernel, int Index_, int Stop>
struct copy_using_evaluator_LinearTraversal_CompleteUnrolling {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    kernel.assignCoeff(Index_);
    copy_using_evaluator_LinearTraversal_CompleteUnrolling<Kernel, Index_ + 1, Stop>::run(kernel);
  }
};

template <typename Kernel, int Stop>
struct copy_using_evaluator_LinearTraversal_CompleteUnrolling<Kernel, Stop, Stop> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel&) {}
};

/**************************
*** Inner vectorization ***
**************************/

template <typename Kernel, int Index_, int Stop>
struct copy_using_evaluator_innervec_CompleteUnrolling {
  using PacketType = typename Kernel::PacketType;
  static constexpr int Outer = Index_ / Kernel::AssignmentTraits::InnerSizeAtCompileTime;
  static constexpr int Inner = Index_ % Kernel::AssignmentTraits::InnerSizeAtCompileTime;
  static constexpr int NextIndex = Index_ + unpacket_traits<PacketType>::size;
  static constexpr int SrcAlignment = Kernel::AssignmentTraits::SrcAlignment;
  static constexpr int DstAlignment = Kernel::AssignmentTraits::DstAlignment;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(Kernel& kernel) {
    kernel.template assignPacketByOuterInner<DstAlignment, SrcAlignment, PacketType>(Outer, Inner);
    copy_using_evaluator_innervec_CompleteUnrolling<Kernel, NextIndex, Stop>::run(kernel);
  }
};

template <typename Kernel, int Stop>
struct copy_using_evaluator_innervec_CompleteUnrolling<Kernel, Stop, Stop> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel&) {}
};

template <typename Kernel, int Index_, int Stop, int SrcAlignment, int DstAlignment>
struct copy_using_evaluator_innervec_InnerUnrolling {
  using PacketType = typename Kernel::PacketType;
  static constexpr int NextIndex = Index_ + unpacket_traits<PacketType>::size;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(Kernel& kernel, Index outer) {
    kernel.template assignPacketByOuterInner<DstAlignment, SrcAlignment, PacketType>(outer, Index_);
    copy_using_evaluator_innervec_InnerUnrolling<Kernel, NextIndex, Stop, SrcAlignment, DstAlignment>::run(kernel,
                                                                                                           outer);
  }
};

template <typename Kernel, int Stop, int SrcAlignment, int DstAlignment>
struct copy_using_evaluator_innervec_InnerUnrolling<Kernel, Stop, Stop, SrcAlignment, DstAlignment> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel&, Index) {}
};

template <typename Kernel, int Start, int Stop, int SrcAlignment, int DstAlignment, bool UsePacketSegment>
struct copy_using_evaluator_innervec_segment {
  using PacketType = typename Kernel::PacketType;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(Kernel& kernel, Index outer) {
    kernel.template assignPacketSegmentByOuterInner<DstAlignment, SrcAlignment, PacketType>(outer, Start, 0,
                                                                                            Stop - Start);
  }
};

template <typename Kernel, int Start, int Stop, int SrcAlignment, int DstAlignment>
struct copy_using_evaluator_innervec_segment<Kernel, Start, Stop, SrcAlignment, DstAlignment,
                                             /*UsePacketSegment*/ false>
    : copy_using_evaluator_DefaultTraversal_InnerUnrolling<Kernel, Start, Stop> {};

template <typename Kernel, int Stop, int SrcAlignment, int DstAlignment>
struct copy_using_evaluator_innervec_segment<Kernel, Stop, Stop, SrcAlignment, DstAlignment,
                                             /*UsePacketSegment*/ true> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel&, Index) {}
};

template <typename Kernel, int Stop, int SrcAlignment, int DstAlignment>
struct copy_using_evaluator_innervec_segment<Kernel, Stop, Stop, SrcAlignment, DstAlignment,
                                             /*UsePacketSegment*/ false> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel&, Index) {}
};

/***************************************************************************
 * Part 3 : implementation of all cases
 ***************************************************************************/

// dense_assignment_loop is based on assign_impl

template <typename Kernel, int Traversal = Kernel::AssignmentTraits::Traversal,
          int Unrolling = Kernel::AssignmentTraits::Unrolling>
struct dense_assignment_loop_impl;

template <typename Kernel, int Traversal = Kernel::AssignmentTraits::Traversal,
          int Unrolling = Kernel::AssignmentTraits::Unrolling>
struct dense_assignment_loop {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
#ifdef __cpp_lib_is_constant_evaluated
    if (internal::is_constant_evaluated())
      dense_assignment_loop_impl<Kernel, Traversal == AllAtOnceTraversal ? AllAtOnceTraversal : DefaultTraversal,
                                 NoUnrolling>::run(kernel);
    else
#endif
      dense_assignment_loop_impl<Kernel, Traversal, Unrolling>::run(kernel);
  }
};

/************************
***** Special Cases *****
************************/

// Zero-sized assignment is a no-op.
template <typename Kernel, int Unrolling>
struct dense_assignment_loop_impl<Kernel, AllAtOnceTraversal, Unrolling> {
  static constexpr int SizeAtCompileTime = Kernel::AssignmentTraits::SizeAtCompileTime;

  EIGEN_DEVICE_FUNC static void EIGEN_STRONG_INLINE constexpr run(Kernel& /*kernel*/) {
    EIGEN_STATIC_ASSERT(SizeAtCompileTime == 0, EIGEN_INTERNAL_ERROR_PLEASE_FILE_A_BUG_REPORT)
  }
};

/************************
*** Default traversal ***
************************/

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, DefaultTraversal, NoUnrolling> {
  EIGEN_DEVICE_FUNC static void EIGEN_STRONG_INLINE constexpr run(Kernel& kernel) {
    for (Index outer = 0; outer < kernel.outerSize(); ++outer) {
      for (Index inner = 0; inner < kernel.innerSize(); ++inner) {
        kernel.assignCoeffByOuterInner(outer, inner);
      }
    }
  }
};

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, DefaultTraversal, CompleteUnrolling> {
  static constexpr int SizeAtCompileTime = Kernel::AssignmentTraits::SizeAtCompileTime;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    copy_using_evaluator_DefaultTraversal_CompleteUnrolling<Kernel, 0, SizeAtCompileTime>::run(kernel);
  }
};

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, DefaultTraversal, InnerUnrolling> {
  static constexpr int InnerSizeAtCompileTime = Kernel::AssignmentTraits::InnerSizeAtCompileTime;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    const Index outerSize = kernel.outerSize();
    for (Index outer = 0; outer < outerSize; ++outer)
      copy_using_evaluator_DefaultTraversal_InnerUnrolling<Kernel, 0, InnerSizeAtCompileTime>::run(kernel, outer);
  }
};

/***************************
*** Linear vectorization ***
***************************/

// The goal of unaligned_dense_assignment_loop is simply to factorize the handling
// of the non vectorizable beginning and ending parts

template <typename PacketType, int DstAlignment, int SrcAlignment, bool UsePacketSegment, bool Skip>
struct unaligned_dense_assignment_loop {
  // if Skip == true, then do nothing
  template <typename Kernel>
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& /*kernel*/, Index /*start*/, Index /*end*/) {}
  template <typename Kernel>
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& /*kernel*/, Index /*outer*/,
                                                                  Index /*innerStart*/, Index /*innerEnd*/) {}
};

template <typename PacketType, int DstAlignment, int SrcAlignment>
struct unaligned_dense_assignment_loop<PacketType, DstAlignment, SrcAlignment, /*UsePacketSegment*/ true,
                                       /*Skip*/ false> {
  template <typename Kernel>
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel, Index start, Index end) {
    Index count = end - start;
    eigen_assert(count <= unpacket_traits<PacketType>::size);
    if (count > 0) kernel.template assignPacketSegment<DstAlignment, SrcAlignment, PacketType>(start, 0, count);
  }
  template <typename Kernel>
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel, Index outer, Index start, Index end) {
    Index count = end - start;
    eigen_assert(count <= unpacket_traits<PacketType>::size);
    if (count > 0)
      kernel.template assignPacketSegmentByOuterInner<DstAlignment, SrcAlignment, PacketType>(outer, start, 0, count);
  }
};

template <typename PacketType, int DstAlignment, int SrcAlignment>
struct unaligned_dense_assignment_loop<PacketType, DstAlignment, SrcAlignment, /*UsePacketSegment*/ false,
                                       /*Skip*/ false> {
  template <typename Kernel>
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel, Index start, Index end) {
    for (Index index = start; index < end; ++index) kernel.assignCoeff(index);
  }
  template <typename Kernel>
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel, Index outer, Index innerStart,
                                                                  Index innerEnd) {
    for (Index inner = innerStart; inner < innerEnd; ++inner) kernel.assignCoeffByOuterInner(outer, inner);
  }
};

template <typename Kernel, int Index_, int Stop>
struct copy_using_evaluator_linearvec_CompleteUnrolling {
  using PacketType = typename Kernel::PacketType;
  static constexpr int SrcAlignment = Kernel::AssignmentTraits::SrcAlignment;
  static constexpr int DstAlignment = Kernel::AssignmentTraits::DstAlignment;
  static constexpr int NextIndex = Index_ + unpacket_traits<PacketType>::size;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(Kernel& kernel) {
    kernel.template assignPacket<DstAlignment, SrcAlignment, PacketType>(Index_);
    copy_using_evaluator_linearvec_CompleteUnrolling<Kernel, NextIndex, Stop>::run(kernel);
  }
};

template <typename Kernel, int Stop>
struct copy_using_evaluator_linearvec_CompleteUnrolling<Kernel, Stop, Stop> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel&) {}
};

template <typename Kernel, int Index_, int Stop, bool UsePacketSegment>
struct copy_using_evaluator_linearvec_segment {
  using PacketType = typename Kernel::PacketType;
  static constexpr int SrcAlignment = Kernel::AssignmentTraits::SrcAlignment;
  static constexpr int DstAlignment = Kernel::AssignmentTraits::DstAlignment;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(Kernel& kernel) {
    kernel.template assignPacketSegment<DstAlignment, SrcAlignment, PacketType>(Index_, 0, Stop - Index_);
  }
};

template <typename Kernel, int Index_, int Stop>
struct copy_using_evaluator_linearvec_segment<Kernel, Index_, Stop, /*UsePacketSegment*/ false>
    : copy_using_evaluator_LinearTraversal_CompleteUnrolling<Kernel, Index_, Stop> {};

template <typename Kernel, int Stop>
struct copy_using_evaluator_linearvec_segment<Kernel, Stop, Stop, /*UsePacketSegment*/ true> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel&) {}
};

template <typename Kernel, int Stop>
struct copy_using_evaluator_linearvec_segment<Kernel, Stop, Stop, /*UsePacketSegment*/ false> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel&) {}
};

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, LinearVectorizedTraversal, NoUnrolling> {
  using Scalar = typename Kernel::Scalar;
  using PacketType = typename Kernel::PacketType;
  static constexpr int PacketSize = unpacket_traits<PacketType>::size;
  static constexpr int SrcAlignment = Kernel::AssignmentTraits::JointAlignment;
  static constexpr int DstAlignment = plain_enum_max(Kernel::AssignmentTraits::DstAlignment, alignof(Scalar));
  static constexpr int RequestedAlignment = unpacket_traits<PacketType>::alignment;
  static constexpr bool Alignable =
      (DstAlignment >= RequestedAlignment) || ((RequestedAlignment - DstAlignment) % sizeof(Scalar) == 0);
  static constexpr int Alignment = Alignable ? RequestedAlignment : DstAlignment;
  static constexpr bool DstIsAligned = DstAlignment >= Alignment;
  static constexpr bool UsePacketSegment = Kernel::AssignmentTraits::UsePacketSegment;

  using head_loop =
      unaligned_dense_assignment_loop<PacketType, DstAlignment, SrcAlignment, UsePacketSegment, DstIsAligned>;
  using tail_loop = unaligned_dense_assignment_loop<PacketType, Alignment, SrcAlignment, UsePacketSegment, false>;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    const Index size = kernel.size();
    const Index alignedStart = DstIsAligned ? 0 : first_aligned<Alignment>(kernel.dstDataPtr(), size);
    const Index alignedEnd = alignedStart + numext::round_down(size - alignedStart, PacketSize);

    head_loop::run(kernel, 0, alignedStart);

    for (Index index = alignedStart; index < alignedEnd; index += PacketSize)
      kernel.template assignPacket<Alignment, SrcAlignment, PacketType>(index);

    tail_loop::run(kernel, alignedEnd, size);
  }
};

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, LinearVectorizedTraversal, CompleteUnrolling> {
  using PacketType = typename Kernel::PacketType;
  static constexpr int PacketSize = unpacket_traits<PacketType>::size;
  static constexpr int Size = Kernel::AssignmentTraits::SizeAtCompileTime;
  static constexpr int AlignedSize = numext::round_down(Size, PacketSize);
  static constexpr bool UsePacketSegment = Kernel::AssignmentTraits::UsePacketSegment;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    copy_using_evaluator_linearvec_CompleteUnrolling<Kernel, 0, AlignedSize>::run(kernel);
    copy_using_evaluator_linearvec_segment<Kernel, AlignedSize, Size, UsePacketSegment>::run(kernel);
  }
};

/**************************
*** Inner vectorization ***
**************************/

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, InnerVectorizedTraversal, NoUnrolling> {
  using PacketType = typename Kernel::PacketType;
  static constexpr int PacketSize = unpacket_traits<PacketType>::size;
  static constexpr int SrcAlignment = Kernel::AssignmentTraits::JointAlignment;
  static constexpr int DstAlignment = Kernel::AssignmentTraits::DstAlignment;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    const Index innerSize = kernel.innerSize();
    const Index outerSize = kernel.outerSize();
    for (Index outer = 0; outer < outerSize; ++outer)
      for (Index inner = 0; inner < innerSize; inner += PacketSize)
        kernel.template assignPacketByOuterInner<DstAlignment, SrcAlignment, PacketType>(outer, inner);
  }
};

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, InnerVectorizedTraversal, CompleteUnrolling> {
  static constexpr int SizeAtCompileTime = Kernel::AssignmentTraits::SizeAtCompileTime;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(Kernel& kernel) {
    copy_using_evaluator_innervec_CompleteUnrolling<Kernel, 0, SizeAtCompileTime>::run(kernel);
  }
};

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, InnerVectorizedTraversal, InnerUnrolling> {
  static constexpr int InnerSize = Kernel::AssignmentTraits::InnerSizeAtCompileTime;
  static constexpr int SrcAlignment = Kernel::AssignmentTraits::SrcAlignment;
  static constexpr int DstAlignment = Kernel::AssignmentTraits::DstAlignment;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(Kernel& kernel) {
    const Index outerSize = kernel.outerSize();
    for (Index outer = 0; outer < outerSize; ++outer)
      copy_using_evaluator_innervec_InnerUnrolling<Kernel, 0, InnerSize, SrcAlignment, DstAlignment>::run(kernel,
                                                                                                          outer);
  }
};

/***********************
*** Linear traversal ***
***********************/

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, LinearTraversal, NoUnrolling> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    const Index size = kernel.size();
    for (Index i = 0; i < size; ++i) kernel.assignCoeff(i);
  }
};

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, LinearTraversal, CompleteUnrolling> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    copy_using_evaluator_LinearTraversal_CompleteUnrolling<Kernel, 0, Kernel::AssignmentTraits::SizeAtCompileTime>::run(
        kernel);
  }
};

/**************************
*** Slice vectorization ***
***************************/

template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, SliceVectorizedTraversal, NoUnrolling> {
  using Scalar = typename Kernel::Scalar;
  using PacketType = typename Kernel::PacketType;
  static constexpr int PacketSize = unpacket_traits<PacketType>::size;
  static constexpr int SrcAlignment = Kernel::AssignmentTraits::JointAlignment;
  static constexpr int DstAlignment = plain_enum_max(Kernel::AssignmentTraits::DstAlignment, alignof(Scalar));
  static constexpr int RequestedAlignment = unpacket_traits<PacketType>::alignment;
  static constexpr bool Alignable =
      (DstAlignment >= RequestedAlignment) || ((RequestedAlignment - DstAlignment) % sizeof(Scalar) == 0);
  static constexpr int Alignment = Alignable ? RequestedAlignment : DstAlignment;
  static constexpr bool DstIsAligned = DstAlignment >= Alignment;
  static constexpr bool UsePacketSegment = Kernel::AssignmentTraits::UsePacketSegment;

  using head_loop = unaligned_dense_assignment_loop<PacketType, DstAlignment, Unaligned, UsePacketSegment, !Alignable>;
  using tail_loop = unaligned_dense_assignment_loop<PacketType, Alignment, Unaligned, UsePacketSegment, false>;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    const Scalar* dst_ptr = kernel.dstDataPtr();
    const Index innerSize = kernel.innerSize();
    const Index outerSize = kernel.outerSize();
    const Index alignedStep = Alignable ? (PacketSize - kernel.outerStride() % PacketSize) % PacketSize : 0;
    Index alignedStart = ((!Alignable) || DstIsAligned) ? 0 : internal::first_aligned<Alignment>(dst_ptr, innerSize);

    for (Index outer = 0; outer < outerSize; ++outer) {
      const Index alignedEnd = alignedStart + numext::round_down(innerSize - alignedStart, PacketSize);

      head_loop::run(kernel, outer, 0, alignedStart);

      // do the vectorizable part of the assignment
      for (Index inner = alignedStart; inner < alignedEnd; inner += PacketSize)
        kernel.template assignPacketByOuterInner<Alignment, Unaligned, PacketType>(outer, inner);

      tail_loop::run(kernel, outer, alignedEnd, innerSize);

      alignedStart = numext::mini((alignedStart + alignedStep) % PacketSize, innerSize);
    }
  }
};

#if EIGEN_UNALIGNED_VECTORIZE
template <typename Kernel>
struct dense_assignment_loop_impl<Kernel, SliceVectorizedTraversal, InnerUnrolling> {
  using PacketType = typename Kernel::PacketType;
  static constexpr int PacketSize = unpacket_traits<PacketType>::size;
  static constexpr int InnerSize = Kernel::AssignmentTraits::InnerSizeAtCompileTime;
  static constexpr int VectorizableSize = numext::round_down(InnerSize, PacketSize);
  static constexpr bool UsePacketSegment = Kernel::AssignmentTraits::UsePacketSegment;

  using packet_loop = copy_using_evaluator_innervec_InnerUnrolling<Kernel, 0, VectorizableSize, Unaligned, Unaligned>;
  using packet_segment_loop = copy_using_evaluator_innervec_segment<Kernel, VectorizableSize, InnerSize, Unaligned,
                                                                    Unaligned, UsePacketSegment>;

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(Kernel& kernel) {
    for (Index outer = 0; outer < kernel.outerSize(); ++outer) {
      packet_loop::run(kernel, outer);
      packet_segment_loop::run(kernel, outer);
    }
  }
};
#endif

/***************************************************************************
 * Part 4 : Generic dense assignment kernel
 ***************************************************************************/

// This class generalize the assignment of a coefficient (or packet) from one dense evaluator
// to another dense writable evaluator.
// It is parametrized by the two evaluators, and the actual assignment functor.
// This abstraction level permits to keep the evaluation loops as simple and as generic as possible.
// One can customize the assignment using this generic dense_assignment_kernel with different
// functors, or by completely overloading it, by-passing a functor.
template <typename DstEvaluatorTypeT, typename SrcEvaluatorTypeT, typename Functor, int Version = Specialized>
class generic_dense_assignment_kernel {
 protected:
  typedef typename DstEvaluatorTypeT::XprType DstXprType;
  typedef typename SrcEvaluatorTypeT::XprType SrcXprType;

 public:
  typedef DstEvaluatorTypeT DstEvaluatorType;
  typedef SrcEvaluatorTypeT SrcEvaluatorType;
  typedef typename DstEvaluatorType::Scalar Scalar;
  typedef copy_using_evaluator_traits<DstEvaluatorTypeT, SrcEvaluatorTypeT, Functor> AssignmentTraits;
  typedef typename AssignmentTraits::PacketType PacketType;

  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr generic_dense_assignment_kernel(DstEvaluatorType& dst,
                                                                                  const SrcEvaluatorType& src,
                                                                                  const Functor& func,
                                                                                  DstXprType& dstExpr)
      : m_dst(dst), m_src(src), m_functor(func), m_dstExpr(dstExpr) {
#ifdef EIGEN_DEBUG_ASSIGN
    AssignmentTraits::debug();
#endif
  }

  EIGEN_DEVICE_FUNC constexpr Index size() const noexcept { return m_dstExpr.size(); }
  EIGEN_DEVICE_FUNC constexpr Index innerSize() const noexcept { return m_dstExpr.innerSize(); }
  EIGEN_DEVICE_FUNC constexpr Index outerSize() const noexcept { return m_dstExpr.outerSize(); }
  EIGEN_DEVICE_FUNC constexpr Index rows() const noexcept { return m_dstExpr.rows(); }
  EIGEN_DEVICE_FUNC constexpr Index cols() const noexcept { return m_dstExpr.cols(); }
  EIGEN_DEVICE_FUNC constexpr Index outerStride() const noexcept { return m_dstExpr.outerStride(); }

  EIGEN_DEVICE_FUNC DstEvaluatorType& dstEvaluator() noexcept { return m_dst; }
  EIGEN_DEVICE_FUNC const SrcEvaluatorType& srcEvaluator() const noexcept { return m_src; }

  /// Assign src(row,col) to dst(row,col) through the assignment functor.
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void assignCoeff(Index row, Index col) {
    m_functor.assignCoeff(m_dst.coeffRef(row, col), m_src.coeff(row, col));
  }

  /// \sa assignCoeff(Index,Index)
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void assignCoeff(Index index) {
    m_functor.assignCoeff(m_dst.coeffRef(index), m_src.coeff(index));
  }

  /// \sa assignCoeff(Index,Index)
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void assignCoeffByOuterInner(Index outer, Index inner) {
    Index row = rowIndexByOuterInner(outer, inner);
    Index col = colIndexByOuterInner(outer, inner);
    assignCoeff(row, col);
  }

  template <int StoreMode, int LoadMode, typename Packet>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void assignPacket(Index row, Index col) {
    m_functor.template assignPacket<StoreMode>(&m_dst.coeffRef(row, col),
                                               m_src.template packet<LoadMode, Packet>(row, col));
  }

  template <int StoreMode, int LoadMode, typename Packet>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void assignPacket(Index index) {
    m_functor.template assignPacket<StoreMode>(&m_dst.coeffRef(index), m_src.template packet<LoadMode, Packet>(index));
  }

  template <int StoreMode, int LoadMode, typename Packet>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void assignPacketByOuterInner(Index outer, Index inner) {
    Index row = rowIndexByOuterInner(outer, inner);
    Index col = colIndexByOuterInner(outer, inner);
    assignPacket<StoreMode, LoadMode, Packet>(row, col);
  }

  template <int StoreMode, int LoadMode, typename Packet>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void assignPacketSegment(Index row, Index col, Index begin, Index count) {
    m_functor.template assignPacketSegment<StoreMode>(
        &m_dst.coeffRef(row, col), m_src.template packetSegment<LoadMode, Packet>(row, col, begin, count), begin,
        count);
  }

  template <int StoreMode, int LoadMode, typename Packet>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void assignPacketSegment(Index index, Index begin, Index count) {
    m_functor.template assignPacketSegment<StoreMode>(
        &m_dst.coeffRef(index), m_src.template packetSegment<LoadMode, Packet>(index, begin, count), begin, count);
  }

  template <int StoreMode, int LoadMode, typename Packet>
  EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void assignPacketSegmentByOuterInner(Index outer, Index inner, Index begin,
                                                                             Index count) {
    Index row = rowIndexByOuterInner(outer, inner);
    Index col = colIndexByOuterInner(outer, inner);
    assignPacketSegment<StoreMode, LoadMode, Packet>(row, col, begin, count);
  }

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr Index rowIndexByOuterInner(Index outer, Index inner) {
    typedef typename DstEvaluatorType::ExpressionTraits Traits;
    return int(Traits::RowsAtCompileTime) == 1          ? 0
           : int(Traits::ColsAtCompileTime) == 1        ? inner
           : int(DstEvaluatorType::Flags) & RowMajorBit ? outer
                                                        : inner;
  }

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr Index colIndexByOuterInner(Index outer, Index inner) {
    typedef typename DstEvaluatorType::ExpressionTraits Traits;
    return int(Traits::ColsAtCompileTime) == 1          ? 0
           : int(Traits::RowsAtCompileTime) == 1        ? inner
           : int(DstEvaluatorType::Flags) & RowMajorBit ? inner
                                                        : outer;
  }

  EIGEN_DEVICE_FUNC const Scalar* dstDataPtr() const { return m_dstExpr.data(); }

 protected:
  DstEvaluatorType& m_dst;
  const SrcEvaluatorType& m_src;
  const Functor& m_functor;
  // TODO find a way to avoid the needs of the original expression
  DstXprType& m_dstExpr;
};

// Special kernel used when computing small products whose operands have dynamic dimensions.  It ensures that the
// PacketSize used is no larger than 4, thereby increasing the chance that vectorized instructions will be used
// when computing the product.

template <typename DstEvaluatorTypeT, typename SrcEvaluatorTypeT, typename Functor>
class restricted_packet_dense_assignment_kernel
    : public generic_dense_assignment_kernel<DstEvaluatorTypeT, SrcEvaluatorTypeT, Functor, BuiltIn> {
 protected:
  typedef generic_dense_assignment_kernel<DstEvaluatorTypeT, SrcEvaluatorTypeT, Functor, BuiltIn> Base;

 public:
  typedef typename Base::Scalar Scalar;
  typedef typename Base::DstXprType DstXprType;
  typedef copy_using_evaluator_traits<DstEvaluatorTypeT, SrcEvaluatorTypeT, Functor, 4> AssignmentTraits;
  typedef typename AssignmentTraits::PacketType PacketType;

  EIGEN_DEVICE_FUNC restricted_packet_dense_assignment_kernel(DstEvaluatorTypeT& dst, const SrcEvaluatorTypeT& src,
                                                              const Functor& func, DstXprType& dstExpr)
      : Base(dst, src, func, dstExpr) {}
};

/***************************************************************************
 * Part 5 : Entry point for dense rectangular assignment
 ***************************************************************************/

template <typename DstXprType, typename SrcXprType, typename Functor>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void resize_if_allowed(DstXprType& dst, const SrcXprType& src,
                                                                       const Functor& /*func*/) {
  EIGEN_ONLY_USED_FOR_DEBUG(dst);
  EIGEN_ONLY_USED_FOR_DEBUG(src);
  eigen_assert(dst.rows() == src.rows() && dst.cols() == src.cols());
}

template <typename DstXprType, typename SrcXprType, typename T1, typename T2>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void resize_if_allowed(DstXprType& dst, const SrcXprType& src,
                                                                       const internal::assign_op<T1, T2>& /*func*/) {
  Index dstRows = src.rows();
  Index dstCols = src.cols();
  if (((dst.rows() != dstRows) || (dst.cols() != dstCols))) dst.resize(dstRows, dstCols);
  eigen_assert(dst.rows() == dstRows && dst.cols() == dstCols);
}

template <typename DstXprType, typename SrcXprType, typename Functor>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void call_dense_assignment_loop(DstXprType& dst, const SrcXprType& src,
                                                                                const Functor& func) {
  typedef evaluator<DstXprType> DstEvaluatorType;
  typedef evaluator<SrcXprType> SrcEvaluatorType;

  SrcEvaluatorType srcEvaluator(src);

  // NOTE To properly handle A = (A*A.transpose())/s with A rectangular,
  // we need to resize the destination after the source evaluator has been created.
  resize_if_allowed(dst, src, func);

  DstEvaluatorType dstEvaluator(dst);

  typedef generic_dense_assignment_kernel<DstEvaluatorType, SrcEvaluatorType, Functor> Kernel;
  Kernel kernel(dstEvaluator, srcEvaluator, func, dst.const_cast_derived());

  dense_assignment_loop<Kernel>::run(kernel);
}

template <typename DstXprType, typename SrcXprType>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void call_dense_assignment_loop(DstXprType& dst, const SrcXprType& src) {
  call_dense_assignment_loop(dst, src, internal::assign_op<typename DstXprType::Scalar, typename SrcXprType::Scalar>());
}

/***************************************************************************
 * Part 6 : Generic assignment
 ***************************************************************************/

// Based on the respective shapes of the destination and source,
// the class AssignmentKind determine the kind of assignment mechanism.
// AssignmentKind must define a Kind typedef.
template <typename DstShape, typename SrcShape>
struct AssignmentKind;

// Assignment kind defined in this file:
struct Dense2Dense {};
struct EigenBase2EigenBase {};

template <typename, typename>
struct AssignmentKind {
  typedef EigenBase2EigenBase Kind;
};
template <>
struct AssignmentKind<DenseShape, DenseShape> {
  typedef Dense2Dense Kind;
};

// This is the main assignment class
template <typename DstXprType, typename SrcXprType, typename Functor,
          typename Kind = typename AssignmentKind<typename evaluator_traits<DstXprType>::Shape,
                                                  typename evaluator_traits<SrcXprType>::Shape>::Kind,
          typename EnableIf = void>
struct Assignment;

// The only purpose of this call_assignment() function is to deal with noalias() / "assume-aliasing" and automatic
// transposition. Indeed, I (Gael) think that this concept of "assume-aliasing" was a mistake, and it makes thing quite
// complicated. So this intermediate function removes everything related to "assume-aliasing" such that Assignment does
// not has to bother about these annoying details.

template <typename Dst, typename Src>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void call_assignment(Dst& dst, const Src& src) {
  call_assignment(dst, src, internal::assign_op<typename Dst::Scalar, typename Src::Scalar>());
}
template <typename Dst, typename Src>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void call_assignment(const Dst& dst, const Src& src) {
  call_assignment(dst, src, internal::assign_op<typename Dst::Scalar, typename Src::Scalar>());
}

// Deal with "assume-aliasing"
template <typename Dst, typename Src, typename Func>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void call_assignment(
    Dst& dst, const Src& src, const Func& func, std::enable_if_t<evaluator_assume_aliasing<Src>::value, void*> = 0) {
  typename plain_matrix_type<Src>::type tmp(src);
  call_assignment_no_alias(dst, tmp, func);
}

template <typename Dst, typename Src, typename Func>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void call_assignment(
    Dst& dst, const Src& src, const Func& func, std::enable_if_t<!evaluator_assume_aliasing<Src>::value, void*> = 0) {
  call_assignment_no_alias(dst, src, func);
}

// by-pass "assume-aliasing"
// When there is no aliasing, we require that 'dst' has been properly resized
template <typename Dst, template <typename> class StorageBase, typename Src, typename Func>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void call_assignment(NoAlias<Dst, StorageBase>& dst, const Src& src,
                                                                     const Func& func) {
  call_assignment_no_alias(dst.expression(), src, func);
}

template <typename Dst, typename Src, typename Func>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void call_assignment_no_alias(Dst& dst, const Src& src,
                                                                              const Func& func) {
  enum {
    NeedToTranspose = ((int(Dst::RowsAtCompileTime) == 1 && int(Src::ColsAtCompileTime) == 1) ||
                       (int(Dst::ColsAtCompileTime) == 1 && int(Src::RowsAtCompileTime) == 1)) &&
                      int(Dst::SizeAtCompileTime) != 1
  };

  typedef std::conditional_t<NeedToTranspose, Transpose<Dst>, Dst> ActualDstTypeCleaned;
  typedef std::conditional_t<NeedToTranspose, Transpose<Dst>, Dst&> ActualDstType;
  ActualDstType actualDst(dst);

  // TODO check whether this is the right place to perform these checks:
  EIGEN_STATIC_ASSERT_LVALUE(Dst)
  EIGEN_STATIC_ASSERT_SAME_MATRIX_SIZE(ActualDstTypeCleaned, Src)
  EIGEN_CHECK_BINARY_COMPATIBILIY(Func, typename ActualDstTypeCleaned::Scalar, typename Src::Scalar);

  Assignment<ActualDstTypeCleaned, Src, Func>::run(actualDst, src, func);
}

template <typename Dst, typename Src, typename Func>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE void call_restricted_packet_assignment_no_alias(Dst& dst, const Src& src,
                                                                                      const Func& func) {
  typedef evaluator<Dst> DstEvaluatorType;
  typedef evaluator<Src> SrcEvaluatorType;
  typedef restricted_packet_dense_assignment_kernel<DstEvaluatorType, SrcEvaluatorType, Func> Kernel;

  EIGEN_STATIC_ASSERT_LVALUE(Dst)
  EIGEN_CHECK_BINARY_COMPATIBILIY(Func, typename Dst::Scalar, typename Src::Scalar);

  SrcEvaluatorType srcEvaluator(src);
  resize_if_allowed(dst, src, func);

  DstEvaluatorType dstEvaluator(dst);
  Kernel kernel(dstEvaluator, srcEvaluator, func, dst.const_cast_derived());

  dense_assignment_loop<Kernel>::run(kernel);
}

template <typename Dst, typename Src>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void call_assignment_no_alias(Dst& dst, const Src& src) {
  call_assignment_no_alias(dst, src, internal::assign_op<typename Dst::Scalar, typename Src::Scalar>());
}

template <typename Dst, typename Src, typename Func>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void call_assignment_no_alias_no_transpose(Dst& dst, const Src& src,
                                                                                           const Func& func) {
  // TODO check whether this is the right place to perform these checks:
  EIGEN_STATIC_ASSERT_LVALUE(Dst)
  EIGEN_STATIC_ASSERT_SAME_MATRIX_SIZE(Dst, Src)
  EIGEN_CHECK_BINARY_COMPATIBILIY(Func, typename Dst::Scalar, typename Src::Scalar);

  Assignment<Dst, Src, Func>::run(dst, src, func);
}
template <typename Dst, typename Src>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE constexpr void call_assignment_no_alias_no_transpose(Dst& dst, const Src& src) {
  call_assignment_no_alias_no_transpose(dst, src, internal::assign_op<typename Dst::Scalar, typename Src::Scalar>());
}

// forward declaration
template <typename Dst, typename Src>
EIGEN_DEVICE_FUNC void check_for_aliasing(const Dst& dst, const Src& src);

// Generic Dense to Dense assignment
// Note that the last template argument "Weak" is needed to make it possible to perform
// both partial specialization+SFINAE without ambiguous specialization
template <typename DstXprType, typename SrcXprType, typename Functor, typename Weak>
struct Assignment<DstXprType, SrcXprType, Functor, Dense2Dense, Weak> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE constexpr void run(DstXprType& dst, const SrcXprType& src,
                                                                  const Functor& func) {
#ifndef EIGEN_NO_DEBUG
    if (!internal::is_constant_evaluated()) {
      internal::check_for_aliasing(dst, src);
    }
#endif

    call_dense_assignment_loop(dst, src, func);
  }
};

template <typename DstXprType, typename SrcPlainObject, typename Weak>
struct Assignment<DstXprType, CwiseNullaryOp<scalar_constant_op<typename DstXprType::Scalar>, SrcPlainObject>,
                  assign_op<typename DstXprType::Scalar, typename DstXprType::Scalar>, Dense2Dense, Weak> {
  using Scalar = typename DstXprType::Scalar;
  using NullaryOp = scalar_constant_op<Scalar>;
  using SrcXprType = CwiseNullaryOp<NullaryOp, SrcPlainObject>;
  using Functor = assign_op<Scalar, Scalar>;
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(DstXprType& dst, const SrcXprType& src,
                                                        const Functor& /*func*/) {
    eigen_fill_impl<DstXprType>::run(dst, src);
  }
};

template <typename DstXprType, typename SrcPlainObject, typename Weak>
struct Assignment<DstXprType, CwiseNullaryOp<scalar_zero_op<typename DstXprType::Scalar>, SrcPlainObject>,
                  assign_op<typename DstXprType::Scalar, typename DstXprType::Scalar>, Dense2Dense, Weak> {
  using Scalar = typename DstXprType::Scalar;
  using NullaryOp = scalar_zero_op<Scalar>;
  using SrcXprType = CwiseNullaryOp<NullaryOp, SrcPlainObject>;
  using Functor = assign_op<Scalar, Scalar>;
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(DstXprType& dst, const SrcXprType& src,
                                                        const Functor& /*func*/) {
    eigen_zero_impl<DstXprType>::run(dst, src);
  }
};

// Generic assignment through evalTo.
// TODO: not sure we have to keep that one, but it helps porting current code to new evaluator mechanism.
// Note that the last template argument "Weak" is needed to make it possible to perform
// both partial specialization+SFINAE without ambiguous specialization
template <typename DstXprType, typename SrcXprType, typename Functor, typename Weak>
struct Assignment<DstXprType, SrcXprType, Functor, EigenBase2EigenBase, Weak> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(
      DstXprType& dst, const SrcXprType& src,
      const internal::assign_op<typename DstXprType::Scalar, typename SrcXprType::Scalar>& /*func*/) {
    Index dstRows = src.rows();
    Index dstCols = src.cols();
    if ((dst.rows() != dstRows) || (dst.cols() != dstCols)) dst.resize(dstRows, dstCols);

    eigen_assert(dst.rows() == src.rows() && dst.cols() == src.cols());
    src.evalTo(dst);
  }

  // NOTE The following two functions are templated to avoid their instantiation if not needed
  //      This is needed because some expressions supports evalTo only and/or have 'void' as scalar type.
  template <typename SrcScalarType>
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(
      DstXprType& dst, const SrcXprType& src,
      const internal::add_assign_op<typename DstXprType::Scalar, SrcScalarType>& /*func*/) {
    Index dstRows = src.rows();
    Index dstCols = src.cols();
    if ((dst.rows() != dstRows) || (dst.cols() != dstCols)) dst.resize(dstRows, dstCols);

    eigen_assert(dst.rows() == src.rows() && dst.cols() == src.cols());
    src.addTo(dst);
  }

  template <typename SrcScalarType>
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE void run(
      DstXprType& dst, const SrcXprType& src,
      const internal::sub_assign_op<typename DstXprType::Scalar, SrcScalarType>& /*func*/) {
    Index dstRows = src.rows();
    Index dstCols = src.cols();
    if ((dst.rows() != dstRows) || (dst.cols() != dstCols)) dst.resize(dstRows, dstCols);

    eigen_assert(dst.rows() == src.rows() && dst.cols() == src.cols());
    src.subTo(dst);
  }
};

}  // namespace internal

}  // end namespace Eigen

#endif  // EIGEN_ASSIGN_EVALUATOR_H
