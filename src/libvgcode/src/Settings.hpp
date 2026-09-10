///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_SETTINGS_HPP
#define VGCODE_SETTINGS_HPP

#include "../include/Types.hpp"

#include <map>

namespace libvgcode {

struct Settings
{
		//
	  // Visualization parameters
		//
		EViewType view_type{ EViewType::FeatureType };
		ETimeMode time_mode{ ETimeMode::Normal };
		bool top_layer_only_view_range{ false };
		// ORCA: when enabled, every layer the layer slider is not scrubbed to is rendered
		// darkened (keeping its color) while showing less than the full print
		bool dim_previous_layers{ false };
		// ORCA: how bright those darkened layers are rendered, 1.0 = unchanged, 0.0 = black
		float dim_previous_layers_brightness{ 0.4f };
		bool spiral_vase_mode{ false };
		// ORCA: while the user drags the camera or a slider, the preview can be drawn from a reduced
		// set of entities: one layer in every reduced_detail_layer_stride kept, and on top of that
		// whatever reduced_detail_mode leaves out. The reduced sets are built alongside the full ones
		// in update_enabled_entities(), so holding reduced_detail costs nothing but a buffer binding.
		// Ignored on the OpenGL ES path, which keeps a single set of entities.
		bool reduced_detail{ false };
		EReducedDetailMode reduced_detail_mode{ EReducedDetailMode::Off };
		uint32_t reduced_detail_layer_stride{ 4 };
		// ORCA: what is left out even at rest, with every layer drawn. Bound whenever the reduced set
		// above is not. Off draws everything.
		EReducedDetailMode rest_detail_mode{ EReducedDetailMode::Off };
		// ORCA: in ShellOnly rest mode, the walls of one layer in this many are drawn, that many layers
		// tall; the exposed surfaces of every layer stay. Meant to follow how many layers fit in a
		// pixel at the current view, so that it changes nothing visible.
		uint32_t rest_layer_stride{ 1 };
		//
		// Required update flags
		//
		bool update_view_full_range{ true };
		bool update_enabled_entities{ true };
		bool update_colors{ true };

		//
		// Visibility maps
		//
		std::array<bool, std::size_t(EOptionType::COUNT)> options_visibility{
			    false, // Travels
				false, // Wipes
				false, // Retractions
				false, // Unretractions
				true,  // Seams
				false, // ToolChanges
				false, // ColorChanges
				false, // PausePrints
				false, // CustomGCodes
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
				false, // CenterOfGravity
				true   // ToolMarker
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
		};

		std::array<bool, std::size_t(EGCodeExtrusionRole::COUNT)> extrusion_roles_visibility{
				true, // None
				true, // Perimeter
				true, // ExternalPerimeter
				true, // OverhangPerimeter
				true, // InternalInfill
                true, // SolidInfill
				true, // TopSolidInfill
				true, // Ironing
				true, // BridgeInfill
				true, // GapFill
				true, // Skirt
				true, // SupportMaterial
				true, // SupportMaterialInterface
				true, // WipeTower
				true, // Custom
		        // ORCA
		        true, // BottomSurface
		        true, // InternalBridgeInfill
		        true, // Brim
		        true, // SupportTransition
		        true, // Mixed
		};
};

} // namespace libvgcode

#endif // VGCODE_SETTINGS_HPP
