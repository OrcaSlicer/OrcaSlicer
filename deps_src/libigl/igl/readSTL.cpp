// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// Copyright (C) 2018 Qingnan Zhou <qnzhou@gmail.com>
// Copyright (C) 2020 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "readSTL.h"
#include "IGL_ASSERT.h"
#include "list_to_matrix.h"
#include "string_utils.h"
#include "read_file_binary.h"
#include "FileMemoryStream.h"

#include <iostream>
#include <cstdint>

namespace igl {

template <typename DerivedV, typename DerivedF, typename DerivedN>
IGL_INLINE bool readSTL(std::istream &input,
                        Eigen::PlainObjectBase<DerivedV> &V,
                        Eigen::PlainObjectBase<DerivedF> &F,
                        Eigen::PlainObjectBase<DerivedN> &N) {
  std::vector<std::array<typename DerivedV::Scalar, 3>> vV;
  std::vector<std::array<typename DerivedN::Scalar, 3>> vN;
  std::vector<std::array<typename DerivedF::Scalar, 3>> vF;
  if (!readSTL(input, vV, vF, vN)) {
    return false;
  }

  if (!list_to_matrix(vV, V)) {
    return false;
  }

  if (!list_to_matrix(vF, F)) {
    return false;
  }

  if (!list_to_matrix(vN, N)) {
    return false;
  }
  return true;
}

IGL_INLINE bool is_stl_binary(std::istream &input) {
  std::streampos start_pos = input.tellg();

  constexpr size_t HEADER_SIZE = 80;
  char header[HEADER_SIZE];
  input.read(header, HEADER_SIZE);
  if (!starts_with(header, "solid")) {
    input.seekg(start_pos);
    return true;
  }
  if (!input.good()) {
    input.seekg(start_pos);
    return false;
  }

  // Check if filesize matches the number of faces claimed.
  char buf[4];
  input.read(buf, 4);
  size_t num_faces = *reinterpret_cast<std::uint32_t *>(buf);
  input.seekg(0, input.end);
  size_t file_size = input.tellg();

  input.seekg(start_pos);

  if (file_size == 80 + 4 + (4 * 12 + 2) * num_faces) {
    return true;
  } else {
    return false;
  }
}

template <typename TypeV, typename TypeF, typename TypeN>
IGL_INLINE bool read_stl_ascii(std::istream &input,
                               std::vector<std::array<TypeV, 3>> &V,
                               std::vector<std::array<TypeF, 3>> &F,
                               std::vector<std::array<TypeN, 3>> &N) {
  constexpr size_t LINE_SIZE = 256;
  char line[LINE_SIZE];
  bool success = true;

  if (!input) {
    throw std::runtime_error("Failed to open file");
  }

  // skip header line.
  input.getline(line, LINE_SIZE);

  auto parse_ascii_normal = [&N](const char *line) {
    double x, y, z;
    size_t n = sscanf(line, " facet normal %lf %lf %lf", &x, &y, &z);
    IGL_ASSERT(n == 3);
    if (n != 3) {
      return false;
    }

    N.push_back({{static_cast<TypeN>(x), static_cast<TypeN>(y),
                  static_cast<TypeN>(z)}});
    return true;
  };

  auto parse_ascii_vertex = [&V](const char *line) {
    double x, y, z;
    size_t n = sscanf(line, " vertex %lf %lf %lf", &x, &y, &z);
    IGL_ASSERT(n == 3);
    if (n != 3) {
      return false;
    }

    V.push_back({{static_cast<TypeV>(x), static_cast<TypeV>(y),
                  static_cast<TypeV>(z)}});
    return true;
  };

  auto parse_ascii_facet = [&parse_ascii_vertex, &parse_ascii_normal](std::istream &fin) {
    constexpr size_t LINE_SIZE = 256;
    constexpr size_t WORD_SIZE = 128;
    char line[LINE_SIZE];
    char first_word[WORD_SIZE];
    const char *face_begin = "facet";
    const char *face_end = "endfacet";
    const char *loop_begin = "outer";
    const char *loop_end = "endloop";
    const char *vertex_flag = "vertex";

    bool reading_facet = false;
    bool reading_loop = false;
    bool success = true;
    size_t num_vts = 0;
    while (!fin.eof()) {
      fin.getline(line, LINE_SIZE);
      size_t n = sscanf(line, " %s", first_word);
      if (n == 0)
        continue;
      if (starts_with(first_word, face_begin)) {
        success = parse_ascii_normal(line);
        IGL_ASSERT(success);
        reading_facet = true;
      } else if (starts_with(first_word, face_end)) {
        IGL_ASSERT(reading_facet);
        reading_facet = false;
      } else if (starts_with(first_word, loop_begin)) {
        reading_loop = true;
      } else if (starts_with(first_word, loop_end)) {
        IGL_ASSERT(reading_loop);
        reading_loop = false;
      } else if (starts_with(first_word, vertex_flag)) {
        IGL_ASSERT(reading_facet);
        IGL_ASSERT(reading_loop);
        success = parse_ascii_vertex(line);
        IGL_ASSERT(success);
        num_vts += 1;
      }
      if (!success) {
        return false;
      }
      if (!reading_facet) {
        break;
      }
    }
    if (num_vts == 0) {
      return true;
    }
    IGL_ASSERT(num_vts == 3);
    if (num_vts != 3) {
      std::cerr << "Warning: mesh contain face made of " << num_vts
                << " vertices" << std::endl;
      return false;
    }
    return true;
  };

  while (!input.eof()) {
    success = parse_ascii_facet(input);
    if (!success) {
      return false;
    }
  }

  F.resize(V.size() / 3);
    for (size_t f = 0; f < F.size(); ++f) {
    auto v = static_cast<TypeF>(f * 3);
    F[f] = {{v, v + 1, v + 2}};
  }
  return success;
}

template <typename TypeV, typename TypeF, typename TypeN>
IGL_INLINE bool read_stl_binary(std::istream &input,
                                std::vector<std::array<TypeV, 3>> &V,
                                std::vector<std::array<TypeF, 3>> &F,
                                std::vector<std::array<TypeN, 3>> &N) {
  if (!input) {
    throw std::runtime_error("Failed to open file");
  }

  constexpr size_t FLOAT_SIZE = sizeof(float);
  static_assert(FLOAT_SIZE == 4, "float type is not 4 bytes");
  constexpr size_t LINE_SIZE = 256;
  char buf[LINE_SIZE];

  // 80 bytes header, no data significance.
  input.read(buf, 80);
  if (!input.good()) {
    throw std::runtime_error("Unable to parse STL header.");
  }

  input.read(buf, 4);
  const size_t num_faces = *reinterpret_cast<std::uint32_t *>(buf);
  if (!input.good()) {
    throw std::runtime_error("Unable to parse STL number of faces.");
  }

  for (size_t i = 0; i < num_faces; i++) {
    // Parse normal
    input.read(buf, FLOAT_SIZE * 3);
    auto nx = static_cast<TypeN>(*reinterpret_cast<float *>(buf));
    auto ny = static_cast<TypeN>(*reinterpret_cast<float *>(buf + FLOAT_SIZE));
    auto nz =
        static_cast<TypeN>(*reinterpret_cast<float *>(buf + FLOAT_SIZE * 2));
    IGL_ASSERT(input.good());

    // vertex 1
    input.read(buf, FLOAT_SIZE * 3);
    auto v1x = static_cast<TypeV>(*reinterpret_cast<float *>(buf));
    auto v1y = static_cast<TypeV>(*reinterpret_cast<float *>(buf + FLOAT_SIZE));
    auto v1z =
        static_cast<TypeV>(*reinterpret_cast<float *>(buf + FLOAT_SIZE * 2));
    IGL_ASSERT(input.good());

    // vertex 2
    input.read(buf, FLOAT_SIZE * 3);
    auto v2x = static_cast<TypeV>(*reinterpret_cast<float *>(buf));
    auto v2y = static_cast<TypeV>(*reinterpret_cast<float *>(buf + FLOAT_SIZE));
    auto v2z =
        static_cast<TypeV>(*reinterpret_cast<float *>(buf + FLOAT_SIZE * 2));
    IGL_ASSERT(input.good());

    // vertex 3
    input.read(buf, FLOAT_SIZE * 3);
    auto v3x = static_cast<TypeV>(*reinterpret_cast<float *>(buf));
    auto v3y = static_cast<TypeV>(*reinterpret_cast<float *>(buf + FLOAT_SIZE));
    auto v3z =
        static_cast<TypeV>(*reinterpret_cast<float *>(buf + FLOAT_SIZE * 2));
    IGL_ASSERT(input.good());

    // attribute (2 bytes), not sure what purpose they serve.
    input.read(buf, 2);

    N.push_back({{nx, ny, nz}});
    V.push_back({{v1x, v1y, v1z}});
    V.push_back({{v2x, v2y, v2z}});
    V.push_back({{v3x, v3y, v3z}});

    IGL_ASSERT(input.good());
    if (!input.good()) {
      std::stringstream err_msg;
      err_msg << "Failed to parse face " << i << " from STL file";
      throw std::runtime_error(err_msg.str());
    }
  }
  std::for_each(V.begin(), V.end(), [](const std::array<TypeV, 3> &v) {
    for (auto x : v) {
      if (!std::isfinite(x)) {
        throw std::runtime_error("NaN or Inf detected in input file.");
      }
    }
  });

  if (!V.empty()) {
    F.resize(V.size() / 3);
    for (size_t f = 0; f < F.size(); ++f) {
      auto v = static_cast<TypeF>(f * 3);
      F[f] = {{v, v + 1, v + 2}};
    }
  }

  return true;
}

template <typename TypeV, typename TypeF, typename TypeN>
IGL_INLINE bool readSTL(std::istream &input,
                        std::vector<std::array<TypeV, 3>> &V,
                        std::vector<std::array<TypeF, 3>> &F,
                        std::vector<std::array<TypeN, 3>> &N) {
  bool success = false;
  if (is_stl_binary(input)) {
    success = read_stl_binary(input, V, F, N);
  } else {
    success = read_stl_ascii(input, V, F, N);
  }
  return success;
}

template <typename DerivedV, typename DerivedF, typename DerivedN>
IGL_INLINE bool readSTL(
  FILE * fp,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedN> & N)
{
  std::vector<std::uint8_t> fileBufferBytes;
  read_file_binary(fp,fileBufferBytes);
  FileMemoryStream stream((char*)fileBufferBytes.data(), fileBufferBytes.size());
  return readSTL(stream, V, F, N);
}

} // namespace igl

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template bool igl::readSTL<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template bool igl::readSTL<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template bool igl::readSTL<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template bool igl::readSTL<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template bool igl::readSTL<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template bool igl::readSTL<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template bool igl::readSTL<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template bool igl::readSTL<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template bool igl::readSTL<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template bool igl::readSTL<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
