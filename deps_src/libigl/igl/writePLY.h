#ifndef IGL_WRITEPLY_H
#define IGL_WRITEPLY_H
#include "igl_inline.h"
#include "FileEncoding.h"

#include <string>
#include <iostream>
#include <vector>
#include <Eigen/Core>


namespace igl
{
  /// write triangular mesh to ply file
  ///
  /// @tparam Derived from Eigen matrix parameters
  /// @param[in] ply_stream  ply file output stream
  /// @param[in] V  (#V,3) matrix of vertex positions
  /// @param[in] F  (#F,3) list of face indices into vertex positions
  /// @param[in] E  (#E,2) list of edge indices into vertex positions
  /// @param[in] N  (#V,3) list of normals
  /// @param[in] UV (#V,2) list of texture coordinates
  /// @param[in] VD (#V,*) additional vertex data
  /// @param[in] Vheader (#V) list of vertex data headers
  /// @param[in] FD (#F,*) additional face data
  /// @param[in] Fheader (#F) list of face data headers
  /// @param[in] ED (#E,*) additional edge data
  /// @param[in] Eheader (#E) list of edge data headers
  /// @param[in] comments (*) file comments
  /// @param[in] encoding - enum, to set binary or ascii file format
  /// @return true on success, false on errors
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
  bool writePLY(
    std::ostream & ply_stream,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedE> & E,
    const Eigen::MatrixBase<DerivedN> & N,
    const Eigen::MatrixBase<DerivedUV> & UV,
    const Eigen::MatrixBase<DerivedVD> & VD,
    const std::vector<std::string> & VDheader,
    const Eigen::MatrixBase<DerivedFD> & FD,
    const std::vector<std::string> & FDheader,
    const Eigen::MatrixBase<DerivedED> & ED,
    const std::vector<std::string> & EDheader,
    const std::vector<std::string> & comments,
    FileEncoding encoding
     );
  /// \overload
  /// \param[in] filename  path to .ply file
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
  bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedE> & E,
    const Eigen::MatrixBase<DerivedN> & N,
    const Eigen::MatrixBase<DerivedUV> & UV,
    const Eigen::MatrixBase<DerivedVD> & VD,
    const std::vector<std::string> & VDheader,
    const Eigen::MatrixBase<DerivedFD> & FD,
    const std::vector<std::string> & FDheader,
    const Eigen::MatrixBase<DerivedED> & ED,
    const std::vector<std::string> & EDheader,
    const std::vector<std::string> & comments,
    FileEncoding encoding);
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF
  >
  bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F
    );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedE
  >
  bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedE> & E
     );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedN,
    typename DerivedUV
  >
  bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedN> & N,
    const Eigen::MatrixBase<DerivedUV> & UV
     );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedE,
    typename DerivedN,
    typename DerivedUV
  >
  bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedE> & E,
    const Eigen::MatrixBase<DerivedN> & N,
    const Eigen::MatrixBase<DerivedUV> & UV
     );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF
  >
  bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    FileEncoding encoding
     );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedE
  >
  bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedE> & E,
    FileEncoding encoding
     );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedN,
    typename DerivedUV,
    typename DerivedVD
  >
  bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedN> & N,
    const Eigen::MatrixBase<DerivedUV> & UV,
    const Eigen::MatrixBase<DerivedVD> & VD=Eigen::MatrixXd(0,0),
    const std::vector<std::string> & VDheader={},
    const std::vector<std::string> & comments={}
     );
  /// \overload
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedE,
    typename DerivedN,
    typename DerivedUV,
    typename DerivedVD
  >
  bool writePLY(
    const std::string & filename,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedE> & E,
    const Eigen::MatrixBase<DerivedN> & N,
    const Eigen::MatrixBase<DerivedUV> & UV,
    const Eigen::MatrixBase<DerivedVD> & VD=Eigen::MatrixXd(0,0),
    const std::vector<std::string> & VDheader={},
    const std::vector<std::string> & comments={}
     );
}



#ifndef IGL_STATIC_LIBRARY
#  include "writePLY.cpp"
#endif
#endif
