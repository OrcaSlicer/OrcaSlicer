#include "pso.h"
#include "IGL_ASSERT.h"
#include <Eigen/StdVector>
#include <vector>
#include <iostream>

template <
  typename Scalar, 
  typename DerivedX,
  typename DerivedLB, 
  typename DerivedUB>
IGL_INLINE Scalar igl::pso(
  const std::function< Scalar (DerivedX &) > f,
  const Eigen::MatrixBase<DerivedLB> & LB,
  const Eigen::MatrixBase<DerivedUB> & UB,
  const int max_iters,
  const int population,
  DerivedX & X)
{
  const Eigen::Array<bool,Eigen::Dynamic,1> P =
    Eigen::Array<bool,Eigen::Dynamic,1>::Zero(LB.size(),1);
  return igl::pso(f,LB,UB,P,max_iters,population,X);
}

template <
  typename Scalar, 
  typename DerivedX,
  typename DerivedLB, 
  typename DerivedUB,
  typename DerivedP>
IGL_INLINE Scalar igl::pso(
  const std::function< Scalar (DerivedX &) > f,
  const Eigen::MatrixBase<DerivedLB> & LB,
  const Eigen::MatrixBase<DerivedUB> & UB,
  const Eigen::DenseBase<DerivedP> & P,
  const int max_iters,
  const int population,
  DerivedX & X)
{
  const int dim = LB.size();
  assert(UB.size() == dim && "UB should match LB size");
  IGL_ASSERT(P.size() == dim && "P should match LB size");
  typedef std::vector<DerivedX,Eigen::aligned_allocator<DerivedX> > VectorList;
  VectorList position(population);
  VectorList best_position(population);
  VectorList velocity(population);
  Eigen::Matrix<Scalar,Eigen::Dynamic,1> best_f(population);
  // https://en.wikipedia.org/wiki/Particle_swarm_optimization#Algorithm
  //
  // g → X
  // p_i → best[i]
  // v_i → velocity[i]
  // x_i → position[i]
  Scalar min_f = std::numeric_limits<Scalar>::max();
  for(int p=0;p<population;p++)
  {
    {
      const DerivedX R = DerivedX::Random(dim).array()*0.5+0.5;
      position[p] = LB.array() + R.array()*(UB-LB).array();
    }
    best_f[p] = f(position[p]);
    best_position[p] = position[p];
    if(best_f[p] < min_f)
    {
      min_f = best_f[p];
      X = best_position[p];
    }
    {
      const DerivedX R = DerivedX::Random(dim);
      velocity[p] = (UB-LB).array() * R.array();
    }
  }

  int iter = 0;
  Scalar omega = 0.98;
  Scalar phi_p = 0.01;
  Scalar phi_g = 0.01;
  while(true)
  {
    //if(iter % 10 == 0)
    //{
    //  std::cout<<iter<<":"<<std::endl;
    //  for(int p=0;p<population;p++)
    //  {
    //    std::cout<<"  "<<best_f[p]<<", "<<best_position[p]<<std::endl;
    //  }
    //  std::cout<<std::endl;
    //}

    for(int p=0;p<population;p++)
    {
      const DerivedX R_p = DerivedX::Random(dim).array()*0.5+0.5;
      const DerivedX R_g = DerivedX::Random(dim).array()*0.5+0.5;
      velocity[p] = 
        omega * velocity[p].array() +
        phi_p * R_p.array() *(best_position[p] - position[p]).array() + 
        phi_g * R_g.array() *(               X - position[p]).array();
      position[p] += velocity[p];
      // Clamp to bounds
      for(int d = 0;d<dim;d++)
      {
//#define IGL_PSO_REFLECTION
#ifdef IGL_PSO_REFLECTION
        assert(!P(d));
        // Reflect velocities if exceeding bounds
        if(position[p](d) < LB(d))
        {
          position[p](d) = LB(d);
          if(velocity[p](d) < 0.0) velocity[p](d) *= -1.0;
        }
        if(position[p](d) > UB(d))
        {
          position[p](d) = UB(d);
          if(velocity[p](d) > 0.0) velocity[p](d) *= -1.0;
        }
#else
//#warning "trying no bounds on periodic"
//        // TODO: I'm not sure this is the right thing to do/enough. The
//        // velocities could be weird. Suppose the current "best" value is ε and
//        // the value is -ε and the "periodic bounds" [0,2π]. Moding will send
//        // the value to 2π-ε but the "velocity" term will now be huge pointing
//        // all the way from 2π-ε to ε.
//        //
//        // Q: Would it be enough to try (all combinations) of ±(UB-LB) before
//        // computing velocities to "best"s? In the example above, instead of
//        //
//        //     v += best - p = ε - (2π-ε) = -2π+2ε
//        //
//        // you'd use
//        //
//        //     v +=  / argmin  |b - p|            \  - p = (ε+2π)-(2π-ε) = 2ε
//        //          |                              |
//        //           \ b∈{best, best+2π, best-2π} /
//        //
//        // Though, for multivariate b,p,v this would seem to explode
//        // combinatorially.
//        //
//        // Maybe periodic things just shouldn't be bounded and we hope that the
//        // forces toward the current minima "regularize" them away from insane
//        // values.
//        if(P(d))
//        {
//          position[p](d) = std::fmod(position[p](d)-LB(d),UB(d)-LB(d))+LB(d);
//        }else
//        {
//          position[p](d) = std::max(LB(d),std::min(UB(d),position[p](d)));
//        }
        position[p](d) = std::max(LB(d),std::min(UB(d),position[p](d)));
#endif
      }
      const Scalar fp = f(position[p]);
      if(fp<best_f[p])
      {
        best_f[p] = fp;
        best_position[p] = position[p];
        if(best_f[p] < min_f)
        {
          min_f = best_f[p];
          X = best_position[p];
        }
      }
    }
    iter++;
    if(iter>=max_iters)
    {
      break;
    }
  }
  return min_f;
}

#ifdef IGL_STATIC_LIBRARY
template float igl::pso<float, Eigen::Matrix<float, 1, -1, 1, 1, -1>, Eigen::Matrix<float, 1, -1, 1, 1, -1>, Eigen::Matrix<float, 1, -1, 1, 1, -1> >(std::function<float (Eigen::Matrix<float, 1, -1, 1, 1, -1>&)>, Eigen::MatrixBase<Eigen::Matrix<float, 1, -1, 1, 1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, -1, 1, 1, -1> > const&, int, int, Eigen::Matrix<float, 1, -1, 1, 1, -1>&);
#endif
