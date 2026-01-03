// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// Copyright (C) 2021 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_ASSIGN_SCALAR_H
#define IGL_COPYLEFT_CGAL_ASSIGN_SCALAR_H
#include "../../igl_inline.h"
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel_with_sqrt.h>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Conduct the casting copy:
      ///   lhs = rhs
      /// using `slow_and_more_precise` rounding if more desired.
      ///
      /// @tparam RHS right-hand side scalar type
      /// @tparam LHS left-hand side scalar type
      /// @param[in] rhs  right-hand side scalar
      /// @param[in] slow_and_more_precise  when appropriate use more elaborate rounding
      ///     guaranteed to find a closest lhs value in an absolute value sense.
      ///     Think of `slow_and_more_precise=true` as "round to closest number"
      ///     and `slow_and_more_precise=false` as "round down/up". CGAL's number
      ///     types are bit mysterious about how exactly rounding is conducted.
      ///     For example, the rationals created during remesh_intersections on
      ///     floating point input appear to be tightly rounded up or down so the
      ///     difference with the `slow_and_more_precise=true` will be exactly
      ///     zero 50% of the time and "one floating point unit" (at whatever
      ///     scale) the other 50% of the time.
      /// @param[out] lhs  left-hand side scalar
      template <typename RHS, typename LHS>
      IGL_INLINE void assign_scalar(
        const RHS & rhs,
        const bool & slow_and_more_precise,
        LHS & lhs);
      /// \overload
      /// \brief For legacy reasons, all of these overload uses
      /// `slow_and_more_precise=true`. This is subject to change if we determine
      /// it is sufficiently overkill. In that case, we'd create a new
      /// non-overloaded function.
      IGL_INLINE void assign_scalar(
        const CGAL::Epeck::FT & cgal,
        CGAL::Epeck::FT & d);
      /// \overload
      IGL_INLINE void assign_scalar(
        const CGAL::Epeck::FT & cgal,
        double & d);
      /// \overload
      IGL_INLINE void assign_scalar(
      /// \overload
        const CGAL::Epeck::FT & cgal,
        float& d);
      IGL_INLINE void assign_scalar(
      /// \overload
        const double & c,
        double & d);
      /// \overload
      IGL_INLINE void assign_scalar(
        const float& c,
        float & d);
      /// \overload
      IGL_INLINE void assign_scalar(
        const float& c,
        double& d);
      /// \overload
      IGL_INLINE void assign_scalar(
        const CGAL::Exact_predicates_exact_constructions_kernel_with_sqrt::FT & cgal,
        CGAL::Exact_predicates_exact_constructions_kernel_with_sqrt::FT & d);
      /// \overload
      IGL_INLINE void assign_scalar(
        const CGAL::Exact_predicates_exact_constructions_kernel_with_sqrt::FT & cgal,
        double & d);
      /// \overload
      IGL_INLINE void assign_scalar(
        const CGAL::Exact_predicates_exact_constructions_kernel_with_sqrt::FT & cgal,
        float& d);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "assign_scalar.cpp"
#endif
#endif
