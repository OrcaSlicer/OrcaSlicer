#ifndef IGL_READPLY_H
#define IGL_READPLY_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <string>
#include <vector>
#include "tinyply.h"

namespace igl
{
  /// Read triangular mesh from ply file, filling in vertex positions, normals
  /// and texture coordinates, if available
  /// also read additional properties associated with vertex,faces and edges 
  /// and file comments
  ///
  /// @tparam Derived* from Eigen matrix parameters
  /// @param[in] ply_stream  ply file input stream
  /// @param[out] V  (#V,3) matrix of vertex positions 
  /// @param[out] F  (#F,3) list of face indices into vertex positions
  /// @param[out] E  (#E,2) list of edge indices into vertex positions
  /// @param[out] N  (#V,3) list of normals
  /// @param[out] UV (#V,2) list of texture coordinates
  /// @param[out] VD (#V,*) additional vertex data
  /// @param[out] Vheader (#V) list of vertex data headers
  /// @param[out] FD (#F,*) additional face data
  /// @param[out] Fheader (#F) list of face data headers
  /// @param[out] ED (#E,*) additional edge data
  /// @param[out] Eheader (#E) list of edge data headers
  /// @param[out] comments (*) file comments
  /// @return true on success, false on errors
  ///
  /// \note Unlike previous versions, all matrices are left untouched if they
  /// are not read from the file. 
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedE,
    typename DerivedN,
    typename DerivedUV,
    typename DerivedVD,
    typename DerivedFD,
    typename DerivedED
    >
  bool readPLY(
    std::istream & ply_stream,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedF> & E,
    Eigen::PlainObjectBase<DerivedN> & N,
    Eigen::PlainObjectBase<DerivedUV> & UV,
    Eigen::PlainObjectBase<DerivedVD> & VD,
    std::vector<std::string> & Vheader,
    Eigen::PlainObjectBase<DerivedFD> & FD,
    std::vector<std::string> & Fheader,
    Eigen::PlainObjectBase<DerivedED> & ED,
    std::vector<std::string> & Eheader,
    std::vector<std::string> & comments
    );
  /// \overload
  /// @param[in] ply_file  ply file name
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedE,
    typename DerivedN,
    typename DerivedUV,
    typename DerivedVD,
    typename DerivedFD,
    typename DerivedED
    >
  bool readPLY(
    const std::string& ply_file,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedE> & E,
    Eigen::PlainObjectBase<DerivedN> & N,
    Eigen::PlainObjectBase<DerivedUV> & UV,
    Eigen::PlainObjectBase<DerivedVD> & VD,
    std::vector<std::string> & VDheader,
    Eigen::PlainObjectBase<DerivedFD> & FD,
    std::vector<std::string> & FDheader,
    Eigen::PlainObjectBase<DerivedED> & ED,
    std::vector<std::string> & EDheader,
    std::vector<std::string> & comments
    );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedN,
    typename DerivedUV,
    typename DerivedVD
    >
  bool readPLY(
    const std::string & filename,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedN> & N,
    Eigen::PlainObjectBase<DerivedUV> & UV,
    Eigen::PlainObjectBase<DerivedVD> & VD,
    std::vector<std::string> & Vheader
    );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedE,
    typename DerivedN,
    typename DerivedUV
    >
  bool readPLY(
    const std::string & filename,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedE> & E,
    Eigen::PlainObjectBase<DerivedN> & N,
    Eigen::PlainObjectBase<DerivedUV> & UV
    );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF
    >
  bool readPLY(
    const std::string & filename,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F
    );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedE
    >
  bool readPLY(
    const std::string & filename,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedE> & E
    );
  /// \overload
  /// @param[in,out] fp  pointer to ply file (will be closed)
  template <
    typename DerivedV,
    typename DerivedF
    >
  IGL_INLINE bool readPLY(
    FILE *fp,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedF> & F
    );
}

#ifndef IGL_STATIC_LIBRARY
#  include "readPLY.cpp"
#endif
#endif
