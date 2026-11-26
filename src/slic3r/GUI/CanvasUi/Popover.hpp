#pragma once

#include <string>

#include "Menu.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

struct Metrics;
class TextMeasure;

struct PopoverOptions
{
    std::string title;
    // What the caller draws in it, measured before drawing.
    Size        content;
};

// A popup with a title over content the caller draws while it is open: opened by
// open_popup(id), placed by its positioner inside the screen, and open from construction to destruction. A click
// outside closes it, and so does Esc, also while a field in it holds the keyboard, which then drops its edit.
class Popover
{
public:
    Popover(const char* id, const Positioner& anchor, const PopoverOptions& options);
    ~Popover();
    Popover(const Popover&)            = delete;
    Popover& operator=(const Popover&) = delete;

    bool open() const { return m_open; }
    // Where the content goes; the next component starts at its top left corner.
    Rect content() const { return m_content; }
    // Closes it after this frame.
    void close();

private:
    bool m_open{false};
    Rect m_content;
};

// The outer size of a popover: its content, or its title when wider, in its padding, with the title over the content.
Size popover_size(const Metrics& metrics, const TextMeasure& title, const PopoverOptions& options);

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
