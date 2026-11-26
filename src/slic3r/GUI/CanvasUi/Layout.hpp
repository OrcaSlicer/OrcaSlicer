#pragma once

#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

// Where the next component starts, in screen pixels.
void place(const Point& pos);

// The components of the scope stacked from where the first one is placed, their left edges in line.
class Group
{
public:
    Group();
    ~Group();
    Group(const Group&)            = delete;
    Group& operator=(const Group&) = delete;
};

// Ids of the scope's components made unique within it, as for the components of one item of a list.
class ScopedId
{
public:
    explicit ScopedId(int id);
    ~ScopedId();
    ScopedId(const ScopedId&)            = delete;
    ScopedId& operator=(const ScopedId&) = delete;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
