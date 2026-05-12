#include "is_self_intersecting.h"
#include "../../placeholders.h"
#include "../../find.h"
#include "../../doublearea.h"
#include "../../remove_unreferenced.h"
#include "RemeshSelfIntersectionsParam.h"
#include "remesh_self_intersections.h"
#include "../../collapse_edge.h"

template <
  typename DerivedV,
  typename DerivedF>
bool igl::copyleft::cgal::is_self_intersecting(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F)
{
  assert(V.cols() == 3);
  assert(F.cols() == 3);
  using MatrixV = Eigen::Matrix<typename DerivedV::Scalar, Eigen::Dynamic, 3>;
  using MatrixF = Eigen::Matrix<typename DerivedF::Scalar, Eigen::Dynamic, 3>;
  const auto valid = 
    igl::find((F.array() != IGL_COLLAPSE_EDGE_NULL).rowwise().any().eval());
  // Extract only the valid faces
  MatrixF FF = F(valid, igl::placeholders::all);
  // Remove unreferneced vertices
  MatrixV VV;
  {
    Eigen::VectorXi I;
    igl::remove_unreferenced(V,MatrixF(FF),VV,FF,I);
  }
  Eigen::Matrix<typename DerivedV::Scalar,Eigen::Dynamic,1> A;
  igl::doublearea(VV,FF,A);
  if(A.minCoeff() <= 0)
  {
    return true;
  }
  if(
       (FF.array().col(0) == FF.array().col(1)).any() ||
       (FF.array().col(1) == FF.array().col(2)).any() ||
       (FF.array().col(2) == FF.array().col(0)).any())

  {
    return true;
  }

  // check for self-intersections VV,FF
  igl::copyleft::cgal::RemeshSelfIntersectionsParam params;
  params.detect_only = true;
  params.first_only = true;
  Eigen::MatrixXi IF;
  Eigen::VectorXi J,IM;
  {
    MatrixV tempV;
    MatrixF tempF;
    igl::copyleft::cgal::remesh_self_intersections(
      V,F,params,tempV,tempF,IF,J,IM);
  }
  return IF.rows() > 0;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template specialization
template bool igl::copyleft::cgal::is_self_intersecting<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
#endif
