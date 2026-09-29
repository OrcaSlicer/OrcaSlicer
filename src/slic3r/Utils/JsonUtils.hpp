#ifndef __JSON_UTILS_HPP
#define __JSON_UTILS_HPP


#include "nlohmann/json.hpp"
#include <string>


class JsonUtils
{
public:
    template<typename T> static T safeGet(const nlohmann::json& j, const std::string& key, const T& default_value)
    {
        try {
            if (j.contains(key)) {
                return j.at(key).get<T>();
            }
        } catch (...) {
            // ignore
        }
        return default_value;
    }

    static int safeGetInt(const nlohmann::json& j, const std::string& key, int default_value)
    {
        try {
            if (j.contains(key) && j.at(key).is_number_integer()) {
                return j.at(key).get<int>();
            }
        } catch (...) {
            // ignore
        }
        return default_value;
    }

    static int64_t safeGetInt64(const nlohmann::json& j, const std::string& key, int64_t default_value)
    {
        try {
            if (j.contains(key) && j.at(key).is_number_integer()) {
                return j.at(key).get<int64_t>();
            }
        } catch (...) {
            // ignore
        }
        return default_value;
    }

    static std::string safeGetString(const nlohmann::json& j, const std::string& key, const std::string& default_value)
    {
        try {
            if (j.contains(key) && j.at(key).is_string()) {
                return j.at(key).get<std::string>();
            }
        } catch (...) {
            // ignore
        }
        return default_value;
    }

    static bool safeGetBool(const nlohmann::json& j, const std::string& key, bool default_value)
    {
        try {
            if (j.contains(key) && j.at(key).is_boolean()) {
                return j.at(key).get<bool>();
            }
        } catch (...) {
            // ignore
        }
        return default_value;
    }

    static nlohmann::json safeGetJson(const nlohmann::json& j, const std::string& key, const nlohmann::json& default_value)
    {
        try {
            if (j.contains(key) && j.at(key).is_object()) {
                return j.at(key);
            }
        } catch (...) {
            // ignore
        }
        return default_value;
    }

    static double safeGetDouble(const nlohmann::json& j, const std::string& key, double default_value)
    {
        // Support float, int
        try {
            if (j.contains(key)) {
                if (j.at(key).is_number_float()) {
                    return j.at(key).get<double>();
                } else if (j.at(key).is_number_integer()) {
                    return static_cast<double>(j.at(key).get<int>());
                }
            }
        } catch (...) {
            // ignore
        }
        return default_value;
    }
};

#endif // __JSON_UTILS_HPP