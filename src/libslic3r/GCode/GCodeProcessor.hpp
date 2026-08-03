#ifndef slic3r_GCodeProcessor_hpp_
#define slic3r_GCodeProcessor_hpp_

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"

#include <cstdint>
#include <array>
#include <vector>
#include <mutex>
#include <string>
#include <string_view>
#include <optional>

namespace Slic3r {

class Print;

// slice warnings enum strings
#define NOZZLE_HRC_CHECKER                                          "the_actual_nozzle_hrc_smaller_than_the_required_nozzle_hrc"
#define BED_TEMP_TOO_HIGH_THAN_FILAMENT                             "bed_temperature_too_high_than_filament"
#define NOT_SUPPORT_TRADITIONAL_TIMELAPSE                           "not_support_traditional_timelapse"
#define NOT_GENERATE_TIMELAPSE                                      "not_generate_timelapse"
#define SMOOTH_TIMELAPSE_WITHOUT_PRIME_TOWER                        "smooth_timelapse_without_prime_tower"
#define LONG_RETRACTION_WHEN_CUT                                    "activate_long_retraction_when_cut"

    enum class EMoveType : unsigned char
    {
        Noop,
        Retract,
        Unretract,
        Seam,
        Tool_change,
        Color_change,
        Pause_Print,
        Custom_GCode,
        Travel,
        Wipe,
        Extrude,
        Count
    };

    // Classifies why a wipe-tower / change_filament / time-lapse region is safe to relocate a
    // pre-heat M104 into, for the pre-heat/pre-cool injector. The shipping time_lapse_gcode
    // template (timelapse-on by default) emits SKIPPABLE_* on essentially every slice, so the
    // "timelapse" payload -> stTimelapse classification is exercised widely.
    enum SkipType
    {
        stTimelapse,
        stHeadWrapDetect,
        stOther,
        stNone
    };

    const std::unordered_map<std::string_view, SkipType> skip_type_map{
        {"timelapse", SkipType::stTimelapse},
        {"head_wrap_detect", SkipType::stHeadWrapDetect}
    };

    struct PrintEstimatedStatistics
    {
        enum class ETimeMode : unsigned char
        {
            Normal,
            Stealth,
            Count
        };

        struct Mode
        {
            float time;
            float prepare_time;
            std::vector<std::pair<CustomGCode::Type, std::pair<float, float>>> custom_gcode_times;

            void reset() {
                time = 0.0f;
                prepare_time = 0.0f;
                custom_gcode_times.clear();
                custom_gcode_times.shrink_to_fit();
            }
        };

        std::vector<double>                                 volumes_per_color_change;
        std::map<size_t, double>                            model_volumes_per_extruder;
        std::map<size_t, double>                            wipe_tower_volumes_per_extruder;
        std::map<size_t, double>                            support_volumes_per_extruder;
        std::map<size_t, double>                            total_volumes_per_extruder;
        //BBS: the flush amount of every filament
        std::map<size_t, double>                            flush_per_filament;
        std::map<ExtrusionRole, std::pair<double, double>>  used_filaments_per_role;

        std::array<Mode, static_cast<size_t>(ETimeMode::Count)> modes;
        unsigned int                                        total_filament_changes;
        // Number of filament changes that actually re-flush a nozzle (a filament-in-nozzle change
        // onto a non-empty nozzle), tracked only by the richer multi-nozzle hotend-change time model.
        // Stays 0 for single-nozzle printers (X1/P1/A1/H2S/A2L), which never enter the two-arg model.
        unsigned int                                        total_flush_filament_changes;
        unsigned int                                        total_extruder_changes;
        float                                               total_filament_load_time;
        float                                               total_filament_unload_time;
        float                                               total_tool_change_time;
        float                                               total_travel_distance;
        unsigned int                                        total_travel_moves;
        float                                               total_seam_gap_distance;
        float                                               total_seam_scarf_distance;

        PrintEstimatedStatistics() { reset(); }

        void reset() {
            for (auto m : modes) {
                m.reset();
            }
            volumes_per_color_change.clear();
            volumes_per_color_change.shrink_to_fit();
            wipe_tower_volumes_per_extruder.clear();
            model_volumes_per_extruder.clear();
            support_volumes_per_extruder.clear();
            total_volumes_per_extruder.clear();
            flush_per_filament.clear();
            used_filaments_per_role.clear();
            total_filament_changes = 0;
            total_flush_filament_changes = 0;
            total_extruder_changes = 0;
            total_filament_load_time = 0.0f;
            total_filament_unload_time = 0.0f;
            total_tool_change_time = 0.0f;
            total_travel_distance = 0.0f;
            total_travel_moves = 0;
            total_seam_gap_distance = 0.0f;
            total_seam_scarf_distance = 0.0f;
        }
    };

    struct ConflictResult
    {
        std::string        _objName1;
        std::string        _objName2;
        double             _height;
        const void *_obj1; // nullptr means wipe tower
        const void *_obj2;
        int                layer = -1;
        ConflictResult(const std::string &objName1, const std::string &objName2, double height, const void *obj1, const void *obj2)
            : _objName1(objName1), _objName2(objName2), _height(height), _obj1(obj1), _obj2(obj2)
        {}
        ConflictResult() = default;
    };

    using ConflictResultOpt = std::optional<ConflictResult>;

    struct GCodeCheckResult
    {
        int error_code = 0;   // 0 means succeed, 0b 0001 multi extruder printable area error, 0b 0010 multi extruder printable height error,
        // 0b 0100 plate printable area error, 0b 1000 plate printable height error, 0b 10000 wrapping detection area error
        std::map<int, std::vector<std::pair<int, int>>> print_area_error_infos;   // printable_area  extruder_id to <filament_id - object_label_id> which cannot printed in this extruder
        std::map<int, std::vector<std::pair<int, int>>> print_height_error_infos;   // printable_height extruder_id to <filament_id - object_label_id> which cannot printed in this extruder
        void reset() {
            error_code = 0;
            print_area_error_infos.clear();
            print_height_error_infos.clear();
        }
    };

    struct FilamentPrintableResult
    {
        std::vector<int> conflict_filament;
        std::string plate_name;
        FilamentPrintableResult(){};
        FilamentPrintableResult(std::vector<int> &conflict_filament, std::string plate_name) : conflict_filament(conflict_filament), plate_name(plate_name) {}
        bool has_value(){
           return !conflict_filament.empty();
        };
    };

    struct GCodeProcessorResult
    {
        struct FilamentSequenceHash
        {
            uint64_t operator()(const std::vector<unsigned int>& layer_filament) const {
                uint64_t key = 0;
                for (auto& f : layer_filament)
                    key |= (uint64_t(1) << f);
                return key;
            }
        };
        ConflictResultOpt conflict_result;
        GCodeCheckResult  gcode_check_result;
        FilamentPrintableResult filament_printable_reuslt;
        // The per-filament -> logical-nozzle grouping the slicer computed for this
        // result, surfaced onto the object the device GUI reads
        // (plater->background_process().get_current_gcode_result()). Populated only from
        // Print::get_layered_nozzle_group_result() (ToolOrdering's static L/R + rack subset);
        // default-empty (null) and read by no g-code emitter, so it is invisible in the emitted
        // g-code. Consumed by the print-dispatch nozzle mapping (DevNozzleMappingCtrl) via
        // DevUtilBackend::GetNozzleGroupResult.
        std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> nozzle_group_result;
        float initial_layer_time;

        struct SettingsIds
        {
            std::string print;
            std::vector<std::string> filament;
            std::string printer;

            void reset() {
                print.clear();
                filament.clear();
                printer.clear();
            }
        };

        struct MoveVertex
        {
            unsigned int gcode_id{ 0 };
            EMoveType type{ EMoveType::Noop };
            ExtrusionRole extrusion_role{ erNone };
            unsigned char extruder_id{ 0 };
            unsigned char cp_color_id{ 0 };
            Vec3f position{ Vec3f::Zero() }; // mm
            float delta_extruder{ 0.0f }; // mm
            float feedrate{ 0.0f }; // mm/s
            float actual_feedrate{ 0.0f }; // mm/s
            float width{ 0.0f }; // mm
            float height{ 0.0f }; // mm
            float mm3_per_mm{ 0.0f };
            float travel_dist{ 0.0f }; // mm
            float fan_speed{ 0.0f }; // percentage
            float temperature{ 0.0f }; // Celsius degrees
// ORCA: Add Pressure Advance visualization support
            float pressure_advance{ 0.0f };
            // ORCA: Add Acceleration visualization support
            float acceleration{ 0.0f }; // mm/s^2
            // ORCA: Add Jerk visualization support
            float jerk{ 0.0f }; // mm/s
            std::array<float, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)> time{ 0.0f, 0.0f }; // s
            float layer_duration{ 0.0f }; // s
            unsigned int layer_id{ 0 };
            bool internal_only{ false };

            //BBS
            int  object_label_id{-1};
            float print_z{0.0f};

            float volumetric_rate() const { return feedrate * mm3_per_mm; }
            float actual_volumetric_rate() const { return actual_feedrate * mm3_per_mm; }
        };

        struct SliceWarning {
            int         level;                  // 0: normal tips, 1: warning; 2: error
            std::string msg;                    // enum string
            std::string error_code;             // error code for studio
            std::vector<std::string> params;    // extra msg info
        };

        std::string filename;
        unsigned int id;
        std::vector<MoveVertex> moves;
        // Positions of ends of lines of the final G-code this->filename after TimeProcessor::post_process() finalizes the G-code.
        std::vector<size_t> lines_ends;
        Pointfs printable_area;
        //BBS: add bed exclude area
        Pointfs bed_exclude_area;
        Pointfs wrapping_exclude_area;
        std::vector<Pointfs> extruder_areas;
        std::vector<double> extruder_heights;
        //BBS: add toolpath_outside
        bool toolpath_outside;
        //BBS: add object_label_enabled
        bool label_object_enabled;
        //BBS : extra retraction when change filament,experiment func
        bool long_retraction_when_cut {0};
        int timelapse_warning_code {0};
        bool support_traditional_timelapse{true};
        float printable_height;
        float z_offset;
        SettingsIds settings_ids;
        size_t filaments_count;
        bool backtrace_enabled;
        std::vector<std::string> extruder_colors;
        std::vector<float> filament_diameters;
        std::vector<int>   required_nozzle_HRC;
        std::vector<float> filament_densities;
        std::vector<float> filament_costs;
        std::vector<int> filament_vitrification_temperature;
        std::vector<int>   filament_maps;
        std::vector<int>   limit_filament_maps;
        PrintEstimatedStatistics print_statistics;
        std::vector<CustomGCode::Item> custom_gcode_per_print_z;
        bool spiral_vase_mode;
        //BBS
        std::vector<SliceWarning> warnings;
        int nozzle_hrc;
        std::vector<NozzleType> nozzle_type;
        // Per-extruder physical hotend type. Fed to the pre-heat injector's TimeProcessContext
        // (mixed-type X2D workaround). Populated in apply_config; unused until the injector side-pass
        // consumes it.
        std::vector<ExtruderType> extruder_types;
        // Machine-slot layout of the per-variant printer arrays (one entry per (extruder x
        // volume-type) slot). Populated in apply_config; keys the per-slot machine-limit lookup.
        std::vector<std::string> printer_extruder_variant;
        std::vector<int>         printer_extruder_id;
        // first key stores filaments, second keys stores the layer ranges(enclosed) that use the filaments
        std::unordered_map<std::vector<unsigned int>, std::vector<std::pair<int, int>>,FilamentSequenceHash> layer_filaments;
        std::vector<unsigned int> nozzle_change_sequence;
        std::vector<unsigned int> filament_change_sequence;
        std::vector<int> optimal_assignment;
        // first key stores `from` filament, second keys stores the `to` filament
        std::map<std::pair<int,int>, int > filament_change_count_map;

        // Accumulated print time spent inside SKIPPABLE regions, per skip type. Populated by the time
        // estimator; consumed only downstream. The shipping time_lapse_gcode template emits SKIPPABLE_*
        // widely, so this is typically populated (stTimelapse) on most slices.
        std::unordered_map<SkipType, float> skippable_part_time;

        BedType bed_type = BedType::btCount;
        void reset();

        //BBS: add mutex for protection of gcode result
        mutable std::mutex result_mutex;
        GCodeProcessorResult& operator=(const GCodeProcessorResult &other)
        {
            filename = other.filename;
            id = other.id;
            moves = other.moves;
            lines_ends = other.lines_ends;
            printable_area = other.printable_area;
            bed_exclude_area = other.bed_exclude_area;
            wrapping_exclude_area = other.wrapping_exclude_area;
            toolpath_outside = other.toolpath_outside;
            label_object_enabled = other.label_object_enabled;
            long_retraction_when_cut = other.long_retraction_when_cut;
            timelapse_warning_code = other.timelapse_warning_code;
            printable_height = other.printable_height;
            settings_ids = other.settings_ids;
            filaments_count = other.filaments_count;
            extruder_colors = other.extruder_colors;
            filament_diameters = other.filament_diameters;
            filament_densities = other.filament_densities;
            filament_costs = other.filament_costs;
            print_statistics = other.print_statistics;
            custom_gcode_per_print_z = other.custom_gcode_per_print_z;
            spiral_vase_mode = other.spiral_vase_mode;
            warnings = other.warnings;
            bed_type = other.bed_type;
            gcode_check_result = other.gcode_check_result;
            limit_filament_maps = other.limit_filament_maps;
            filament_printable_reuslt = other.filament_printable_reuslt;
            // Orca: copy the shared grouping result so a copied result keeps it (shared_ptr =>
            // memory-safe), rather than leaving a stale pointer on the target. No g-code effect either way.
            nozzle_group_result = other.nozzle_group_result;
            // Keep the per-extruder hotend types on a copied result (injector input).
            extruder_types = other.extruder_types;
            printer_extruder_variant = other.printer_extruder_variant;
            printer_extruder_id = other.printer_extruder_id;
            layer_filaments = other.layer_filaments;
            filament_change_sequence = other.filament_change_sequence;
            nozzle_change_sequence = other.nozzle_change_sequence;
            optimal_assignment = other.optimal_assignment;
            filament_change_count_map = other.filament_change_count_map;
            // Keep the SKIPPABLE per-type time on a copied result.
            skippable_part_time = other.skippable_part_time;
            initial_layer_time = other.initial_layer_time;
#if ENABLE_GCODE_VIEWER_STATISTICS
            time = other.time;
#endif
            return *this;
        }
        void  lock() const { result_mutex.lock(); }
        void  unlock() const { result_mutex.unlock(); }
    };

    // First-pass usage-block descriptors for the pre-heat/pre-cool injector. FilamentUsageBlock
    // records the [lower,upper) output-line-id span a single filament occupies; ExtruderUsageBlcok
    // (the "Blcok" typo is intentional) records the span an extruder is active in, with the start/end
    // filament + logical-nozzle ids and the post-extrusion (pre-switch) partial-free sub-range. Built
    // during run_post_process, consumed only by the injector side-pass under the enable_pre_heating gate.
    namespace ExtruderPreHeating
    {
        struct FilamentUsageBlock
        {
            int filament_id;
            int extruder_id;
            int nozzle_id;
            unsigned int lower_gcode_id;
            unsigned int upper_gcode_id;  // [lower_gcode_id,upper_gcode_id) uses current filament , upper gcode id will be set after finding next block
            FilamentUsageBlock(int filament_id_, int extruder_id_, int nozzle_id_, unsigned int lower_gcode_id_, unsigned int upper_gcode_id_) :filament_id(filament_id_), extruder_id(extruder_id_), nozzle_id(nozzle_id_), lower_gcode_id(lower_gcode_id_), upper_gcode_id(upper_gcode_id_) {}
        };

        /**
         * @brief Describle the usage of a exturder in a section
         *
         * The strucutre stores the start and end lines of the sections as well as
         * the filament used at the beginning and end of the section.
         * Post extrusion means the final extrusion before switching to the next extruder.
         *
         * Simplified GCode Flow:
         * 1.Extruder Change Block (ext0 switch to ext1)
         * 2.Extruder Usage Block  (use ext1 to print)
         * 3.Extruder Change Block (ext1 switch to ext0)
         * 4.Extruder Usage Block  (use ext0 to print)
         * 5.Extruder Change Block (ext0 switch to ex1)
         * ...
         *
         * So the construct of extruder usage block relys on two extruder change block
        */
        struct ExtruderUsageBlcok
        {
            int extruder_id = -1;
            unsigned int start_id = -1;
            unsigned int end_id = -1;
            int start_filament = -1;
            int end_filament = -1;
            int start_nozzle_id = -1;
            int end_nozzle_id = -1;
            unsigned int post_extrusion_start_id = -1;
            unsigned int post_extrusion_end_id = -1;
            bool         ignore_cooling_before_tower = false;

            void initialize_step_1(int extruder_id_, int start_id_, int start_filament_, int start_nozzle_id_) {
                extruder_id = extruder_id_;
                start_id = start_id_;
                start_filament = start_filament_;
                start_nozzle_id = start_nozzle_id_;
            };
            void initialize_step_2(int post_extrusion_start_id_) {
                post_extrusion_start_id = post_extrusion_start_id_;
            }
            void initialize_step_3(int end_id_, int end_filament_, int post_extrusion_end_id_, int end_nozzle_id_) {
                end_id = end_id_;
                end_filament = end_filament_;
                post_extrusion_end_id = post_extrusion_end_id_;
                end_nozzle_id = end_nozzle_id_;
            }
            void reset() {
                *this = ExtruderUsageBlcok();
            }
            ExtruderUsageBlcok() = default;
        };
    }


    class CommandProcessor {
    public:
        using command_handler_t = std::function<void(const GCodeReader::GCodeLine& line)>;
    private:
        struct TrieNode {
            command_handler_t handler{ nullptr };
            std::unordered_map<char, std::unique_ptr<TrieNode>> children;
            bool early_quit{ false }; // stop matching, trigger handle imediately
        };
    public:
        CommandProcessor();
        void register_command(const std::string& str, command_handler_t handler,bool early_quit = false);
        bool process_comand(std::string_view cmd, const GCodeReader::GCodeLine& line);
    private:
        std::unique_ptr<TrieNode> root;
    };


    class GCodeProcessor
    {
        static const std::vector<std::string> Reserved_Tags;
        static const std::vector<std::string> Reserved_Tags_compatible;
        static const std::string Flush_Start_Tag;
        static const std::string Flush_End_Tag;
        static const std::string VFlush_Start_Tag;
        static const std::string VFlush_End_Tag;
        static const std::string External_Purge_Tag;
    public:
        // Orca: SKIPPABLE region tags, stored as static strings (the FLUSH idiom above) rather than
        // a CustomETags/CustomTags array. Public so the emission sites (WipeTower / change_filament
        // path) can reference them single-sourced.
        static const std::string Skippable_Start_Tag;
        static const std::string Skippable_End_Tag;
        static const std::string Skippable_Type_Tag;
        // Orca: usage-block builder markers (MACHINE_START_GCODE_END / MACHINE_END_GCODE_START /
        // NOZZLE_CHANGE_START / NOZZLE_CHANGE_END / CP_TOOLCHANGE_WIPE), stored as static strings (the
        // FLUSH/SKIPPABLE idiom above) rather than extending the Reserved_Tags arrays — these are
        // multi-nozzle markers only ever emitted by BBL-printer paths. Public so the emission sites can
        // reference them single-sourced. The MACHINE_*_GCODE_* emission (GCode.cpp, gated
        // enable_pre_heating) activates the usage-block builder.
        static const std::string Machine_Start_GCode_End_Tag;
        static const std::string Machine_End_GCode_Start_Tag;
        static const std::string Nozzle_Change_Start_Tag;
        static const std::string Nozzle_Change_End_Tag;
        static const std::string Toolchange_Wipe_Tag;
    public:
        enum class ETags : unsigned char
        {
            Role,
            Wipe_Start,
            Wipe_End,
            Height,
            Width,
            Layer_Change,
            Color_Change,
            Pause_Print,
            Custom_Code,
            First_Line_M73_Placeholder,
            Last_Line_M73_Placeholder,
            Estimated_Printing_Time_Placeholder,
            Total_Layer_Number_Placeholder,
            Manual_Tool_Change,
            During_Print_Exhaust_Fan,
            Wipe_Tower_Start,
            Wipe_Tower_End,
            PA_Change,
            Print_Time_Sec_Placeholder,
            Used_Filament_Length_Placeholder,
        };

        static const std::string& reserved_tag(ETags tag) { return s_IsBBLPrinter ? Reserved_Tags[static_cast<unsigned char>(tag)] : Reserved_Tags_compatible[static_cast<unsigned char>(tag)]; }
        // checks the given gcode for reserved tags and returns true when finding the 1st (which is returned into found_tag) 
        static bool contains_reserved_tag(const std::string& gcode, std::string& found_tag);
        // checks the given gcode for reserved tags and returns true when finding any
        // (the first max_count found tags are returned into found_tag)
        static bool contains_reserved_tags(const std::string& gcode, unsigned int max_count, std::vector<std::string>& found_tag);

        static int get_gcode_last_filament(const std::string &gcode_str);
        static bool get_last_z_from_gcode(const std::string& gcode_str, double& z);
        static bool get_last_position_from_gcode(const std::string &gcode_str, Vec3f &pos);

        static const float Wipe_Width;
        static const float Wipe_Height;

        static bool s_IsBBLPrinter;

    private:
        using AxisCoords = std::array<double, 4>;
        using ExtruderColors = std::vector<unsigned char>;
        using ExtruderTemps = std::vector<float>;

        enum class EUnits : unsigned char
        {
            Millimeters,
            Inches
        };

        enum class EPositioningType : unsigned char
        {
            Absolute,
            Relative
        };

        struct CachedPosition
        {
            AxisCoords position; // mm
            float feedrate; // mm/s

            void reset();
        };

        struct CpColor
        {
            unsigned char counter;
            unsigned char current;

            void reset();
        };

    public:
        struct FeedrateProfile
        {
            float entry{ 0.0f }; // mm/s
            float cruise{ 0.0f }; // mm/s
            float exit{ 0.0f }; // mm/s
        };

        struct Trapezoid
        {
            float accelerate_until{ 0.0f }; // mm
            float decelerate_after{ 0.0f }; // mm
            float cruise_feedrate{ 0.0f }; // mm/sec

            float acceleration_time(float entry_feedrate, float acceleration) const;
            float cruise_time() const { return (cruise_feedrate != 0.0f) ? cruise_distance() / cruise_feedrate : 0.0f; }
            float deceleration_time(float distance, float acceleration) const;
            float acceleration_distance() const { return accelerate_until; }
            float cruise_distance() const { return decelerate_after - accelerate_until; }
            float deceleration_distance(float distance) const { return distance - decelerate_after; }
            bool is_cruise_only(float distance) const { return std::abs(cruise_distance() - distance) < EPSILON; }
        };

        struct TimeBlock
        {
            struct Flags
            {
                bool recalculate{ false };
                bool nominal_length{ false };
                bool prepare_stage{ false };
            };

            EMoveType move_type{ EMoveType::Noop };
            ExtrusionRole role{ erNone };
            // SKIPPABLE tag classification stamped onto each time block. Feeds skippable_part_time
            // and the injector's SKIPPABLE relocation. stNone unless inside a SKIPPABLE_* region.
            SkipType skippable_type{ SkipType::stNone };
            unsigned int move_id{ 0 };
            unsigned int g1_line_id{ 0 };
            unsigned int remaining_internal_g1_lines{ 0 };
            unsigned int layer_id{ 0 };
            float distance{ 0.0f }; // mm
            float acceleration{ 0.0f }; // mm/s^2
            float max_entry_speed{ 0.0f }; // mm/s
            float safe_feedrate{ 0.0f }; // mm/s
            Flags flags;
            FeedrateProfile feedrate_profile;
            Trapezoid trapezoid;

            // Calculates this block's trapezoid
            void calculate_trapezoid();

            float time() const {
                return trapezoid.acceleration_time(feedrate_profile.entry, acceleration) +
                       trapezoid.cruise_time() + trapezoid.deceleration_time(distance, acceleration);
            }
        };


    private:
        friend class ExportLines;
        struct TimeMachine
        {
            struct State
            {
                float feedrate; // mm/s
                float safe_feedrate; // mm/s
                //BBS: feedrate of X-Y-Z-E axis. But when the move is G2 and G3, X-Y will be
                //same value which means feedrate in X-Y plane.
                AxisCoords axis_feedrate; // mm/s
                AxisCoords abs_axis_feedrate; // mm/s

                //BBS: unit vector of enter speed and exit speed in x-y-z space.
                //For line move, there are same. For arc move, there are different.
                Vec3f enter_direction;
                Vec3f exit_direction;

                void reset();
            };

            struct CustomGCodeTime
            {
                bool needed;
                float cache;
                std::vector<std::pair<CustomGCode::Type, float>> times;

                void reset();
            };

            struct G1LinesCacheItem
            {
                unsigned int id;
                unsigned int remaining_internal_g1_lines{ 0 };
                float elapsed_time;
            };

            struct ActualSpeedMove
            {
                unsigned int move_id{ 0 };
                std::optional<Vec3f> position;
                float actual_feedrate{ 0.0f };
                std::optional<float> delta_extruder;
                std::optional<float> feedrate;
                std::optional<float> width;
                std::optional<float> height;
                std::optional<float> mm3_per_mm;
                std::optional<float> fan_speed;
                std::optional<float> temperature;
            };

            bool enabled;
            float acceleration; // mm/s^2
            // hard limit for the acceleration, to which the firmware will clamp.
            float max_acceleration; // mm/s^2
            float retract_acceleration; // mm/s^2
            // hard limit for the acceleration, to which the firmware will clamp.
            float max_retract_acceleration; // mm/s^2
            float travel_acceleration; // mm/s^2
            // hard limit for the travel acceleration, to which the firmware will clamp.
            float max_travel_acceleration; // mm/s^2
            float extrude_factor_override_percentage;
            // We accumulate total print time in doubles to reduce the loss of precision
            // while adding big floating numbers with small float numbers.
            double time; // s
            struct StopTime
            {
                unsigned int g1_line_id;
                float elapsed_time;
            };
            std::vector<StopTime> stop_times;
            std::string line_m73_main_mask;
            std::string line_m73_stop_mask;
            State curr;
            State prev;
            CustomGCodeTime gcode_time;
            std::vector<TimeBlock> blocks;
            std::vector<G1LinesCacheItem> g1_times_cache;
            float first_layer_time;
            std::vector<ActualSpeedMove> actual_speed_moves;
            //BBS: prepare stage time before print model, including start gcode time and mostly same with start gcode time
            float prepare_time;

            // Orca: extra time (e.g. a filament-change delay) that can't be attributed to a
            // matching block on this pass is buffered here and retried on a later pass, so it
            // is never folded into an unrelated move. On the final pass no later pass remains,
            // so any still-unmatched remainder is added to the machine total (never to a move
            // vertex) instead of being dropped, keeping get_time() consistent with the
            // filament-change statistics. Orca-only EOF hardening; BambuStudio drops it.
            using AdditionalBufferBlock = std::pair<EMoveType, float>;
            using AdditionalBuffer      = std::vector<AdditionalBufferBlock>;
            AdditionalBuffer m_additional_time_buffer;

            void reset();

            // Merge adjacent buffer entries that target the same move type.
            static AdditionalBuffer merge_adjacent_additional_time_blocks(const AdditionalBuffer& buffer);

            // additional_time is attributed to the first block matching target_move_type
            // (EMoveType::Noop matches any block, i.e. the first processed block).
            void calculate_time(GCodeProcessorResult& result, PrintEstimatedStatistics::ETimeMode mode, size_t keep_last_n_blocks = 0, float additional_time = 0.0f, EMoveType target_move_type = EMoveType::Noop, bool is_final = false);
        };

        struct UsedFilaments  // filaments per ColorChange
        {
            double color_change_cache;
            std::vector<double> volumes_per_color_change;

            double model_extrude_cache;
            std::map<size_t, double> model_volumes_per_filament;

            double wipe_tower_cache;
            std::map<size_t, double>wipe_tower_volumes_per_filament;

            double support_volume_cache;
            std::map<size_t, double>support_volumes_per_filament;

            //BBS: the flush amount of every filament
            std::map<size_t, double> flush_per_filament;

            double total_volume_cache;
            std::map<size_t, double>total_volumes_per_filament;

            double role_cache;
            std::map<ExtrusionRole, std::pair<double, double>> filaments_per_role;

            void reset();

            void increase_support_caches(double extruded_volume);
            void increase_model_caches(double extruded_volume);
            void increase_wipe_tower_caches(double extruded_volume);

            void process_color_change_cache();
            void process_model_cache(GCodeProcessor* processor);
            void process_wipe_tower_cache(GCodeProcessor* processor);
            void process_support_cache(GCodeProcessor* processor);
            void process_total_volume_cache(GCodeProcessor* processor);

            void update_flush_per_filament(size_t extrude_id, float flush_length);
            void process_role_cache(GCodeProcessor* processor);
            void process_caches(GCodeProcessor* processor);

            friend class GCodeProcessor;
        };

        struct TimeProcessor
        {
            // Orca: the insert-line taxonomy + the ordered map of lines the pre-heat/pre-cool injector
            // splices into the finished g-code, keyed by output-line id. Orca keeps its single-pass
            // run_post_process (M73 / filament stats / ActualSpeedMove / Backtrace /
            // machine_tool_change_time) intact and applies this map in a separate, gated ADDITIVE
            // second file-rewrite pass (run_second_pass_injection); with an empty map that pass is a
            // byte-for-byte identity rewrite. The map is populated by the PreCoolingInjector.
            enum InsertLineType
            {
                PlaceholderReplace,
                TimePredict,
                FilamentChangePredict,
                ExtruderChangePredict,
                PreCooling,
                PreHeating,
            };

            // first key is line id, second key is content
            using InsertedLinesMap = std::map<unsigned int, std::vector<std::pair<std::string, InsertLineType>>>;

            struct Planner
            {
                // Size of the firmware planner queue. The old 8-bit Marlins usually just managed 16 trapezoidal blocks.
                // Let's be conservative and plan for newer boards with more memory.
                static constexpr size_t queue_size = 64;
                // The firmware recalculates last planner_queue_size trapezoidal blocks each time a new block is added.
                // We are not simulating the firmware exactly, we calculate a sequence of blocks once a reasonable number of blocks accumulate.
                static constexpr size_t refresh_threshold = queue_size * 4;
            };

            // extruder_id is currently used to correctly calculate filament load / unload times into the total print time.
            // This is currently only really used by the MK3 MMU2:
            // extruder_unloaded = true means no filament is loaded yet, all the filaments are parked in the MK3 MMU2 unit.
            bool extruder_unloaded;
            // allow to skip the lines M201/M203/M204/M205 generated by GCode::print_machine_envelope() for non-Normal time estimate mode
            bool machine_envelope_processing_enabled;
            MachineEnvelopeConfig machine_limits;
            // Additional load / unload times for a filament exchange sequence.
            float filament_load_times;
            float filament_unload_times;
            //Orca:  time for tool change
            float machine_tool_change_time;

            std::array<TimeMachine, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)> machines;

            void reset();
        };

        // The pre-cool / pre-heat injection engine. It consumes the already-computed per-move time
        // substrate (moves[i].time[valid_machine_id] / .gcode_id) and the first-pass usage blocks to
        // locate idle-hotend windows, then emits M632/M400/M104/M633 lines into a
        // TimeProcessor::InsertedLinesMap that the additive second file-rewrite pass
        // (run_second_pass_injection) splices into the finished g-code. It is constructed and run ONLY
        // when m_enable_pre_heating — single-nozzle printers (X1/P1/A1/H2S, flag false) never reach it.
        // Every input is a const reference bundled from GCodeProcessor members; the injector never
        // mutates GCodeProcessor state.
        class PreCoolingInjector {
        public:
            struct ExtruderFreeBlock {
                unsigned int free_lower_gcode_id;
                unsigned int free_upper_gcode_id;
                unsigned int partial_free_lower_id; // range of extrusion in wipe tower; without a wipe tower
                unsigned int partial_free_upper_id; // partial_free lower/upper equal free_lower_gcode_id
                int last_filament_id;
                int next_filament_id;
                int last_nozzle_id;
                int next_nozzle_id;
                int extruder_id; // partition key for the pre-heat/pre-cool region (extruder or hotend), not
                                 // necessarily a real extruder id
                bool ignore_cooling_before_tower = false;
            };

            void process_pre_cooling_and_heating(TimeProcessor::InsertedLinesMap& inserted_operation_lines);
            void build_extruder_free_blocks(const std::vector<ExtruderPreHeating::FilamentUsageBlock>& filament_usage_blocks, const std::vector<ExtruderPreHeating::ExtruderUsageBlcok>& extruder_usage_blocks);

            PreCoolingInjector(
                const std::vector<GCodeProcessorResult::MoveVertex>& moves_,
                const std::vector<std::string>& filament_types_,
                const MultiNozzleUtils::LayeredNozzleGroupResult& nozzle_group_result_,
                const std::vector<int>& filament_nozzle_temps_,
                const std::vector<int>& filament_nozzle_temps_initial_layer_,
                const std::vector<int>& physical_extruder_map_,
                int valid_machine_id_,
                float inject_time_threshold_,
                bool handle_hotend_as_extruder_,
                bool has_filament_switcher_,
                const std::vector<int>& pre_cooling_temp_,
                const std::vector<double>& cooling_rate_,
                const std::vector<double>& heating_rate_,
                const std::vector<std::pair<unsigned int, unsigned int>>& skippable_blocks_,
                const std::vector<int>& extruder_max_nozzle_count_,
                const std::vector<double>& filament_preheat_temperature_delta_,
                const std::vector<double>& filament_max_temperature_drop_when_ec_,
                unsigned int machine_start_gcode_end_id_,
                unsigned int machine_end_gcode_start_id_,
                const std::vector<ExtruderType>& extruder_types_,
                const std::vector<double>& nozzle_diameter_
            ) :
                moves(moves_),
                filament_types(filament_types_),
                nozzle_group_result(nozzle_group_result_),
                filament_nozzle_temps(filament_nozzle_temps_),
                filament_nozzle_temps_initial_layer(filament_nozzle_temps_initial_layer_),
                physical_extruder_map(physical_extruder_map_),
                valid_machine_id(valid_machine_id_),
                inject_time_threshold(inject_time_threshold_),
                handle_hotend_as_extruder(handle_hotend_as_extruder_),
                has_filament_switcher(has_filament_switcher_),
                filament_pre_cooling_temps(pre_cooling_temp_),
                cooling_rate(cooling_rate_),
                heating_rate(heating_rate_),
                skippable_blocks(skippable_blocks_),
                extruder_max_nozzle_count(extruder_max_nozzle_count_),
                filament_preheat_temperature_delta(filament_preheat_temperature_delta_),
                filament_max_temperature_drop_when_ec(filament_max_temperature_drop_when_ec_),
                machine_start_gcode_end_id(machine_start_gcode_end_id_),
                machine_end_gcode_start_id(machine_end_gcode_start_id_),
                extruder_types(extruder_types_),
                nozzle_diameter(nozzle_diameter_)
            {
            }

        private:
            std::vector<ExtruderFreeBlock> m_extruder_free_blocks;
            const std::vector<GCodeProcessorResult::MoveVertex>& moves;
            const std::vector<std::string>& filament_types;
            const MultiNozzleUtils::LayeredNozzleGroupResult& nozzle_group_result;
            const std::vector<int>& filament_nozzle_temps;
            const std::vector<int>& filament_nozzle_temps_initial_layer;
            const std::vector<int>& physical_extruder_map;
            const int valid_machine_id;
            const float inject_time_threshold;
            const bool handle_hotend_as_extruder;
            const bool has_filament_switcher;
            const std::vector<double>& cooling_rate;
            const std::vector<double>& heating_rate;
            const std::vector<int>& filament_pre_cooling_temps; // target cooling temp during post extrusion
            const std::vector<std::pair<unsigned int, unsigned int>>& skippable_blocks;
            const std::vector<int>& extruder_max_nozzle_count;
            const std::vector<double>& filament_preheat_temperature_delta;
            const std::vector<double>& filament_max_temperature_drop_when_ec;
            const unsigned int machine_start_gcode_end_id;
            const unsigned int machine_end_gcode_start_id;
            const std::vector<ExtruderType>& extruder_types;
            const std::vector<double>& nozzle_diameter;

            void inject_cooling_heating_command(
                TimeProcessor::InsertedLinesMap& inserted_operation_lines,
                const ExtruderFreeBlock& free_block,
                float curr_temp,
                float target_temp,
                bool pre_cooling,
                bool pre_heating
            );

            void build_by_filament_blocks(const std::vector<ExtruderPreHeating::FilamentUsageBlock>& filament_usage_blocks);
            void build_by_extruder_blocks(const std::vector<ExtruderPreHeating::ExtruderUsageBlcok>& extruder_usage_blocks);
        };
    public:
        class SeamsDetector
        {
            bool m_active{ false };
            std::optional<Vec3f> m_first_vertex;

        public:
            void activate(bool active) {
                if (m_active != active) {
                    m_active = active;
                    if (m_active)
                        m_first_vertex.reset();
                }
            }

            std::optional<Vec3f> get_first_vertex() const { return m_first_vertex; }
            void set_first_vertex(const Vec3f& vertex) { m_first_vertex = vertex; }

            bool is_active() const { return m_active; }
            bool has_first_vertex() const { return m_first_vertex.has_value(); }
        };

        // Helper class used to fix the z for color change, pause print and
        // custom gcode markes
        class OptionsZCorrector
        {
            GCodeProcessorResult& m_result;
            std::optional<size_t> m_move_id;
            std::optional<size_t> m_custom_gcode_per_print_z_id;

        public:
            explicit OptionsZCorrector(GCodeProcessorResult& result) : m_result(result) {
            }

            void set() {
                m_move_id = m_result.moves.size() - 1;
                m_custom_gcode_per_print_z_id = m_result.custom_gcode_per_print_z.size() - 1;
            }

            void update(float height) {
                if (!m_move_id.has_value() || !m_custom_gcode_per_print_z_id.has_value())
                    return;

                const Vec3f position = m_result.moves.back().position;

                GCodeProcessorResult::MoveVertex& move = m_result.moves.emplace_back(m_result.moves[*m_move_id]);
                move.position = position;
                move.height = height;
                m_result.moves.erase(m_result.moves.begin() + *m_move_id);
                m_result.custom_gcode_per_print_z[*m_custom_gcode_per_print_z_id].print_z = position.z();
                reset();
            }

            void reset() {
                m_move_id.reset();
                m_custom_gcode_per_print_z_id.reset();
            }
        };

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        struct DataChecker
        {
            struct Error
            {
                float value;
                float tag_value;
                ExtrusionRole role;
            };

            std::string type;
            float threshold{ 0.01f };
            float last_tag_value{ 0.0f };
            unsigned int count{ 0 };
            std::vector<Error> errors;

            DataChecker(const std::string& type, float threshold)
                : type(type), threshold(threshold)
            {}

            void update(float value, ExtrusionRole role) {
                if (role != erCustom) {
                    ++count;
                    if (last_tag_value != 0.0f) {
                        if (std::abs(value - last_tag_value) / last_tag_value > threshold)
                            errors.push_back({ value, last_tag_value, role });
                    }
                }
            }

            void reset() { last_tag_value = 0.0f; errors.clear(); count = 0; }

            std::pair<float, float> get_min() const {
                float delta_min = FLT_MAX;
                float perc_min = 0.0f;
                for (const Error& e : errors) {
                    if (delta_min > e.value - e.tag_value) {
                        delta_min = e.value - e.tag_value;
                        perc_min = 100.0f * delta_min / e.tag_value;
                    }
                }
                return { delta_min, perc_min };
            }

            std::pair<float, float> get_max() const {
                float delta_max = -FLT_MAX;
                float perc_max = 0.0f;
                for (const Error& e : errors) {
                    if (delta_max < e.value - e.tag_value) {
                        delta_max = e.value - e.tag_value;
                        perc_max = 100.0f * delta_max / e.tag_value;
                    }
                }
                return { delta_max, perc_max };
            }

            void output() const {
                if (!errors.empty()) {
                    std::cout << type << ":\n";
                    std::cout << "Errors: " << errors.size() << " (" << 100.0f * float(errors.size()) / float(count) << "%)\n";
                    auto [min, perc_min] = get_min();
                    auto [max, perc_max] = get_max();
                    std::cout << "min: " << min << "(" << perc_min << "%) - max: " << max << "(" << perc_max << "%)\n";
                }
            }
        };
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    private:
        CommandProcessor m_command_processor;
        GCodeReader m_parser;
        EUnits m_units;
        EPositioningType m_global_positioning_type;
        EPositioningType m_e_local_positioning_type;
        std::vector<Vec3f> m_extruder_offsets;
        GCodeFlavor m_flavor;
        std::vector<float> m_nozzle_volume;
        AxisCoords m_start_position; // mm
        AxisCoords m_end_position; // mm
        AxisCoords m_origin; // mm
        CachedPosition m_cached_position;
        bool m_wiping;
        bool m_flushing; // mark a section with real flush
        bool m_virtual_flushing; // mark a section with virtual flush, only for statistics
        bool m_wipe_tower;
        // Current-section SKIPPABLE state. Set by process_tags when inside a SKIPPABLE_* region;
        // stamped onto each TimeBlock. The shipping time_lapse_gcode template emits SKIPPABLE_*
        // widely, so these commonly go active (true / stTimelapse) and stamp blocks on most slices.
        bool m_skippable{false};
        SkipType m_skippable_type{SkipType::stNone};
        int m_object_label_id{-1};
        float m_print_z{0.0f};
        std::vector<float> m_remaining_volume;
        ExtruderTemps m_filament_nozzle_temp;
        ExtruderTemps m_filament_nozzle_temp_first_layer;
        std::vector<int> m_physical_extruder_map;
        // Multi-nozzle context state. Per-extruder max (sub-)nozzle count; >1 marks a multi-nozzle
        // extruder. Input for the pre-heat/filament-change-time injection model; not yet consumed by
        // Orca's time estimator, so it is inert for existing printers.
        std::vector<int> m_extruder_max_nozzle_count{1};
        // Pre-heat / pre-cool injector estimator inputs. Populated from the config in apply_config
        // (both overloads) and cleared in reset(), so the PreCoolingInjector has its inputs in place.
        // Consumed only by the injector two-pass side-pass, gated on m_enable_pre_heating.
        std::vector<std::string> m_filament_types;
        std::vector<double> m_nozzle_diameter;
        std::vector<double> m_hotend_cooling_rate{ 2.f };
        std::vector<double> m_hotend_heating_rate{ 2.f };
        std::vector<int> m_filament_pre_cooling_temp{ 0 };
        std::vector<double> m_filament_preheat_temperature_delta;
        bool m_enable_pre_heating{ false };
        bool m_handle_hotend_as_extruder{ false };
        bool m_has_filament_switcher{ false };
        // [start,end] output-line-id ranges of each SKIPPABLE region, collected during
        // run_post_process. The injector relocates pre-heat M104s out of these ranges. The shipping
        // time_lapse_gcode template emits SKIPPABLE_* widely, so on a timelapse-on slice this is
        // populated with many timelapse ranges (not empty) — the consumer must expect the common
        // timelapse case, not only H2C/A2L wipe-tower ranges.
        std::vector<std::pair<unsigned int, unsigned int>> m_skippable_blocks;
        // First-pass usage blocks, built in run_post_process and stored on the member so the
        // injector side-pass can consume them. Filled only when m_enable_pre_heating — single-nozzle
        // printers (X1/P1/A1/H2S) never build them. They depend on the MACHINE_*_GCODE_* /
        // NOZZLE_CHANGE_* emission the builder keys off.
        std::vector<ExtruderPreHeating::FilamentUsageBlock> m_filament_blocks;
        std::vector<ExtruderPreHeating::ExtruderUsageBlcok>  m_extruder_blocks;
        unsigned int m_machine_start_gcode_end_line_id{ (unsigned int) (-1) };
        unsigned int m_machine_end_gcode_start_line_id{ (unsigned int) (-1) };
        // Set when the MACHINE_END_GCODE_START tag is seen during the streaming parse; tells
        // process_M400 to skip post-print end-gcode dwells (air purification, timelapse, sound)
        // so they don't inflate the M73 estimate. BBS excludes them in calculate_time(is_final).
        bool m_skip_end_gcode_delays{ false };
        // Tracks, during the stream, which filament sits in each physical nozzle and which nozzle each
        // extruder currently carries. Written by both branches of the two-arg process_filament_change
        // (the fallback branch does occupancy bookkeeping only); read by the richer change-time model
        // and by the per-slot machine-limit resolution. Single-nozzle printers never populate it.
        MultiNozzleUtils::NozzleStatusRecorder m_nozzle_status_recorder;
        // Nozzle grouping context for slot resolution during the streaming pass. Set before the
        // replay begins (see initialize_from_context); deliberately separate from
        // m_result.nozzle_group_result, which is handed over only after the stream for the
        // pre-heat injector's second pass and gates the richer change-time model.
        std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> m_nozzle_group_result;
        bool m_manual_filament_change;

        //BBS: x, y offset for gcode generated
        double          m_x_offset{ 0 };
        double          m_y_offset{ 0 };

        unsigned int m_line_id;
        unsigned int m_last_line_id;
        float m_feedrate; // mm/s
        float m_width; // mm
        float m_height; // mm
        float m_forced_width; // mm
        float m_forced_height; // mm
        float m_mm3_per_mm;
        float m_travel_dist; // mm
        float m_fan_speed; // percentage
        float m_z_offset; // mm
// ORCA: Add Pressure Advance visualization support
        float m_pressure_advance;
        ExtrusionRole m_extrusion_role;
        std::vector<int> m_filament_maps;
        std::vector<unsigned char> m_last_filament_id;
        std::vector<unsigned char> m_filament_id;
        unsigned char m_extruder_id;
        // Cached get_machine_config_idx() value; its inputs (active extruder + recorder occupancy)
        // change only on filament-change events, where it is recomputed.
        int m_machine_config_idx{0};
        ExtruderColors m_extruder_colors;
        ExtruderTemps m_extruder_temps;
        bool  m_is_XL_printer = false;
        int m_highest_bed_temp;
        float m_extruded_last_z;
        float m_first_layer_height; // mm
        float m_zero_layer_height; // mm
        bool m_processing_start_custom_gcode;
        unsigned int m_g1_line_id;
        unsigned int m_layer_id;
        CpColor m_cp_color;
        SeamsDetector m_seams_detector;
        OptionsZCorrector m_options_z_corrector;
        size_t m_last_default_color_id;
        bool m_detect_layer_based_on_tag {false};
        int m_seams_count;
        bool m_measure_g29_time {false};
        bool m_single_extruder_multi_material;
        float m_preheat_time;
        int m_preheat_steps;
        bool m_disable_m73;
        std::string m_printer_model;

        enum class EProducer
        {
            Unknown,
            OrcaSlicer,
            Slic3rPE,
            Slic3r,
            SuperSlicer,
            Cura,
            Simplify3D,
            CraftWare,
            ideaMaker,
            KissSlicer
        };

        static const std::vector<std::pair<GCodeProcessor::EProducer, std::string>> Producers;
        EProducer m_producer;

        TimeProcessor m_time_processor;
        UsedFilaments m_used_filaments;

        Print* m_print{ nullptr };

        GCodeProcessorResult m_result;
        static unsigned int s_result_id;

    public:
        GCodeProcessor();
        void init_filament_maps_and_nozzle_type_when_import_only_gcode();
        // Reprocessing an already-generated g-code (from-previous / imported g-code) does not rebuild
        // the per-filament nozzle grouping the multi-nozzle device GUI needs. Surface it onto the
        // result: keep an already-seeded grouping (from initialize_from_context), otherwise synthesize
        // a default one from the filament map so the result is never left without it.
        void ensure_nozzle_group_result(int min_filament_count);
        // check whether the gcode path meets the filament_map grouping requirements
        bool check_multi_extruder_gcode_valid(const int                         extruder_size,
                                              const Pointfs                     plate_printable_area,
                                              const double                      plate_printable_height,
                                              const Pointfs                     wrapping_exclude_area,
                                              const std::vector<Polygons> &unprintable_areas,
                                              const std::vector<double>   &printable_heights,
                                              const std::vector<int>      &filament_map,
                                              const std::vector<std::set<int>>& unprintable_filament_types );
        void apply_config(const PrintConfig& config);
        void set_print(Print* print) { m_print = print; }
        // Hand the nozzle grouping context to the estimator BEFORE the streaming replay, so the
        // per-slot machine-limit resolution can follow the active nozzle. Null is fine (slot 0).
        void initialize_from_context(const std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase>& nozzle_group_result) {
            m_nozzle_group_result = nozzle_group_result;
        }

        DynamicConfig export_config_for_render() const;

        void enable_stealth_time_estimator(bool enabled);
        bool is_stealth_time_estimator_enabled() const {
            return m_time_processor.machines[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].enabled;
        }
        void enable_machine_envelope_processing(bool enabled) { m_time_processor.machine_envelope_processing_enabled = enabled; }
        void reset();

        const GCodeProcessorResult& get_result() const { return m_result; }
        GCodeProcessorResult& result() { return m_result; }
        GCodeProcessorResult&& extract_result() { return std::move(m_result); }

        // Load a G-code into a stand-alone G-code viewer.
        // throws CanceledException through print->throw_if_canceled() (sent by the caller as callback).
        void process_file(const std::string& filename, std::function<void()> cancel_callback = nullptr);

        // Streaming interface, for processing G-codes just generated by PrusaSlicer in a pipelined fashion.
        void initialize(const std::string& filename);
        void initialize_result_moves() {
            // 1st move must be a dummy move
            assert(m_result.moves.empty());
            m_result.moves.emplace_back(GCodeProcessorResult::MoveVertex());
        }
        void process_buffer(const std::string& buffer);
        void finalize(bool post_process);

        float get_time(PrintEstimatedStatistics::ETimeMode mode) const;
        float get_prepare_time(PrintEstimatedStatistics::ETimeMode mode) const;
        std::string get_time_dhm(PrintEstimatedStatistics::ETimeMode mode) const;
        std::vector<std::pair<CustomGCode::Type, std::pair<float, float>>> get_custom_gcode_times(PrintEstimatedStatistics::ETimeMode mode, bool include_remaining) const;

        float get_first_layer_time(PrintEstimatedStatistics::ETimeMode mode) const;

        //BBS: set offset for gcode writer
        void set_xy_offset(double x, double y) { m_x_offset = x; m_y_offset = y; }

        // Orca: if true, only change new layer if ETags::Layer_Change occurs
        // otherwise when we got a lift of z during extrusion, a new layer will be added
        void detect_layer_based_on_tag(bool enabled) { m_detect_layer_based_on_tag = enabled; }

    private:
        void register_commands();
        void apply_config(const DynamicPrintConfig& config);
        void apply_config_simplify3d(const std::string& filename);
        void apply_config_superslicer(const std::string& filename);
        void process_gcode_line(const GCodeReader::GCodeLine& line, bool producers_enabled);

        // Process tags embedded into comments
        void process_tags(const std::string_view comment, bool producers_enabled);
        bool process_producers_tags(const std::string_view comment);
        bool process_bambuslicer_tags(const std::string_view comment);
        bool process_cura_tags(const std::string_view comment);
        bool process_simplify3d_tags(const std::string_view comment);
        bool process_craftware_tags(const std::string_view comment);
        bool process_ideamaker_tags(const std::string_view comment);
        bool process_kissslicer_tags(const std::string_view comment);

        bool detect_producer(const std::string_view comment);

        // Move
        void process_G0(const GCodeReader::GCodeLine& line);
        void process_G1(const GCodeReader::GCodeLine& line, const std::optional<unsigned int>& remaining_internal_g1_lines = std::nullopt);
        enum class G1DiscretizationOrigin {
            G1,
            G2G3,
        };
        void process_G1(const std::array<std::optional<double>, 4>& axes = { std::nullopt, std::nullopt, std::nullopt, std::nullopt },
            const std::optional<double>& feedrate = std::nullopt, G1DiscretizationOrigin origin = G1DiscretizationOrigin::G1,
            const std::optional<unsigned int>& remaining_internal_g1_lines = std::nullopt);

        // Arc Move
        void process_G2_G3(const GCodeReader::GCodeLine& line, bool clockwise);

        void process_VG1(const GCodeReader::GCodeLine& line);


        // BBS: handle delay command
        void process_G4(const GCodeReader::GCodeLine& line);

        // Retract
        void process_G10(const GCodeReader::GCodeLine& line);

        // Unretract
        void process_G11(const GCodeReader::GCodeLine& line);

        // Set Units to Inches
        void process_G20(const GCodeReader::GCodeLine& line);

        // Set Units to Millimeters
        void process_G21(const GCodeReader::GCodeLine& line);

        // Firmware controlled Retract
        void process_G22(const GCodeReader::GCodeLine& line);

        // Firmware controlled Unretract
        void process_G23(const GCodeReader::GCodeLine& line);

        // Move to origin
        void process_G28(const GCodeReader::GCodeLine& line);

        // BBS
        void process_G29(const GCodeReader::GCodeLine& line);

        // Set to Absolute Positioning
        void process_G90(const GCodeReader::GCodeLine& line);

        // Set to Relative Positioning
        void process_G91(const GCodeReader::GCodeLine& line);

        // Set Position
        void process_G92(const GCodeReader::GCodeLine& line);

        // Sleep or Conditional stop
        void process_M1(const GCodeReader::GCodeLine& line);

        // Set extruder to absolute mode
        void process_M82(const GCodeReader::GCodeLine& line);

        // Set extruder to relative mode
        void process_M83(const GCodeReader::GCodeLine& line);

        // Set extruder temperature
        void process_M104(const GCodeReader::GCodeLine& line);

        // Process virtual command of M104, in order to help gcodeviewer work
        void process_VM104(const GCodeReader::GCodeLine& line);

        // Process virtual command of M109, in order to help gcodeviewer work
        void process_VM109(const GCodeReader::GCodeLine& line);

        // Set fan speed
        void process_M106(const GCodeReader::GCodeLine& line);

        // Disable fan
        void process_M107(const GCodeReader::GCodeLine& line);

// ORCA: Add Pressure Advance visualization support
        // Set pressure advance
        void process_M900(const GCodeReader::GCodeLine& line);
        void process_M572(const GCodeReader::GCodeLine &line);
        void process_SET_PRESSURE_ADVANCE(const GCodeReader::GCodeLine& line);

        // Set tool (Sailfish)
        void process_M108(const GCodeReader::GCodeLine& line);

        // Set extruder temperature and wait
        void process_M109(const GCodeReader::GCodeLine& line);

        // Recall stored home offsets
        void process_M132(const GCodeReader::GCodeLine& line);

        // Set tool (MakerWare)
        void process_M135(const GCodeReader::GCodeLine& line);

        //BBS: Set bed temperature
        void process_M140(const GCodeReader::GCodeLine& line);

        //BBS: wait bed temperature
        void process_M190(const GCodeReader::GCodeLine& line);

        //BBS: wait chamber temperature
        void process_M191(const GCodeReader::GCodeLine& line);

        // Set max printing acceleration
        void process_M201(const GCodeReader::GCodeLine& line);

        // Set maximum feedrate
        void process_M203(const GCodeReader::GCodeLine& line);

        // Set default acceleration
        void process_M204(const GCodeReader::GCodeLine& line);

        // Advanced settings
        void process_M205(const GCodeReader::GCodeLine& line);

        // Klipper SET_VELOCITY_LIMIT
        void process_SET_VELOCITY_LIMIT(const GCodeReader::GCodeLine& line);

        // Set extrude factor override percentage
        void process_M221(const GCodeReader::GCodeLine& line);

        // BBS: handle delay command. M400 is defined by BBL only
        void process_M400(const GCodeReader::GCodeLine& line);

        // Repetier: Store x, y and z position
        void process_M401(const GCodeReader::GCodeLine& line);

        // Repetier: Go to stored position
        void process_M402(const GCodeReader::GCodeLine& line);

        // Set allowable instantaneous speed change
        void process_M566(const GCodeReader::GCodeLine& line);

        // Unload the current filament into the MK3 MMU2 unit at the end of print.
        void process_M702(const GCodeReader::GCodeLine& line);

        //Used for Elegoo printer to change tool head
        void process_M6211(const GCodeReader::GCodeLine& line);
        void process_elegoo_M6211(const GCodeReader::GCodeLine& line);

        void process_SYNC(const GCodeReader::GCodeLine& line);

        // Processes T line (Select Tool)
        void process_T(const GCodeReader::GCodeLine& line);
        void process_T(const std::string_view command);
        // T variant carrying the H<nozzle> logical-nozzle id parsed off the command line. -1 = absent.
        void process_T(const std::string_view command, int nozzle_id);
        void process_M1020(const GCodeReader::GCodeLine &line);

        void process_M622(const GCodeReader::GCodeLine &line);
        void process_M623(const GCodeReader::GCodeLine &line);

        void process_filament_change(int id);
        // Richer hotend-change time model distinguishing extruder-switch / nozzle-in-extruder change /
        // filament-in-nozzle change. Self-gated: for single-nozzle printers it delegates to
        // process_filament_change(int) so their time estimate — hence exported g-code — is unchanged.
        void process_filament_change(int id, int nozzle_id);
        // Destination nozzle of a filament change: the explicit H<nozzle> id when given, else the
        // filament's first nozzle in the grouping. Shared by the change-time model and the
        // fallback-path occupancy bookkeeping.
        std::optional<MultiNozzleUtils::NozzleInfo> resolve_target_nozzle(
            const MultiNozzleUtils::NozzleGroupResultBase &group, int id, int nozzle_id) const;
        // Machine slot of the nozzle currently mounted in the active extruder (0 when no grouping
        // context / unknown extruder — the single-slot layout). Cached in m_machine_config_idx,
        // recomputed on filament-change events.
        int  get_machine_config_idx() const;
        // True only for multi-nozzle-capable printers (H2C cluster, or a dual/multi-extruder machine
        // like H2D/X2D): the gate that admits the richer two-arg hotend-change time model. False for
        // every single-extruder single-nozzle printer (X1/P1/A1/H2S/A2L).
        bool use_multi_nozzle_change_time_model() const;

        // post process the file with the given filename to:
        // 1) add remaining time lines M73 and update moves' gcode ids accordingly
        // 2) update used filament data
        void run_post_process();

        // Additive second file-rewrite pass. Splices the pre-heat/pre-cool injector's InsertedLinesMap
        // into the finished g-code and re-shifts every move's gcode_id by the number of inserted lines
        // before it. Runs only when m_enable_pre_heating, AFTER run_post_process, so single-nozzle
        // printers (X1/P1/A1/H2S) never enter it; with an empty map it is a byte-for-byte identity rewrite.
        void run_second_pass_injection();
        // Shift each move's gcode_id by the count of injector lines inserted before it. No-op when the
        // map is empty.
        void handle_offsets_of_second_process(const TimeProcessor::InsertedLinesMap& inserted_operation_lines);

        //BBS: different path_type is only used for arc move
        void store_move_vertex(EMoveType type, EMovePathType path_type = EMovePathType::Noop_move, bool internal_only = false);

        void set_extrusion_role(ExtrusionRole role);
        // Resolve the SKIPPABLE_TYPE payload to a SkipType.
        void set_skippable_type(const std::string_view type);

        float minimum_feedrate(PrintEstimatedStatistics::ETimeMode mode, float feedrate) const;
        float minimum_travel_feedrate(PrintEstimatedStatistics::ETimeMode mode, float feedrate) const;
        // Speed/acceleration limit arrays are slot-major with two mode entries per machine slot:
        // [slot*2 + mode], slot from get_machine_config_idx() (0 = the only slot on single-variant
        // printers, whose arrays hold just [Normal, Stealth]). The 2-arg forms read slot 0 and stay
        // exactly the historical mode-only lookup; jerk and the accelerations below are mode-only.
        float get_axis_max_feedrate(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const;
        float get_axis_max_feedrate(PrintEstimatedStatistics::ETimeMode mode, Axis axis, int machine_idx) const;
        float get_axis_max_acceleration(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const;
        float get_axis_max_acceleration(PrintEstimatedStatistics::ETimeMode mode, Axis axis, int machine_idx) const;
        float get_axis_max_jerk_with_jd(PrintEstimatedStatistics::ETimeMode mode, Axis axis, float acceleration) const;
        float get_axis_max_jerk_with_jd(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const;
        float get_axis_max_jerk(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const;
        Vec3f get_xyz_max_jerk(PrintEstimatedStatistics::ETimeMode mode) const;
        float get_retract_acceleration(PrintEstimatedStatistics::ETimeMode mode) const;
        void  set_retract_acceleration(PrintEstimatedStatistics::ETimeMode mode, float value);
    float get_acceleration(PrintEstimatedStatistics::ETimeMode mode) const;
        void  set_acceleration(PrintEstimatedStatistics::ETimeMode mode, float value);
        float get_travel_acceleration(PrintEstimatedStatistics::ETimeMode mode) const;
        void  set_travel_acceleration(PrintEstimatedStatistics::ETimeMode mode, float value);
        float get_filament_load_time(size_t extruder_id);
        float get_filament_unload_time(size_t extruder_id);
        float get_extruder_change_time(size_t extruder_id);
        int   get_filament_vitrification_temperature(size_t extrude_id);
        void process_custom_gcode_time(CustomGCode::Type code);
        void process_filaments(CustomGCode::Type code);

        void calculate_time(GCodeProcessorResult& result, size_t keep_last_n_blocks = 0, float additional_time = 0.0f, EMoveType target_move_type = EMoveType::Noop, bool is_final = false);

        // Simulates firmware st_synchronize() call
        void simulate_st_synchronize(float additional_time = 0.0f, EMoveType target_move_type = EMoveType::Noop);

        void update_estimated_times_stats();

        double extract_absolute_position_on_axis(Axis axis, const GCodeReader::GCodeLine& line, double area_filament_cross_section);

        //BBS:
        void update_slice_warnings();

        // get current used filament
        int get_filament_id(bool force_initialize = true) const;
        // get last used filament in the same extruder with current filament
        int get_last_filament_id(bool force_initialize = true) const;
        //get current used extruder
        int get_extruder_id(bool force_initialize = true)const;
   };

} /* namespace Slic3r */

#endif /* slic3r_GCodeProcessor_hpp_ */


