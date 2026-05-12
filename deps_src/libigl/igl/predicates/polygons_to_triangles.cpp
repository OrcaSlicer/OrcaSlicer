#include "polygons_to_triangles.h"
#include "ear_clipping.h"
#include "../sort.h"
#include "../placeholders.h"
#include "../PlainMatrix.h"
#include <Eigen/Eigenvalues>

template <
  typename DerivedV,
  typename DerivedI,
  typename DerivedC,
  typename DerivedF,
  typename DerivedJ>
IGL_INLINE void igl::predicates::polygons_to_triangles(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedI> & I,
  const Eigen::MatrixBase<DerivedC> & C,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedJ> & J)
{
  typedef Eigen::Index Index;
  // Each polygon results in #sides-2 triangles. So ∑#sides-2
  F.resize(C(C.size()-1) - (C.size()-1)*2,3);
  J.resize(F.rows());
  {
    Index f = 0;
    for(Index p = 0;p<C.size()-1;p++)
    {
      const Index np = C(p+1)-C(p);
      Eigen::MatrixXi pF;
      if(np == 3)
      {
        pF = (Eigen::MatrixXi(1,3)<<0,1,2).finished();
      }else
      {
        // Make little copy of this polygon with an initial fan
        PlainMatrix<DerivedV,Eigen::Dynamic> pV(np,V.cols());
        for(Index c = 0;c<np;c++)
        {
          pV.row(c) = V.row(I(C(p)+c));
        }
        // Use PCA to project to 2D
        Eigen::MatrixXd S;
        switch(V.cols())
        {
          case 2:
            S = V.template cast<double>();
            break;
          case 3:
          {
            Eigen::MatrixXd P = (pV.rowwise() - pV.colwise().mean()).template cast<double>();
            Eigen::Matrix3d O = P.transpose() * P;
            Eigen::EigenSolver<Eigen::Matrix3d> es(O);
            Eigen::Matrix3d C = es.eigenvectors().real();
            {
              Eigen::Vector3d _1;
              Eigen::Vector3i I;
              igl::sort(es.eigenvalues().real().eval(),1,false,_1,I);
              C = C(igl::placeholders::all,I).eval();
            }
            S = P*C.leftCols(2);
            break;
          }
          default: assert(false && "dim>3 not supported");
        }

        Eigen::VectorXi RT = Eigen::VectorXi::Zero(S.rows(),1);
        Eigen::VectorXi _I;
        Eigen::MatrixXd _nS;

        // compute signed area
        {
          double area = 0;
          for(Index c = 0;c<np;c++)
          {
            area += S((c+0)%np,0)*S((c+1)%np,1) - S((c+1)%np,0)*S((c+0)%np,1);
          }
          //prIndexf("area: %g\n",area);
          if(area<0)
          {
            S.col(0) *= -1;
          }
        }

        // This is a really low quality triangulator and will contain nearly
        // degenerate elements which become degenerate or worse when unprojected
        // back to 3D.
        // igl::predicates::ear_clipping does not gracefully fail when the input
        // is not simple. Instead it (tends?) to output too few triangles.
        if(! igl::predicates::ear_clipping(S,pF) )
        {
          // Fallback, use a fan
          //std::cout<<igl::matlab_format(S,"S")<<std::endl;
          //std::cout<<igl::matlab_format(RT,"RT")<<std::endl;
          //std::cout<<igl::matlab_format(_I,"I")<<std::endl;
          //std::cout<<igl::matlab_format(pF,"pF")<<std::endl;
          //std::cout<<igl::matlab_format(_nS,"nS")<<std::endl;
          //std::cout<<std::endl;

          pF.resize(np-2,3);
          for(Index c = 0;c<np;c++)
          {
            if(c>0 && c<np-1)
            {
              pF(c-1,0) = 0;
              pF(c-1,1) = c;
              pF(c-1,2) = c+1;
            }
          }
        }
        assert(pF.rows() == np-2);

        // Could at least flip edges of degenerate edges

        //if(pF.rows()>1)
        //{
        //  // Delaunay-ize 
        //  Eigen::MatrixXd pl;
        //  igl::edge_lengths(pV,pF,pl);

        //  typedef Eigen::Matrix<Index,Eigen::Dynamic,2> MatrixX2I;
        //  typedef Eigen::Matrix<Index,Eigen::Dynamic,1> VectorXI;
        //  MatrixX2I E,uE;
        //  VectorXI EMAP;
        //  std::vector<std::vector<Index> > uE2E;
        //  igl::unique_edge_map(pF, E, uE, EMAP, uE2E);
        //  typedef Index Index;
        //  typedef double Scalar;
        //  const Index num_faces = pF.rows();
        //  std::vector<Index> Q;
        //  Q.reserve(uE2E.size());
        //  for (size_t uei=0; uei<uE2E.size(); uei++) 
        //  {
        //    Q.push_back(uei);
        //  }
        //  while(!Q.empty())
        //  {
        //    const Index uei = Q.back();
        //    Q.pop_back();
        //    if (uE2E[uei].size() == 2) 
        //    {
        //      double w;
        //      igl::is_Indexrinsic_delaunay(pl,uE2E,num_faces,uei,w);
        //      prIndexf("%d : %0.17f\n",uei,w);
        //      if(w<-1e-7) 
        //      {
        //        prIndexf("  flippin'\n");
        //        //
        //        //          v1                 v1
        //        //          /|\                / \
        //        //        c/ | \b            c/f1 \b
        //        //     v3 /f2|f1\ v4  =>  v3 /__f__\ v4
        //        //        \  e  /            \ f2  /
        //        //        d\ | /a            d\   /a
        //        //          \|/                \ /
        //        //          v2                 v2
        //        //
        //        // hmm... is the flip actually in the other direction?
        //        const Index f1 = uE2E[uei][0]%num_faces;
        //        const Index f2 = uE2E[uei][1]%num_faces;
        //        const Index c1 = uE2E[uei][0]/num_faces;
        //        const Index c2 = uE2E[uei][1]/num_faces;
        //        const size_t e_24 = f1 + ((c1 + 1) % 3) * num_faces;
        //        const size_t e_41 = f1 + ((c1 + 2) % 3) * num_faces;
        //        const size_t e_13 = f2 + ((c2 + 1) % 3) * num_faces;
        //        const size_t e_32 = f2 + ((c2 + 2) % 3) * num_faces;
        //        const size_t ue_24 = EMAP(e_24);
        //        const size_t ue_41 = EMAP(e_41);
        //        const size_t ue_13 = EMAP(e_13);
        //        const size_t ue_32 = EMAP(e_32);
        //        // new edge lengths
        //        const Index v1 = pF(f1, (c1+1)%3);
        //        const Index v2 = pF(f1, (c1+2)%3);
        //        const Index v4 = pF(f1, c1);
        //        const Index v3 = pF(f2, c2);
        //        {
        //          const Scalar e = pl(f1,c1);
        //          const Scalar a = pl(f1,(c1+1)%3);
        //          const Scalar b = pl(f1,(c1+2)%3);
        //          const Scalar c = pl(f2,(c2+1)%3);
        //          const Scalar d = pl(f2,(c2+2)%3);
        //          const double f = (pV.row(v3)-pV.row(v4)).norm();
        //          // New order
        //          pl(f1,0) = f;
        //          pl(f1,1) = b;
        //          pl(f1,2) = c;
        //          pl(f2,0) = f;
        //          pl(f2,1) = d;
        //          pl(f2,2) = a;
        //        }
        //        prIndexf("%d,%d %d,%d -> %d,%d\n",uE(uei,0),uE(uei,1),v1,v2,v3,v4);
        //        igl::flip_edge(pF, E, uE, EMAP, uE2E, uei);
        //        std::cout<<"  "<<pl.row(f1)<<std::endl;
        //        std::cout<<"  "<<pl.row(f2)<<std::endl;
        //        //// new edge lengths, slow!
        //        //igl::edge_lengths(pV,pF,pl);
        //        // recompute edge lengths of two faces. (extra work on untouched
        //        // edges)
        //        for(Index f : {f1,f2})
        //        {
        //          for(Index c=0;c<3;c++)
        //          {
        //            pl(f,c) = 
        //              (pV.row(pF(f,(c+1)%3))-pV.row(pF(f,(c+2)%3))).norm();
        //          }
        //        }
        //        std::cout<<"  "<<pl.row(f1)<<std::endl;
        //        std::cout<<"  "<<pl.row(f2)<<std::endl;
        //        std::cout<<std::endl;

        //        Q.push_back(ue_24);
        //        Q.push_back(ue_41);
        //        Q.push_back(ue_13);
        //        Q.push_back(ue_32);
        //      }
        //    }
        //  }


        //  // check for self-loops (I claim these cannot happen)
        //  for(Index f = 0;f<pF.rows();f++)
        //  {
        //    for(Index c =0;c<3;c++)
        //    {
        //      assert(pF(f,c) != pF(f,(c+1)%3) && "self loops should not exist");
        //    }
        //  }
        //}
      }
      // Copy Indexo global list
      for(Index i = 0;i<pF.rows();i++)
      {
        for(Index c =0;c<3;c++)
        {
          F(f,c) = I(C(p)+pF(i,c));
        }
        J(f) = p;
        f++;
      }

    }
    assert(f == F.rows());
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
