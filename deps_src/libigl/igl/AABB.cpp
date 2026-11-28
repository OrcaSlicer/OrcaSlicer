// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "AABB.h"
#include "EPS.h"
#include "barycenter.h"
#include "colon.h"
#include "doublearea.h"
#include "increment_ulp.h"
#include "point_simplex_squared_distance.h"
#include "project_to_line_segment.h"
#include "sort.h"
#include "volume.h"
#include "ray_box_intersect.h"
#include "parallel_for.h"
#include "ray_mesh_intersect.h"
#include "box_surface_area.h"
#include "pad_box.h"
#include <iostream>
#include <iomanip>
#include <limits>
#include <list>
#include <queue>
#include <stack>
#include <string>
#include <stdio.h>

// This would be so much better with C++17 if constexpr
namespace
{
  template <
    typename DerivedV,
    typename DerivedEle,
    typename Derivedq,
    int DIM>
  struct AABB_all_positive_barycentric_coordinates_helper;

  template <
    typename DerivedV,
    typename DerivedEle,
    typename Derivedq>
  struct AABB_all_positive_barycentric_coordinates_helper<
  DerivedV,
  DerivedEle,
  Derivedq,
  2>
  {
    static inline bool compute(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedEle> & Ele,
      const int primitive,
      const Eigen::MatrixBase<Derivedq> & q)
    {
      using namespace igl;
      using Scalar = typename DerivedV::Scalar;
      const Scalar epsilon = igl::EPS<Scalar>();
      static_assert(
        DerivedV::ColsAtCompileTime == 2 ||
        DerivedV::ColsAtCompileTime == Eigen::Dynamic,
        "V should be 2D");
      static_assert(
        Derivedq::ColsAtCompileTime == 2 ||
        Derivedq::ColsAtCompileTime == Eigen::Dynamic,
        "q should be 2D");
      // Barycentric coordinates
      typedef Eigen::Matrix<Scalar,2,1> Vector2S;
      const Vector2S V1 = V.row(Ele(primitive,0));
      const Vector2S V2 = V.row(Ele(primitive,1));
      const Vector2S V3 = V.row(Ele(primitive,2));
      // Hack for now to keep templates simple. If becomes bottleneck
      // consider using std::enable_if_t
      const Vector2S q2 = q.head(2);
      Scalar a1 = doublearea_single(V1,V2,q2);
      Scalar a2 = doublearea_single(V2,V3,q2);
      Scalar a3 = doublearea_single(V3,V1,q2);
      // Normalization is important for correcting magnitude near epsilon
      Scalar sum = a1+a2+a3;
      a1 /= sum;
      a2 /= sum;
      a3 /= sum;
      return a1>=-epsilon && a2>=-epsilon && a3>=-epsilon;
    }
  };


  template <
    typename DerivedV,
    typename DerivedEle,
    typename Derivedq>
  struct AABB_all_positive_barycentric_coordinates_helper<
  DerivedV,
  DerivedEle,
  Derivedq,
  3>
  {
    static inline bool compute(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedEle> & Ele,
      const int primitive,
      const Eigen::MatrixBase<Derivedq> & q)
    {
      using namespace igl;
      using Scalar = typename DerivedV::Scalar;
      const Scalar epsilon = igl::EPS<Scalar>();
      static_assert(
        DerivedV::ColsAtCompileTime == 3 ||
        DerivedV::ColsAtCompileTime == Eigen::Dynamic,
        "V should be 2D");
      static_assert(
        Derivedq::ColsAtCompileTime == 3 ||
        Derivedq::ColsAtCompileTime == Eigen::Dynamic,
        "q should be 2D");
      // Barycentric coordinates
      typedef Eigen::Matrix<Scalar,1,3> RowVector3S;
      const RowVector3S V1 = V.row(Ele(primitive,0));
      const RowVector3S V2 = V.row(Ele(primitive,1));
      const RowVector3S V3 = V.row(Ele(primitive,2));
      const RowVector3S V4 = V.row(Ele(primitive,3));
      Scalar a1 = volume_single(V2,V4,V3,(RowVector3S)q);
      Scalar a2 = volume_single(V1,V3,V4,(RowVector3S)q);
      Scalar a3 = volume_single(V1,V4,V2,(RowVector3S)q);
      Scalar a4 = volume_single(V1,V2,V3,(RowVector3S)q);
      Scalar sum = a1+a2+a3+a4;
      a1 /= sum;
      a2 /= sum;
      a3 /= sum;
      a4 /= sum;
      return a1>=-epsilon && a2>=-epsilon && a3>=-epsilon && a4>=-epsilon;
    }
  };

}

///////////////////////////////////////////////////////////////////////////////
// Non-templated member functions
///////////////////////////////////////////////////////////////////////////////

template <typename DerivedV, int DIM>
IGL_INLINE bool igl::AABB<DerivedV,DIM>::is_leaf() const
{
  // This way (rather than `m_primitive != -1` we can have empty leaves
  return m_left == nullptr && m_right == nullptr;
}

template <typename DerivedV, int DIM>
IGL_INLINE bool igl::AABB<DerivedV,DIM>::is_root() const
{
  return m_parent == nullptr;
}

template <typename DerivedV, int DIM>
IGL_INLINE igl::AABB<DerivedV,DIM>* igl::AABB<DerivedV,DIM>::root() const
{
  return (is_root() ?  const_cast<igl::AABB<DerivedV,DIM>*>(this) : m_parent->root());
}

template <typename DerivedV, int DIM>
IGL_INLINE igl::AABB<DerivedV,DIM>* igl::AABB<DerivedV,DIM>::detach()
{
  if(!this->m_parent)
  {
    // Before
    //   ⊘-this
    //
    // After
    //   ⊘-this
    //
    // `this` is the root. It's already detached.
    return this;
  }

  auto * parent = this->m_parent;
  assert(parent);
  // Detach from parent
  this->m_parent = nullptr;
  auto * grandparent = parent->m_parent;
  auto * sibling = parent->m_left == this ? parent->m_right : parent->m_left;
  assert(sibling);
  // Before
  //     grandparent
  //        ╱
  //     parent
  //     ╱    ╲
  // sibling  this
  //
  // After
  //     grandparent
  //        /
  //     sibling®   ⊘     ☠️ parent☠️
  //                |
  //               this
  //
  // Grandparent is sibling's parent now: works even if parent is root
  // (grandparent is null)
  sibling->m_parent = grandparent;
  // Parent is now parentless and childless (to make clear() happy)
  parent->m_parent = nullptr;
  parent->m_left = nullptr;
  parent->m_right = nullptr;
  // Tell grandparent sibling is new child.
  if(grandparent)
  {
    (grandparent->m_left == parent ?
      grandparent->m_left : grandparent->m_right) = sibling;
  }
  // Did your code just crash here? Perhaps it's because you statically
  // allocated your tree instead of using `new`. That's not allowed if you're
  // going to call dynamic methods like `detach` and `insert`.
  //
  // This is a design flaw that requires a refactor of igl::AABB to fix.
  delete parent;

  return sibling;
}

template <typename DerivedV, int DIM>
IGL_INLINE void igl::AABB<DerivedV,DIM>::refit_lineage()
{
  const decltype(m_box) old_box = m_box;
  if(!is_leaf())
  {
    m_box.setEmpty();
    if(m_left) { m_box.extend(m_left->m_box); }
    if(m_right) { m_box.extend(m_right->m_box); }
  }
  // Q: is box.contains(old_box) ever true?
  // H: It's really supposed to be testing whether anything changed. But the
  // call patterns for refit_lineage might be only hitting cases where box is always
  // smaler than old_box.
  if(m_parent && !m_box.contains(old_box)) { m_parent->refit_lineage(); }
}

template <typename DerivedV, int DIM>
IGL_INLINE std::vector<igl::AABB<DerivedV,DIM>*>
  igl::AABB<DerivedV,DIM>::gather_leaves(const int m)
{
  auto * tree = this;
  std::vector<igl::AABB<DerivedV,DIM>*> leaves(m,nullptr);
  {
    std::vector<igl::AABB<DerivedV,DIM>* > stack;
    stack.push_back(tree);
    while(!stack.empty())
    {
      auto * node = stack.back();
      stack.pop_back();
      if(!node) { continue; }
      stack.push_back(node->m_left);
      stack.push_back(node->m_right);
      if(node->is_leaf())
      {
        assert(node->m_primitive < m && node->m_primitive >= 0);
        leaves[node->m_primitive] = node;
      }
    }
  }
  return leaves;
}

template <typename DerivedV, int DIM>
IGL_INLINE std::vector<igl::AABB<DerivedV,DIM>*>
  igl::AABB<DerivedV,DIM>::gather_leaves()
{
  int max_primitive = -1;
  {
    std::vector<igl::AABB<DerivedV,DIM>* > stack;
    stack.push_back(this);
    while(!stack.empty())
    {
      auto * node = stack.back();
      stack.pop_back();
      if(!node) { continue; }
      stack.push_back(node->m_left);
      stack.push_back(node->m_right);
      max_primitive = std::max(max_primitive,node->m_primitive);
    }
  }
  return gather_leaves(max_primitive+1);
}

template <typename DerivedV, int DIM>
IGL_INLINE igl::AABB<DerivedV,DIM>* igl::AABB<DerivedV,DIM>::pad(
  const std::vector<igl::AABB<DerivedV,DIM>*> & leaves,
  const igl::AABB<DerivedV,DIM>::Scalar pad,
  const int polish_rotate_passes)
{
  // Will get reset to root below anyway. This does _not_ operate on subtrees.
  auto * tree = this->root();
  for(auto * leaf : leaves)
  {
    assert(leaf);
    auto * sibling = leaf->detach();
    tree = sibling->root();
    // Refit near where leaf used to be since it might move far away.
    sibling->refit_lineage();
    pad_box(pad,leaf->m_box);
    tree = tree->insert(leaf)->root();
    // Will potentially reach "above" `this`
    leaf->refit_lineage();
    leaf->rotate_lineage();
  }
  for(int pass = 0;pass<polish_rotate_passes;pass++)
  {
    for(auto * leaf : leaves)
    {
      assert(leaf);
      leaf->rotate_lineage();
    }
  }
  // `this` may have been deleted (during `detach` above). Return (possibly new)
  // root.
  return tree->root();
}

template <typename DerivedV, int DIM>
IGL_INLINE igl::AABB<DerivedV,DIM>* igl::AABB<DerivedV,DIM>::update(
    const Eigen::AlignedBox<Scalar,DIM> & new_box,
    const Scalar pad)
{
  auto * leaf = this;
  if(leaf->m_box.contains(new_box)) { return leaf; }
  leaf->m_box = new_box;
  pad_box(pad,leaf->m_box);
  assert(leaf);
  auto * sibling = leaf->detach();
  auto * tree = sibling->root();
  tree = tree->insert(leaf)->root();
  leaf->refit_lineage();
  leaf->rotate_lineage();
  return this->root();
}

template <typename DerivedV, int DIM>
IGL_INLINE igl::AABB<DerivedV,DIM>* igl::AABB<DerivedV,DIM>::insert_as_sibling(AABB * other)
{
  // Before
  //        parent
  //          |
  //        this(C)
  //        ╱  ╲
  //      left right
  //
  // After
  //        parent
  //          |
  //         newbie
  //        ╱   ╲
  //     this    other
  //     ╱    ╲
  //   left  right
  //

  auto * parent = this->m_parent;

  AABB<DerivedV,DIM> * newbie = new AABB<DerivedV,DIM>();
  newbie->m_parent = parent;
  newbie->m_left = this;
  newbie->m_right = other;
  newbie->m_box.extend(this->m_box);
  newbie->m_box.extend(other->m_box);
  newbie->m_primitive = -1;
  this->m_parent = newbie;
  other->m_parent = newbie;

  if(parent)
  {
    parent->m_left == this ? parent->m_left = newbie : parent->m_right = newbie;
    assert(parent->m_box.contains(newbie->m_box));
  }
  return newbie;
}

template <typename DerivedV, int DIM>
IGL_INLINE typename DerivedV::Scalar igl::AABB<DerivedV,DIM>::rotate(const bool dry_run)
{
  if(is_root()) { return false; }
  // Biased order.
  //
  // Would be good to check for a ton of insertions whether only one of these
  // ever returns true.
  //
  //        grandparent
  //        ╱         ╲
  //     parent       pibling°
  //     ╱    ╲         ╱    ╲
  // sibling  this   cuz1°  cuz2°
  //  ╱    ╲
  // nib1° nib2°

  // There is a _ton_ of repeated computation of surface areas and new-boxes in
  // these.

  // Rotate across first is much better. Then up seems slightly better than
  // down, but this is moot if we're chooseing the best one.
  const Scalar across_delta_sa = rotate_across(true);
  const Scalar up_delta_sa     = rotate_up(    true);
  const Scalar down_delta_sa   = rotate_down(  true);
  if(dry_run){ std::min({across_delta_sa,up_delta_sa,down_delta_sa}); }

  // conduct the rotate with smallest delta
  if(across_delta_sa <= up_delta_sa && across_delta_sa <= down_delta_sa)
  {
    return rotate_across(false);
  }else if(up_delta_sa <= down_delta_sa)
  {
    return rotate_up(false);
  }else
  {
    return rotate_down(false);
  }
}

template <typename DerivedV, int DIM>
IGL_INLINE typename DerivedV::Scalar igl::AABB<DerivedV,DIM>::rotate_across(const bool dry_run)
{
  // Before
  //        grandparent
  //        ╱         ╲
  //     parent       pibling
  //     ╱    ╲         ╱    ╲
  // sibling  this    cuz1  cuz2
  //
  //
  // Candidates
  //        grandparent
  //        ╱         ╲
  //     parent       pibling
  //     ╱    ╲         ╱    ╲
  // sibling  cuz1   this   cuz2
  //
  // Or
  //        grandparent
  //        ╱         ╲
  //     parent       pibling
  //     ╱    ╲         ╱    ╲
  // sibling  cuz2    cuz1  this

  auto * parent = this->m_parent;
  if(!parent) { return false; }
  const auto * grandparent = parent->m_parent;
  if(!grandparent) { return false; }
  auto * pibling = grandparent->m_left == parent ? grandparent->m_right : grandparent->m_left;
  if(!pibling) { return false; }
  const auto * sibling = parent->m_left == this ? parent->m_right : parent->m_left;

  const Scalar current_sa = box_surface_area(parent->m_box) + box_surface_area(pibling->m_box);

  const auto rotate_across_with = [&](
      bool inner_dry_run,
      igl::AABB<DerivedV,DIM> * cuz,
      igl::AABB<DerivedV,DIM> * other_cuz
      )->Scalar
  {
    // Before
    //        grandparent
    //        ╱         ╲
    //     parent       pibling
    //     ╱    ╲         ╱    ╲
    // sibling  this    cuz  other_cuz
    //
    //
    // Candidates
    //        grandparent
    //        ╱         ╲
    //     parent       pibling
    //     ╱    ╲         ╱    ╲
    // sibling  cuz   this   other_cuz
    if(!cuz){ return 0.0; }

    Eigen::AlignedBox<Scalar,DIM> new_parent_box;
    if(sibling) { new_parent_box.extend(sibling->m_box); }
    new_parent_box.extend(cuz->m_box);
    Eigen::AlignedBox<Scalar,DIM> new_pibling_box = this->m_box;
    if(other_cuz) { new_pibling_box.extend(other_cuz->m_box); }
    const Scalar new_sa =
      box_surface_area(new_parent_box) + box_surface_area(new_pibling_box);
    if(current_sa <= new_sa)
    {
      return 0.0;
    }

    if(!inner_dry_run)
    {
      assert(!dry_run);
      parent->m_box = new_parent_box;
      (parent->m_left == this ? parent->m_left : parent->m_right) = cuz;
      cuz->m_parent = parent;

      pibling->m_box = new_pibling_box;
      (pibling->m_left == cuz ? pibling->m_left : pibling->m_right) = this;
      this->m_parent = pibling;
    }
    return new_sa - current_sa;
  };

  auto * cuz1 = pibling->m_left;
  auto * cuz2 = pibling->m_right;
  const Scalar delta_1 = rotate_across_with(true,cuz1,cuz2);
  const Scalar delta_2 = rotate_across_with(true,cuz2,cuz1);
  if(delta_1 < delta_2)
  {
    return dry_run ? delta_1 : rotate_across_with(false,cuz1,cuz2);
  }else
  {
    return dry_run ? delta_2 : rotate_across_with(false,cuz2,cuz1);
  }
}

template <typename DerivedV, int DIM>
IGL_INLINE typename DerivedV::Scalar igl::AABB<DerivedV,DIM>::rotate_up(const bool dry_run)
{
  // Before
  //    grandparent
  //       ╱    ╲
  //     other  parent
  //            ╱  ╲
  //         this  sibling
  //
  //
  // Candidate
  //    grandparent
  //       ╱    ╲
  //    this    parent
  //            ╱  ╲
  //        other  sibling
  //
  auto * challenger = this;
  auto * parent = challenger->m_parent;
  if(!parent) { return false; }
  auto * grandparent = parent->m_parent;
  if(!grandparent) { return false; }
  auto * reining = grandparent->m_left == parent ? grandparent->m_right : grandparent->m_left;
  if(!reining) { return false; }
  auto * sibling = parent->m_left == challenger ? parent->m_right : parent->m_left;
  return rotate_up(dry_run,reining,grandparent,parent,challenger,sibling);
}


template <typename DerivedV, int DIM>
IGL_INLINE typename DerivedV::Scalar igl::AABB<DerivedV,DIM>::rotate_down(const bool dry_run)
{
  // Before
  //       parent
  //       ╱    ╲
  //     this   sibling
  //            ╱  ╲
  //         left  right
  //
  //
  // Candidates
  //       parent
  //       ╱    ╲
  //   left     sibling
  //            ╱  ╲
  //        this   right
  //
  // Or
  //
  //       parent
  //       ╱    ╲
  //   right    sibling
  //            ╱  ╲
  //        left   this
  auto * parent = this->m_parent;
  if(!parent) { return false; }
  auto * sibling = parent->m_left == this ? parent->m_right : parent->m_left;
  if(!sibling) { return false; }
  const Scalar left_sa  = rotate_up(true,this,parent,sibling,sibling->m_left,sibling->m_right);
  const Scalar right_sa = rotate_up(true,this,parent,sibling,sibling->m_right,sibling->m_left);
  if(left_sa < right_sa)
  {
    return dry_run ?  left_sa : rotate_up(false,this,parent,sibling,sibling->m_left,sibling->m_right);
  }else
  {
    return dry_run ? right_sa : rotate_up(false,this,parent,sibling,sibling->m_right,sibling->m_left);
  }
}

template <typename DerivedV, int DIM>
IGL_INLINE typename DerivedV::Scalar igl::AABB<DerivedV,DIM>::rotate_up(
  const bool dry_run,
  igl::AABB<DerivedV,DIM>* reining,
  igl::AABB<DerivedV,DIM>* grandparent,
  igl::AABB<DerivedV,DIM>* parent,
  igl::AABB<DerivedV,DIM>* challenger,
  igl::AABB<DerivedV,DIM>* sibling)
{
  // if any are null return false
  if(!reining) { return false; }
  if(!grandparent) { return false; }
  if(!parent) { return false; }
  if(!challenger) { return false; }
  if(!sibling) { return false; }
  // Before
  //    grandparent
  //       ╱      ╲
  //  reining      parent
  //               ╱    ╲
  //        challenger  sibling
  //
  //
  // Candidate
  //    grandparent
  //       ╱      ╲
  // challenger    parent
  //               ╱    ╲
  //          reining   sibling
  //

  auto sibling_sa = 0;
  // Sibling doesn't actually need to exist but probably should if parent is a
  // true internal node.
  if(sibling)
  {
    sibling_sa = box_surface_area(sibling->m_box);
  }

  const auto challenger_sa = box_surface_area(challenger->m_box);
  const auto parent_sa = box_surface_area(challenger->m_parent->m_box);
  const auto reining_sa = box_surface_area(reining->m_box);
  auto new_parent_box = reining->m_box;
  new_parent_box.extend(sibling->m_box);
  const auto new_parent_sa = box_surface_area(new_parent_box);
                                       // Cancels in comparison
  const auto before_sa = parent_sa    ;// + reining_sa + challenger_sa + sibling_sa;
  const auto after_sa  = new_parent_sa;// + reining_sa + challenger_sa + sibling_sa;
  if(before_sa <= after_sa)
  {
    // No improvment.
    return 0.0;
  }
  if(!dry_run)
  {
    // May reorder left and right but challenger doesn't matter.
    grandparent->m_left = challenger;
    grandparent->m_right = parent;
    challenger->m_parent = grandparent;
    parent->m_parent = grandparent;
    parent->m_left = reining;
    parent->m_right = sibling;
    reining->m_parent = parent;
    if(sibling){ sibling->m_parent = parent; }
    parent->m_box = new_parent_box;
  }
  return after_sa - before_sa;
}

template <typename DerivedV, int DIM>
IGL_INLINE int igl::AABB<DerivedV,DIM>::subtree_size() const
{
  // 1 for self
  int n = 1;
  int n_left = 0,n_right = 0;
  if(m_left != nullptr)
  {
    n_left = m_left->subtree_size();
  }
  if(m_right != nullptr)
  {
    n_right = m_right->subtree_size();
  }
  n += 2*std::max(n_left,n_right);
  return n;
}

template <typename DerivedV, int DIM>
IGL_INLINE void igl::AABB<DerivedV,DIM>::rotate_lineage()
{
  std::vector<igl::AABB<DerivedV, DIM> *> lineage;
  {
    auto * node = this;
    while(node)
    {
      lineage.push_back(node);
      node = node->m_parent;
    }
  }
  // O(h)
  while(!lineage.empty())
  {
    auto * node = lineage.back();
    lineage.pop_back();
    assert(node);
    const bool ret = node->rotate();
  }
}

template <typename DerivedV, int DIM>
IGL_INLINE bool igl::AABB<DerivedV,DIM>::append_intersecting_leaves(
    const Eigen::AlignedBox<igl::AABB<DerivedV,DIM>::Scalar,DIM> & box,
    std::vector<const igl::AABB<DerivedV,DIM>*> & leaves) const
{
  if(!box.intersects(m_box)){ return false;}

  if(is_leaf())
  {
    leaves.push_back(this);
    return true;
  }
  bool any_left = (m_left ? m_left->append_intersecting_leaves(box,leaves) : false);
  bool any_right = (m_right ? m_right->append_intersecting_leaves(box,leaves) : false);
  return any_left || any_right;
}

template <typename DerivedV, int DIM>
IGL_INLINE typename DerivedV::Scalar igl::AABB<DerivedV,DIM>::internal_surface_area() const
{
  // Don't include self (parent's call will add me if I'm not a root or leaf)
  Scalar surface_area = 0;
  if(m_left && !m_left->is_leaf())
  {
    surface_area += box_surface_area(m_left->m_box);
    surface_area += m_left->internal_surface_area();
  }
  if(m_right && !m_right->is_leaf())
  {
    surface_area += box_surface_area(m_right->m_box);
    surface_area += m_right->internal_surface_area();
  }
  return surface_area;
}

template <typename DerivedV, int DIM>
IGL_INLINE void igl::AABB<DerivedV,DIM>::validate() const
{
  if(this->is_leaf())
  {
    assert(this->m_primitive >= 0 || this->is_root());
  }
  if(this->m_left)
  {
    assert(this->m_box.contains(this->m_left->m_box));
    assert(this->m_left->m_parent == this);
    this->m_left->validate();
  }
  if(this->m_right)
  {
    assert(this->m_box.contains(this->m_right->m_box));
    assert(this->m_right->m_parent == this);
    this->m_right->validate();
  }
}

template <typename DerivedV, int DIM>
IGL_INLINE void igl::AABB<DerivedV,DIM>::print(const int depth) const
{
  const auto indent = std::string(depth*2,' ');
  printf("%s%p",indent.c_str(),this);
  if(this->is_leaf())
  {
    printf(" [%d]",this->m_primitive);
  }
  printf("\n");
  if(this->m_left)
  {
    assert(this->m_box.contains(this->m_left->m_box));
    assert(this->m_left->m_parent == this);
    this->m_left->print(depth+1);
  }
  if(this->m_right)
  {
    assert(this->m_box.contains(this->m_right->m_box));
    assert(this->m_right->m_parent == this);
    this->m_right->print(depth+1);
  }
}

template <typename DerivedV, int DIM>
IGL_INLINE int igl::AABB<DerivedV,DIM>::size() const
{
  return 1 +
    (this->m_left ? this->m_left ->size():0) +
    (this->m_right? this->m_right->size():0);
}
template <typename DerivedV, int DIM>
IGL_INLINE int igl::AABB<DerivedV,DIM>::height() const
{
  return 1 + std::max(
    (this->m_left ?this->m_left ->height():0),
    (this->m_right?this->m_right->height():0));
}

template <typename DerivedV, int DIM>
IGL_INLINE void igl::AABB<DerivedV,DIM>::set_min(
  const RowVectorDIMS & /* p */,
  const Scalar sqr_d_candidate,
  const int & i_candidate,
  const RowVectorDIMS & c_candidate,
  Scalar & sqr_d,
  int & i,
  Eigen::PlainObjectBase<RowVectorDIMS> & c) const
{
  if(sqr_d_candidate < sqr_d)
  {
    i = i_candidate;
    c = c_candidate;
    sqr_d = sqr_d_candidate;
  }
}


///////////////////////////////////////////////////////////////////////////////
// Templated member functions
///////////////////////////////////////////////////////////////////////////////


template <typename DerivedV, int DIM>
template <typename DerivedEle, typename Derivedbb_mins, typename Derivedbb_maxs, typename Derivedelements>
IGL_INLINE void igl::AABB<DerivedV,DIM>::init(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedEle> & Ele,
    const Eigen::MatrixBase<Derivedbb_mins> & bb_mins,
    const Eigen::MatrixBase<Derivedbb_maxs> & bb_maxs,
    const Eigen::MatrixBase<Derivedelements> & elements,
    const int i)
{
  using namespace std;
  using namespace Eigen;
  clear();
  if(bb_mins.size() > 0)
  {
    assert(bb_mins.rows() == bb_maxs.rows() && "Serial tree arrays must match");
    assert(bb_mins.cols() == V.cols() && "Serial tree array dim must match V");
    assert(bb_mins.cols() == bb_maxs.cols() && "Serial tree arrays must match");
    assert(bb_mins.rows() == elements.rows() &&
        "Serial tree arrays must match");
    // construct from serialization
    m_box.extend(bb_mins.row(i).transpose());
    m_box.extend(bb_maxs.row(i).transpose());
    m_primitive = elements(i);
    // Not leaf then recurse
    if(m_primitive == -1)
    {
      m_left = new AABB();
      m_left->init( V,Ele,bb_mins,bb_maxs,elements,2*i+1);
      m_left->m_parent = this;
      m_right = new AABB();
      m_right->init( V,Ele,bb_mins,bb_maxs,elements,2*i+2);
      m_right->m_parent = this;
      //m_depth = std::max( m_left->m_depth, m_right->m_depth)+1;
    }
  }else
  {
    VectorXi allI = colon<int>(0,Ele.rows()-1);
    MatrixXDIMS BC;
    if(Ele.cols() == 1)
    {
      // points
      BC = V;
    }else
    {
      // Simplices
      barycenter(V,Ele,BC);
    }
    MatrixXi SI(BC.rows(),BC.cols());
    {
      MatrixXDIMS _;
      MatrixXi IS;
      igl::sort(BC,1,true,_,IS);
      // Need SI(i) to tell which place i would be sorted into
      const int dim = IS.cols();
      for(int i = 0;i<IS.rows();i++)
      {
        for(int d = 0;d<dim;d++)
        {
          SI(IS(i,d),d) = i;
        }
      }
    }
    init(V,Ele,SI,allI);
  }
}

template <typename DerivedV, int DIM>
template <typename DerivedEle>
void igl::AABB<DerivedV,DIM>::init(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedEle> & Ele)
{
  using namespace Eigen;
  // clear will be immediately called...
  return init(V,Ele,MatrixXDIMS(),MatrixXDIMS(),VectorXi(),0);
}

  template <typename DerivedV, int DIM>
template <
  typename DerivedEle,
  typename DerivedSI,
  typename DerivedI>
IGL_INLINE void igl::AABB<DerivedV,DIM>::init(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedEle> & Ele,
    const Eigen::MatrixBase<DerivedSI> & SI,
    const Eigen::MatrixBase<DerivedI> & I)
{
  using namespace Eigen;
  using namespace std;
  clear();
  if(V.size() == 0 || Ele.size() == 0 || I.size() == 0)
  {
    return;
  }
  assert(DIM == V.cols() && "V.cols() should matched declared dimension");
  //const Scalar inf = numeric_limits<Scalar>::infinity();
  m_box = AlignedBox<Scalar,DIM>();
  // Compute bounding box
  for(int i = 0;i<I.rows();i++)
  {
    for(int c = 0;c<Ele.cols();c++)
    {
      m_box.extend(V.row(Ele(I(i),c)).transpose());
      m_box.extend(V.row(Ele(I(i),c)).transpose());
    }
  }
  switch(I.size())
  {
    case 0:
      {
        assert(false);
      }
    case 1:
      {
        m_primitive = I(0);
        break;
      }
    default:
      {
        // Compute longest direction
        int max_d = -1;
        m_box.diagonal().maxCoeff(&max_d);
        // Can't use median on BC directly because many may have same value,
        // but can use median on sorted BC indices
        VectorXi SIdI(I.rows());
        for(int i = 0;i<I.rows();i++)
        {
          SIdI(i) = SI(I(i),max_d);
        }
        // Pass by copy to avoid changing input
        const auto median = [](VectorXi A)->int
        {
          size_t n = (A.size()-1)/2;
          nth_element(A.data(),A.data()+n,A.data()+A.size());
          return A(n);
        };
        const int med = median(SIdI);
        VectorXi LI((I.rows()+1)/2),RI(I.rows()/2);
        assert(LI.rows()+RI.rows() == I.rows());
        // Distribute left and right
        {
          int li = 0;
          int ri = 0;
          for(int i = 0;i<I.rows();i++)
          {
            if(SIdI(i)<=med)
            {
              LI(li++) = I(i);
            }else
            {
              RI(ri++) = I(i);
            }
          }
        }
        //m_depth = 0;
        if(LI.rows()>0)
        {
          m_left = new AABB();
          m_left->init(V,Ele,SI,LI);
          m_left->m_parent = this;
          //m_depth = std::max(m_depth, m_left->m_depth+1);
        }
        if(RI.rows()>0)
        {
          m_right = new AABB();
          m_right->init(V,Ele,SI,RI);
          m_right->m_parent = this;
          //m_depth = std::max(m_depth, m_right->m_depth+1);
        }
      }
  }
}


template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE igl::AABB<DerivedV,DIM>* igl::AABB<DerivedV,DIM>::update_primitive(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedEle> & Ele,
    const Scalar pad)
{
  assert(this->is_leaf());
  assert(this->m_primitive >= 0 && this->m_primitive < Ele.rows());
  Eigen::AlignedBox<double, 3> new_box;
  for(int c = 0;c<Ele.cols();c++)
  {
    new_box.extend(V.row(Ele(this->m_primitive,c)).transpose());
  }
  return this->update(new_box,pad);
}

template <typename DerivedV, int DIM>
IGL_INLINE igl::AABB<DerivedV,DIM>* igl::AABB<DerivedV,DIM>::insert(AABB * other)
{
  // test if this is the same pointer as other
  if(this == other)
  {
    // Only expecting this to happen when this/other are a singleton tree
    assert(this->is_root() && this->is_leaf());
    // Nothing changed.
    return this;
  }


  if(this->is_leaf())
  {
    return this->insert_as_sibling(other);
  }

  // internal node case
  if(!this->m_box.contains(other->m_box))
  {
    // it's annoying to have these special root handling cases. I wonder if the
    // root should have been an ∞ node...
    return insert_as_sibling(other);
  }

  Eigen::AlignedBox<Scalar,DIM> left_grow = this->m_left->m_box;
  left_grow.extend(other->m_box);
  Eigen::AlignedBox<Scalar,DIM> right_grow = this->m_right->m_box;
  right_grow.extend(other->m_box);
  const auto left_surface_area_increase =
    box_surface_area(left_grow) - box_surface_area(this->m_left->m_box);
  const auto right_surface_area_increase =
    box_surface_area(right_grow) - box_surface_area(this->m_right->m_box);
  assert(left_surface_area_increase >= 0);
  assert(right_surface_area_increase >= 0);
  // Handle both (left_surface_area_increase <= 0 && right_surface_area_increase <= 0)
  return left_surface_area_increase < right_surface_area_increase ?
    this->m_left->insert(other) :
    this->m_right->insert(other);
}


template <typename DerivedV, int DIM>
template <typename DerivedEle, typename Derivedq>
IGL_INLINE std::vector<int> igl::AABB<DerivedV,DIM>::find(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedEle> & Ele,
    const Eigen::MatrixBase<Derivedq> & q,
    const bool first) const
{
  using namespace std;
  using namespace Eigen;
  assert(q.size() == DIM &&
      "Query dimension should match aabb dimension");
  assert(Ele.cols() == V.cols()+1 &&
      "AABB::find only makes sense for (d+1)-simplices");
  // Check if outside bounding box
  bool inside = m_box.contains(q.transpose());
  if(!inside)
  {
    return std::vector<int>();
  }
  assert(m_primitive==-1 || (m_left == nullptr && m_right == nullptr));
  if(is_leaf())
  {
    if(AABB_all_positive_barycentric_coordinates_helper<
        DerivedV,DerivedEle,Derivedq, DIM>::compute(V,Ele,m_primitive,q))
    {
      return std::vector<int>(1,m_primitive);
    }else
    {
      return std::vector<int>();
    }
  }
  std::vector<int> left = m_left->find(V,Ele,q,first);
  if(first && !left.empty())
  {
    return left;
  }
  std::vector<int> right = m_right->find(V,Ele,q,first);
  if(first)
  {
    return right;
  }
  left.insert(left.end(),right.begin(),right.end());
  return left;
}



template <typename DerivedV, int DIM>
template <typename Derivedbb_mins, typename Derivedbb_maxs, typename Derivedelements>
IGL_INLINE void igl::AABB<DerivedV,DIM>::serialize(
    Eigen::PlainObjectBase<Derivedbb_mins> & bb_mins,
    Eigen::PlainObjectBase<Derivedbb_maxs> & bb_maxs,
    Eigen::PlainObjectBase<Derivedelements> & elements,
    const int i) const
{
  using namespace std;
  using namespace Eigen;
  // Calling for root then resize output
  if(i==0)
  {
    const int m = subtree_size();
    //cout<<"m: "<<m<<endl;
    bb_mins.resize(m,DIM);
    bb_maxs.resize(m,DIM);
    elements.resize(m,1);
  }
  //cout<<i<<" ";
  bb_mins.row(i) = m_box.min();
  bb_maxs.row(i) = m_box.max();
  elements(i) = m_primitive;
  if(m_left != nullptr)
  {
    m_left->serialize(bb_mins,bb_maxs,elements,2*i+1);
  }
  if(m_right != nullptr)
  {
    m_right->serialize(bb_mins,bb_maxs,elements,2*i+2);
  }
}

template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE typename igl::AABB<DerivedV,DIM>::Scalar
igl::AABB<DerivedV,DIM>::squared_distance(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & p,
  int & i,
  Eigen::PlainObjectBase<RowVectorDIMS> & c) const
{
  return squared_distance(V,Ele,p,std::numeric_limits<Scalar>::infinity(),i,c);
}


template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE typename igl::AABB<DerivedV,DIM>::Scalar
igl::AABB<DerivedV,DIM>::squared_distance(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & p,
  Scalar low_sqr_d,
  Scalar up_sqr_d,
  int & i,
  Eigen::PlainObjectBase<RowVectorDIMS> & c) const
{
  using namespace Eigen;
  using namespace std;
  //assert(low_sqr_d <= up_sqr_d);
  if(low_sqr_d > up_sqr_d)
  {
    return low_sqr_d;
  }
  Scalar sqr_d = up_sqr_d;
  //assert(DIM == 3 && "Code has only been tested for DIM == 3");
  assert((Ele.cols() == 3 || Ele.cols() == 2 || Ele.cols() == 1)
    && "Code has only been tested for simplex sizes 3,2,1");

  assert(m_primitive==-1 || (m_left == nullptr && m_right == nullptr));
  if(is_leaf())
  {
    leaf_squared_distance(V,Ele,p,low_sqr_d,sqr_d,i,c);
  }else
  {
    bool looked_left = false;
    bool looked_right = false;
    const auto & look_left = [&]()
    {
      int i_left;
      RowVectorDIMS c_left = c;
      Scalar sqr_d_left =
        m_left->squared_distance(V,Ele,p,low_sqr_d,sqr_d,i_left,c_left);
      this->set_min(p,sqr_d_left,i_left,c_left,sqr_d,i,c);
      looked_left = true;
    };
    const auto & look_right = [&]()
    {
      int i_right;
      RowVectorDIMS c_right = c;
      Scalar sqr_d_right =
        m_right->squared_distance(V,Ele,p,low_sqr_d,sqr_d,i_right,c_right);
      this->set_min(p,sqr_d_right,i_right,c_right,sqr_d,i,c);
      looked_right = true;
    };

    // must look left or right if in box
    if(m_left->m_box.contains(p.transpose()))
    {
      look_left();
    }
    if(m_right->m_box.contains(p.transpose()))
    {
      look_right();
    }
    // if haven't looked left and could be less than current min, then look
    Scalar left_up_sqr_d =
      m_left->m_box.squaredExteriorDistance(p.transpose());
    Scalar right_up_sqr_d =
      m_right->m_box.squaredExteriorDistance(p.transpose());
    if(left_up_sqr_d < right_up_sqr_d)
    {
      if(!looked_left && left_up_sqr_d<sqr_d)
      {
        look_left();
      }
      if( !looked_right && right_up_sqr_d<sqr_d)
      {
        look_right();
      }
    }else
    {
      if( !looked_right && right_up_sqr_d<sqr_d)
      {
        look_right();
      }
      if(!looked_left && left_up_sqr_d<sqr_d)
      {
        look_left();
      }
    }
  }
  return sqr_d;
}

template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE typename igl::AABB<DerivedV,DIM>::Scalar
igl::AABB<DerivedV,DIM>::squared_distance(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & p,
  Scalar up_sqr_d,
  int & i,
  Eigen::PlainObjectBase<RowVectorDIMS> & c) const
{
  return squared_distance(V,Ele,p,0.0,up_sqr_d,i,c);
}

template <typename DerivedV, int DIM>
template <
  typename DerivedEle,
  typename DerivedP,
  typename DerivedsqrD,
  typename DerivedI,
  typename DerivedC>
IGL_INLINE void igl::AABB<DerivedV,DIM>::squared_distance(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const Eigen::MatrixBase<DerivedP> & P,
  Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedC> & C) const
{
  assert(P.cols() == V.cols() && "cols in P should match dim of cols in V");
  sqrD.resize(P.rows(),1);
  I.resize(P.rows(),1);
  C.resizeLike(P);
  // O( #P * log #Ele ), where log #Ele is really the depth of this AABB
  // hierarchy
  //for(int p = 0;p<P.rows();p++)
  igl::parallel_for(P.rows(),[&](int p)
    {
      RowVectorDIMS Pp = P.row(p).template head<DIM>(), c;
      int Ip;
      sqrD(p) = squared_distance(V,Ele,Pp,Ip,c);
      I(p) = Ip;
      C.row(p).head(DIM) = c;
    },
    10000);
}

template <typename DerivedV, int DIM>
template <
  typename DerivedEle,
  typename Derivedother_V,
  typename Derivedother_Ele,
  typename DerivedsqrD,
  typename DerivedI,
  typename DerivedC>
IGL_INLINE void igl::AABB<DerivedV,DIM>::squared_distance(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const AABB<Derivedother_V,DIM> & other,
  const Eigen::MatrixBase<Derivedother_V> & other_V,
  const Eigen::MatrixBase<Derivedother_Ele> & other_Ele,
  Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedC> & C) const
{
  assert(other_Ele.cols() == 1 &&
    "Only implemented for other as list of points");
  assert(other_V.cols() == V.cols() && "other must match this dimension");
  sqrD.setConstant(other_Ele.rows(),1,std::numeric_limits<double>::infinity());
  I.resize(other_Ele.rows(),1);
  C.resize(other_Ele.rows(),other_V.cols());
  // All points in other_V currently think they need to check against root of
  // this. The point of using another AABB is to quickly prune chunks of
  // other_V so that most points just check some subtree of this.

  // This holds a conservative estimate of max(sqr_D) where sqr_D is the
  // current best minimum squared distance for all points in this subtree
  double up_sqr_d = std::numeric_limits<double>::infinity();
  squared_distance_helper(
    V,Ele,&other,other_V,other_Ele,0,up_sqr_d,sqrD,I,C);
}


template <typename DerivedV, int DIM>
template <
  typename DerivedEle,
  typename Derivedother_V,
  typename Derivedother_Ele,
  typename DerivedsqrD,
  typename DerivedI,
  typename DerivedC>
IGL_INLINE typename igl::AABB<DerivedV,DIM>::Scalar
  igl::AABB<DerivedV,DIM>::squared_distance_helper(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const AABB<Derivedother_V,DIM> * other,
  const Eigen::MatrixBase<Derivedother_V> & other_V,
  const Eigen::MatrixBase<Derivedother_Ele> & other_Ele,
  const Scalar /*up_sqr_d*/,
  Eigen::PlainObjectBase<DerivedsqrD> & sqrD,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedC> & C) const
{
  using namespace std;
  using namespace Eigen;

  // This implementation is a bit disappointing. There's no major speed up. Any
  // performance gains seem to come from accidental cache coherency and
  // diminish for larger "other" (the opposite of what was intended).

  // Base case
  if(other->is_leaf() && this->is_leaf())
  {
    Scalar sqr_d = sqrD(other->m_primitive);
    int i = I(other->m_primitive);
    RowVectorDIMS c = C.row(      other->m_primitive);
    RowVectorDIMS p = other_V.row(other->m_primitive);
    leaf_squared_distance(V,Ele,p,sqr_d,i,c);
    sqrD( other->m_primitive) = sqr_d;
    I(    other->m_primitive) = i;
    C.row(other->m_primitive) = c;
    //cout<<"leaf: "<<sqr_d<<endl;
    //other->m_low_sqr_d = sqr_d;
    return sqr_d;
  }

  if(other->is_leaf())
  {
    Scalar sqr_d = sqrD(other->m_primitive);
    int i = I(other->m_primitive);
    RowVectorDIMS c = C.row(      other->m_primitive);
    RowVectorDIMS p = other_V.row(other->m_primitive);
    sqr_d = squared_distance(V,Ele,p,sqr_d,i,c);
    sqrD( other->m_primitive) = sqr_d;
    I(    other->m_primitive) = i;
    C.row(other->m_primitive) = c;
    //other->m_low_sqr_d = sqr_d;
    return sqr_d;
  }

  //// Exact minimum squared distance between arbitrary primitives inside this and
  //// othre's bounding boxes
  //const auto & min_squared_distance = [&](
  //  const AABB<DerivedV,DIM> * A,
  //  const AABB<Derivedother_V,DIM> * B)->Scalar
  //{
  //  return A->m_box.squaredExteriorDistance(B->m_box);
  //};

  if(this->is_leaf())
  {
    //if(min_squared_distance(this,other) < other->m_low_sqr_d)
    if(true)
    {
      this->squared_distance_helper(
        V,Ele,other->m_left,other_V,other_Ele,0,sqrD,I,C);
      this->squared_distance_helper(
        V,Ele,other->m_right,other_V,other_Ele,0,sqrD,I,C);
    }else
    {
      // This is never reached...
    }
    //// we know other is not a leaf
    //other->m_low_sqr_d = std::max(other->m_left->m_low_sqr_d,other->m_right->m_low_sqr_d);
    return 0;
  }

  // FORCE DOWN TO OTHER LEAF EVAL
  //if(min_squared_distance(this,other) < other->m_low_sqr_d)
  if(true)
  {
    if(true)
    {
      this->squared_distance_helper(
        V,Ele,other->m_left,other_V,other_Ele,0,sqrD,I,C);
      this->squared_distance_helper(
        V,Ele,other->m_right,other_V,other_Ele,0,sqrD,I,C);
    }else // this direction never seems to be faster
    {
      this->m_left->squared_distance_helper(
        V,Ele,other,other_V,other_Ele,0,sqrD,I,C);
      this->m_right->squared_distance_helper(
        V,Ele,other,other_V,other_Ele,0,sqrD,I,C);
    }
  }else
  {
    // this is never reached ... :-(
  }
  //// we know other is not a leaf
  //other->m_low_sqr_d = std::max(other->m_left->m_low_sqr_d,other->m_right->m_low_sqr_d);

  return 0;
}

template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE void igl::AABB<DerivedV,DIM>::leaf_squared_distance(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & p,
  const Scalar low_sqr_d,
  Scalar & sqr_d,
  int & i,
  Eigen::PlainObjectBase<RowVectorDIMS> & c) const
{
  using namespace Eigen;
  using namespace std;
  if(low_sqr_d > sqr_d)
  {
    sqr_d = low_sqr_d;
    return;
  }
  RowVectorDIMS c_candidate;
  Scalar sqr_d_candidate;
  igl::point_simplex_squared_distance<DIM>(
    p,V,Ele,m_primitive,sqr_d_candidate,c_candidate);
  set_min(p,sqr_d_candidate,m_primitive,c_candidate,sqr_d,i,c);
}

template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE void igl::AABB<DerivedV,DIM>::leaf_squared_distance(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & p,
  Scalar & sqr_d,
  int & i,
  Eigen::PlainObjectBase<RowVectorDIMS> & c) const
{
  return leaf_squared_distance(V,Ele,p,0,sqr_d,i,c);
}



template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE bool
igl::AABB<DerivedV,DIM>::intersect_ray(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & origin,
  const RowVectorDIMS & dir,
  std::vector<igl::Hit<typename DerivedV::Scalar>> & hits) const
{
  RowVectorDIMS inv_dir = dir.cwiseInverse();
  RowVectorDIMS inv_dir_pad = inv_dir;
  igl::increment_ulp(inv_dir_pad, 2);
  return intersect_ray_opt(V, Ele, origin, dir, inv_dir, inv_dir_pad, hits);
}

template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE bool
igl::AABB<DerivedV,DIM>::intersect_ray(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & origin,
  const RowVectorDIMS & dir,
  igl::Hit<typename DerivedV::Scalar> & hit) const
{
#if false
  // BFS
  std::queue<const AABB *> Q;
  // Or DFS
  //std::stack<const AABB *> Q;
  Q.push(this);
  bool any_hit = false;
  hit.t = std::numeric_limits<Scalar>::infinity();
  while(!Q.empty())
  {
    const AABB * tree = Q.front();
    //const AABB * tree = Q.top();
    Q.pop();
    {
      Scalar _1,_2;
      if(!ray_box_intersect(
        origin,dir,tree->m_box,Scalar(0),Scalar(hit.t),_1,_2))
      {
        continue;
      }
    }
    if(tree->is_leaf())
    {
      // Actually process elements
      assert((Ele.size() == 0 || Ele.cols() == 3) && "Elements should be triangles");
      igl::Hit<typename DerivedV::Scalar> leaf_hit;
      if(
        ray_mesh_intersect(origin,dir,V,Ele.row(tree->m_primitive),leaf_hit)&&
        leaf_hit.t < hit.t)
      {
        // correct the id
        leaf_hit.id = tree->m_primitive;
        hit = leaf_hit;
      }
      continue;
    }
    // Add children to queue
    Q.push(tree->m_left);
    Q.push(tree->m_right);
  }
  return any_hit;
#else
  // DFS
  return intersect_ray(
    V,Ele,origin,dir,std::numeric_limits<Scalar>::infinity(),hit);
#endif
}

template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE bool
igl::AABB<DerivedV,DIM>::intersect_ray(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & origin,
  const RowVectorDIMS & dir,
  const Scalar _min_t,
  igl::Hit<typename DerivedV::Scalar> & hit) const
{
  RowVectorDIMS inv_dir = dir.cwiseInverse();
  RowVectorDIMS inv_dir_pad = inv_dir;
  igl::increment_ulp(inv_dir_pad, 2);
  return intersect_ray_opt(V, Ele, origin, dir, inv_dir, inv_dir_pad, _min_t, hit);
}


template <typename DerivedV, int DIM>
template <
  typename DerivedEle,
  typename DerivedOrigin,
  typename DerivedDir,
  typename DerivedI,
  typename DerivedT,
  typename DerivedUV>
IGL_INLINE void igl::AABB<DerivedV,DIM>::intersect_ray(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const Eigen::MatrixBase<DerivedOrigin> & origin,
  const Eigen::MatrixBase<DerivedDir> & dir,
  const Scalar min_t,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedT> & T,
  Eigen::PlainObjectBase<DerivedUV> & UV)
{
  assert(origin.rows() == dir.rows());
  I.setConstant(origin.rows(),1,-1);
  T.setConstant(origin.rows(),1,std::numeric_limits<Scalar>::quiet_NaN());
  UV.resize(origin.rows(),2);

  igl::parallel_for(origin.rows(),[&](int i)
  {
    RowVectorDIMS origin_i = origin.row(i);
    RowVectorDIMS dir_i = dir.row(i);
    igl::Hit<typename DerivedV::Scalar> hit_i;
    if(intersect_ray(V,Ele,origin_i,dir_i,min_t,hit_i))
    {
      I(i) = hit_i.id;
      UV.row(i) << hit_i.u, hit_i.v;
      T(i) = hit_i.t;
    }
  },
  10000);
}

template <typename DerivedV, int DIM>
template <
  typename DerivedEle,
  typename DerivedOrigin,
  typename DerivedDir>
IGL_INLINE void igl::AABB<DerivedV,DIM>::intersect_ray(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const Eigen::MatrixBase<DerivedOrigin> & origin,
  const Eigen::MatrixBase<DerivedDir> & dir,
  std::vector<std::vector<igl::Hit<typename DerivedV::Scalar>>> & hits)
{
  assert(origin.rows() == dir.rows());
  hits.resize(origin.rows());
  igl::parallel_for(origin.rows(),[&](int i)
  {
    RowVectorDIMS origin_i = origin.row(i);
    RowVectorDIMS dir_i = dir.row(i);
    this->intersect_ray(V,Ele,origin_i,dir_i,hits[i]);
  },
  10000);
}

template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE bool
igl::AABB<DerivedV,DIM>::intersect_ray_opt(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & origin,
  const RowVectorDIMS & dir,
  const RowVectorDIMS & inv_dir,
  const RowVectorDIMS & inv_dir_pad,
  std::vector<igl::Hit<typename DerivedV::Scalar>> & hits) const
{
  hits.clear();
  const Scalar t0 = 0;
  const Scalar t1 = std::numeric_limits<Scalar>::infinity();
  {
    Scalar _1,_2;


    if(!ray_box_intersect(origin,inv_dir,inv_dir_pad,m_box,t0,t1,_1,_2))
    {
      return false;
    }
  }
  if(this->is_leaf())
  {
    // Actually process elements
    assert((Ele.size() == 0 || Ele.cols() == 3) && "Elements should be triangles");
    // Cheesecake way of hitting element
    bool ret = ray_mesh_intersect(origin,dir,V,Ele.row(m_primitive),hits);
    // Since we only gave ray_mesh_intersect a single face, it will have set
    // any hits to id=0. Set these to this primitive's id
    for(auto & hit: hits)
    {
      hit.id = m_primitive;
    }
    return ret;
  }
  std::vector<igl::Hit<typename DerivedV::Scalar>> left_hits;
  std::vector<igl::Hit<typename DerivedV::Scalar>> right_hits;
  const bool left_ret = m_left->intersect_ray_opt(V,Ele,origin,dir,inv_dir,inv_dir_pad,left_hits);
  const bool right_ret = m_right->intersect_ray_opt(V,Ele,origin,dir,inv_dir,inv_dir_pad,right_hits);
  hits.insert(hits.end(),left_hits.begin(),left_hits.end());
  hits.insert(hits.end(),right_hits.begin(),right_hits.end());
  return left_ret || right_ret;
}

template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE bool
igl::AABB<DerivedV,DIM>::intersect_ray_opt(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & origin,
  const RowVectorDIMS & dir,
  const RowVectorDIMS & inv_dir,
  const RowVectorDIMS & inv_dir_pad,
  const Scalar _min_t,
  igl::Hit<typename DerivedV::Scalar> & hit) const
{
  Scalar min_t = _min_t;
  const Scalar t0 = 0;
  {
    Scalar _1,_2;
    if(!ray_box_intersect(origin,inv_dir,inv_dir_pad,m_box,t0,min_t,_1,_2))
    {
      return false;
    }
  }
  if(this->is_leaf())
  {
    // Actually process elements
    assert((Ele.size() == 0 || Ele.cols() == 3) && "Elements should be triangles");
    // Cheesecake way of hitting element
    bool ret = ray_mesh_intersect(origin,dir,V,Ele.row(m_primitive),hit);
    hit.id = m_primitive;
    return ret;
  }

  // Doesn't seem like smartly choosing left before/after right makes a
  // differnce
  igl::Hit<typename DerivedV::Scalar> left_hit;
  igl::Hit<typename DerivedV::Scalar> right_hit;
  bool left_ret = m_left->intersect_ray_opt(V,Ele,origin,dir,inv_dir,inv_dir_pad,min_t,left_hit);
  if(left_ret && left_hit.t<min_t)
  {
    // It's scary that this line doesn't seem to matter....
    min_t = left_hit.t;
    hit = left_hit;
    left_ret = true;
  }else
  {
    left_ret = false;
  }
  bool right_ret = m_right->intersect_ray_opt(V,Ele,origin,dir,inv_dir,inv_dir_pad,min_t,right_hit);
  if(right_ret && right_hit.t<min_t)
  {
    min_t = right_hit.t;
    hit = right_hit;
    right_ret = true;
  }else
  {
    right_ret = false;
  }
  return left_ret || right_ret;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template bool igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::intersect_ray<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, std::vector<igl::Hit<double>, std::allocator<igl::Hit<double>> >&) const;

template class igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>;
template class igl::AABB<Eigen::Matrix<double, -1, 3, 0, -1, 3>, 3>;
template class igl::AABB<Eigen::Matrix<double, -1, 3, 1, -1, 3>, 3>;
template class igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>;
template class igl::AABB<Eigen::Matrix<double, -1, 2, 0, -1, 2>, 2>;

template igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>* igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::update_primitive<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, double);
// generated by autoexplicit.sh
template double igl::AABB<Eigen::Matrix<double, -1, 3, 0, -1, 3>, 3>::squared_distance<Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, double, double, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&) const;
// generated by autoexplicit.sh
template double igl::AABB<Eigen::Matrix<double, -1, 2, 0, -1, 2>, 2>::squared_distance<Eigen::Matrix<int, -1, 2, 0, -1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, Eigen::Matrix<double, 1, 2, 1, 1, 2> const&, double, double, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, 3, 0, -1, 3>, 3>::init<Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&);
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, 2, 0, -1, 2>, 2>::init<Eigen::Matrix<int, -1, 2, 0, -1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&);
// generated by autoexplicit.sh
template bool igl::AABB<Eigen::Matrix<float, -1, -1, 0, -1, -1>, 3>::intersect_ray<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<float, 1, 3, 1, 1, 3> const&, Eigen::Matrix<float, 1, 3, 1, 1, 3> const&, std::vector<igl::Hit<float>, std::allocator<igl::Hit<float>> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<float, -1, -1, 0, -1, -1>, 3>::init<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, 3, 1, -1, 3>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, 3, 1, -1, 3>, 3>::init<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
template bool igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::intersect_ray<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, igl::Hit<double>&) const;
template double igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, 1, 2, 1, 1, 2> const&, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&) const;
template double igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, double, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&) const;
template double igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&) const;
template double igl::AABB<Eigen::Matrix<double, -1, 3, 1, -1, 3>, 3>::squared_distance<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, double, double, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&) const;
template float igl::AABB<Eigen::Matrix<float, -1, 3, 0, -1, 3>, 3>::squared_distance<Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::Matrix<float, 1, 3, 1, 1, 3> const&, float, float, int&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&) const;
template float igl::AABB<Eigen::Matrix<float, -1, 3, 1, -1, 3>, 3>::squared_distance<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<float, 1, 3, 1, 1, 3> const&, int&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&) const;
template std::vector<int, std::allocator<int> > igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::find<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 1, -1, 1, 1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, bool) const;
template std::vector<int, std::allocator<int> > igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::find<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, bool) const;
template std::vector<int, std::allocator<int> > igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::find<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 1, -1, 1, 1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, bool) const;
template std::vector<int> igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::find<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 1, 2, 1, 1, 2>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2>> const&, bool) const;
template std::vector<int> igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::find<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 1, 3, 1, 1, 3>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3>> const&, bool) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::init<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::init<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int);
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::init<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&);
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::serialize<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, int) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::squared_distance<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::init<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::init<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int);
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::init<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&);
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::serialize<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, int) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<long, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 2, 3, 0, 2, 3>, Eigen::Matrix<double, 2, 1, 0, 2, 1>, Eigen::Matrix<int, 2, 1, 0, 2, 1>, Eigen::Matrix<double, 2, 3, 0, 2, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 2, 3, 0, 2, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 1, 0, 2, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, 2, 1, 0, 2, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 3, 0, 2, 3> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, 3, 1, -1, 3>, 3>::init<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&);
template void igl::AABB<Eigen::Matrix<float, -1, 3, 0, -1, 3>, 3>::init<Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&);
template void igl::AABB<Eigen::Matrix<float, -1, 3, 1, -1, 3>, 3>::init<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&);
template double igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&) const;
template double igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::squared_distance<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::Matrix<double, 1, 2, 1, 1, 2> const&, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&) const;
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::intersect_ray<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, double, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1>>&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>>&);
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::intersect_ray<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, std::vector<std::vector<igl::Hit<double>, std::allocator<igl::Hit<double>>>, std::allocator<std::vector<igl::Hit<double>, std::allocator<igl::Hit<double>>>>>&);
#ifdef WIN32
template void igl::AABB<class Eigen::Matrix<double,-1,-1,0,-1,-1>,3>::squared_distance<class Eigen::Matrix<int,-1,-1,0,-1,-1>,class Eigen::Matrix<double,-1,-1,0,-1,-1>,class Eigen::Matrix<double,-1,1,0,-1,1>,class Eigen::Matrix<__int64,-1,1,0,-1,1>,class Eigen::Matrix<double,-1,3,0,-1,3> >(class Eigen::MatrixBase<class Eigen::Matrix<double,-1,-1,0,-1,-1> > const &,class Eigen::MatrixBase<class Eigen::Matrix<int,-1,-1,0,-1,-1> > const &,class Eigen::MatrixBase<class Eigen::Matrix<double,-1,-1,0,-1,-1> > const &,class Eigen::PlainObjectBase<class Eigen::Matrix<double,-1,1,0,-1,1> > &,class Eigen::PlainObjectBase<class Eigen::Matrix<__int64,-1,1,0,-1,1> > &,class Eigen::PlainObjectBase<class Eigen::Matrix<double,-1,3,0,-1,3> > &)const;
#endif
#endif
