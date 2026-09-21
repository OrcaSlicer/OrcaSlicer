#ifndef IGL_QUADPROG_H
#define IGL_QUADPROG_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Solve a convex quadratic program. Optimized for small dense problems.
  /// Still works for Eigen::Dynamic (and then everything needs to be Dynamic).
  ///
  ///     min_x ½ xᵀ H x + xᵀf
  ///     subject to:
  ///       lbi ≤ Ai x ≤ ubi
  ///       lb ≤ x ≤ u
  ///
  /// @tparam Scalar  (e.g., double)
  /// @tparam n  #H or Eigen::Dynamic if not known at compile time
  /// @tparam ni  #Ai or Eigen::Dynamic if not known at compile time
  /// @param[in] H  #H by #H quadratic coefficients (only lower triangle used)
  /// @param[in] f  #H linear coefficients
  /// @param[in] Ai  #Ai by #H list of linear equality constraint coefficients
  /// @param[in] lbi  #Ai list of linear equality lower bounds
  /// @param[in] ubi  #Ai list of linear equality upper bounds
  /// @param[in] lb  #H list of lower bounds
  /// @param[in] ub  #H list of lower bounds
  /// @return #H-long solution x
  template <typename Scalar, int n, int ni>
  IGL_INLINE Eigen::Matrix<Scalar,n,1> quadprog(
    const Eigen::Matrix<Scalar,n,n> & H,
    const Eigen::Matrix<Scalar,n,1> & f,
    const Eigen::Matrix<Scalar,ni,n> & Ai,
    const Eigen::Matrix<Scalar,ni,1> & lbi,
    const Eigen::Matrix<Scalar,ni,1> & ubi,
    const Eigen::Matrix<Scalar,n,1> & lb,
    const Eigen::Matrix<Scalar,n,1> & ub);
  /// Solve a convex quadratic program. Optimized for small dense problems. All
  /// inequalities must be simple bounds.
  ///
  ///      min_x ½ xᵀ H x + xᵀf
  ///      subject to:
  ///        A x = b
  ///        lb ≤ x ≤ u
  ///
  /// @tparam Scalar  (e.g., double)
  /// @tparam n  #H or Eigen::Dynamic if not known at compile time
  /// @tparam m  #A or Eigen::Dynamic if not known at compile time
  /// @param[in] H  #H by #H quadratic coefficients (only lower triangle used)
  /// @param[in] f  #H linear coefficients
  /// @param[in] A  #A by #H list of linear equality constraint coefficients
  /// @param[in] b  #A list of linear equality lower bounds
  /// @param[in] lb  #H list of lower bounds
  /// @param[in] ub  #H list of lower bounds
  /// @return #H-long solution x
  template <typename Scalar, int n, int m>
  IGL_INLINE Eigen::Matrix<Scalar,n,1> quadprog(
    const Eigen::Matrix<Scalar,n,n> & H,
    const Eigen::Matrix<Scalar,n,1> & f,
    const Eigen::Matrix<Scalar,m,n> & A,
    const Eigen::Matrix<Scalar,m,1> & b,
    const Eigen::Matrix<Scalar,n,1> & lb,
    const Eigen::Matrix<Scalar,n,1> & ub);
  /// Solve a convex quadratic program. Optimized for small dense problems. All
  /// constraints must be simple bounds.
  ///
  ///      min_x ½ xᵀ H x + xᵀf
  ///      subject to:
  ///        lb ≤ x ≤ u
  ///
  /// @tparam Scalar  (e.g., double)
  /// @tparam n  #H or Eigen::Dynamic if not known at compile time
  /// @param[in] H  #H by #H quadratic coefficients (only lower triangle used)
  /// @param[in] f  #H linear coefficients
  /// @param[in] lb  #H list of lower bounds
  /// @param[in] ub  #H list of lower bounds
  /// @return #H-long solution x
  template <typename Scalar, int n>
  IGL_INLINE Eigen::Matrix<Scalar,n,1> quadprog(
    const Eigen::Matrix<Scalar,n,n> & H,
    const Eigen::Matrix<Scalar,n,1> & f,
    const Eigen::Matrix<Scalar,n,1> & lb,
    const Eigen::Matrix<Scalar,n,1> & ub);
}

#ifndef IGL_STATIC_LIBRARY
#  include "quadprog.cpp"
#endif 

#endif 
