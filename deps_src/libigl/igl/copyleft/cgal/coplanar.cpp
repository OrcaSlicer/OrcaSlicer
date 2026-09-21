#include "coplanar.h"
#include "row_to_point.h"
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Point_3.h>

template <typename DerivedV>
IGL_INLINE bool igl::copyleft::cgal::coplanar(
  const Eigen::MatrixBase<DerivedV> & V)
{
  // 3 points in 3D are always coplanar
  if(V.rows() < 4){ return true; }
  // spanning points found so far
  std::vector<CGAL::Point_3<CGAL::Epick> > p;
  for(int i = 0;i<V.rows();i++)
  {
    const CGAL::Point_3<CGAL::Epick> pi(V(i,0), V(i,1), V(i,2));
    switch(p.size())
    {
      case 0:
        p.push_back(pi);
        break;
      case 1:
        if(p[0] != pi)
        {
          p.push_back(pi);
        }
        break;
      case 2:
        if(!CGAL::collinear(p[0],p[1],pi))
        {
          p.push_back(pi);
        }
        break;
      case 3:
        if(!CGAL::coplanar(p[0],p[1],p[2],pi))
        {
          return false;
        }
        break;
    }
  }
  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::copyleft::cgal::coplanar<Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&);
#endif
