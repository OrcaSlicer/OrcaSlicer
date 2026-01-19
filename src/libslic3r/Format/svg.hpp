#pragma once
namespace Slic3r {
class Model;

extern bool load_svg(const char *path, Model *model, std::string &message);

}; // namespace Slic3r
