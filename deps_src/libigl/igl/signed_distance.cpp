// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "signed_distance.h"
#include "get_seconds.h"
#include "per_edge_normals.h"
#include "parallel_for.h"
#include "per_face_normals.h"
#include "per_vertex_normals.h"
#include "point_mesh_squared_distance.h"
#include "pseudonormal_test.h"


template <
  typename DerivedP,
  typename DerivedV,
  typename DerivedF,
  typename DerivedS,
  typename DerivedI,
  typename DerivedC,
  typename DerivedN>
IGL_INLINE void igl::signed_distance(
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const SignedDistanceType sign_type,
  const typename DerivedV::Scalar lower_bound,
  const typename DerivedV::Scalar upper_bound,
  Eigen::PlainObjectBase<DerivedS> & S,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedC> & C,
  Eigen::PlainObjectBase<DerivedN> & N)
{
  using namespace Eigen;
  using namespace std;
  const int dim = V.cols();
  assert((V.cols() == 3||V.cols() == 2) && "V should have 3d or 2d positions");
  assert((P.cols() == 3||P.cols() == 2) && "P should have 3d or 2d positions");
  assert(V.cols() == P.cols() && "V should have same dimension as P");
  // Only unsigned distance is supported for non-triangles
  if(sign_type != SIGNED_DISTANCE_TYPE_UNSIGNED)
  {
    assert(F.cols() == dim && "F should have co-dimension 0 simplices");
  }
  typedef Eigen::Matrix<typename DerivedV::Scalar,1,3> RowVector3S;

  // Prepare distance computation
  AABB<DerivedV,3> tree3;
  AABB<DerivedV,2> tree2;
  switch(dim)
  {
    default:
    case 3:
      tree3.init(V,F);
      break;
    case 2:
      tree2.init(V,F);
      break;
  }

  Eigen::Matrix<typename DerivedV::Scalar,Eigen::Dynamic,3> FN,VN,EN;
  Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,2> E;
  Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,1> EMAP;
  WindingNumberAABB<RowVector3S,DerivedV,DerivedF> hier3;
  switch(sign_type)
  {
    default:
      assert(false && "Unknown SignedDistanceType");
    case SIGNED_DISTANCE_TYPE_UNSIGNED:
      // do nothing
      break;
    case SIGNED_DISTANCE_TYPE_DEFAULT:
    case SIGNED_DISTANCE_TYPE_WINDING_NUMBER:
      switch(dim)
      {
        default:
        case 3:
          hier3.set_mesh(V,F);
          hier3.grow();
          break;
        case 2:
          // no precomp, no hierarchy
          break;
      }
      break;
    case SIGNED_DISTANCE_TYPE_PSEUDONORMAL:
      switch(dim)
      {
        default:
        case 3:
          // "Signed Distance Computation Using the Angle Weighted Pseudonormal"
          // [Bærentzen & Aanæs 2005]
          per_face_normals(V,F,FN);
          per_vertex_normals(V,F,PER_VERTEX_NORMALS_WEIGHTING_TYPE_ANGLE,FN,VN);
          per_edge_normals(
            V,F,PER_EDGE_NORMALS_WEIGHTING_TYPE_UNIFORM,FN,EN,E,EMAP);
          break;
        case 2:
          FN.resize(F.rows(),2);
          VN = DerivedV::Zero(V.rows(),2);
          for(int e = 0;e<F.rows();e++)
          {
            // rotate edge vector
            FN(e,0) =  (V(F(e,1),1)-V(F(e,0),1));
            FN(e,1) = -(V(F(e,1),0)-V(F(e,0),0));
            FN.row(e).normalize();
            // add to vertex normal
            VN.row(F(e,1)) += FN.row(e);
            VN.row(F(e,0)) += FN.row(e);
          }
          // normalize to average
          VN.rowwise().normalize();
          break;
      }
      N.resize(P.rows(),dim);
      break;
  }
  //
  // convert to bounds on (unsiged) squared distances
  typedef typename DerivedV::Scalar Scalar; 
  const Scalar max_abs = std::max(std::abs(lower_bound),std::abs(upper_bound));
  const Scalar up_sqr_d = std::pow(max_abs,2.0);
  const Scalar low_sqr_d = 
    std::pow(std::max(max_abs-(upper_bound-lower_bound),(Scalar)0.0),2.0);

  S.resize(P.rows(),1);
  I.resize(P.rows(),1);
  C.resize(P.rows(),dim);

  parallel_for(P.rows(),[&](const int p)
  //for(int p = 0;p<P.rows();p++)
  {
    RowVector3S q3;
    Eigen::Matrix<typename DerivedV::Scalar,1,2>  q2;
    switch(P.cols())
    {
      default:
      case 3:
        q3.head(P.row(p).size()) = P.row(p);
        break;
      case 2:
        q2 = P.row(p).head(2);
        break;
    }
    typename DerivedV::Scalar s=1,sqrd=0;
    Eigen::Matrix<typename DerivedV::Scalar,1,Eigen::Dynamic>  c;
    RowVector3S c3;
    Eigen::Matrix<typename DerivedV::Scalar,1,2>  c2;
    int i=-1;
    // in all cases compute squared unsiged distances
    sqrd = dim==3?
      tree3.squared_distance(V,F,q3,low_sqr_d,up_sqr_d,i,c3):
      tree2.squared_distance(V,F,q2,low_sqr_d,up_sqr_d,i,c2);
    if(sqrd >= up_sqr_d || sqrd <= low_sqr_d)
    {
      // Out of bounds gets a nan (nans on grids can be flood filled later using
      // igl::flood_fill)
      S(p) = std::numeric_limits<double>::quiet_NaN();
      I(p) = F.rows()+1;
      C.row(p).setConstant(0);
    }else
    {
      // Determine sign
      switch(sign_type)
      {
        default:
          assert(false && "Unknown SignedDistanceType");
        case SIGNED_DISTANCE_TYPE_UNSIGNED:
          break;
        case SIGNED_DISTANCE_TYPE_DEFAULT:
        case SIGNED_DISTANCE_TYPE_WINDING_NUMBER:
        {
          Scalar w = 0;
          if(dim == 3)
          {
            s = 1.-2.*hier3.winding_number(q3.transpose());
          }else
          {
            assert(!V.derived().IsRowMajor);
            assert(!F.derived().IsRowMajor);
            s = 1.-2.*winding_number(V,F,q2);
          }
          break;
        }
        case SIGNED_DISTANCE_TYPE_PSEUDONORMAL:
        {
          RowVector3S n3;
          Eigen::Matrix<typename DerivedV::Scalar,1,2>  n2;
          dim==3 ?
            pseudonormal_test(V,F,FN,VN,EN,EMAP,q3,i,c3,s,n3):
            pseudonormal_test(V,E,EN,VN,q2,i,c2,s,n2);
          Eigen::Matrix<typename DerivedV::Scalar,1,Eigen::Dynamic>  n;
          (dim==3 ? n = n3 : n = n2);
          N.row(p) = n;
          break;
        }
      }
      I(p) = i;
      S(p) = s*sqrt(sqrd);
      C.row(p) = (dim==3 ? c=c3 : c=c2);
    }
  }
  ,10000);
}

template <
  typename DerivedP,
  typename DerivedV,
  typename DerivedF,
  typename DerivedS,
  typename DerivedI,
  typename DerivedC,
  typename DerivedN>
IGL_INLINE void igl::signed_distance(
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const SignedDistanceType sign_type,
  Eigen::PlainObjectBase<DerivedS> & S,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedC> & C,
  Eigen::PlainObjectBase<DerivedN> & N)
{
  typedef typename DerivedV::Scalar Scalar;
  Scalar lower = std::numeric_limits<Scalar>::min();
  Scalar upper = std::numeric_limits<Scalar>::max();
  return signed_distance(P,V,F,sign_type,lower,upper,S,I,C,N);
}


template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedFN,
  typename DerivedVN,
  typename DerivedEN,
  typename DerivedEMAP,
  typename Derivedq>
IGL_INLINE typename DerivedV::Scalar igl::signed_distance_pseudonormal(
  const AABB<DerivedV,3> & tree,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedFN> & FN,
  const Eigen::MatrixBase<DerivedVN> & VN,
  const Eigen::MatrixBase<DerivedEN> & EN,
  const Eigen::MatrixBase<DerivedEMAP> & EMAP,
  const Eigen::MatrixBase<Derivedq> & q)
{
  typename DerivedV::Scalar s,sqrd;
  Eigen::Matrix<typename DerivedV::Scalar,1,3> n,c;
  int i = -1;
  signed_distance_pseudonormal(tree,V,F,FN,VN,EN,EMAP,q,s,sqrd,i,c,n);
  return s*sqrt(sqrd);
}

template <
  typename DerivedP,
  typename DerivedV,
  typename DerivedF,
  typename DerivedFN,
  typename DerivedVN,
  typename DerivedEN,
  typename DerivedEMAP,
  typename DerivedS,
  typename DerivedI,
  typename DerivedC,
  typename DerivedN>
IGL_INLINE void igl::signed_distance_pseudonormal(
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const AABB<DerivedV,3> & tree,
  const Eigen::MatrixBase<DerivedFN> & FN,
  const Eigen::MatrixBase<DerivedVN> & VN,
  const Eigen::MatrixBase<DerivedEN> & EN,
  const Eigen::MatrixBase<DerivedEMAP> & EMAP,
  Eigen::PlainObjectBase<DerivedS> & S,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedC> & C,
  Eigen::PlainObjectBase<DerivedN> & N)
{
  using namespace Eigen;
  const size_t np = P.rows();
  S.resize(np,1);
  I.resize(np,1);
  N.resize(np,3);
  C.resize(np,3);
  typedef typename AABB<DerivedV,3>::RowVectorDIMS RowVector3S;
# pragma omp parallel for if(np>1000)
  for(size_t p = 0;p<np;p++)
  {
    typename DerivedV::Scalar s,sqrd;
    RowVector3S n,c;
    int i = -1;
    RowVector3S q = P.row(p);
    signed_distance_pseudonormal(tree,V,F,FN,VN,EN,EMAP,q,s,sqrd,i,c,n);
    S(p) = s*sqrt(sqrd);
    I(p) = i;
    N.row(p) = n;
    C.row(p) = c;
  }
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedFN,
  typename DerivedVN,
  typename DerivedEN,
  typename DerivedEMAP,
  typename Derivedq,
  typename Scalar,
  typename Derivedc,
  typename Derivedn>
IGL_INLINE void igl::signed_distance_pseudonormal(
  const AABB<DerivedV,3> & tree,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedFN> & FN,
  const Eigen::MatrixBase<DerivedVN> & VN,
  const Eigen::MatrixBase<DerivedEN> & EN,
  const Eigen::MatrixBase<DerivedEMAP> & EMAP,
  const Eigen::MatrixBase<Derivedq> & q,
  Scalar & s,
  Scalar & sqrd,
  int & i,
  Eigen::PlainObjectBase<Derivedc> & c,
  Eigen::PlainObjectBase<Derivedn> & n)
{
  using namespace Eigen;
  using namespace std;
  //typedef Eigen::Matrix<typename DerivedV::Scalar,1,3> RowVector3S;
  // Alec: Why was this constructor around q necessary?
  //sqrd = tree.squared_distance(V,F,RowVector3S(q),i,(RowVector3S&)c);
  // Alec: Why was this constructor around c necessary?
  //sqrd = tree.squared_distance(V,F,q,i,(RowVector3S&)c);
  sqrd = tree.squared_distance(V,F,q,i,c);
  pseudonormal_test(V,F,FN,VN,EN,EMAP,q,i,c,s,n);
}

template <
  typename DerivedV,
  typename DerivedE,
  typename DerivedEN,
  typename DerivedVN,
  typename Derivedq,
  typename Scalar,
  typename Derivedc,
  typename Derivedn>
IGL_INLINE void igl::signed_distance_pseudonormal(
  const AABB<DerivedV,2> & tree,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedE> & E,
  const Eigen::MatrixBase<DerivedEN> & EN,
  const Eigen::MatrixBase<DerivedVN> & VN,
  const Eigen::MatrixBase<Derivedq> & q,
  Scalar & s,
  Scalar & sqrd,
  int & i,
  Eigen::PlainObjectBase<Derivedc> & c,
  Eigen::PlainObjectBase<Derivedn> & n)
{
  using namespace Eigen;
  using namespace std;
  typedef Eigen::Matrix<typename DerivedV::Scalar,1,2> RowVector2S;
  sqrd = tree.squared_distance(V,E,RowVector2S(q),i,(RowVector2S&)c);
  pseudonormal_test(V,E,EN,VN,q,i,c,s,n);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename Derivedq>
IGL_INLINE typename DerivedV::Scalar igl::signed_distance_winding_number(
  const AABB<DerivedV,3> & tree,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const igl::WindingNumberAABB<Derivedq,DerivedV,DerivedF> & hier,
  const Eigen::MatrixBase<Derivedq> & q)
{
  typedef typename DerivedV::Scalar Scalar;
  Scalar s,sqrd;
  Eigen::Matrix<Scalar,1,3> c;
  int i=-1;
  signed_distance_winding_number(tree,V,F,hier,q,s,sqrd,i,c);
  return s*sqrt(sqrd);
}


template <
  typename DerivedV,
  typename DerivedF,
  typename Derivedq,
  typename Scalar,
  typename Derivedc>
IGL_INLINE void igl::signed_distance_winding_number(
  const AABB<DerivedV,3> & tree,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const igl::WindingNumberAABB<Derivedq,DerivedV,DerivedF> & hier,
  const Eigen::MatrixBase<Derivedq> & q,
  Scalar & s,
  Scalar & sqrd,
  int & i,
  Eigen::PlainObjectBase<Derivedc> & c)
{
  using namespace Eigen;
  using namespace std;
  typedef Eigen::Matrix<typename DerivedV::Scalar,1,3> RowVector3S;
  sqrd = tree.squared_distance(V,F,RowVector3S(q),i,(RowVector3S&)c);
  const Scalar w = hier.winding_number(q.transpose());
  s = 1.-2.*w;
}

template <
  typename DerivedV,
  typename DerivedF,
  typename Derivedq,
  typename Scalar,
  typename Derivedc>
IGL_INLINE void igl::signed_distance_winding_number(
  const AABB<DerivedV,2> & tree,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<Derivedq> & q,
  Scalar & s,
  Scalar & sqrd,
  int & i,
  Eigen::PlainObjectBase<Derivedc> & c)
{
  using namespace Eigen;
  using namespace std;
  typedef Eigen::Matrix<typename DerivedV::Scalar,1,2> RowVector2S;
  sqrd = tree.squared_distance(V,F,RowVector2S(q),i,(RowVector2S&)c);
  Scalar w;
  // TODO: using .data() like this is very dangerous... This is assuming
  // colmajor order
  assert(!V.derived().IsRowMajor);
  assert(!F.derived().IsRowMajor);
  s = 1.-2.*winding_number(V,F,q);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::signed_distance<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, igl::SignedDistanceType, Eigen::Matrix<double, -1, 3, 1, -1, 3>::Scalar, Eigen::Matrix<double, -1, 3, 1, -1, 3>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
// generated by autoexplicit.sh
template void igl::signed_distance<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, igl::SignedDistanceType, Eigen::Matrix<float, -1, 3, 0, -1, 3>::Scalar, Eigen::Matrix<float, -1, 3, 0, -1, 3>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&);
// generated by autoexplicit.sh
template void igl::signed_distance<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, igl::SignedDistanceType, Eigen::Matrix<float, -1, 3, 1, -1, 3>::Scalar, Eigen::Matrix<float, -1, 3, 1, -1, 3>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&);
template void igl::signed_distance<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<float, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, igl::SignedDistanceType, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&);
template void igl::signed_distance_pseudonormal<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, double&, double&, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::signed_distance<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::SignedDistanceType, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar igl::signed_distance_pseudonormal<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&);
template void igl::signed_distance_pseudonormal<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::signed_distance<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::SignedDistanceType, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::signed_distance_winding_number<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, double, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::WindingNumberAABB<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, double&, double&, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar igl::signed_distance_winding_number<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 3, 1, 0, 3, 1> >(igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::WindingNumberAABB<Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> > const&);
#endif
