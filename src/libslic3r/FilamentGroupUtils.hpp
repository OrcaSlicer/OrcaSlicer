#ifndef FILAMENT_GROUP_UTILS_HPP
#define FILAMENT_GROUP_UTILS_HPP

#include <vector>
#include <map>
#include <string>
#include <exception>

#include "PrintConfig.hpp"
#include "MultiNozzleUtils.hpp"


namespace Slic3r
{
    class PrintObject;
    namespace FilamentGroupUtils
    {
        struct Color
        {
            unsigned char r = 0;
            unsigned char g = 0;
            unsigned char b = 0;
            unsigned char a = 255;
            Color(unsigned char r_ = 0, unsigned char g_ = 0, unsigned char b_ = 0, unsigned a_ = 255) :r(r_), g(g_), b(b_), a(a_) {}
            Color(const std::string& hexstr);
            bool operator<(const Color& other) const;
            bool operator==(const Color& other) const;
            bool operator!=(const Color& other) const;
            std::string to_hex_str(bool include_alpha = false) const;
        };

        enum FilamentUsageType {
            SupportOnly,
            ModelOnly,
            Hybrid
        };


        struct FilamentInfo {
            Color color;
            std::string type;
            bool is_support;
            FilamentUsageType usage_type;
        };

        struct MachineFilamentInfo: public FilamentInfo {
            int extruder_id;
            bool is_extended;
            bool operator<(const MachineFilamentInfo& other) const;
        };

        class FilamentGroupException: public std::exception {
        public:
            enum ErrorCode {
                EmptyAmsFilaments,
                ConflictLimits,
                Unknown
            };

        private:
            ErrorCode code_;
            std::string message_;

        public:
            FilamentGroupException(ErrorCode code, const std::string& message)
                : code_(code), message_(message) {}

            ErrorCode code() const noexcept {
                return code_;
            }

            const char* what() const noexcept override {
                return message_.c_str();
            }
        };

        std::vector<int> calc_max_group_size(const std::vector<std::map<int, int>>& ams_counts,bool ignore_ext_filament);

        std::vector<std::vector<MachineFilamentInfo>> build_machine_filaments(const std::vector<std::vector<DynamicPrintConfig>>& filament_configs, const std::vector<std::map<int, int>>& ams_counts, bool ignore_ext_filament);

        bool collect_unprintable_limits(const std::vector<std::set<int>>& physical_unprintables, const std::vector<std::set<int>>& geometric_unprintables, std::vector<std::set<int>>& unprintable_limits);

        bool remove_intersection(std::set<int>& a, std::set<int>& b);

        void extract_indices(const std::vector<unsigned int>& used_filaments, const std::vector<std::set<int>>& unprintable_elems, std::vector<std::set<int>>& unprintable_idxs);

        void extract_unprintable_limit_indices(const std::vector<std::set<int>>& unprintable_elems, const std::vector<unsigned int>& used_filaments, std::map<int, int>& unplaceable_limits);

        void extract_unprintable_limit_indices(const std::vector<std::set<int>>& unprintable_elems, const std::vector<unsigned int>& used_filaments, std::unordered_map<int, std::vector<int>>& unplaceable_limits);

        bool check_printable(const std::vector<std::set<int>>& groups, const std::map<int, int>& unprintable);

        int get_estimate_extruder_change_count(const std::vector<std::vector<unsigned int>>& layer_filaments, const MultiNozzleUtils::MultiNozzleGroupResult& extruder_nozzle_info);

        int get_estimate_nozzle_change_count(const std::vector<std::vector<unsigned int>>& layer_filaments, const MultiNozzleUtils::MultiNozzleGroupResult& extruder_nozzle_info);

        std::pair<int, int> get_estimate_extruder_filament_change_count(const std::vector<std::vector<unsigned int>>   &layer_filaments, const MultiNozzleUtils::MultiNozzleGroupResult &extruder_nozzle_info);

        std::map<int, std::vector<int>> build_extruder_nozzle_list(const std::vector<MultiNozzleUtils::NozzleInfo>& nozzle_list);

        std::vector<FilamentUsageType> build_filament_usage_type_list(const PrintConfig& config, const std::vector<const PrintObject*>& objects);

        std::vector<int> update_used_filament_values(const std::vector<int>& old_values, const std::vector<int>& new_values, const std::vector<unsigned int>& used_filaments);

}


}


#endif