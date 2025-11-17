#ifndef FILAMENT_GROUP_UTILS_HPP
#define FILAMENT_GROUP_UTILS_HPP

#include <vector>
#include <map>
#include <string>
#include <exception>

#include "PrintConfig.hpp"

namespace Slic3r
{
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


        struct FilamentInfo {
            Color color;
            std::string type;
            bool is_support;
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
    }


}


#endif