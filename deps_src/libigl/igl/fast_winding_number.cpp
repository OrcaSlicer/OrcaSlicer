#include "fast_winding_number.h"
#include "octree.h"
#include "parallel_for.h"
#include "PI.h"
#include <vector>
#include <cassert>

template <
  typename DerivedP, 
  typename DerivedA, 
  typename DerivedN,
  typename Index, 
  typename DerivedCH, 
  typename DerivedCM, 
  typename DerivedR,
  typename DerivedEC>
IGL_INLINE void igl::fast_winding_number(
  const Eigen::MatrixBase<DerivedP>& P,
  const Eigen::MatrixBase<DerivedN>& N,
  const Eigen::MatrixBase<DerivedA>& A,
  const std::vector<std::vector<Index> > & point_indices,
  const Eigen::MatrixBase<DerivedCH>& CH,
  const int expansion_order,
  Eigen::PlainObjectBase<DerivedCM>& CM,
  Eigen::PlainObjectBase<DerivedR>& R,
  Eigen::PlainObjectBase<DerivedEC>& EC)
{
  typedef typename DerivedP::Scalar real_p;
  typedef typename DerivedCM::Scalar real_cm;
  typedef typename DerivedR::Scalar real_r;
  typedef typename DerivedEC::Scalar real_ec;


  int m = CH.size();
  int num_terms = -1;

  assert(expansion_order < 3 && expansion_order >= 0 && "m must be less than n");
  if(expansion_order == 0){
      num_terms = 3;
  } else if(expansion_order ==1){
      num_terms = 3 + 9;
  } else if(expansion_order == 2){
      num_terms = 3 + 9 + 27;
  }
  assert(num_terms > 0);

  R.resize(m);
  CM.resize(m,3);
  EC.resize(m,num_terms);
  EC.setZero(m,num_terms);
  std::function< void(const int) > helper;
  helper = [&helper,
            &P,&N,&A,&point_indices,&CH,&EC,&R,&CM]
  (const int index)-> void
  {
      Eigen::Matrix<real_cm,1,3> masscenter;
      masscenter << 0,0,0;
      Eigen::Matrix<real_ec,1,3> zeroth_expansion;
      zeroth_expansion << 0,0,0;
      real_p areatotal = 0.0;
      const int num_points = point_indices[index].size();
      for(int j = 0; j < num_points; j++){
          int curr_point_index = point_indices[index][j];
        
          areatotal += A(curr_point_index);
          masscenter += A(curr_point_index)*P.row(curr_point_index);
          zeroth_expansion += A(curr_point_index)*N.row(curr_point_index);
      }
      // Avoid divide by zero
      if(num_points > 0)
      {
        masscenter = masscenter/areatotal;
      }else
      {
        masscenter.setConstant(std::numeric_limits<real_cm>::quiet_NaN());
      }
      CM.row(index) = masscenter;
      EC.block(index,0,1,3) = zeroth_expansion;
    
      real_r max_norm = 0;
      real_r curr_norm;
    
      for(int i = 0; i < point_indices[index].size(); i++){
          //Get max distance from center of mass:
          int curr_point_index = point_indices[index][i];
          Eigen::Matrix<real_r,1,3> point =
              P.row(curr_point_index)-masscenter;
          curr_norm = point.norm();
          if(curr_norm > max_norm){
              max_norm = curr_norm;
          }
        
          //Calculate higher order terms if necessary
          Eigen::Matrix<real_ec,3,3> TempCoeffs;
          if(EC.cols() >= (3+9)){
              TempCoeffs = A(curr_point_index)*point.transpose()*
                              N.row(curr_point_index);
              EC.block(index,3,1,9) +=
              Eigen::Map<Eigen::Matrix<real_ec,1,9> >(TempCoeffs.data(),
                                                      TempCoeffs.size());
          }
        
          if(EC.cols() == (3+9+27)){
              for(int k = 0; k < 3; k++){
                  TempCoeffs = 0.5 * point(k) * (A(curr_point_index)*
                                point.transpose()*N.row(curr_point_index));
                  EC.block(index,12+9*k,1,9) += Eigen::Map<
                    Eigen::Matrix<real_ec,1,9> >(TempCoeffs.data(),
                                                 TempCoeffs.size());
              }
          }
      }
    
      R(index) = max_norm;
      if(CH(index,0) != -1)
      {
          for(int i = 0; i < 8; i++){
              int child = CH(index,i);
              helper(child);
          }
      }
  };
  helper(0);
}

template <
  typename DerivedP, 
  typename DerivedA, 
  typename DerivedN,
  typename Index, 
  typename DerivedCH, 
  typename DerivedCM, 
  typename DerivedR,
  typename DerivedEC, 
  typename DerivedQ, 
  typename BetaType,
  typename DerivedWN>
IGL_INLINE void igl::fast_winding_number(
  const Eigen::MatrixBase<DerivedP>& P,
  const Eigen::MatrixBase<DerivedN>& N,
  const Eigen::MatrixBase<DerivedA>& A,
  const std::vector<std::vector<Index> > & point_indices,
  const Eigen::MatrixBase<DerivedCH>& CH,
  const Eigen::MatrixBase<DerivedCM>& CM,
  const Eigen::MatrixBase<DerivedR>& R,
  const Eigen::MatrixBase<DerivedEC>& EC,
  const Eigen::MatrixBase<DerivedQ>& Q,
  const BetaType beta,
  Eigen::PlainObjectBase<DerivedWN>& WN)
{

  typedef typename DerivedEC::Scalar real_ec;
  typedef typename DerivedQ::Scalar real_q;
  typedef typename DerivedWN::Scalar real_wn;
  const real_wn PI_4 = 4.0*igl::PI;

  typedef Eigen::Matrix<real_q,1,3> RowVec;

  auto direct_eval = [&PI_4](
    const RowVec & loc,
    const Eigen::Matrix<real_ec,1,3> & anorm)->real_wn
  {
    const typename RowVec::Scalar loc_norm = loc.norm();
    if(loc_norm == 0)
    {
      return 0.5;
    }else
    {
      return (loc(0)*anorm(0)+loc(1)*anorm(1)+loc(2)*anorm(2))
                                  /(PI_4*(loc_norm*loc_norm*loc_norm));
    }
  };

  auto expansion_eval = 
    [&direct_eval,&EC,&PI_4](
      const RowVec & loc,
      const int & child_index)->real_wn
  {
    real_wn wn;
    wn = direct_eval(loc,EC.row(child_index).template head<3>());
    real_wn r = loc.norm();
    real_wn PI_4_r3;
    real_wn PI_4_r5;
    real_wn PI_4_r7;
    if(EC.row(child_index).size()>3)
    {
      PI_4_r3 = PI_4*r*r*r;
      PI_4_r5 = PI_4_r3*r*r;
      const real_ec d = 1.0/(PI_4_r3);
      Eigen::Matrix<real_ec,3,3> SecondDerivative = 
        loc.transpose()*loc*(-3.0/(PI_4_r5));
      SecondDerivative(0,0) += d;
      SecondDerivative(1,1) += d;
      SecondDerivative(2,2) += d;
      wn += 
        Eigen::Map<Eigen::Matrix<real_ec,1,9> >(
          SecondDerivative.data(),
          SecondDerivative.size()).dot(
            EC.row(child_index).template segment<9>(3));
    }
    if(EC.row(child_index).size()>3+9)
    {
      PI_4_r7 = PI_4_r5*r*r;
      const Eigen::Matrix<real_ec,3,3> locTloc = loc.transpose()*(loc/(PI_4_r7));
      for(int i = 0; i < 3; i++)
      {
        Eigen::Matrix<real_ec,3,3> RowCol_Diagonal = 
          Eigen::Matrix<real_ec,3,3>::Zero(3,3);
        for(int u = 0;u<3;u++)
        {
          for(int v = 0;v<3;v++)
          {
            if(u==v) RowCol_Diagonal(u,v) += loc(i);
            if(u==i) RowCol_Diagonal(u,v) += loc(v);
            if(v==i) RowCol_Diagonal(u,v) += loc(u);
          }
        }
        Eigen::Matrix<real_ec,3,3> ThirdDerivative = 
          15.0*loc(i)*locTloc + (-3.0/(PI_4_r5))*(RowCol_Diagonal);

        wn += Eigen::Map<Eigen::Matrix<real_ec,1,9> >(
                ThirdDerivative.data(),
                ThirdDerivative.size()).dot(
            EC.row(child_index).template segment<9>(12 + i*9));
      }
    }
    return wn;
  };

  int m = Q.rows();
  WN.resize(m,1);

  std::function< real_wn(const RowVec & , const std::vector<int> &) > helper;
  helper = [&helper,
            &P,&N,&A,
            &point_indices,&CH,
            &CM,&R,&beta,
            &direct_eval,&expansion_eval]
  (const RowVec & query, const std::vector<int> & near_indices)-> real_wn
  {
    real_wn wn = 0;
    std::vector<int> new_near_indices;
    new_near_indices.reserve(8);
    for(int i = 0; i < near_indices.size(); i++)
    {
      int index = near_indices[i];
      //Leaf Case, Brute force
      if(CH(index,0) == -1)
      {
        for(int j = 0; j < point_indices[index].size(); j++)
        {
          int curr_row = point_indices[index][j];
          wn += direct_eval(P.row(curr_row)-query,
                            N.row(curr_row)*A(curr_row));
        }
      }
      //Non-Leaf Case
      else 
      {
        for(int child = 0; child < 8; child++)
        {
          int child_index = CH(index,child);
          if(point_indices[child_index].size() > 0)
          {
            const RowVec CMciq = (CM.row(child_index)-query);
            if(CMciq.norm() > beta*R(child_index))
            {
              if(CH(child_index,0) == -1)
              {
                for(int j=0;j<point_indices[child_index].size();j++)
                {
                  int curr_row = point_indices[child_index][j];
                  wn += direct_eval(P.row(curr_row)-query,
                                    N.row(curr_row)*A(curr_row));
                }
              }else{
                wn += expansion_eval(CMciq,child_index);
              }
            }else 
            {
              new_near_indices.emplace_back(child_index);
            }
          }
        }
      }
    }
    if(new_near_indices.size() > 0)
    {
      wn += helper(query,new_near_indices);
    }
    return wn;
  };

  if(beta > 0)
  {
    const std::vector<int> near_indices_start = {0};
    igl::parallel_for(m,[&](int iter){
      WN(iter) = helper(Q.row(iter).eval(),near_indices_start);
    },1000);
  } else 
  {
    igl::parallel_for(m,[&](int iter){
      double wn = 0;
      for(int j = 0; j <P.rows(); j++)
      {
        wn += direct_eval(P.row(j)-Q.row(iter),N.row(j)*A(j));
      }
      WN(iter) = wn;
    },1000);
  }
}

template <
  typename DerivedP, 
  typename DerivedA, 
  typename DerivedN,
  typename DerivedQ, 
  typename BetaType, 
  typename DerivedWN>
IGL_INLINE void igl::fast_winding_number(
  const Eigen::MatrixBase<DerivedP>& P,
  const Eigen::MatrixBase<DerivedN>& N,
  const Eigen::MatrixBase<DerivedA>& A,
  const Eigen::MatrixBase<DerivedQ>& Q,
  const int expansion_order,
  const BetaType beta,
  Eigen::PlainObjectBase<DerivedWN>& WN)
{
  typedef typename DerivedWN::Scalar real;
  
  std::vector<std::vector<int> > point_indices;
  Eigen::Matrix<int,Eigen::Dynamic,8> CH;
  Eigen::Matrix<real,Eigen::Dynamic,3> CN;
  Eigen::Matrix<real,Eigen::Dynamic,1> W;

  octree(P,point_indices,CH,CN,W);

  Eigen::Matrix<real,Eigen::Dynamic,Eigen::Dynamic> EC;
  Eigen::Matrix<real,Eigen::Dynamic,3> CM;
  Eigen::Matrix<real,Eigen::Dynamic,1> R;

  fast_winding_number(P,N,A,point_indices,CH,expansion_order,CM,R,EC);
  fast_winding_number(P,N,A,point_indices,CH,CM,R,EC,Q,beta,WN);
}

template <
  typename DerivedP, 
  typename DerivedA, 
  typename DerivedN,
  typename DerivedQ, 
  typename DerivedWN>
IGL_INLINE void igl::fast_winding_number(
  const Eigen::MatrixBase<DerivedP>& P,
  const Eigen::MatrixBase<DerivedN>& N,
  const Eigen::MatrixBase<DerivedA>& A,
  const Eigen::MatrixBase<DerivedQ>& Q,
  Eigen::PlainObjectBase<DerivedWN>& WN)
{
  fast_winding_number(P,N,A,Q,2,2.0,WN);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedQ,
  typename DerivedW>
IGL_INLINE void igl::fast_winding_number(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedQ> & Q,
  Eigen::PlainObjectBase<DerivedW> & W)
{
  igl::FastWindingNumberBVH fwn_bvh;
  int order = 2;
  igl::fast_winding_number(V,F,order,fwn_bvh);
  float accuracy_scale = 2;
  igl::fast_winding_number(fwn_bvh,accuracy_scale,Q,W);
}

template <
  typename DerivedV,
  typename DerivedF>
IGL_INLINE void igl::fast_winding_number(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const int order,
  FastWindingNumberBVH & fwn_bvh)
{
  assert(V.cols() == 3 && "V should be 3D");
  assert(F.cols() == 3 && "F should contain triangles");
  // Extra copies. Usuually this won't be the bottleneck.
  fwn_bvh.U.resize(V.rows());
  for(int i = 0;i<V.rows();i++)
  {
    for(int j = 0;j<3;j++)
    {
      fwn_bvh.U[i][j] = V(i,j);
    }
  }
  // Wouldn't need to copy if F is **RowMajor**
  fwn_bvh.F.resize(F.size());
  for(int f = 0;f<F.rows();f++)
  {
    for(int c = 0;c<F.cols();c++)
    {
      fwn_bvh.F[c+f*F.cols()] = F(f,c);
    }
  }
  fwn_bvh.ut_solid_angle.clear();
  fwn_bvh.ut_solid_angle.init(
     fwn_bvh.F.size()/3, 
    &fwn_bvh.F[0], 
     fwn_bvh.U.size(), 
    &fwn_bvh.U[0], 
    order);
}

template <
  typename DerivedQ,
  typename DerivedW>
IGL_INLINE void igl::fast_winding_number(
  const FastWindingNumberBVH & fwn_bvh,
  const float accuracy_scale,
  const Eigen::MatrixBase<DerivedQ> & Q,
  Eigen::PlainObjectBase<DerivedW> & W)
{
  assert(Q.cols() == 3 && "Q should be 3D");
  W.resize(Q.rows(),1);
  igl::parallel_for(Q.rows(),[&](int p)
  {
    FastWindingNumber::HDK_Sample::UT_Vector3T<float>Qp;
    Qp[0] = Q(p,0);
    Qp[1] = Q(p,1);
    Qp[2] = Q(p,2);
    W(p) = fwn_bvh.ut_solid_angle.computeSolidAngle(Qp,accuracy_scale) / (4.0*igl::PI);
  },1000);
}

template <typename Derivedp>
IGL_INLINE typename Derivedp::Scalar igl::fast_winding_number(
  const FastWindingNumberBVH & fwn_bvh,
  const float accuracy_scale,
  const Eigen::MatrixBase<Derivedp> & p)
{
  assert(p.cols() == 3 && "p should be 3D");

  FastWindingNumber::HDK_Sample::UT_Vector3T<float>Qp;
  Qp[0] = p(0,0);
  Qp[1] = p(0,1);
  Qp[2] = p(0,2);

  typename Derivedp::Scalar w = fwn_bvh.ut_solid_angle.computeSolidAngle(Qp,accuracy_scale) / (4.0*igl::PI);

  return w;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::fast_winding_number<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(igl::FastWindingNumberBVH const&, float, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template Eigen::Matrix<float, -1, -1, 0, -1, -1>::Scalar igl::fast_winding_number<Eigen::Matrix<float, -1, -1, 0, -1, -1> >(igl::FastWindingNumberBVH const&, float, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&);
// generated by autoexplicit.sh
template void igl::fast_winding_number<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::fast_winding_number<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, int, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, int, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::fast_winding_number<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, int, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::fast_winding_number<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(igl::FastWindingNumberBVH const&, float, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::fast_winding_number<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, igl::FastWindingNumberBVH&);
template void igl::fast_winding_number<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, 1, 0, -1, 1> >(igl::FastWindingNumberBVH const&, float, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 1, 0, -1, 1> >&);
template void igl::fast_winding_number<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, igl::FastWindingNumberBVH&);

// tom did this manually. Unsure how to generate otherwise... sorry.
template Eigen::Matrix<float, 1, 3, 1, 1, 3>::Scalar igl::fast_winding_number<Eigen::Matrix<float, 1, 3, 1, 1, 3> >(igl::FastWindingNumberBVH const&, float, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&);
template void igl::fast_winding_number<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, int, igl::FastWindingNumberBVH&);
template void igl::fast_winding_number<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, int, igl::FastWindingNumberBVH&);
template Eigen::CwiseUnaryOp<Eigen::internal::scalar_cast_op<double, float>, Eigen::Matrix<double, 1, 3, 1, 1, 3> const>::Scalar igl::fast_winding_number<Eigen::CwiseUnaryOp<Eigen::internal::scalar_cast_op<double, float>, Eigen::Matrix<double, 1, 3, 1, 1, 3> const> >(igl::FastWindingNumberBVH const&, float, Eigen::MatrixBase<Eigen::CwiseUnaryOp<Eigen::internal::scalar_cast_op<double, float>, Eigen::Matrix<double, 1, 3, 1, 1, 3> const> > const&);
#endif
