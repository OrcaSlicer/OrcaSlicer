#include "AnimatedTexture.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/system/error_code.hpp>

#include <wx/animdecod.h>
#include <wx/gdicmn.h>
#include <wx/gifdecod.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/mstream.h>

#include "slic3r/GUI/GLTexture.hpp"

namespace Slic3r::GUI {

namespace {

std::vector<unsigned char> read_file_bytes(const std::string &path)
{
    std::ifstream ifs(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

bool has_gif_signature(const std::vector<unsigned char> &bytes)
{
    return bytes.size() > 6 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F';
}

// wxImage keeps colour and alpha in two separate buffers, and a file with no alpha at all has none;
// OpenGL wants the four channels interleaved.
std::vector<unsigned char> image_to_rgba(const wxImage &image)
{
    const size_t         n   = size_t(image.GetWidth()) * size_t(image.GetHeight());
    const unsigned char *rgb = image.GetData();
    const unsigned char *a   = image.HasAlpha() ? image.GetAlpha() : nullptr;

    std::vector<unsigned char> out(n * 4);
    for (size_t i = 0; i < n; ++i) {
        out[i * 4 + 0] = rgb[i * 3 + 0];
        out[i * 4 + 1] = rgb[i * 3 + 1];
        out[i * 4 + 2] = rgb[i * 3 + 2];
        out[i * 4 + 3] = a != nullptr ? a[i] : 255;
    }
    return out;
}

// One decoded frame on its way to the GPU: the whole animation's area, already composited.
struct DecodedFrame
{
    std::vector<unsigned char> rgba;
    double                     delay; // seconds
};

// Every frame of a GIF, composited onto the animation's own canvas.
//
// A GIF frame is a sub-rectangle that is drawn over what came before it, and the one before says how
// to clean up after itself (leave it, erase it, put back what it covered). Without replaying that, an
// animation that only re-sends the pixels that changed - which is most of them - draws as a flicker of
// disconnected patches. wxGIFDecoder hands over the raw palette indices, which is what makes replaying
// it here straightforward: a transparent index means "keep what is underneath", nothing else does.
bool decode_gif(const std::vector<unsigned char> &bytes, int &width, int &height, std::vector<DecodedFrame> &out)
{
    wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxGIFDecoder        decoder;

    // A truncated file still decodes everything up to the cut, which is worth showing.
    const wxGIFErrorCode err = decoder.LoadGIF(stream);
    if ((err != wxGIF_OK && err != wxGIF_TRUNCATED) || decoder.GetFrameCount() == 0)
        return false;

    const wxSize size = decoder.GetAnimationSize();
    width             = size.GetWidth();
    height            = size.GetHeight();
    if (width <= 0 || height <= 0)
        return false;

    std::vector<unsigned char> canvas(size_t(width) * size_t(height) * 4, 0);
    std::vector<unsigned char> saved;

    for (unsigned int i = 0; i < decoder.GetFrameCount(); ++i) {
        const wxSize              frame_size = decoder.GetFrameSize(i);
        const wxPoint             frame_pos  = decoder.GetFramePosition(i);
        const wxAnimationDisposal disposal   = decoder.GetDisposalMethod(i);
        const unsigned char      *indices    = decoder.GetData(i);
        const unsigned char      *palette    = decoder.GetPalette(i);
        const int                 transparent = decoder.GetTransparentColourIndex(i);

        if (disposal == wxANIM_TOPREVIOUS)
            saved = canvas;

        if (indices != nullptr && palette != nullptr)
            for (int y = 0; y < frame_size.GetHeight(); ++y) {
                const int cy = frame_pos.y + y;
                if (cy < 0 || cy >= height)
                    continue;
                for (int x = 0; x < frame_size.GetWidth(); ++x) {
                    const int cx = frame_pos.x + x;
                    if (cx < 0 || cx >= width)
                        continue;
                    const unsigned char index = indices[size_t(y) * size_t(frame_size.GetWidth()) + size_t(x)];
                    if (int(index) == transparent)
                        continue;
                    unsigned char *dst = &canvas[(size_t(cy) * size_t(width) + size_t(cx)) * 4];
                    dst[0]             = palette[3 * size_t(index) + 0];
                    dst[1]             = palette[3 * size_t(index) + 1];
                    dst[2]             = palette[3 * size_t(index) + 2];
                    dst[3]             = 255;
                }
            }

        // Browsers treat 0 and 1 hundredths of a second as "as fast as the file can mean", and settle
        // on 100 ms; an animation authored against that convention plays at the intended speed only if
        // this does the same.
        const long delay_ms = decoder.GetDelay(i);
        out.push_back({ canvas, (delay_ms <= 10 ? 100.0 : double(delay_ms)) / 1000.0 });

        if (disposal == wxANIM_TOBACKGROUND) {
            for (int y = 0; y < frame_size.GetHeight(); ++y) {
                const int cy = frame_pos.y + y;
                if (cy < 0 || cy >= height)
                    continue;
                const int x0 = std::max(0, frame_pos.x);
                const int x1 = std::min(width, frame_pos.x + frame_size.GetWidth());
                if (x1 > x0)
                    std::fill_n(&canvas[(size_t(cy) * size_t(width) + size_t(x0)) * 4], size_t(x1 - x0) * 4, 0);
            }
        } else if (disposal == wxANIM_TOPREVIOUS) {
            canvas = saved;
        }
    }
    return !out.empty();
}

} // namespace

bool AnimatedTexture::load_from_file(const std::string &path)
{
    reset();

    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(path, ec))
        return false;

    const std::vector<unsigned char> bytes = read_file_bytes(path);
    if (bytes.empty())
        return false;

    std::vector<DecodedFrame> frames;
    int                       width = 0, height = 0;

    if (has_gif_signature(bytes)) {
        if (!decode_gif(bytes, width, height, frames)) {
            BOOST_LOG_TRIVIAL(error) << "AnimatedTexture: cannot decode GIF " << path;
            return false;
        }
    } else {
        wxLogNull           no_log; // a format wxWidgets cannot read is handled here, not in a dialog
        wxImage             image;
        wxMemoryInputStream stream(bytes.data(), bytes.size());
        if (!image.LoadFile(stream, wxBITMAP_TYPE_ANY) || !image.IsOk()) {
            BOOST_LOG_TRIVIAL(error) << "AnimatedTexture: cannot decode image " << path;
            return false;
        }
        width  = image.GetWidth();
        height = image.GetHeight();
        frames.push_back({ image_to_rgba(image), 0.0 });
    }

    m_width  = width;
    m_height = height;
    m_frames.reserve(frames.size());
    for (DecodedFrame &frame : frames) {
        auto texture = std::make_unique<GLTexture>();
        // Mipmapped: a hint is drawn at whatever width the panel has room for, which is usually
        // smaller than the source, and an unfiltered downscale of a diagram is all aliasing.
        if (!texture->load_from_raw_data(std::move(frame.rgba), (unsigned int) width, (unsigned int) height, false,
                                         /* use_mipmaps */ true)) {
            BOOST_LOG_TRIVIAL(error) << "AnimatedTexture: cannot upload " << path;
            reset();
            return false;
        }
        m_frames.push_back({ std::move(texture), m_duration, frame.delay });
        m_duration += frame.delay;
    }
    return true;
}

void AnimatedTexture::reset()
{
    m_frames.clear();
    m_width    = 0;
    m_height   = 0;
    m_duration = 0.;
}

unsigned int AnimatedTexture::texture_id(double time) const
{
    if (m_frames.empty())
        return 0;
    if (m_frames.size() == 1 || m_duration <= 0.)
        return m_frames.front().texture->get_id();

    const double t = time - m_duration * std::floor(time / m_duration);
    // Frames are few; a linear walk beats keeping an index around and getting it stale.
    size_t i = 0;
    while (i + 1 < m_frames.size() && t >= m_frames[i].start + m_frames[i].delay)
        ++i;
    return m_frames[i].texture->get_id();
}

const AnimatedTexture *AnimatedTextureCache::get(const std::string &path)
{
    const auto it = m_items.find(path);
    if (it != m_items.end())
        return it->second.get();

    auto texture = std::make_unique<AnimatedTexture>();
    if (!texture->load_from_file(path))
        texture.reset(); // remembered as a failure, so the file is not re-read every frame

    const AnimatedTexture *result = texture.get();
    m_items.emplace(path, std::move(texture));
    return result;
}

} // namespace Slic3r::GUI
