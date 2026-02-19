#include "BBLUtil.hpp"
#include "libslic3r_version.h"

#include <boost/log/trivial.hpp>

#include <openssl/aes.h>

using namespace std;

#if BBL_RELEASE_TO_PUBLIC
static bool s_enable_cross_talk = true;
#else
static bool s_enable_cross_talk = false;
#endif

namespace Slic3r
{

std::string BBLCrossTalk::Crosstalk_DevId(const std::string& dev_id)
{
    if (!s_enable_cross_talk) { return dev_id; }
    if (dev_id.size() > 6)
    {
        const string& cs_devid = dev_id.substr(0, 3) + std::string(dev_id.size() - 6, '*') + dev_id.substr(dev_id.size() - 3, 3);
        return cs_devid;
    }

    return dev_id;
}

std::string BBLCrossTalk::Crosstalk_DevIP(const std::string& str)
{
    if (!s_enable_cross_talk) { return str; }

    std::string format_ip = str;
    size_t pos_st = 0;
    size_t pos_en = 0;

    for (int i = 0; i < 2; i++) {
        pos_en = format_ip.find('.', pos_st + 1);
        if (pos_en == std::string::npos) {
            return str;
        }
        format_ip.replace(pos_st, pos_en - pos_st, "***");
        pos_st = pos_en + 1;
    }

    return format_ip;
}

std::string BBLCrossTalk::Crosstalk_DevName(const std::string& dev_name)
{
    if (!s_enable_cross_talk) { return dev_name; }

    auto get_name = [](const std::string& dev_name, int count) ->std::string
    {
        const string& cs_dev_name = std::string(dev_name.size() - count, '*') + dev_name.substr(dev_name.size() - count, count);
        return cs_dev_name;
    };

    if (dev_name.size() > 3)
    {
        return get_name(dev_name, 3);
    }
    else if(dev_name.size() > 2)
    {
        return get_name(dev_name, 2);
    }
    else if(dev_name.size() > 1)
    {
        return get_name(dev_name, 1);
    }

    return  std::string(dev_name.size(), '*');
}

std::string BBLCrossTalk::Crosstalk_ChannelName(const std::string& channel_name)
{
    if (!s_enable_cross_talk) { return channel_name; }

    int pos = channel_name.find("_");
    if (pos != std::string::npos && pos < channel_name.size() - 1)
    {
        std::string cs_channel_name = Crosstalk_DevId(channel_name.substr(0, pos))  + channel_name.substr(pos, channel_name.size() - pos);
        return cs_channel_name;
    }

    return "****";
}

std::string BBLCrossTalk::Crosstalk_JsonLog(const nlohmann::json& json)
{
    if (!s_enable_cross_talk) { return json.dump(1); }// Return the original JSON string if cross-talk is disabled 

    nlohmann::json copied_json = json;

    try
    {
        std::vector<nlohmann::json*> to_traverse_jsons {&copied_json};
        while (!to_traverse_jsons.empty())
        {
            nlohmann::json* json = to_traverse_jsons.back();
            to_traverse_jsons.pop_back();

            auto iter = json->begin();
            while (iter != json->end())
            {
                const std::string& key_str = iter.key();
                if (iter.value().is_string())
                {
                    if (key_str.find("dev_id") != string::npos || key_str.find("sn") != string::npos)
                    {
                        iter.value() = Crosstalk_DevId(iter.value().get<std::string>());
                    }
                    else if (key_str.find("dev_name") != string::npos)
                    {
                        iter.value() = Crosstalk_DevName(iter.value().get<std::string>());
                    }
                    else if (key_str.find("access_code")!= string::npos)
                    {
                        iter.value() = "******";
                    }
                    else if (key_str.find("url") != string::npos)
                    {
                        iter.value() = "******";
                    }
                    else if (key_str.find("channel_name") != string::npos)
                    {
                        iter.value() = Crosstalk_ChannelName(iter.value().get<std::string>());
                    }
                    else if (key_str.find("ttcode_enc") != string::npos)
                    {
                        iter.value() = "******";
                    }
                    else if (key_str.find("authkey") != string::npos)
                    {
                        iter.value() = "******";
                    }
                    else if (key_str == "uid")
                    {
                        iter.value() = Crosstalk_UsrId(iter.value().get<std::string>());
                    }
                    else if (key_str.find("region") != string::npos)
                    {
                        iter.value() = "******";
                    }
                    else if (key_str.find("token") != string::npos)
                    {
                        iter.value() = "******";
                    }
                }
                else if (iter.value().is_number())
                {
                    if (key_str == "uid") {
                        iter.value() = Crosstalk_UsrId(std::to_string(iter.value().get<int>()));
                    }
                }
                else if (iter.value().is_object())
                {
                    to_traverse_jsons.push_back(&iter.value());
                }
                else if (iter.value().is_array())
                {
                    for (auto& array_item : iter.value())
                    {
                        if (array_item.is_object() || array_item.is_array()) {
                            to_traverse_jsons.push_back(&array_item);
                        } else if (array_item.is_string()) {
                            try {
                                const auto& string_val = array_item.get<std::string>();
                                if (!string_val.empty()) {
                                    const auto& sub_json = nlohmann::json::parse(string_val);
                                    array_item = Crosstalk_JsonLog(sub_json);
                                }
                            } catch (...) {
                                continue;
                            }
                        }
                    }
                }

                iter++;
            }
        }
    }
    catch (const nlohmann::json::exception& e)
    {
        // Handle JSON parsing exceptions if necessary
        BOOST_LOG_TRIVIAL(info) << "Error processing JSON for cross-talk: " << e.what();
        return std::string();
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(info) << "Error processing JSON for cross-talk: " << e.what();
        return std::string();
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(info) << "Error processing JSON for cross-talk";
    };

    return copied_json.dump(1);
}

std::string BBLCrossTalk::Crosstalk_UsrId(const std::string& uid)
{
    if (!s_enable_cross_talk) { return uid; }
    if (uid.size() > 2)
    {
        const string& cs_uid = std::string(uid.size() - 2, '*') + uid.substr(uid.size() - 2, 2);
        return cs_uid;
    }

    return "******";
}

std::string BBLCrossTalk::Encode_DevIp(const std::string& str, const std::string& rand)
{
    if (str.empty() || rand.empty()) {
        return str;
    }

    uint32_t seed = 2166136261u;
    for (unsigned char ch : rand) {
        seed ^= ch;
        seed *= 16777619u;
    }
    std::mt19937 rng(seed);

    std::string obfuscated;
    obfuscated.resize(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        unsigned char k = static_cast<unsigned char>(rng() & 0xFF);
        obfuscated[i] = static_cast<char>(static_cast<unsigned char>(str[i]) ^ k);
    }

    static const char hexmap[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(obfuscated.size() * 2);
    for (char* p = obfuscated.data(); p < obfuscated.data() + obfuscated.size(); ++p) {
        unsigned char v = static_cast<unsigned char>(*p);
        if (v >> 4 > 16) {
            BOOST_LOG_TRIVIAL(error) << "Encode_DevIp error: invalid byte value.";
            return "";
        }

        if ((v & 0x0F) > 16) {
            BOOST_LOG_TRIVIAL(error) << "Encode_DevIp error: invalid byte value.";
            return "";
        }

        hex.push_back(hexmap[v >> 4]);
        hex.push_back(hexmap[v & 0x0F]);
    }

    return hex;
}

std::string BBLCrossTalk::Decode_DevIp(const std::string& str, const std::string& rand)
{
    if (str.empty() || rand.empty()) {
        return str;
    }

    if ((str.size() % 2) != 0) {
        return str;
    }

    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string bytes;
    bytes.reserve(str.size() / 2);
    for (size_t i = 0; i < str.size(); i += 2) {
        int hi = hexval(str[i]);
        int lo = hexval(str[i + 1]);
        if (hi < 0 || lo < 0) {
            return str;
        }
        unsigned char v = static_cast<unsigned char>((hi << 4) | lo);
        bytes.push_back(static_cast<char>(v));
    }

    uint32_t seed = 2166136261u;
    for (unsigned char ch : rand) {
        seed ^= ch;
        seed *= 16777619u;
    }
    std::mt19937 rng(seed);

    std::string plain;
    plain.resize(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
        unsigned char k = static_cast<unsigned char>(rng() & 0xFF);
        plain[i] = static_cast<char>(static_cast<unsigned char>(bytes[i]) ^ k);
    }

    return plain;
}

bool BBL_Encrypt::AESEncrypt(unsigned char* src, unsigned src_len, unsigned char* encrypt,
                             unsigned& out_len, const std::string& fixed_key)
{
    unsigned char user_key[AES_BLOCK_SIZE];
    for (int i = 0; i < AES_BLOCK_SIZE; i++) {
        user_key[i] = (unsigned char)fixed_key[i % fixed_key.length()];
    }

    AES_KEY key;
    if (AES_set_encrypt_key(user_key, 128, &key) != 0) {
        return false;
    };

    unsigned len = 0;
    while (len < src_len) {
        AES_encrypt(src + len, encrypt + len, &key);
        len += AES_BLOCK_SIZE;
        out_len = len;
    }

    return true;
}

bool BBL_Encrypt::AESDecrypt(unsigned char* encrypt,
                             unsigned encrypt_len,
                             unsigned char* src,
                             unsigned& out_len,
                             const std::string& fixed_key)
{
    if (encrypt_len % AES_BLOCK_SIZE != 0) {
        return false;
    }

    unsigned char user_key[AES_BLOCK_SIZE];
    for (int i = 0; i < AES_BLOCK_SIZE; i++) {
        user_key[i] = (unsigned char)fixed_key[i % fixed_key.length()];
    }

    AES_KEY key;
    if (AES_set_decrypt_key(user_key, 128, &key) != 0) {
        return false;
    };

    unsigned len = 0;
    while (len < encrypt_len) {
        AES_decrypt(encrypt + len, src + len, &key);
        len += AES_BLOCK_SIZE;
        out_len = len;
    }

    return true;
}


static inline void s_bbl_fill_key_32(unsigned char(&key_out)[32], const std::string& key)
{
    for (size_t i = 0; i < 32; ++i) {
        key_out[i] = static_cast<unsigned char>(key[i % key.size()]);
    }
}

static inline void s_bbl_fill_iv_16(unsigned char(&iv_out)[AES_BLOCK_SIZE], const std::string& iv)
{
    for (size_t i = 0; i < AES_BLOCK_SIZE; ++i) {
        iv_out[i] = static_cast<unsigned char>(iv[i % iv.size()]);
    }
}


bool BBL_Encrypt::AES256CBC_Encrypt(unsigned char* src,
                                    unsigned src_len,
                                    unsigned char* encrypt,
                                    unsigned& out_len,
                                    const std::string& key,
                                    const std::string& iv)
{
    if (src == nullptr || encrypt == nullptr) {
        return false;
    }
    if (key.empty() || iv.empty()) {
        return false;
    }
    // No padding: require input length to be a multiple of AES block size.
    if (src_len == 0 || (src_len % AES_BLOCK_SIZE) != 0) {
        return false;
    }

    unsigned char key_bytes[32];
    s_bbl_fill_key_32(key_bytes, key);

    AES_KEY aes_key;
    if (AES_set_encrypt_key(key_bytes, 256, &aes_key) != 0) {
        return false;
    }

    unsigned char iv_bytes[AES_BLOCK_SIZE];
    s_bbl_fill_iv_16(iv_bytes, iv);

    unsigned char ivec[AES_BLOCK_SIZE];
    std::memcpy(ivec, iv_bytes, AES_BLOCK_SIZE);
        
    AES_cbc_encrypt(src, encrypt, src_len, &aes_key, ivec, AES_ENCRYPT);

    out_len = src_len;
    return true;
}

bool BBL_Encrypt::AES256CBC_Decrypt(unsigned char* encrypt,
                                    unsigned encrypt_len,
                                    unsigned char* src,
                                    unsigned& out_len,
                                    const std::string& key,
                                    const std::string& iv)
{
    if (encrypt == nullptr || src == nullptr) {
        return false;
    }
    if (key.empty() || iv.empty()) {
        return false;
    }
    // No padding: require ciphertext length to be a multiple of AES block size.
    if (encrypt_len == 0 || (encrypt_len % AES_BLOCK_SIZE) != 0) {
        return false;
    }

    unsigned char key_bytes[32];
    s_bbl_fill_key_32(key_bytes, key);

    AES_KEY aes_key;
    if (AES_set_decrypt_key(key_bytes, 256, &aes_key) != 0) {
        return false;
    }

    unsigned char iv_bytes[AES_BLOCK_SIZE];
    s_bbl_fill_iv_16(iv_bytes, iv);

    unsigned char ivec[AES_BLOCK_SIZE];
    std::memcpy(ivec, iv_bytes, AES_BLOCK_SIZE);

    AES_cbc_encrypt(encrypt, src, encrypt_len, &aes_key, ivec, AES_DECRYPT);

    out_len = encrypt_len;
    return true;
}

}// End of namespace Slic3r

