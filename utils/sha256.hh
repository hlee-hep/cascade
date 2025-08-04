#pragma once
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
#include <string>

inline std::string sha256(const std::string &input)
{
    unsigned char hash[SHA256_DIGEST_LENGTH]; // 32 bytes
    SHA256(reinterpret_cast<const unsigned char *>(input.c_str()), input.size(), hash);

    std::ostringstream result;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);

    return result.str(); // 64-character hex string
}