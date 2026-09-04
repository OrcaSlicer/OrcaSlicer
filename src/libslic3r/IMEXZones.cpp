#include "libslic3r/IMEXZones.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "libslic3r/IMEXHelpers.hpp"

namespace Slic3r {

ImexZoneLayout compute_imex_zone_layout(const DynamicPrintConfig& printer_cfg,
                                        const std::string&        plate_mode,
                                        const std::string&        process_mode,
                                        const BoundingBoxf&       bed_extents)
{
    ImexZoneLayout out;

    auto* is_imex_opt = printer_cfg.option<ConfigOptionBool>("is_imex");
    if (!is_imex_opt || !is_imex_opt->value)
        return out;

    // Per-plate mode takes priority over the process preset.
    std::string active_mode = plate_mode;
    if (active_mode == kImexPrimaryMode && !process_mode.empty())
        active_mode = process_mode;
    if (active_mode == kImexPrimaryMode || active_mode.empty())
        return out;

    // Grid dimensions and tool layout from printer config
    auto* gantry_opt  = printer_cfg.option<ConfigOptionInt>("imex_gantry_count");
    auto* tpg_opt     = printer_cfg.option<ConfigOptionInt>("imex_tools_per_gantry");
    auto* layout_opt  = printer_cfg.option<ConfigOptionEnum<ImexToolLayout>>("imex_tool_layout");

    int n_cols  = tpg_opt    ? std::max(1, tpg_opt->value)    : 2;
    int n_rows  = gantry_opt ? std::max(1, gantry_opt->value) : 1;

    // Which corner is T0? flip_x: col 0 is right(max-X); flip_y: row 0 is rear(max-Y)
    const ImexToolLayout layout = layout_opt ? layout_opt->value : ImexToolLayout::FrontLeft;
    bool flip_x = (layout == ImexToolLayout::FrontRight || layout == ImexToolLayout::RearRight);
    bool flip_y = (layout == ImexToolLayout::RearLeft   || layout == ImexToolLayout::RearRight);

    // Convert tool index to physical (col=X-index, row=Y-index), col/row 0 = min-X/min-Y
    auto tool_to_phys = [&](int idx) -> std::pair<int,int> {
        int raw_col = idx % n_cols, raw_row = idx / n_cols;
        return { flip_x ? (n_cols - 1 - raw_col) : raw_col,
                 flip_y ? (n_rows - 1 - raw_row) : raw_row };
    };

    int pri_col = 0, pri_row = 0;

    if (n_cols == 1 && n_rows == 1)
        return out; // nothing to dim with a single zone

    // Look up secondary tool indices (active in mode, but NOT the primary tool).
    // An unresolved mode, and a mode whose row the tools array does not reach, both come
    // back as an empty string; the zone_roles guard below then returns an empty layout.
    const std::string active_tools_str = find_imex_mode(printer_cfg, active_mode).active_tools;

    // Which heads own a zone cell, and in what role. Roles carry through from
    // parse_imex_active_tools() rather than being flattened into small ints, so the
    // classification below is a comparison the compiler checks and a new role cannot slip
    // through as an unhandled number.
    //
    // imex_primary_tool_for_mode handles the Primary slot (it owns the bare-legacy-token →
    // Primary rule); parse_imex_active_tools fills in the Copy/Mirror secondaries. Mode
    // strings use physical T-indices directly; filament routing is separate
    // (imex_head_filament_map).
    //
    // Only Primary / Copy / Mirror land in this map. A Span head shares the primary's zone
    // instead of owning one, so it deliberately gets no entry (and hence no zone centre --
    // see ImexZoneLayout::head_zone_centers), and an extra Primary entry beyond the first is
    // ignored. Any further role must decide here whether it owns a cell.
    std::map<int, ImexRole> zone_roles;
    {
        const int primary = imex_primary_tool_for_mode(active_tools_str);
        if (primary >= 0 && primary < n_rows * n_cols)
            zone_roles[primary] = ImexRole::Primary;
        for (const auto& [phys_idx, role] : parse_imex_active_tools(active_tools_str)) {
            if (phys_idx < 0 || phys_idx >= n_rows * n_cols) continue;
            if (phys_idx == primary) continue;
            // Exhaustive on purpose, with no default: a role added to the enum has to answer
            // "does this own a zone cell?" here, and -Wswitch asks the question at compile
            // time instead of letting the head silently vanish from the layout.
            switch (role) {
            case ImexRole::Copy:
            case ImexRole::Mirror:
                zone_roles[phys_idx] = role;
                break;
            case ImexRole::Primary:  // an extra Primary beyond the first is ignored
            case ImexRole::Span:     // shares the primary's zone rather than owning one
                break;
            }
        }
    }

    // If the mode name was found but has no tools (e.g. stale process-preset mode on a new
    // printer that has no modes defined yet), there is nothing to compute.
    if (zone_roles.empty())
        return out;

    // Identify the Primary tool from the mode definition
    for (const auto& [idx, role] : zone_roles) {
        if (role == ImexRole::Primary) { auto [c, r] = tool_to_phys(idx); pri_col = c; pri_row = r;
            out.primary_head = idx; break; }
    }

    // No Primary landed inside the grid. Either the mode declares none at all, or its `:P`
    // index is off-grid: the modes editor deliberately preserves tool assignments across
    // imex_gantry_count changes, so a mode authored on a 4-tool IQEX with Primary on T3 keeps
    // that assignment when the printer is reconfigured as a 2-tool IDEX, and the
    // `primary < n_rows * n_cols` bound above then drops it from zone_roles.
    //
    // Every zone, strip, ghost centre and slice offset below is derived from pri_col/pri_row,
    // which would still be sitting at their (0,0) initialisers — a position no active tool
    // occupies. That yields a primary zone and a secondary zone that both claim the whole bed:
    // imex_primary_zone() reports the plate clear while the blocking boxes report it full, and
    // under imex_firmware_managed_zones compute_imex_slice_offset() shifts the slice by the bed
    // centre. Degrade to "this plate has no IMEX zones" instead — the same answer the earlier
    // early returns give, and the one every consumer already handles.
    if (out.primary_head < 0)
        return ImexZoneLayout{};

    // Per-gantry grouping drives Span-based aggregation. When the mode declares a
    // Span partner on primary's gantry, non-primary gantries with multiple same-role
    // tools collapse to one cell each (placed at primary's column so has_col_sep
    // stays false and make_boxes expands the cell into a full-X row-strip).
    // Mixed-role / single-tool / no-Span configurations keep per-tool cells.
    const ImexGantryGrouping grouping =
        group_imex_active_tools_by_gantry(active_tools_str, n_cols);
    std::map<int, const ImexGantryGroup*> group_by_gantry;
    for (const auto& grp : grouping.groups)
        group_by_gantry[grp.gantry_index] = &grp;

    // Separate copy and mirror secondary cells using physical coordinates
    std::set<std::pair<int,int>> copy_cells, mirror_cells;
    for (const auto& [idx, role] : zone_roles) {
        if (role == ImexRole::Primary) continue;  // primary handled separately

        const int phys_gantry = idx / n_cols;
        auto git = group_by_gantry.find(phys_gantry);
        const ImexGantryGroup* grp = (git == group_by_gantry.end()) ? nullptr : git->second;

        if (grp && grp->aggregate) {
            // Only the representative contributes a cell; non-reps are folded into
            // the same row-strip and skipped entirely.
            if (idx != grp->representative_phys) continue;
            auto [rep_c, r] = tool_to_phys(idx);
            (void)rep_c;  // intentionally discarded — aggregated cell pins to pri_col
            if      (role == ImexRole::Copy)   copy_cells.insert({pri_col, r});
            else if (role == ImexRole::Mirror) mirror_cells.insert({pri_col, r});
        } else {
            auto [c, r] = tool_to_phys(idx);
            if      (role == ImexRole::Copy)   copy_cells.insert({c, r});
            else if (role == ImexRole::Mirror) mirror_cells.insert({c, r});
        }
    }

    // All active secondary cells combined (for separation axis computation)
    std::set<std::pair<int,int>> all_secondary;
    for (auto& p : copy_cells)   all_secondary.insert(p);
    for (auto& p : mirror_cells) all_secondary.insert(p);

    double x_min = bed_extents.min(0), x_max = bed_extents.max(0);
    double y_min = bed_extents.min(1), y_max = bed_extents.max(1);

    // Zone sizing is based on ACTIVE tool count per axis, not total grid dimensions.
    // Inactive tools (absent from zone_roles) donate their bed share to active neighbors.
    // Sorted active col/row lists map physical index k → zone index k.
    std::vector<int> active_cols_v, active_rows_v;
    {
        std::set<int> ac_set, ar_set;
        for (const auto& [idx, role] : zone_roles) {
            auto [c, r] = tool_to_phys(idx);
            ac_set.insert(c); ar_set.insert(r);
        }
        active_cols_v.assign(ac_set.begin(), ac_set.end()); // sorted ascending
        active_rows_v.assign(ar_set.begin(), ar_set.end());
    }
    int n_active_cols = std::max(1, (int)active_cols_v.size());
    int n_active_rows = std::max(1, (int)active_rows_v.size());

    std::map<int,int> col_to_zone, row_to_zone;
    for (int k = 0; k < n_active_cols; ++k) col_to_zone[active_cols_v[k]] = k;
    for (int k = 0; k < n_active_rows; ++k) row_to_zone[active_rows_v[k]] = k;

    double zone_w = (x_max - x_min) / n_active_cols;
    double zone_h = (y_max - y_min) / n_active_rows;

    // Zone index of the primary tool
    int pri_col_k = col_to_zone.count(pri_col) ? col_to_zone[pri_col] : 0;
    int pri_row_k = row_to_zone.count(pri_row) ? row_to_zone[pri_row] : 0;

    // Zone center per physical head — ghost placement consumes this so that
    // ghosts land in their own secondary zone instead of stacking on primary.
    // Every active tool (including primary) gets an entry; ghost offset math is
    // simply center[head] - center[primary].
    for (const auto& [idx, role] : zone_roles) {
        auto [c, r] = tool_to_phys(idx);
        int ck = col_to_zone.count(c) ? col_to_zone.at(c) : 0;
        int rk = row_to_zone.count(r) ? row_to_zone.at(r) : 0;
        out.head_zone_centers[idx] = Vec2d(
            x_min + (ck + 0.5) * zone_w,
            y_min + (rk + 0.5) * zone_h);
    }

    // Separation axes: row-sep = secondaries on a different gantry, col-sep = different column
    bool has_row_sep = false, has_col_sep = false;
    for (const auto& [sc, sr] : all_secondary) {
        if (sr != pri_row) has_row_sep = true;
        if (sc != pri_col) has_col_sep = true;
    }

    // Build expanded zone rects for a set of cells.
    // When secondaries share only a row difference (same column as primary) → expand to full X width.
    // When secondaries share only a column difference → expand to full Y height.
    // When both axes differ → per-quadrant.
    auto make_boxes = [&](const std::set<std::pair<int,int>>& cells) -> std::vector<BoundingBoxf> {
        std::vector<BoundingBoxf> boxes;
        if (cells.empty()) return boxes;
        auto ck = [&](int c) { return col_to_zone.count(c) ? col_to_zone.at(c) : 0; };
        auto rk = [&](int r) { return row_to_zone.count(r) ? row_to_zone.at(r) : 0; };
        if (has_row_sep && !has_col_sep) {
            std::set<int> rows; for (auto& [c,r] : cells) rows.insert(r);
            for (int sr : rows) {
                int k = rk(sr);
                boxes.emplace_back(Vec2d(x_min, y_min + k*zone_h), Vec2d(x_max, y_min + (k+1)*zone_h));
            }
        } else if (has_col_sep && !has_row_sep) {
            std::set<int> cols; for (auto& [c,r] : cells) cols.insert(c);
            for (int sc : cols) {
                int k = ck(sc);
                boxes.emplace_back(Vec2d(x_min + k*zone_w, y_min), Vec2d(x_min + (k+1)*zone_w, y_max));
            }
        } else {
            for (auto& [sc,sr] : cells) {
                int ck_ = ck(sc), rk_ = rk(sr);
                boxes.emplace_back(Vec2d(x_min + ck_*zone_w, y_min + rk_*zone_h),
                                   Vec2d(x_min + (ck_+1)*zone_w, y_min + (rk_+1)*zone_h));
            }
        }
        return boxes;
    };

    out.copy_zones   = make_boxes(copy_cells);
    out.mirror_zones = make_boxes(mirror_cells);

    // --- Secondary zone blocking ---
    // The expanded bounding boxes for all secondary (copy+mirror) zones. check_outside()
    // uses these to prevent objects being placed outside the primary zone.
    auto push_secondary_box = [&](const std::vector<BoundingBoxf>& boxes) {
        for (const auto& b : boxes)
            out.secondary_zone_boxes.emplace_back(Vec3d(b.min.x(), b.min.y(), -1.0),
                                                  Vec3d(b.max.x(), b.max.y(), 1e4));
    };
    push_secondary_box(out.copy_zones);
    push_secondary_box(out.mirror_zones);

    // --- Carriage collision danger strips ---
    // Only add strips at boundaries of the PRIMARY zone — objects are only placed in the
    // primary zone, so secondary-to-secondary boundaries have no relevance.
    //
    // Strip width is the literal nozzle clearance value on the primary side only:
    //   right X boundary: [bnd_x - nozzle_clearance_x, bnd_x]
    //   left  X boundary: [bnd_x, bnd_x + nozzle_clearance_x]
    //   top   Y boundary: [bnd_y - nozzle_clearance_y, bnd_y]
    //   bottom Y boundary:[bnd_y, bnd_y + nozzle_clearance_y]
    //
    // Strip length matches the primary zone extent (same expansion logic as make_boxes):
    //   !has_row_sep → full bed height;  has_row_sep → primary row only
    //   !has_col_sep → full bed width;   has_col_sep → primary column only

    auto* cw_opt  = printer_cfg.option<ConfigOptionFloat>("imex_nozzle_clearance_x");
    auto* ch_opt  = printer_cfg.option<ConfigOptionFloat>("imex_nozzle_clearance_y");
    auto* mgn_opt = printer_cfg.option<ConfigOptionFloat>("imex_carriage_margin");
    // Fallbacks mirror the values registered in PrintConfig.cpp: 30.0 for both clearances
    // (6664, 6672), 0.0 for the margin (6680). The clearances must NOT fall back to 0.0 --
    // both strip loops below are gated on `carriage_w > 0.0` / `carriage_h > 0.0`, so a zero
    // would silently emit no collision strips at all while the preview still draws 30 mm
    // toolhead boxes. A config built from the ConfigDef always carries these; a partial or
    // hand-built one is the only way to reach the fallback.
    double carriage_w = cw_opt  ? cw_opt->value  : 30.0;
    double carriage_h = ch_opt  ? ch_opt->value  : 30.0;
    double margin     = mgn_opt ? mgn_opt->value : 0.0;

    // Primary zone extent (the clear printable area):
    //   row-sep only → full bed width × primary row's Y band
    //   col-sep only → primary col's X band × full bed height
    //   both         → primary quadrant
    // Uses zone indices so inactive tools don't shrink the zone.
    double pz_x0 = has_col_sep ? x_min + pri_col_k       * zone_w : x_min;
    double pz_x1 = has_col_sep ? x_min + (pri_col_k + 1) * zone_w : x_max;
    double pz_y0 = has_row_sep ? y_min + pri_row_k        * zone_h : y_min;
    double pz_y1 = has_row_sep ? y_min + (pri_row_k + 1)  * zone_h : y_max;
    out.primary_zone_box = BoundingBoxf(Vec2d(pz_x0, pz_y0), Vec2d(pz_x1, pz_y1));

    auto add_strip = [&](double sx0, double sx1, double sy0, double sy1) {
        out.collision_zones.emplace_back(Vec3d(sx0, sy0, -1.0), Vec3d(sx1, sy1, 1e4));
    };
    auto add_margin_band = [&](double sx0, double sx1, double sy0, double sy1) {
        out.margin_bands.emplace_back(Vec2d(sx0, sy0), Vec2d(sx1, sy1));
    };

    // Determine which directions have mirror secondaries adjacent to the primary.
    // Only mirror tools can cause carriage collisions — they move toward each other.
    // Copy tools always move in the same direction, so no collision strip is needed.
    //
    // Driven by `mirror_cells`, not by zone_roles: on an AGGREGATED gantry the cell is
    // pinned to the primary's column and make_boxes then expands it into a full-width row
    // strip, so the painted zone — not any one head's own cell — is what the primary's
    // carriage can meet. Reading each head's own cell instead lost the strip whenever the
    // representative's column differed from the primary's: at tpg=3, gantry_count=2, mode
    // "0:P,1:S,4:M,5:M" the rear gantry paints a full-width mirror strip against the
    // primary's rear boundary, yet neither T4 (col 1) nor T5 (col 2) is in the primary's
    // column, so no strip and no margin band were emitted at all.
    //
    // Both axes require zone-adjacent AND same row/column on the OTHER axis: a mirror
    // diagonally offset from primary (different row AND different column) can't collide
    // with primary's carriage on either axis since the gantries don't overlap there.
    // Without these checks, paired-gantry mc-mirror (`0:P,1:S,2:M,3:M`) would draw a
    // spurious right-edge strip from T3 even though T3 lives on the other gantry's row.
    //
    // The adjacency tests (±1) are in ZONE indices so an inactive column or row between
    // two active ones cannot hide the boundary they really share (see "inactive tools
    // donate their bed share"), while the co-linearity test on the other axis stays in
    // physical indices. The two forms agree for any cell that owns a zone — col_to_zone
    // and row_to_zone are order-preserving bijections over exactly the active indices —
    // so this is one rule written two ways, not two rules.
    bool has_right_sec = false, has_left_sec  = false;
    bool has_top_sec   = false, has_bottom_sec = false;
    for (const auto& [sc, sr] : mirror_cells) {
        int zc = col_to_zone.count(sc) ? col_to_zone.at(sc) : -1;
        int zr = row_to_zone.count(sr) ? row_to_zone.at(sr) : -1;
        if (zc < 0 || zr < 0) continue; // no zone of its own, so no boundary with primary
        // X-boundary strips: mirror zone-adjacent in X, same physical row as primary.
        if (zr == pri_row_k && zc == pri_col_k + 1) has_right_sec = true;
        if (zr == pri_row_k && zc == pri_col_k - 1) has_left_sec  = true;
        // Y-boundary strips: mirror zone-adjacent in Y, same physical column as primary.
        if (zr == pri_row_k + 1 && sc == pri_col) has_top_sec    = true;
        if (zr == pri_row_k - 1 && sc == pri_col) has_bottom_sec = true;
    }

    // X-axis boundaries (vertical strips, width = nozzle_clearance_x on primary side)
    if (carriage_w > 0.0) {
        if (has_right_sec) {
            double bnd_x = x_min + (pri_col_k + 1) * zone_w;
            double strip_inner = bnd_x - carriage_w;
            add_strip(strip_inner, bnd_x, pz_y0, pz_y1);
            if (margin > 0.0)
                add_margin_band(strip_inner - margin, strip_inner, pz_y0, pz_y1);
        }
        if (has_left_sec) {
            double bnd_x = x_min + pri_col_k * zone_w;
            double strip_inner = bnd_x + carriage_w;
            add_strip(bnd_x, strip_inner, pz_y0, pz_y1);
            if (margin > 0.0)
                add_margin_band(strip_inner, strip_inner + margin, pz_y0, pz_y1);
        }
    }

    // Y-axis boundaries (horizontal strips, width = nozzle_clearance_y on primary side)
    if (carriage_h > 0.0) {
        if (has_top_sec) {
            double bnd_y = y_min + (pri_row_k + 1) * zone_h;
            double strip_inner = bnd_y - carriage_h;
            add_strip(pz_x0, pz_x1, strip_inner, bnd_y);
            if (margin > 0.0)
                add_margin_band(pz_x0, pz_x1, strip_inner - margin, strip_inner);
        }
        if (has_bottom_sec) {
            double bnd_y = y_min + pri_row_k * zone_h;
            double strip_inner = bnd_y + carriage_h;
            add_strip(pz_x0, pz_x1, bnd_y, strip_inner);
            if (margin > 0.0)
                add_margin_band(pz_x0, pz_x1, strip_inner, strip_inner + margin);
        }
    }

    return out;
}

} // namespace Slic3r
