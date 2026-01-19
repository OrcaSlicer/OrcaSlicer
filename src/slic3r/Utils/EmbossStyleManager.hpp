#ifndef slic3r_EmbossStyleManager_hpp_
#define slic3r_EmbossStyleManager_hpp_

#include <memory>
#include <optional>
#include <string>
#include <functional>
#include <imgui/imgui.h>
#include <wx/font.h>
#include <GL/glew.h>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/Emboss.hpp>
#include <libslic3r/TextConfiguration.hpp>
#include <libslic3r/EmbossShape.hpp>
#include <libslic3r/AppConfig.hpp>

namespace Slic3r::GUI::Emboss {
/// <summary>
/// Manage Emboss text styles
/// Cache actual state of style
///     + imgui font
///     + wx font
/// </summary>
class StyleManager
{
    friend class CreateFontStyleImagesJob; // access to StyleImagesData
public:
    /// <param name="language_glyph_range">Character to load for imgui when initialize imgui font</param>
    /// <param name="create_default_styles">Function to create default styles</param>
    StyleManager(const ImWchar *language_glyph_range, const std::function<EmbossStyles()>& create_default_styles);
        
    /// <summary>
    /// Release imgui font and style images from GPU
    /// </summary>
    ~StyleManager();

    /// <summary>
    /// Load font style list from config
    /// Also select actual activ font
    /// </summary>
    /// <param name="app_config">Application configuration loaded from file "PrusaSlicer.ini"
    /// + cfg is stored to privat variable</param>
    void init(AppConfig *app_config);
    
    /// <summary>
    /// Write font list into AppConfig
    /// </summary>
    /// <param name="item_to_store">Configuration</param>
    /// <param name="use_modification">When true cache state will be used for store</param>
    /// <param name="use_modification">When true store activ index into configuration</param>
    /// <returns>True on succes otherwise False.</returns>
    bool store_styles_to_app_config(bool use_modification = true, bool store_active_index = true);

    /// <summary>
    /// Append actual style to style list
    /// </summary>
    /// <param name="name">New name for style</param>
    void add_style(const std::string& name);

    /// <summary>
    /// Change order of style item in m_styles.
    /// Fix selected font index when (i1 || i2) == m_font_selected 
    /// </summary>
    /// <param name="i1">First index to m_styles</param>
    /// <param name="i2">Second index to m_styles</param>
    void swap(size_t i1, size_t i2);

    /// <summary>
    /// Discard changes in activ style
    /// When no activ style use last used OR first loadable
    /// </summary>
    void discard_style_changes();

    /// <summary>
    /// Remove style from m_styles.
    /// Fix selected font index when index is under m_font_selected
    /// </summary>
    /// <param name="index">Index of style to be removed</param>
    void erase(size_t index);

    /// <summary>
    /// Rename actual selected font item
    /// </summary>
    /// <param name="name">New name</param>
    void rename(const std::string &name);
        
    /// <summary>
    /// load some valid style
    /// </summary>
    void load_valid_style();

    /// <summary>
    /// Change active font
    /// When font not loaded roll back activ font
    /// </summary>
    /// <param name="font_index">New font index(from m_styles range)</param>
    /// <returns>True on succes. False on fail load font</returns>
    bool load_style(size_t font_index);
    // load font style not stored in list
    struct Style;
    bool load_style(const Style &style);
    // fastering load font on index by wxFont, ignore type and descriptor
    bool load_style(const Style &style, const wxFont &font);
    
    // clear actual selected glyphs cache
    void clear_glyphs_cache();

    // remove cached imgui font for actual selected font
    void clear_imgui_font();

    // getters for private data
    const Style *get_stored_style() const;

    const Style &get_style() const     { return m_style_cache.style; }
          Style &get_style()           { return m_style_cache.style; }
          size_t get_style_index() const     { return m_style_cache.style_index; }
    std::string &get_truncated_name()        { return m_style_cache.truncated_name; }
    const ImFontAtlas &get_atlas() const     { return m_style_cache.atlas; } 
    const FontProp    &get_font_prop() const { return get_style().prop; }
          FontProp    &get_font_prop()       { return get_style().prop; }
    const wxFont &get_wx_font()        const { return m_style_cache.wx_font; }
    const wxFont &get_stored_wx_font() const { return m_style_cache.stored_wx_font; }
    Slic3r::Emboss::FontFileWithCache &get_font_file_with_cache()   { return m_style_cache.font_file; }
    bool has_collections() const { return m_style_cache.font_file.font_file != nullptr && 
                                          m_style_cache.font_file.font_file->infos.size() > 1; }

    // True when activ style has same name as some of stored style
    bool exist_stored_style() const { return m_style_cache.style_index != std::numeric_limits<size_t>::max(); }
    
    /// <summary>
    /// check whether current style differ to selected
    /// </summary>
    /// <returns></returns>
    bool is_font_changed() const;

    bool is_unique_style_name(const std::string &name) const;

    /// <summary>
    /// Setter on wx_font when changed
    /// </summary>
    /// <param name="wx_font">new wx font</param>
    /// <returns>True on success set otherwise FALSE</returns>
    bool set_wx_font(const wxFont &wx_font);

    /// <summary>
    /// Faster way of set wx_font when font file is known(do not load font file twice)
    /// When you not sure that wx_font is made by font_file use only set_wx_font(wx_font)
    /// </summary>
    /// <param name="wx_font">Must be source of font file</param>
    /// <param name="font_file">font file created by WxFontUtils::create_font_file(wx_font)</param>
    /// <returns>True on success otherwise false</returns>
    bool set_wx_font(const wxFont &wx_font, std::unique_ptr<Slic3r::Emboss::FontFile> font_file);

    // Getter on acitve font pointer for imgui
    // Initialize imgui font(generate texture) when doesn't exist yet.
    // Extend font atlas when not in glyph range
    ImFont *get_imgui_font();
    // initialize font range by unique symbols in text
    ImFont *create_imgui_font(const std::string& text, double scale);
    
    // init truncated names of styles
    void init_trunc_names(float max_width);

    /// <summary>
    /// Initialization texture with rendered font style
    /// </summary>
    /// <param name="max_size">Maximal width and height of one style texture</param>
    /// <param name="text">Text to render by style</param>
    void init_style_images(const Vec2i32& max_size, const std::string &text);
    void free_style_images();
    
    // access to all managed font styles
    const std::vector<Style> &get_styles() const;

    /// <summary>
    /// Describe image in GPU to show settings of style
    /// </summary>
    struct StyleImage
    {
        void* texture_id = nullptr; // GLuint
        BoundingBox bounding_box;
        ImVec2 tex_size;
        ImVec2 uv0;
        ImVec2 uv1;
        Point  offset = Point(0, 0);
    };

    /// <summary>
    /// All connected with one style 
    /// keep temporary data and caches for style
    /// </summary>
    struct Style : public EmbossStyle
    {
        // Define how to emboss shape
        EmbossProjection projection;

        // distance from surface point
        // used for move over model surface
        // When not set value is zero and is not stored
        std::optional<float> distance; // [in mm]

        // Angle of rotation around emboss direction (Z axis)
        // It is calculate on the fly from volume world transformation
        // only StyleManager keep actual value for comparision with style
        // When not set value is zero and is not stored
        std::optional<float> angle; // [in radians] form -Pi to Pi

        bool operator==(const Style &other) const
        {
            return EmbossStyle::operator==(other) && 
                projection == other.projection &&
                distance == other.distance && 
                angle == other.angle;
        }

        // cache for view font name with maximal width in imgui
        std::string truncated_name; 

        // visualization of style
        std::optional<StyleImage> image;
    };
    using Styles = std::vector<Style>;

    // check if exist selected font style in manager
    bool is_active_font();

    // Limits for imgui loaded font size
    // Value out of limits is crop
    static float min_imgui_font_size;
    static float max_imgui_font_size;
    static float get_imgui_font_size(const FontProp &prop, const Slic3r::Emboss::FontFile &file, double scale);

private:
    // function to create default style list
    std::function<EmbossStyles()> m_create_default_styles;
    // keep language dependent glyph range
    const ImWchar *m_imgui_init_glyph_range;

    /// <summary>
    /// Cache data from style to reduce amount of:
    /// 1) loading font from file
    /// 2) Create atlas of symbols for imgui
    /// 3) Keep loaded(and modified by style) glyphs from font
    /// </summary>
    struct StyleCache
    {
        // share font file data with emboss job thread
        Slic3r::Emboss::FontFileWithCache font_file = {};

        // must live same as imgui_font inside of atlas
        ImVector<ImWchar> ranges = {};

        // Keep only actual style in atlas
        ImFontAtlas atlas = {};

        // wx widget font
        wxFont wx_font = {};

        // cache for view font name with maximal width in imgui
        std::string truncated_name; 

        // actual used font item
        Style style = {};

        // cache for stored wx font to not create every frame
        wxFont stored_wx_font = {};

        // index into m_styles
        size_t style_index = std::numeric_limits<size_t>::max();

    } m_style_cache;

    // Privat member
    Styles m_styles;
    AppConfig *m_app_config = nullptr;
    size_t m_last_style_index = std::numeric_limits<size_t>::max();

    /// <summary>
    /// Keep data needed to create Font Style Images in Job
    /// </summary>
    struct StyleImagesData
    {
        struct Item
        {
            Slic3r::Emboss::FontFileWithCache font;
            std::string               text;
            FontProp                  prop;
        };
        using Items = std::vector<Item>;

        // Keep styles to render
        Items styles;
        // Maximal width and height in pixels of image
        Vec2i32 max_size;
        // Text to render
        std::string text;

        /// <summary>
        /// Result of job
        /// </summary>
        struct StyleImages
        {
            // vector of inputs
            StyleImagesData::Items styles;
            // job output
            std::vector<StyleImage> images;
        };

        // place to store result in main thread in Finalize
        std::shared_ptr<StyleImages> result;
                
        // pixel per milimeter (scaled DPI)
        double ppm;
    };
    std::shared_ptr<StyleImagesData::StyleImages> m_temp_style_images = nullptr;
    bool m_exist_style_images = false;
};

} // namespace Slic3r

#endif // slic3r_EmbossStyleManager_hpp_
