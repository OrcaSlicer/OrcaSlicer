#include <exception>

#include "miniz_extension.hpp"

#if defined(_MSC_VER) || defined(__MINGW64__)
#include "boost/nowide/cstdio.hpp"
#endif

#include "I18N.hpp"

//! macro used to mark string used at localization,
//! return same string
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

namespace {
bool open_zip(mz_zip_archive *zip, const char *fname, bool isread)
{
    if (!zip) return false;
    const char *mode = isread ? "rb" : "wb";

    FILE *f = nullptr;
#if defined(_MSC_VER) || defined(__MINGW64__)
    f = boost::nowide::fopen(fname, mode);
#elif defined(__GNUC__) && defined(_LARGEFILE64_SOURCE)
    f = fopen64(fname, mode);
#else
    f = fopen(fname, mode);
#endif

    if (!f) {
        zip->m_last_error = MZ_ZIP_FILE_OPEN_FAILED;
        return false;
    }

    bool res = false;
    if (isread)
    {
        res = mz_zip_reader_init_cfile(zip, f, 0, 0);
        if (!res)
            // if we get here it means we tried to open a non-zip file
            // we need to close the file here because the call to mz_zip_get_cfile() made into close_zip() returns a null pointer
            // see: https://github.com/prusa3d/PrusaSlicer/issues/3536
            fclose(f);
    }
    else
        res = mz_zip_writer_init_cfile(zip, f, 0);

    return res;
}

bool close_zip(mz_zip_archive *zip, bool isread)
{
    bool ret = false;
    if (zip) {
        FILE *f = mz_zip_get_cfile(zip);
        ret     = bool(isread ? mz_zip_reader_end(zip)
                          : mz_zip_writer_end(zip));
        if (f) fclose(f);
    }
    return ret;
}
}

bool open_zip_reader(mz_zip_archive *zip, const std::string &fname)
{
    return open_zip(zip, fname.c_str(), true);
}

bool open_zip_writer(mz_zip_archive *zip, const std::string &fname)
{
    return open_zip(zip, fname.c_str(), false);
}

bool close_zip_reader(mz_zip_archive *zip) { return close_zip(zip, true); }
bool close_zip_writer(mz_zip_archive *zip) { return close_zip(zip, false); }

MZ_Archive::MZ_Archive()
{
    mz_zip_zero_struct(&arch);
}

std::string MZ_Archive::get_errorstr(mz_zip_error mz_err)
{
    switch (mz_err)
    {
    case MZ_ZIP_NO_ERROR:
        return "no error";
    case MZ_ZIP_UNDEFINED_ERROR:
        return L("undefined error");
    case MZ_ZIP_TOO_MANY_FILES:
        return L("too many files");
    case MZ_ZIP_FILE_TOO_LARGE:
        return L("file too large");
    case MZ_ZIP_UNSUPPORTED_METHOD:
        return L("unsupported method");
    case MZ_ZIP_UNSUPPORTED_ENCRYPTION:
        return L("unsupported encryption");
    case MZ_ZIP_UNSUPPORTED_FEATURE:
        return L("unsupported feature");
    case MZ_ZIP_FAILED_FINDING_CENTRAL_DIR:
        return L("failed finding central directory");
    case MZ_ZIP_NOT_AN_ARCHIVE:
        return L("not a ZIP archive");
    case MZ_ZIP_INVALID_HEADER_OR_CORRUPTED:
        return L("invalid header or corrupted");
    case MZ_ZIP_UNSUPPORTED_MULTIDISK:
        return L("unsupported multidisk");
    case MZ_ZIP_DECOMPRESSION_FAILED:
        return L("decompression failed");
    case MZ_ZIP_COMPRESSION_FAILED:
        return L("compression failed");
    case MZ_ZIP_UNEXPECTED_DECOMPRESSED_SIZE:
        return L("unexpected decompressed size");
    case MZ_ZIP_CRC_CHECK_FAILED:
        return L("CRC check failed");
    case MZ_ZIP_UNSUPPORTED_CDIR_SIZE:
        return L("unsupported central directory size");
    case MZ_ZIP_ALLOC_FAILED:
        return L("allocation failed");
    case MZ_ZIP_FILE_OPEN_FAILED:
        return L("file open failed");
    case MZ_ZIP_FILE_CREATE_FAILED:
        return L("file create failed");
    case MZ_ZIP_FILE_WRITE_FAILED:
        return L("file write failed");
    case MZ_ZIP_FILE_READ_FAILED:
        return L("file read failed");
    case MZ_ZIP_FILE_CLOSE_FAILED:
        return L("file close failed");
    case MZ_ZIP_FILE_SEEK_FAILED:
        return L("file seek failed");
    case MZ_ZIP_FILE_STAT_FAILED:
        return L("file stat failed");
    case MZ_ZIP_INVALID_PARAMETER:
        return L("invalid parameter");
    case MZ_ZIP_INVALID_FILENAME:
        return L("invalid filename");
    case MZ_ZIP_BUF_TOO_SMALL:
        return L("buffer too small");
    case MZ_ZIP_INTERNAL_ERROR:
        return L("internal error");
    case MZ_ZIP_FILE_NOT_FOUND:
        return L("file not found");
    case MZ_ZIP_ARCHIVE_TOO_LARGE:
        return L("archive too large");
    case MZ_ZIP_VALIDATION_FAILED:
        return L("validation failed");
    case MZ_ZIP_WRITE_CALLBACK_FAILED:
        return L("write callback failed");
    default:
        break;
    }

    return "unknown error";
}

} // namespace Slic3r
