// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

namespace Eigen {

namespace internal {

template <typename _Scalar>
struct duccfft_impl {
  using Scalar = _Scalar;
  using Complex = std::complex<Scalar>;
  using shape_t = ducc0::fmav_info::shape_t;
  using stride_t = ducc0::fmav_info::stride_t;

  inline void clear() {}

  inline void fwd(Complex* dst, const Scalar* src, int nfft) {
    const shape_t axes{0};
    ducc0::cfmav<Scalar> m_in(src, shape_t{static_cast<size_t>(nfft)});
    ducc0::vfmav<Complex> m_out(dst, shape_t{static_cast<size_t>(nfft) / 2 + 1});
    ducc0::r2c(m_in, m_out, axes, /*forward=*/true, /*scale=*/static_cast<Scalar>(1));
  }

  inline void fwd(Complex* dst, const Complex* src, int nfft) {
    const shape_t axes{0};
    ducc0::cfmav<Complex> m_in(src, shape_t{static_cast<size_t>(nfft)});
    ducc0::vfmav<Complex> m_out(dst, shape_t{static_cast<size_t>(nfft)});
    ducc0::c2c(m_in, m_out, axes, /*forward=*/true, /*scale=*/static_cast<Scalar>(1));
  }

  inline void inv(Scalar* dst, const Complex* src, int nfft) {
    const shape_t axes{0};
    ducc0::cfmav<Complex> m_in(src, shape_t{static_cast<size_t>(nfft) / 2 + 1});
    ducc0::vfmav<Scalar> m_out(dst, shape_t{static_cast<size_t>(nfft)});
    ducc0::c2r(m_in, m_out, axes, /*forward=*/false, /*scale=*/static_cast<Scalar>(1));
  }

  inline void inv(Complex* dst, const Complex* src, int nfft) {
    const shape_t axes{0};
    ducc0::cfmav<Complex> m_in(src, shape_t{static_cast<size_t>(nfft)});
    ducc0::vfmav<Complex> m_out(dst, shape_t{static_cast<size_t>(nfft)});
    ducc0::c2c(m_in, m_out, axes, /*forward=*/false, /*scale=*/static_cast<Scalar>(1));
  }

  inline void fwd2(Complex* dst, const Complex* src, int nfft0, int nfft1) {
    const shape_t axes{0, 1};
    const shape_t in_shape{static_cast<size_t>(nfft0), static_cast<size_t>(nfft1)};
    const shape_t out_shape{static_cast<size_t>(nfft0), static_cast<size_t>(nfft1)};
    const stride_t stride{static_cast<ptrdiff_t>(nfft1), static_cast<ptrdiff_t>(1)};
    ducc0::cfmav<Complex> m_in(src, in_shape, stride);
    ducc0::vfmav<Complex> m_out(dst, out_shape, stride);
    ducc0::c2c(m_in, m_out, axes, /*forward=*/true, /*scale=*/static_cast<Scalar>(1));
  }

  inline void inv2(Complex* dst, const Complex* src, int nfft0, int nfft1) {
    const shape_t axes{0, 1};
    const shape_t in_shape{static_cast<size_t>(nfft0), static_cast<size_t>(nfft1)};
    const shape_t out_shape{static_cast<size_t>(nfft0), static_cast<size_t>(nfft1)};
    const stride_t stride{static_cast<ptrdiff_t>(nfft1), static_cast<ptrdiff_t>(1)};
    ducc0::cfmav<Complex> m_in(src, in_shape, stride);
    ducc0::vfmav<Complex> m_out(dst, out_shape, stride);
    ducc0::c2c(m_in, m_out, axes, /*forward=*/false, /*scale=*/static_cast<Scalar>(1));
  }
};

}  // namespace internal
}  // namespace Eigen
