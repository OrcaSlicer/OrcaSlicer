// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#if __has_include(<Eigen/Core>)
#include <Eigen/Core>
#else
static_assert(false, "Eigen must be included to use glaze/ext/eigen.hpp");
#endif

#include <span>

#include "glaze/api/std/array.hpp"
#include "glaze/beve/read.hpp"
#include "glaze/beve/write.hpp"
#include "glaze/cbor/read.hpp"
#include "glaze/cbor/write.hpp"
#include "glaze/core/common.hpp"
#include "glaze/core/meta.hpp"
#include "glaze/json/json_ptr.hpp"
#include "glaze/json/read.hpp"
#include "glaze/json/write.hpp"

namespace glz
{
   template <matrix_t T>
      requires(T::RowsAtCompileTime >= 0 && T::ColsAtCompileTime >= 0)
   struct from<BEVE, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         ++it;
         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }
         constexpr uint8_t layout = uint8_t(!T::IsRowMajor);
         if (uint8_t(*it) != layout) {
            ctx.error = error_code::syntax_error;
         }
         ++it;
         std::array<Eigen::Index, 2> extents;
         parse<BEVE>::op<Opts>(extents, ctx, it, end);

         std::span<typename T::Scalar, T::RowsAtCompileTime * T::ColsAtCompileTime> view(value.data(), value.size());
         parse<BEVE>::op<Opts>(view, ctx, it, end);
      }
   };

   // A dynamic matrix in both rows and columns
   template <matrix_t T>
      requires(T::RowsAtCompileTime < 0 && T::ColsAtCompileTime < 0)
   struct from<BEVE, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         ++it;
         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }
         constexpr uint8_t layout = uint8_t(!T::IsRowMajor);
         if (uint8_t(*it) != layout) {
            ctx.error = error_code::syntax_error;
            return;
         }
         ++it;
         std::array<Eigen::Index, 2> extents;
         parse<BEVE>::op<Opts>(extents, ctx, it, end);

         value.resize(extents[0], extents[1]);
         std::span<typename T::Scalar> view(value.data(), extents[0] * extents[1]);
         parse<BEVE>::op<Opts>(view, ctx, it, end);
      }
   };

   template <matrix_t T>
      requires(T::RowsAtCompileTime >= 0 && T::ColsAtCompileTime >= 0)
   struct to<BEVE, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&&... args)
      {
         constexpr uint8_t matrix = 0b00010'000;
         constexpr uint8_t tag = tag::extensions | matrix;
         dump_type(tag, args...);

         constexpr uint8_t layout = uint8_t(!T::IsRowMajor);
         dump_type(layout, args...);

         std::array<Eigen::Index, 2> extents{T::RowsAtCompileTime, T::ColsAtCompileTime};
         serialize<BEVE>::op<Opts>(extents, ctx, args...);

         std::span<const typename T::Scalar, T::RowsAtCompileTime * T::ColsAtCompileTime> view(value.data(),
                                                                                               value.size());
         serialize<BEVE>::op<Opts>(view, ctx, args...);
      }
   };

   // A dynamic matrix in both rows and columns
   template <matrix_t T>
      requires(T::RowsAtCompileTime < 0 && T::ColsAtCompileTime < 0)
   struct to<BEVE, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&&... args)
      {
         constexpr uint8_t matrix = 0b00010'000;
         constexpr uint8_t tag = tag::extensions | matrix;
         dump_type(tag, args...);

         constexpr uint8_t layout = uint8_t(!T::IsRowMajor);
         dump_type(layout, args...);

         std::array<Eigen::Index, 2> extents{value.rows(), value.cols()};
         serialize<BEVE>::op<Opts>(extents, ctx, args...);

         std::span<const typename T::Scalar> view(value.data(), extents[0] * extents[1]);
         serialize<BEVE>::op<Opts>(view, ctx, args...);
      }
   };

   // CBOR Eigen support (matrices and vectors)
   // Uses RFC 8746 multi-dimensional array tags:
   //   Tag 40: row-major order
   //   Tag 1040: column-major order (Eigen default)
   // Format: tag([[rows, cols], typed_array])
   template <eigen_t T>
   struct to<CBOR, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         using Scalar = typename std::remove_cvref_t<T>::Scalar;
         using EigenType = std::remove_cvref_t<T>;

         // Write tag: 40 for row-major, 1040 for column-major
         constexpr uint64_t tag =
            EigenType::IsRowMajor ? cbor::semantic_tag::multi_dim_array : cbor::semantic_tag::multi_dim_array_col_major;
         cbor_detail::encode_arg(cbor::major::tag, tag, b, ix);

         // Write 2-element array: [dimensions, data]
         cbor_detail::encode_arg(cbor::major::array, 2, b, ix);

         // Write dimensions as array [rows, cols]
         cbor_detail::encode_arg(cbor::major::array, 2, b, ix);
         cbor_detail::encode_arg(cbor::major::uint, static_cast<uint64_t>(value.rows()), b, ix);
         cbor_detail::encode_arg(cbor::major::uint, static_cast<uint64_t>(value.cols()), b, ix);

         // Write data as typed array
         std::span<const Scalar> view(value.data(), value.size());
         serialize<CBOR>::op<Opts>(view, ctx, b, ix);
      }
   };

   // CBOR Eigen read (matrices and vectors)
   // Parses RFC 8746 multi-dimensional array format:
   //   Tag 40 or 1040 followed by [[dimensions], typed_array]
   template <eigen_t T>
   struct from<CBOR, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         using Scalar = typename std::remove_cvref_t<T>::Scalar;
         using EigenType = std::remove_cvref_t<T>;

         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         // Read and verify tag (40 = row-major, 1040 = column-major)
         uint8_t initial;
         std::memcpy(&initial, it, 1);
         if (cbor::get_major_type(initial) != cbor::major::tag) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }
         ++it;
         const uint64_t tag = cbor_detail::decode_arg(ctx, it, end, cbor::get_additional_info(initial));
         if (bool(ctx.error)) [[unlikely]]
            return;

         // Verify tag matches expected layout
         constexpr uint64_t expected_tag =
            EigenType::IsRowMajor ? cbor::semantic_tag::multi_dim_array : cbor::semantic_tag::multi_dim_array_col_major;
         if (tag != expected_tag) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         // Read outer array header (expect 2 elements: [dimensions, data])
         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }
         std::memcpy(&initial, it, 1);
         if (cbor::get_major_type(initial) != cbor::major::array) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }
         ++it;
         const auto outer_size = cbor_detail::decode_arg(ctx, it, end, cbor::get_additional_info(initial));
         if (bool(ctx.error)) [[unlikely]]
            return;
         if (outer_size != 2) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         // Read dimensions array [rows, cols]
         if (it >= end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }
         std::memcpy(&initial, it, 1);
         if (cbor::get_major_type(initial) != cbor::major::array) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }
         ++it;
         const auto dims_size = cbor_detail::decode_arg(ctx, it, end, cbor::get_additional_info(initial));
         if (bool(ctx.error)) [[unlikely]]
            return;
         if (dims_size != 2) [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return;
         }

         // Read rows
         std::memcpy(&initial, it, 1);
         ++it;
         const auto rows = cbor_detail::decode_arg(ctx, it, end, cbor::get_additional_info(initial));
         if (bool(ctx.error)) [[unlikely]]
            return;

         // Read cols
         std::memcpy(&initial, it, 1);
         ++it;
         const auto cols = cbor_detail::decode_arg(ctx, it, end, cbor::get_additional_info(initial));
         if (bool(ctx.error)) [[unlikely]]
            return;

         // Resize if dynamic
         if constexpr (EigenType::RowsAtCompileTime < 0 || EigenType::ColsAtCompileTime < 0) {
            value.resize(static_cast<Eigen::Index>(rows), static_cast<Eigen::Index>(cols));
         }

         // Read data as typed array
         std::span<Scalar> view(value.data(), rows * cols);
         parse<CBOR>::op<Opts>(view, ctx, it, end);
      }
   };

   template <matrix_t T>
      requires(T::RowsAtCompileTime >= 0 && T::ColsAtCompileTime >= 0)
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         std::span<typename T::Scalar, T::RowsAtCompileTime * T::ColsAtCompileTime> view(value.data(), value.size());
         parse<JSON>::op<Opts>(view, ctx, it, end);
      }
   };

   template <matrix_t T>
      requires(T::RowsAtCompileTime >= 0 && T::ColsAtCompileTime >= 0)
   struct to<JSON, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         std::span<const typename T::Scalar, T::RowsAtCompileTime * T::ColsAtCompileTime> view(value.data(),
                                                                                               value.size());
         using Value = std::remove_cvref_t<decltype(value)>;
         to<JSON, Value>::template op<Opts>(view, ctx, b, ix);
      }
   };

   // A dynamic matrix in both rows and columns
   template <matrix_t T>
      requires(T::RowsAtCompileTime < 0 && T::ColsAtCompileTime < 0)
   struct to<JSON, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         dump('[', b, ix);
         using RowColT = std::array<Eigen::Index, 2>;
         RowColT extents{value.rows(), value.cols()};
         to<JSON, RowColT>::template op<Opts>(extents, ctx, b, ix);
         dump(',', b, ix);

         std::span<const typename T::Scalar> view(value.data(), value.size());
         using Value = std::remove_cvref_t<decltype(view)>;
         to<JSON, Value>::template op<Opts>(view, ctx, b, ix);
         dump(']', b, ix);
      }
   };

   // A dynamic matrix in both rows and columns
   template <matrix_t T>
      requires(T::RowsAtCompileTime < 0 && T::ColsAtCompileTime < 0)
   struct from<JSON, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if (match_invalid_end<'[', Opts>(ctx, it, end)) {
            return;
         }
         std::array<Eigen::Index, 2> extents; // NOLINT
         parse<JSON>::op<Opts>(extents, ctx, it, end);
         value.resize(extents[0], extents[1]);
         if (*it == ',') {
            // we have data
            ++it;
            if constexpr (not Opts.null_terminated) {
               if (it == end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return;
               }
            }
            std::span<typename T::Scalar> view(value.data(), extents[0] * extents[1]);
            parse<JSON>::op<Opts>(view, ctx, it, end);
         }
         match<']'>(ctx, it);
      }
   };

   template <typename Scalar, int Dim, int Mode>
   struct from<JSON, Eigen::Transform<Scalar, Dim, Mode>>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto size = Mode == Eigen::TransformTraits::AffineCompact ? (Dim + 1) * Dim : (Dim + 1) * (Dim + 1);
         std::span<Scalar, size> view(value.data(), size);
         parse<JSON>::op<Opts>(view, ctx, it, end);
      }
   };

   template <typename Scalar, int Dim, int Mode>
   struct to<JSON, Eigen::Transform<Scalar, Dim, Mode>>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         constexpr auto size = Mode == Eigen::TransformTraits::AffineCompact ? (Dim + 1) * Dim : (Dim + 1) * (Dim + 1);
         std::span<const Scalar, size> view(value.data(), size);
         using Value = std::remove_cvref_t<decltype(value)>;
         to<JSON, Value>::template op<Opts>(view, ctx, b, ix);
      }
   };
}

template <class Scalar, int Rows, int Cols>
struct glz::meta<Eigen::Matrix<Scalar, Rows, Cols>>
{
   static constexpr std::string_view name = join_v<chars<"Eigen::Matrix<">, name_v<Scalar>, chars<",">, //
                                                   chars<num_to_string<Rows>::value>, chars<",">, //
                                                   chars<num_to_string<Cols>::value>, chars<",">, //
                                                   chars<">">>;
};
