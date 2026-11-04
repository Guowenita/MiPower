// MiioClient.h - miIO 协议客户端（纯协议层，不含业务语义）
#pragma once
#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <string>
#include <vector>

#include "Crypto.h"

namespace mp {

// ─── 错误分类 ──────────────────────────────────────────────
enum class ErrorKind {
    None,
    Timeout,
    NetworkUnreachable,
    ChecksumMismatch,     // token 无效
    DeviceBusy,           // ack timeout
    ProtocolError,        // 响应解析失败
    PropertyUnsupported,  // code -4003 / -4001
    Unknown
};

const wchar_t* ErrorKindText(ErrorKind k);

// ─── 属性引用与值 ──────────────────────────────────────────
struct PropertyRef {
    int  siid = -1;
    int  piid = -1;
};

struct PropertyValue {
    int      siid = -1;
    int      piid = -1;
    int      code = 0;
    bool     hasValue = false;
    double   value = 0.0;
    bool     isBool = false;
    bool     boolValue = false;
    bool     isString = false;
    std::string strValue;
};

// ─── 设备信息（miIO.info）──────────────────────────────────
struct DeviceInfo {
    std::string model;
    std::string firmware;
    std::string hardware;
    std::string mac;
    std::string token;       // 设备回显的 token，用于校验
    std::string ssid;
    int         rssi = 0;
    unsigned int uid = 0;
    bool        valid = false;
};

// ─── miIO 客户端 ───────────────────────────────────────────
class MiioClient {
public:
    MiioClient();
    ~MiioClient();

    // tokenHex 为 32 位十六进制字符串
    bool Open(const std::string& ip, const std::string& tokenHex, int timeoutMs = 3000);
    void Close();
    bool IsOpen() const { return m_socket != INVALID_SOCKET; }

    const std::string& Ip() const { return m_ip; }
    const std::string& TokenHex() const { return m_tokenHex; }

    // 握手：获取 device_id 与服务器时间戳。幂等（30 秒内复用）。
    bool Handshake(ErrorKind& err);

    // 发送命令并返回 result 字段的 JSON 片段
    bool Call(const std::string& method, const std::string& paramsJson,
              std::string& outResult, ErrorKind& err);

    // MIoT 批量读属性（一次往返取多个）
    bool GetProperties(const std::vector<PropertyRef>& props,
                       std::vector<PropertyValue>& outValues, ErrorKind& err);

    // 读取 miIO.info
    bool GetDeviceInfo(DeviceInfo& out, ErrorKind& err);

    // 解析 get_properties 的 result 数组
    static bool ParsePropertyResult(const std::string& resultJson,
                                     std::vector<PropertyValue>& out);

    // 轻量 JSON 字符串取值（取 "key": 后面的字符串值）
    static bool ExtractJsonString(const std::string& json, const char* key,
                                   std::string& out);
    static bool ExtractJsonNumber(const std::string& json, const char* key,
                                  double& out);
    static bool ExtractJsonInt(const std::string& json, const char* key, int& out);
    static bool ExtractJsonUInt(const std::string& json, const char* key,
                                 unsigned int& out);
    static std::string ExtractJsonStringOr(const std::string& json, const char* key);

    // 让主机回应答（心跳用）—— 发一个 hello 包并等待
    bool Ping(ErrorKind& err);

    // 切换 token（热重载用），会重置握手状态
    bool SetToken(const std::string& tokenHex, ErrorKind& err);

private:
    bool SendRaw(const std::vector<uint8_t>& buf, ErrorKind& err);
    bool SendReceive(const std::vector<uint8_t>& sendBuf,
                     std::vector<uint8_t>& recvBuf, ErrorKind& err);
    bool BuildPacket(const std::string& payloadJson, std::vector<uint8_t>& out);
    bool ParsePacket(const std::vector<uint8_t>& data, std::string& outPayload, ErrorKind& err);

    SOCKET      m_socket = INVALID_SOCKET;
    std::string m_ip;
    std::string m_tokenHex;
    MiioKeys    m_keys{};
    std::vector<uint8_t> m_rawToken;

    int           m_timeoutMs = 3000;
    unsigned int  m_deviceId = 0;
    unsigned int  m_serverStamp = 0;
    bool          m_handshaked = false;
    ULONGLONG     m_lastHandshakeTick = 0;
    int           m_msgId = 1;

    // 解析用缓冲（加锁保护）
    std::vector<uint8_t> m_scratch;
};

} // namespace mp
