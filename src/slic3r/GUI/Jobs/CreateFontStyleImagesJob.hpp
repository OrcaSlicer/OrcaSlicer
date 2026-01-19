#ifndef slic3r_CreateFontStyleImagesJob_hpp_
#define slic3r_CreateFontStyleImagesJob_hpp_

#include <vector>
#include <string>
#include <libslic3r/Emboss.hpp>
#include "slic3r/Utils/EmbossStyleManager.hpp"
#include "Job.hpp"

namespace Slic3r::GUI::Emboss {

/// <summary>
/// Create texture with name of styles written by its style
/// NOTE: Access to glyph cache is possible only from job
/// </summary>
class CreateFontStyleImagesJob : public Job
{
    StyleManager::StyleImagesData m_input;

    // Output data
    // texture size
    int m_width, m_height;
    // texture data
    std::vector<unsigned char> m_pixels; 
    // descriptors of sub textures
    std::vector<StyleManager::StyleImage> m_images;

public:
    CreateFontStyleImagesJob(StyleManager::StyleImagesData &&input);
    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &) override;
};

} // namespace Slic3r::GUI

#endif // slic3r_CreateFontStyleImagesJob_hpp_
