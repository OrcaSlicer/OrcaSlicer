#ifndef SLIC3R_TEST_UTILS
#define SLIC3R_TEST_UTILS

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Format/OBJ.hpp>

#include <boost/filesystem.hpp>

#if defined(WIN32) || defined(_WIN32)
#define PATH_SEPARATOR R"(\)"
#else
#define PATH_SEPARATOR R"(/)"
#endif

inline Slic3r::TriangleMesh load_model(const std::string &obj_filename)
{
    Slic3r::TriangleMesh mesh;
    auto fpath = TEST_DATA_DIR PATH_SEPARATOR + obj_filename;
    Slic3r::ObjInfo obj_info;
    std::string message;
    Slic3r::load_obj(fpath.c_str(), &mesh, obj_info, message);
    return mesh;
}

// RAII holder for a unique temporary file path, removed when the guard goes out
// of scope so a failing assertion never leaks it. Uses the system temp dir with
// a unique name (parallel-safe, cross-platform). The file itself is created by
// whoever writes to path()/string(); this only reserves the name and cleans up.
class ScopedTemporaryFile
{
public:
    explicit ScopedTemporaryFile(const std::string &extension = ".tmp")
        : m_path(boost::filesystem::temp_directory_path()
                 / boost::filesystem::unique_path("orca-%%%%-%%%%-%%%%" + extension))
    {}
    ~ScopedTemporaryFile() { boost::system::error_code ec; boost::filesystem::remove(m_path, ec); }
    ScopedTemporaryFile(const ScopedTemporaryFile &) = delete;
    ScopedTemporaryFile &operator=(const ScopedTemporaryFile &) = delete;

    const boost::filesystem::path &path() const { return m_path; }
    std::string string() const { return m_path.string(); }

private:
    boost::filesystem::path m_path;
};

#endif // SLIC3R_TEST_UTILS
