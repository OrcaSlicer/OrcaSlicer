#include "quadprog.h"
#include "min_quad_with_fixed.h"
#include <iostream>

template <typename Scalar, int n, int m>
IGL_INLINE Eigen::Matrix<Scalar,n,1> igl::quadprog(
  const Eigen::Matrix<Scalar,n,n> & H,
  const Eigen::Matrix<Scalar,n,1> & f,
  const Eigen::Matrix<Scalar,m,n> & A,
  const Eigen::Matrix<Scalar,m,1> & b,
  const Eigen::Matrix<Scalar,n,1> & lb,
  const Eigen::Matrix<Scalar,n,1> & ub)
{
  // Alec 16/2/2021:
  // igl::quadprog implements a very simple primal active set method. The new
  // igl::min_quad_with_fixed is very fast for small dense problems so the
  // iterations of igl::quadprog become very fast. Even if it ends up doing many
  // more iterations than igl::copyleft::quadprog it would be much faster (in
  // reality it doesn't do that many more iterations). It's a healthy 10-100x
  // faster than igl::copyleft::quadprog for specific cases of QPs.
  //
  // Unfortunately, that set is limited. igl::quadprog is really good at tiny
  // box-constrained QPs with a positive definite objective (like the kind that show
  // up in dual contouring). igl::copyleft::quadprog handles more general problems
  // (and also starts to beat igl::quadprog when the number of variables gets over
  // ~20). I tried extending igl::quadprog so that we could use it for
  // igl::copyleft::progressive_hulls and drop igl::copyleft::quadprog but it was
  // trickier than I thought. Something like qpmad or the non GPL version of
  // quadrog++ would be good future PR.
  //
  typedef Eigen::Matrix<Scalar,n,1> VectorSn;
  typedef Eigen::Array<bool,n,1>    Arraybn;
  assert( (lb.array() < ub.array() ).all() );
  const int dyn_n = n == Eigen::Dynamic ? H.rows() : n;
  VectorSn x(dyn_n);
  VectorSn bc = VectorSn::Constant(dyn_n,1,-1e26);
  Arraybn k = Arraybn::Constant(dyn_n,1,false);
  Eigen::Index iter;
  // n³ is probably way too conservative. 
  for(iter = 0;iter<dyn_n*dyn_n*dyn_n;iter++)
  {
    // For dual contouring 99% of the time the active set is empty.
    // Optimize for this common case.
    // Windows needs template arguments spelled out
    x = min_quad_with_fixed<Scalar,n,m>(H,f,k,bc,A,b);
    // constraint violations 
    VectorSn vl = lb-x;
    VectorSn vu = x-ub;

    // try to add/remove constraints
    Eigen::Index best_add = -1; Scalar worst_offense = 0;
    bool add_lower;
    Eigen::Index best_remove = -1; Scalar worst_lambda = 0;
    for(Eigen::Index i = 0;i<dyn_n;i++)
    {
      if(vl(i)>worst_offense)
      {
        best_add = i;
        add_lower = true;
        worst_offense = vl(i);
      }
      if(vu(i)>worst_offense)
      {
        best_add = i;
        add_lower = false;
        worst_offense = vu(i);
      }
      // bias toward adding constraints
      if(best_add<0 && k(i))
      {
        const Scalar sign = bc(i)==ub(i)?1:-1;
        const Scalar lambda_i = sign * (H.row(i)*x+f(i));
        if(lambda_i > worst_lambda)
        {
          best_remove = i;
          worst_lambda = lambda_i;
        }
      }
    }
    // bias toward adding constraints
    if(best_add >= 0)
    {
      const auto i = best_add;
      assert(!k(i));
      bc(i) = add_lower ? lb(i) : ub(i);
      k(i) = true;
    }else if(best_remove >= 0)
    {
      const auto i = best_remove;
      assert(k(i));
      k(i) = false;
    }else /*if(best_add < 0 && best_remove < 0)*/
    {
      return x;
    }
  }
  // Should never happen.
  assert(false && "quadprog failed after too many iterations");
  return VectorSn::Zero(dyn_n);
}

template <typename Scalar, int n>
IGL_INLINE Eigen::Matrix<Scalar,n,1> igl::quadprog(
  const Eigen::Matrix<Scalar,n,n> & H,
  const Eigen::Matrix<Scalar,n,1> & f,
  const Eigen::Matrix<Scalar,n,1> & lb,
  const Eigen::Matrix<Scalar,n,1> & ub)
{
  const int m = n == Eigen::Dynamic ? Eigen::Dynamic : 0;
  // Windows needs template parameters spelled out
  return quadprog<Scalar,n,m>(
    H,f,
    Eigen::Matrix<Scalar,m,n>(0,H.cols()),
    Eigen::Matrix<Scalar,m,1>(0,1),
    lb,ub);
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template Eigen::Matrix<double, 2, 1, ((Eigen::StorageOptions)0) | ((((2) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 2, 1> igl::quadprog<double, 2>(Eigen::Matrix<double, 2, 2, ((Eigen::StorageOptions)0) | ((((2) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)1) : ((((2) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 2, 2> const&, Eigen::Matrix<double, 2, 1, ((Eigen::StorageOptions)0) | ((((2) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 2, 1> const&, Eigen::Matrix<double, 2, 1, ((Eigen::StorageOptions)0) | ((((2) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 2, 1> const&, Eigen::Matrix<double, 2, 1, ((Eigen::StorageOptions)0) | ((((2) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 2, 1> const&);
template Eigen::Matrix<double, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> igl::quadprog<double, 3>(Eigen::Matrix<double, 3, 3, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)1) : ((((3) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 3> const&, Eigen::Matrix<double, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> const&, Eigen::Matrix<double, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> const&, Eigen::Matrix<double, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> const&);
template Eigen::Matrix<float, 2, 1, ((Eigen::StorageOptions)0) | ((((2) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 2, 1> igl::quadprog<float, 2>(Eigen::Matrix<float, 2, 2, ((Eigen::StorageOptions)0) | ((((2) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)1) : ((((2) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 2, 2> const&, Eigen::Matrix<float, 2, 1, ((Eigen::StorageOptions)0) | ((((2) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 2, 1> const&, Eigen::Matrix<float, 2, 1, ((Eigen::StorageOptions)0) | ((((2) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 2, 1> const&, Eigen::Matrix<float, 2, 1, ((Eigen::StorageOptions)0) | ((((2) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((2) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 2, 1> const&);
template Eigen::Matrix<float, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> igl::quadprog<float, 3>(Eigen::Matrix<float, 3, 3, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)1) : ((((3) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 3> const&, Eigen::Matrix<float, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> const&, Eigen::Matrix<float, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> const&, Eigen::Matrix<float, 3, 1, ((Eigen::StorageOptions)0) | ((((3) == (1)) && ((1) != (1))) ? ((Eigen::StorageOptions)1) : ((((1) == (1)) && ((3) != (1))) ? ((Eigen::StorageOptions)0) : ((Eigen::StorageOptions)0))), 3, 1> const&);
#endif
