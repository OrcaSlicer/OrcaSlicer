#include "hausdorff.h"
#include "../../hausdorff.h"
#include <functional>

template <
  typename DerivedV,
  typename Kernel,
  typename Scalar>
IGL_INLINE void igl::copyleft::cgal::hausdorff(
  const Eigen::MatrixBase<DerivedV>& V,
  const CGAL::AABB_tree<
    CGAL::AABB_traits<Kernel, 
      CGAL::AABB_triangle_primitive<Kernel, 
        typename std::vector<CGAL::Triangle_3<Kernel> >::iterator
      >
    >
  > & treeB,
  const std::vector<CGAL::Triangle_3<Kernel> > & /*TB*/,
  Scalar & l,
  Scalar & u)
{
  // Not sure why using `auto` here doesn't work with the `hausdorff` function
  // parameter but explicitly naming the type does...
  const std::function<double(const double &,const double &,const double &)> 
    dist_to_B = [&treeB](
    const double & x, const double & y, const double & z)->double
  {
    CGAL::Point_3<Kernel> query(x,y,z);
    typename CGAL::AABB_tree<
      CGAL::AABB_traits<Kernel, 
        CGAL::AABB_triangle_primitive<Kernel, 
          typename std::vector<CGAL::Triangle_3<Kernel> >::iterator
        >
      >
    >::Point_and_primitive_id pp = treeB.closest_point_and_primitive(query);
    return std::sqrt((query-pp.first).squared_length());
  };
  return igl::hausdorff(V,dist_to_B,l,u);
}
