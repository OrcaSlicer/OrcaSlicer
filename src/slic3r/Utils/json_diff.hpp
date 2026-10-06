#ifndef __JSON_DIFF_HPP
#define __JSON_DIFF_HPP

#include <string>
#include <atomic>
#include <vector>

#include "nlohmann/json.hpp"


using namespace std;

class json_diff
{
private:
    std::string printer_type;
    std::string printer_version = "00.00.00.00";
    nlohmann::json settings_base;
    nlohmann::json full_message;

    nlohmann::json diff2all_base;
    nlohmann::json all2diff_base;
    int  decode_error_count = 0;

    int  diff_objects(nlohmann::json const &in, nlohmann::json &out, nlohmann::json const &base);
    int  restore_objects(nlohmann::json const &in, nlohmann::json &out, nlohmann::json const &base);
    int  restore_append_objects(nlohmann::json const &in, nlohmann::json &out);
    void merge_objects(nlohmann::json const &in, nlohmann::json &out);

public:
    bool load_compatible_settings(std::string const &type, std::string const &version);
    int all2diff(nlohmann::json const &in, nlohmann::json &out);
    int  diff2all(nlohmann::json const &in, nlohmann::json &out);
    int  all2diff_base_reset(nlohmann::json const &base);
    int  diff2all_base_reset(nlohmann::json &base);
    void compare_print(nlohmann::json &a, nlohmann::json &b);

    bool is_need_request();
};
#endif // __JSON_DIFF_HPP
