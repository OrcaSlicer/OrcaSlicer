// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2017 Amir Vaxman <avaxman@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include <igl/shapeup.h>
#include <igl/min_quad_with_fixed.h>
#include <igl/igl_inline.h>
#include <igl/setdiff.h>
#include <igl/cat.h>
#include <igl/PI.h>
#include <Eigen/Core>
#include <vector>

namespace igl
{
    
  //This projection does nothing but render points into projP. Mostly used for "echoing" the global step
  IGL_INLINE bool shapeup_identity_projection(const Eigen::PlainObjectBase<Eigen::MatrixXd>& P, const Eigen::PlainObjectBase<Eigen::VectorXi>& SC, const Eigen::PlainObjectBase<Eigen::MatrixXi>& S,  Eigen::PlainObjectBase<Eigen::MatrixXd>& projP){
    projP.conservativeResize(SC.rows(), 3*SC.maxCoeff());
    for (int i=0;i<S.rows();i++){
      Eigen::RowVector3d avgCurrP=Eigen::RowVector3d::Zero();
      for (int j=0;j<SC(i);j++)
        avgCurrP+=P.row(S(i,j))/(double)(SC(i));
  
      for (int j=0;j<SC(i);j++)
        projP.block(i,3*j,1,3)=P.row(S(i,j))-avgCurrP;
    }
    return true;
  }
  
  
  //the projection assumes that the sets are vertices of polygons in order
  IGL_INLINE bool shapeup_regular_face_projection(const Eigen::PlainObjectBase<Eigen::MatrixXd>& P, const Eigen::PlainObjectBase<Eigen::VectorXi>& SC, const Eigen::PlainObjectBase<Eigen::MatrixXi>& S,  Eigen::PlainObjectBase<Eigen::MatrixXd>& projP){
    projP.conservativeResize(SC.rows(), 3*SC.maxCoeff());
    for (int currRow=0;currRow<SC.rows();currRow++){
    //computing average
      int N=SC(currRow);
      const Eigen::RowVectorXi SRow=S.row(currRow);
      Eigen::RowVector3d avgCurrP=Eigen::RowVector3d::Zero();
      Eigen::MatrixXd targetPolygon(N, 3);
      Eigen::MatrixXd sourcePolygon(N, 3);
      for (int j=0;j<N;j++)
        avgCurrP+=P.row(SRow(j))/(double)(N);
  
      for (int j=0;j<N;j++)
        targetPolygon.row(j)=P.row(SRow(j))-avgCurrP;
  
      //creating perfectly regular source polygon
      for (int j=0;j<N;j++)
        sourcePolygon.row(j)<<cos(2*igl::PI*(double)j/(double(N))), sin(2*igl::PI*(double)j/(double(N))),0.0;
  
      //finding closest similarity transformation between source and target
      Eigen::MatrixXd corrMat=sourcePolygon.transpose()*targetPolygon;
      Eigen::JacobiSVD<Eigen::Matrix3d> svd(corrMat, Eigen::ComputeFullU | Eigen::ComputeFullV);
      Eigen::MatrixXd R=svd.matrixU()*svd.matrixV().transpose();
      //getting scale by edge length change average. TODO: by singular values
      Eigen::VectorXd sourceEdgeLengths(N);
      Eigen::VectorXd targetEdgeLengths(N);
      for (int j=0;j<N;j++){
        sourceEdgeLengths(j)=(sourcePolygon.row((j+1)%N)-sourcePolygon.row(j)).norm();
        targetEdgeLengths(j)=(targetPolygon.row((j+1)%N)-targetPolygon.row(j)).norm();
      }
      double scale=(targetEdgeLengths.cwiseQuotient(sourceEdgeLengths)).mean();
  
      for (int j=0;j<N;j++)
        projP.block(currRow,3*j,1,3)=sourcePolygon.row(j)*R*scale;
    }
  
    return true;
  }

  template <
  typename DerivedP,
  typename DerivedSC,
  typename DerivedS,
  typename Derivedw>
  IGL_INLINE bool shapeup_precomputation(const Eigen::PlainObjectBase<DerivedP>& P,
                                         const Eigen::PlainObjectBase<DerivedSC>& SC,
                                         const Eigen::PlainObjectBase<DerivedS>& S,
                                         const Eigen::PlainObjectBase<DerivedS>& E,
                                         const Eigen::PlainObjectBase<DerivedSC>& b,
                                         const Eigen::PlainObjectBase<Derivedw>& wShape,
                                         const Eigen::PlainObjectBase<Derivedw>& wSmooth,
                                         ShapeupData & sudata)
  {
      using namespace std;
      using namespace Eigen;
      sudata.P=P;
      sudata.SC=SC;
      sudata.S=S;
      sudata.b=b;
      typedef typename DerivedP::Scalar Scalar;
      
      //checking for consistency of the input
      assert(SC.rows()==S.rows());
      assert(SC.rows()==wShape.rows());
      assert(E.rows()==wSmooth.rows());
      assert(b.rows()!=0);  //would lead to matrix becoming SPD
      
      sudata.DShape.conservativeResize(SC.sum(), P.rows());  //Shape matrix (integration);
      sudata.DClose.conservativeResize(b.rows(), P.rows());  //Closeness matrix for positional constraints
      sudata.DSmooth.conservativeResize(E.rows(), P.rows());  //smoothness matrix
        
      //Building shape matrix
      std::vector<Triplet<Scalar> > DShapeTriplets;
      int currRow=0;
      for (int i=0;i<S.rows();i++){
          Scalar avgCoeff=1.0/(Scalar)SC(i);
            
          for (int j=0;j<SC(i);j++){
            for (int k=0;k<SC(i);k++){
              if (j==k)
                DShapeTriplets.push_back(Triplet<Scalar>(currRow+j, S(i,k), (1.0-avgCoeff)));
              else
                DShapeTriplets.push_back(Triplet<Scalar>(currRow+j, S(i,k), (-avgCoeff)));
            }
          }
        currRow+=SC(i);
        
      }
 
      sudata.DShape.setFromTriplets(DShapeTriplets.begin(), DShapeTriplets.end());

      //Building closeness matrix
      std::vector<Triplet<Scalar> > DCloseTriplets;
      for (int i=0;i<b.size();i++)
        DCloseTriplets.push_back(Triplet<Scalar>(i,b(i), 1.0));
      
      sudata.DClose.setFromTriplets(DCloseTriplets.begin(), DCloseTriplets.end());
      
      //Building smoothness matrix
      std::vector<Triplet<Scalar> > DSmoothTriplets;
      for (int i=0; i<E.rows(); i++) {
        DSmoothTriplets.push_back(Triplet<Scalar>(i, E(i, 0), -1));
        DSmoothTriplets.push_back(Triplet<Scalar>(i, E(i, 1), 1));
      }
        
      SparseMatrix<Scalar> tempMat;
      igl::cat(1, sudata.DShape, sudata.DClose, tempMat);
      igl::cat(1, tempMat, sudata.DSmooth, sudata.A);
        
      //weight matrix
      vector<Triplet<Scalar> > WTriplets;
        
      //one weight per set in S.
      currRow=0;
      for (int i=0;i<SC.rows();i++){
          for (int j=0;j<SC(i);j++)
              WTriplets.push_back(Triplet<double>(currRow+j,currRow+j,sudata.shapeCoeff*wShape(i)));
          currRow+=SC(i);
      }
        
      for (int i=0;i<b.size();i++)
          WTriplets.push_back(Triplet<double>(SC.sum()+i, SC.sum()+i, sudata.closeCoeff));
        
      for (int i=0;i<E.rows();i++)
          WTriplets.push_back(Triplet<double>(SC.sum()+b.size()+i, SC.sum()+b.size()+i, sudata.smoothCoeff*wSmooth(i)));
        
      sudata.W.conservativeResize(SC.sum()+b.size()+E.rows(), SC.sum()+b.size()+E.rows());
      sudata.W.setFromTriplets(WTriplets.begin(), WTriplets.end());
        
      sudata.At=sudata.A.transpose();  //for efficieny, as we use the transpose a lot in the iteration
      sudata.Q=sudata.At*sudata.W*sudata.A;

      return min_quad_with_fixed_precompute(sudata.Q,VectorXi(),SparseMatrix<double>(),true,sudata.solver_data);
  }


  template <
  typename DerivedP,
  typename DerivedSC,
  typename DerivedS>
  IGL_INLINE bool shapeup_solve(const Eigen::PlainObjectBase<DerivedP>& bc,
                                const std::function<bool(const Eigen::PlainObjectBase<DerivedP>&, const Eigen::PlainObjectBase<DerivedSC>&, const Eigen::PlainObjectBase<DerivedS>&,  Eigen::PlainObjectBase<DerivedP>&)>& local_projection,
                                const Eigen::PlainObjectBase<DerivedP>& P0,
                                const ShapeupData & sudata,
                                const bool quietIterations,
                                Eigen::PlainObjectBase<DerivedP>& P)
  {
    using namespace Eigen;
    using namespace std;
    MatrixXd currP=P0;
    MatrixXd prevP=P0;
    MatrixXd projP;
    
    assert(bc.rows()==sudata.b.rows());
    
		MatrixXd rhs(sudata.A.rows(), 3); rhs.setZero();
    rhs.block(sudata.DShape.rows(), 0, sudata.b.rows(),3)=bc;  //this stays constant throughout the iterations
        
    if (!quietIterations){
        cout<<"Shapeup Iterations, "<<sudata.DShape.rows()<<" constraints, solution size "<<P0.size()<<endl;
        cout<<"**********************************************************************************************"<<endl;
    }
    projP.conservativeResize(sudata.SC.rows(), 3*sudata.SC.maxCoeff());
    for (int iter=0;iter<sudata.maxIterations;iter++){
      
      local_projection(currP, sudata.SC,sudata.S,projP);
            
      //constructing the projection part of the (DShape rows of the) right hand side
      int currRow=0;
      for (int i=0;i<sudata.S.rows();i++)
        for (int j=0;j<sudata.SC(i);j++)
          rhs.row(currRow++)=projP.block(i, 3*j, 1,3);
      
      DerivedP lsrhs=-sudata.At*sudata.W*rhs;
      MatrixXd Y(0,3), Beq(0,3);  //We do not use the min_quad_solver fixed variables mechanism; they are treated with the closeness energy of ShapeUp.
      min_quad_with_fixed_solve(sudata.solver_data, lsrhs,Y,Beq,currP);
      
      double currChange=(currP-prevP).lpNorm<Infinity>();
      if (!quietIterations)
        cout << "Iteration "<<iter<<", integration Linf error: "<<currChange<< endl;
      prevP=currP;
      if (currChange<sudata.pTolerance){
        P=currP;
        return true;
      }
    }
    
    P=currP;
    return false;  //we went over maxIterations
    
  }
}





#ifdef IGL_STATIC_LIBRARY
template bool igl::shapeup_precomputation< typename Eigen::Matrix<double, -1, -1, 0, -1, -1>, typename Eigen::Matrix<int, -1, 1, 0, -1, 1>, typename Eigen::Matrix<int, -1, -1, 0, -1, -1>, typename Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&,   Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, igl::ShapeupData&);

template bool igl::shapeup_solve<typename Eigen::Matrix<double, -1, -1, 0, -1, -1>, typename Eigen::Matrix<int, -1, 1, 0, -1, 1>, typename Eigen::Matrix<int, -1, -1, 0, -1, -1> >(const Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >& bc, const std::function<bool(const Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, const Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, const Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&,  Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >& ) >& local_projection, const Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >& P0, const igl::ShapeupData & sudata, const bool quietIterations, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >& P);
#endif
