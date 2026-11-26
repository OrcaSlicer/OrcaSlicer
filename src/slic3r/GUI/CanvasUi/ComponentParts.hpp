#pragma once

#include <string_view>

#include <imgui/imgui.h>

#include "Availability.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Metrics;
struct Theme;

// The parts the components are drawn with, for the toolkit's own sources.
namespace detail {

const Theme&   theme();
const Metrics& metrics();
void           reason_tooltip(const Availability& availability);

struct Hit
{
    ImVec2 min;
    ImVec2 max;
    bool   clicked;
    bool   hovered;
    bool   held;
};

bool invisible_button(const char* id, const ImVec2& size, const Availability& availability);
Hit  hit(const char* id, const ImVec2& size, const Availability& availability);

void choice_label(const ImVec2& min, const ImVec2& max, std::string_view text, bool chosen, ImU32 color);

void push_field_style();
void pop_field_style();
void push_popup_style();
void pop_popup_style();
void focus_ring();

} // namespace detail

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
