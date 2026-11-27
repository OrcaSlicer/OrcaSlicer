// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_GLFW_IMGUI_IMGUITRAITS_H
#define IGL_OPENGL_GLFW_IMGUI_IMGUITRAITS_H

#include <imgui.h>

// Extend ImGui by populating its namespace directly
namespace ImGui
{

// Infer ImGuiDataType enum based on actual type
template<typename T>
class ImGuiDataTypeTraits
{
	static const ImGuiDataType value; // link error
	static const char * format;
};

template<>
class ImGuiDataTypeTraits<int>
{
	static constexpr ImGuiDataType value = ImGuiDataType_S32;
	static constexpr const char *format = "%d";
};

template<>
class ImGuiDataTypeTraits<unsigned int>
{
	static constexpr ImGuiDataType value = ImGuiDataType_U32;
	static constexpr const char *format = "%u";
};

template<>
class ImGuiDataTypeTraits<long long>
{
	static constexpr ImGuiDataType value = ImGuiDataType_S64;
	static constexpr const char *format = "%lld";
};

template<>
class ImGuiDataTypeTraits<unsigned long long>
{
	static constexpr ImGuiDataType value = ImGuiDataType_U64;
	static constexpr const char *format = "%llu";
};

template<>
class ImGuiDataTypeTraits<float>
{
	static constexpr ImGuiDataType value = ImGuiDataType_Float;
	static constexpr const char *format = "%.3f";
};

template<>
class ImGuiDataTypeTraits<double>
{
	static constexpr ImGuiDataType value = ImGuiDataType_Double;
	static constexpr const char *format = "%.6f";
};

} // namespace ImGui

#endif // IGL_OPENGL_GLFW_IMGUI_IMGUIHELPERS_H
