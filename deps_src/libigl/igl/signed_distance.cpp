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
#include "fast_winding_number.h"

namespace
{
  template <
    typename DerivedP,
    typename DerivedV,
    typename DerivedF,
    typename DerivedS,
    typename DerivedI,
    typename DerivedC,
    typename DerivedN>
  void signed_distance_3(
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const igl::SignedDistanceType sign_type,
    const typename DerivedV::Scalar lower_bound,
    const typename DerivedV::Scalar upper_bound,
    Eigen::PlainObjectBase<DerivedS> & S,
    Eigen::PlainObjectBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedN> & N)
  {
    using namespace igl;
    AABB<DerivedV,3> tree;
    tree.init(V,F);
    Eigen::Matrix<typename DerivedV::Scalar,Eigen::Dynamic,3> FN,VN,EN;
    Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,2> E;
    Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,1> EMAP;
    typedef Eigen::Matrix<typename DerivedV::Scalar,1,3> RowVectorS;
    using Scalar = typename DerivedV::Scalar;
    using Index = typename DerivedF::Scalar;
    WindingNumberAABB<Scalar,Index> hier3;
    igl::FastWindingNumberBVH fwn_bvh;
    Eigen::VectorXf W;

    switch(sign_type)
    {
      default:
        assert(false && "Unknown SignedDistanceType");
      case SIGNED_DISTANCE_TYPE_UNSIGNED:
        // do nothing
        break;
      case SIGNED_DISTANCE_TYPE_DEFAULT:
      case SIGNED_DISTANCE_TYPE_WINDING_NUMBER:
        hier3.set_mesh(V,F);
        hier3.grow();
        break;
      case SIGNED_DISTANCE_TYPE_FAST_WINDING_NUMBER:
        igl::fast_winding_number(V.template cast<float>().eval(), F, 2, fwn_bvh);
        break;
      case SIGNED_DISTANCE_TYPE_PSEUDONORMAL:
         // "Signed Distance Computation Using the Angle Weighted Pseudonormal"
         // [Bærentzen & Aanæs 2005]
        igl::per_face_normals(V,F,FN);
        igl::per_vertex_normals(V,F,PER_VERTEX_NORMALS_WEIGHTING_TYPE_ANGLE,FN,VN);
        igl::per_edge_normals(
           V,F,PER_EDGE_NORMALS_WEIGHTING_TYPE_UNIFORM,FN,EN,E,EMAP);
        N.resize(P.rows(),3);
        break;
    }

    // convert to bounds on (unsiged) squared distances
    typedef typename DerivedV::Scalar Scalar; 
    const Scalar max_abs = std::max(std::abs(lower_bound),std::abs(upper_bound));
    const Scalar up_sqr_d = std::pow(max_abs,2.0);
    const Scalar low_sqr_d = 
      std::pow(std::max(max_abs-(upper_bound-lower_bound),(Scalar)0.0),2.0);

    S.resize(P.rows(),1);
    I.resize(P.rows(),1);
    C.resize(P.rows(),3);

    igl::parallel_for(P.rows(),[&](const int p)
    //for(int p = 0;p<P.rows();p++)
    {
      RowVectorS q = P.row(p);
      typename DerivedV::Scalar s=1,sqrd=0;
      RowVectorS c;
      int i=-1;
      // in all cases compute squared unsiged distances
      sqrd = tree.squared_distance(V,F,q,low_sqr_d,up_sqr_d,i,c);
      if(sqrd >= up_sqr_d || sqrd < low_sqr_d)
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
            s = 1.-2.*hier3.winding_number(q.transpose());
            break;
          case SIGNED_DISTANCE_TYPE_FAST_WINDING_NUMBER:
          {
            Scalar w = fast_winding_number(fwn_bvh, 2, q.template cast<float>().eval());         
            s = 1.-2.*std::abs(w);  
            break;
          }
          case SIGNED_DISTANCE_TYPE_PSEUDONORMAL:
          {
            RowVectorS n;
            pseudonormal_test(V,F,FN,VN,EN,EMAP,q,i,c,s,n);
            N.row(p) = n.template cast<typename DerivedN::Scalar>();
            break;
          }
        }
        I(p) = i;
        S(p) = s*sqrt(sqrd);
        C.row(p) = c.template cast<typename DerivedC::Scalar>();
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
  void signed_distance_2(
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const igl::SignedDistanceType sign_type,
    const typename DerivedV::Scalar lower_bound,
    const typename DerivedV::Scalar upper_bound,
    Eigen::PlainObjectBase<DerivedS> & S,
    Eigen::PlainObjectBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedN> & N)
  {
    using namespace igl;
    AABB<DerivedV,2> tree;
    tree.init(V,F);
    Eigen::Matrix<typename DerivedV::Scalar,Eigen::Dynamic,2> FN,VN,EN;
    Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,2> E;
    Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,1> EMAP;

    switch(sign_type)
    {
      default:
        assert(false && "Unknown or unsupported SignedDistanceType for 2D");
      case SIGNED_DISTANCE_TYPE_DEFAULT:
      case SIGNED_DISTANCE_TYPE_WINDING_NUMBER:
      case SIGNED_DISTANCE_TYPE_UNSIGNED:
        // no precomp for 2D
        break;
      case SIGNED_DISTANCE_TYPE_PSEUDONORMAL:
        // "Signed Distance Computation Using the Angle Weighted Pseudonormal"
        // [Bærentzen & Aanæs 2005]
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
        N.resize(P.rows(),2);
        break;
    }

    // convert to bounds on (unsiged) squared distances
    typedef typename DerivedV::Scalar Scalar; 
    const Scalar max_abs = std::max(std::abs(lower_bound),std::abs(upper_bound));
    const Scalar up_sqr_d = std::pow(max_abs,2.0);
    const Scalar low_sqr_d = 
      std::pow(std::max(max_abs-(upper_bound-lower_bound),(Scalar)0.0),2.0);

    S.resize(P.rows(),1);
    I.resize(P.rows(),1);
    C.resize(P.rows(),2);

    typedef Eigen::Matrix<typename DerivedV::Scalar,1,2> RowVectorS;
    igl::parallel_for(P.rows(),[&](const int p)
    //for(int p = 0;p<P.rows();p++)
    {
      RowVectorS q = P.row(p);
      typename DerivedV::Scalar s=1,sqrd=0;
      RowVectorS c;
      int i=-1;
      // in all cases compute squared unsiged distances
      sqrd = tree.squared_distance(V,F,q,low_sqr_d,up_sqr_d,i,c);
      if(sqrd >= up_sqr_d || sqrd < low_sqr_d)
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
            assert(!V.derived().IsRowMajor);
            assert(!F.derived().IsRowMajor);
            s = 1.-2.*winding_number(V,F,q);
            break;
          case SIGNED_DISTANCE_TYPE_PSEUDONORMAL:
          {
            RowVectorS n;
            pseudonormal_test(V,F,FN,VN,q,i,c,s,n);
            N.row(p) = n.template cast<typename DerivedN::Scalar>();
            break;
          }
        }
        I(p) = i;
        S(p) = s*sqrt(sqrd);
        C.row(p) = c.template cast<typename DerivedC::Scalar>();
      }
    }
    ,10000);
  }
  // Class whose templates can be specialized on whether all inputs have dynamic
  // columns or not.
  template <
    typename DerivedP,
    typename DerivedV,
    typename DerivedF,
    typename DerivedS,
    typename DerivedI,
    typename DerivedC,
    typename DerivedN,
    int ColsAtCompileTime>
      struct signed_distance_DIM_Handler;
  // All inputs have dynamic number of columns
  template <
    typename DerivedP,
    typename DerivedV,
    typename DerivedF,
    typename DerivedS,
    typename DerivedI,
    typename DerivedC,
    typename DerivedN>
      struct signed_distance_DIM_Handler<
      DerivedP, DerivedV, DerivedF, DerivedS, DerivedI, DerivedC, DerivedN,
      Eigen::Dynamic>
  {
    static void compute(
      const Eigen::MatrixBase<DerivedP> & P,
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      const igl::SignedDistanceType sign_type,
      const typename DerivedV::Scalar lower_bound,
      const typename DerivedV::Scalar upper_bound,
      Eigen::PlainObjectBase<DerivedS> & S,
      Eigen::PlainObjectBase<DerivedI> & I,
      Eigen::PlainObjectBase<DerivedC> & C,
      Eigen::PlainObjectBase<DerivedN> & N)
    {
      // Just need to check P as V and F are checked in signed_distance
      if(P.cols() == 3)
      {
        signed_distance_3(P,V,F,sign_type,lower_bound,upper_bound,S,I,C,N);
      }else if(P.cols() == 2)
      {
        signed_distance_2(P,V,F,sign_type,lower_bound,upper_bound,S,I,C,N);
      }else
      {
        assert(false && "P should have 3d or 2d positions");
      }
    }
  };
  // Some input is 3D
  template <
    typename DerivedP,
    typename DerivedV,
    typename DerivedF,
    typename DerivedS,
    typename DerivedI,
    typename DerivedC,
    typename DerivedN>
      struct signed_distance_DIM_Handler<
      DerivedP, DerivedV, DerivedF, DerivedS, DerivedI, DerivedC, DerivedN,
      3>
  {
    static void compute(
      const Eigen::MatrixBase<DerivedP> & P,
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      const igl::SignedDistanceType sign_type,
      const typename DerivedV::Scalar lower_bound,
      const typename DerivedV::Scalar upper_bound,
      Eigen::PlainObjectBase<DerivedS> & S,
      Eigen::PlainObjectBase<DerivedI> & I,
      Eigen::PlainObjectBase<DerivedC> & C,
      Eigen::PlainObjectBase<DerivedN> & N)
    {
      signed_distance_3(P,V,F,sign_type,lower_bound,upper_bound,S,I,C,N);
    }
  };
  // Some input is 2D
  template <
    typename DerivedP,
    typename DerivedV,
    typename DerivedF,
    typename DerivedS,
    typename DerivedI,
    typename DerivedC,
    typename DerivedN>
      struct signed_distance_DIM_Handler<
      DerivedP, DerivedV, DerivedF, DerivedS, DerivedI, DerivedC, DerivedN,
      2>
  {
    static void compute(
      const Eigen::MatrixBase<DerivedP> & P,
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      const igl::SignedDistanceType sign_type,
      const typename DerivedV::Scalar lower_bound,
      const typename DerivedV::Scalar upper_bound,
      Eigen::PlainObjectBase<DerivedS> & S,
      Eigen::PlainObjectBase<DerivedI> & I,
      Eigen::PlainObjectBase<DerivedC> & C,
      Eigen::PlainObjectBase<DerivedN> & N)
    {
      signed_distance_2(P,V,F,sign_type,lower_bound,upper_bound,S,I,C,N);
    }
  };

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
  const typename DerivedV::Scalar lower_bound,
  const typename DerivedV::Scalar upper_bound,
  Eigen::PlainObjectBase<DerivedS> & S,
  Eigen::PlainObjectBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedC> & C,
  Eigen::PlainObjectBase<DerivedN> & N)
{
  constexpr int DIM = 
    DerivedP::ColsAtCompileTime != Eigen::Dynamic ? DerivedP::ColsAtCompileTime :
    DerivedV::ColsAtCompileTime != Eigen::Dynamic ? DerivedV::ColsAtCompileTime :
    DerivedF::ColsAtCompileTime != Eigen::Dynamic ? DerivedF::ColsAtCompileTime :
    DerivedN::ColsAtCompileTime != Eigen::Dynamic ? DerivedN::ColsAtCompileTime :
    DerivedC::ColsAtCompileTime != Eigen::Dynamic ? DerivedC::ColsAtCompileTime :
    Eigen::Dynamic;
  static_assert(DIM == 3 || DIM == 2 || DIM == Eigen::Dynamic,"DIM should be 2 or 3 or Dynamic");
  assert(V.cols() == P.cols() && "V should have same dimension as P");
  assert(V.cols() == F.cols() || sign_type == SIGNED_DISTANCE_TYPE_UNSIGNED && "V and F should have same number of columns");
  if (sign_type == SIGNED_DISTANCE_TYPE_FAST_WINDING_NUMBER){
    assert(V.cols() == 3 && "V should be 3D for fast winding number");
  }

  if(F.rows() == 0)
  {
    S.setConstant(P.rows(),1,std::numeric_limits<typename DerivedS::Scalar>::quiet_NaN());
    I.setConstant(P.rows(),1,-1);
    C.setConstant(P.rows(),P.cols(),std::numeric_limits<typename DerivedC::Scalar>::quiet_NaN());
    N.setConstant(P.rows(),P.cols(),std::numeric_limits<typename DerivedC::Scalar>::quiet_NaN());
    return;
  }

  signed_distance_DIM_Handler<
    DerivedP,
    DerivedV,
    DerivedF,
    DerivedS,
    DerivedI,
    DerivedC,
    DerivedN,
    DIM>::compute(P,V,F,sign_type,lower_bound,upper_bound,S,I,C,N);
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
  parallel_for(np,[&](const int p)
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
  },1000);
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
  static_assert(
    DerivedV::ColsAtCompileTime == 3 || DerivedV::ColsAtCompileTime == Eigen::Dynamic,
    "V should have 3 or Dynamic columns");
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
  static_assert(
    DerivedV::ColsAtCompileTime == 2 || DerivedV::ColsAtCompileTime == Eigen::Dynamic,
    "V should have 2 or Dynamic columns");
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
  const igl::WindingNumberAABB<typename DerivedV::Scalar,typename DerivedF::Scalar> & hier,
  const Eigen::MatrixBase<Derivedq> & q)
{
  static_assert(
    DerivedV::ColsAtCompileTime == 3 || DerivedV::ColsAtCompileTime == Eigen::Dynamic,
    "V should have 3 or Dynamic columns");
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
  const igl::WindingNumberAABB<typename DerivedV::Scalar, typename DerivedF::Scalar> & hier,
  const Eigen::MatrixBase<Derivedq> & q,
  Scalar & s,
  Scalar & sqrd,
  int & i,
  Eigen::PlainObjectBase<Derivedc> & c)
{
  static_assert(
    DerivedV::ColsAtCompileTime == 3 || DerivedV::ColsAtCompileTime == Eigen::Dynamic,
    "V should have 3 or Dynamic columns");
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
  static_assert(
    DerivedV::ColsAtCompileTime == 2 || DerivedV::ColsAtCompileTime == Eigen::Dynamic,
    "V should have 2 or Dynamic columns");
  using namespace Eigen;
  using namespace std;
  typedef Eigen::Matrix<typename DerivedV::Scalar,1,2> RowVector2S;
  sqrd = tree.squared_distance(V,F,RowVector2S(q),i,(RowVector2S&)c);
  // TODO: using .data() like this is very dangerous... This is assuming
  // colmajor order
  assert(!V.derived().IsRowMajor);
  assert(!F.derived().IsRowMajor);
  s = 1.-2.*winding_number(V,F,q);
}

//Multi point by parrallel for on single point
template <
  typename DerivedP,
  typename DerivedV,
  typename DerivedF,
  typename DerivedS>
IGL_INLINE void igl::signed_distance_fast_winding_number(
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const AABB<DerivedV,3> & tree,
    const igl::FastWindingNumberBVH & fwn_bvh,
    Eigen::PlainObjectBase<DerivedS> & S)
  {
    static_assert(
      DerivedP::ColsAtCompileTime == 3 || DerivedP::ColsAtCompileTime == Eigen::Dynamic,
      "P should have 3 or Dynamic columns");
    typedef Eigen::Matrix<typename DerivedV::Scalar,1,3> RowVector3S;
    S.resize(P.rows(),1);
    int min_parallel = 10000; 
    parallel_for(P.rows(), [&](const int p)
    {
      RowVector3S q;
      q.head(P.row(p).size()) = P.row(p);
      // get sdf for single point, update result matrix
      S(p) = signed_distance_fast_winding_number(q, V, F, tree,fwn_bvh);
    }
    ,min_parallel);  
  }

//Single Point
template <
  typename Derivedq,
  typename DerivedV,
  typename DerivedF>
IGL_INLINE typename DerivedV::Scalar igl::signed_distance_fast_winding_number(
    const Eigen::MatrixBase<Derivedq> & q,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const AABB<DerivedV,3> & tree,
    const igl::FastWindingNumberBVH & fwn_bvh)
  {
    static_assert(
      DerivedV::ColsAtCompileTime == 3 || DerivedV::ColsAtCompileTime == Eigen::Dynamic,
      "V should have 3 or Dynamic columns");
    typedef typename DerivedV::Scalar Scalar;
    Scalar sqrd;
    Eigen::Matrix<Scalar,1,3> c;
    int i = -1;
    sqrd = tree.squared_distance(V,F,q,i,c);
    Scalar w = fast_winding_number(fwn_bvh,2,q.template cast<float>());
    //0.5 is on surface
    return sqrt(sqrd)*(1.-2.*std::abs(w));
  }

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::signed_distance<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::SignedDistanceType, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template void igl::signed_distance<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, igl::SignedDistanceType, Eigen::Matrix<double, -1, 3, 0, -1, 3>::Scalar, Eigen::Matrix<double, -1, 3, 0, -1, 3>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
// generated by autoexplicit.sh
template void igl::signed_distance<Eigen::Matrix<double, -1, 2, 0, -1, 2>, Eigen::Matrix<double, -1, 2, 0, -1, 2>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 2, 0, -1, 2>, Eigen::Matrix<double, -1, 2, 0, -1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, igl::SignedDistanceType, Eigen::Matrix<double, -1, 2, 0, -1, 2>::Scalar, Eigen::Matrix<double, -1, 2, 0, -1, 2>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> >&);
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
template void igl::signed_distance_fast_winding_number<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3> const&, igl::FastWindingNumberBVH const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1>>&);
#endif
