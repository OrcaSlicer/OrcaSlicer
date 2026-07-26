#ifndef SLIC3R_TEST_UTILS
#define SLIC3R_TEST_UTILS

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Format/OBJ.hpp>
#include <libslic3r/SVG.hpp>

#include <boost/filesystem.hpp>

#include <cstdio>
#include <fstream>
#include <string>

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

// ---------------------------------------------------------------------------
// Debug-only test artifacts
//
// Files a test dumps for inspection: a mesh, an SVG, or any streamable blob such
// as a PNG. In debug builds each run writes to a fresh temp folder (path printed
// once); the name may include a subfolder (e.g. "marchingsquares/foo.svg").
// ---------------------------------------------------------------------------

// Maps name to a path under the run's temp folder, creating any parent dirs
// (forward slashes work on Windows). Not gated, so only call it from a
// write_debug_* helper or inside an #ifndef NDEBUG block.
inline std::string debug_artifact_path(const std::string &name)
{
    static const boost::filesystem::path root = [] {
        boost::filesystem::path dir = boost::filesystem::temp_directory_path()
            / boost::filesystem::unique_path("orca-test-artifacts-%%%%-%%%%");
        boost::filesystem::create_directories(dir);
        std::printf("Debug test artifacts will be written to %s\n", dir.string().c_str());
        return dir;
    }();
    boost::filesystem::path full = root / name;
    boost::filesystem::create_directories(full.parent_path());
    return full.string();
}

// Dump a mesh as OBJ.
inline void write_debug_obj(const std::string &name, const Slic3r::TriangleMesh &mesh)
{
#ifndef NDEBUG
    mesh.WriteOBJFile(debug_artifact_path(name).c_str());
#else
    (void) name; (void) mesh;
#endif
}

inline void write_debug_obj(const std::string &name, const indexed_triangle_set &its)
{
#ifndef NDEBUG
    its_write_obj(its, debug_artifact_path(name).c_str());
#else
    (void) name; (void) its;
#endif
}

// Dump a mesh as ASCII STL.
inline void write_debug_stl(const std::string &name, const Slic3r::TriangleMesh &mesh)
{
#ifndef NDEBUG
    mesh.write_ascii(debug_artifact_path(name).c_str());
#else
    (void) name; (void) mesh;
#endif
}

// Draw an SVG artifact through a callback that receives the open SVG. Second
// overload takes a BoundingBox when the drawing needs one.
template<class Draw>
inline void write_debug_svg(const std::string &name, Draw &&draw)
{
#ifndef NDEBUG
    Slic3r::SVG svg(debug_artifact_path(name));
    draw(svg);
    svg.Close();
#else
    (void) name; (void) draw;
#endif
}

template<class Draw>
inline void write_debug_svg(const std::string &name, const Slic3r::BoundingBox &bbox, Draw &&draw)
{
#ifndef NDEBUG
    Slic3r::SVG svg(debug_artifact_path(name), bbox);
    draw(svg);
    svg.Close();
#else
    (void) name; (void) bbox; (void) draw;
#endif
}

// Write a callback's result (e.g. raster.encode(sla::PNGRasterEncoder{})) to an
// artifact. operator<< is resolved by ADL at the call site, so this header needn't
// include the producer's headers.
template<class Produce>
inline void write_debug_stream(const std::string &name, Produce &&produce)
{
#ifndef NDEBUG
    std::ofstream out(debug_artifact_path(name), std::ios::out | std::ios::binary);
    out << produce();
#else
    (void) name; (void) produce;
#endif
}

#endif // SLIC3R_TEST_UTILS
