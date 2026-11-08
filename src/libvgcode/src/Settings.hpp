///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_SETTINGS_HPP
#define VGCODE_SETTINGS_HPP

#include "../include/Types.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <vector>

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
		// ORCA: when disabled, the filaments are drawn opaque whatever their color's opacity
		bool translucent_filaments{ true };
		bool spiral_vase_mode{ false };
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

		//
		// Declarative cut planes: per-section ("cut-plane segment") visibility, indexed by
		// PathVertex::cut_segment_id. Sized at load() from the sections actually present; empty
		// (the default, and for any G-code without cut planes) means "no section filtering", so an
		// ordinary print behaves exactly as before. Moves belonging to no section (id < 0) are
		// never hidden by it.
		//
		std::vector<bool> cut_segments_visibility;
};

} // namespace libvgcode

#endif // VGCODE_SETTINGS_HPP
