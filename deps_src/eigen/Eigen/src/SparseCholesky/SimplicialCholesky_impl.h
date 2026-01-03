// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2008-2012 Gael Guennebaud <gael.guennebaud@inria.fr>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

/*
NOTE: these functions have been adapted from the LDL library:

LDL Copyright (c) 2005 by Timothy A. Davis.  All Rights Reserved.

The author of LDL, Timothy A. Davis., has executed a license with Google LLC
to permit distribution of this code and derivative works as part of Eigen under
the Mozilla Public License v. 2.0, as stated at the top of this file.
 */

#ifndef EIGEN_SIMPLICIAL_CHOLESKY_IMPL_H
#define EIGEN_SIMPLICIAL_CHOLESKY_IMPL_H

// IWYU pragma: private
#include "./InternalHeaderCheck.h"

namespace Eigen {

namespace internal {

template <typename Scalar, typename StorageIndex>
struct simpl_chol_helper {
  using CholMatrixType = SparseMatrix<Scalar, ColMajor, StorageIndex>;
  using InnerIterator = typename CholMatrixType::InnerIterator;
  using VectorI = Matrix<StorageIndex, Dynamic, 1>;
  static constexpr StorageIndex kEmpty = -1;

  // Implementation of a stack or last-in first-out structure with some debugging machinery.
  struct Stack {
    StorageIndex* m_data;
    Index m_size;
#ifndef EIGEN_NO_DEBUG
    const Index m_maxSize;
    Stack(StorageIndex* data, StorageIndex size, StorageIndex maxSize)
        : m_data(data), m_size(size), m_maxSize(maxSize) {
      eigen_assert(size >= 0);
      eigen_assert(maxSize >= size);
    }
#else
    Stack(StorageIndex* data, StorageIndex size, StorageIndex /*maxSize*/) : m_data(data), m_size(size) {}
#endif
    bool empty() const { return m_size == 0; }
    Index size() const { return m_size; }
    StorageIndex back() const {
      eigen_assert(m_size > 0);
      return m_data[m_size - 1];
    }
    void push(const StorageIndex& value) {
#ifndef EIGEN_NO_DEBUG
      eigen_assert(m_size < m_maxSize);
#endif
      m_data[m_size] = value;
      m_size++;
    }
    void pop() {
      eigen_assert(m_size > 0);
      m_size--;
    }
  };

  // Implementation of a disjoint-set or union-find structure with path compression.
  struct DisjointSet {
    StorageIndex* m_set;
    DisjointSet(StorageIndex* set, StorageIndex size) : m_set(set) { std::iota(set, set + size, 0); }
    // Find the set representative or root of `u`.
    StorageIndex find(StorageIndex u) const {
      eigen_assert(u != kEmpty);
      while (m_set[u] != u) {
        // manually unroll the loop by a factor of 2 to improve performance
        u = m_set[m_set[u]];
      }
      return u;
    }
    // Perform full path compression such that each node from `u` to `v` points to `v`.
    void compress(StorageIndex u, StorageIndex v) {
      eigen_assert(u != kEmpty);
      eigen_assert(v != kEmpty);
      while (m_set[u] != v) {
        StorageIndex next = m_set[u];
        m_set[u] = v;
        u = next;
      }
    };
  };

  // Computes the higher adjacency pattern by transposing the input lower adjacency matrix.
  // Only the index arrays are calculated, as the values are not needed for the symbolic factorization.
  // The outer index array provides the size requirements of the inner index array.

  // Computes the outer index array of the higher adjacency matrix.
  static void calc_hadj_outer(const StorageIndex size, const CholMatrixType& ap, StorageIndex* outerIndex) {
    for (StorageIndex j = 1; j < size; j++) {
      for (InnerIterator it(ap, j); it; ++it) {
        StorageIndex i = it.index();
        if (i < j) outerIndex[i + 1]++;
      }
    }
    std::partial_sum(outerIndex, outerIndex + size + 1, outerIndex);
  }

  // inner index array
  static void calc_hadj_inner(const StorageIndex size, const CholMatrixType& ap, const StorageIndex* outerIndex,
                              StorageIndex* innerIndex, StorageIndex* tmp) {
    std::fill_n(tmp, size, 0);

    for (StorageIndex j = 1; j < size; j++) {
      for (InnerIterator it(ap, j); it; ++it) {
        StorageIndex i = it.index();
        if (i < j) {
          StorageIndex b = outerIndex[i] + tmp[i];
          innerIndex[b] = j;
          tmp[i]++;
        }
      }
    }
  }

  // Adapted from:
  // Joseph W. Liu. (1986).
  // A compact row storage scheme for Cholesky factors using elimination trees.
  // ACM Trans. Math. Softw. 12, 2 (June 1986), 127-148. https://doi.org/10.1145/6497.6499

  // Computes the elimination forest of the lower adjacency matrix, a compact representation of the sparse L factor.
  // The L factor may contain multiple elimination trees if a column contains only its diagonal element.
  // Each elimination tree is an n-ary tree in which each node points to its parent.
  static void calc_etree(const StorageIndex size, const CholMatrixType& ap, StorageIndex* parent, StorageIndex* tmp) {
    std::fill_n(parent, size, kEmpty);

    DisjointSet ancestor(tmp, size);

    for (StorageIndex j = 1; j < size; j++) {
      for (InnerIterator it(ap, j); it; ++it) {
        StorageIndex i = it.index();
        if (i < j) {
          StorageIndex r = ancestor.find(i);
          if (r != j) parent[r] = j;
          ancestor.compress(i, j);
        }
      }
    }
  }

  // Computes the child pointers of the parent tree to facilitate a depth-first search traversal.
  static void calc_lineage(const StorageIndex size, const StorageIndex* parent, StorageIndex* firstChild,
                           StorageIndex* firstSibling) {
    std::fill_n(firstChild, size, kEmpty);
    std::fill_n(firstSibling, size, kEmpty);

    for (StorageIndex j = 0; j < size; j++) {
      StorageIndex p = parent[j];
      if (p == kEmpty) continue;
      StorageIndex c = firstChild[p];
      if (c == kEmpty)
        firstChild[p] = j;
      else {
        while (firstSibling[c] != kEmpty) c = firstSibling[c];
        firstSibling[c] = j;
      }
    }
  }

  // Computes a post-ordered traversal of the elimination tree.
  static void calc_post(const StorageIndex size, const StorageIndex* parent, StorageIndex* firstChild,
                        const StorageIndex* firstSibling, StorageIndex* post, StorageIndex* dfs) {
    Stack post_stack(post, 0, size);
    for (StorageIndex j = 0; j < size; j++) {
      if (parent[j] != kEmpty) continue;
      // Begin at a root
      Stack dfs_stack(dfs, 0, size);
      dfs_stack.push(j);
      while (!dfs_stack.empty()) {
        StorageIndex i = dfs_stack.back();
        StorageIndex c = firstChild[i];
        if (c == kEmpty) {
          post_stack.push(i);
          dfs_stack.pop();
        } else {
          dfs_stack.push(c);
          // Remove the path from `i` to `c` for future traversals.
          firstChild[i] = firstSibling[c];
        }
      }
    }
    eigen_assert(post_stack.size() == size);
    eigen_assert(std::all_of(firstChild, firstChild + size, [](StorageIndex a) { return a == kEmpty; }));
  }

  // Adapted from:
  // Gilbert, J. R., Ng, E., & Peyton, B. W. (1994).
  // An efficient algorithm to compute row and column counts for sparse Cholesky factorization.
  // SIAM Journal on Matrix Analysis and Applications, 15(4), 1075-1091.

  // Computes the non-zero pattern of the L factor.
  static void calc_colcount(const StorageIndex size, const StorageIndex* hadjOuter, const StorageIndex* hadjInner,
                            const StorageIndex* parent, StorageIndex* prevLeaf, StorageIndex* tmp,
                            const StorageIndex* post, StorageIndex* nonZerosPerCol, bool doLDLT) {
    // initialize nonZerosPerCol with 1 for leaves, 0 for non-leaves
    std::fill_n(nonZerosPerCol, size, 1);
    for (StorageIndex j = 0; j < size; j++) {
      StorageIndex p = parent[j];
      // p is not a leaf
      if (p != kEmpty) nonZerosPerCol[p] = 0;
    }

    DisjointSet parentSet(tmp, size);
    // prevLeaf is already initialized
    eigen_assert(std::all_of(prevLeaf, prevLeaf + size, [](StorageIndex a) { return a == kEmpty; }));

    for (StorageIndex j_ = 0; j_ < size; j_++) {
      StorageIndex j = post[j_];
      nonZerosPerCol[j] += hadjOuter[j + 1] - hadjOuter[j];
      for (StorageIndex k = hadjOuter[j]; k < hadjOuter[j + 1]; k++) {
        StorageIndex i = hadjInner[k];
        eigen_assert(i > j);
        StorageIndex prev = prevLeaf[i];
        if (prev != kEmpty) {
          StorageIndex q = parentSet.find(prev);
          parentSet.compress(prev, q);
          nonZerosPerCol[q]--;
        }
        prevLeaf[i] = j;
      }
      StorageIndex p = parent[j];
      if (p != kEmpty) parentSet.compress(j, p);
    }

    for (StorageIndex j = 0; j < size; j++) {
      StorageIndex p = parent[j];
      if (p != kEmpty) nonZerosPerCol[p] += nonZerosPerCol[j] - 1;
      if (doLDLT) nonZerosPerCol[j]--;
    }
  }

  // Finalizes the non zero pattern of the L factor and allocates the memory for the factorization.
  static void init_matrix(const StorageIndex size, const StorageIndex* nonZerosPerCol, CholMatrixType& L) {
    eigen_assert(L.outerIndexPtr()[0] == 0);
    std::partial_sum(nonZerosPerCol, nonZerosPerCol + size, L.outerIndexPtr() + 1);
    L.resizeNonZeros(L.outerIndexPtr()[size]);
  }

  // Driver routine for the symbolic sparse Cholesky factorization.
  static void run(const StorageIndex size, const CholMatrixType& ap, CholMatrixType& L, VectorI& parent,
                  VectorI& workSpace, bool doLDLT) {
    parent.resize(size);
    workSpace.resize(4 * size);
    L.resize(size, size);

    StorageIndex* tmp1 = workSpace.data();
    StorageIndex* tmp2 = workSpace.data() + size;
    StorageIndex* tmp3 = workSpace.data() + 2 * size;
    StorageIndex* tmp4 = workSpace.data() + 3 * size;

    // Borrow L's outer index array for the higher adjacency pattern.
    StorageIndex* hadj_outer = L.outerIndexPtr();
    calc_hadj_outer(size, ap, hadj_outer);
    // Request additional temporary storage for the inner indices of the higher adjacency pattern.
    ei_declare_aligned_stack_constructed_variable(StorageIndex, hadj_inner, hadj_outer[size], nullptr);
    calc_hadj_inner(size, ap, hadj_outer, hadj_inner, tmp1);

    calc_etree(size, ap, parent.data(), tmp1);
    calc_lineage(size, parent.data(), tmp1, tmp2);
    calc_post(size, parent.data(), tmp1, tmp2, tmp3, tmp4);
    calc_colcount(size, hadj_outer, hadj_inner, parent.data(), tmp1, tmp2, tmp3, tmp4, doLDLT);
    init_matrix(size, tmp4, L);
  }
};

// Symbol is ODR-used, so we need a definition.
template <typename Scalar, typename StorageIndex>
constexpr StorageIndex simpl_chol_helper<Scalar, StorageIndex>::kEmpty;

}  // namespace internal

template <typename Derived>
void SimplicialCholeskyBase<Derived>::analyzePattern_preordered(const CholMatrixType& ap, bool doLDLT) {
  using Helper = internal::simpl_chol_helper<Scalar, StorageIndex>;

  eigen_assert(ap.innerSize() == ap.outerSize());
  const StorageIndex size = internal::convert_index<StorageIndex>(ap.outerSize());

  Helper::run(size, ap, m_matrix, m_parent, m_workSpace, doLDLT);

  m_isInitialized = true;
  m_info = Success;
  m_analysisIsOk = true;
  m_factorizationIsOk = false;
}

template <typename Derived>
template <bool DoLDLT, bool NonHermitian>
void SimplicialCholeskyBase<Derived>::factorize_preordered(const CholMatrixType& ap) {
  using std::sqrt;
  const StorageIndex size = StorageIndex(ap.rows());

  eigen_assert(m_analysisIsOk && "You must first call analyzePattern()");
  eigen_assert(ap.rows() == ap.cols());
  eigen_assert(m_parent.size() == size);
  eigen_assert(m_workSpace.size() >= 3 * size);

  const StorageIndex* Lp = m_matrix.outerIndexPtr();
  StorageIndex* Li = m_matrix.innerIndexPtr();
  Scalar* Lx = m_matrix.valuePtr();

  ei_declare_aligned_stack_constructed_variable(Scalar, y, size, 0);
  StorageIndex* nonZerosPerCol = m_workSpace.data();
  StorageIndex* pattern = m_workSpace.data() + size;
  StorageIndex* tags = m_workSpace.data() + 2 * size;

  bool ok = true;
  m_diag.resize(DoLDLT ? size : 0);

  for (StorageIndex k = 0; k < size; ++k) {
    // compute nonzero pattern of kth row of L, in topological order
    y[k] = Scalar(0);         // Y(0:k) is now all zero
    StorageIndex top = size;  // stack for pattern is empty
    tags[k] = k;              // mark node k as visited
    nonZerosPerCol[k] = 0;    // count of nonzeros in column k of L
    for (typename CholMatrixType::InnerIterator it(ap, k); it; ++it) {
      StorageIndex i = it.index();
      if (i <= k) {
        y[i] += getSymm(it.value()); /* scatter A(i,k) into Y (sum duplicates) */
        Index len;
        for (len = 0; tags[i] != k; i = m_parent[i]) {
          pattern[len++] = i; /* L(k,i) is nonzero */
          tags[i] = k;        /* mark i as visited */
        }
        while (len > 0) pattern[--top] = pattern[--len];
      }
    }

    /* compute numerical values kth row of L (a sparse triangular solve) */

    DiagonalScalar d =
        getDiag(y[k]) * m_shiftScale + m_shiftOffset;  // get D(k,k), apply the shift function, and clear Y(k)
    y[k] = Scalar(0);
    for (; top < size; ++top) {
      Index i = pattern[top]; /* pattern[top:n-1] is pattern of L(:,k) */
      Scalar yi = y[i];       /* get and clear Y(i) */
      y[i] = Scalar(0);

      /* the nonzero entry L(k,i) */
      Scalar l_ki;
      if (DoLDLT)
        l_ki = yi / getDiag(m_diag[i]);
      else
        yi = l_ki = yi / Lx[Lp[i]];

      Index p2 = Lp[i] + nonZerosPerCol[i];
      Index p;
      for (p = Lp[i] + (DoLDLT ? 0 : 1); p < p2; ++p) y[Li[p]] -= getSymm(Lx[p]) * yi;
      d -= getDiag(l_ki * getSymm(yi));
      Li[p] = k; /* store L(k,i) in column form of L */
      Lx[p] = l_ki;
      ++nonZerosPerCol[i]; /* increment count of nonzeros in col i */
    }
    if (DoLDLT) {
      m_diag[k] = d;
      if (d == RealScalar(0)) {
        ok = false; /* failure, D(k,k) is zero */
        break;
      }
    } else {
      Index p = Lp[k] + nonZerosPerCol[k]++;
      Li[p] = k; /* store L(k,k) = sqrt (d) in column k */
      if (NonHermitian ? d == RealScalar(0) : numext::real(d) <= RealScalar(0)) {
        ok = false; /* failure, matrix is not positive definite */
        break;
      }
      Lx[p] = sqrt(d);
    }
  }

  m_info = ok ? Success : NumericalIssue;
  m_factorizationIsOk = true;
}

}  // end namespace Eigen

#endif  // EIGEN_SIMPLICIAL_CHOLESKY_IMPL_H
