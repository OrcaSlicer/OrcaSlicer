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

 /* Sequence Id*/
#define STUDIO_START_SEQ_ID    20000
#define STUDIO_END_SEQ_ID      30000
#define CLOUD_SEQ_ID     0

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
    static uint32_t get_flag_bits_no_border(std::string str, int start_idx, int count = 1);

    // eg. get_hex_bits(16, 1, 10) = 1
    static int get_hex_bits(int num, int pos, int input_num_base = 10) { return get_flag_bits(num, pos * 4, 4, input_num_base);};

    static float string_to_float(const std::string& str_value);

    static std::string convertToIp(long long ip);

    // sequence id check
    static bool is_studio_cmd(int seq) { return seq >= STUDIO_START_SEQ_ID && seq < STUDIO_END_SEQ_ID;};
    static bool is_cloud_cmd(int seq) { return seq == CLOUD_SEQ_ID;};
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

enum class DirtyMode{
    COUNTER,
    TIMER
};

template<typename T>
class DevDirtyHandler{
public:
    DevDirtyHandler(T init_value, int setting_threshold, DirtyMode mode): m_value(init_value), m_setting_threshold(setting_threshold), m_mode(mode)
    {
        m_threshold = setting_threshold;
    }
    ~DevDirtyHandler(){};

    T GetValue() const { return m_value; };

    void SetOptimisticValue(const T& data)
    {
        m_value = data;
        m_start_time = time(nullptr);
        m_threshold = m_setting_threshold;
    }

    void UpdateValue(const T& data)
    {
        if (m_mode == DirtyMode::COUNTER)
        {
            if (m_threshold > 0)
                m_threshold--;
            else
                m_value = data;
        }
        else if (m_mode == DirtyMode::TIMER)
        {
            if (time(nullptr) - m_start_time > m_threshold)
                m_value = data;
        }
    }

private:
    T           m_value;
    DirtyMode   m_mode;
    int         m_setting_threshold{0};

    int         m_start_time{0};
    int         m_threshold{0};
};

}; // namespace Slic3r