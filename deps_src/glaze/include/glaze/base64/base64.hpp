// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace glz
{
   inline constexpr std::string_view base64_chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789+/";

   inline std::string read_base64(const std::string_view input)
   {
      std::string decoded_data;
      static constexpr std::array<int, 256> decode_table = [] {
         std::array<int, 256> t;
         t.fill(-1);
         for (int i = 0; i < 64; ++i) {
            t[base64_chars[i]] = i;
         }
         return t;
      }();

      int val = 0, valb = -8;
      for (unsigned char c : input) {
         if (decode_table[c] == -1) break; // Stop decoding at padding '=' or invalid characters
         val = (val << 6) + decode_table[c];
         valb += 6;
         if (valb >= 0) {
            decoded_data.push_back((val >> valb) & 0xFF);
            valb -= 8;
         }
      }

      return decoded_data;
   }

   inline std::string write_base64(const std::string_view input)
   {
      std::string encoded_data;

      int val = 0, valb = -6;
      for (unsigned char c : input) {
         val = (val << 8) + c;
         valb += 8;
         while (valb >= 0) {
            encoded_data.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
         }
      }
      if (valb > -6) {
         encoded_data.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
      }
      while (encoded_data.size() % 4) {
         encoded_data.push_back('=');
      }
      return encoded_data;
   }
}
