#ifndef slic3r_IMToolbar_hpp_
#define slic3r_IMToolbar_hpp_

#include <functional>
#include <string>
#include <vector>

#include "GLTexture.hpp"
#include "Event.hpp"
#include <imgui/imgui.h>

#define DEFAULT_TOOLBAR_BUTTON_WIDTH    80.0f
#define DEFAULT_TOOLBAR_BUTTON_HEIGHT   80.0f

namespace Slic3r {
namespace GUI {

class IMToolbarItem
{
public:
    enum SliceState {
        UNSLICED = 0,
        SLICING = 1,
        SLICED = 2,
        SLICE_FAILED = 3,
    };

    bool selected{ false };
    float percent;
    GLTexture image_stats;
    GLTexture image_slicing;
    GLTexture image_idle;
    GLTexture image_failed;
    SliceState slice_state;

    ImTextureID texture_id { 0 };
    std::vector<unsigned char> image_data;
    unsigned int image_width;
    unsigned int image_height;

    bool generate_texture();
    ~IMToolbarItem();
};

class IMToolbar {
private:
    bool m_enabled { false };

public:
    float icon_width;
    float icon_height;
    bool is_display_scrollbar;
    bool show_stats_item{ false };
    bool is_render_finish{false};
    IMToolbar() {
        icon_width = DEFAULT_TOOLBAR_BUTTON_WIDTH;
        icon_height = DEFAULT_TOOLBAR_BUTTON_HEIGHT;
    }

    void del_all_item();
    void del_stats_item();

    IMToolbarItem* m_all_plates_stats_item = nullptr;
    std::vector<IMToolbarItem*> m_items;
    float fontScale;

    bool is_enabled() const { return m_enabled; }
    void set_enabled(bool enable);

    void set_icon_size(float width, float height) {
        icon_width = width;
        icon_height = height;
    }

    int get_items_count() { return m_items.size();  }
};

class IMReturnToolbar {
private:
    bool m_enabled{ false };
    ImTextureID texture_id;
    GLTexture   return_textrue;

public:
    IMReturnToolbar() {}

    bool init();
    bool is_enabled() const { return m_enabled; }
    void set_enabled(bool enable) { m_enabled = enable; }
    ImTextureID get_return_texture_id() { return texture_id; }
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_IMToolbar_hpp_
