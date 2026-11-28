#ifndef MINIZ_EXTENSION_HPP
#define MINIZ_EXTENSION_HPP

#include <boost/filesystem/path.hpp>
#include <string>
#include <miniz.h>

namespace Slic3r {

bool open_zip_reader(mz_zip_archive *zip, const std::string &fname_utf8);
bool open_zip_writer(mz_zip_archive *zip, const std::string &fname_utf8);
bool close_zip_reader(mz_zip_archive *zip);
bool close_zip_writer(mz_zip_archive *zip);
std::string decode_archive_entry_path(mz_zip_archive *zip, const mz_zip_archive_file_stat &stat);
// Extracts every entry of the archive under dest_dir. Nothing is written if any entry would resolve outside dest_dir.
bool extract_archive_confined(const std::string &zip_path_utf8, const std::string &dest_dir);
// Extracts one entry into dest_path, binary-safe and overwriting. On Windows the destination is
// opened through an extended-length path so it works past MAX_PATH even with LongPathsEnabled off;
// a partial file is removed if the write fails. False for a directory/unsupported entry or on error.
bool extract_zip_entry_to_file(mz_zip_archive *zip, mz_uint file_index, const boost::filesystem::path &dest_path);

class MZ_Archive {
public:
    mz_zip_archive arch;
    
    MZ_Archive();
    
    static std::string get_errorstr(mz_zip_error mz_err);
    
    std::string get_errorstr() const
    {
        return get_errorstr(arch.m_last_error) + "!";
    }

    bool is_alive() const
    {
        return arch.m_zip_mode != MZ_ZIP_MODE_WRITING_HAS_BEEN_FINALIZED;
    }
};

} // namespace Slic3r

#endif // MINIZ_EXTENSION_HPP
