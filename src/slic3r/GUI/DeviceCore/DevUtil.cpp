#include "DevUtil.h"
#include "fast_float/fast_float.h"

namespace Slic3r
{

int DevUtil::get_flag_bits(std::string str, int start, int count)
{
    try
    {
        unsigned long long decimal_value = std::stoull(str, nullptr, 16);
        unsigned long long mask = (1ULL << count) - 1;
        int flag = (decimal_value >> start) & mask;
        return flag;
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << e.what();
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": failed";
    }

    return 0;
}

int DevUtil::get_flag_bits(int num, int start, int count, int base)
{
    try
    {
        unsigned long long mask = (1ULL << count) - 1;
        unsigned long long value;
        if (base == 10)
        {
            value = static_cast<unsigned long long>(num);
        }
        else if (base == 16)
        {
            value = static_cast<unsigned long long>(std::stoul(std::to_string(num), nullptr, 16));
        }
        else
        {
            throw std::invalid_argument("Unsupported base");
        }

        int flag = (value >> start) & mask;
        return flag;
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << e.what();
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": failed";
    }

    return 0;
}

float DevUtil::string_to_float(const std::string& str_value)
{
    float value = 0.0f;

    try
    {
        fast_float::from_chars(str_value.c_str(), str_value.c_str() + str_value.size(), value);
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << e.what();
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": failed";
    }

    return value;
}

std::string DevUtil::convertToIp(long long ip)
{
    std::stringstream ss;
    ss << ((ip >> 0) & 0xFF) << "." << ((ip >> 8) & 0xFF) << "." << ((ip >> 16) & 0xFF) << "." << ((ip >> 24) & 0xFF);
    return ss.str();
}

std::string DevJsonValParser::get_longlong_val(const nlohmann::json& j)
{
    try
    {
        if (j.is_number())
        {
            return std::to_string(j.get<long long>());
        }
        else if (j.is_string())
        {
            return j.get<std::string>();
        }
    }
    catch (const nlohmann::json::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << e.what();
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": " << e.what();
    }

    return std::string();
}

};// namespace Slic3r