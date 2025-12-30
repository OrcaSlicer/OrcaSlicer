#include "ImGuiWrapper.hpp"

#include <cstdio>
#include <vector>
#include <cmath>
#include <stdexcept>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/convert.hpp>
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT

#include <wx/string.h>
#include <wx/event.h>
#include <wx/clipbrd.h>
#include <wx/debug.h>

#include <GL/glew.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

#include "libslic3r/libslic3r.h"
#include <libslic3r/ClipperUtils.hpp>
#include "libslic3r/Utils.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Shape/TextShape.hpp"

#include "3DScene.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "Search.hpp"
#include "BitmapCache.hpp"
#include "GUI_App.hpp"

#include "../Utils/MacDarkMode.hpp"
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>
#include "OpenGLManager.hpp"
#include "GUI_App.hpp"

namespace Slic3r {
namespace GUI {

static const std::map<const wchar_t, std::string> font_icons = {
    {ImGui::PrintIconMarker       , "cog"                           },
    {ImGui::PrinterIconMarker     , "printer"                       },
    // remove sla_printer
    //{ImGui::PrinterSlaIconMarker  , "sla_printer"                   },
    {ImGui::FilamentIconMarker    , "spool"                         },
    {ImGui::MaterialIconMarker    , "blank_16"                      },
    {ImGui::MinimalizeButton      , "notification_minimalize"       },
    {ImGui::MinimalizeHoverButton , "notification_minimalize_hover" },
    {ImGui::RightArrowButton      , "notification_right"            },
    {ImGui::RightArrowHoverButton , "notification_right_hover"      },
    //{ImGui::PreferencesButton      , "notification_preferences"      },
    //{ImGui::PreferencesHoverButton , "notification_preferences_hover"},
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    {ImGui::SliderFloatEditBtnIcon, "edit_button"                    },
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    {ImGui::ClipboardBtnIcon      , "copy_menu"                     },
    {ImGui::CircleButtonIcon       , "circle_paint"                  },
    {ImGui::TriangleButtonIcon     , "triangle_paint"                },
    {ImGui::FillButtonIcon         , "fill_paint"                    },
    {ImGui::HeightRangeIcon        , "height_range"                  },
    {ImGui::ConfirmIcon            , "confirm"                       },
    {ImGui::GapFillIcon            , "gap_fill"                      },
    {ImGui::FoldButtonIcon         , "im_fold"                       },
    {ImGui::UnfoldButtonIcon       , "im_unfold"                     },
    {ImGui::gCodeButtonIcon        , "im_code"                       }, //ORCA
    {ImGui::VisibleIcon            , "im_visible"                    }, //ORCA
    {ImGui::HiddenIcon             , "im_hidden"                     }, //ORCA
    {ImGui::SphereButtonIcon       , "toolbar_modifier_sphere"       },
    // dark mode icon
    {ImGui::MinimalizeDarkButton       , "notification_minimalize_dark"       },
    {ImGui::MinimalizeHoverDarkButton  , "notification_minimalize_hover_dark" },
    {ImGui::RightArrowDarkButton       , "notification_right_dark"            },
    {ImGui::RightArrowHoverDarkButton  , "notification_right_hover_dark"      },
    //{ImGui::PreferencesDarkButton      , "notification_preferences_dark"      },
    //{ImGui::PreferencesHoverDarkButton , "notification_preferences_hover_dark"},

    {ImGui::ClipboardBtnDarkIcon       , "copy_menu_dark"                     },
    {ImGui::CircleButtonDarkIcon       , "circle_paint_dark"                  },
    {ImGui::TriangleButtonDarkIcon     , "triangle_paint_dark"                },
    {ImGui::FillButtonDarkIcon         , "fill_paint_dark"                    },
    {ImGui::HeightRangeDarkIcon        , "height_range_dark"                  },
    {ImGui::ConfirmDarkIcon            , "confirm_dark"                       },
    {ImGui::GapFillDarkIcon            , "gap_fill_dark"                      },
    {ImGui::SphereButtonDarkIcon       , "toolbar_modifier_sphere_dark"       },

    {ImGui::TextSearchIcon             , "im_text_search"             },
    {ImGui::TextSearchCloseIcon        , "im_text_search_close"       },

    {ImGui::ExpandBtn                  , "expand_btn"                 },
    {ImGui::CollapseBtn                , "collapse_btn"               },
    {ImGui::RevertBtn                  , "revert_btn"                 },

    {ImGui::CloseBlockNotifButton      , "block_notification_close"           },
    {ImGui::CloseBlockNotifHoverButton , "block_notification_close_hover"     },

    {ImGui::CollapseArrowIcon,            "notification_collapse"             },
    {ImGui::ExpandArrowIcon,              "notification_expand"               },
    {ImGui::OpenArrowIcon,                "notification_arrow_open"           },
};
static const std::map<const wchar_t, std::string> font_icons_large = {
    {ImGui::CloseNotifButton        , "notification_close"              },
    {ImGui::CloseNotifHoverButton   , "notification_close_hover"        },
    //BBS removed
    {ImGui::EjectButton             , "notification_eject_sd"           },
    {ImGui::EjectHoverButton        , "notification_eject_sd_hover"     },
    //{ImGui::WarningMarker           , "notification_warning"            },
    //{ImGui::ErrorMarker             , "notification_error"              },
    {ImGui::CancelButton            , "notification_cancel"             },
    {ImGui::CancelHoverButton       , "notification_cancel_hover"       },

//    {ImGui::SinkingObjectMarker     , "move"                            },
//    {ImGui::CustomSupportsMarker    , "toolbar_support"                    },
//    {ImGui::CustomSeamMarker        , "seam"                            },
//    {ImGui::MmuSegmentationMarker   , "mmu_segmentation"                },
//    {ImGui::VarLayerHeightMarker    , "layers"                          },
    //{ImGui::DocumentationButton     , "notification_documentation"      },
    //{ImGui::DocumentationHoverButton, "notification_documentation_hover"},
    //{ImGui::InfoMarker              , "notification_info"               },
    // dark mode icon
    {ImGui::CloseNotifDarkButton        , "notification_close_dark"              },
    {ImGui::CloseNotifHoverDarkButton   , "notification_close_hover_dark"        },
    //{ImGui::DocumentationDarkButton     , "notification_documentation_dark"      },
    //{ImGui::DocumentationHoverDarkButton, "notification_documentation_hover_dark"},
    {ImGui::BlockNotifErrorIcon,          "block_notification_error"             },
    {ImGui::PrevArrowBtnIcon,             "notification_arrow_left"              },
    {ImGui::NextArrowBtnIcon,             "notification_arrow_right"             },
    {ImGui::CompleteIcon,                 "notification_slicing_complete"        },
    {ImGui::FilamentGreen,                "filament_green"                       },

    {ImGui::PlayButton,                   "notification_play"                    },
    {ImGui::PlayDarkButton,               "notification_play_dark"               },
    {ImGui::PlayHoverButton,              "notification_play_hover"              },
    {ImGui::PlayHoverDarkButton,          "notification_play_hover_dark"         },
    {ImGui::PauseButton,                  "notification_pause"                   },
    {ImGui::PauseDarkButton,              "notification_pause_dark"              },
    {ImGui::PauseHoverButton,             "notification_pause_hover"             },
    {ImGui::PauseHoverDarkButton,         "notification_pause_hover_dark"        },
    {ImGui::OpenButton,                   "notification_open"                    },
    {ImGui::OpenDarkButton,               "notification_open_dark"               },
    {ImGui::OpenHoverButton,              "notification_open_hover"              },
    {ImGui::OpenHoverDarkButton,          "notification_open_hover_dark"         },
};

static const std::map<const wchar_t, std::string> font_icons_extra_large = {
    //BBS do not use notification_clippy
    //{ImGui::ClippyMarker            , "notification_clippy"             },
};

const ImVec4 ImGuiWrapper::COL_GREY_DARK         = { 0.333f, 0.333f, 0.333f, 1.0f };
const ImVec4 ImGuiWrapper::COL_GREY_LIGHT        = { 0.4f, 0.4f, 0.4f, 1.0f };
const ImVec4 ImGuiWrapper::COL_ORANGE_DARK       = { 0.757f, 0.404f, 0.216f, 1.0f };
const ImVec4 ImGuiWrapper::COL_ORANGE_LIGHT      = to_ImVec4(ColorRGBA::ORANGE());
const ImVec4 ImGuiWrapper::COL_WINDOW_BACKGROUND = { 0.1f, 0.1f, 0.1f, 0.8f };
const ImVec4 ImGuiWrapper::COL_BUTTON_BACKGROUND = COL_ORANGE_DARK;
const ImVec4 ImGuiWrapper::COL_BUTTON_HOVERED    = COL_ORANGE_LIGHT;
const ImVec4 ImGuiWrapper::COL_BUTTON_ACTIVE     = COL_BUTTON_HOVERED;

//BBS
const ImVec4 ImGuiWrapper::COL_RED               = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
const ImVec4 ImGuiWrapper::COL_GREEN             = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
const ImVec4 ImGuiWrapper::COL_BLUE              = ImVec4(0.0f, 0.0f, 1.0f, 1.0f);
const ImVec4 ImGuiWrapper::COL_BLUE_LIGHT        = ImVec4(0.122f, 0.557f, 0.918f, 1.0f);
const ImVec4 ImGuiWrapper::COL_GREEN_LIGHT       = { 0.f, 156 / 255.f, 136 / 255.f, 0.25f }; // ORCA used on various places like text selection bg. Replaced with orca color
const ImVec4 ImGuiWrapper::COL_HOVER             = { 0.933f, 0.933f, 0.933f, 1.0f };
const ImVec4 ImGuiWrapper::COL_ACTIVE            = { 0.675f, 0.675f, 0.675f, 1.0f };
const ImVec4 ImGuiWrapper::COL_SEPARATOR         = { 0.93f, 0.93f, 0.93f, 1.0f };
const ImVec4 ImGuiWrapper::COL_SEPARATOR_DARK    = { 0.24f, 0.24f, 0.27f, 1.0f };
const ImVec4 ImGuiWrapper::COL_TITLE_BG          = { 0.745f, 0.745f, 0.745f, 1.0f };
const ImVec4 ImGuiWrapper::COL_WINDOW_BG         = { 1.000f, 1.000f, 1.000f, 1.0f };
const ImVec4 ImGuiWrapper::COL_WINDOW_BG_DARK    = { 45 / 255.f, 45 / 255.f, 49 / 255.f, 1.f };
const ImVec4 ImGuiWrapper::COL_TOOLBAR_BG        = { 250 / 255.f, 250 / 255.f, 250 / 255.f, 1.f }; // ORCA color matches with toolbar_background.png
const ImVec4 ImGuiWrapper::COL_TOOLBAR_BG_DARK   = { 57  / 255.f, 60  / 255.f, 66  / 255.f, 1.f }; // ORCA color matches with toolbar_background_dark.png
const ImVec4 ImGuiWrapper::COL_ORCA              = to_ImVec4(ColorRGBA::ORCA());
const ImVec4 ImGuiWrapper::COL_MODIFIED          = { 253.f / 255.f, 111.f / 255.f, 40.f / 255.f, 1}; // ORCA same color with m_color_label_modified

int ImGuiWrapper::TOOLBAR_WINDOW_FLAGS = ImGuiWindowFlags_AlwaysAutoResize
                                 | ImGuiWindowFlags_NoMove
                                 | ImGuiWindowFlags_NoResize
                                 | ImGuiWindowFlags_NoCollapse
                                 | ImGuiWindowFlags_NoTitleBar;


bool get_data_from_svg(const std::string &filename, unsigned int max_size_px, ThumbnailData &thumbnail_data)
{
    bool compression_enabled = false;

    NSVGimage *image = nsvgParseFromFile(filename.c_str(), "px", 96.0f);
    if (image == nullptr) { return false; }

    float scale = (float) max_size_px / std::max(image->width, image->height);

    thumbnail_data.width  = (int) (scale * image->width);
    thumbnail_data.height = (int) (scale * image->height);

    int n_pixels = thumbnail_data.width * thumbnail_data.height;

    if (n_pixels <= 0) {
        nsvgDelete(image);
        return false;
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (rast == nullptr) {
        nsvgDelete(image);
        return false;
    }

    // creates the temporary buffer only once, with max size, and reuse it for all the levels, if generating mipmaps
    std::vector<unsigned char> data(n_pixels * 4, 0);
    thumbnail_data.pixels = data;
    nsvgRasterize(rast, image, 0, 0, scale, thumbnail_data.pixels.data(), thumbnail_data.width, thumbnail_data.height, thumbnail_data.width * 4);

    // we manually generate mipmaps because glGenerateMipmap() function is not reliable on all graphics cards
    int   lod_w = thumbnail_data.width;
    int   lod_h = thumbnail_data.height;
    GLint level = 0;
    while (lod_w > 1 || lod_h > 1) {
        ++level;

        lod_w = std::max(lod_w / 2, 1);
        lod_h = std::max(lod_h / 2, 1);
        scale /= 2.0f;

        data.resize(lod_w * lod_h * 4);

        nsvgRasterize(rast, image, 0, 0, scale, data.data(), lod_w, lod_h, lod_w * 4);
    }

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    return true;
}


bool slider_behavior(ImGuiID id, const ImRect& region, const ImS32 v_min, const ImS32 v_max, ImS32* out_value, ImRect* out_handle, ImGuiSliderFlags flags/* = 0*/, const int fixed_value/* = -1*/, const ImVec4& fixed_rect/* = ImRect()*/)
{
    ImGuiContext& context = *GImGui;
    ImGuiIO& io = ImGui::GetIO();

    const ImGuiAxis axis = (flags & ImGuiSliderFlags_Vertical) ? ImGuiAxis_Y : ImGuiAxis_X;

    const ImVec2 handle_sz = out_handle->GetSize();
    ImS32 v_range = (v_min < v_max ? v_max - v_min : v_min - v_max);
    const float region_usable_sz = (region.Max[axis] - region.Min[axis]);
    const float region_usable_pos_min = region.Min[axis];
    const float region_usable_pos_max = region.Max[axis];

    // Process interacting with the slider
    ImS32 v_new = *out_value;
    bool value_changed = false;

    // drag behavior
    if (context.ActiveId == id)
    {
        float mouse_pos_ratio = 0.0f;
        if (context.ActiveIdSource == ImGuiInputSource_Mouse)
        {
            if (context.IO.MouseReleased[0])
            {
                ImGui::ClearActiveID();
            }
            if (context.IO.MouseDown[0])
            {
                const float mouse_abs_pos = context.IO.MousePos[axis];
                mouse_pos_ratio = (region_usable_sz > 0.0f) ? ImClamp((mouse_abs_pos - region_usable_pos_min) / region_usable_sz, 0.0f, 1.0f) : 0.0f;
                if (axis == ImGuiAxis_Y)
                    mouse_pos_ratio = 1.0f - mouse_pos_ratio;
                v_new = v_min + (ImS32)(v_range * mouse_pos_ratio + 0.5f);
            }
        }
    }
    // click in fixed_rect behavior
    if (ImGui::ItemHoverable(fixed_rect, id) && context.IO.MouseReleased[0])
    {
        v_new = fixed_value;
    }

	// apply result, output value
	if (*out_value != v_new)
	{
		*out_value = v_new;
		value_changed = true;
	}

    // Output handle position so it can be displayed by the caller
    const ImS32 v_clamped = (v_min < v_max) ? ImClamp(*out_value, v_min, v_max) : ImClamp(*out_value, v_max, v_min);
    float handle_pos_ratio = v_range != 0 ? ((float)(v_clamped - v_min) / (float)v_range) : 0.0f;
    handle_pos_ratio = axis == ImGuiAxis_Y ? 1.0f - handle_pos_ratio : handle_pos_ratio;
    const float handle_pos = region_usable_pos_min + (region_usable_pos_max - region_usable_pos_min) * handle_pos_ratio;

    ImVec2 new_handle_center = axis == ImGuiAxis_Y ? ImVec2(out_handle->GetCenter().x, handle_pos) : ImVec2(handle_pos, out_handle->GetCenter().y);
    *out_handle = ImRect(new_handle_center - handle_sz * 0.5f, new_handle_center + handle_sz * 0.5f);

    return value_changed;
}

bool button_with_pos(ImTextureID user_texture_id, const ImVec2 &size, const ImVec2 &pos, const ImVec2 &uv0, const ImVec2 &uv1, int frame_padding, const ImVec4 &bg_col, const ImVec4 &tint_col, const ImVec2 &margin)
{

    ImGuiContext &g      = *GImGui;
    ImGuiWindow * window = g.CurrentWindow;
    if (window->SkipItems) return false;

    // Default to using texture ID as ID. User can still push string/integer prefixes.
    ImGui::PushID((void *) (intptr_t) user_texture_id);
    const ImGuiID id = window->GetID("#image");
    ImGui::PopID();

    const ImVec2 padding = (frame_padding >= 0) ? ImVec2((float) frame_padding, (float) frame_padding) : g.Style.FramePadding;

    const ImRect bb(pos, pos + size + padding * 2 + margin * 2);

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

    // Render
    const ImU32 col = ImGui::GetColorU32((held && hovered) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    ImGui::RenderNavHighlight(bb, id);

    const float border_size = g.Style.FrameBorderSize;
    if (border_size > 0.0f) {
        window->DrawList->AddRect(bb.Min + ImVec2(1, 1), bb.Max + ImVec2(1, 1), col, g.Style.FrameRounding, 0, border_size);
        window->DrawList->AddRect(bb.Min, bb.Max, col, g.Style.FrameRounding, 0, border_size);
    }

    if (bg_col.w > 0.0f) window->DrawList->AddRectFilled(bb.Min + padding, bb.Max - padding, ImGui::GetColorU32(bg_col));
    window->DrawList->AddImage(user_texture_id, bb.Min + padding + margin, bb.Max - padding - margin, uv0, uv1, ImGui::GetColorU32(tint_col));

    return pressed;
}

ImGuiWrapper::ImGuiWrapper()
{
    ImGui::CreateContext();

    init_input();
    init_style();

    ImGui::GetIO().IniFilename = nullptr;
}

ImGuiWrapper::~ImGuiWrapper()
{
    //destroy_fonts_texture();
    destroy_font();
    ImGui::DestroyContext();
}

void ImGuiWrapper::set_language(const std::string &language)
{
    if (m_new_frame_open) {
        // ImGUI internally locks the font between NewFrame() and EndFrame()
        // NewFrame() might've been called here because of input from the 3D scene;
        // call EndFrame()
        ImGui::EndFrame();
        m_new_frame_open = false;
    }

    const ImWchar *ranges = nullptr;
    size_t idx = language.find('_');
    std::string lang = (idx == std::string::npos) ? language : language.substr(0, idx);
    static const ImWchar ranges_latin2[] =
    {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x0100, 0x017F, // Latin Extended-A
        0,
    };
    static const ImWchar ranges_turkish[] = {
        0x0020, 0x01FF, // Basic Latin + Latin Supplement
        0x0100, 0x017F, // Latin Extended-A
        0x0180, 0x01FF, // Turkish
        0,
    };
    static const ImWchar ranges_vietnamese[] =
    {
        0x0020, 0x00FF, // Basic Latin
        0x0102, 0x0103,
        0x0110, 0x0111,
        0x0128, 0x0129,
        0x0168, 0x0169,
        0x01A0, 0x01A1,
        0x01AF, 0x01B0,
        0x1EA0, 0x1EF9,
        0,
    };
    m_font_cjk = false;
    m_is_korean = false;
    if (lang == "cs" || lang == "pl") {
        ranges = ranges_latin2;
    } else if (lang == "ru" || lang == "uk") {
        ranges = ImGui::GetIO().Fonts->GetGlyphRangesCyrillic(); // Default + about 400 Cyrillic characters
    } else if (lang == "tr") {
        ranges = ranges_turkish;
    } else if (lang == "vi") {
        ranges = ranges_vietnamese;
    } else if (lang == "ja") {
        ranges = ImGui::GetIO().Fonts->GetGlyphRangesJapanese(); // Default + Hiragana, Katakana, Half-Width, Selection of 1946 Ideographs
        m_font_cjk = true;
    } else if (lang == "ko") {
        ranges = ImGui::GetIO().Fonts->GetGlyphRangesKorean(); // Default + Korean characters
        m_font_cjk = true;
        m_is_korean = true;
    } else if (lang == "zh") {
        ranges = (language == "zh_TW") ?
            // Traditional Chinese
            // Default + Half-Width + Japanese Hiragana/Katakana + full set of about 21000 CJK Unified Ideographs
            ImGui::GetIO().Fonts->GetGlyphRangesChineseFull() :
            // Simplified Chinese
            // Default + Half-Width + Japanese Hiragana/Katakana + set of 2500 CJK Unified Ideographs for common simplified Chinese
            ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon();
        m_font_cjk = true;
    } else if (lang == "th") {
        ranges = ImGui::GetIO().Fonts->GetGlyphRangesThai(); // Default + Thai characters
    }
    else if (lang == "en") {
        ranges = ImGui::GetIO().Fonts->GetGlyphRangesEnglish(); // Basic Latin
    }
    else{
        ranges = ImGui::GetIO().Fonts->GetGlyphRangesOthers();
    }

    if (ranges != m_glyph_ranges) {
        m_glyph_ranges = ranges;
        //destroy_fonts_texture();
        destroy_font();
    }

    m_glyph_basic_ranges = ImGui::GetIO().Fonts->GetGlyphRangesBasic();
}

void ImGuiWrapper::set_display_size(float w, float h)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w, h);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
}

void ImGuiWrapper::set_scaling(float font_size, float scale_style, float scale_both)
{
    font_size *= scale_both;
    scale_style *= scale_both;

    if (m_font_size == font_size && m_style_scaling == scale_style) {
        return;
    }

    m_font_size = font_size;

    ImGui::GetStyle().ScaleAllSizes(scale_style / m_style_scaling);
    m_style_scaling = scale_style;

    //destroy_fonts_texture();
    destroy_font();
}

bool ImGuiWrapper::update_mouse_data(wxMouseEvent& evt)
{
    if (! display_initialized()) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = ImVec2((float)evt.GetX(), (float)evt.GetY());
    io.MouseDown[0] = evt.LeftIsDown();
    io.MouseDown[1] = evt.RightIsDown();
    io.MouseDown[2] = evt.MiddleIsDown();
    float wheel_delta = static_cast<float>(evt.GetWheelDelta());
    if (wheel_delta != 0.0f) {
        io.MouseWheel = evt.GetWheelRotation() > 0 ? 1.f : -1.f;
    }
    unsigned buttons = (evt.LeftIsDown() ? 1 : 0) | (evt.RightIsDown() ? 2 : 0) | (evt.MiddleIsDown() ? 4 : 0);
    m_mouse_buttons = buttons;

    if (want_mouse())
        new_frame();
    return want_mouse();
}

bool ImGuiWrapper::update_key_data(wxKeyEvent &evt)
{
    if (! display_initialized()) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();

    if (evt.GetEventType() == wxEVT_CHAR) {
        // Char event
        const auto key = evt.GetUnicodeKey();
        if (key != 0) {
            io.AddInputCharacter(key);
        }
    } else {
        // Key up/down event
        int key = evt.GetKeyCode();
        wxCHECK_MSG(key >= 0 && key < IM_ARRAYSIZE(io.KeysDown), false, "Received invalid key code");

        io.KeysDown[key] = evt.GetEventType() == wxEVT_KEY_DOWN;
        io.KeyShift = evt.ShiftDown();
        io.KeyCtrl = evt.ControlDown();
        io.KeyAlt = evt.AltDown();
        io.KeySuper = evt.MetaDown();
    }
    bool ret = want_keyboard() || want_text_input();
    if (ret)
        new_frame();
    return ret;
}

void ImGuiWrapper::new_frame()
{
    if (m_new_frame_open) {
        return;
    }

    if (m_font_texture == 0) {
        init_font(true);
    }

    ImGuiIO& io = ImGui::GetIO();

    ImGui::NewFrame();
    m_new_frame_open = true;

    // BBL: we should render the new frame first, than reset keys' status
    // BBL: copy & paste form prusa github repo (https://github.com/prusa3d/PrusaSlicer/blob/master/src/slic3r/GUI/ImGuiWrapper.cpp#L375C5-L402C6)
    // synchronize key states
    // when the application loses the focus it may happen that the key up event is not processed

    // synchronize modifier keys
    constexpr std::array<std::pair<ImGuiKeyModFlags_, wxKeyCode>, 3> imgui_mod_keys{
        std::make_pair(ImGuiKeyModFlags_Ctrl, WXK_CONTROL),
        std::make_pair(ImGuiKeyModFlags_Shift, WXK_SHIFT),
        std::make_pair(ImGuiKeyModFlags_Alt, WXK_ALT) };
    for (const std::pair<ImGuiKeyModFlags_, wxKeyCode>& key : imgui_mod_keys) {
        if ((io.KeyMods & key.first) != 0 && !wxGetKeyState(key.second))
            io.KeyMods &= ~key.first;
    }

    // Not sure if it is neccessary
    // values from 33 to 126 are reserved for the standard ASCII characters
    for (size_t i = 33; i <= 126; ++i) {
        wxKeyCode keycode = static_cast<wxKeyCode>(i);
        if (io.KeysDown[i] && keycode != WXK_NONE && !wxGetKeyState(keycode))
            io.KeysDown[i] = false;
    }

    // special keys: delete, backspace, ...
    for (int key : io.KeyMap) {
        wxKeyCode keycode = static_cast<wxKeyCode>(key);
        if (io.KeysDown[key] && keycode != WXK_NONE && !wxGetKeyState(keycode))
            io.KeysDown[key] = false;
    }

    // BBL: end copy & paste
}

void ImGuiWrapper::render()
{
    ImGui::Render();
    render_draw_data(ImGui::GetDrawData());
    m_new_frame_open = false;
}

ImVec2 ImGuiWrapper::calc_text_size(std::string_view text,
                                    bool  hide_text_after_double_hash,
                                    float wrap_width)
{
    return ImGui::CalcTextSize(text.data(), text.data() + text.length(),
                               hide_text_after_double_hash, wrap_width);
}

ImVec2 ImGuiWrapper::calc_text_size(const std::string& text,
                                    bool  hide_text_after_double_hash,
                                    float wrap_width)
{
    return ImGui::CalcTextSize(text.c_str(), NULL, hide_text_after_double_hash, wrap_width);
}

ImVec2 ImGuiWrapper::calc_text_size(const wxString &text,
                                    bool  hide_text_after_double_hash,
                                    float wrap_width)
{
    auto text_utf8 = into_u8(text);
    ImVec2 size = ImGui::CalcTextSize(text_utf8.c_str(), NULL, hide_text_after_double_hash, wrap_width);

/*#ifdef __linux__
    size.x *= m_style_scaling;
    size.y *= m_style_scaling;
#endif*/

    return size;
}

float ImGuiWrapper::find_widest_text(std::vector<wxString> &text_list)
{
    float width = .0f;
    for(const wxString &text : text_list) {
        width = std::max(width, this->calc_text_size(text).x);
    }
    return width;
}

ImVec2 ImGuiWrapper::calc_button_size(const wxString &text, const ImVec2 &button_size) const
{
    const ImVec2        text_size = this->calc_text_size(text);
    const ImGuiContext &g         = *GImGui;
    const ImGuiStyle   &style     = g.Style;

    return ImGui::CalcItemSize(button_size, text_size.x + style.FramePadding.x * 2.0f, text_size.y + style.FramePadding.y * 2.0f);
}

ImVec2 ImGuiWrapper::get_item_spacing() const
{
    const ImGuiContext &g     = *GImGui;
    const ImGuiStyle   &style = g.Style;
    return style.ItemSpacing;
}

float ImGuiWrapper::get_slider_float_height() const
{
    const ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    return g.FontSize + style.FramePadding.y * 2.0f + style.ItemSpacing.y;
}

void ImGuiWrapper::set_next_window_pos(float x, float y, int flag, float pivot_x, float pivot_y)
{
    ImGui::SetNextWindowPos(ImVec2(x, y), (ImGuiCond)flag, ImVec2(pivot_x, pivot_y));
    ImGui::SetNextWindowSize(ImVec2(0.0, 0.0));
}

void ImGuiWrapper::set_next_window_bg_alpha(float alpha)
{
    ImGui::SetNextWindowBgAlpha(alpha);
}

void ImGuiWrapper::set_next_window_size(float x, float y, ImGuiCond cond)
{
    ImGui::SetNextWindowSize(ImVec2(x, y), cond);
}

/* BBL style widgets */
bool ImGuiWrapper::bbl_combo_with_filter(const char* label, const std::string& preview_value, const std::vector<std::string>& all_items, std::vector<int>* filtered_items_idx, bool* is_filtered, float item_height)
{
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    static char pattern_buffer[256] = { 0 };
    auto simple_match = [](const char* pattern, const char* str) {
        wxString sub_str = wxString::FromUTF8(pattern).Lower();
        wxString main_str = wxString::FromUTF8(str).Lower();
        return main_str.Find(sub_str);
    };

    bool is_filtering = false;
    bool is_new_open = false;

    float sz = ImGui::GetFrameHeight();
    ImVec2 arrow_size(sz, sz);
    ImVec2 CursorPos = window->DC.CursorPos;
    const ImRect arrow_bb(CursorPos, CursorPos + arrow_size);

    float ButtonTextAlignX = g.Style.ButtonTextAlign.x;
    g.Style.ButtonTextAlign.x = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { sz, style.FramePadding.y});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    if (button(preview_value + label, ImGui::CalcItemWidth(), 0))
    {
        ImGui::OpenPopup(label);
        is_new_open = true;
    }
    g.Style.ButtonTextAlign.x = ButtonTextAlignX;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::BBLRenderArrow(window->DrawList, arrow_bb.Min + ImVec2(ImMax(0.0f, (arrow_size.x - g.FontSize) * 0.5f), ImMax(0.0f, (arrow_size.y - g.FontSize) * 0.5f)), ImGui::GetColorU32(ImGuiCol_Text), ImGuiDir_Down);

    if (is_new_open)
        memset(pattern_buffer, 0, IM_ARRAYSIZE(pattern_buffer));

    float item_rect_width = ImGui::GetItemRectSize().x;
    float item_rect_height = item_height ? item_height : ImGui::GetItemRectSize().y;
    ImGui::SetNextWindowPos({ CursorPos.x, ImGui::GetItemRectMax().y + 4 * m_style_scaling });
    ImGui::SetNextWindowSize({ item_rect_width, 0 });
    if (ImGui::BeginPopup(label))
    {
        ImGuiWindow* popup_window = ImGui::GetCurrentWindow();

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * m_style_scaling, item_rect_height - g.FontSize) * 0.5f);
        wchar_t ICON_SEARCH = *pattern_buffer != '\0' ? ImGui::TextSearchCloseIcon : ImGui::TextSearchIcon;
        const ImVec2 label_size = ImGui::CalcTextSize(into_u8(ICON_SEARCH).c_str(), nullptr, true);
        const ImVec2 search_icon_pos(ImGui::GetItemRectMax().x - label_size.x, popup_window->DC.CursorPos.y + style.FramePadding.y);
        ImGui::RenderText(search_icon_pos, into_u8(ICON_SEARCH).c_str());

        auto temp = popup_window->DC.CursorPos;
        popup_window->DC.CursorPos = search_icon_pos;
        ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_Border, { 0, 0, 0, 0 });
        if (button("##invisible_clear_button", label_size.x, label_size.y))
        {
            if (*pattern_buffer != '\0')
                memset(pattern_buffer, 0, IM_ARRAYSIZE(pattern_buffer));
        }
        ImGui::PopStyleColor(5);
        popup_window->DC.CursorPos = temp;


        ImGui::PushItemWidth(item_rect_width);
        if (is_new_open)
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##bbl_combo_with_filter_inputText", pattern_buffer, sizeof(pattern_buffer));
        ImGui::PopItemWidth();
        ImGui::PopStyleVar();

        if (*pattern_buffer != '\0')
            is_filtering = true;

        if (is_filtering)
        {
            std::vector<std::pair<int, int> > filtered_items_with_priority;// std::pair<index, priority>
            for (int i = 0; i < all_items.size(); i++)
            {
                int priority = simple_match(pattern_buffer, all_items[i].c_str());
                if (priority != wxNOT_FOUND)
                    filtered_items_with_priority.push_back({ i, priority });
            }
            std::sort(filtered_items_with_priority.begin(), filtered_items_with_priority.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {return (b.second > a.second); });
            for (auto item : filtered_items_with_priority)
            {
                filtered_items_idx->push_back(item.first);
            }
        }

        *is_filtered = is_filtering;

        popup_window->DC.CursorPos.y -= 1 * m_style_scaling;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f) * m_style_scaling);
        if (ImGui::BeginListBox("##bbl_combo_with_filter_listBox", { item_rect_width, item_rect_height * 7.75f})) {
            ImGui::PopStyleVar(2);
            return true;
        }
        else
        {
            ImGui::PopStyleVar(2);
            ImGui::EndPopup();
            return false;
        }
    }
    else
        return false;
}

bool ImGuiWrapper::bbl_input_double(const wxString& label, const double& value, const std::string& format)
{
    //return ImGui::InputDouble(label.c_str(), const_cast<double *>(&value), 0.0f, 0.0f, format.c_str(), ImGuiInputTextFlags_CharsDecimal);
    return ImGui::InputDouble(label.c_str(), const_cast<double *>(&value), 0.0f, 0.0f, format.c_str(), ImGuiInputTextFlags_CharsDecimal);
}

bool ImGuiWrapper::bbl_slider_float_style(const std::string &label, float *v, float v_min, float v_max, const char *format, float power, bool clamp, const wxString &tooltip)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.81f, 0.81f, 0.81f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));

    bool ret = bbl_slider_float(label, v, v_min,v_max, format, power, clamp,tooltip);

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(1);

    return ret;
}

bool ImGuiWrapper::bbl_slider_float(const std::string& label, float* v, float v_min, float v_max, const char* format, float power, bool clamp, const wxString& tooltip)
{

    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    // let the label string start with "##" to hide the automatic label from ImGui::SliderFloat()
    bool label_visible = !boost::algorithm::istarts_with(label, "##");
    std::string str_label = label_visible ? std::string("##") + std::string(label) : std::string(label);

    // removes 2nd evenience of "##", if present
    std::string::size_type pos = str_label.find("##", 2);
    if (pos != std::string::npos)
        str_label = str_label.substr(0, pos) + str_label.substr(pos + 2);

    bool ret = ImGui::BBLSliderFloat(str_label.c_str(), v, v_min, v_max, format, power);

    m_last_slider_status.hovered = ImGui::IsItemHovered();
    m_last_slider_status.edited = ImGui::IsItemEdited();
    m_last_slider_status.clicked = ImGui::IsItemClicked();
    m_last_slider_status.deactivated_after_edit = ImGui::IsItemDeactivatedAfterEdit();

    if (!tooltip.empty() && ImGui::IsItemHovered())
        this->tooltip(into_u8(tooltip).c_str(), max_tooltip_width);

    if (clamp)
        *v = std::clamp(*v, v_min, v_max);

    const ImGuiStyle& style = ImGui::GetStyle();

    if (label_visible) {
        // if the label is visible, hide the part of it that should be hidden
        std::string out_label = std::string(label);
        std::string::size_type pos = out_label.find("##");
        if (pos != std::string::npos)
            out_label = out_label.substr(0, pos);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 1, style.ItemSpacing.y });
        ImGui::SameLine();
        this->text(out_label.c_str());
        ImGui::PopStyleVar();
    }


    return ret;
}

bool ImGuiWrapper::begin(const std::string &name, int flags)
{
    return ImGui::Begin(name.c_str(), nullptr, (ImGuiWindowFlags)flags);
}

bool ImGuiWrapper::begin(const wxString &name, int flags)
{
    return begin(into_u8(name), flags);
}

bool ImGuiWrapper::begin(const std::string& name, bool* close, int flags)
{
    return ImGui::Begin(name.c_str(), close, (ImGuiWindowFlags)flags);
}

bool ImGuiWrapper::begin(const wxString& name, bool* close, int flags)
{
    return begin(into_u8(name), close, flags);
}

void ImGuiWrapper::end()
{
    ImGui::End();
}

bool ImGuiWrapper::button(const wxString &label, const wxString& tooltip)
{
    auto label_utf8 = into_u8(label);
    const bool ret = ImGui::Button(label_utf8.c_str());

    if (!tooltip.IsEmpty() && ImGui::IsItemHovered()) {
        auto tooltip_utf8 = into_u8(tooltip);
        ImGui::SetTooltip(tooltip_utf8.c_str(), nullptr);
    }

    return ret;
}

bool ImGuiWrapper::bbl_button(const wxString &label, const wxString& tooltip)
{
    auto label_utf8 = into_u8(label);
    const bool ret = ImGui::BBLButton(label_utf8.c_str());

    if (!tooltip.IsEmpty() && ImGui::IsItemHovered()) {
        auto tooltip_utf8 = into_u8(tooltip);
        ImGui::SetTooltip(tooltip_utf8.c_str(), nullptr);
    }

    return ret;
}

bool ImGuiWrapper::button(const wxString& label, float width, float height)
{
    auto label_utf8 = into_u8(label);
    return ImGui::Button(label_utf8.c_str(), ImVec2(width, height));
}

bool ImGuiWrapper::button(const wxString& label, const ImVec2 &size, bool enable)
{
    disabled_begin(!enable);

    auto label_utf8 = into_u8(label);
    bool res = ImGui::Button(label_utf8.c_str(), size);

    disabled_end();
    return (enable) ? res : false;
}

bool ImGuiWrapper::radio_button(const wxString &label, bool active)
{
    auto label_utf8 = into_u8(label);
    return ImGui::RadioButton(label_utf8.c_str(), active);
}

ImVec4 ImGuiWrapper::to_ImVec4(const ColorRGB &color) {
    return {color.r(), color.g(), color.b(), 1.0};
}

bool ImGuiWrapper::input_double(const std::string &label, const double &value, const std::string &format)
{
    return ImGui::InputDouble(label.c_str(), const_cast<double*>(&value), 0.0f, 0.0f, format.c_str(), ImGuiInputTextFlags_CharsDecimal);
}

bool ImGuiWrapper::input_double(const wxString &label, const double &value, const std::string &format)
{
    auto label_utf8 = into_u8(label);
    return input_double(label_utf8, value, format);
}

bool ImGuiWrapper::input_vec3(const std::string &label, const Vec3d &value, float width, const std::string &format)
{
    bool value_changed = false;

    ImGui::BeginGroup();

    for (int i = 0; i < 3; ++i)
    {
        std::string item_label = (i == 0) ? "X" : ((i == 1) ? "Y" : "Z");
        ImGui::PushID(i);
        ImGui::PushItemWidth(width);
        value_changed |= ImGui::InputDouble(item_label.c_str(), const_cast<double*>(&value(i)), 0.0f, 0.0f, format.c_str());
        ImGui::PopID();
    }
    ImGui::EndGroup();

    return value_changed;
}

bool ImGuiWrapper::checkbox(const wxString &label, bool &value)
{
    auto label_utf8 = into_u8(label);
    return ImGui::Checkbox(label_utf8.c_str(), &value);
}

bool ImGuiWrapper::bbl_checkbox(const wxString &label, bool &value)
{
    bool result;
    bool b_value = value;
    if (b_value) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
    }
    auto label_utf8 = into_u8(label);
    result          = ImGui::BBLCheckbox(label_utf8.c_str(), &value);

    if (b_value) { ImGui::PopStyleColor(3);}
    return result;
}

bool ImGuiWrapper::bbl_radio_button(const char *label, bool active)
{
    bool result;
    bool b_value = active;
    if (b_value) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
    }
    result = ImGui::BBLRadioButton(label,active);
    if (b_value) { ImGui::PopStyleColor(3); }
    return result;
}

bool ImGuiWrapper::bbl_sliderin(const char *label, int *v, int v_min, int v_max, const char *format, ImGuiSliderFlags flags)
{
    return ImGui::BBLSliderScalarIn(label, ImGuiDataType_S32, v, &v_min, &v_max, format, flags);
}

void ImGuiWrapper::text(const char *label)
{
    ImGui::Text("%s", label);
}

void ImGuiWrapper::text(const std::string &label)
{
    ImGuiWrapper::text(label.c_str());
}

void ImGuiWrapper::text(const wxString &label)
{
    auto label_utf8 = into_u8(label);
    ImGuiWrapper::text(label_utf8.c_str());
}

void ImGuiWrapper::warning_text(const char *label)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::to_ImVec4(ColorRGB::WARNING()));
    this->text(label);
    ImGui::PopStyleColor();
}

void ImGuiWrapper::warning_text(const wxString &all_text)
{
    auto label_utf8 = into_u8(all_text);
    warning_text(label_utf8.c_str());
}

void ImGuiWrapper::text_colored(const ImVec4& color, const char* label)
{
    ImGui::TextColored(color, "%s", label);
}

void ImGuiWrapper::text_colored(const ImVec4& color, const std::string& label)
{
    ImGuiWrapper::text_colored(color, label.c_str());
}

void ImGuiWrapper::text_colored(const ImVec4& color, const wxString& label)
{
    auto label_utf8 = into_u8(label);
    ImGuiWrapper::text_colored(color, label_utf8.c_str());
}

void ImGuiWrapper::text_wrapped(const char *label, float wrap_width)
{
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrap_width);
    this->text(label);
    ImGui::PopTextWrapPos();
}

void ImGuiWrapper::text_wrapped(const std::string &label, float wrap_width)
{
    this->text_wrapped(label.c_str(), wrap_width);
}

void ImGuiWrapper::text_wrapped(const wxString &label, float wrap_width)
{
    auto label_utf8 = into_u8(label);
    this->text_wrapped(label_utf8.c_str(), wrap_width);
}

void ImGuiWrapper::tooltip(const char *label, float wrap_width)
{
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(wrap_width);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor(1);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void ImGuiWrapper::tooltip(const std::string &label, float wrap_width) {
    tooltip(label.c_str(), wrap_width);
}

void ImGuiWrapper::tooltip(const wxString &label, float wrap_width)
{
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(wrap_width);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    ImGui::TextUnformatted(label.ToUTF8().data());
    ImGui::PopStyleColor(1);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
ImVec2 ImGuiWrapper::get_slider_icon_size() const
{
    return this->calc_button_size(std::wstring(&ImGui::SliderFloatEditBtnIcon, 1));
}

bool ImGuiWrapper::slider_float(const char* label, float* v, float v_min, float v_max, const char* format/* = "%.3f"*/, float power/* = 1.0f*/, bool clamp /*= true*/, const wxString& tooltip /*= ""*/, bool show_edit_btn /*= true*/)
{
    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    // let the label string start with "##" to hide the automatic label from ImGui::SliderFloat()
    bool label_visible = !boost::algorithm::istarts_with(label, "##");
    std::string str_label = label_visible ? std::string("##") + std::string(label) : std::string(label);

    // removes 2nd evenience of "##", if present
    std::string::size_type pos = str_label.find("##", 2);
    if (pos != std::string::npos)
        str_label = str_label.substr(0, pos) + str_label.substr(pos + 2);

    bool ret = ImGui::SliderFloat(str_label.c_str(), v, v_min, v_max, format, power);

    m_last_slider_status.hovered = ImGui::IsItemHovered();
    m_last_slider_status.edited = ImGui::IsItemEdited();
    m_last_slider_status.clicked = ImGui::IsItemClicked();
    m_last_slider_status.deactivated_after_edit = ImGui::IsItemDeactivatedAfterEdit();
    if (!m_last_slider_status.can_take_snapshot)
        m_last_slider_status.can_take_snapshot = ImGui::IsItemClicked();

    if (!tooltip.empty() && ImGui::IsItemHovered())
        this->tooltip(into_u8(tooltip).c_str(), max_tooltip_width);

    if (clamp)
        *v = std::clamp(*v, v_min, v_max);

    const ImGuiStyle& style = ImGui::GetStyle();
    if (show_edit_btn) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 1, style.ItemSpacing.y });
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);        // ORCA match edit button style
        ImGui::SameLine();
        std::wstring btn_name = ImGui::SliderFloatEditBtnIcon + boost::nowide::widen(str_label);
        ImGui::PushStyleColor(ImGuiCol_Button, { 0.25f, 0.25f, 0.25f, 0.0f });
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0, 0, 0, 0 }); // ORCA match edit button style
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0, 0, 0, 0 }); // ORCA match edit button style
        if (ImGui::Button(into_u8(btn_name).c_str())) {
            ImGui::SetKeyboardFocusHere(-1);
            this->set_requires_extra_frame();
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            this->tooltip(into_u8(_L("Edit")).c_str(), max_tooltip_width);

        ImGui::PopStyleVar(2); // ORCA
    }

    if (label_visible) {
        // if the label is visible, hide the part of it that should be hidden
        std::string out_label = std::string(label);
        std::string::size_type pos = out_label.find("##");
        if (pos != std::string::npos)
            out_label = out_label.substr(0, pos);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 1, style.ItemSpacing.y });
        ImGui::SameLine();
        this->text(out_label.c_str());
        ImGui::PopStyleVar();
    }

    return ret;
}

bool ImGuiWrapper::slider_float(const std::string& label, float* v, float v_min, float v_max, const char* format/* = "%.3f"*/, float power/* = 1.0f*/, bool clamp /*= true*/, const wxString& tooltip /*= ""*/, bool show_edit_btn /*= true*/)
{
    return this->slider_float(label.c_str(), v, v_min, v_max, format, power, clamp, tooltip, show_edit_btn);
}

bool ImGuiWrapper::slider_float(const wxString& label, float* v, float v_min, float v_max, const char* format/* = "%.3f"*/, float power/* = 1.0f*/, bool clamp /*= true*/, const wxString& tooltip /*= ""*/, bool show_edit_btn /*= true*/)
{
    auto label_utf8 = into_u8(label);
    return this->slider_float(label_utf8.c_str(), v, v_min, v_max, format, power, clamp, tooltip, show_edit_btn);
}
#else
bool ImGuiWrapper::slider_float(const char* label, float* v, float v_min, float v_max, const char* format/* = "%.3f"*/, float power/* = 1.0f*/, bool clamp /*= true*/)
{
    bool ret = ImGui::SliderFloat(label, v, v_min, v_max, format, power);
    if (clamp)
        *v = std::clamp(*v, v_min, v_max);
    return ret;
}

bool ImGuiWrapper::slider_float(const std::string& label, float* v, float v_min, float v_max, const char* format/* = "%.3f"*/, float power/* = 1.0f*/, bool clamp /*= true*/)
{
    return this->slider_float(label.c_str(), v, v_min, v_max, format, power, clamp);
}

bool ImGuiWrapper::slider_float(const wxString& label, float* v, float v_min, float v_max, const char* format/* = "%.3f"*/, float power/* = 1.0f*/, bool clamp /*= true*/)
{
    auto label_utf8 = into_u8(label);
    return this->slider_float(label_utf8.c_str(), v, v_min, v_max, format, power, clamp);
}
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT

static bool image_button_ex(ImGuiID id, ImTextureID texture_id, const ImVec2& size, const ImVec2& uv0, const ImVec2& uv1, const ImVec2& padding, const ImVec4& bg_col, const ImVec4& tint_col, ImGuiButtonFlags flags)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const ImRect bb(window->DC.CursorPos, window->DC.CursorPos + size + padding * 2);
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, flags);

    // Render
    const ImU32 col = ImGui::GetColorU32((held && hovered) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    ImGui::RenderNavHighlight(bb, id);
    ImGui::RenderFrame(bb.Min, bb.Max, col, true, ImClamp((float)ImMin(padding.x, padding.y), 0.0f, g.Style.FrameRounding));
    if (bg_col.w > 0.0f)
        window->DrawList->AddRectFilled(bb.Min + padding, bb.Max - padding, ImGui::GetColorU32(bg_col));
    window->DrawList->AddImage(texture_id, bb.Min + padding, bb.Max - padding, uv0, uv1, ImGui::GetColorU32(tint_col));

    return pressed;
}

bool ImGuiWrapper::image_button(ImTextureID user_texture_id, const ImVec2& size, const ImVec2& uv0, const ImVec2& uv1, int frame_padding, const ImVec4& bg_col, const ImVec4& tint_col, ImGuiButtonFlags flags)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    if (window->SkipItems)
        return false;

    // Default to using texture ID as ID. User can still push string/integer prefixes.
    ImGui::PushID((void*)(intptr_t)user_texture_id);
    const ImGuiID id = window->GetID("#image");
    ImGui::PopID();

    const ImVec2 padding = (frame_padding >= 0) ? ImVec2((float)frame_padding, (float)frame_padding) : g.Style.FramePadding;
    return image_button_ex(id, user_texture_id, size, uv0, uv1, padding, bg_col, tint_col, flags);
}

bool ImGuiWrapper::image_button(const wchar_t icon, const wxString& tooltip)
{
    const ImGuiIO& io = ImGui::GetIO();
    const ImTextureID tex_id = io.Fonts->TexID;
    assert(io.Fonts->TexWidth > 0 && io.Fonts->TexHeight > 0);
    const float inv_tex_w = 1.0f / float(io.Fonts->TexWidth);
    const float inv_tex_h = 1.0f / float(io.Fonts->TexHeight);
    const ImFontAtlasCustomRect* const rect = GetTextureCustomRect(icon);
    const ImVec2 size = { float(rect->Width), float(rect->Height) };
    const ImVec2 uv0 = ImVec2(float(rect->X) * inv_tex_w, float(rect->Y) * inv_tex_h);
    const ImVec2 uv1 = ImVec2(float(rect->X + rect->Width) * inv_tex_w, float(rect->Y + rect->Height) * inv_tex_h);
    ImGui::PushStyleColor(ImGuiCol_Button, { 0.25f, 0.25f, 0.25f, 0.0f });
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, { 0.4f, 0.4f, 0.4f, 1.0f });
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, { 0.25f, 0.25f, 0.25f, 1.0f });
    const bool res = image_button(tex_id, size, uv0, uv1);
    ImGui::PopStyleColor(3);

    if (!tooltip.empty() && ImGui::IsItemHovered())
        this->tooltip(tooltip, ImGui::GetFontSize() * 20.0f);

    return res;
}

bool ImGuiWrapper::combo(const wxString& label, const std::vector<std::string>& options, int& selection, ImGuiComboFlags flags/* = 0*/, float label_width/* = 0.0f*/, float item_width/* = 0.0f*/)
{
    return combo(into_u8(label), options, selection, flags, label_width, item_width);
}

bool ImGuiWrapper::combo(const std::string& label, const std::vector<std::string>& options, int& selection, ImGuiComboFlags flags/* = 0*/, float label_width/* = 0.0f*/, float item_width/* = 0.0f*/)
{
    // this is to force the label to the left of the widget:
    const bool hidden_label = boost::starts_with(label, "##");
    if (!label.empty() && !hidden_label) {
        text(label);
        ImGui::SameLine(label_width);
    }
    ImGui::PushItemWidth(item_width);

    int selection_out = selection;
    bool res = false;

    const char *selection_str = selection < int(options.size()) && selection >= 0 ? options[selection].c_str() : "";
    if (ImGui::BeginCombo(hidden_label ? label.c_str() : ("##" + label).c_str(), selection_str, flags)) {
        for (int i = 0; i < (int)options.size(); i++) {
            if (ImGui::Selectable(options[i].c_str(), i == selection)) {
                selection_out = i;
                res = true;
            }
        }

        ImGui::EndCombo();
    }

    selection = selection_out;
    return res;
}

// Scroll up for one item
static void scroll_up()
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;

    float item_size_y = window->DC.PrevLineSize.y + g.Style.ItemSpacing.y;
    float win_top = window->Scroll.y;

    ImGui::SetScrollY(win_top - item_size_y);
}

// Scroll down for one item
static void scroll_down()
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;

    float item_size_y = window->DC.PrevLineSize.y + g.Style.ItemSpacing.y;
    float win_top = window->Scroll.y;

    ImGui::SetScrollY(win_top + item_size_y);
}

static void process_mouse_wheel(int& mouse_wheel)
{
    if (mouse_wheel > 0)
        scroll_up();
    else if (mouse_wheel < 0)
        scroll_down();
    mouse_wheel = 0;
}

bool ImGuiWrapper::undo_redo_list(const ImVec2& size, const bool is_undo, bool (*items_getter)(const bool , int , const char**), int& hovered, int& selected, int& mouse_wheel)
{
    bool is_hovered = false;
    ImGui::ListBoxHeader("", size);

    int i=0;
    const char* item_text;
    while (items_getter(is_undo, i, &item_text)) {
        ImGui::Selectable(item_text, i < hovered);

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", item_text);
            hovered = i;
            is_hovered = true;
        }

        if (ImGui::IsItemClicked())
            selected = i;
        i++;
    }

    if (is_hovered)
        process_mouse_wheel(mouse_wheel);

    ImGui::ListBoxFooter();
    return is_hovered;
}

// It's a copy of IMGui::Selactable function.
// But a little beat modified to change a label text.
// If item is hovered we should use another color for highlighted letters.
// To do that we push a ColorMarkerHovered symbol at the very beginning of the label
// This symbol will be used to a color selection for the highlighted letters.
// see imgui_draw.cpp, void ImFont::RenderText()
static bool selectable(const char* label, bool selected, ImGuiSelectableFlags flags = 0, const ImVec2& size_arg = ImVec2(0, 0), bool* out_hovered = nullptr)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;

    // Submit label or explicit size to ItemSize(), whereas ItemAdd() will submit a larger/spanning rectangle.
    ImGuiID id = window->GetID(label);
    ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);
    ImVec2 size(size_arg.x != 0.0f ? size_arg.x : label_size.x, size_arg.y != 0.0f ? size_arg.y : label_size.y);
    ImVec2 pos = window->DC.CursorPos;
    pos.y += window->DC.CurrLineTextBaseOffset;
    ImGui::ItemSize(size, 0.0f);

    // Fill horizontal space
    // We don't support (size < 0.0f) in Selectable() because the ItemSpacing extension would make explicitly right-aligned sizes not visibly match other widgets.
    const bool span_all_columns = (flags & ImGuiSelectableFlags_SpanAllColumns) != 0;
    const float min_x = span_all_columns ? window->ParentWorkRect.Min.x : pos.x;
    const float max_x = span_all_columns ? window->ParentWorkRect.Max.x : window->WorkRect.Max.x;
    if (size_arg.x == 0.0f || (flags & ImGuiSelectableFlags_SpanAvailWidth))
        size.x = ImMax(label_size.x, max_x - min_x);

    // Text stays at the submission position, but bounding box may be extended on both sides
    const ImVec2 text_min = pos;
    const ImVec2 text_max(min_x + size.x, pos.y + size.y);

    // Selectables are meant to be tightly packed together with no click-gap, so we extend their box to cover spacing between selectable.
    ImRect bb(min_x, pos.y, text_max.x, text_max.y);
    if ((flags & ImGuiSelectableFlags_NoPadWithHalfSpacing) == 0)
    {
        const float spacing_x = span_all_columns ? 0.0f : style.ItemSpacing.x;
        const float spacing_y = style.ItemSpacing.y;
        const float spacing_L = IM_FLOOR(spacing_x * 0.50f);
        const float spacing_U = IM_FLOOR(spacing_y * 0.50f);
        bb.Min.x -= spacing_L;
        bb.Min.y -= spacing_U;
        bb.Max.x += (spacing_x - spacing_L);
        bb.Max.y += (spacing_y - spacing_U);
    }
    //if (g.IO.KeyCtrl) { GetForegroundDrawList()->AddRect(bb.Min, bb.Max, IM_COL32(0, 255, 0, 255)); }

    // Modify ClipRect for the ItemAdd(), faster than doing a PushColumnsBackground/PushTableBackground for every Selectable..
    const float backup_clip_rect_min_x = window->ClipRect.Min.x;
    const float backup_clip_rect_max_x = window->ClipRect.Max.x;
    if (span_all_columns)
    {
        window->ClipRect.Min.x = window->ParentWorkRect.Min.x;
        window->ClipRect.Max.x = window->ParentWorkRect.Max.x;
    }

    bool item_add;
    if (flags & ImGuiSelectableFlags_Disabled)
    {
        ImGuiItemFlags backup_item_flags = g.CurrentItemFlags;
        g.CurrentItemFlags |= ImGuiItemFlags_Disabled | ImGuiItemFlags_NoNavDefaultFocus;
        item_add = ImGui::ItemAdd(bb, id);
        g.CurrentItemFlags = backup_item_flags;
    }
    else
    {
        item_add = ImGui::ItemAdd(bb, id);
    }

    if (span_all_columns)
    {
        window->ClipRect.Min.x = backup_clip_rect_min_x;
        window->ClipRect.Max.x = backup_clip_rect_max_x;
    }

    if (!item_add)
        return false;

    // FIXME: We can standardize the behavior of those two, we could also keep the fast path of override ClipRect + full push on render only,
    // which would be advantageous since most selectable are not selected.
    if (span_all_columns && window->DC.CurrentColumns)
        ImGui::PushColumnsBackground();
    else if (span_all_columns && g.CurrentTable)
        ImGui::TablePushBackgroundChannel();

    // We use NoHoldingActiveID on menus so user can click and _hold_ on a menu then drag to browse child entries
    ImGuiButtonFlags button_flags = 0;
    if (flags & ImGuiSelectableFlags_NoHoldingActiveID) { button_flags |= ImGuiButtonFlags_NoHoldingActiveId; }
    if (flags & ImGuiSelectableFlags_SelectOnClick)     { button_flags |= ImGuiButtonFlags_PressedOnClick; }
    if (flags & ImGuiSelectableFlags_SelectOnRelease)   { button_flags |= ImGuiButtonFlags_PressedOnRelease; }
    if (flags & ImGuiSelectableFlags_Disabled)          { button_flags |= ImGuiButtonFlags_Disabled; }
    if (flags & ImGuiSelectableFlags_AllowDoubleClick)  { button_flags |= ImGuiButtonFlags_PressedOnClickRelease | ImGuiButtonFlags_PressedOnDoubleClick; }
    if (flags & ImGuiSelectableFlags_AllowItemOverlap)  { button_flags |= ImGuiButtonFlags_AllowItemOverlap; }

    if (flags & ImGuiSelectableFlags_Disabled)
        selected = false;

    const bool was_selected = selected;
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, button_flags);

    // Update NavId when clicking or when Hovering (this doesn't happen on most widgets), so navigation can be resumed with gamepad/keyboard
    if (pressed || (hovered && (flags & ImGuiSelectableFlags_SetNavIdOnHover)))
    {
        if (!g.NavDisableMouseHover && g.NavWindow == window && g.NavLayer == window->DC.NavLayerCurrent)
        {
            ImGui::SetNavID(id, window->DC.NavLayerCurrent, window->DC.NavFocusScopeIdCurrent, ImRect(bb.Min - window->Pos, bb.Max - window->Pos));
            g.NavDisableHighlight = true;
        }
    }
    if (pressed)
        ImGui::MarkItemEdited(id);

    if (flags & ImGuiSelectableFlags_AllowItemOverlap)
        ImGui::SetItemAllowOverlap();

    // In this branch, Selectable() cannot toggle the selection so this will never trigger.
    if (selected != was_selected) //-V547
        window->DC.LastItemStatusFlags |= ImGuiItemStatusFlags_ToggledSelection;

    // Render
    if (held && (flags & ImGuiSelectableFlags_DrawHoveredWhenHeld))
        hovered = true;
    if (hovered || selected)
    {
        const ImU32 col = ImGui::GetColorU32((held && hovered) ? ImGuiCol_HeaderActive : hovered ? ImGuiCol_HeaderHovered : ImGuiCol_Header);
        ImGui::RenderFrame(bb.Min, bb.Max, col, false, 0.0f);
        ImGui::RenderNavHighlight(bb, id, ImGuiNavHighlightFlags_TypeThin | ImGuiNavHighlightFlags_NoRounding);
    }

    if (span_all_columns && window->DC.CurrentColumns)
        ImGui::PopColumnsBackground();
    else if (span_all_columns && g.CurrentTable)
        ImGui::TablePopBackgroundChannel();

    // mark a label with a ColorMarkerHovered, if item is hovered
    char marked_label[512]; //255 symbols is not enough for translated string (e.t. to Russian)
    if (hovered || selected) {
        sprintf(marked_label, "%c%s", ImGui::ColorMarkerHovered, label);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    }
    else
        strcpy(marked_label, label);

    if (flags & ImGuiSelectableFlags_Disabled) ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    ImGui::RenderTextClipped(text_min, text_max, marked_label, NULL, &label_size, style.SelectableTextAlign, &bb);
    if (flags & ImGuiSelectableFlags_Disabled) ImGui::PopStyleColor();
    if (hovered || selected) ImGui::PopStyleColor();

    if (out_hovered) *out_hovered = hovered;

    // Automatically close popups
    if (pressed && (window->Flags & ImGuiWindowFlags_Popup) && !(flags & ImGuiSelectableFlags_DontClosePopups) && !(g.CurrentItemFlags & ImGuiItemFlags_SelectableDontClosePopup))
        ImGui::CloseCurrentPopup();
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, window->DC.LastItemStatusFlags);
    return pressed;
}

bool begin_menu(const char *label, bool enabled)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext &    g            = *GImGui;
    const ImGuiStyle &style        = g.Style;
    const ImGuiID     id           = window->GetID(label);
    bool              menu_is_open = ImGui::IsPopupOpen(id, ImGuiPopupFlags_None);

    // Sub-menus are ChildWindow so that mouse can be hovering across them (otherwise top-most popup menu would steal focus and not allow hovering on parent menu)
    ImGuiWindowFlags flags = ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus;
    if (window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu)) flags |= ImGuiWindowFlags_ChildWindow;

    // If a menu with same the ID was already submitted, we will append to it, matching the behavior of Begin().
    // We are relying on a O(N) search - so O(N log N) over the frame - which seems like the most efficient for the expected small amount of BeginMenu() calls per frame.
    // If somehow this is ever becoming a problem we can switch to use e.g. ImGuiStorage mapping key to last frame used.
    if (g.MenusIdSubmittedThisFrame.contains(id)) {
        if (menu_is_open)
            menu_is_open = ImGui::BeginPopupEx(id, flags); // menu_is_open can be 'false' when the popup is completely clipped (e.g. zero size display)
        else
            g.NextWindowData.ClearFlags(); // we behave like Begin() and need to consume those values
        return menu_is_open;
    }

    // Tag menu as used. Next time BeginMenu() with same ID is called it will append to existing menu
    g.MenusIdSubmittedThisFrame.push_back(id);

    ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);
    bool   pressed;
    bool   menuset_is_open = !(window->Flags & ImGuiWindowFlags_Popup) &&
                           (g.OpenPopupStack.Size > g.BeginPopupStack.Size && g.OpenPopupStack[g.BeginPopupStack.Size].OpenParentId == window->IDStack.back());
    ImGuiWindow *backed_nav_window = g.NavWindow;
    if (menuset_is_open) g.NavWindow = window; // Odd hack to allow hovering across menus of a same menu-set (otherwise we wouldn't be able to hover parent)

    // The reference position stored in popup_pos will be used by Begin() to find a suitable position for the child menu,
    // However the final position is going to be different! It is chosen by FindBestWindowPosForPopup().
    // e.g. Menus tend to overlap each other horizontally to amplify relative Z-ordering.
    ImVec2 popup_pos, pos = window->DC.CursorPos;
    if (window->DC.LayoutType == ImGuiLayoutType_Horizontal) {
        // Menu inside an horizontal menu bar
        // Selectable extend their highlight by half ItemSpacing in each direction.
        // For ChildMenu, the popup position will be overwritten by the call to FindBestWindowPosForPopup() in Begin()
        popup_pos = ImVec2(pos.x - 1.0f - IM_FLOOR(style.ItemSpacing.x * 0.5f), pos.y - style.FramePadding.y + window->MenuBarHeight());
        window->DC.CursorPos.x += IM_FLOOR(style.ItemSpacing.x * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 2.0f, style.ItemSpacing.y));
        float w = label_size.x;
        pressed = selectable(label, menu_is_open,
                             ImGuiSelectableFlags_NoHoldingActiveID | ImGuiSelectableFlags_SelectOnClick | ImGuiSelectableFlags_DontClosePopups |
                                 (!enabled ? ImGuiSelectableFlags_Disabled : 0),
                             ImVec2(w, 0.0f));
        ImGui::PopStyleVar();
        window->DC.CursorPos.x += IM_FLOOR(
            style.ItemSpacing.x *
            (-1.0f +
             0.5f)); // -1 spacing to compensate the spacing added when Selectable() did a SameLine(). It would also work to call SameLine() ourselves after the PopStyleVar().
    } else {
        // Menu inside a menu
        // (In a typical menu window where all items are BeginMenu() or MenuItem() calls, extra_w will always be 0.0f.
        //  Only when they are other items sticking out we're going to add spacing, yet only register minimum width into the layout system.
        popup_pos      = ImVec2(pos.x, pos.y - style.WindowPadding.y);
        float min_w    = window->DC.MenuColumns.DeclColumns(label_size.x, 0.0f, IM_FLOOR(g.FontSize * 1.20f)); // Feedback to next frame
        float extra_w  = ImMax(0.0f, ImGui::GetContentRegionAvail().x - min_w);
        pressed        = selectable(label, menu_is_open,
                             ImGuiSelectableFlags_NoHoldingActiveID | ImGuiSelectableFlags_SelectOnClick | ImGuiSelectableFlags_DontClosePopups |
                                 ImGuiSelectableFlags_SpanAvailWidth | (!enabled ? ImGuiSelectableFlags_Disabled : 0),
                             ImVec2(min_w, 0.0f));
        ImU32 text_col = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        ImGui::RenderArrow(window->DrawList, pos + ImVec2(window->DC.MenuColumns.Pos[2] + extra_w + g.FontSize * 0.30f, 0.0f), text_col, ImGuiDir_Right);
    }

    const bool hovered = enabled && ImGui::ItemHoverable(window->DC.LastItemRect, id);
    if (menuset_is_open) g.NavWindow = backed_nav_window;

    bool want_open  = false;
    bool want_close = false;
    if (window->DC.LayoutType == ImGuiLayoutType_Vertical) // (window->Flags & (ImGuiWindowFlags_Popup|ImGuiWindowFlags_ChildMenu))
    {
        // Close menu when not hovering it anymore unless we are moving roughly in the direction of the menu
        // Implement http://bjk5.com/post/44698559168/breaking-down-amazons-mega-dropdown to avoid using timers, so menus feels more reactive.
        bool moving_toward_other_child_menu = false;

        ImGuiWindow *child_menu_window = (g.BeginPopupStack.Size < g.OpenPopupStack.Size && g.OpenPopupStack[g.BeginPopupStack.Size].SourceWindow == window) ?
                                             g.OpenPopupStack[g.BeginPopupStack.Size].Window :
                                             NULL;
        if (g.HoveredWindow == window && child_menu_window != NULL && !(window->Flags & ImGuiWindowFlags_MenuBar)) {
            // FIXME-DPI: Values should be derived from a master "scale" factor.
            ImRect next_window_rect = child_menu_window->Rect();
            ImVec2 ta               = g.IO.MousePos - g.IO.MouseDelta;
            ImVec2 tb               = (window->Pos.x < child_menu_window->Pos.x) ? next_window_rect.GetTL() : next_window_rect.GetTR();
            ImVec2 tc               = (window->Pos.x < child_menu_window->Pos.x) ? next_window_rect.GetBL() : next_window_rect.GetBR();
            float  extra            = ImClamp(ImFabs(ta.x - tb.x) * 0.30f, 5.0f, 30.0f); // add a bit of extra slack.
            ta.x += (window->Pos.x < child_menu_window->Pos.x) ? -0.5f : +0.5f;          // to avoid numerical issues
            tb.y = ta.y +
                   ImMax((tb.y - extra) - ta.y, -100.0f); // triangle is maximum 200 high to limit the slope and the bias toward large sub-menus // FIXME: Multiply by fb_scale?
            tc.y                           = ta.y + ImMin((tc.y + extra) - ta.y, +100.0f);
            moving_toward_other_child_menu = ImTriangleContainsPoint(ta, tb, tc, g.IO.MousePos);
            // GetForegroundDrawList()->AddTriangleFilled(ta, tb, tc, moving_within_opened_triangle ? IM_COL32(0,128,0,128) : IM_COL32(128,0,0,128)); // [DEBUG]
        }
        if (menu_is_open && !hovered && g.HoveredWindow == window && g.HoveredIdPreviousFrame != 0 && g.HoveredIdPreviousFrame != id && !moving_toward_other_child_menu)
            want_close = true;

        if (!menu_is_open && hovered && pressed) // Click to open
            want_open = true;
        else if (!menu_is_open && hovered && !moving_toward_other_child_menu) // Hover to open
            want_open = true;

        if (g.NavActivateId == id) {
            want_close = menu_is_open;
            want_open  = !menu_is_open;
        }
        if (g.NavId == id && g.NavMoveRequest && g.NavMoveDir == ImGuiDir_Right) // Nav-Right to open
        {
            want_open = true;
            ImGui::NavMoveRequestCancel();
        }
    } else {
        // Menu bar
        if (menu_is_open && pressed && menuset_is_open) // Click an open menu again to close it
        {
            want_close = true;
            want_open = menu_is_open = false;
        } else if (pressed || (hovered && menuset_is_open && !menu_is_open)) // First click to open, then hover to open others
        {
            want_open = true;
        } else if (g.NavId == id && g.NavMoveRequest && g.NavMoveDir == ImGuiDir_Down) // Nav-Down to open
        {
            want_open = true;
            ImGui::NavMoveRequestCancel();
        }
    }

    if (!enabled) // explicitly close if an open menu becomes disabled, facilitate users code a lot in pattern such as 'if (BeginMenu("options", has_object)) { ..use object.. }'
        want_close = true;
    if (want_close && ImGui::IsPopupOpen(id, ImGuiPopupFlags_None)) ImGui::ClosePopupToLevel(g.BeginPopupStack.Size, true);

    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, window->DC.LastItemStatusFlags | ImGuiItemStatusFlags_Openable | (menu_is_open ? ImGuiItemStatusFlags_Opened : 0));

    if (!menu_is_open && want_open && g.OpenPopupStack.Size > g.BeginPopupStack.Size) {
        // Don't recycle same menu level in the same frame, first close the other menu and yield for a frame.
        ImGui::OpenPopup(label);
        return false;
    }

    menu_is_open |= want_open;
    if (want_open) ImGui::OpenPopup(label);

    if (menu_is_open) {
        ImGui::SetNextWindowPos(popup_pos,
                                ImGuiCond_Always);     // Note: this is super misleading! The value will serve as reference for FindBestWindowPosForPopup(), not actual pos.
        menu_is_open = ImGui::BeginPopupEx(id, flags); // menu_is_open can be 'false' when the popup is completely clipped (e.g. zero size display)
    } else {
        g.NextWindowData.ClearFlags(); // We behave like Begin() and need to consume those values
    }

    return menu_is_open;
}

void end_menu()
{
    ImGui::EndMenu();
}

bool menu_item_with_icon(const char *label, const char *shortcut, ImVec2 icon_size /* = ImVec2(0, 0)*/, ImU32 icon_color /* = 0*/, bool selected /* = false*/, bool enabled /* = true*/, bool* hovered/* = nullptr*/)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext &g          = *GImGui;
    ImGuiStyle &  style      = g.Style;
    ImVec2        pos        = window->DC.CursorPos;
    ImVec2        label_size = ImGui::CalcTextSize(label, NULL, true);

    // We've been using the equivalent of ImGuiSelectableFlags_SetNavIdOnHover on all Selectable() since early Nav system days (commit 43ee5d73),
    // but I am unsure whether this should be kept at all. For now moved it to be an opt-in feature used by menus only.
    ImGuiSelectableFlags flags = ImGuiSelectableFlags_SelectOnRelease | ImGuiSelectableFlags_SetNavIdOnHover | (enabled ? 0 : ImGuiSelectableFlags_Disabled);
    bool                 pressed;
    if (window->DC.LayoutType == ImGuiLayoutType_Horizontal) {
        // Mimic the exact layout spacing of BeginMenu() to allow MenuItem() inside a menu bar, which is a little misleading but may be useful
        // Note that in this situation: we don't render the shortcut, we render a highlight instead of the selected tick mark.
        float w = label_size.x;
        window->DC.CursorPos.x += IM_FLOOR(style.ItemSpacing.x * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 2.0f, style.ItemSpacing.y));
        pressed = ImGui::Selectable(label, selected, flags, ImVec2(w, 0.0f));
        ImGui::PopStyleVar();
        window->DC.CursorPos.x += IM_FLOOR(
            style.ItemSpacing.x *
            (-1.0f +
             0.5f)); // -1 spacing to compensate the spacing added when Selectable() did a SameLine(). It would also work to call SameLine() ourselves after the PopStyleVar().
    } else {
        // Menu item inside a vertical menu
        // (In a typical menu window where all items are BeginMenu() or MenuItem() calls, extra_w will always be 0.0f.
        //  Only when they are other items sticking out we're going to add spacing, yet only register minimum width into the layout system.
        float shortcut_w = shortcut ? ImGui::CalcTextSize(shortcut, NULL).x : 0.0f;
        float min_w      = window->DC.MenuColumns.DeclColumns(label_size.x, shortcut_w, IM_FLOOR(g.FontSize * 1.20f)); // Feedback for next frame
        float extra_w    = std::max(0.0f, ImGui::GetContentRegionAvail().x - min_w);
        pressed          = selectable(label, false, flags | ImGuiSelectableFlags_SpanAvailWidth, ImVec2(min_w, 0.0f), hovered);

        if (icon_size.x != 0 && icon_size.y != 0) {
            float selectable_pos_y = pos.y + -0.5f * style.ItemSpacing.y;
            float icon_pos_y = selectable_pos_y + (label_size.y + style.ItemSpacing.y - icon_size.y) / 2;
            float icon_pos_x = pos.x + window->DC.MenuColumns.Pos[2] + extra_w + g.FontSize * 0.40f;
            ImVec2 icon_pos = ImVec2(icon_pos_x, icon_pos_y);
            if (icon_color != 0)
                ImGui::RenderFrame(icon_pos, icon_pos + icon_size, icon_color);
            else {
                static ImTextureID transparent;
                IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/transparent.svg", icon_size.x, icon_size.y, transparent);
                window->DrawList->AddImage(transparent, icon_pos, icon_pos + icon_size, { 0,0 }, { 1,1 }, ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 1.f)));
            }
        }

        if (shortcut_w > 0.0f) {
            ImGui::PushStyleColor(ImGuiCol_Text, g.Style.Colors[ImGuiCol_TextDisabled]);
            ImGui::RenderText(pos + ImVec2(window->DC.MenuColumns.Pos[1] + extra_w, 0.0f), shortcut, NULL, false);
            ImGui::PopStyleColor();
        }
        if (selected) {
            //ImGui::RenderCheckMark(window->DrawList, pos + ImVec2(window->DC.MenuColumns.Pos[2] + extra_w + g.FontSize * 0.40f, g.FontSize * 0.134f * 0.5f),
            //                       ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), g.FontSize * 0.866f);
        }
    }

    IMGUI_TEST_ENGINE_ITEM_INFO(window->DC.LastItemId, label, window->DC.LastItemStatusFlags | ImGuiItemStatusFlags_Checkable | (selected ? ImGuiItemStatusFlags_Checked : 0));
    return pressed;
}

// Scroll so that the hovered item is at the top of the window
static void scroll_y(int hover_id)
{
    if (hover_id < 0)
        return;
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;

    float item_size_y = window->DC.PrevLineSize.y + g.Style.ItemSpacing.y;
    float item_delta = 0.5 * item_size_y;

    float item_top = item_size_y * hover_id;
    float item_bottom = item_top + item_size_y;

    float win_top = window->Scroll.y;
    float win_bottom = window->Scroll.y + window->Size.y;

    if (item_bottom + item_delta >= win_bottom)
        ImGui::SetScrollY(win_top + item_size_y);
    else if (item_top - item_delta <= win_top)
        ImGui::SetScrollY(win_top - item_size_y);
}

// Use this function instead of ImGui::IsKeyPressed.
// ImGui::IsKeyPressed is related for *GImGui.IO.KeysDownDuration[user_key_index]
// And after first key pressing IsKeyPressed() return "true" always even if key wasn't pressed
static void process_key_down(ImGuiKey imgui_key, std::function<void()> f)
{
    if (ImGui::IsKeyDown(ImGui::GetKeyIndex(imgui_key)))
    {
        f();
        // set KeysDown to false to avoid redundant key down processing
        ImGuiContext& g = *GImGui;
        g.IO.KeysDown[ImGui::GetKeyIndex(imgui_key)] = false;
    }
}

void ImGuiWrapper::search_list(const ImVec2& size_, bool (*items_getter)(int, const char** label, const char** tooltip), char* search_str,
                               Search::OptionViewParameters& view_params, int& selected, bool& edited, int& mouse_wheel, bool is_localized)
{
    int& hovered_id = view_params.hovered_id;
    // ImGui::ListBoxHeader("", size);
    {
        // rewrote part of function to add a TextInput instead of label Text
        ImGuiContext& g = *GImGui;
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return ;

        const ImGuiStyle& style = g.Style;

        // Size default to hold ~7 items. Fractional number of items helps seeing that we can scroll down/up without looking at scrollbar.
        ImVec2 size = ImGui::CalcItemSize(size_, ImGui::CalcItemWidth(), ImGui::GetTextLineHeightWithSpacing() * 7.4f + style.ItemSpacing.y);
        ImRect frame_bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + size.x, window->DC.CursorPos.y + size.y));

        ImRect bb(frame_bb.Min, frame_bb.Max);
        window->DC.LastItemRect = bb; // Forward storage for ListBoxFooter.. dodgy.
        g.NextItemData.ClearFlags();

        if (!ImGui::IsRectVisible(bb.Min, bb.Max))
        {
            ImGui::ItemSize(bb.GetSize(), style.FramePadding.y);
            ImGui::ItemAdd(bb, 0, &frame_bb);
            return ;
        }

        ImGui::BeginGroup();

        const ImGuiID id = ImGui::GetID(search_str);
        ImVec2 search_size = ImVec2(size.x, ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y);

        if (!ImGui::IsAnyItemFocused() && !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0))
            ImGui::SetKeyboardFocusHere(0);

        // The press on Esc key invokes editing of InputText (removes last changes)
        // So we should save previous value...
        std::string str = search_str;
        ImGui::InputTextEx("", NULL, search_str, 40, search_size, ImGuiInputTextFlags_AutoSelectAll, NULL, NULL);
        edited = ImGui::IsItemEdited();
        if (edited)
            hovered_id = 0;

        process_key_down(ImGuiKey_Escape, [&selected, search_str, str]() {
            // use 9999 to mark selection as a Esc key
            selected = 9999;
            // ... and when Esc key was pressed, than revert search_str value
            strcpy(search_str, str.c_str());
        });

        ImGui::BeginChildFrame(id, frame_bb.GetSize());
    }

    int i = 0;
    const char* item_text;
    const char* tooltip;
    int mouse_hovered = -1;

    while (items_getter(i, &item_text, &tooltip))
    {
        selectable(item_text, i == hovered_id);

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", /*item_text*/tooltip);
                hovered_id = -1;
            mouse_hovered = i;
        }

        if (ImGui::IsItemClicked())
            selected = i;
        i++;
    }

    // Process mouse wheel
    if (mouse_hovered > 0)
        process_mouse_wheel(mouse_wheel);

    // process Up/DownArrows and Enter
    process_key_down(ImGuiKey_UpArrow, [&hovered_id, mouse_hovered]() {
        if (mouse_hovered > 0)
            scroll_up();
        else {
            if (hovered_id > 0)
                --hovered_id;
            scroll_y(hovered_id);
        }
    });

    process_key_down(ImGuiKey_DownArrow, [&hovered_id, mouse_hovered, i]() {
        if (mouse_hovered > 0)
            scroll_down();
        else {
            if (hovered_id < 0)
                hovered_id = 0;
            else if (hovered_id < i - 1)
                ++hovered_id;
            scroll_y(hovered_id);
        }
    });

    process_key_down(ImGuiKey_Enter, [&selected, hovered_id]() {
        selected = hovered_id;
    });

    ImGui::ListBoxFooter();

    auto check_box = [&edited, this](const wxString& label, bool& check) {
        ImGui::SameLine();
        bool ch = check;
        checkbox(label, ch);
        if (ImGui::IsItemClicked()) {
            check = !check;
            edited = true;
        }
    };

    ImGui::AlignTextToFramePadding();

    // add checkboxes for show/hide Categories and Groups
    //text(_L("Use for search")+":");
    //check_box(_L("Category"),   view_params.category);
    //if (is_localized)
    //    check_box(_L("Search in English"), view_params.english);
}

void ImGuiWrapper::bold_text(const std::string& str)
{
    if (bold_font){
        ImGui::PushFont(bold_font);
        text(str);
        ImGui::PopFont();
    } else {
        text(str);
    }
}
bool ImGuiWrapper::push_bold_font() {
    if (bold_font) {
        ImGui::PushFont(bold_font);
        return true;
    }
    else {
        return false;
    }
}
bool ImGuiWrapper::pop_bold_font() {
    if (bold_font) {
        ImGui::PopFont();
        return true;
    }
    else {
        return false;
    }
}
bool ImGuiWrapper::push_font_by_name(std::string font_name)
{
    auto sys_font = im_fonts_map.find(font_name);
    if (sys_font != im_fonts_map.end()) {
        ImFont* font = sys_font->second;
        if (font && font->ContainerAtlas && font->Glyphs.Size > 4)
            ImGui::PushFont(font);
        else {
            ImGui::PushFont(default_font);
        }
        return true;
    }
    return false;
}

bool ImGuiWrapper::pop_font_by_name(std::string font_name)
{
    auto sys_font = im_fonts_map.find(font_name);
    if (sys_font != im_fonts_map.end()) {
        ImGui::PopFont();
        return true;
    }
    return false;
}

void ImGuiWrapper::title(const std::string& str)
{
    if (bold_font){
        ImGui::PushFont(bold_font);
        text(str);
        ImGui::PopFont();
    } else {
        text(str);
    }
    ImGui::Separator();
}

void ImGuiWrapper::title(const std::string &str, bool suppress_seperator)
{
    if (bold_font) {
        ImGui::PushFont(bold_font);
        text(str);
        ImGui::PopFont();
    } else {
        text(str);
    }
    if (!suppress_seperator)    {
      ImGui::Separator();
    }
}

void ImGuiWrapper::disabled_begin(bool disabled)
{
    wxCHECK_RET(!m_disabled, "ImGUI: Unbalanced disabled_begin() call");

    if (disabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        m_disabled = true;
    }
}

void ImGuiWrapper::disabled_end()
{
    if (m_disabled) {
        ImGui::PopItemFlag();
        ImGui::PopStyleVar();
        m_disabled = false;
    }
}

bool ImGuiWrapper::want_mouse() const
{
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiWrapper::want_keyboard() const
{
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool ImGuiWrapper::want_text_input() const
{
    return ImGui::GetIO().WantTextInput;
}

bool ImGuiWrapper::want_any_input() const
{
    const auto io = ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput;
}

ImFontAtlasCustomRect* ImGuiWrapper::GetTextureCustomRect(const wchar_t& tex_id)
{
    auto item = m_custom_glyph_rects_ids.find(tex_id);
    return (item != m_custom_glyph_rects_ids.end()) ? ImGui::GetIO().Fonts->GetCustomRectByIndex(m_custom_glyph_rects_ids[tex_id]) : nullptr;
}

void ImGuiWrapper::disable_background_fadeout_animation()
{
    GImGui->DimBgRatio = 1.0f;
}

ImU32 ImGuiWrapper::to_ImU32(const ColorRGBA& color)
{
    return ImGui::GetColorU32({ color.r(), color.g(), color.b(), color.a() });
}

ImVec4 ImGuiWrapper::to_ImVec4(const ColorRGBA& color)
{
    return { color.r(), color.g(), color.b(), color.a() };
}

ColorRGBA ImGuiWrapper::from_ImU32(const ImU32& color)
{
    return from_ImVec4(ImGui::ColorConvertU32ToFloat4(color));
}

ColorRGBA ImGuiWrapper::from_ImVec4(const ImVec4& color)
{
    return { color.x, color.y, color.z, color.w };
}

template <typename T, typename Func> 
static bool input_optional(std::optional<T> &v, Func& f, std::function<bool(const T&)> is_default, const T& def_val)
{
    if (v.has_value()) {
        if (f(*v)) {
            if (is_default(*v)) v.reset();
            return true;
        }
    } else {
        T val = def_val;
        if (f(val)) {
            if (!is_default(val)) v = val;
            return true;
        }
    }
    return false;
}

bool ImGuiWrapper::input_optional_int(const char *        label,
                                      std::optional<int>& v,
                                      int                 step,
                                      int                 step_fast,
                                      ImGuiInputTextFlags flags,
                                      int                 def_val)
{
    auto func = [&](int &value) {
        return ImGui::InputInt(label, &value, step, step_fast, flags);
    };
    std::function<bool(const int &)> is_default = 
        [def_val](const int &value) -> bool { return value == def_val; };
    return input_optional(v, func, is_default, def_val);
}

bool ImGuiWrapper::input_optional_float(const char *          label,
                                        std::optional<float> &v,
                                        float                 step,
                                        float                 step_fast,
                                        const char *          format,
                                        ImGuiInputTextFlags   flags,
                                        float                 def_val)
{
    auto func = [&](float &value) {
        return ImGui::InputFloat(label, &value, step, step_fast, format, flags);
    };
    std::function<bool(const float &)> is_default =
        [def_val](const float &value) -> bool {
        return std::fabs(value-def_val) <= std::numeric_limits<float>::epsilon();
    };
    return input_optional(v, func, is_default, def_val);
}

bool ImGuiWrapper::drag_optional_float(const char *          label,
                                       std::optional<float> &v,
                                       float                 v_speed,
                                       float                 v_min,
                                       float                 v_max,
                                       const char *          format,
                                       float                 power,
                                       float                 def_val)
{
    auto func = [&](float &value) {
        return ImGui::DragFloat(label, &value, v_speed, v_min, v_max, format, power);
    };
    std::function<bool(const float &)> is_default =
        [def_val](const float &value) -> bool {
        return std::fabs(value-def_val) <= std::numeric_limits<float>::epsilon();
    };
    return input_optional(v, func, is_default, def_val);
}

bool ImGuiWrapper::slider_optional_float(const char           *label,
                                         std::optional<float> &v,
                                         float                 v_min,
                                         float                 v_max,
                                         const char           *format,
                                         float                 power,
                                         bool                  clamp,
                                         const wxString       &tooltip,
                                         bool                  show_edit_btn,
                                         float                 def_val)
{
    auto func = [&](float &value) {
        return slider_float(label, &value, v_min, v_max, format, power, clamp, tooltip, show_edit_btn);
    };
    std::function<bool(const float &)> is_default =
        [def_val](const float &value) -> bool {
        return std::fabs(value - def_val) <= std::numeric_limits<float>::epsilon();
    };
    return input_optional(v, func, is_default, def_val);
}

bool ImGuiWrapper::slider_optional_int(const char         *label,
                                       std::optional<int> &v,
                                       int                 v_min,
                                       int                 v_max,
                                       const char         *format,
                                       float               power,
                                       bool                clamp,
                                       const wxString     &tooltip,
                                       bool                show_edit_btn,
                                       int                 def_val)
{
    std::optional<float> val;
    if (v.has_value()) val = static_cast<float>(*v);
    auto func = [&](float &value) {
        return slider_float(label, &value, v_min, v_max, format, power, clamp, tooltip, show_edit_btn);
    };
    std::function<bool(const float &)> is_default =
        [def_val](const float &value) -> bool {
        return std::fabs(value - def_val) < 0.9f;
    };

    float default_value = static_cast<float>(def_val);
    if (input_optional(val, func, is_default, default_value)) {
        if (val.has_value())
            v = static_cast<int>(std::round(*val));
        else
            v.reset(); 
        return true;
    } else return false;
}

std::optional<ImVec2> ImGuiWrapper::change_window_position(const char *window_name, bool try_to_fix) {
    ImGuiWindow *window = ImGui::FindWindowByName(window_name);
    // is window just created
    if (window == NULL)
        return {};

    // position of window on screen
    ImVec2 position = window->Pos;
    ImVec2 size     = window->SizeFull;

    // screen size
    ImVec2 screen = ImGui::GetMainViewport()->Size;

    std::optional<ImVec2> output_window_offset;
    if (position.x < 0) {
        if (position.y < 0)
            // top left 
            output_window_offset = ImVec2(0, 0); 
        else
            // only left
            output_window_offset = ImVec2(0, position.y); 
    } else if (position.y < 0) {
        // only top
        output_window_offset = ImVec2(position.x, 0); 
    } else if (screen.x < (position.x + size.x)) {
        if (screen.y < (position.y + size.y))
            // right bottom
            output_window_offset = ImVec2(screen.x - size.x, screen.y - size.y);
        else
            // only right
            output_window_offset = ImVec2(screen.x - size.x, position.y);
    } else if (screen.y < (position.y + size.y)) {
        // only bottom
        output_window_offset = ImVec2(position.x, screen.y - size.y);
    }

    if (!try_to_fix && output_window_offset.has_value())
        output_window_offset = ImVec2(-1, -1); // Put on default position

    return output_window_offset;
}

void ImGuiWrapper::left_inputs() { 
    ImGui::ClearActiveID(); 
}

std::string ImGuiWrapper::trunc(const std::string &text,
                                float              width,
                                const char *       tail)
{
    float text_width = ImGui::CalcTextSize(text.c_str()).x;
    if (text_width < width) return text;
    float tail_width = ImGui::CalcTextSize(tail).x;
    assert(width > tail_width);
    if (width <= tail_width) return "Error: Can't add tail and not be under wanted width.";
    float allowed_width = width - tail_width;
    
    // guess approx count of letter
    float average_letter_width = calc_text_size(std::string_view("n")).x; // average letter width
    unsigned count_letter  = static_cast<unsigned>(allowed_width / average_letter_width);

    std::string_view text_ = text;
    std::string_view result_text = text_.substr(0, count_letter);
    text_width = calc_text_size(result_text).x;
    if (text_width < allowed_width) {
        // increase letter count
        while (count_letter < text.length()) {
            ++count_letter;
            std::string_view act_text = text_.substr(0, count_letter);
            text_width = calc_text_size(act_text).x;
            if (text_width > allowed_width) break;
            result_text = act_text;
        }
    } else {
        // decrease letter count
        while (count_letter > 1) {
            --count_letter;
            result_text = text_.substr(0, count_letter);
            text_width  = calc_text_size(result_text).x;
            if (text_width < allowed_width) break;            
        } 
    }
    return std::string(result_text) + tail;
}

void ImGuiWrapper::escape_double_hash(std::string &text)
{
    // add space between hashes
    const std::string search  = "##";
    const std::string replace = "# #";
    size_t pos = 0;
    while ((pos = text.find(search, pos)) != std::string::npos) 
        text.replace(pos, search.length(), replace);
}

ImVec2 ImGuiWrapper::suggest_location(const ImVec2 &dialog_size,
                                      const Slic3r::Polygon &interest,
                                      const ImVec2 &canvas_size)
{
    // IMPROVE 1: do not select place over menu
    // BoundingBox top_menu;
    // GLGizmosManager &gizmo_mng = canvas->get_gizmos_manager();
    // BoundingBox      side_menu; // gizmo_mng.get_size();
    // BoundingBox left_bottom_menu; // is permanent?
    // NotificationManager *notify_mng = plater->get_notification_manager();
    // BoundingBox          notifications; // notify_mng->get_size();
    // m_window_width, m_window_height + position

    // IMPROVE 2: use polygon of interest not only bounding box
    BoundingBox bb(interest.points);
    Point       center = bb.center(); // interest.centroid();

    // area size
    Point window_center(canvas_size.x / 2, canvas_size.y / 2);

    // mov on side
    Point bb_half_size = (bb.max - bb.min) / 2 + Point(1,1);
    Point diff_center  = window_center - center;
    Vec2d diff_norm(diff_center.x() / (double) bb_half_size.x(),
                    diff_center.y() / (double) bb_half_size.y());
    if (diff_norm.x() > 1.) diff_norm.x() = 1.;
    if (diff_norm.x() < -1.) diff_norm.x() = -1.;
    if (diff_norm.y() > 1.) diff_norm.y() = 1.;
    if (diff_norm.y() < -1.) diff_norm.y() = -1.;

    Vec2d abs_diff(abs(diff_norm.x()), abs(diff_norm.y()));
    if (abs_diff.x() < 1. && abs_diff.y() < 1.) {
        if (abs_diff.x() > abs_diff.y())
            diff_norm.x() = (diff_norm.x() < 0.) ? (-1.) : 1.;
        else
            diff_norm.y() = (diff_norm.y() < 0.) ? (-1.) : 1.;
    }

    Point half_dialog_size(dialog_size.x / 2., dialog_size.y / 2.);
    Point move_size       = bb_half_size + half_dialog_size;
    Point offseted_center = center - half_dialog_size;
    Vec2d offset(offseted_center.x() + diff_norm.x() * move_size.x(),
                 offseted_center.y() + diff_norm.y() * move_size.y());

    // move offset close to center
    Points window_polygon = {offset.cast<coord_t>(),
                             Point(offset.x(), offset.y() + dialog_size.y),
                             Point(offset.x() + dialog_size.x,
                                   offset.y() + dialog_size.y),
                             Point(offset.x() + dialog_size.x, offset.y())};
    // check that position by Bounding box is not intersecting
    assert(Slic3r::intersection(interest, Polygon(window_polygon)).empty());

    double allowed_space = 10; // in px
    double allowed_space_sq = allowed_space * allowed_space;
    Vec2d  move_vec         = (center - (offset.cast<coord_t>() + half_dialog_size))
                         .cast<double>();    
    Vec2d result_move(0, 0);
    do {
        move_vec             = move_vec / 2.;
        Point  move_point    = (move_vec + result_move).cast<coord_t>();
        Points moved_polygon = window_polygon; // copy
        for (Point &p : moved_polygon) p += move_point;
        if (Slic3r::intersection(interest, Polygon(moved_polygon)).empty())
            result_move += move_vec;
        
    } while (move_vec.squaredNorm() >= allowed_space_sq);
    offset += result_move;

    return ImVec2(offset.x(), offset.y());
}

void ImGuiWrapper::draw(
    const Polygon &polygon,
    ImDrawList *   draw_list /* = ImGui::GetOverlayDrawList()*/,
    ImU32          color     /* = ImGui::GetColorU32(COL_ORANGE_LIGHT)*/,
    float          thickness /* = 3.f*/)
{
    // minimal one line consist of 2 points
    if (polygon.size() < 2) return;
    // need a place to draw
    if (draw_list == nullptr) return;

    const Point *prev_point = &polygon.points.back();
    for (const Point &point : polygon.points) {
        ImVec2 p1(prev_point->x(), prev_point->y());
        ImVec2 p2(point.x(), point.y());
        draw_list->AddLine(p1, p2, color, thickness);
        prev_point = &point;
    }
}

void ImGuiWrapper::draw_cross_hair(const ImVec2 &position, float radius, ImU32 color, int num_segments, float thickness) {
    auto draw_list = ImGui::GetOverlayDrawList();
    draw_list->AddCircle(position, radius, color, num_segments, thickness);
    auto dirs = {ImVec2{0, 1}, ImVec2{1, 0}, ImVec2{0, -1}, ImVec2{-1, 0}};
    for (const ImVec2 &dir : dirs) {
        ImVec2 start(position.x + dir.x * 0.5 * radius, position.y + dir.y * 0.5 * radius);
        ImVec2 end(position.x + dir.x * 1.5 * radius, position.y + dir.y * 1.5 * radius);
        draw_list->AddLine(start, end, color, thickness);
    }
}

bool ImGuiWrapper::contain_all_glyphs(const ImFont      *font,
                                     const std::string &text)
{
    if (font == nullptr) return false;
    if (!font->IsLoaded()) return false;
    const ImFontConfig *fc = font->ConfigData;
    if (fc == nullptr) return false;
    if (text.empty()) return true;
    return is_chars_in_ranges(fc->GlyphRanges, text.c_str());
}

bool ImGuiWrapper::is_char_in_ranges(const ImWchar *ranges,
                                     unsigned int   letter)
{
    for (const ImWchar *range = ranges; range[0] && range[1]; range += 2) {
        ImWchar from = range[0];
        ImWchar to   = range[1];
        if (from <= letter && letter <= to) return true;
        if (letter < to) return false; // ranges should be sorted
    }
    return false;
};

bool ImGuiWrapper::is_chars_in_ranges(const ImWchar *ranges,
                                     const char    *chars_ptr)
{
    while (*chars_ptr) {
        unsigned int c = 0;
        // UTF-8 to 32-bit character need imgui_internal
        int c_len = ImTextCharFromUtf8(&c, chars_ptr, NULL);
        chars_ptr += c_len;
        if (c_len == 0) break;
        if (!is_char_in_ranges(ranges, c)) return false;
    }
    return true;
}


#ifdef __APPLE__
static const ImWchar ranges_keyboard_shortcuts[] =
{
    0x21E7, 0x21E7, // OSX Shift Key symbol
    0x2318, 0x2318, // OSX Command Key symbol
    0x2325, 0x2325, // OSX Option Key symbol
    0,
};
#endif // __APPLE__


std::vector<unsigned char> ImGuiWrapper::load_svg(const std::string& bitmap_name, unsigned target_width, unsigned target_height, unsigned *outwidth, unsigned *outheight)
{
    std::vector<unsigned char> empty_vector;

    NSVGimage* image = BitmapCache::nsvgParseFromFileWithReplace(Slic3r::var(bitmap_name + ".svg").c_str(), "px", 96.0f, { { "\"#808080\"", "\"#FFFFFF\"" } });
    if (image == nullptr)
        return empty_vector;

    float svg_scale = target_height != 0 ?
        (float)target_height / image->height : target_width != 0 ?
        (float)target_width / image->width : 1;

    int   width = (int)(svg_scale * image->width + 0.5f);
    int   height = (int)(svg_scale * image->height + 0.5f);
    int   n_pixels = width * height;
    if (n_pixels <= 0) {
        ::nsvgDelete(image);
        return empty_vector;
    }

    NSVGrasterizer* rast = ::nsvgCreateRasterizer();
    if (rast == nullptr) {
        ::nsvgDelete(image);
        return empty_vector;
    }

    std::vector<unsigned char> data(n_pixels * 4, 0);
    ::nsvgRasterize(rast, image, 0, 0, svg_scale, data.data(), width, height, width * 4);
    ::nsvgDeleteRasterizer(rast);
    ::nsvgDelete(image);

    *outwidth = width;
    *outheight = height;

    return data;
}

//BBS
static bool m_is_dark_mode = false;

void ImGuiWrapper::on_change_color_mode(bool is_dark)
{
    m_is_dark_mode = is_dark;
}

void ImGuiWrapper::push_toolbar_style(const float scale)
{
    if (m_is_dark_mode) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 10.0f) * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f) * scale);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.88f));                                        // 1
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGuiWrapper::COL_WINDOW_BG_DARK);                                   // 2
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImGuiWrapper::COL_TITLE_BG);                                          // 3
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImGuiWrapper::COL_TITLE_BG);                                    // 4
        ImGui::PushStyleColor(ImGuiCol_Separator, ImGuiWrapper::COL_SEPARATOR_DARK);                                  // 5
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(62 / 255.0f, 62 / 255.0f, 69 / 255.0f, 1.00f));                 // 6
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(73 / 255.0f, 73 / 255.0f, 78 / 255.0f, 1.00f));          // 7
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(73 / 255.0f, 73 / 255.0f, 78 / 255.0f, 1.00f));           // 8
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(84 / 255.0f, 84 / 255.0f, 90 / 255.0f, 1.00f));         // 9
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(62 / 255.0f, 62 / 255.0f, 69 / 255.0f, 1.00f));          // 10
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));             // 11
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(43 / 255.0f, 64 / 255.0f, 54 / 255.0f, 1.00f));         // 12
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));                                // 13
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));                            // 14
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));                     // 15
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));                      // 16
    }
    else {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 10.0f) * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f) * scale);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(50 / 255.0f, 58 / 255.0f, 61 / 255.0f, 1.00f));       // 1
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGuiWrapper::COL_WINDOW_BG);          // 2
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImGuiWrapper::COL_TITLE_BG);            // 3
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImGuiWrapper::COL_TITLE_BG);      // 4
        ImGui::PushStyleColor(ImGuiCol_Separator, ImGuiWrapper::COL_SEPARATOR);         // 5
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));     // 6
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGuiWrapper::COL_HOVER);         // 7
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 1.00f)); // 8
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(172 / 255.0f, 172 / 255.0f, 172 / 255.0f, 1.00f));                        // 9
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 1.00f));  // 10
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));        // 11
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, COL_GREEN_LIGHT);                                     // 12
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));//13
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
    }
}

void ImGuiWrapper::pop_toolbar_style()
{
    // size in push toolbar style
    ImGui::PopStyleColor(16);
    ImGui::PopStyleVar(6);
}

void ImGuiWrapper::push_menu_style(const float scale)
{
    if (m_is_dark_mode) {
        ImGuiWrapper::push_toolbar_style(scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f) * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BG_DARK);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
    }
    else {
        ImGuiWrapper::push_toolbar_style(scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f) * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BG);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.00f, 0.59f, 0.53f, 1.0f));
    }
}
void ImGuiWrapper::pop_menu_style()
{
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
    ImGuiWrapper::pop_toolbar_style();
}

void ImGuiWrapper::push_common_window_style(const float scale) {
    if (m_is_dark_mode) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 10.0f) * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, ImVec2(0.05f, 0.50f) * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.88f));                                   // 1
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGuiWrapper::COL_WINDOW_BG_DARK);                              // 2
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(54 / 255.0f, 54 / 255.0f, 60 / 255.0f, 1.00f));           // 3
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(54 / 255.0f, 54 / 255.0f, 60 / 255.0f, 1.00f));     // 4
        ImGui::PushStyleColor(ImGuiCol_Separator, ImGuiWrapper::COL_SEPARATOR_DARK);                             // 5
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));                              // 6
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));                       // 7
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));                        // 8
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(84 / 255.0f, 84 / 255.0f, 90 / 255.0f, 1.00f));    // 9
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(62 / 255.0f, 62 / 255.0f, 69 / 255.0f, 1.00f));     // 10
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));        // 11
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));                           // 12
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(43 / 255.0f, 64 / 255.0f, 54 / 255.0f, 1.00f));    // 13
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));                       // 14
    }
    else {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 10.0f) * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, ImVec2(0.05f, 0.50f) * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(38 / 255.0f, 46 / 255.0f, 48 / 255.0f, 1.00f));              // 1
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));                            // 2
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(245 / 255.0f, 245 / 255.0f, 245 / 255.0f, 1.00f));        // 3
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(245 / 255.0f, 245 / 255.0f, 245 / 255.0f, 1.00f));  // 4
        ImGui::PushStyleColor(ImGuiCol_Separator, ImGuiWrapper::COL_SEPARATOR);                                  // 5
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));                              // 6
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));                       // 7
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));                        // 8
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 1.00f)); // 9
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 1.00f));  // 10
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));        // 11
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));                           // 12
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImGuiWrapper::COL_GREEN_LIGHT);                           // 13
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));                       // 14
    }
}

void ImGuiWrapper::pop_common_window_style() {
    ImGui::PopStyleColor(14);
    ImGui::PopStyleVar(5);
}

void ImGuiWrapper::push_confirm_button_style() {
    if (m_is_dark_mode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f / 255.f, 150.f / 255.f, 136.f / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f / 255.f, 150.f / 255.f, 136.f / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, to_ImVec4(decode_color_to_float_array("#267E73")));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(27.f / 255.f, 136.f / 255.f, 68.f / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.f, 1.f, 1.f, 0.88f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 0.88f));
    }
    else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f / 255.f, 150.f / 255.f, 136.f / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f / 255.f, 150.f / 255.f, 136.f / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, to_ImVec4(decode_color_to_float_array("#26A69A")));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(27.f / 255.f, 136.f / 255.f, 68.f / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.f, 1.f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
    }
}

void ImGuiWrapper::pop_confirm_button_style() {
    ImGui::PopStyleColor(6);
}

void ImGuiWrapper::push_cancel_button_style() {
    if (m_is_dark_mode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.64f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(73 / 255.f, 73 / 255.f, 78 / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(129 / 255.f, 129 / 255.f, 131 / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.f, 1.f, 1.f, 0.64f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 0.64f));
    }
    else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.f, 1.f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(38 / 255.f, 46 / 255.f, 48 / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(238.f / 255.f, 238.f / 255.f, 238.f / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(206.f / 255.f, 206.f / 255.f, 206.f / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.f, 0.f, 0.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(38.f / 255.0f, 46.f / 255.0f, 48.f / 255.0f, 1.00f));
    }
}

void ImGuiWrapper::pop_cancel_button_style() {
    ImGui::PopStyleColor(6);
}

void ImGuiWrapper::push_button_disable_style() {
    if (m_is_dark_mode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(54 / 255.f, 54 / 255.f, 60 / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(54 / 255.f, 54 / 255.f, 60 / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 0.4f));
    }
    else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(206.f / 255.f, 206.f / 255.f, 206.f / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(206.f / 255.f, 206.f / 255.f, 206.f / 255.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
    }
}

void ImGuiWrapper::pop_button_disable_style() {
    ImGui::PopStyleColor(3);
}

void ImGuiWrapper::push_combo_style(const float scale)
{
    if (m_is_dark_mode) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BG_DARK);
        ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.f, 150.f / 255.f, 136.f / 255.f, 0.6f));  // ORCA hovered item border color
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.f, 0.f, 0.f, 0.f});                            // ORCA hovered item background color
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, COL_ORCA);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.f, 150.f / 255.f, 136.f / 255.f, 0.25f));       // ORCA active item background color
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImGuiWrapper::COL_WINDOW_BG_DARK);
        ImGui::PushStyleColor(ImGuiCol_Button, {1.00f, 1.00f, 1.00f, 0.0f});
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BG);
        ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.f, 150.f / 255.f, 136.f / 255.f, 0.6f));  // ORCA hovered item border color
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.f, 0.f, 0.f, 0.f});                            // ORCA hovered item background color
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, COL_ORCA);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.f, 150.f / 255.f, 136.f / 255.f, 0.25f));       // ORCA active item background color
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImGuiWrapper::COL_WINDOW_BG);
        ImGui::PushStyleColor(ImGuiCol_Button, {1.00f, 1.00f, 1.00f, 0.0f});
    }
}

void ImGuiWrapper::pop_combo_style()
{
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(7);
}

void ImGuiWrapper::push_radio_style(const float scale)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.5f, 1.5f) * scale); // ORCA ensure icon size stays consistent
    if (m_is_dark_mode) {
        ImGui::PushStyleColor(ImGuiCol_CheckMark, to_ImVec4(decode_color_to_float_array("#009688"))); // ORCA use orca color for radio buttons
        ImGui::PushStyleColor(ImGuiCol_Border   , to_ImVec4(decode_color_to_float_array("#949494"))); // ORCA match border color
    } else {
        ImGui::PushStyleColor(ImGuiCol_CheckMark, to_ImVec4(decode_color_to_float_array("#009688"))); // ORCA use orca color for radio buttons
        ImGui::PushStyleColor(ImGuiCol_Border   , to_ImVec4(decode_color_to_float_array("#7C8282"))); // ORCA match border color
    }
}

void ImGuiWrapper::pop_radio_style()
{
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(1);
}

void ImGuiWrapper::init_font(bool compress)
{
    destroy_font();

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // Create ranges of characters from m_glyph_ranges, possibly adding some OS specific special characters.
    ImVector<ImWchar> ranges;
    ImVector<ImWchar> basic_ranges;
    ImFontAtlas::GlyphRangesBuilder builder;
    builder.AddRanges(m_glyph_ranges);
    builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
#ifdef __APPLE__
    if (m_font_cjk)
        // Apple keyboard shortcuts are only contained in the CJK fonts.
        builder.AddRanges(ranges_keyboard_shortcuts);
#endif
    builder.BuildRanges(&ranges); // Build the final result (ordered ranges with all the unique characters submitted)

    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    ImFontConfig cfg = ImFontConfig();
    cfg.OversampleH = cfg.OversampleV = 1;
    //FIXME replace with io.Fonts->AddFontFromMemoryTTF(buf_decompressed_data, (int)buf_decompressed_size, m_font_size, nullptr, ranges.Data);
    //https://github.com/ocornut/imgui/issues/220

    // Orca: temp fix for Korean font
    auto font_name_regular = "HarmonyOS_Sans_SC_Regular.ttf";
    auto font_name_bold = "HarmonyOS_Sans_SC_Bold.ttf";
    if(m_glyph_ranges == ImGui::GetIO().Fonts->GetGlyphRangesKorean()) {
        font_name_regular = "NanumGothic-Regular.ttf";
        font_name_bold = "NanumGothic-Bold.ttf";
    }
    default_font = io.Fonts->AddFontFromFileTTF((Slic3r::resources_dir() + "/fonts/" + font_name_regular).c_str(), m_font_size, &cfg, ranges.Data);
    if (default_font == nullptr) {
        default_font = io.Fonts->AddFontDefault();
        if (default_font == nullptr) {
            throw Slic3r::RuntimeError("ImGui: Could not load deafult font");
        }
    }

    bold_font        = io.Fonts->AddFontFromFileTTF((Slic3r::resources_dir() + "/fonts/" + font_name_bold).c_str(), m_font_size, &cfg, ranges.Data);
    if (bold_font == nullptr) {
        bold_font = io.Fonts->AddFontDefault();
        if (bold_font == nullptr) { throw Slic3r::RuntimeError("ImGui: Could not load deafult font"); }
    }

#ifdef _WIN32
    // Render the text a bit larger (see GLCanvas3D::_resize() and issue #3401), but only if the scale factor
    // for the Display is greater than 300%.
    if (wxGetApp().em_unit() > 30) {
        default_font->Scale = 1.5f;
        bold_font->Scale    = 1.5f;
    }
#endif

#ifdef __APPLE__
    ImFontConfig config;
    config.MergeMode = true;
    if (! m_font_cjk) {
        // Apple keyboard shortcuts are only contained in the CJK fonts.
        [[maybe_unused]]ImFont *font_cjk = io.Fonts->AddFontFromFileTTF((Slic3r::resources_dir() + "/fonts/HarmonyOS_Sans_SC_Regular.ttf").c_str(), m_font_size, &config, ranges_keyboard_shortcuts);
        assert(font_cjk != nullptr);
    }
#endif

    float font_scale = m_font_size/15;
    int icon_sz = lround(16 * font_scale); // default size of icon is 16 px

    int rect_id = io.Fonts->CustomRects.Size;  // id of the rectangle added next
    // add rectangles for the icons to the font atlas
    for (auto& icon : font_icons) {
        m_custom_glyph_rects_ids[icon.first] =
        io.Fonts->AddCustomRectFontGlyph(default_font, icon.first, icon_sz, icon_sz, 3.0 * font_scale + icon_sz);
    }
    for (auto& icon : font_icons_large) {
        m_custom_glyph_rects_ids[icon.first] =
        io.Fonts->AddCustomRectFontGlyph(default_font, icon.first, icon_sz * 2, icon_sz * 2, 3.0 * font_scale + icon_sz * 2);
    }
    for (auto& icon : font_icons_extra_large) {
        m_custom_glyph_rects_ids[icon.first] =
        io.Fonts->AddCustomRectFontGlyph(default_font, icon.first, icon_sz * 4, icon_sz * 4, 3.0 * font_scale + icon_sz * 4);
    }

    // Build texture atlas
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);   // Load as RGBA 32-bits (75% of the memory is wasted, but default font is so small) because it is more likely to be compatible with user's existing shaders. If your ImTextureId represent a higher-level concept than just a GL texture id, consider calling GetTexDataAsAlpha8() instead to save on GPU memory.
    BOOST_LOG_TRIVIAL(trace) << "Build default font texture done. width: " << width << ", height: " << height;

    auto load_icon_from_svg = [this, &io, pixels, width, &rect_id](const std::pair<const wchar_t, std::string> icon, int icon_sz) {
        if (const ImFontAtlas::CustomRect* rect = io.Fonts->GetCustomRectByIndex(rect_id)) {
            assert(rect->Width == icon_sz);
            assert(rect->Height == icon_sz);
            unsigned                   outwidth, outheight;
            std::vector<unsigned char> raw_data = load_svg(icon.second, icon_sz, icon_sz, &outwidth, &outheight);
            if (!raw_data.empty()) {
                const ImU32* pIn = (ImU32*)raw_data.data();
                for (unsigned y = 0; y < outheight; y++) {
                    ImU32* pOut = (ImU32*)pixels + (rect->Y + y) * width + (rect->X);
                    for (unsigned x = 0; x < outwidth; x++)
                        *pOut++ = *pIn++;
                }
            }
        }
        rect_id++;
    };

    // Fill rectangles from the SVG-icons
    for (auto icon : font_icons) {
        load_icon_from_svg(icon, icon_sz);
    }

    icon_sz *= 2; // default size of large icon is 32 px
    for (auto icon : font_icons_large) {
        load_icon_from_svg(icon, icon_sz);
    }

    icon_sz *= 2; // default size of extra large icon is 64 px
    for (auto icon : font_icons_extra_large) {
        load_icon_from_svg(icon, icon_sz);
    }

    // Upload texture to graphics system
    GLint last_texture;
    glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture));
    glsafe(::glGenTextures(1, &m_font_texture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_font_texture));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
    if (compress && GLEW_EXT_texture_compression_s3tc)
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels));
    else
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels));

    // Store our identifier
    io.Fonts->TexID = (ImTextureID)(intptr_t)m_font_texture;

    // Restore state
    glsafe(::glBindTexture(GL_TEXTURE_2D, last_texture));
}

void ImGuiWrapper::load_fonts_texture()
{
    //if (m_font_another_texture == 0) {
    //    ImGuiIO& io = ImGui::GetIO();
    //    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    //    ImFontConfig cfg = ImFontConfig();
    //    cfg.OversampleH = cfg.OversampleV = 1;
    //    std::map<std::string, std::string> sys_fonts_map = get_occt_fonts_maps(); // map<font name, font path>
    //    im_fonts_map.clear();                                                     // map<font name, ImFont*>
    //    BOOST_LOG_TRIVIAL(info) << "init_im_font start";
    //    for (auto sys_font : sys_fonts_map) {
    //        boost::filesystem::path font_path(sys_font.second);
    //        if (!boost::filesystem::exists(font_path)) {
    //            BOOST_LOG_TRIVIAL(trace) << "load font = " << sys_font.first << ", path = " << font_path << " is not exists";
    //            continue;
    //        }
    //        ImFont* im_font = io.Fonts->AddFontFromFileTTF(sys_font.second.c_str(), m_font_size, &cfg, ImGui::GetIO().Fonts->GetGlyphRangesBasic());
    //        if (im_font == nullptr) {
    //            BOOST_LOG_TRIVIAL(trace) << "load font = " << sys_font.first << " failed, path = " << font_path << " is not exists";
    //            continue;
    //        }
    //        im_fonts_map.insert({ sys_font.first, im_font });
    //    }
    //    BOOST_LOG_TRIVIAL(info) << "init_im_font end";

    //    unsigned char* pixels;
    //    int            width, height;
    //    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    //    BOOST_LOG_TRIVIAL(trace) << "Build system fonts texture done. width: " << width << ", height: " << height;

    //    if (m_fonts_names.size() == 0) {
    //        std::vector<std::string> to_delete_fonts;
    //        for (auto im_font : im_fonts_map) {
    //            if (im_font.second->Glyphs.Size < 4) { to_delete_fonts.push_back(im_font.first); }
    //        }
    //        for (auto to_delete_font : to_delete_fonts) {
    //            sys_fonts_map.erase(to_delete_font);
    //            im_fonts_map.erase(to_delete_font);
    //        }
    //        for (auto im_font : im_fonts_map) m_fonts_names.push_back(im_font.first);
    //    }

    //    GLint last_texture;
    //    glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture));
    //    glsafe(::glGenTextures(1, &(m_font_another_texture)));
    //    glsafe(::glBindTexture(GL_TEXTURE_2D, m_font_another_texture));
    //    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    //    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    //    glsafe(::glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));

    //    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels));

    //    // Store our identifier
    //    io.Fonts->TexID = (ImTextureID)(intptr_t)m_font_another_texture;

    //    // Restore state
    //    glsafe(::glBindTexture(GL_TEXTURE_2D, last_texture));
    //}
}

void ImGuiWrapper::init_input()
{
    ImGuiIO& io = ImGui::GetIO();

    // Keyboard mapping. ImGui will use those indices to peek into the io.KeysDown[] array.
    io.KeyMap[ImGuiKey_Tab] = WXK_TAB;
    io.KeyMap[ImGuiKey_LeftArrow] = WXK_LEFT;
    io.KeyMap[ImGuiKey_RightArrow] = WXK_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow] = WXK_UP;
    io.KeyMap[ImGuiKey_DownArrow] = WXK_DOWN;
    io.KeyMap[ImGuiKey_PageUp] = WXK_PAGEUP;
    io.KeyMap[ImGuiKey_PageDown] = WXK_PAGEDOWN;
    io.KeyMap[ImGuiKey_Home] = WXK_HOME;
    io.KeyMap[ImGuiKey_End] = WXK_END;
    io.KeyMap[ImGuiKey_Insert] = WXK_INSERT;
    io.KeyMap[ImGuiKey_Delete] = WXK_DELETE;
    io.KeyMap[ImGuiKey_Backspace] = WXK_BACK;
    io.KeyMap[ImGuiKey_Space] = WXK_SPACE;
    io.KeyMap[ImGuiKey_Enter] = WXK_RETURN;
    io.KeyMap[ImGuiKey_KeyPadEnter] = WXK_NUMPAD_ENTER;
    io.KeyMap[ImGuiKey_Escape] = WXK_ESCAPE;
    io.KeyMap[ImGuiKey_A] = 'A';
    io.KeyMap[ImGuiKey_C] = 'C';
    io.KeyMap[ImGuiKey_V] = 'V';
    io.KeyMap[ImGuiKey_X] = 'X';
    io.KeyMap[ImGuiKey_Y] = 'Y';
    io.KeyMap[ImGuiKey_Z] = 'Z';

    // Don't let imgui special-case Mac, wxWidgets already do that
    io.ConfigMacOSXBehaviors = false;

    // Setup clipboard interaction callbacks
    io.SetClipboardTextFn = clipboard_set;
    io.GetClipboardTextFn = clipboard_get;
    io.ClipboardUserData = this;
}

void ImGuiWrapper::init_style()
{
    ImGuiStyle &style = ImGui::GetStyle();

    auto set_color = [&](ImGuiCol_ entity, ImVec4 color) {
        style.Colors[entity] = color;
    };

    //BBS modify style
    // Window
    style.WindowRounding = 0.0f;
    set_color(ImGuiCol_WindowBg, COL_WINDOW_BACKGROUND);
    set_color(ImGuiCol_TitleBgActive, COL_WINDOW_BACKGROUND);

    // Generics
    set_color(ImGuiCol_FrameBg, COL_GREY_DARK);
    set_color(ImGuiCol_FrameBgHovered, COL_GREY_LIGHT);
    set_color(ImGuiCol_FrameBgActive, COL_GREY_LIGHT);

    // Text selection
    set_color(ImGuiCol_TextSelectedBg, COL_ORANGE_DARK);

    // Buttons
    set_color(ImGuiCol_Button, COL_BUTTON_BACKGROUND);
    set_color(ImGuiCol_ButtonHovered, COL_HOVER);
    set_color(ImGuiCol_ButtonActive, COL_ACTIVE);

    // Checkbox
    set_color(ImGuiCol_CheckMark, COL_BLUE_LIGHT);

    // ComboBox items
    set_color(ImGuiCol_Header, COL_ORANGE_DARK);
    set_color(ImGuiCol_HeaderHovered,	to_ImVec4(to_rgba(ColorRGB::ORCA(), 0.50f))); // ORCA Use orca color for headers
    set_color(ImGuiCol_HeaderActive,	to_ImVec4(to_rgba(ColorRGB::ORCA(), 0.75f)));  // ORCA Use orca color for headers

    // Slider
    set_color(ImGuiCol_SliderGrab,			to_ImVec4(to_rgba(ColorRGB::ORCA(), 0.50f))); // ORCA Use orca color for slider thumbs 
    set_color(ImGuiCol_SliderGrabActive,	to_ImVec4(to_rgba(ColorRGB::ORCA(), 0.75f))); // ORCA Use orca color for slider thumbs 

    // Separator
    set_color(ImGuiCol_Separator, COL_BLUE_LIGHT);

    // Tabs
    set_color(ImGuiCol_Tab, COL_ORANGE_DARK);
    set_color(ImGuiCol_TabHovered, COL_BLUE_LIGHT);
    set_color(ImGuiCol_TabActive, COL_BLUE_LIGHT);
    set_color(ImGuiCol_TabUnfocused, COL_GREY_DARK);
    set_color(ImGuiCol_TabUnfocusedActive, COL_GREY_LIGHT);

    // Scrollbars
    set_color(ImGuiCol_ScrollbarGrab, COL_ORANGE_DARK);
    set_color(ImGuiCol_ScrollbarGrabHovered, COL_ORANGE_LIGHT);
    set_color(ImGuiCol_ScrollbarGrabActive, COL_ORANGE_LIGHT);
}

void ImGuiWrapper::render_draw_data(ImDrawData *draw_data)
{
    if (draw_data == nullptr || draw_data->CmdListsCount == 0)
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("imgui");
    if (shader == nullptr)
        return;

    // Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
    ImGuiIO& io = ImGui::GetIO();
    const int fb_width  = (int)(draw_data->DisplaySize.x * io.DisplayFramebufferScale.x);
    const int fb_height = (int)(draw_data->DisplaySize.y * io.DisplayFramebufferScale.y);
    if (fb_width == 0 || fb_height == 0)
        return;

    GLShaderProgram* curr_shader = wxGetApp().get_current_shader();
    if (curr_shader != nullptr)
        curr_shader->stop_using();

    shader->start_using();

    // We are using the OpenGL fixed pipeline to make the example code simpler to read!
    // Setup render state: alpha-blending enabled, no face culling, no depth testing, scissor enabled, vertex/texcoord/color pointers, polygon fill.
    GLint last_texture;          glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture));
    GLint last_polygon_mode[2];  glsafe(::glGetIntegerv(GL_POLYGON_MODE, last_polygon_mode));
    GLint last_viewport[4];      glsafe(::glGetIntegerv(GL_VIEWPORT, last_viewport));
    GLint last_scissor_box[4];   glsafe(::glGetIntegerv(GL_SCISSOR_BOX, last_scissor_box));
    GLint last_texture_env_mode; glsafe(::glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &last_texture_env_mode));
    glsafe(::glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_SCISSOR_TEST));
    glsafe(::glEnable(GL_TEXTURE_2D));
    glsafe(::glPolygonMode(GL_FRONT_AND_BACK, GL_FILL));
    glsafe(::glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE));

    // Setup viewport, orthographic projection matrix
    // Our visible imgui space lies from draw_data->DisplayPos (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos is (0,0) for single viewport apps.
    glsafe(::glViewport(0, 0, (GLsizei)fb_width, (GLsizei)fb_height));
    const float L = draw_data->DisplayPos.x;
    const float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
    const float T = draw_data->DisplayPos.y;
    const float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;

    Matrix4f ortho_projection;
    ortho_projection <<
        2.0f / (R - L), 0.0f,           0.0f,  (R + L) / (L - R),
        0.0f,           2.0f / (T - B), 0.0f,  (T + B) / (B - T),
        0.0f,           0.0f,           -1.0f, 0.0f,
        0.0f,           0.0f,           0.0f,  1.0f;

    shader->set_uniform("Texture", 0);
    shader->set_uniform("ProjMtx", ortho_projection);

    // Will project scissor/clipping rectangles into framebuffer space
    const ImVec2 clip_off   = draw_data->DisplayPos;       // (0,0) unless using multi-viewports
    const ImVec2 clip_scale = draw_data->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

    // Render command lists
    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        const ImDrawVert* vtx_buffer = cmd_list->VtxBuffer.Data;
        const ImDrawIdx* idx_buffer  = cmd_list->IdxBuffer.Data;
        const GLsizeiptr vtx_buffer_size = (GLsizeiptr)cmd_list->VtxBuffer.Size * (int)sizeof(ImDrawVert);
        const GLsizeiptr idx_buffer_size = (GLsizeiptr)cmd_list->IdxBuffer.Size * (int)sizeof(ImDrawIdx);

        GLuint vbo_id;
        glsafe(::glGenBuffers(1, &vbo_id));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, vbo_id));
        glsafe(::glBufferData(GL_ARRAY_BUFFER, vtx_buffer_size, vtx_buffer, GL_STATIC_DRAW));

        GLuint ibo_id;
        glsafe(::glGenBuffers(1, &ibo_id));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_id));
        glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_buffer_size, idx_buffer, GL_STATIC_DRAW));

        const int position_id = shader->get_attrib_location("Position");
        if (position_id != -1) {
            glsafe(::glVertexAttribPointer(position_id, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (const void*)IM_OFFSETOF(ImDrawVert, pos)));
            glsafe(::glEnableVertexAttribArray(position_id));
        }
        const int uv_id = shader->get_attrib_location("UV");
        if (uv_id != -1) {
            glsafe(::glVertexAttribPointer(uv_id, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (const void*)IM_OFFSETOF(ImDrawVert, uv)));
            glsafe(::glEnableVertexAttribArray(uv_id));
        }
        const int color_id = shader->get_attrib_location("Color");
        if (color_id != -1) {
            glsafe(::glVertexAttribPointer(color_id, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (const void*)IM_OFFSETOF(ImDrawVert, col)));
            glsafe(::glEnableVertexAttribArray(color_id));
        }

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; ++cmd_i) {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback)
                // User callback (registered via ImDrawList::AddCallback)
                pcmd->UserCallback(cmd_list, pcmd);
            else {
                // Project scissor/clipping rectangles into framebuffer space
                const ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x, (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
                const ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x, (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                // Apply scissor/clipping rectangle (Y is inverted in OpenGL)
                glsafe(::glScissor((int)clip_min.x, (int)(fb_height - clip_max.y), (int)(clip_max.x - clip_min.x), (int)(clip_max.y - clip_min.y)));

                // Bind texture, Draw
                glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)pcmd->GetTexID()));
                glsafe(::glDrawElements(GL_TRIANGLES, (GLsizei)pcmd->ElemCount, sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, (void*)(intptr_t)(pcmd->IdxOffset * sizeof(ImDrawIdx))));
            }
        }

        if (position_id != -1)
            glsafe(::glDisableVertexAttribArray(position_id));
        if (uv_id != -1)
            glsafe(::glDisableVertexAttribArray(uv_id));
        if (color_id != -1)
            glsafe(::glDisableVertexAttribArray(color_id));

        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

        glsafe(::glDeleteBuffers(1, &ibo_id));
        glsafe(::glDeleteBuffers(1, &vbo_id));
    }

    // Restore modified state
    glsafe(::glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, last_texture_env_mode));
    glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint)last_texture));
    glsafe(::glPopAttrib());
    glsafe(::glPolygonMode(GL_FRONT, (GLenum)last_polygon_mode[0]);
    glsafe(::glPolygonMode(GL_BACK,  (GLenum)last_polygon_mode[1])));
    glsafe(::glViewport(last_viewport[0], last_viewport[1], (GLsizei)last_viewport[2], (GLsizei)last_viewport[3]));
    glsafe(::glScissor(last_scissor_box[0], last_scissor_box[1], (GLsizei)last_scissor_box[2], (GLsizei)last_scissor_box[3]));

    shader->stop_using();

    if (curr_shader != nullptr)
        curr_shader->start_using();
}

bool ImGuiWrapper::display_initialized() const
{
    const ImGuiIO& io = ImGui::GetIO();
    return io.DisplaySize.x >= 0.0f && io.DisplaySize.y >= 0.0f;
}

void ImGuiWrapper::destroy_font()
{
    if (m_font_texture != 0) {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->TexID = 0;
        glsafe(::glDeleteTextures(1, &m_font_texture));
        m_font_texture = 0;
    }
}

void ImGuiWrapper::destroy_fonts_texture() {
    //if (m_font_another_texture != 0) {
    //    if (m_new_frame_open) {
    //        render();
    //    }
    //    init_font(true);
    //    glsafe(::glDeleteTextures(1, &m_font_another_texture));
    //    m_font_another_texture = 0;
    //    if (!m_new_frame_open) {
    //        new_frame();
    //    }
    //}
}

const char* ImGuiWrapper::clipboard_get(void* user_data)
{
    ImGuiWrapper* self = reinterpret_cast<ImGuiWrapper*>(user_data);

    const char* res = "";

    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_TEXT) || wxTheClipboard->IsSupported(wxDF_UNICODETEXT)) {
            wxTextDataObject data;
            wxTheClipboard->GetData(data);

            if (data.GetTextLength() > 0) {
                self->m_clipboard_text = into_u8(data.GetText());
                res = self->m_clipboard_text.c_str();
            }
        }

        wxTheClipboard->Close();
    }

    return res;
}

void ImGuiWrapper::clipboard_set(void* /* user_data */, const char* text)
{
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text)));   // object owned by the clipboard
        wxTheClipboard->Close();
    }
}

std::tuple<ImVec2, bool>  ImGuiWrapper::calculate_filament_group_text_size(const std::string& filament_type)
{
    ImVec2             text_size = ImGui::CalcTextSize(filament_type.c_str());
    float         four_word_width = ImGui::CalcTextSize("ABCD").x;

    float wrap_width = four_word_width;
    float line_height = ImGui::GetTextLineHeight();

    bool is_multiline = text_size.x > wrap_width;
    int line_count = std::ceil(text_size.x / wrap_width);
    float text_height = line_count * line_height;

    float final_width = is_multiline ? wrap_width : text_size.x;
    float final_height = line_count * line_height;

    return { { final_width,final_height },is_multiline };
}

void ImGuiWrapper::filament_group(const std::string& filament_type, const char* hex_color, unsigned char filament_id, float align_width)
{
    //ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    std::string id = std::to_string(static_cast<unsigned int> (filament_id + 1));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    static ImTextureID transparent;
    ImVec2             text_size = ImGui::CalcTextSize(filament_type.c_str());
    // BBS image sizing based on text width (DPI scaling)
    float         img_width = ImGui::CalcTextSize("ABC").x;
    ImVec2        img_size = { img_width, img_width };
    ImVec2        id_text_size = this->calc_text_size(id);
    unsigned char rgba[4];
    rgba[3] = 0xff;
    Slic3r::GUI::BitmapCache::parse_color4(hex_color, rgba);
    std::string svg_path = "/images/outlined_rect.svg";
    if (rgba[3] == 0x00) {
        svg_path = "/images/outlined_rect_transparent.svg";
    }
    BitmapCache::load_from_svg_file_change_color(Slic3r::resources_dir() + svg_path, img_size.x, img_size.y, transparent, hex_color);
    ImGui::BeginGroup();
    {
        ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
        draw_list->AddImage(transparent, cursor_pos, { cursor_pos.x + img_size.x, cursor_pos.y + img_size.y }, { 0, 0 }, { 1, 1 }, ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 1.f)));
        // image border test
        // draw_list->AddRect(cursor_pos, {cursor_pos.x + img_size.x, cursor_pos.y + img_size.y}, IM_COL32(0, 0, 0, 255));
        ImVec2 current_cursor = ImGui::GetCursorPos();
        ImGui::SetCursorPos({ current_cursor.x + (img_size.x - id_text_size.x) * 0.5f, current_cursor.y + (img_size.y - id_text_size.y) * 0.5f });

        float gray = 0.299 * rgba[0] + 0.587 * rgba[1] + 0.114 * rgba[2];
        ImVec4 text_color = gray < 80 ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0, 0, 0, 1.0f);
        this->text_colored(text_color, id.c_str());

        auto wrapped_text_info = calculate_filament_group_text_size(filament_type);
        ImVec2 wrapped_text_size = std::get<0>(wrapped_text_info);
        bool is_multiline = std::get<1>(wrapped_text_info);

        float text_y_offset = 4.f;
        float text_x_offset = is_multiline ? (img_size.x - wrapped_text_size.x) * 0.5f + 2.f : (img_size.x - wrapped_text_size.x) * 0.5f + 2.f;

        auto cursor_x_before_text = ImGui::GetCursorPosX();
        current_cursor = ImGui::GetCursorPos();
        ImGui::SetCursorPos({
            current_cursor.x + text_x_offset,
            current_cursor.y + text_y_offset
            });

        if (is_multiline) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapped_text_size.x);
        }
        this->text(filament_type);
        if (is_multiline) {
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy(ImVec2(align_width, 0));
        ImGui::EndGroup();
    }
    //ImGui::PopStyleVar(1);
}

void ImGuiWrapper::sub_title(const std::string &label)
{
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    text_colored(ImVec4(1.0f, 1.0f, 1.0f, 0.5f), label);
    ImGui::SameLine();
    ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
    float available_width = ImGui::GetContentRegionAvail().x;
    draw_list->AddLine(ImVec2(cursor_pos.x, cursor_pos.y + 8.0f), ImVec2(cursor_pos.x + available_width, cursor_pos.y + 8.0f), IM_COL32(255, 255, 255, 100));
    ImGui::NewLine();
}

bool IMTexture::load_from_svg_file(const std::string& filename, unsigned width, unsigned height, ImTextureID& texture_id)
{
    NSVGimage* image = nsvgParseFromFile(filename.c_str(), "px", 96.0f);
    if (image == nullptr) {
        return false;
    }

    float scale = (float)width / std::max(image->width, image->height);

    int n_pixels = width * height;

    if (n_pixels <= 0) {
        nsvgDelete(image);
        return false;
    }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (rast == nullptr) {
        nsvgDelete(image);
        return false;
    }
    std::vector<unsigned char> data(n_pixels * 4, 0);
    nsvgRasterize(rast, image, 0, 0, scale, data.data(), width, height, width * 4);

    bool compress = false;
    GLint last_texture;
    unsigned m_image_texture{ 0 };
    unsigned char* pixels = (unsigned char*)(&data[0]);

    glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture));
    glsafe(::glGenTextures(1, &m_image_texture));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_image_texture));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels));

    // Store our identifier
    texture_id = (ImTextureID)(intptr_t)m_image_texture;

    // Restore state
    glsafe(::glBindTexture(GL_TEXTURE_2D, last_texture));

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    return true;
}

} // namespace GUI
} // namespace Slic3r
