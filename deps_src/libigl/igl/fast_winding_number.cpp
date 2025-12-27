#include "fast_winding_number.h"
#include "octree.h"
#include "knn.h"
#include "parallel_for.h"
#include "PI.h"
#include <vector>

namespace igl {
  template <typename DerivedP, typename DerivedA, typename DerivedN,
  typename Index, typename DerivedCH, typename DerivedCM, typename DerivedR,
  typename DerivedEC>
  IGL_INLINE void fast_winding_number(const Eigen::MatrixBase<DerivedP>& P,
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
    typedef typename DerivedN::Scalar real_n;
    typedef typename DerivedA::Scalar real_a;
    typedef typename DerivedCM::Scalar real_cm;
    typedef typename DerivedR::Scalar real_r;
    typedef typename DerivedEC::Scalar real_ec;
  
    typedef Eigen::Matrix<real_p,1,3> RowVec3p;
  
    int m = CH.size();
    int num_terms;
  
    assert(expansion_order < 3 && expansion_order >= 0 && "m must be less than n");
    if(expansion_order == 0){
        num_terms = 3;
    } else if(expansion_order ==1){
        num_terms = 3 + 9;
    } else if(expansion_order == 2){
        num_terms = 3 + 9 + 27;
    }
  
    R.resize(m);
    CM.resize(m,3);
    EC.resize(m,num_terms);
    EC.setZero(m,num_terms);
    std::function< void(const int) > helper;
    helper = [&helper,
              &P,&N,&A,&expansion_order,&point_indices,&CH,&EC,&R,&CM]
    (const int index)-> void
    {
        Eigen::Matrix<real_cm,1,3> masscenter;
        masscenter << 0,0,0;
        Eigen::Matrix<real_ec,1,3> zeroth_expansion;
        zeroth_expansion << 0,0,0;
        real_p areatotal = 0.0;
        for(int j = 0; j < point_indices.at(index).size(); j++){
            int curr_point_index = point_indices.at(index).at(j);
          
            areatotal += A(curr_point_index);
            masscenter += A(curr_point_index)*P.row(curr_point_index);
            zeroth_expansion += A(curr_point_index)*N.row(curr_point_index);
        }
      
        masscenter = masscenter/areatotal;
        CM.row(index) = masscenter;
        EC.block(index,0,1,3) = zeroth_expansion;
      
        real_r max_norm = 0;
        real_r curr_norm;
      
        for(int i = 0; i < point_indices.at(index).size(); i++){
            //Get max distance from center of mass:
            int curr_point_index = point_indices.at(index).at(i);
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
                        Eigen::PlainObjectBase<DerivedWN>& WN){
  
    typedef typename DerivedP::Scalar real_p;
    typedef typename DerivedN::Scalar real_n;
    typedef typename DerivedA::Scalar real_a;
    typedef typename DerivedCM::Scalar real_cm;
    typedef typename DerivedR::Scalar real_r;
    typedef typename DerivedEC::Scalar real_ec;
    typedef typename DerivedQ::Scalar real_q;
    typedef typename DerivedWN::Scalar real_wn;
  
    typedef Eigen::Matrix<real_q,1,3> RowVec;
    typedef Eigen::Matrix<real_ec,3,3> EC_3by3;
  
    auto direct_eval = [](const RowVec & loc,
                          const Eigen::Matrix<real_ec,1,3> & anorm){
        real_wn wn = (loc(0)*anorm(0)+loc(1)*anorm(1)+loc(2)*anorm(2))
                                    /(4.0*igl::PI*std::pow(loc.norm(),3));
        if(std::isnan(wn)){
            return 0.5;
        }else{
            return wn;
        }
    };
  
    auto expansion_eval = [&direct_eval](const RowVec & loc,
                                         const Eigen::RowVectorXd & EC){
      real_wn wn = direct_eval(loc,EC.head<3>());
      double r = loc.norm();
      if(EC.size()>3){
        Eigen::Matrix<real_ec,3,3> SecondDerivative =
            Eigen::Matrix<real_ec,3,3>::Identity()/(4.0*igl::PI*std::pow(r,3));
        SecondDerivative += -3.0*loc.transpose()*loc/(4.0*igl::PI*std::pow(r,5));
        Eigen::Matrix<real_ec,1,9> derivative_vector =
          Eigen::Map<Eigen::Matrix<real_ec,1,9> >(SecondDerivative.data(),
                                                  SecondDerivative.size());
        wn += derivative_vector.cwiseProduct(EC.segment<9>(3)).sum();
      }
      if(EC.size()>3+9){
          Eigen::Matrix<real_ec,3,3> ThirdDerivative;
          for(int i = 0; i < 3; i++){
              ThirdDerivative =
                  15.0*loc(i)*loc.transpose()*loc/(4.0*igl::PI*std::pow(r,7));
              Eigen::Matrix<real_ec,3,3> Diagonal;
              Diagonal << loc(i), 0, 0,
              0, loc(i), 0,
              0, 0, loc(i);
              Eigen::Matrix<real_ec,3,3> RowCol;
              RowCol.setZero(3,3);
              RowCol.row(i) = loc;
              Eigen::Matrix<real_ec,3,3> RowColT = RowCol.transpose();
              RowCol = RowCol + RowColT;
              ThirdDerivative +=
                  -3.0/(4.0*igl::PI*std::pow(r,5))*(RowCol+Diagonal);
              Eigen::Matrix<real_ec,1,9> derivative_vector =
                Eigen::Map<Eigen::Matrix<real_ec,1,9> >(ThirdDerivative.data(),
                                                        ThirdDerivative.size());
              wn += derivative_vector.cwiseProduct(
                                              EC.segment<9>(12 + i*9)).sum();
          }
      }
      return wn;
    };
  
    int m = Q.rows();
    WN.resize(m,1);
  
    std::function< real_wn(const RowVec, const std::vector<int>) > helper;
    helper = [&helper,
              &P,&N,&A,
              &point_indices,&CH,
              &CM,&R,&EC,&beta,
              &direct_eval,&expansion_eval]
    (const RowVec query, const std::vector<int> near_indices)-> real_wn
    {
      std::vector<int> new_near_indices;
      real_wn wn = 0;
      for(int i = 0; i < near_indices.size(); i++){
        int index = near_indices.at(i);
        //Leaf Case, Brute force
        if(CH(index,0) == -1){
          for(int j = 0; j < point_indices.at(index).size(); j++){
            int curr_row = point_indices.at(index).at(j);
            wn += direct_eval(P.row(curr_row)-query,
                              N.row(curr_row)*A(curr_row));
          }
        }
        //Non-Leaf Case
        else {
          for(int child = 0; child < 8; child++){
              int child_index = CH(index,child);
              if(point_indices.at(child_index).size() > 0){
                if((CM.row(child_index)-query).norm() > beta*R(child_index)){
                  if(CH(child_index,0) == -1){
                    for(int j=0;j<point_indices.at(child_index).size();j++){
                      int curr_row = point_indices.at(child_index).at(j);
                      wn += direct_eval(P.row(curr_row)-query,
                                        N.row(curr_row)*A(curr_row));
                    }
                  }else{
                    wn += expansion_eval(CM.row(child_index)-query,
                                         EC.row(child_index));
                  }
                }else {
                  new_near_indices.emplace_back(child_index);
              }
            }
          }
        }
      }
      if(new_near_indices.size() > 0){
          wn += helper(query,new_near_indices);
      }
      return wn;
    };
  
  
    if(beta > 0){
      std::vector<int> near_indices_start = {0};
      igl::parallel_for(m,[&](int iter){
        WN(iter) = helper(Q.row(iter),near_indices_start);
      },1000);
    } else {
      igl::parallel_for(m,[&](int iter){
        double wn = 0;
        for(int j = 0; j <P.rows(); j++){
          wn += direct_eval(P.row(j)-Q.row(iter),N.row(j)*A(j));
        }
        WN(iter) = wn;
      },1000);
    }
  }
  
  template <typename DerivedP, typename DerivedA, typename DerivedN,
    typename DerivedQ, typename BetaType, typename DerivedWN>
  IGL_INLINE void fast_winding_number(const Eigen::MatrixBase<DerivedP>& P,
                                      const Eigen::MatrixBase<DerivedN>& N,
                                      const Eigen::MatrixBase<DerivedA>& A,
                                      const Eigen::MatrixBase<DerivedQ>& Q,
                                      const int expansion_order,
                                      const BetaType beta,
                                      Eigen::PlainObjectBase<DerivedWN>& WN
                                      ){
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
  
  template <typename DerivedP, typename DerivedA, typename DerivedN,
    typename DerivedQ, typename DerivedWN>
  IGL_INLINE void fast_winding_number(const Eigen::MatrixBase<DerivedP>& P,
                                      const Eigen::MatrixBase<DerivedN>& N,
                                      const Eigen::MatrixBase<DerivedA>& A,
                                      const Eigen::MatrixBase<DerivedQ>& Q,
                                      Eigen::PlainObjectBase<DerivedWN>& WN
                                      ){
    fast_winding_number(P,N,A,Q,2,2.0,WN);
  }
}

























