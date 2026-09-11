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
uint32_t DevUtil::get_flag_bits_no_border(std::string str, int start_idx, int count)
{
    if (start_idx < 0 || count <= 0) return 0;

    try {
        // --- 1) trim ---
        auto ltrim = [](std::string &s) { s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); })); };
        auto rtrim = [](std::string &s) { s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end()); };
        ltrim(str);
        rtrim(str);

        // --- 2) remove 0x/0X prefix ---
        if (str.size() >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) { str.erase(0, 2); }

        // --- 3) keep only hex digits ---
        std::string hex;
        hex.reserve(str.size());
        for (char c : str) {
            if (std::isxdigit(static_cast<unsigned char>(c))) hex.push_back(c);
        }
        if (hex.empty()) return 0;

        // --- 4) use size_t for all index/bit math ---
        const size_t total_bits = hex.size() * 4ULL;

        const size_t ustart = static_cast<size_t>(start_idx);
        if (ustart >= total_bits) return 0;

        const int    int_bits  = std::numeric_limits<uint32_t>::digits; // typically 32
        const size_t need_bits = static_cast<size_t>(std::min(count, int_bits));

        // [first_bit, last_bit]
        const size_t first_bit = ustart;
        const size_t last_bit  = std::min(ustart + need_bits, total_bits) - 1ULL;
        if (last_bit < first_bit) return 0;

        const size_t right_index = hex.size() - 1ULL;

        const size_t first_nibble = first_bit / 4ULL;
        const size_t last_nibble  = last_bit / 4ULL;

        const size_t start_idx = right_index - last_nibble;
        const size_t end_idx   = right_index - first_nibble;
        if (end_idx < start_idx) return 0;

        const size_t sub_len = end_idx - start_idx + 1ULL;
        if (end_idx >= hex.size()) return 0;

        const std::string sub_hex = hex.substr(start_idx, sub_len);

        unsigned long long chunk = std::stoull(sub_hex, nullptr, 16);

        const unsigned           nibble_offset = static_cast<unsigned>(first_bit % 4ULL);
        const unsigned long long shifted       = (nibble_offset == 0U) ? chunk : (chunk >> nibble_offset);

        uint32_t mask;
        if (need_bits >= static_cast<size_t>(std::numeric_limits<uint32_t>::digits)) {
            mask = std::numeric_limits<uint32_t>::max();
        } else {
            mask = static_cast<uint32_t>((1ULL << need_bits) - 1ULL);
        }

        const uint32_t val = static_cast<uint32_t>(shifted & mask);
        return val;
    } catch (const std::invalid_argument &) {
        return 0;
    } catch (const std::out_of_range &) {
        return 0;
    } catch (...) {
        return 0;
    }
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