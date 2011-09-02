#ifndef __JSON_DIFF_HPP
#define __JSON_DIFF_HPP

#include <string>
#include <atomic>
#include <vector>

#include "nlohmann/json.hpp"


using json = nlohmann::json;
using namespace std;

class json_diff
{
private:
    std::string printer_type;
    std::string printer_version = "00.00.00.00";
    json settings_base;
    json full_message;

    json diff2all_base;
    json all2diff_base;
    int  decode_error_count = 0;

    int  diff_objects(json const &in, json &out, json const &base);
    int  restore_objects(json const &in, json &out, json const &base);
    int  restore_append_objects(json const &in, json &out);
    void merge_objects(json const &in, json &out);

public:
    bool load_compatible_settings(std::string const &type, std::string const &version);
    int all2diff(json const &in, json &out);
    int  diff2all(json const &in, json &out);
    int  all2diff_base_reset(json const &base);
    int  diff2all_base_reset(json &base);
    void compare_print(json &a, json &b);

    bool is_need_request();
};
#endif // __JSON_DIFF_HPP
