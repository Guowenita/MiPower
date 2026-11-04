// selftest.cpp - 密码学与协议层自检（独立可执行文件）
// 用已知向量验证 MD5 / AES / miIO 密钥派生，任何一项失败即退出码非 0
#include "Crypto.h"
#include "MiioClient.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mp;

static int g_fail = 0;

static void Check(bool cond, const std::string& name, const std::string& detail = "") {
    if (cond) {
        printf("  [PASS] %s\n", name.c_str());
    } else {
        printf("  [FAIL] %s  %s\n", name.c_str(), detail.c_str());
        g_fail++;
    }
}

static std::string HexOf(const std::vector<uint8_t>& v) { return ToHex(v.data(), v.size()); }

// ─── MD5 已知向量 (RFC 1321) ───────────────────────────────
static void TestMd5() {
    printf("[MD5]\n");

    const char* inputs[] = { "", "a", "abc", "message digest",
                             "abcdefghijklmnopqrstuvwxyz" };
    const char* expects[] = {
        "d41d8cd98f00b204e9800998ecf8427e",
        "0cc175b9c0f1b6a831c399e269772661",
        "900150983cd24fb0d6963f7d28e17f72",
        "f96b697d7cb7938d525a2f31aaf161d0",
        "c3fcd3d76192e4007dfb496cca67e13b"
    };
    for (int i = 0; i < 5; i++) {
        std::vector<uint8_t> in(reinterpret_cast<const uint8_t*>(inputs[i]),
                                reinterpret_cast<const uint8_t*>(inputs[i]) + strlen(inputs[i]));
        std::string got = Md5::HexDigest(in);
        Check(got == expects[i], std::string("md5(\"") + inputs[i] + "\")", " got=" + got);
    }

    // 1,000,000 个 'a' —— 测试多块与位长填充
    {
        std::vector<uint8_t> in(1000000, 'a');
        std::string got = Md5::HexDigest(in);
        Check(got == "7707d6ae4e027c70eea2a935c2296f21", "md5(1M 'a')", " got=" + got);
    }
}

// ─── AES-128 已知向量 (FIPS-197) ───────────────────────────
static void TestAes() {
    printf("[AES-128]\n");

    // FIPS-197 附录 C.1 —— AES-128 单块
    // key=000102030405060708090a0b0c0d0e0f, pt=00112233...eeff
    // ct = 69c4e0d86a7b0430d8cdb78070b4c55a
    {
        uint8_t key[16], iv[16];
        std::vector<uint8_t> kin, pin;
        FromHex("000102030405060708090a0b0c0d0e0f", kin);
        FromHex("00112233445566778899aabbccddeeff", pin);
        memcpy(key, kin.data(), 16);
        memset(iv, 0, 16);
        std::vector<uint8_t> out;
        Aes::EncryptCbc(key, iv, pin.data(), pin.size(), out);
        std::string got = HexOf(out);
        Check(got == "69c4e0d86a7b0430d8cdb78070b4c55a", "FIPS-197 C.1 encrypt", " got=" + got);

        std::vector<uint8_t> back;
        Aes::DecryptCbc(key, iv, out.data(), out.size(), back);
        Check(back == pin, "FIPS-197 C.1 decrypt roundtrip");
    }

    // FIPS-197 附录 B —— key=2b7e1516..., pt=3243f6a8...
    // ct = 3925841d02dc09fbdc118597196a0b32
    {
        uint8_t key[16], iv[16];
        std::vector<uint8_t> kin, pin;
        FromHex("2b7e151628aed2a6abf7158809cf4f3c", kin);
        FromHex("3243f6a8885a308d313198a2e0370734", pin);
        memcpy(key, kin.data(), 16);
        memset(iv, 0, 16);
        std::vector<uint8_t> out;
        Aes::EncryptCbc(key, iv, pin.data(), pin.size(), out);
        std::string got = HexOf(out);
        Check(got == "3925841d02dc09fbdc118597196a0b32", "FIPS-197 B encrypt", " got=" + got);
    }

    // NIST SP800-38A CBC-AES128 (前 3 组)
    {
        const char* k = "2b7e151628aed2a6abf7158809cf4f3c";
        const char* i = "000102030405060708090a0b0c0d0e0f";
        const char* p = "6bc1bee22e409f96e93d7e117393172a"
                        "ae2d8a571e03ac9c9eb76fac45af8e51"
                        "30c81c46a35ce411e5fbc1191a0a52ef"
                        "f69f2445df4f9b17ad2b417be66c3710";
        uint8_t key[16], iv[16];
        std::vector<uint8_t> kin, iin, pin;
        FromHex(k, kin); FromHex(i, iin); FromHex(p, pin);
        memcpy(key, kin.data(), 16);
        memcpy(iv, iin.data(), 16);

        std::vector<uint8_t> out;
        Aes::EncryptCbc(key, iv, pin.data(), pin.size(), out);
        std::string got = HexOf(out);
        const char* expect =
            "7649abac8119b246cee98e9b12e9197d"
            "5086cb9b507219ee95db113a917678b2"
            "73bed6b8e3c1743b7116e69e22229516"
            "3ff1caa1681fac09120eca307586e1a7";
        Check(got == expect, "NIST SP800-38A CBC-AES128 encrypt", " got=" + got);

        std::vector<uint8_t> back;
        Aes::DecryptCbc(key, iv, out.data(), out.size(), back);
        Check(back == pin, "NIST SP800-38A CBC decrypt roundtrip");
    }
}

// ─── miIO 密钥派生 ─────────────────────────────────────────
// 实测基准：本机设备 token 00112233445566778899aabbccddeeff
// 对应 key = md5(raw) 与 iv = md5(key+raw)
static void TestMiioKeys() {
    printf("[miIO key derivation]\n");

    MiioKeys k;
    bool ok = MiioKeys::Derive("00112233445566778899aabbccddeeff", k);
    Check(ok, "derive from token hex");

    std::string keyHex = ToHex(k.key, 16);
    std::string ivHex = ToHex(k.iv, 16);

    // 与 python-miio 计算结果对照
    Check(keyHex == "6e8311168ee16d6aa1aa48c64145003c",
          "key = md5(raw_token)", " got=" + keyHex);
    Check(ivHex == "6f434fa9acd75da73e5fb999f641cda2",
          "iv = md5(key + raw_token)", " got=" + ivHex);

    // 非法 token 必须被拒绝
    MiioKeys bad;
    Check(!MiioKeys::Derive("short", bad), "reject short token");
    Check(!MiioKeys::Derive("zzzz7be79aaed7ba573779d66299ae46", bad), "reject non-hex token");
}

// ─── Hex 工具 ──────────────────────────────────────────────
static void TestHex() {
    printf("[hex]\n");
    std::vector<uint8_t> v;
    Check(FromHex("00ff10", v) && v.size() == 3 && v[0] == 0 && v[1] == 0xff && v[2] == 0x10,
          "FromHex basic");
    Check(!FromHex("abc", v), "FromHex odd length rejected");
    Check(!FromHex("zz", v), "FromHex invalid char rejected");
    Check(ToHex(v.data(), v.size()) == "", "ToHex empty");
}


static void TestJsonParse() {
    printf("[JSON property parse]\n");
    std::string j =
        "[{\"did\":\"props\",\"siid\":4,\"piid\":2,\"code\":0,\"value\":63},"
        "{\"did\":\"props\",\"siid\":4,\"piid\":1,\"code\":0,\"value\":3161},"
        "{\"did\":\"props\",\"siid\":2,\"piid\":1,\"code\":0,\"value\":true}]";
    std::vector<PropertyValue> vals;
    bool ok = MiioClient::ParsePropertyResult(j, vals);
    printf("  parse ok=%d count=%zu\n", ok, vals.size());
    for (const auto& v : vals)
        printf("    siid=%d piid=%d code=%d hasValue=%d value=%.2f isBool=%d bool=%d\n",
               v.siid, v.piid, v.code, v.hasValue, v.value, v.isBool, v.boolValue);
    Check(ok && vals.size() == 3, "parse 3 property objects");
    if (vals.size() == 3) {
        Check(vals[0].siid == 4 && vals[0].piid == 2 && vals[0].code == 0
              && vals[0].hasValue && vals[0].value == 63.0,
              "property[0] = siid4.piid2 value 63");
        Check(vals[1].value == 3161.0, "property[1] value 3161");
        Check(vals[2].isBool && vals[2].boolValue, "property[2] bool true");
    }
}

int main() {
    printf("=== MiPower selftest ===\n\n");
    TestMd5();
    printf("\n");
    TestAes();
    printf("\n");
    TestMiioKeys();
    printf("\n");
    TestHex();
    printf("\n");
    TestJsonParse();
    printf("\n=== %s (failures=%d) ===\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
