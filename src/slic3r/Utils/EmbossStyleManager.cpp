#include "EmbossStyleManager.hpp"
#include <optional>
#include <GL/glew.h> // Imgui texture
#include <imgui/imgui_internal.h> // ImTextCharFromUtf8
#include <libslic3r/AppConfig.hpp>
#include <libslic3r/Utils.hpp> // ScopeGuard

#include "WxFontUtils.hpp"
#include "slic3r/GUI/3DScene.hpp" // ::glsafe
#include "slic3r/GUI/Jobs/CreateFontStyleImagesJob.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp" // check of font ranges

#include <boost/assign.hpp>
#include <boost/bimap.hpp>

using namespace Slic3r;
using namespace Slic3r::Emboss;
using namespace Slic3r::GUI::Emboss;

StyleManager::StyleManager(const ImWchar *language_glyph_range, const std::function<EmbossStyles()>& create_default_styles)
    : m_create_default_styles(create_default_styles)
    , m_imgui_init_glyph_range(language_glyph_range)
{}

StyleManager::~StyleManager() { 
    clear_imgui_font();
    free_style_images();
}

/// <summary>
/// For store/load emboss style to/from AppConfig
/// </summary>
namespace {
void                    store_style_index(AppConfig &cfg, size_t index);
::std::optional<size_t> load_style_index(const AppConfig &cfg);

StyleManager::Styles load_styles(const AppConfig &cfg);
void store_styles(AppConfig &cfg, const StyleManager::Styles &styles);
void make_unique_name(const StyleManager::Styles &styles, std::string &name);

// Enum map to string and vice versa
using HorizontalAlignToName = boost::bimap<FontProp::HorizontalAlign, std::string_view>;
const HorizontalAlignToName horizontal_align_to_name = 
boost::assign::list_of<HorizontalAlignToName::relation>
    (FontProp::HorizontalAlign::left, "left")
    (FontProp::HorizontalAlign::center, "center")
    (FontProp::HorizontalAlign::right, "right");
using VerticalAlignToName = boost::bimap<FontProp::VerticalAlign, std::string_view>;
const VerticalAlignToName vertical_align_to_name =
boost::assign::list_of<VerticalAlignToName::relation>
    (FontProp::VerticalAlign::top, "top")
    (FontProp::VerticalAlign::center, "middle")
    (FontProp::VerticalAlign::bottom, "bottom");
} // namespace

void StyleManager::init(AppConfig *app_config)
{
    assert(app_config != nullptr);
    m_app_config = app_config;
    m_styles = ::load_styles(*app_config);

    if (m_styles.empty()) {
        // No styles loaded from ini file so use default
        EmbossStyles styles = m_create_default_styles();
        for (EmbossStyle &style : styles) {
            ::make_unique_name(m_styles, style.name);
            m_styles.push_back({style});
        }
    }

    std::optional<size_t> active_index_opt = (app_config != nullptr) ? 
        ::load_style_index(*app_config) : 
        std::optional<size_t>{};

    size_t active_index = 0;
    if (active_index_opt.has_value()) active_index = *active_index_opt;    
    if (active_index >= m_styles.size()) active_index = 0;
    
    // find valid font item
    if (load_style(active_index))
        return; // style is loaded

    // Try to fix that style can't be loaded
    m_styles.erase(m_styles.begin() + active_index);

    load_valid_style();
}

bool StyleManager::store_styles_to_app_config(bool use_modification, bool store_active_index)
{
    assert(m_app_config != nullptr);
    if (m_app_config == nullptr) return false;
    if (use_modification) {
        if (exist_stored_style()) {
            // update stored item
            m_styles[m_style_cache.style_index] = m_style_cache.style;
        } else {
            // add new into stored list
            EmbossStyle &style = m_style_cache.style;
            ::make_unique_name(m_styles, style.name);
            m_style_cache.truncated_name.clear();
            m_style_cache.style_index = m_styles.size();
            m_styles.push_back({style});
        }
        m_style_cache.stored_wx_font = m_style_cache.wx_font;
    }

    if (store_active_index)
    {
        size_t style_index = exist_stored_style() ?
                                 m_style_cache.style_index :
                                 m_last_style_index;
        store_style_index(*m_app_config, style_index);
    }

    store_styles(*m_app_config, m_styles);
    return true;
}

void StyleManager::add_style(const std::string &name) {
    EmbossStyle& style = m_style_cache.style;
    style.name = name;
    ::make_unique_name(m_styles, style.name);
    m_style_cache.style_index = m_styles.size();
    m_style_cache.stored_wx_font = m_style_cache.wx_font;
    m_style_cache.truncated_name.clear();
    m_styles.push_back({style});
}

void StyleManager::swap(size_t i1, size_t i2) {
    if (i1 >= m_styles.size() || 
        i2 >= m_styles.size()) return;
    std::swap(m_styles[i1], m_styles[i2]);
    // fix selected index
    if (!exist_stored_style()) return;
    if (m_style_cache.style_index == i1) {
        m_style_cache.style_index = i2;
    } else if (m_style_cache.style_index == i2) {
        m_style_cache.style_index = i1;
    }
}

void StyleManager::discard_style_changes() {
    if (exist_stored_style()) {
        if (load_style(m_style_cache.style_index))
            return; // correct reload style
    } else {
        if(load_style(m_last_style_index))
            return; // correct load last used style
    }

    // try to save situation by load some font
    load_valid_style();
}

void StyleManager::erase(size_t index) {
    if (index >= m_styles.size()) return;

    // fix selected index
    if (exist_stored_style()) {
        size_t &i = m_style_cache.style_index;
        if (index < i) --i;
        else if (index == i) i = std::numeric_limits<size_t>::max();
    }

    m_styles.erase(m_styles.begin() + index);
}

void StyleManager::rename(const std::string& name) {
    m_style_cache.style.name = name;
    m_style_cache.truncated_name.clear();
    if (exist_stored_style()) { 
        Style &it = m_styles[m_style_cache.style_index];
        it.name = name;
        it.truncated_name.clear();
    }
}

void StyleManager::load_valid_style()
{
    // iterate over all known styles
    while (!m_styles.empty()) {
        if (load_style(0))
            return;
        // can't load so erase it from list
        m_styles.erase(m_styles.begin());
    }

    // no one style is loadable
    // set up default font list
    EmbossStyles def_style = m_create_default_styles();
    for (EmbossStyle &style : def_style) {
        ::make_unique_name(m_styles, style.name);
        m_styles.push_back({std::move(style)});
    }

    // iterate over default styles
    // There have to be option to use build in font
    while (!m_styles.empty()) {
        if (load_style(0))
            return;
        // can't load so erase it from list
        m_styles.erase(m_styles.begin());
    }

    // This OS doesn't have TTF as default font,
    // find some loadable font out of default list
    assert(false);
}

bool StyleManager::load_style(size_t style_index)
{
    if (style_index >= m_styles.size()) return false;
    if (!load_style(m_styles[style_index])) return false;
    m_style_cache.style_index    = style_index;
    m_style_cache.stored_wx_font = m_style_cache.wx_font; // copy
    m_last_style_index           = style_index;
    return true;
}

bool StyleManager::load_style(const Style &style) {
    if (style.type == EmbossStyle::Type::file_path) {
        std::unique_ptr<FontFile> font_ptr =
            create_font_file(style.path.c_str());
        if (font_ptr == nullptr) return false;
        m_style_cache.wx_font = {};
        m_style_cache.font_file = 
            FontFileWithCache(std::move(font_ptr));
        m_style_cache.style          = style; // copy
        m_style_cache.style_index    = std::numeric_limits<size_t>::max();
        m_style_cache.stored_wx_font = {};
        return true;
    }
    if (style.type != WxFontUtils::get_current_type()) return false;
    std::optional<wxFont> wx_font_opt = WxFontUtils::load_wxFont(style.path);
    if (!wx_font_opt.has_value()) return false;
    return load_style(style, *wx_font_opt);
}

bool StyleManager::load_style(const Style &style, const wxFont &font)
{
    m_style_cache.style = style; // copy

    // wx font property has bigger priority to set
    // it must be after copy of the style
    if (!set_wx_font(font)) return false;

    m_style_cache.style_index = std::numeric_limits<size_t>::max();
    m_style_cache.stored_wx_font = {};
    m_style_cache.truncated_name.clear();
    return true;
}

bool StyleManager::is_font_changed() const
{
    const wxFont &wx_font = get_wx_font();
    if (!wx_font.IsOk())
        return false;
    if (!exist_stored_style())
        return false;
    const EmbossStyle *stored_style = get_stored_style();
    if (stored_style == nullptr)
        return false;

    const wxFont &wx_font_stored = get_stored_wx_font();
    if (!wx_font_stored.IsOk())
        return false;

    const FontProp &prop = get_style().prop;
    const FontProp &prop_stored = stored_style->prop;

    // Exist change in face name?
    if(wx_font_stored.GetFaceName() != wx_font.GetFaceName()) return true;

    const std::optional<float> &skew = prop.skew;
    bool is_italic = skew.has_value() || WxFontUtils::is_italic(wx_font);
    const std::optional<float> &skew_stored = prop_stored.skew;
    bool is_stored_italic = skew_stored.has_value() || WxFontUtils::is_italic(wx_font_stored);
    // is italic changed
    if (is_italic != is_stored_italic)
        return true;

    const std::optional<float> &boldness = prop.boldness;
    bool is_bold = boldness.has_value() || WxFontUtils::is_bold(wx_font);
    const std::optional<float> &boldness_stored = prop_stored.boldness;
    bool is_stored_bold = boldness_stored.has_value() || WxFontUtils::is_bold(wx_font_stored);
    // is bold changed
    return is_bold != is_stored_bold;
}

bool StyleManager::is_unique_style_name(const std::string &name) const {
    for (const StyleManager::Style &style : m_styles)
        if (style.name == name)
            return false;
    return true;
}

bool StyleManager::is_active_font() { return m_style_cache.font_file.has_value(); }

const StyleManager::Style *StyleManager::get_stored_style() const
{
    if (m_style_cache.style_index >= m_styles.size()) return nullptr;
    return &m_styles[m_style_cache.style_index];
}

void StyleManager::clear_glyphs_cache()
{
    FontFileWithCache &ff = m_style_cache.font_file;
    if (!ff.has_value()) return;
    ff.cache = std::make_shared<Glyphs>();
}

void StyleManager::clear_imgui_font() { m_style_cache.atlas.Clear(); }

ImFont *StyleManager::get_imgui_font()
{
    if (!is_active_font()) return nullptr;
    
    ImVector<ImFont *> &fonts = m_style_cache.atlas.Fonts;
    if (fonts.empty()) return nullptr;

    // check correct index
    int f_size = fonts.size();
    assert(f_size == 1);
    if (f_size != 1) return nullptr;
    ImFont *font = fonts.front();
    if (font == nullptr) return nullptr;
    return font;
}

const StyleManager::Styles &StyleManager::get_styles() const{ return m_styles; }
void StyleManager::init_trunc_names(float max_width) { 
    for (auto &s : m_styles)
        if (s.truncated_name.empty()) {
            std::string name = s.name;
            ImGuiWrapper::escape_double_hash(name);
            s.truncated_name = ImGuiWrapper::trunc(name, max_width);
        }
}

// for access to worker
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp" 

// for get DPI
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"

void StyleManager::init_style_images(const Vec2i32 &max_size,
                                    const std::string &text)
{
    // check already initialized
    if (m_exist_style_images) return;

    // check is initializing
    if (m_temp_style_images != nullptr) {
        // is initialization finished
        if (!m_temp_style_images->styles.empty()) { 
            assert(m_temp_style_images->images.size() ==
                   m_temp_style_images->styles.size());
            // copy images into styles
            for (StyleManager::StyleImage &image : m_temp_style_images->images){
                size_t index = &image - &m_temp_style_images->images.front();
                StyleImagesData::Item &style = m_temp_style_images->styles[index];

                // find style in font list and copy to it
                for (auto &it : m_styles) {
                    if (it.name != style.text ||
                        !(it.prop == style.prop))
                        continue;
                    it.image = image;
                    break;
                }
            }
            m_temp_style_images = nullptr;
            m_exist_style_images = true;
            return;
        }
        // in process of initialization inside of job
        return;
    }

    // create job for init images
    m_temp_style_images = std::make_shared<StyleImagesData::StyleImages>();
    StyleImagesData::Items styles;
    styles.reserve(m_styles.size());
    for (const Style &style : m_styles) {
        std::optional<wxFont> wx_font_opt = WxFontUtils::load_wxFont(style.path);
        if (!wx_font_opt.has_value()) continue;
        std::unique_ptr<FontFile> font_file =
            WxFontUtils::create_font_file(*wx_font_opt);
        if (font_file == nullptr) continue;
        styles.push_back({
            FontFileWithCache(std::move(font_file)), 
            style.name,
            style.prop
        });
    }

    auto mf = wxGetApp().mainframe;
    // dot per inch for monitor
    int dpi = get_dpi_for_window(mf);
    // pixel per milimeter
    double ppm = dpi / GizmoObjectManipulation::in_to_mm;

    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    StyleImagesData data{std::move(styles), max_size, text, m_temp_style_images, ppm};
    queue_job(worker, std::make_unique<CreateFontStyleImagesJob>(std::move(data)));
}

void StyleManager::free_style_images() {
    if (!m_exist_style_images) return;
    GLuint tex_id = 0;
    for (Style &it : m_styles) {
        if (tex_id == 0 && it.image.has_value())
            tex_id = (GLuint)(intptr_t) it.image->texture_id;
        it.image.reset();
    }
    if (tex_id != 0)
        glsafe(::glDeleteTextures(1, &tex_id));
    m_exist_style_images = false;
}

float StyleManager::min_imgui_font_size = 18.f;
float StyleManager::max_imgui_font_size = 60.f;
float StyleManager::get_imgui_font_size(const FontProp &prop, const FontFile &file, double scale)
{
    const FontFile::Info& info = get_font_info(file, prop);
    // coeficient for convert line height to font size
    float c1 = (info.ascent - info.descent + info.linegap) /
               (float) info.unit_per_em;

    // The point size is defined as 1/72 of the Anglo-Saxon inch (25.4 mm):
    // It is approximately 0.0139 inch or 352.8 um.
    return c1 * std::abs(prop.size_in_mm) / 0.3528f * scale;
}

ImFont *StyleManager::create_imgui_font(const std::string &text, double scale)
{
    // inspiration inside of ImGuiWrapper::init_font
    auto& ff = m_style_cache.font_file;
    if (!ff.has_value()) return nullptr;
    const FontFile &font_file = *ff.font_file;

    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(m_imgui_init_glyph_range);
    if (!text.empty())
        builder.AddText(text.c_str());

    ImVector<ImWchar> &ranges = m_style_cache.ranges;
    ranges.clear();
    builder.BuildRanges(&ranges);
        
    m_style_cache.atlas.Flags |= ImFontAtlasFlags_NoMouseCursors |
                                ImFontAtlasFlags_NoPowerOfTwoHeight;

    const FontProp &font_prop = m_style_cache.style.prop;
    float font_size = get_imgui_font_size(font_prop, font_file, scale);
    if (font_size < min_imgui_font_size)
        font_size = min_imgui_font_size;
    if (font_size > max_imgui_font_size)
        font_size = max_imgui_font_size;

    ImFontConfig font_config;
    // TODO: start using merge mode
    //font_config.MergeMode = true;
    int unit_per_em = get_font_info(font_file, font_prop).unit_per_em;
    float coef = font_size / (double) unit_per_em;
    if (font_prop.char_gap.has_value())
        font_config.GlyphExtraSpacing.x = coef * (*font_prop.char_gap);    
    if (font_prop.line_gap.has_value())
        font_config.GlyphExtraSpacing.y = coef * (*font_prop.line_gap);    

    font_config.FontDataOwnedByAtlas = false;

    const std::vector<unsigned char> &buffer = *font_file.data;
    ImFont * font = m_style_cache.atlas.AddFontFromMemoryTTF(
        (void *) buffer.data(), buffer.size(), font_size, &font_config, m_style_cache.ranges.Data);

    unsigned char *pixels;
    int            width, height;
    m_style_cache.atlas.GetTexDataAsRGBA32(&pixels, &width, &height);

    // Upload texture to graphics system
    GLint last_texture;
    glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture));
    ScopeGuard sg([last_texture]() {
        glsafe(::glBindTexture(GL_TEXTURE_2D, last_texture));
    });

    GLuint font_texture;
    glsafe(::glGenTextures(1, &font_texture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, font_texture));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
    if (OpenGLManager::are_compressed_textures_supported())
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels));
    else
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels));

    // Store our identifier
    m_style_cache.atlas.TexID = (ImTextureID) (intptr_t) font_texture;
    assert(!m_style_cache.atlas.Fonts.empty());
    if (m_style_cache.atlas.Fonts.empty()) return nullptr;
    assert(font == m_style_cache.atlas.Fonts.back());
    if (!font->IsLoaded()) return nullptr;
    assert(font->IsLoaded());
    return font;
}

bool StyleManager::set_wx_font(const wxFont &wx_font) {
    std::unique_ptr<FontFile> font_file = 
        WxFontUtils::create_font_file(wx_font);
    return set_wx_font(wx_font, std::move(font_file));
}

bool StyleManager::set_wx_font(const wxFont &wx_font, std::unique_ptr<FontFile> font_file)
{
    if (font_file == nullptr) return false;
    m_style_cache.wx_font = wx_font; // copy
    m_style_cache.font_file = 
        FontFileWithCache(std::move(font_file));

    EmbossStyle &style = m_style_cache.style;
    style.type = WxFontUtils::get_current_type();
    // update string path
    style.path = WxFontUtils::store_wxFont(wx_font);
    WxFontUtils::update_property(style.prop, wx_font);
    clear_imgui_font();
    return true;
}

#include <libslic3r/AppConfig.hpp>
#include "WxFontUtils.hpp"
#include "fast_float/fast_float.h"

// StylesSerializable
namespace {

using namespace Slic3r;
using namespace Slic3r::GUI;
using Section = std::map<std::string,std::string>;

const std::string APP_CONFIG_FONT_NAME        = "name";
const std::string APP_CONFIG_FONT_DESCRIPTOR  = "descriptor";
const std::string APP_CONFIG_FONT_LINE_HEIGHT = "line_height";
const std::string APP_CONFIG_FONT_DEPTH       = "depth";
const std::string APP_CONFIG_FONT_USE_SURFACE = "use_surface";
const std::string APP_CONFIG_PER_GLYPH        = "per_glyph";
const std::string APP_CONFIG_VERTICAL_ALIGN   = "vertical_align";
const std::string APP_CONFIG_HORIZONTAL_ALIGN = "horizontal_align";
const std::string APP_CONFIG_FONT_BOLDNESS    = "boldness";
const std::string APP_CONFIG_FONT_SKEW        = "skew";
const std::string APP_CONFIG_FONT_DISTANCE    = "distance";
const std::string APP_CONFIG_FONT_ANGLE       = "angle";
const std::string APP_CONFIG_FONT_COLLECTION  = "collection";
const std::string APP_CONFIG_FONT_CHAR_GAP    = "char_gap";
const std::string APP_CONFIG_FONT_LINE_GAP    = "line_gap";

const std::string APP_CONFIG_ACTIVE_FONT = "active_font";

std::string create_section_name(unsigned index)
{
    return AppConfig::SECTION_EMBOSS_STYLE + ':' + std::to_string(index);
}

// check only existence of flag
bool read(const Section &section, const std::string &key, bool &value)
{
    auto item = section.find(key);
    if (item == section.end())
        return false;

    value = true;
    return true;
}

bool read(const Section &section, const std::string &key, Slic3r::FontProp::HorizontalAlign &value) {
    auto item = section.find(key);
    if (item == section.end())
        return false;

    const std::string &data = item->second;
    if (data.empty())
        return false;

    const auto& map = horizontal_align_to_name.right; 
    auto it = map.find(data);
    value = (it != map.end()) ? it->second : Slic3r::FontProp::HorizontalAlign::center;
    return true;
}

bool read(const Section &section, const std::string &key, Slic3r::FontProp::VerticalAlign &value) {
    auto item = section.find(key);
    if (item == section.end())
        return false;

    const std::string &data = item->second;
    if (data.empty())
        return false;

    const auto &map = vertical_align_to_name.right;
    auto it = map.find(data);
    value = (it != map.end()) ? it->second : Slic3r::FontProp::VerticalAlign::center;
    return true;
}

bool read(const Section &section, const std::string &key, float &value)
{
    auto item = section.find(key);
    if (item == section.end())
        return false;
    const std::string &data = item->second;
    if (data.empty())
        return false;
    float value_;
    fast_float::from_chars(data.c_str(), data.c_str() + data.length(), value_);
    // read only non zero value
    if (fabs(value_) <= std::numeric_limits<float>::epsilon())
        return false;

    value = value_;
    return true;
}

bool read(const Section &section, const std::string &key, std::optional<int> &value)
{
    auto item = section.find(key);
    if (item == section.end())
        return false;
    const std::string &data = item->second;
    if (data.empty())
        return false;
    int value_ = std::atoi(data.c_str());
    if (value_ == 0)
        return false;

    value = value_;
    return true;
}

bool read(const Section &section, const std::string &key, std::optional<unsigned int> &value)
{
    auto item = section.find(key);
    if (item == section.end())
        return false;
    const std::string &data = item->second;
    if (data.empty())
        return false;
    int value_ = std::atoi(data.c_str());
    if (value_ <= 0)
        return false;

    value = static_cast<unsigned int>(value_);
    return true;
}

bool read(const Section &section, const std::string &key, std::optional<float> &value)
{
    auto item = section.find(key);
    if (item == section.end())
        return false;
    const std::string &data = item->second;
    if (data.empty())
        return false;
    float value_;
    fast_float::from_chars(data.c_str(), data.c_str() + data.length(), value_);
    // read only non zero value
    if (fabs(value_) <= std::numeric_limits<float>::epsilon())
        return false;

    value = value_;
    return true;
}

std::optional<StyleManager::Style> load_style(const Section &app_cfg_section)
{
    auto path_it = app_cfg_section.find(APP_CONFIG_FONT_DESCRIPTOR);
    if (path_it == app_cfg_section.end())
        return {};
        
    StyleManager::Style s;
    EmbossProjection& ep = s.projection;
    FontProp& fp = s.prop;
    
    s.path = path_it->second;
    s.type = WxFontUtils::get_current_type();
    auto name_it = app_cfg_section.find(APP_CONFIG_FONT_NAME);
    const std::string  default_name = "font_name";
    s.name = (name_it == app_cfg_section.end()) ? default_name : name_it->second;

    read(app_cfg_section, APP_CONFIG_FONT_LINE_HEIGHT, fp.size_in_mm);
    float depth = 1.;
    read(app_cfg_section, APP_CONFIG_FONT_DEPTH, depth);
    ep.depth = depth;
    read(app_cfg_section, APP_CONFIG_FONT_USE_SURFACE, ep.use_surface);
    read(app_cfg_section, APP_CONFIG_PER_GLYPH, fp.per_glyph);
    read(app_cfg_section, APP_CONFIG_HORIZONTAL_ALIGN, fp.align.first);
    read(app_cfg_section, APP_CONFIG_VERTICAL_ALIGN, fp.align.second);
    read(app_cfg_section, APP_CONFIG_FONT_BOLDNESS, fp.boldness);
    read(app_cfg_section, APP_CONFIG_FONT_SKEW, fp.skew);
    read(app_cfg_section, APP_CONFIG_FONT_DISTANCE, s.distance);
    read(app_cfg_section, APP_CONFIG_FONT_ANGLE, s.angle);
    read(app_cfg_section, APP_CONFIG_FONT_COLLECTION, fp.collection_number);
    read(app_cfg_section, APP_CONFIG_FONT_CHAR_GAP, fp.char_gap);
    read(app_cfg_section, APP_CONFIG_FONT_LINE_GAP, fp.line_gap);
    return s;
}

void store_style(AppConfig &cfg, const StyleManager::Style &s, unsigned index)
{
    const EmbossProjection &ep = s.projection;
    Section data;
    data[APP_CONFIG_FONT_NAME]        = s.name;
    data[APP_CONFIG_FONT_DESCRIPTOR]  = s.path;
    const FontProp &fp                = s.prop;
    data[APP_CONFIG_FONT_LINE_HEIGHT] = std::to_string(fp.size_in_mm);
    data[APP_CONFIG_FONT_DEPTH]       = std::to_string(ep.depth);
    if (ep.use_surface)
        data[APP_CONFIG_FONT_USE_SURFACE] = "true";
    if (fp.per_glyph)
        data[APP_CONFIG_PER_GLYPH] = "true";
    if (fp.align.first != FontProp::HorizontalAlign::center)
        data[APP_CONFIG_HORIZONTAL_ALIGN] = horizontal_align_to_name.left.find(fp.align.first)->second;
    if (fp.align.second != FontProp::VerticalAlign::center)
        data[APP_CONFIG_VERTICAL_ALIGN] = vertical_align_to_name.left.find(fp.align.second)->second;
    if (fp.boldness.has_value())
        data[APP_CONFIG_FONT_BOLDNESS] = std::to_string(*fp.boldness);
    if (fp.skew.has_value())
        data[APP_CONFIG_FONT_SKEW] = std::to_string(*fp.skew);
    if (s.distance.has_value())
        data[APP_CONFIG_FONT_DISTANCE] = std::to_string(*s.distance);
    if (s.angle.has_value())
        data[APP_CONFIG_FONT_ANGLE] = std::to_string(*s.angle);
    if (fp.collection_number.has_value())
        data[APP_CONFIG_FONT_COLLECTION] = std::to_string(*fp.collection_number);
    if (fp.char_gap.has_value())
        data[APP_CONFIG_FONT_CHAR_GAP] = std::to_string(*fp.char_gap);
    if (fp.line_gap.has_value())
        data[APP_CONFIG_FONT_LINE_GAP] = std::to_string(*fp.line_gap);
    cfg.set_section(create_section_name(index), std::move(data));
}

void store_style_index(AppConfig &cfg, size_t index)
{
    // store actual font index
    // active font first index is +1 to correspond with section name
    Section data;
    data[APP_CONFIG_ACTIVE_FONT] = std::to_string(index);
    cfg.set_section(AppConfig::SECTION_EMBOSS_STYLE, std::move(data));
}

std::optional<size_t> load_style_index(const AppConfig &cfg)
{
    if (!cfg.has_section(AppConfig::SECTION_EMBOSS_STYLE))
        return {};

    auto section = cfg.get_section(AppConfig::SECTION_EMBOSS_STYLE);
    auto it      = section.find(APP_CONFIG_ACTIVE_FONT);
    if (it == section.end())
        return {};

    size_t active_font = static_cast<size_t>(std::atoi(it->second.c_str()));
    // order in config starts with number 1
    return active_font - 1;
}

::StyleManager::Styles load_styles(const AppConfig &cfg)
{
    StyleManager::Styles result;
    // human readable index inside of config starts from 1 !!
    unsigned    index        = 1;
    std::string section_name = create_section_name(index);
    while (cfg.has_section(section_name)) {
        std::optional<StyleManager::Style> style_opt = load_style(cfg.get_section(section_name));
        if (style_opt.has_value()) {
            make_unique_name(result, style_opt->name);
            result.emplace_back(*style_opt);
        }

        section_name = create_section_name(++index);
    }
    return result;
}

void store_styles(AppConfig &cfg, const StyleManager::Styles &styles)
{
    EmbossStyle::Type current_type = WxFontUtils::get_current_type();
    // store styles
    unsigned index = 1;
    for (const StyleManager::Style &style : styles) {
        // skip file paths + fonts from other OS(loaded from .3mf)
        assert(style.type == current_type);
        if (style.type != current_type)
            continue;
        store_style(cfg, style, index);
        ++index;
    }

    // remove rest of font sections (after deletation)
    std::string section_name = create_section_name(index);
    while (cfg.has_section(section_name)) {
        cfg.clear_section(section_name);
        section_name = create_section_name(index);
        ++index;
    }
}

void make_unique_name(const StyleManager::Styles& styles, std::string &name)
{
    auto is_unique = [&styles](const std::string &name){
        for (const StyleManager::Style &it : styles)
            if (it.name == name) return false;
        return true;
    };

    // Style name can't be empty so default name is set
    if (name.empty()) name = "Text style";

    // When name is already unique, nothing need to be changed
    if (is_unique(name)) return;

    // when there is previous version of style name only find number
    const char *prefix = " (";
    const char  suffix  = ')';
    auto pos = name.find_last_of(prefix);
    if (name.c_str()[name.size() - 1] == suffix && 
        pos != std::string::npos) {
        // short name by ord number
        name = name.substr(0, pos);
    }

    int order = 1; // start with value 2 to represents same font name
    std::string new_name;
    do {
        new_name = name + prefix + std::to_string(++order) + suffix;
    } while (!is_unique(new_name));
    name = new_name;
}

} // namespace
