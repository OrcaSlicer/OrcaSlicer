#include "MkvWriter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ios>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// Element IDs, written as their bytes (an EBML ID includes its own length marker).
constexpr uint32_t kEbml               = 0x1A45DFA3;
constexpr uint32_t kEbmlVersion        = 0x4286;
constexpr uint32_t kEbmlReadVersion    = 0x42F7;
constexpr uint32_t kEbmlMaxIdLength    = 0x42F2;
constexpr uint32_t kEbmlMaxSizeLength  = 0x42F3;
constexpr uint32_t kDocType            = 0x4282;
constexpr uint32_t kDocTypeVersion     = 0x4287;
constexpr uint32_t kDocTypeReadVersion = 0x4285;
constexpr uint32_t kSegment            = 0x18538067;
constexpr uint32_t kInfo               = 0x1549A966;
constexpr uint32_t kTimestampScale     = 0x2AD7B1;
constexpr uint32_t kMuxingApp          = 0x4D80;
constexpr uint32_t kWritingApp         = 0x5741;
constexpr uint32_t kDuration           = 0x4489;
constexpr uint32_t kTracks             = 0x1654AE6B;
constexpr uint32_t kTrackEntry         = 0xAE;
constexpr uint32_t kTrackNumber        = 0xD7;
constexpr uint32_t kTrackUid           = 0x73C5;
constexpr uint32_t kTrackType          = 0x83;
constexpr uint32_t kFlagLacing         = 0x9C;
constexpr uint32_t kCodecId            = 0x86;
constexpr uint32_t kDefaultDuration    = 0x23E383;
constexpr uint32_t kVideo              = 0xE0;
constexpr uint32_t kPixelWidth         = 0xB0;
constexpr uint32_t kPixelHeight        = 0xBA;
constexpr uint32_t kCluster            = 0x1F43B675;
constexpr uint32_t kClusterTimestamp   = 0xE7;
constexpr uint32_t kSimpleBlock        = 0xA3;

// A new Cluster this often, well inside the 32.767 s a block's 16-bit relative timestamp reaches.
constexpr int64_t kClusterMs = 5000;

// An 8-byte size field meaning "unknown", patched once the element ends.
const std::string kUnknownSize("\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8);

std::string id_bytes(uint32_t id)
{
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8)
        if (const unsigned char b = (id >> shift) & 0xFF; b != 0 || !out.empty())
            out.push_back(char(b));
    return out;
}

// The shortest EBML variable-size integer for `value`.
std::string vint(uint64_t value)
{
    int len = 1;
    while (len < 8 && value >= (uint64_t(1) << (7 * len)) - 1) // all ones is reserved for "unknown"
        ++len;
    std::string out(std::size_t(len), '\0');
    for (int i = len - 1; i >= 0; --i, value >>= 8)
        out[std::size_t(i)] = char(value & 0xFF);
    out[0] = char(out[0] | (0x80 >> (len - 1)));
    return out;
}

// An 8-byte EBML size field holding `value`, the form the patched sizes take.
std::string vint8(uint64_t value)
{
    std::string out(8, '\0');
    for (int i = 7; i >= 1; --i, value >>= 8)
        out[std::size_t(i)] = char(value & 0xFF);
    out[0] = char(0x01);
    return out;
}

std::string element(uint32_t id, const std::string& payload) { return id_bytes(id) + vint(payload.size()) + payload; }

std::string uint_element(uint32_t id, uint64_t value)
{
    std::string payload;
    do {
        payload.insert(payload.begin(), char(value & 0xFF));
        value >>= 8;
    } while (value != 0);
    return element(id, payload);
}

std::string float_bytes(double value)
{
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double is 64 bits");
    std::memcpy(&bits, &value, sizeof(bits));
    std::string out(8, '\0');
    for (int i = 7; i >= 0; --i, bits >>= 8)
        out[std::size_t(i)] = char(bits & 0xFF);
    return out;
}

} // namespace

bool MkvWriter::open(const std::string& path_utf8, int width, int height, double fps, std::string& err)
{
    m_out.open(path_utf8.c_str(), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!m_out.is_open()) {
        err = "Cannot create " + path_utf8;
        return false;
    }
    m_size = 0;
    m_cluster_size = 0;
    m_last_ms      = 0;

    const std::string header = element(kEbml, uint_element(kEbmlVersion, 1) + uint_element(kEbmlReadVersion, 1) +
                                                   uint_element(kEbmlMaxIdLength, 4) + uint_element(kEbmlMaxSizeLength, 8) +
                                                   element(kDocType, "matroska") + uint_element(kDocTypeVersion, 4) +
                                                   uint_element(kDocTypeReadVersion, 2));
    const std::string segment = id_bytes(kSegment) + kUnknownSize;

    // Duration goes last in Info, so its payload offset is easy to find for the patch on close.
    const std::string info_head = uint_element(kTimestampScale, 1000000) + element(kMuxingApp, "OrcaSlicer") +
                                  element(kWritingApp, "OrcaSlicer MCP recording") + id_bytes(kDuration) + vint(8);
    const std::string info = id_bytes(kInfo) + vint(info_head.size() + 8) + info_head;

    std::string video = uint_element(kPixelWidth, uint64_t(std::max(width, 1))) + uint_element(kPixelHeight, uint64_t(std::max(height, 1)));
    std::string track = uint_element(kTrackNumber, 1) + uint_element(kTrackUid, 1) + uint_element(kTrackType, 1) +
                        uint_element(kFlagLacing, 0) + element(kCodecId, "V_MJPEG");
    if (fps > 0)
        track += uint_element(kDefaultDuration, uint64_t(1e9 / fps));
    track += element(kVideo, video);
    const std::string tracks = element(kTracks, element(kTrackEntry, track));

    m_segment_data = header.size() + segment.size();
    m_duration_at  = m_segment_data + info.size();
    return write(header + segment + info + float_bytes(0.) + tracks, err);
}

bool MkvWriter::add_frame(int64_t ms, const unsigned char* data, std::size_t size, std::string& err)
{
    ms = std::max(ms, m_last_ms);
    if (m_cluster_size == 0 || ms - m_cluster_ms >= kClusterMs)
        if (!finish_cluster(err) || !start_cluster(ms, err))
            return false;
    m_last_ms = ms;
    const int16_t relative = int16_t(ms - m_cluster_ms);
    std::string   block    = vint(1); // track number 1
    block.push_back(char((uint16_t(relative) >> 8) & 0xFF));
    block.push_back(char(uint16_t(relative) & 0xFF));
    block.push_back(char(0x80)); // keyframe: every MJPEG frame is
    const std::string head = id_bytes(kSimpleBlock) + vint(block.size() + size) + block;
    return write(head, err) && write(std::string(reinterpret_cast<const char*>(data), size), err);
}

bool MkvWriter::close(int64_t end_ms, std::string& err)
{
    if (!m_out.is_open())
        return true;
    const bool ok = finish_cluster(err) && patch(m_segment_data - 8, vint8(m_size - m_segment_data), err) &&
                    patch(m_duration_at, float_bytes(double(std::max(end_ms, m_last_ms))), err);
    m_out.close();
    if (ok && m_out.fail()) {
        err = "Closing the recording failed";
        return false;
    }
    return ok;
}

bool MkvWriter::write(const std::string& bytes, std::string& err)
{
    m_out.write(bytes.data(), std::streamsize(bytes.size()));
    if (!m_out) {
        err = "Writing the recording failed (disk full?)";
        return false;
    }
    m_size += bytes.size();
    return true;
}

bool MkvWriter::start_cluster(int64_t ms, std::string& err)
{
    m_cluster_ms = ms;
    if (!write(id_bytes(kCluster), err))
        return false;
    m_cluster_size = m_size;
    return write(kUnknownSize, err) && write(uint_element(kClusterTimestamp, uint64_t(std::max<int64_t>(ms, 0))), err);
}

bool MkvWriter::finish_cluster(std::string& err)
{
    if (m_cluster_size == 0)
        return true;
    const uint64_t at = m_cluster_size;
    m_cluster_size    = 0;
    return patch(at, vint8(m_size - (at + 8)), err);
}

bool MkvWriter::patch(uint64_t at, const std::string& bytes, std::string& err)
{
    m_out.seekp(std::streamoff(at));
    m_out.write(bytes.data(), std::streamsize(bytes.size()));
    m_out.seekp(std::streamoff(m_size));
    if (!m_out) {
        err = "Writing the recording failed (disk full?)";
        return false;
    }
    return true;
}

}}} // namespace Slic3r::GUI::Mcp
