#ifndef slic3r_GLToolbar_hpp_
#define slic3r_GLToolbar_hpp_

#include <functional>
#include <string>
#include <vector>

#include "GLTexture.hpp"
#include "Event.hpp"
#include "libslic3r/Point.hpp"

class wxEvtHandler;

namespace Slic3r {
namespace GUI {

class GLCanvas3D;


//BBS: GUI refactor: GLToolbar
wxDECLARE_EVENT(EVT_GLTOOLBAR_OPEN_PROJECT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_SLICE_ALL, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_SLICE_PLATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_PRINT_ALL, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_PRINT_PLATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_EXPORT_GCODE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_SEND_GCODE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_UPLOAD_GCODE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_EXPORT_SLICED_FILE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_EXPORT_ALL_SLICED_FILE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_PRINT_SELECT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_SEND_TO_PRINTER, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_SEND_TO_PRINTER_ALL, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_PRINT_MULTI_MACHINE, SimpleEvent);

wxDECLARE_EVENT(EVT_GLTOOLBAR_SELECT_ALL_PLATES_STATS, SimpleEvent);

wxDECLARE_EVENT(EVT_GLTOOLBAR_ADD, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_DELETE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_DELETE_ALL, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_ADD_PLATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_DEL_PLATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_ORIENT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_ARRANGE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_CUT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_COPY, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_PASTE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_LAYERSEDITING, SimpleEvent);
//BBS: add clone event
wxDECLARE_EVENT(EVT_GLTOOLBAR_CLONE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_MORE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_FEWER, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_SPLIT_OBJECTS, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_SPLIT_VOLUMES, SimpleEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_FILLCOLOR, IntEvent);
wxDECLARE_EVENT(EVT_GLTOOLBAR_SELECT_SLICED_PLATE, wxCommandEvent);

wxDECLARE_EVENT(EVT_GLVIEWTOOLBAR_3D, SimpleEvent);
wxDECLARE_EVENT(EVT_GLVIEWTOOLBAR_PREVIEW, SimpleEvent);
wxDECLARE_EVENT(EVT_GLVIEWTOOLBAR_ASSEMBLE, SimpleEvent);



class GLToolbarItem
{
public:
    typedef std::function<void()> ActionCallback;
    typedef std::function<bool()> VisibilityCallback;
    typedef std::function<bool()> EnablingCallback;
    typedef std::function<void(float, float, float, float)> RenderCallback;

    enum EType : unsigned char
    {
        Action,
        Separator,
        //BBS: GUI refactor: GLToolbar
        ActionWithText,
        ActionWithTextImage,
        SeparatorLine,
        Num_Types
    };

    enum EActionType : unsigned char
    {
        Undefined,
        Left,
        Right,
        Num_Action_Types
    };

    enum EState : unsigned char
    {
        Normal,
        Pressed,
        Disabled,
        Hover,
        HoverPressed,
        HoverDisabled,
        Num_States
    };

    enum EHighlightState : unsigned char
    {
        HighlightedShown,
        HighlightedHidden,
        Num_Rendered_Highlight_States,
        NotHighlighted
    };

    struct Data
    {
        struct Option
        {
            bool toggable;
            ActionCallback action_callback;
            RenderCallback render_callback;

            Option();

            bool can_render() const { return toggable && (render_callback != nullptr); }
        };

        std::string name;
        std::string icon_filename;
        std::string tooltip;
        std::string additional_tooltip;
        //BBS: GUI refactor: GLToolbar
        std::string button_text;
        float extra_size_ratio;
        GLTexture text_texture;
        GLTexture image_texture;
        std::vector<unsigned char> image_data;
        unsigned int image_width;
        unsigned int image_height;

        unsigned int sprite_id;
        // mouse left click
        Option left;
        // mouse right click
        Option right;
        bool visible;
        VisibilityCallback visibility_callback;
        EnablingCallback enabling_callback;

        Data();
        //BBS: GUI refactor: GLToolbar
        Data(const GLToolbarItem::Data& data)
        {
            name = data.name;
            icon_filename = data.icon_filename;
            tooltip = data.tooltip;
            additional_tooltip = data.additional_tooltip;
            button_text = data.button_text;
            extra_size_ratio = data.extra_size_ratio;
            sprite_id = data.sprite_id;
            left = data.left;
            right = data.right;
            visible = data.visible;
            visibility_callback = data.visibility_callback;
            enabling_callback = data.enabling_callback;
            image_data = data.image_data;
            image_width = data.image_width;
            image_height = data.image_height;
        }
    };

    static const ActionCallback Default_Action_Callback;
    static const VisibilityCallback Default_Visibility_Callback;
    static const EnablingCallback Default_Enabling_Callback;
    static const RenderCallback Default_Render_Callback;

private:
    EType m_type;
    EState m_state;
    Data m_data;
    EActionType m_last_action_type;
    EHighlightState m_highlight_state;
public:
    // remember left position for rendering menu
    mutable float render_left_pos;

    GLToolbarItem(EType type, const Data& data);

    EState get_state() const { return m_state; }
    void set_state(EState state) { m_state = state; }

    EHighlightState get_highlight() const { return m_highlight_state; }
    void set_highlight(EHighlightState state) { m_highlight_state = state; }

    const std::string& get_name() const { return m_data.name; }
    const std::string& get_icon_filename() const { return m_data.icon_filename; }
    void set_icon_filename(const std::string& filename) { m_data.icon_filename = filename; }
    const std::string& get_tooltip() const { return m_data.tooltip; }
    const std::string& get_additional_tooltip() const { return m_data.additional_tooltip; }
    void set_additional_tooltip(const std::string& text) { m_data.additional_tooltip = text; }
    void set_tooltip(const std::string& text)            { m_data.tooltip = text; }

    void do_left_action() { m_last_action_type = Left; m_data.left.action_callback(); }
    void do_right_action() { m_last_action_type = Right; m_data.right.action_callback(); }

    bool is_enabled() const { return (m_state != Disabled) && (m_state != HoverDisabled); }
    bool is_disabled() const { return (m_state == Disabled) || (m_state == HoverDisabled); }
    bool is_hovered() const { return (m_state == Hover) || (m_state == HoverPressed) || (m_state == HoverDisabled); }
    bool is_pressed() const { return (m_state == Pressed) || (m_state == HoverPressed); }
    bool is_visible() const { return m_data.visible; }
    bool is_separator() const { return m_type == Separator; }

    bool is_left_toggable() const { return m_data.left.toggable; }
    bool is_right_toggable() const { return m_data.right.toggable; }

    bool has_left_render_callback() const { return m_data.left.render_callback != nullptr; }
    bool has_right_render_callback() const { return m_data.right.render_callback != nullptr; }

    EActionType get_last_action_type() const { return m_last_action_type; }
    void reset_last_action_type() { m_last_action_type = Undefined; }

    // returns true if the state changes
    bool update_visibility();
    // returns true if the state changes
    bool update_enabled_state();

    //BBS: GUI refactor: GLToolbar
    bool is_action() const { return m_type == Action; }
    bool is_action_with_text() const { return m_type == ActionWithText; }
    bool is_action_with_text_image() const { return m_type == ActionWithTextImage; }
    const std::string& get_button_text() const { return m_data.button_text; }
    void set_button_text(const std::string& text) { m_data.button_text = text; }
    float get_extra_size_ratio() const { return m_data.extra_size_ratio; }
    void set_extra_size_ratio(const float ratio) { m_data.extra_size_ratio = ratio; }
    void render_text(float left, float right, float bottom, float top) const;
    int generate_texture(wxFont& font);
    int generate_image_texture();

    void render(unsigned int tex_id, float left, float right, float bottom, float top, unsigned int tex_width, unsigned int tex_height, unsigned int icon_size) const;
    void render_image(unsigned int tex_id, float left, float right, float bottom, float top, unsigned int tex_width, unsigned int tex_height, unsigned int icon_size) const;
private:
    void set_visible(bool visible) { m_data.visible = visible; }

    friend class GLToolbar;
};

struct BackgroundTexture
{
    struct Metadata
    {
        // path of the file containing the background texture
        std::string filename;
        // size of the left edge, in pixels
        unsigned int left;
        // size of the right edge, in pixels
        unsigned int right;
        // size of the top edge, in pixels
        unsigned int top;
        // size of the bottom edge, in pixels
        unsigned int bottom;

        Metadata();
    };

    GLTexture texture;
    Metadata metadata;
};

class GLToolbar
{
public:
    static const float Default_Icons_Size;

    enum EType : unsigned char
    {
        Normal,
        Radio,
        Num_Types
    };

    struct Layout
    {
        enum EType : unsigned char
        {
            Horizontal,
            Vertical,
            Num_Types
        };

        enum EHorizontalOrientation : unsigned char
        {
            HO_Left,
            HO_Center,
            HO_Right,
            Num_Horizontal_Orientations
        };

        enum EVerticalOrientation : unsigned char
        {
            VO_Top,
            VO_Center,
            VO_Bottom,
            Num_Vertical_Orientations
        };

        EType type;
        EHorizontalOrientation horizontal_orientation;
        EVerticalOrientation vertical_orientation;
        float top;
        float left;
        float border;
        float separator_size;
        float gap_size;
        float icons_size;
        float text_size;
        float image_width;
        float image_height;
        float scale;

        float width;
        float height;
        bool dirty;

        Layout();
    };

private:
    typedef std::vector<GLToolbarItem*> ItemsList;

    EType m_type;
    std::string m_name;
    bool m_enabled;
    GLTexture m_icons_texture;
    bool m_icons_texture_dirty;
    mutable GLTexture m_images_texture;
    mutable bool m_images_texture_dirty;
    BackgroundTexture m_background_texture;
    GLTexture m_arrow_texture;
    Layout m_layout;

    ItemsList m_items;

    struct MouseCapture
    {
        bool left;
        bool middle;
        bool right;
        GLCanvas3D* parent;

        MouseCapture() { reset(); }

        bool any() const { return left || middle || right; }
        void reset() { left = middle = right = false; parent = nullptr; }
    };

    MouseCapture m_mouse_capture;
    int m_pressed_toggable_id;

public:
    GLToolbar(EType type, const std::string& name);
    ~GLToolbar();

    bool init(const BackgroundTexture::Metadata& background_texture);

    bool init_arrow(const std::string& filename);

    Layout::EType get_layout_type() const;
    void set_layout_type(Layout::EType type);
    void set_icon_dirty() { m_icons_texture_dirty = true; }
    Layout::EHorizontalOrientation get_horizontal_orientation() const { return m_layout.horizontal_orientation; }
    void set_horizontal_orientation(Layout::EHorizontalOrientation orientation) { m_layout.horizontal_orientation = orientation; }
    Layout::EVerticalOrientation get_vertical_orientation() const { return m_layout.vertical_orientation; }
    void set_vertical_orientation(Layout::EVerticalOrientation orientation) { m_layout.vertical_orientation = orientation; }

    void set_position(float top, float left);
    void set_border(float border);
    void set_separator_size(float size);
    void set_gap_size(float size);
    void set_icons_size(float size);
    void set_text_size(float size);
    void set_scale(float scale);

    bool is_enabled() const { return m_enabled; }
    void set_enabled(bool enable) { m_enabled = enable; }

    //BBS: GUI refactor: GLToolbar
    bool add_item(const GLToolbarItem::Data& data, GLToolbarItem::EType type = GLToolbarItem::Action);
    bool add_separator();
    bool del_all_item();

    float get_icons_size() { return m_layout.icons_size; }
    float get_width();
    float get_height();

    void select_item(const std::string& name);

    bool is_item_pressed(const std::string& name) const;
    bool is_item_disabled(const std::string& name) const;
    bool is_item_visible(const std::string& name) const;

    bool is_any_item_pressed() const;

    unsigned int get_items_count() const { return (unsigned int)m_items.size(); }
    int get_item_id(const std::string& name) const;

    void force_left_action(int item_id, GLCanvas3D& parent) { do_action(GLToolbarItem::Left, item_id, parent, false); }
    void force_right_action(int item_id, GLCanvas3D& parent) { do_action(GLToolbarItem::Right, item_id, parent, false); }

    std::string get_tooltip() const;

    void get_additional_tooltip(int item_id, std::string& text);
    void set_additional_tooltip(int item_id, const std::string& text);
    void set_tooltip(int item_id, const std::string& text);
    int  get_visible_items_cnt() const;

    // returns true if any item changed its state
    bool update_items_state();

    void render(const GLCanvas3D& parent,GLToolbarItem::EType type = GLToolbarItem::Action);
    void render_arrow(const GLCanvas3D& parent, GLToolbarItem* highlighted_item);

    bool on_mouse(wxMouseEvent& evt, GLCanvas3D& parent);
    // get item pointer for highlighter timer
    GLToolbarItem* get_item(const std::string& item_name);

    //BBS: GUI refactor: GLToolbar
    int generate_button_text_textures(wxFont& font);
    int generate_image_textures();
    float get_scaled_icon_size();

private:
    void calc_layout();
    float get_width_horizontal() const;
    float get_width_vertical() const;
    float get_height_horizontal() const;
    float get_height_vertical() const;
    float get_main_size() const;
    void do_action(GLToolbarItem::EActionType type, int item_id, GLCanvas3D& parent, bool check_hover);
    void update_hover_state(const Vec2d& mouse_pos, GLCanvas3D& parent);
    void update_hover_state_horizontal(const Vec2d& mouse_pos, GLCanvas3D& parent);
    void update_hover_state_vertical(const Vec2d& mouse_pos, GLCanvas3D& parent);
    // returns the id of the item under the given mouse position or -1 if none
    int contains_mouse(const Vec2d& mouse_pos, const GLCanvas3D& parent) const;
    int contains_mouse_horizontal(const Vec2d& mouse_pos, const GLCanvas3D& parent) const;
    int contains_mouse_vertical(const Vec2d& mouse_pos, const GLCanvas3D& parent) const;

    void render_background(float left, float top, float right, float bottom, float border_w, float border_h) const;
    void render_horizontal(const GLCanvas3D &parent, GLToolbarItem::EType type);
    void render_vertical(const GLCanvas3D& parent);

    bool generate_icons_texture();

    // returns true if any item changed its state
    bool update_items_visibility();
    // returns true if any item changed its state
    bool update_items_enabled_state();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLToolbar_hpp_
