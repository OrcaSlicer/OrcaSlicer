#pragma once

#include <boost/nowide/fstream.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// Writes MJPEG frames into a Matroska file as they come: one video track (V_MJPEG) whose frames carry
// their own millisecond timestamps, so a recording keeps its real, uneven timing. The Segment and each
// Cluster are written with an "unknown" size and patched by close(), so a file whose writer never
// closed it (a crash) still reads up to its last frame, and a closed file has exact sizes and a
// Duration. Not thread-safe; wx-free.
class MkvWriter
{
public:
    MkvWriter() = default;
    MkvWriter(const MkvWriter&)            = delete;
    MkvWriter& operator=(const MkvWriter&) = delete;

    // Creates `path_utf8` (replacing it) and writes the header of one `width` x `height` MJPEG track.
    // `fps` only sets the track's DefaultDuration, a hint for players.
    bool open(const std::string& path_utf8, int width, int height, double fps, std::string& err);
    // Appends one JPEG image at `ms` from the start. A timestamp lower than the previous one is raised
    // to it.
    bool add_frame(int64_t ms, const unsigned char* data, std::size_t size, std::string& err);
    // Patches the sizes and sets the Duration to `end_ms`, then closes the file.
    bool close(int64_t end_ms, std::string& err);

    bool     is_open() const { return m_out.is_open(); }
    uint64_t bytes_written() const { return m_size; }

private:
    bool write(const std::string& bytes, std::string& err);
    bool start_cluster(int64_t ms, std::string& err);
    bool finish_cluster(std::string& err);
    bool patch(uint64_t at, const std::string& bytes, std::string& err);

    boost::nowide::fstream m_out;
    uint64_t               m_size          = 0; // bytes written so far, the end of the file
    uint64_t               m_segment_data  = 0; // offset of the Segment's payload
    uint64_t               m_duration_at   = 0; // offset of the Duration float's payload
    uint64_t               m_cluster_size  = 0; // offset of the open Cluster's size field, 0 for none
    int64_t                m_cluster_ms    = 0;
    int64_t                m_last_ms       = 0;
};

}}} // namespace Slic3r::GUI::Mcp
