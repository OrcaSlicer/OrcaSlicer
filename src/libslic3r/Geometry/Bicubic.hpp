#ifndef BICUBIC_HPP
#define BICUBIC_HPP

#include <algorithm>
#include <vector>
#include <cmath>

#include <Eigen/Dense>

namespace Slic3r {

namespace Geometry {

namespace BicubicInternal {
// Linear kernel, to be able to test cubic methods with hat kernels.
template<typename T>
struct LinearKernel
{
    typedef T FloatType;

    static T a00() {
        return T(0.);
    }
    static T a01() {
        return T(0.);
    }
    static T a02() {
        return T(0.);
    }
    static T a03() {
        return T(0.);
    }
    static T a10() {
        return T(1.);
    }
    static T a11() {
        return T(-1.);
    }
    static T a12() {
        return T(0.);
    }
    static T a13() {
        return T(0.);
    }
    static T a20() {
        return T(0.);
    }
    static T a21() {
        return T(1.);
    }
    static T a22() {
        return T(0.);
    }
    static T a23() {
        return T(0.);
    }
    static T a30() {
        return T(0.);
    }
    static T a31() {
        return T(0.);
    }
    static T a32() {
        return T(0.);
    }
    static T a33() {
        return T(0.);
    }
};

// Interpolation kernel aka Catmul-Rom aka Keyes kernel.
template<typename T>
struct CubicCatmulRomKernel
{
    typedef T FloatType;

    static T a00() {
        return 0;
    }
    static T a01() {
        return T( -0.5);
    }
    static T a02() {
        return T( 1.);
    }
    static T a03() {
        return T( -0.5);
    }
    static T a10() {
        return T( 1.);
    }
    static T a11() {
        return 0;
    }
    static T a12() {
        return T( -5. / 2.);
    }
    static T a13() {
        return T( 3. / 2.);
    }
    static T a20() {
        return 0;
    }
    static T a21() {
        return T( 0.5);
    }
    static T a22() {
        return T( 2.);
    }
    static T a23() {
        return T( -3. / 2.);
    }
    static T a30() {
        return 0;
    }
    static T a31() {
        return 0;
    }
    static T a32() {
        return T( -0.5);
    }
    static T a33() {
        return T( 0.5);
    }
};

// B-spline kernel
template<typename T>
struct CubicBSplineKernel
{
    typedef T FloatType;

    static T a00() {
        return T( 1. / 6.);
    }
    static T a01() {
        return T( -3. / 6.);
    }
    static T a02() {
        return T( 3. / 6.);
    }
    static T a03() {
        return T( -1. / 6.);
    }
    static T a10() {
        return T( 4. / 6.);
    }
    static T a11() {
        return 0;
    }
    static T a12() {
        return T( -6. / 6.);
    }
    static T a13() {
        return T( 3. / 6.);
    }
    static T a20() {
        return T( 1. / 6.);
    }
    static T a21() {
        return T( 3. / 6.);
    }
    static T a22() {
        return T( 3. / 6.);
    }
    static T a23() {
        return T( -3. / 6.);
    }
    static T a30() {
        return 0;
    }
    static T a31() {
        return 0;
    }
    static T a32() {
        return 0;
    }
    static T a33() {
        return T( 1. / 6.);
    }
};

template<class T>
inline T clamp(T a, T lower, T upper)
        {
    return (a < lower) ? lower :
           (a > upper) ? upper : a;
}
}

template<typename Kernel>
struct CubicKernelWrapper
{
    typedef typename Kernel::FloatType FloatType;

    static constexpr size_t kernel_span = 4;

    static FloatType kernel(FloatType x)
            {
        x = fabs(x);
        if (x >= (FloatType) 2.)
            return 0.0f;
        if (x <= (FloatType) 1.) {
            FloatType x2 = x * x;
            FloatType x3 = x2 * x;
            return Kernel::a10() + Kernel::a11() * x + Kernel::a12() * x2 + Kernel::a13() * x3;
        }
        assert(x > (FloatType )1. && x < (FloatType )2.);
        x -= (FloatType) 1.;
        FloatType x2 = x * x;
        FloatType x3 = x2 * x;
        return Kernel::a00() + Kernel::a01() * x + Kernel::a02() * x2 + Kernel::a03() * x3;
    }

    static FloatType interpolate(FloatType f0, FloatType f1, FloatType f2, FloatType f3, FloatType x)
            {
        const FloatType x2 = x * x;
        const FloatType x3 = x * x * x;
        return f0 * (Kernel::a00() + Kernel::a01() * x + Kernel::a02() * x2 + Kernel::a03() * x3) +
                f1 * (Kernel::a10() + Kernel::a11() * x + Kernel::a12() * x2 + Kernel::a13() * x3) +
                f2 * (Kernel::a20() + Kernel::a21() * x + Kernel::a22() * x2 + Kernel::a23() * x3) +
                f3 * (Kernel::a30() + Kernel::a31() * x + Kernel::a32() * x2 + Kernel::a33() * x3);
    }
};

// Linear splines
template<typename NumberType>
using LinearKernel = CubicKernelWrapper<BicubicInternal::LinearKernel<NumberType>>;

// Catmul-Rom splines
template<typename NumberType>
using CubicCatmulRomKernel = CubicKernelWrapper<BicubicInternal::CubicCatmulRomKernel<NumberType>>;

// Cubic B-splines
template<typename NumberType>
using CubicBSplineKernel = CubicKernelWrapper<BicubicInternal::CubicBSplineKernel<NumberType>>;

template<typename KernelWrapper>
static typename KernelWrapper::FloatType cubic_interpolate(const Eigen::ArrayBase<typename KernelWrapper::FloatType> &F,
        const typename KernelWrapper::FloatType pt) {
    typedef typename KernelWrapper::FloatType T;
    const int w = int(F.size());
    const int ix = (int) floor(pt);
    const T s = pt - T( ix);

    if (ix > 1 && ix + 2 < w) {
        // Inside the fully interpolated region.
        return KernelWrapper::interpolate(F[ix - 1], F[ix], F[ix + 1], F[ix + 2], s);
    }
    // Transition region. Extend with a constant function.
    auto f = [&F, w](T x) {
        return F[BicubicInternal::clamp(x, 0, w - 1)];
    };
    return KernelWrapper::interpolate(f(ix - 1), f(ix), f(ix + 1), f(ix + 2), s);
}

template<typename Kernel, typename Derived>
static float bicubic_interpolate(const Eigen::MatrixBase<Derived> &F,
        const Eigen::Matrix<typename Kernel::FloatType, 2, 1, Eigen::DontAlign> &pt) {
    typedef typename Kernel::FloatType T;
    const int w = F.cols();
    const int h = F.rows();
    const int ix = (int) floor(pt[0]);
    const int iy = (int) floor(pt[1]);
    const T s = pt[0] - T( ix);
    const T t = pt[1] - T( iy);

    if (ix > 1 && ix + 2 < w && iy > 1 && iy + 2 < h) {
        // Inside the fully interpolated region.
        return Kernel::interpolate(
                Kernel::interpolate(F(ix - 1, iy - 1), F(ix, iy - 1), F(ix + 1, iy - 1), F(ix + 2, iy - 1), s),
                Kernel::interpolate(F(ix - 1, iy), F(ix, iy), F(ix + 1, iy), F(ix + 2, iy), s),
                Kernel::interpolate(F(ix - 1, iy + 1), F(ix, iy + 1), F(ix + 1, iy + 1), F(ix + 2, iy + 1), s),
                Kernel::interpolate(F(ix - 1, iy + 2), F(ix, iy + 2), F(ix + 1, iy + 2), F(ix + 2, iy + 2), s), t);
    }
    // Transition region. Extend with a constant function.
    auto f = [&F, w, h](int x, int y) {
        return F(BicubicInternal::clamp(x, 0, w - 1), BicubicInternal::clamp(y, 0, h - 1));
    };
    return Kernel::interpolate(
            Kernel::interpolate(f(ix - 1, iy - 1), f(ix, iy - 1), f(ix + 1, iy - 1), f(ix + 2, iy - 1), s),
            Kernel::interpolate(f(ix - 1, iy), f(ix, iy), f(ix + 1, iy), f(ix + 2, iy), s),
            Kernel::interpolate(f(ix - 1, iy + 1), f(ix, iy + 1), f(ix + 1, iy + 1), f(ix + 2, iy + 1), s),
            Kernel::interpolate(f(ix - 1, iy + 2), f(ix, iy + 2), f(ix + 1, iy + 2), f(ix + 2, iy + 2), s), t);
}

} //namespace Geometry

} // namespace Slic3r

#endif /* BICUBIC_HPP */
