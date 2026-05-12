#include "writePLY.h"
#include <fstream>

#include "tinyply.h"
#include <stdint.h>


#include <cstdint>
namespace igl
{
  template <typename Scalar> tinyply::Type tynyply_type();

  template <> tinyply::Type IGL_INLINE tynyply_type<std::int8_t   >() { return tinyply::Type::INT8; }
  template <> tinyply::Type IGL_INLINE tynyply_type<std::int16_t  >() { return tinyply::Type::INT16; }
  template <> tinyply::Type IGL_INLINE tynyply_type<std::int32_t  >() { return tinyply::Type::INT32; }
  template <> tinyply::Type IGL_INLINE tynyply_type<std::uint8_t  >() { return tinyply::Type::UINT8; }
  template <> tinyply::Type IGL_INLINE tynyply_type<std::uint16_t >() { return tinyply::Type::UINT16; }
  template <> tinyply::Type IGL_INLINE tynyply_type<std::uint32_t >() { return tinyply::Type::UINT32; }
  template <> tinyply::Type IGL_INLINE tynyply_type<float         >() { return tinyply::Type::FLOAT32; }
  template <> tinyply::Type IGL_INLINE tynyply_type<double        >() { return tinyply::Type::FLOAT64; }
}


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
bool igl::writePLY(
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
   )
{
    typedef typename DerivedV::Scalar VScalar;
    typedef typename DerivedN::Scalar NScalar;
    typedef typename DerivedUV::Scalar UVScalar;
    typedef typename DerivedF::Scalar FScalar;
    typedef typename DerivedE::Scalar EScalar;

    typedef typename DerivedVD::Scalar VDScalar;
    typedef typename DerivedFD::Scalar FDScalar;
    typedef typename DerivedED::Scalar EDScalar;

    // temporary storage for data to be passed to tinyply internals
    std::vector<VScalar> _v;
    std::vector<NScalar> _n;
    std::vector<UVScalar> _uv;
    std::vector<VDScalar> _vd;
    std::vector<FDScalar> _fd;
    std::vector<EScalar> _ev;
    std::vector<EDScalar> _ed;

    // check dimensions
    if( V.cols()!=3)
    {
      std::cerr << "writePLY: unexpected dimensions " << std::endl;
      return false;
    }
    tinyply::PlyFile file;

    _v.resize(V.size());
    Eigen::Map< Eigen::Matrix<VScalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor > >( &_v[0], V.rows(), V.cols() ) = V;

    file.add_properties_to_element("vertex", { "x", "y", "z" },
        tynyply_type<VScalar>(), V.rows(), reinterpret_cast<uint8_t*>( &_v[0] ), tinyply::Type::INVALID, 0);

    if(N.rows()>0)
    {
        _n.resize(N.size());
        Eigen::Map<Eigen::Matrix<NScalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor > >( &_n[0], N.rows(), N.cols() ) = N;
        file.add_properties_to_element("vertex", { "nx", "ny", "nz" },
            tynyply_type<NScalar>(), N.rows(), reinterpret_cast<uint8_t*>( &_n[0] ),tinyply::Type::INVALID, 0);
    }

    if(UV.rows()>0)
    {
        _uv.resize(UV.size());
        Eigen::Map<Eigen::Matrix<UVScalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor > >( &_uv[0], UV.rows(), UV.cols() ) = UV;

        file.add_properties_to_element("vertex", { "u", "v" },
            tynyply_type<UVScalar>(), UV.rows() , reinterpret_cast<uint8_t*>( &_uv[0] ), tinyply::Type::INVALID, 0);
    }

    if(VD.cols()>0)
    {
        assert(VD.cols() == VDheader.size());
        assert(VD.rows() == V.rows());

        _vd.resize(VD.size());
        Eigen::Map< Eigen::Matrix<VDScalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor > >( &_vd[0], VD.rows(), VD.cols() ) = VD;

        file.add_properties_to_element("vertex", VDheader,
            tynyply_type<VDScalar>(), VD.rows(), reinterpret_cast<uint8_t*>( &_vd[0] ), tinyply::Type::INVALID, 0);
    }



    std::vector<FScalar> _f(F.size());
    Eigen::Map<Eigen::Matrix<FScalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor > >( &_f[0], F.rows(), F.cols() ) = F;
    file.add_properties_to_element("face", { "vertex_indices" },
        tynyply_type<FScalar>(), F.rows(), reinterpret_cast<uint8_t*>(&_f[0]), tinyply::Type::UINT8, F.cols() );

    if(FD.cols()>0)
    {
        assert(FD.rows()==F.rows());
        assert(FD.cols() == FDheader.size());

        _fd.resize(FD.size());
        Eigen::Map<Eigen::Matrix<FDScalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor > >( &_fd[0], FD.rows(), FD.cols() ) = FD;

        file.add_properties_to_element("face", FDheader,
            tynyply_type<FDScalar>(), FD.rows(), reinterpret_cast<uint8_t*>( &_fd[0] ), tinyply::Type::INVALID, 0);
    }

    if(E.rows()>0)
    {
        assert(E.cols()==2);
        _ev.resize(E.size());
        Eigen::Map<Eigen::Matrix<EScalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor > >( &_ev[0], E.rows(), E.cols() ) = E;

        file.add_properties_to_element("edge", { "vertex1", "vertex2" },
            tynyply_type<EScalar>(), E.rows() , reinterpret_cast<uint8_t*>( &_ev[0] ), tinyply::Type::INVALID, 0);
    }

    if(ED.cols()>0)
    {
        assert(ED.rows()==E.rows());
        assert(ED.cols() == EDheader.size());

        _ed.resize(ED.size());
        Eigen::Map<Eigen::Matrix<EDScalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor > >( &_ed[0], ED.rows(), ED.cols() ) = ED;

        file.add_properties_to_element("edge", EDheader,
            tynyply_type<EDScalar>(), ED.rows(), reinterpret_cast<uint8_t*>( &_ed[0] ), tinyply::Type::INVALID, 0);
    }

    for(auto a:comments)
      file.get_comments().push_back(a);

    // Write a binary file
    file.write(ply_stream, (encoding == FileEncoding::Binary));

    return true;
}

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
bool igl::writePLY(
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
  FileEncoding encoding
   )
{
  try
  {
    if(encoding == FileEncoding::Binary)
    {
      std::filebuf fb_binary;
      fb_binary.open(filename , std::ios::out | std::ios::binary);
      std::ostream outstream_binary(&fb_binary);
      if (outstream_binary.fail()) {
        std::cerr << "writePLY: Error opening file " << filename << std::endl;
        return false; //throw std::runtime_error("failed to open " + filename);
      }
      return writePLY(outstream_binary,V,F,E,N,UV,VD,VDheader,FD,FDheader,ED,EDheader,comments,encoding);
    } else {
      std::filebuf fb_ascii;
      fb_ascii.open(filename, std::ios::out);
      std::ostream outstream_ascii(&fb_ascii);
      if (outstream_ascii.fail()) {
        std::cerr << "writePLY: Error opening file " << filename << std::endl;
        return false; //throw std::runtime_error("failed to open " + filename);
      }
      return writePLY(outstream_ascii,V,F,E,N,UV,VD,VDheader,FD,FDheader,ED,EDheader,comments,encoding);
    }
  }
  catch(const std::exception& e)
  {
    std::cerr << "writePLY error: " << filename << e.what() << std::endl;
  }
  return false;
}

template <
  typename DerivedV,
  typename DerivedF
>
bool igl::writePLY(
  const std::string & filename,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F
   )
{
  Eigen::MatrixXd _dummy;
  std::vector<std::string> _dummy_header;

  return writePLY(filename,V,F,_dummy, _dummy, _dummy, _dummy, _dummy_header, _dummy, _dummy_header, _dummy, _dummy_header, _dummy_header, FileEncoding::Binary);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE
>
bool igl::writePLY(
  const std::string & filename,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedE> & E
   )
{
  Eigen::MatrixXd _dummy;
  std::vector<std::string> _dummy_header;

  return writePLY(filename,V,F,E, _dummy, _dummy, _dummy, _dummy_header, _dummy, _dummy_header, _dummy, _dummy_header, _dummy_header, FileEncoding::Binary);
}


template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedN,
  typename DerivedUV
>
bool igl::writePLY(
  const std::string & filename,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedN> & N,
  const Eigen::MatrixBase<DerivedUV> & UV
   )
{
  Eigen::MatrixXd _dummy;
  std::vector<std::string> _dummy_header;

  return writePLY(filename,V,F,_dummy, N,UV, _dummy, _dummy_header, _dummy, _dummy_header, _dummy, _dummy_header, _dummy_header, FileEncoding::Binary);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE,
  typename DerivedN,
  typename DerivedUV
>
bool igl::writePLY(
  const std::string & filename,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedE> & E,
  const Eigen::MatrixBase<DerivedN> & N,
  const Eigen::MatrixBase<DerivedUV> & UV
   )
{
  Eigen::MatrixXd _dummy;
  std::vector<std::string> _dummy_header;

  return writePLY(filename,V,F,E, N,UV, _dummy, _dummy_header, _dummy, _dummy_header, _dummy, _dummy_header, _dummy_header, FileEncoding::Binary);
}

template <
  typename DerivedV,
  typename DerivedF
>
bool igl::writePLY(
  const std::string & filename,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  FileEncoding encoding
   )
{
  Eigen::MatrixXd _dummy(0,0);
  std::vector<std::string> _dummy_header;

  return writePLY(filename,V,F,_dummy, _dummy,_dummy, _dummy, _dummy_header,
                         _dummy, _dummy_header, _dummy, _dummy_header, _dummy_header, encoding);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE
>
bool igl::writePLY(
  const std::string & filename,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedE> & E,
  FileEncoding encoding
   )
{
  Eigen::MatrixXd _dummy(0,0);
  std::vector<std::string> _dummy_header;

  return writePLY(filename,V,F,E, _dummy,_dummy, _dummy, _dummy_header,
                         _dummy, _dummy_header, _dummy, _dummy_header, _dummy_header, encoding);
}




template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedN,
  typename DerivedUV,
  typename DerivedVD
>
bool igl::writePLY(
  const std::string & filename,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedN> & N,
  const Eigen::MatrixBase<DerivedUV> & UV,
  const Eigen::MatrixBase<DerivedVD> & VD,
  const std::vector<std::string> & VDheader,
  const std::vector<std::string> & comments
   )
{
  Eigen::MatrixXd _dummy(0,0);
  std::vector<std::string> _dummy_header;

  return writePLY(filename,V,F,_dummy, N, UV, VD, VDheader,
                         _dummy, _dummy_header, _dummy, _dummy_header, comments, FileEncoding::Binary);
}




template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedE,
  typename DerivedN,
  typename DerivedUV,
  typename DerivedVD
>
bool igl::writePLY(
  const std::string & filename,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedE> & E,
  const Eigen::MatrixBase<DerivedN> & N,
  const Eigen::MatrixBase<DerivedUV> & UV,
  const Eigen::MatrixBase<DerivedVD> & VD,
  const std::vector<std::string> & VDheader,
  const std::vector<std::string> & comments
  )
{
  Eigen::MatrixXd _dummy(0,0);
  std::vector<std::string> _dummy_header;

  return writePLY(filename,V,F,E, N, UV, VD, VDheader,
                         _dummy, _dummy_header, _dummy, _dummy_header, comments, FileEncoding::Binary);

}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::writePLY<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
template bool igl::writePLY<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::FileEncoding);
template bool igl::writePLY<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&, igl::FileEncoding);
template bool igl::writePLY<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&);
template bool igl::writePLY<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, igl::FileEncoding);
template bool igl::writePLY<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, igl::FileEncoding);
template bool igl::writePLY<Eigen::Matrix<double, 8, 3, 0, 8, 3>, Eigen::Matrix<int, 12, 3, 0, 12, 3> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 8, 3, 0, 8, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, 12, 3, 0, 12, 3> > const&, igl::FileEncoding);
template bool igl::writePLY<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&, std::vector<std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const&, igl::FileEncoding);
template bool igl::writePLY<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, igl::FileEncoding);
#endif
