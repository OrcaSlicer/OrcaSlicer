#ifndef slic3r_AnimatedTexture_hpp_
#define slic3r_AnimatedTexture_hpp_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "slic3r/GUI/GLTexture.hpp"

namespace Slic3r::GUI {

// An illustration for the UI to draw: one still image, or the frames of an animated GIF together with
// the delays the file asks for. Decoded on the CPU once and uploaded as one GL texture per frame.
//
// A texture per frame rather than an atlas: these are small hints a few hundred pixels across and a
// handful of frames long, so the extra textures cost little, while drawing a frame stays a plain
// ImGui::Image with no UV arithmetic and no dependence on the maximum texture size.
//
// load_from_file() uploads to the GPU, so it needs a current GL context - call it from rendering code.
class AnimatedTexture
{
public:
    AnimatedTexture()                                   = default;
    AnimatedTexture(AnimatedTexture &&)                 = default;
    AnimatedTexture &operator=(AnimatedTexture &&)      = default;
    AnimatedTexture(const AnimatedTexture &)            = delete;
    AnimatedTexture &operator=(const AnimatedTexture &) = delete;

    // PNG, JPG and BMP through wxImage; GIF, animated or not, through wxGIFDecoder. False, and nothing
    // loaded, if the file is missing or cannot be decoded.
    bool load_from_file(const std::string &path);
    void reset();

    bool is_valid() const { return !m_frames.empty(); }
    bool is_animated() const { return m_frames.size() > 1; }
    int  width() const { return m_width; }
    int  height() const { return m_height; }
    // Length of one loop, in seconds; 0 for a still image.
    double duration() const { return m_duration; }

    // The texture to draw `time` seconds into the loop, which wraps. 0 when nothing is loaded.
    unsigned int texture_id(double time) const;

private:
    struct Frame
    {
        std::unique_ptr<GLTexture> texture;
        double                     start{0.}; // seconds into the loop this frame appears at
        double                     delay{0.}; // seconds it stays up
    };

    std::vector<Frame> m_frames;
    int                m_width{0};
    int                m_height{0};
    double             m_duration{0.};
};

// The illustrations a panel has shown at least once, keyed by absolute path, decoded and uploaded on
// first use and kept until clear() - which has to run while the GL context is still current.
class AnimatedTextureCache
{
public:
    // Null when the file is missing or cannot be decoded. The failure is remembered too, so a tooltip
    // whose illustration has not been drawn yet costs one lookup, not one decode attempt per frame.
    const AnimatedTexture *get(const std::string &path);
    void                   clear() { m_items.clear(); }

private:
    std::map<std::string, std::unique_ptr<AnimatedTexture>> m_items; // null value: tried once, failed
};

} // namespace Slic3r::GUI

#endif // slic3r_AnimatedTexture_hpp_
