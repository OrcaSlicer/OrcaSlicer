#ifndef slic3r_GCodeProcessor_hpp_
#define slic3r_GCodeProcessor_hpp_

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/CustomGCode.hpp"

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
            std::vector<std::pair<EMoveType, float>> moves_times;
            std::vector<std::pair<ExtrusionRole, float>> roles_times;
            std::vector<float> layers_times;

            void reset() {
                time = 0.0f;
                prepare_time = 0.0f;
                custom_gcode_times.clear();
                custom_gcode_times.shrink_to_fit();
                moves_times.clear();
                moves_times.shrink_to_fit();
                roles_times.clear();
                roles_times.shrink_to_fit();
                layers_times.clear();
                layers_times.shrink_to_fit();
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
        unsigned int                                        total_extruder_changes;

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
            total_extruder_changes = 0;
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
            float width{ 0.0f }; // mm
            float height{ 0.0f }; // mm
            float mm3_per_mm{ 0.0f };
            float travel_dist{ 0.0f }; // mm
            float fan_speed{ 0.0f }; // percentage
            float temperature{ 0.0f }; // Celsius degrees
            float time{ 0.0f }; // s
            float layer_duration{ 0.0f }; // s (layer id before finalize)


            //BBS: arc move related data
            EMovePathType move_path_type{ EMovePathType::Noop_move };
            Vec3f arc_center_position{ Vec3f::Zero() };      // mm
            std::vector<Vec3f> interpolation_points;     // interpolation points of arc for drawing
            int  object_label_id{-1};
            float print_z{0.0f};

            float volumetric_rate() const { return feedrate * mm3_per_mm; }
            //BBS: new function to support arc move
            bool is_arc_move_with_interpolation_points() const {
                return (move_path_type == EMovePathType::Arc_move_ccw || move_path_type == EMovePathType::Arc_move_cw) && interpolation_points.size();
            }
            bool is_arc_move() const {
                return move_path_type == EMovePathType::Arc_move_ccw || move_path_type == EMovePathType::Arc_move_cw;
            }
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
        std::vector<std::pair<float, std::pair<size_t, size_t>>> spiral_vase_layers;
        //BBS
        std::vector<SliceWarning> warnings;
        int nozzle_hrc;
        std::vector<NozzleType> nozzle_type;
        // first key stores filaments, second keys stores the layer ranges(enclosed) that use the filaments
        std::unordered_map<std::vector<unsigned int>, std::vector<std::pair<int, int>>,FilamentSequenceHash> layer_filaments;
        // first key stores `from` filament, second keys stores the `to` filament
        std::map<std::pair<int,int>, int > filament_change_count_map;

        BedType bed_type = BedType::btCount;
#if ENABLE_GCODE_VIEWER_STATISTICS
        int64_t time{ 0 };
#endif // ENABLE_GCODE_VIEWER_STATISTICS
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
            spiral_vase_layers = other.spiral_vase_layers;
            warnings = other.warnings;
            bed_type = other.bed_type;
            gcode_check_result = other.gcode_check_result;
            limit_filament_maps = other.limit_filament_maps;
            filament_printable_reuslt = other.filament_printable_reuslt;
            layer_filaments = other.layer_filaments;
            filament_change_count_map = other.filament_change_count_map;
            initial_layer_time = other.initial_layer_time;
#if ENABLE_GCODE_VIEWER_STATISTICS
            time = other.time;
#endif
            return *this;
        }
        void  lock() const { result_mutex.lock(); }
        void  unlock() const { result_mutex.unlock(); }
    };


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

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        static const std::string Mm3_Per_Mm_Tag;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

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
            float cruise_time() const;
            float deceleration_time(float distance, float acceleration) const;
            float cruise_distance() const;
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

            float time() const;
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
            float time; // s
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
            std::array<float, static_cast<size_t>(EMoveType::Count)> moves_time;
            std::array<float, static_cast<size_t>(ExtrusionRole::erCount)> roles_time;
            std::vector<float> layers_time;
            //BBS: prepare stage time before print model, including start gcode time and mostly same with start gcode time
            float prepare_time;

            void reset();

            // Simulates firmware st_synchronize() call
            void simulate_st_synchronize(float additional_time = 0.0f);
            void calculate_time(size_t keep_last_n_blocks = 0, float additional_time = 0.0f);
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
        int m_object_label_id{-1};
        float m_print_z{0.0f};
        std::vector<float> m_remaining_volume;
        ExtruderTemps m_filament_nozzle_temp;
        ExtruderTemps m_filament_nozzle_temp_first_layer;
        std::vector<int> m_physical_extruder_map;
        bool m_manual_filament_change;

        //BBS: x, y offset for gcode generated
        double          m_x_offset{ 0 };
        double          m_y_offset{ 0 };
        //BBS: arc move related data
        EMovePathType m_move_path_type{ EMovePathType::Noop_move };
        Vec3f m_arc_center{ Vec3f::Zero() };    // mm
        std::vector<Vec3f> m_interpolation_points;

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
        ExtrusionRole m_extrusion_role;
        std::vector<int> m_filament_maps;
        std::vector<unsigned char> m_last_filament_id;
        std::vector<unsigned char> m_filament_id;
        unsigned char m_extruder_id;
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
#if ENABLE_GCODE_VIEWER_STATISTICS
        std::chrono::time_point<std::chrono::high_resolution_clock> m_start_time;
#endif // ENABLE_GCODE_VIEWER_STATISTICS

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

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        DataChecker m_mm3_per_mm_compare{ "mm3_per_mm", 0.01f };
        DataChecker m_height_compare{ "height", 0.01f };
        DataChecker m_width_compare{ "width", 0.01f };
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    public:
        GCodeProcessor();
        void init_filament_maps_and_nozzle_type_when_import_only_gcode();
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
        void process_buffer(const std::string& buffer);
        void finalize(bool post_process);

        float get_time(PrintEstimatedStatistics::ETimeMode mode) const;
        float get_prepare_time(PrintEstimatedStatistics::ETimeMode mode) const;
        std::string get_time_dhm(PrintEstimatedStatistics::ETimeMode mode) const;
        std::vector<std::pair<CustomGCode::Type, std::pair<float, float>>> get_custom_gcode_times(PrintEstimatedStatistics::ETimeMode mode, bool include_remaining) const;

        std::vector<std::pair<EMoveType, float>> get_moves_time(PrintEstimatedStatistics::ETimeMode mode) const;
        std::vector<std::pair<ExtrusionRole, float>> get_roles_time(PrintEstimatedStatistics::ETimeMode mode) const;
        std::vector<float> get_layers_time(PrintEstimatedStatistics::ETimeMode mode) const;

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
        void process_G2_G3(const GCodeReader::GCodeLine& line);

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

        void process_SYNC(const GCodeReader::GCodeLine& line);

        // Processes T line (Select Tool)
        void process_T(const GCodeReader::GCodeLine& line);
        void process_T(const std::string_view command);
        void process_M1020(const GCodeReader::GCodeLine &line);

        void process_M622(const GCodeReader::GCodeLine &line);
        void process_M623(const GCodeReader::GCodeLine &line);

        void process_filament_change(int id);

        // post process the file with the given filename to:
        // 1) add remaining time lines M73 and update moves' gcode ids accordingly
        // 2) update used filament data
        void run_post_process();

        //BBS: different path_type is only used for arc move
        void store_move_vertex(EMoveType type, EMovePathType path_type = EMovePathType::Noop_move);

        void set_extrusion_role(ExtrusionRole role);

        float minimum_feedrate(PrintEstimatedStatistics::ETimeMode mode, float feedrate) const;
        float minimum_travel_feedrate(PrintEstimatedStatistics::ETimeMode mode, float feedrate) const;
        float get_axis_max_feedrate(PrintEstimatedStatistics::ETimeMode mode, Axis axis, int extruder_id) const;
        float get_axis_max_acceleration(PrintEstimatedStatistics::ETimeMode mode, Axis axis, int extruder_id) const;
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

        // Simulates firmware st_synchronize() call
        void simulate_st_synchronize(float additional_time = 0.0f);

        void update_estimated_times_stats();
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


