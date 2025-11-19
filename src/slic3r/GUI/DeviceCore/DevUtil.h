/**
 * @file DevUtil.h
 * @brief Provides common static utility methods for general use.
 *
 * This class offers a collection of static helper functions such as string manipulation,
 * file operations, and other frequently used utilities.
 */

#pragma once
#include <string>
#include <sstream>
#include <stdexcept>

#include <boost/log/trivial.hpp>
#include "nlohmann/json.hpp"

namespace Slic3r
{

class DevUtil
{
public:
    DevUtil() = delete;
    DevUtil(const DevUtil&) = delete;
    DevUtil& operator=(const DevUtil&) = delete;

public:
    static int get_flag_bits(std::string str, int start, int count = 1);
    static int get_flag_bits(int num, int start, int count = 1, int base = 10);

    static float string_to_float(const std::string& str_value);

    static std::string convertToIp(long long ip);
};


class DevJsonValParser
{
public:
    template<typename T>
    static T GetVal(const nlohmann::json& j, const std::string& key, const T& default_val = T())
    {
        try
        {
            if (j.contains(key)) { return j[key].get<T>(); }
        }
        catch (const nlohmann::json::exception& e)
        {
            assert(0 && __FUNCTION__);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << e.what();
        }

        return default_val;
    }

    template<typename T>
    static void ParseVal(const nlohmann::json& j, const std::string& key, T& val)
    {
        try
        {
            if (j.contains(key)) { val = j[key].get<T>(); }
        }
        catch (const nlohmann::json::exception& e)
        {
            assert(0 && __FUNCTION__);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << e.what();
        }
    }

    template<typename T>
    static void ParseVal(const nlohmann::json& j, const std::string& key, T& val, T default_val)
    {
        try
        {
            j.contains(key) ? (val = j[key].get<T>()) : (val = default_val);
        }
        catch (const nlohmann::json::exception& e)
        {
            assert(0 && __FUNCTION__);
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << e.what();
        }
    }

public:
    static std::string get_longlong_val(const nlohmann::json& j);
};


struct NumericStrCompare
{
    bool operator()(const std::string& a, const std::string& b) const noexcept
    {
        int ai = -1;
        try {
            ai = std::stoi(a);
        } catch (...) { };

        int bi = -1;
        try {
            bi = std::stoi(b);
        } catch (...) { };

        return ai < bi;
    }
};

}; // namespace Slic3r