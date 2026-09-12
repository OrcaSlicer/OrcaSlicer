#ifndef SLIC3R_TEST_UTILS
#define SLIC3R_TEST_UTILS

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Format/OBJ.hpp>
#include <libslic3r/SVG.hpp>
#include <libslic3r/Utils.hpp>

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

// ---------------------------------------------------------------------------
// Scoped temporary paths
// ---------------------------------------------------------------------------

// Owns a unique path under the system temp dir, "<prefix>-<unique>[<extension>]"
// (parallel-safe, cross-platform). Shared base for the RAII temp guards below.
class ScopedTemporaryPath
{
public:
    const boost::filesystem::path &path() const { return m_path; }
    std::string string() const { return m_path.string(); }
    ScopedTemporaryPath(const ScopedTemporaryPath &) = delete;
    ScopedTemporaryPath &operator=(const ScopedTemporaryPath &) = delete;

protected:
    ScopedTemporaryPath(const std::string &prefix, const std::string &extension)
        : m_path(boost::filesystem::temp_directory_path()
                 / boost::filesystem::unique_path(prefix + "-%%%%-%%%%-%%%%" + extension))
    {}
    ~ScopedTemporaryPath() = default; // non-virtual: never deleted through a base pointer

    boost::filesystem::path m_path;
};

// A temp file the caller creates by writing to path()/string(); the guard only
// reserves the name and removes the file on scope exit.
class ScopedTemporaryFile : public ScopedTemporaryPath
{
public:
    explicit ScopedTemporaryFile(const std::string &extension = ".tmp")
        : ScopedTemporaryPath("orca", extension) {}
    ~ScopedTemporaryFile() { boost::system::error_code ec; boost::filesystem::remove(m_path, ec); }
};

// A temp directory created on construction and removed recursively on scope exit.
class ScopedTemporaryDir : public ScopedTemporaryPath
{
public:
    explicit ScopedTemporaryDir(const std::string &prefix = "orca")
        : ScopedTemporaryPath(prefix, "") { boost::filesystem::create_directories(m_path); }
    ~ScopedTemporaryDir() { boost::system::error_code ec; boost::filesystem::remove_all(m_path, ec); }
};

// A temp directory that is also Slic3r::temporary_dir() for its lifetime. No test
// process sets that global, so code under test which writes there (for example
// StepPreProcessor::preprocess) lands at the filesystem root. Restored on scope exit
// even when an assertion throws, so it cannot leak into later tests.
class ScopedSlic3rTemporaryDir : public ScopedTemporaryDir
{
public:
    explicit ScopedSlic3rTemporaryDir(const std::string &prefix = "orca")
        : ScopedTemporaryDir(prefix), m_previous(Slic3r::temporary_dir())
    { Slic3r::set_temporary_dir(string()); }
    // Runs before ~ScopedTemporaryDir, so the setting goes back while the directory
    // it names still exists.
    ~ScopedSlic3rTemporaryDir() { Slic3r::set_temporary_dir(m_previous); }

private:
    const std::string m_previous;
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
        std::fprintf(stderr, "Debug test artifacts will be written to %s\n", dir.string().c_str());
        return dir;
    }();
    boost::filesystem::path full = root / name;
    boost::filesystem::create_directories(full.parent_path());
    return full.string();
}

// Dump a mesh as OBJ.
inline void write_debug_obj([[maybe_unused]] const std::string &name,
                            [[maybe_unused]] const Slic3r::TriangleMesh &mesh)
{
#ifndef NDEBUG
    mesh.WriteOBJFile(debug_artifact_path(name).c_str());
#endif
}

inline void write_debug_obj([[maybe_unused]] const std::string &name,
                            [[maybe_unused]] const indexed_triangle_set &its)
{
#ifndef NDEBUG
    its_write_obj(its, debug_artifact_path(name).c_str());
#endif
}

// Dump a mesh as ASCII STL.
inline void write_debug_stl([[maybe_unused]] const std::string &name,
                            [[maybe_unused]] const Slic3r::TriangleMesh &mesh)
{
#ifndef NDEBUG
    mesh.write_ascii(debug_artifact_path(name).c_str());
#endif
}

// Draw an SVG artifact through a callback that receives the open SVG. Second
// overload takes a BoundingBox when the drawing needs one.
template<class Draw>
inline void write_debug_svg([[maybe_unused]] const std::string &name, [[maybe_unused]] Draw &&draw)
{
#ifndef NDEBUG
    Slic3r::SVG svg(debug_artifact_path(name));
    draw(svg);
    svg.Close();
#endif
}

template<class Draw>
inline void write_debug_svg([[maybe_unused]] const std::string &name,
                            [[maybe_unused]] const Slic3r::BoundingBox &bbox,
                            [[maybe_unused]] Draw &&draw)
{
#ifndef NDEBUG
    Slic3r::SVG svg(debug_artifact_path(name), bbox);
    draw(svg);
    svg.Close();
#endif
}

// Write a callback's result (e.g. raster.encode(sla::PNGRasterEncoder{})) to an
// artifact. operator<< is resolved by ADL at the call site, so this header needn't
// include the producer's headers.
template<class Produce>
inline void write_debug_stream([[maybe_unused]] const std::string &name, [[maybe_unused]] Produce &&produce)
{
#ifndef NDEBUG
    std::ofstream out(debug_artifact_path(name), std::ios::out | std::ios::binary);
    out << produce();
#endif
}

#endif // SLIC3R_TEST_UTILS
