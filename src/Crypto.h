// Crypto.h - MD5 + AES-128-CBC (自包含实现，不依赖 OpenSSL/Windows CNG)
#pragma once
#include <Windows.h>
#include <string>
#include <vector>

namespace mp {

// ─── MD5 ───────────────────────────────────────────────────
class Md5 {
public:
    static void Compute(const uint8_t* data, size_t len, uint8_t digest[16]);
    static std::vector<uint8_t> Compute(const std::vector<uint8_t>& data);
    static std::string HexDigest(const std::vector<uint8_t>& data);
};

// ─── AES-128-CBC ───────────────────────────────────────────
class Aes {
public:
    // 密钥与 IV 均 16 字节
    static bool EncryptCbc(const uint8_t key[16], const uint8_t iv[16],
                           const uint8_t* in, size_t inLen,
                           std::vector<uint8_t>& out);

    static bool DecryptCbc(const uint8_t key[16], const uint8_t iv[16],
                           const uint8_t* in, size_t inLen,
                           std::vector<uint8_t>& out);
};

// ─── miIO 密钥派生 ─────────────────────────────────────────
// key = md5(rawToken)
// iv  = md5(key + rawToken)
struct MiioKeys {
    uint8_t key[16]{};
    uint8_t iv[16]{};

    // tokenHex: 32 位十六进制字符串
    static bool Derive(const std::string& tokenHex, MiioKeys& out);
};

// ─── 工具 ──────────────────────────────────────────────────
std::string ToHex(const uint8_t* data, size_t len);
bool FromHex(const std::string& hex, std::vector<uint8_t>& out);

} // namespace mp
