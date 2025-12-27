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
#include "point_simplex_squared_distance.h"
#include "project_to_line_segment.h"
#include "sort.h"
#include "volume.h"
#include "ray_box_intersect.h"
#include "parallel_for.h"
#include "ray_mesh_intersect.h"
#include <iostream>
#include <iomanip>
#include <limits>
#include <list>
#include <queue>
#include <stack>

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
  deinit();
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
      m_right = new AABB();
      m_right->init( V,Ele,bb_mins,bb_maxs,elements,2*i+2);
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
  // deinit will be immediately called...
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
  deinit();
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
          //m_depth = std::max(m_depth, m_left->m_depth+1);
        }
        if(RI.rows()>0)
        {
          m_right = new AABB();
          m_right->init(V,Ele,SI,RI);
          //m_depth = std::max(m_depth, m_right->m_depth+1);
        }
      }
  }
}

template <typename DerivedV, int DIM>
IGL_INLINE bool igl::AABB<DerivedV,DIM>::is_leaf() const
{
  return m_primitive != -1;
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
  const Scalar epsilon = igl::EPS<Scalar>();
  // Check if outside bounding box
  bool inside = m_box.contains(q.transpose());
  if(!inside)
  {
    return std::vector<int>();
  }
  assert(m_primitive==-1 || (m_left == NULL && m_right == NULL));
  if(is_leaf())
  {
    // Initialize to some value > -epsilon
    Scalar a1=0,a2=0,a3=0,a4=0;
    switch(DIM)
    {
      case 3:
        {
          // Barycentric coordinates
          typedef Eigen::Matrix<Scalar,1,3> RowVector3S;
          const RowVector3S V1 = V.row(Ele(m_primitive,0));
          const RowVector3S V2 = V.row(Ele(m_primitive,1));
          const RowVector3S V3 = V.row(Ele(m_primitive,2));
          const RowVector3S V4 = V.row(Ele(m_primitive,3));
          a1 = volume_single(V2,V4,V3,(RowVector3S)q);
          a2 = volume_single(V1,V3,V4,(RowVector3S)q);
          a3 = volume_single(V1,V4,V2,(RowVector3S)q);
          a4 = volume_single(V1,V2,V3,(RowVector3S)q);
          break;
        }
      case 2:
        {
          // Barycentric coordinates
          typedef Eigen::Matrix<Scalar,2,1> Vector2S;
          const Vector2S V1 = V.row(Ele(m_primitive,0));
          const Vector2S V2 = V.row(Ele(m_primitive,1));
          const Vector2S V3 = V.row(Ele(m_primitive,2));
          // Hack for now to keep templates simple. If becomes bottleneck
          // consider using std::enable_if_t
          const Vector2S q2 = q.head(2);
          a1 = doublearea_single(V1,V2,q2);
          a2 = doublearea_single(V2,V3,q2);
          a3 = doublearea_single(V3,V1,q2);
          break;
        }
      default:assert(false);
    }
    // Normalization is important for correcting sign
    Scalar sum = a1+a2+a3+a4;
    a1 /= sum;
    a2 /= sum;
    a3 /= sum;
    a4 /= sum;
    if(
        a1>=-epsilon &&
        a2>=-epsilon &&
        a3>=-epsilon &&
        a4>=-epsilon)
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
IGL_INLINE int igl::AABB<DerivedV,DIM>::subtree_size() const
{
  // 1 for self
  int n = 1;
  int n_left = 0,n_right = 0;
  if(m_left != NULL)
  {
    n_left = m_left->subtree_size();
  }
  if(m_right != NULL)
  {
    n_right = m_right->subtree_size();
  }
  n += 2*std::max(n_left,n_right);
  return n;
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
  if(m_left != NULL)
  {
    m_left->serialize(bb_mins,bb_maxs,elements,2*i+1);
  }
  if(m_right != NULL)
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

  assert(m_primitive==-1 || (m_left == NULL && m_right == NULL));
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
      RowVectorDIMS Pp = P.row(p), c;
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
#if 0 // False

  // _Very_ conservative approximation of maximum squared distance between
  // primitives inside this and other's bounding boxes
  const auto & max_squared_distance = [](
    const AABB<DerivedV,DIM> * A,
    const AABB<Derivedother_V,DIM> * B)->Scalar
  {
    AlignedBox<Scalar,DIM> combo = A->m_box;
    combo.extend(B->m_box);
    return combo.diagonal().squaredNorm();
  };

  //// other base-case
  //if(other->is_leaf())
  //{
  //  double sqr_d = sqrD(other->m_primitive);
  //  int i = I(other->m_primitive);
  //  RowVectorDIMS c = C.row(m_primitive);
  //  RowVectorDIMS p = other_V.row(m_primitive);
  //  leaf_squared_distance(V,Ele,p,sqr_d,i,c);
  //  sqrD(other->m_primitive) = sqr_d;
  //  I(other->m_primitive) = i;
  //  C.row(m_primitive) = c;
  //  return;
  //}
  std::vector<const AABB<DerivedV,DIM> * > this_list;
  if(this->is_leaf())
  {
    this_list.push_back(this);
  }else
  {
    assert(this->m_left);
    this_list.push_back(this->m_left);
    assert(this->m_right);
    this_list.push_back(this->m_right);
  }
  std::vector<AABB<Derivedother_V,DIM> *> other_list;
  if(other->is_leaf())
  {
    other_list.push_back(other);
  }else
  {
    assert(other->m_left);
    other_list.push_back(other->m_left);
    assert(other->m_right);
    other_list.push_back(other->m_right);
  }

  //const std::function<Scalar(
  //  const AABB<Derivedother_V,DIM> * other)
  //    > low_sqr_d = [&sqrD,&low_sqr_d](const AABB<Derivedother_V,DIM> * other)->Scalar
  //  {
  //    if(other->is_leaf())
  //    {
  //      return sqrD(other->m_primitive);
  //    }else
  //    {
  //      return std::max(low_sqr_d(other->m_left),low_sqr_d(other->m_right));
  //    }
  //  };

  //// Potentially recurse on all pairs, if minimum distance is less than running
  //// bound
  //Eigen::Matrix<Scalar,Eigen::Dynamic,1> other_low_sqr_d =
  //  Eigen::Matrix<Scalar,Eigen::Dynamic,1>::Constant(other_list.size(),1,up_sqr_d);
  for(size_t child = 0;child<other_list.size();child++)
  {
    auto other_tree = other_list[child];

    Eigen::Matrix<Scalar,Eigen::Dynamic,1> this_low_sqr_d(this_list.size(),1);
    for(size_t t = 0;t<this_list.size();t++)
    {
      const auto this_tree = this_list[t];
      this_low_sqr_d(t) = max_squared_distance(this_tree,other_tree);
    }
    if(this_list.size() ==2 &&
      ( this_low_sqr_d(0) > this_low_sqr_d(1))
      )
    {
      std::swap(this_list[0],this_list[1]);
      //std::swap(this_low_sqr_d(0),this_low_sqr_d(1));
    }
    const Scalar sqr_d = this_low_sqr_d.minCoeff();


    for(size_t t = 0;t<this_list.size();t++)
    {
      const auto this_tree = this_list[t];

      //const auto mm = low_sqr_d(other_tree);
      //const Scalar mc = other_low_sqr_d(child);
      //assert(mc == mm);
      // Only look left/right in this_list if can possible decrease somebody's
      // distance in this_tree.
      const Scalar min_this_other = min_squared_distance(this_tree,other_tree);
      if(
          min_this_other < sqr_d &&
          min_this_other < other_tree->m_low_sqr_d)
      {
        //cout<<"before: "<<other_low_sqr_d(child)<<endl;
        //other_low_sqr_d(child) = std::min(
        //  other_low_sqr_d(child),
        //  this_tree->squared_distance_helper(
        //    V,Ele,other_tree,other_V,other_Ele,other_low_sqr_d(child),sqrD,I,C));
        //cout<<"after: "<<other_low_sqr_d(child)<<endl;
          this_tree->squared_distance_helper(
            V,Ele,other_tree,other_V,other_Ele,0,sqrD,I,C);
      }
    }
  }
  //const Scalar ret = other_low_sqr_d.maxCoeff();
  //const auto mm = low_sqr_d(other);
  //assert(mm == ret);
  //cout<<"non-leaf: "<<ret<<endl;
  //return ret;
  if(!other->is_leaf())
  {
    other->m_low_sqr_d = std::max(other->m_left->m_low_sqr_d,other->m_right->m_low_sqr_d);
  }
  return 0;
#endif
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
IGL_INLINE void igl::AABB<DerivedV,DIM>::set_min(
  const RowVectorDIMS &
#ifndef NDEBUG
  p
#endif
  ,
  const Scalar sqr_d_candidate,
  const int i_candidate,
  const RowVectorDIMS & c_candidate,
  Scalar & sqr_d,
  int & i,
  Eigen::PlainObjectBase<RowVectorDIMS> & c) const
{
#ifndef NDEBUG
  //std::cout<<matlab_format(c_candidate,"c_candidate")<<std::endl;
  //// This doesn't quite make sense to check with bounds
  // const Scalar pc_norm = (p-c_candidate).squaredNorm();
  // const Scalar diff = fabs(sqr_d_candidate - pc_norm);
  // assert(diff<=1e-10 && "distance should match norm of difference");
#endif
  if(sqr_d_candidate < sqr_d)
  {
    i = i_candidate;
    c = c_candidate;
    sqr_d = sqr_d_candidate;
  }
}


template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE bool
igl::AABB<DerivedV,DIM>::intersect_ray(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & origin,
  const RowVectorDIMS & dir,
  std::vector<igl::Hit> & hits) const
{
  hits.clear();
  const Scalar t0 = 0;
  const Scalar t1 = std::numeric_limits<Scalar>::infinity();
  {
    Scalar _1,_2;
    if(!ray_box_intersect(origin,dir,m_box,t0,t1,_1,_2))
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
    for(auto & hit : hits)
    {
      hit.id = m_primitive;
    }
    return ret;
  }
  std::vector<igl::Hit> left_hits;
  std::vector<igl::Hit> right_hits;
  const bool left_ret = m_left->intersect_ray(V,Ele,origin,dir,left_hits);
  const bool right_ret = m_right->intersect_ray(V,Ele,origin,dir,right_hits);
  hits.insert(hits.end(),left_hits.begin(),left_hits.end());
  hits.insert(hits.end(),right_hits.begin(),right_hits.end());
  return left_ret || right_ret;
}

template <typename DerivedV, int DIM>
template <typename DerivedEle>
IGL_INLINE bool
igl::AABB<DerivedV,DIM>::intersect_ray(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const RowVectorDIMS & origin,
  const RowVectorDIMS & dir,
  igl::Hit & hit) const
{
    // FIXME: Needs a proper path
#if /*false*/ 0
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
      igl::Hit leaf_hit;
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
  igl::Hit & hit) const
{
  //// Naive, slow
  //std::vector<igl::Hit> hits;
  //intersect_ray(V,Ele,origin,dir,hits);
  //if(hits.size() > 0)
  //{
  //  hit = hits.front();
  //  return true;
  //}else
  //{
  //  return false;
  //}
  Scalar min_t = _min_t;
  const Scalar t0 = 0;
  {
    Scalar _1,_2;
    if(!ray_box_intersect(origin,dir,m_box,t0,min_t,_1,_2))
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
  igl::Hit left_hit;
  igl::Hit right_hit;
  bool left_ret = m_left->intersect_ray(V,Ele,origin,dir,min_t,left_hit);
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
  bool right_ret = m_right->intersect_ray(V,Ele,origin,dir,min_t,right_hit);
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

// This is a bullshit template because AABB annoyingly needs templates for bad
// combinations of 3D V with DIM=2 AABB
//
// _Define_ as a no-op rather than monkeying around with the proper code above
//
// Meanwhile, GCC seems to have a bug. Let's see if GCC likes using explicit
// namespace block instead. https://stackoverflow.com/a/25594681/148668
namespace igl
{
  template<> template<> IGL_INLINE float AABB<Eigen::Matrix<float, -1, 3, 1, -1, 3>, 2>::squared_distance( Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<float, 1, 2, 1, 1, 2> const&, int&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&) const { assert(false);return -1;};
  template<> template<> IGL_INLINE float igl::AABB<Eigen::Matrix<float, -1, 3, 1, -1, 3>, 2>::squared_distance( Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<float, 1, 2, 1, 1, 2> const&, float, float, int&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&) const { assert(false);return -1;};
  template<> template<> IGL_INLINE void igl::AABB<Eigen::Matrix<float, -1, 3, 1, -1, 3>, 2>::init (Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&) { assert(false);};
  template<> template<> IGL_INLINE double AABB<Eigen::Matrix<double, -1, 3, 1, -1, 3>, 2>::squared_distance( Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<double, 1, 2, 1, 1, 2> const&, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&) const { assert(false);return -1;};
  template<> template<> IGL_INLINE double igl::AABB<Eigen::Matrix<double, -1, 3, 1, -1, 3>, 2>::squared_distance( Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<double, 1, 2, 1, 1, 2> const&, double, double, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&) const { assert(false);return -1;};
  template<> template<> IGL_INLINE void igl::AABB<Eigen::Matrix<double, -1, 3, 1, -1, 3>, 2>::init (Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&) { assert(false);};
  template<> template<> IGL_INLINE void igl::AABB<Eigen::Matrix<float, -1, 3, 0, -1, 3>, 2>::init(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&) {assert(false);};
  template<> template<> IGL_INLINE float igl::AABB<Eigen::Matrix<float, -1, 3, 0, -1, 3>, 2>::squared_distance(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::Matrix<float, 1, 2, 1, 1, 2> const&, float, float, int&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 2, 1, 1, 2> >&) const { assert(false);return -1;};
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template double igl::AABB<Eigen::Matrix<double, -1, 3, 1, -1, 3>, 3>::squared_distance<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, double, double, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, 3, 1, -1, 3>, 3>::init<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&);
// generated by autoexplicit.sh
// generated by autoexplicit.sh
template float igl::AABB<Eigen::Matrix<float, -1, 3, 0, -1, 3>, 3>::squared_distance<Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::Matrix<float, 1, 3, 1, 1, 3> const&, float, float, int&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&) const;
// generated by autoexplicit.sh
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<float, -1, 3, 0, -1, 3>, 3>::init<Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&);
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::serialize<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, int) const;
// generated by autoexplicit.sh
template std::vector<int, std::allocator<int> > igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::find<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 1, -1, 1, 1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, bool) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::serialize<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, int) const;
// generated by autoexplicit.sh
template std::vector<int, std::allocator<int> > igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::find<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 1, -1, 1, 1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, bool) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::init<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int);
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::init<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int);
// generated by autoexplicit.sh
template float igl::AABB<Eigen::Matrix<float, -1, 3, 1, -1, 3>, 3>::squared_distance<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::Matrix<float, 1, 3, 1, 1, 3> const&, int&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<long, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
// generated by autoexplicit.sh
template double igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<long, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&) const;
// generated by autoexplicit.sh
template double igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, 1, 2, 1, 1, 2> const&, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> >&) const;
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<float, -1, 3, 1, -1, 3>, 3>::init<Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&);
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::init<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
// generated by autoexplicit.sh
template void igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 2>::init<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
template double igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::squared_distance<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, double, int&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&) const;
template bool igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3>::intersect_ray<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, Eigen::Matrix<double, 1, 3, 1, 1, 3> const&, igl::Hit&) const;
#endif
