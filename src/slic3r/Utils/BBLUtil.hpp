#pragma once
#include "nlohmann/json.hpp"

namespace Slic3r
{

class BBLCrossTalk
{
public:
    BBLCrossTalk() = delete;
    ~BBLCrossTalk() = delete;

public:
    static std::string Crosstalk_DevId(const std::string& str);
    static std::string Crosstalk_DevIP(const std::string& str);
    static std::string Crosstalk_DevName(const std::string& str);
    static std::string Crosstalk_JsonLog(const nlohmann::json& json);
    static std::string Crosstalk_UsrId(const std::string& uid);

    static std::string Encode_DevIp(const std::string& str, const std::string& rand);
    static std::string Decode_DevIp(const std::string& str, const std::string& rand);

private:
    static std::string Crosstalk_ChannelName(const std::string& str);
};

class BBL_Encrypt
{
public:
    BBL_Encrypt() = delete;
    ~BBL_Encrypt() = delete;

public:
    static bool AESEncrypt(unsigned char* src, unsigned src_len, unsigned char* encrypt,
                           unsigned& out_len, const std::string& fixed_key);
    static bool AESDecrypt(unsigned char* encrypt, unsigned encrypt_len, unsigned char* src,
                           unsigned& out_len, const std::string& fixed_key);

    // warning: the len should be multiple of AES_BLOCK_SIZE
    static bool AES256CBC_Encrypt(unsigned char* src, unsigned src_len, unsigned char* encrypt,
                                  unsigned& out_len, const std::string& key, const std::string& iv);
    static bool AES256CBC_Decrypt(unsigned char* encrypt, unsigned encrypt_len, unsigned char* src,
                                  unsigned& out_len, const std::string& key, const std::string& iv);
};

}; // namespace Slic3r