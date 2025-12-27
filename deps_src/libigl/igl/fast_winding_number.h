#ifndef IGL_FAST_WINDING_NUMBER
#define IGL_FAST_WINDING_NUMBER
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  // Generate the precomputation for the fast winding number for point data
  // [Barill et. al 2018].
  //
  // Given a set of 3D points P, with normals N, areas A, along with octree
  // data, and an expansion order, we define a taylor series expansion at each
  // octree cell.
  //
  // The octree data is designed to come from igl::octree, and the areas
  // (if not obtained at scan time), may be calculated using
  // igl::copyleft::cgal::point_areas.
  //
  // Inputs:
  //   P  #P by 3 list of point locations
  //   N  #P by 3 list of point normals
  //   A  #P by 1 list of point areas
  //   point_indices  a vector of vectors, where the ith entry is a vector of
  //                  the indices into P that are the ith octree cell's points
  //   CH             #OctreeCells by 8, where the ith row is the indices of
  //                  the ith octree cell's children
  //   expansion_order    the order of the taylor expansion. We support 0,1,2.
  // Outputs:
  //   CM  #OctreeCells by 3 list of each cell's center of mass
  //   R   #OctreeCells by 1 list of each cell's maximum distance of any point
  //       to the center of mass
  //   EC  #OctreeCells by #TaylorCoefficients list of expansion coefficients.
  //       (Note that #TaylorCoefficients = ∑_{i=1}^{expansion_order} 3^i)
  //
  template <typename DerivedP, typename DerivedA, typename DerivedN,
    typename Index, typename DerivedCH, typename DerivedCM, typename DerivedR,
    typename DerivedEC>
  IGL_INLINE void fast_winding_number(const Eigen::MatrixBase<DerivedP>& P,
                                      const Eigen::MatrixBase<DerivedN>& N,
                                      const Eigen::MatrixBase<DerivedA>& A,
            const std::vector<std::vector<Index> > & point_indices,
            const Eigen::MatrixBase<DerivedCH>& CH,
            const int exansion_order,
            Eigen::PlainObjectBase<DerivedCM>& CM,
            Eigen::PlainObjectBase<DerivedR>& R,
            Eigen::PlainObjectBase<DerivedEC>& EC);
  
  // Evaluate the fast winding number for point data, having already done the
  // the precomputation
  //
  // Inputs:
  //   P  #P by 3 list of point locations
  //   N  #P by 3 list of point normals
  //   A  #P by 1 list of point areas
  //   point_indices  a vector of vectors, where the ith entry is a vector of
  //                  the indices into P that are the ith octree cell's points
  //   CH  #OctreeCells by 8, where the ith row is the indices of
  //       the ith octree cell's children
  //   CM  #OctreeCells by 3 list of each cell's center of mass
  //   R   #OctreeCells by 1 list of each cell's maximum distance of any point
  //       to the center of mass
  //   EC  #OctreeCells by #TaylorCoefficients list of expansion coefficients.
  //        (Note that #TaylorCoefficients = ∑_{i=1}^{expansion_order} 3^i)
  //   Q  #Q by 3 list of query points for the winding number
  //   beta  This is a Barnes-Hut style accuracy term that separates near feild
  //         from far field. The higher the beta, the more accurate and slower
  //         the evaluation. We reccommend using a beta value of 2. Note that
  //         for a beta value ≤ 0, we use the direct evaluation, rather than
  //         the fast approximation
  // Outputs:
  //   WN  #Q by 1 list of windinng number values at each query point
  //
  template <typename DerivedP, typename DerivedA, typename DerivedN,
    typename Index, typename DerivedCH, typename DerivedCM, typename DerivedR,
    typename DerivedEC, typename DerivedQ, typename BetaType,
    typename DerivedWN>
  IGL_INLINE void fast_winding_number(const Eigen::MatrixBase<DerivedP>& P,
                                      const Eigen::MatrixBase<DerivedN>& N,
                                      const Eigen::MatrixBase<DerivedA>& A,
            const std::vector<std::vector<Index> > & point_indices,
            const Eigen::MatrixBase<DerivedCH>& CH,
            const Eigen::MatrixBase<DerivedCM>& CM,
            const Eigen::MatrixBase<DerivedR>& R,
            const Eigen::MatrixBase<DerivedEC>& EC,
            const Eigen::MatrixBase<DerivedQ>& Q,
            const BetaType beta,
            Eigen::PlainObjectBase<DerivedWN>& WN);
  
  // Evaluate the fast winding number for point data, with default expansion
  // order and beta (both are set to 2).
  //
  // This function performes the precomputation and evaluation all in one.
  // If you need to acess the precomuptation for repeated evaluations, use the
  // two functions designed for exposed precomputation (described above).
  //
  // Inputs:
  //   P  #P by 3 list of point locations
  //   N  #P by 3 list of point normals
  //   A  #P by 1 list of point areas
  //   Q  #Q by 3 list of query points for the winding number
  //   beta  This is a Barnes-Hut style accuracy term that separates near feild
  //         from far field. The higher the beta, the more accurate and slower
  //         the evaluation. We reccommend using a beta value of 2.
  //   expansion_order    the order of the taylor expansion. We support 0,1,2.
  // Outputs:
  //   WN  #Q by 1 list of windinng number values at each query point
  //
  template <typename DerivedP, typename DerivedA, typename DerivedN,
    typename DerivedQ, typename DerivedWN>
  IGL_INLINE void fast_winding_number(const Eigen::MatrixBase<DerivedP>& P,
                                      const Eigen::MatrixBase<DerivedN>& N,
                                      const Eigen::MatrixBase<DerivedA>& A,
                                      const Eigen::MatrixBase<DerivedQ>& Q,
                                      Eigen::PlainObjectBase<DerivedWN>& WN
                                      );
}
#ifndef IGL_STATIC_LIBRARY
#  include "fast_winding_number.cpp"
#endif

#endif

