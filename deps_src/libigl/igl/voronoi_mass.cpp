#include "voronoi_mass.h"
#include "circumradius.h"
#include "centroid.h"
#include "unique_simplices.h"

template <
  typename DerivedV,
  typename DerivedT,
  typename DerivedM>
void igl::voronoi_mass(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedT> & T,
    Eigen::PlainObjectBase<DerivedM> & M)
{
  // DerivedM::Scalar must be same as DerivedV::Scalar
  static_assert(std::is_same<typename DerivedM::Scalar, typename DerivedV::Scalar>::value, "DerivedM::Scalar must be same as DerivedV::Scalar");
  // DerivedM must be a vector (rows or cols at compile time = 1)
  static_assert(DerivedM::RowsAtCompileTime == 1 || DerivedM::ColsAtCompileTime == 1, "DerivedM must be a vector (rows or cols at compile time = 1)");
  // DerivedT must have Dynamic or 4 cols
  static_assert(DerivedT::ColsAtCompileTime == Eigen::Dynamic || DerivedT::ColsAtCompileTime == 4, "DerivedT must have Dynamic or 4 cols");
  assert(T.cols() == 4 && "Tetrahedra should have 4 vertices");
  // DerivedV must have Dynamic or 3 cols
  static_assert(DerivedV::ColsAtCompileTime == Eigen::Dynamic || DerivedV::ColsAtCompileTime == 3, "DerivedV must have Dynamic or 3 cols");
  assert(V.cols() == 3 && "V should have 3 columns");


  using Scalar = typename DerivedV::Scalar;
  using VectorXS = Eigen::Matrix<Scalar,Eigen::Dynamic,1>;
  using MatrixX3S = Eigen::Matrix<Scalar,Eigen::Dynamic,3>;
  using MatrixX4S = Eigen::Matrix<Scalar,Eigen::Dynamic,4>;
  using MatrixX3I = Eigen::Matrix<int,Eigen::Dynamic,3>;
  using MatrixX4I = Eigen::Matrix<int,Eigen::Dynamic,4>;
  MatrixX3I F;
  MatrixX4I I;
  {
    MatrixX3I allF(T.rows()*T.cols(),3);
    for(int i = 0;i<T.rows();i++)
    {
      for(int j = 0;j<T.cols();j++)
      {
        allF.row(j*T.rows()+i) <<
          T(i,(j+1)%T.cols()),
          T(i,(j+2)%T.cols()),
          T(i,(j+3)%T.cols());
      }
    }
    Eigen::VectorXi _;
    Eigen::VectorXi Ivec;
    igl::unique_simplices(allF,F,_,Ivec);
    I = Ivec.reshaped(T.rows(),T.cols());
  }

  // Face circumcenters
  MatrixX3S CF;
  {
    VectorXS R;
    MatrixX3S B;
    circumradius(V,F,R,CF,B);
    for(int i = 0;i<F.rows();i++)
    {
      int j;
      // Snap to edge "circumcenter"
      if(B.row(i).minCoeff(&j) < 0)
      {
        CF.row(i) = (V.row(F(i,(j+1)%3)) + V.row(F(i,(j+2)%3)))*0.5;
      }
    }
  }
  MatrixX3S CT;
  {
    VectorXS R;
    MatrixX4S B;
    circumradius(V,T,R,CT,B);
    for(int i = 0;i<T.rows();i++)
    {
      int j;
      // Snap to face "circumcenter"
      if(B.row(i).minCoeff(&j) < 0)
      {
        CT.row(i) = CF.row(I(i,j));
      }
    }
  }

  M.setZero(V.rows());
  for(int i = 0;i<T.rows();i++)
  {
    for(int j = 0;j<4;j++)
    {
      Eigen::Matrix<Scalar,8,3> U(8,V.cols());
      U.row(0) = V.row(T(i,j));
      // edge circumcenters
      for(int k = 1;k<4;k++)
      {
        U.row(k) = 0.5*(U.row(0) + V.row(T(i,(j+k)%4)));
      }
      // face circumcenters
      {
        U.block(4,0,3,V.cols()) <<
          CF.row(I(i,(j+1)%4)),
          CF.row(I(i,(j+2)%4)),
          CF.row(I(i,(j+3)%4));
      }
      // Tet circumcenter
      {
        U.row(7) = CT.row(i);
      }
      Eigen::Matrix<int,12,3> Fij(12,3);
      Fij<< 
        4,2,0,
        5,3,0,
        6,1,0,
        7,2,4,
        7,3,5,
        7,1,6,
        4,0,3,
        5,0,1,
        6,0,2,
        7,4,3,
        7,5,1,
        7,6,2;
      if(j%2)
      {
        Fij = Fij.rowwise().reverse().eval();
      }
      Scalar vol;
      {
        Eigen::Matrix<Scalar,1,3> cen;
        igl::centroid(U,Fij,cen,vol);
      }
      M(T(i,j)) += vol;
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::voronoi_mass<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::voronoi_mass<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::voronoi_mass<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::voronoi_mass<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 4, 0, -1, 4>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 4, 0, -1, 4> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
#endif
