// MiioClient.cpp - miIO 协议实现
//
// 报文结构（已由抓包逆向确认，全部为大端）：
//   头 32 字节：
//     0x00  uint16  magic = 0x2131
//     0x02  uint16  报文总长 (len + 32)
//     0x04  uint32  unknown = 0
//     0x08  uint32  device_id  (握手回包 offset 8..11)
//     0x0C  uint32  ts         (握手回包 offset 12..15)
//     0x10  byte[16] checksum = md5(header16 + rawToken + ciphertext)
//   密文 = AES-128-CBC(PKCS7(json + "\0"))
//   key = md5(rawToken), iv = md5(key + rawToken)
//
#include "MiioClient.h"
#include "DiagLog.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

namespace mp {

const wchar_t* ErrorKindText(ErrorKind k) {
    switch (k) {
        case ErrorKind::None:                return L"正常";
        case ErrorKind::Timeout:             return L"超时";
        case ErrorKind::NetworkUnreachable:  return L"网络不可达";
        case ErrorKind::ChecksumMismatch:    return L"Token 无效";
        case ErrorKind::DeviceBusy:          return L"设备忙";
        case ErrorKind::ProtocolError:       return L"协议错误";
        case ErrorKind::PropertyUnsupported: return L"属性不支持";
        default:                             return L"未知错误";
    }
}

// ══════════════════════════════════════════════════════════
// 生命周期
// ══════════════════════════════════════════════════════════
//
// 注意：绝不能在这里调用 WSACleanup()。
// TrafficMonitor 主程序自身也在使用 Winsock，插件在 DLL 卸载或对象析构时
// 调用 WSACleanup 会把整个进程的网络栈关掉 —— 表现为插件的 UDP 请求
// 全部超时（socket 创建成功但收不到任何回应）。
//
// 这里只做 WSAStartup 并用一个进程级计数器记录引用次数，
// 真正的 WSACleanup 交给进程退出时由系统回收。
namespace {

int EnsureWsaStarted() {
    static int refCount = 0;
    if (refCount == 0) {
        WSADATA wsa;
        int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        DiagLogEx("MiioClient: WSAStartup rc=%d err=%d ver=%d.%d",
                  rc, WSAGetLastError(), wsa.wVersion & 0xFF, (wsa.wVersion >> 8) & 0xFF);
    }
    refCount++;
    return refCount;
}

} // namespace

MiioClient::MiioClient() {
    EnsureWsaStarted();
}

MiioClient::~MiioClient() {
    Close();
    // 故意不调用 WSACleanup —— 详见上方说明
}

bool MiioClient::Open(const std::string& ip, const std::string& tokenHex, int timeoutMs) {
    Close();
    m_ip = ip;
    m_tokenHex = tokenHex;
    m_timeoutMs = timeoutMs;
    m_handshaked = false;

    if (!MiioKeys::Derive(tokenHex, m_keys))
        return false;
    FromHex(tokenHex, m_rawToken);

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET)
        return false;

    DWORD tv = static_cast<DWORD>(m_timeoutMs);
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv));

    // 刻意不做 bind()。
    //
    // 踩坑记录: 之前为了「避免端口冲突」显式 bind 到 INADDR_ANY:0，
    // 结果在 TrafficMonitor（管理员权限进程）里 bind() 调用被防火墙
    // 策略挂起 —— 表现为 socket 创建成功但后续所有步骤卡死，
    // 插件永远拿不到数据（任务栏显示 0.0 W (!)）。
    // 而普通权限进程（如 hosttest）里 bind 正常，所以极难定位。
    //
    // UDP 不 bind 也能正常收发：系统会在第一次 sendto 时自动分配源端口，
    // 设备会把响应发回该端口。不需要显式绑定。

    return true;
}

void MiioClient::Close() {
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    m_handshaked = false;
}

bool MiioClient::SetToken(const std::string& tokenHex, ErrorKind& err) {
    MiioKeys k;
    if (!MiioKeys::Derive(tokenHex, k)) {
        err = ErrorKind::ProtocolError;
        return false;
    }
    m_keys = k;
    m_tokenHex = tokenHex;
    FromHex(tokenHex, m_rawToken);
    m_handshaked = false;   // 需要重新握手
    err = ErrorKind::None;
    return true;
}

// ══════════════════════════════════════════════════════════
// 底层收发
// ══════════════════════════════════════════════════════════
bool MiioClient::SendRaw(const std::vector<uint8_t>& buf, ErrorKind& err) {
    if (m_socket == INVALID_SOCKET) {
        err = ErrorKind::NetworkUnreachable;
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(54321);
    addr.sin_addr.s_addr = inet_addr(m_ip.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        err = ErrorKind::NetworkUnreachable;
        return false;
    }
    int n = sendto(m_socket, reinterpret_cast<const char*>(buf.data()),
                    static_cast<int>(buf.size()), 0,
                    reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (n == SOCKET_ERROR) {
        DiagLogEx("MiioClient: sendto failed wsaErr=%d ip=%s", WSAGetLastError(), m_ip.c_str());
        err = ErrorKind::NetworkUnreachable;
        return false;
    }
    err = ErrorKind::None;
    return true;
}

bool MiioClient::SendReceive(const std::vector<uint8_t>& sendBuf,
                             std::vector<uint8_t>& recvBuf, ErrorKind& err) {
    if (!SendRaw(sendBuf, err))
        return false;

    std::vector<uint8_t> buf(4096);
    sockaddr_in from{};
    int fromLen = sizeof(from);
    int n = recvfrom(m_socket, reinterpret_cast<char*>(buf.data()),
                     static_cast<int>(buf.size()), 0,
                     reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e != WSAETIMEDOUT && e != WSAEWOULDBLOCK)
            DiagLogEx("MiioClient: recvfrom failed wsaErr=%d", e);
        err = (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) ? ErrorKind::Timeout
                                                          : ErrorKind::NetworkUnreachable;
        return false;
    }
    buf.resize(n);
    recvBuf.swap(buf);
    err = ErrorKind::None;
    return true;
}

// ══════════════════════════════════════════════════════════
// 封包 / 解包
// ══════════════════════════════════════════════════════════
namespace {

inline void WriteBE16(std::vector<uint8_t>& v, uint16_t val) {
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(val & 0xFF));
}

inline void WriteBE32(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    v.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(val & 0xFF));
}

inline uint32_t ReadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // namespace

bool MiioClient::BuildPacket(const std::string& payloadJson, std::vector<uint8_t>& out) {
    // 明文 = json + '\0'，PKCS7 填充
    std::vector<uint8_t> plain(payloadJson.begin(), payloadJson.end());
    plain.push_back(0x00);
    size_t pad = 16 - (plain.size() % 16);
    plain.insert(plain.end(), pad, static_cast<uint8_t>(pad));

    std::vector<uint8_t> cipher;
    if (!Aes::EncryptCbc(m_keys.key, m_keys.iv, plain.data(), plain.size(), cipher))
        return false;

    uint16_t total = static_cast<uint16_t>(32 + cipher.size());

    std::vector<uint8_t> head;
    head.reserve(16);
    WriteBE16(head, 0x2131);
    WriteBE16(head, total);
    WriteBE32(head, 0);
    WriteBE32(head, m_deviceId);
    WriteBE32(head, m_serverStamp);

    // checksum = md5(header16 + rawToken + ciphertext)
    std::vector<uint8_t> chkInput;
    chkInput.reserve(head.size() + m_rawToken.size() + cipher.size());
    chkInput.insert(chkInput.end(), head.begin(), head.end());
    chkInput.insert(chkInput.end(), m_rawToken.begin(), m_rawToken.end());
    chkInput.insert(chkInput.end(), cipher.begin(), cipher.end());
    uint8_t digest[16];
    Md5::Compute(chkInput.data(), chkInput.size(), digest);

    out.clear();
    out.reserve(32 + cipher.size());
    out.insert(out.end(), head.begin(), head.end());
    out.insert(out.end(), digest, digest + 16);
    out.insert(out.end(), cipher.begin(), cipher.end());
    return true;
}

bool MiioClient::ParsePacket(const std::vector<uint8_t>& data,
                             std::string& outPayload, ErrorKind& err) {
    if (data.size() < 32) {
        err = ErrorKind::ProtocolError;
        return false;
    }
    if (data[0] != 0x21 || data[1] != 0x31) {
        err = ErrorKind::ProtocolError;
        return false;
    }

    std::vector<uint8_t> cipher(data.begin() + 32, data.end());
    if (cipher.empty() || (cipher.size() % 16) != 0) {
        err = ErrorKind::ProtocolError;
        return false;
    }

    std::vector<uint8_t> plain;
    if (!Aes::DecryptCbc(m_keys.key, m_keys.iv, cipher.data(), cipher.size(), plain)) {
        err = ErrorKind::ChecksumMismatch;
        return false;
    }

    // 去掉 PKCS7 填充
    if (!plain.empty()) {
        uint8_t pad = plain.back();
        if (pad > 0 && pad <= 16 && pad <= plain.size())
            plain.resize(plain.size() - pad);
    }
    // 去掉结尾的 '\0'
    while (!plain.empty() && plain.back() == 0x00)
        plain.pop_back();

    outPayload.assign(plain.begin(), plain.end());
    err = ErrorKind::None;
    return true;
}

// ══════════════════════════════════════════════════════════
// 握手
// ══════════════════════════════════════════════════════════
bool MiioClient::Handshake(ErrorKind& err) {
    if (m_socket == INVALID_SOCKET) {
        err = ErrorKind::NetworkUnreachable;
        return false;
    }

    ULONGLONG now = GetTickCount64();
    if (m_handshaked && (now - m_lastHandshakeTick) < 30000) {
        err = ErrorKind::None;
        return true;
    }

    // 32 字节 hello 包：magic + len(32) + 28×0xFF
    std::vector<uint8_t> hello;
    WriteBE16(hello, 0x2131);
    WriteBE16(hello, 32);
    hello.insert(hello.end(), 28, 0xFF);

    std::vector<uint8_t> rsp;
    if (!SendReceive(hello, rsp, err))
        return false;

    if (rsp.size() < 16) {
        err = ErrorKind::ProtocolError;
        return false;
    }

    m_deviceId = ReadBE32(&rsp[8]);
    m_serverStamp = ReadBE32(&rsp[12]);
    m_handshaked = true;
    m_lastHandshakeTick = now;
    err = ErrorKind::None;
    return true;
}

bool MiioClient::Ping(ErrorKind& err) {
    return Handshake(err);
}

// ══════════════════════════════════════════════════════════
// 命令
// ══════════════════════════════════════════════════════════
bool MiioClient::Call(const std::string& method, const std::string& paramsJson,
                      std::string& outResult, ErrorKind& err) {
    if (!Handshake(err))
        return false;

    std::ostringstream oss;
    oss << "{\"id\":" << m_msgId++ << ",\"method\":\"" << method << "\",\"params\":" << paramsJson << "}";

    std::vector<uint8_t> pkt;
    if (!BuildPacket(oss.str(), pkt)) {
        err = ErrorKind::ProtocolError;
        return false;
    }

    std::vector<uint8_t> rsp;
    if (!SendReceive(pkt, rsp, err))
        return false;

    std::string payload;
    if (!ParsePacket(rsp, payload, err)) {
        return false;
    }

    // 提取 "result": 之后的内容
    size_t pos = payload.find("\"result\":");
    if (pos != std::string::npos) {
        pos += 8;   // 跳过 "result":
        while (pos < payload.size() &&
               (payload[pos] == ' ' || payload[pos] == '\t' || payload[pos] == ':'))
            pos++;
        // 找到 result 的值起点（'[' 或 '{'）
        size_t valStart = pos;
        while (valStart < payload.size() &&
               (payload[valStart] == ' ' || payload[valStart] == '\t'))
            valStart++;

        // 用括号配对截取完整的 result 值
        if (valStart < payload.size() &&
            (payload[valStart] == '[' || payload[valStart] == '{')) {
            char open = payload[valStart];
            char close = (open == '[') ? ']' : '}';
            int depth = 0;
            bool inStr = false;
            size_t i = valStart;
            for (; i < payload.size(); i++) {
                char ch = payload[i];
                if (inStr) {
                    if (ch == '\\') { i++; continue; }
                    if (ch == '"') inStr = false;
                    continue;
                }
                if (ch == '"') { inStr = true; continue; }
                if (ch == open) depth++;
                else if (ch == close) {
                    depth--;
                    if (depth == 0) { i++; break; }
                }
            }
            outResult = payload.substr(valStart, i - valStart);
        } else {
            // 非结构化值 —— 取到逗号或结尾
            size_t end = payload.find(',', valStart);
            if (end == std::string::npos) end = payload.size();
            outResult = payload.substr(valStart, end - valStart);
        }
        err = ErrorKind::None;
        return true;
    }

    // 没有 result —— 可能是错误响应
    if (payload.find("\"error\"") != std::string::npos) {
        int code = 0;
        if (ExtractJsonInt(payload, "code", code)) {
            if (code == -4003 || code == -4001) {
                err = ErrorKind::PropertyUnsupported;
            } else if (code == -9999) {
                err = ErrorKind::DeviceBusy;
            } else {
                err = ErrorKind::ProtocolError;
            }
        } else {
            err = ErrorKind::ProtocolError;
        }
        outResult = payload;
        return false;
    }

    err = ErrorKind::ProtocolError;
    outResult = payload;
    return false;
}

// ══════════════════════════════════════════════════════════
// MIoT 属性
// ══════════════════════════════════════════════════════════
bool MiioClient::GetProperties(const std::vector<PropertyRef>& props,
                               std::vector<PropertyValue>& outValues, ErrorKind& err) {
    if (props.empty()) {
        err = ErrorKind::None;
        return true;
    }

    // did 用十进制字符串占位（设备按 siid/piid 匹配，did 不参与判定）
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < props.size(); i++) {
        if (i) oss << ",";
        oss << "{\"did\":\"props\",\"siid\":" << props[i].siid
            << ",\"piid\":" << props[i].piid << "}";
    }
    oss << "]";

    std::string result;
    if (!Call("get_properties", oss.str(), result, err)) {
        DiagLogEx("GetProperties: Call failed err=%d", (int)err);
        return false;
    }

    if (!ParsePropertyResult(result, outValues)) {
        err = ErrorKind::ProtocolError;
        return false;
    }

    // 若所有属性都返回不支持，标记为 PropertyUnsupported
    bool anyOk = false;
    bool anyUnsupported = false;
    for (const auto& v : outValues) {
        if (v.code == 0) anyOk = true;
        if (v.code == -4003 || v.code == -4001) anyUnsupported = true;
    }
    if (!anyOk && anyUnsupported) {
        err = ErrorKind::PropertyUnsupported;
        return false;
    }
    err = ErrorKind::None;
    return true;
}

bool MiioClient::ParsePropertyResult(const std::string& resultJson,
                                     std::vector<PropertyValue>& out) {
    out.clear();
    // 形如: [{"did":"props","siid":4,"piid":2,"code":0,"value":63}]
    size_t pos = resultJson.find('{');
    while (pos != std::string::npos) {
        size_t end = resultJson.find('}', pos);
        if (end == std::string::npos) break;
        std::string obj = resultJson.substr(pos, end - pos + 1);

        PropertyValue pv;
        ExtractJsonInt(obj, "siid", pv.siid);
        ExtractJsonInt(obj, "piid", pv.piid);
        ExtractJsonInt(obj, "code", pv.code);

        size_t vpos = obj.find("\"value\":");
        if (vpos != std::string::npos) {
            vpos += 8;   // 跳过 "value": （8 字符，含冒号）
            while (vpos < obj.size() &&
                   (isspace(static_cast<unsigned char>(obj[vpos])) || obj[vpos] == ':'))
                vpos++;
            if (vpos < obj.size()) {
                if (obj[vpos] == '"') {
                    pv.isString = true;
                    size_t e = obj.find('"', vpos + 1);
                    if (e != std::string::npos) pv.strValue = obj.substr(vpos + 1, e - vpos - 1);
                } else if (obj.compare(vpos, 4, "true") == 0) {
                    pv.isBool = true; pv.boolValue = true;
                } else if (obj.compare(vpos, 5, "false") == 0) {
                    pv.isBool = true; pv.boolValue = false;
                } else {
                    double d = 0;
                    size_t consumed = 0;
                    try {
                        d = std::stod(obj.substr(vpos), &consumed);
                        if (consumed > 0) {
                            pv.hasValue = true;
                            pv.value = d;
                        }
                    } catch (...) { /* 非数值，忽略 */ }
                }
            }
        }
        out.push_back(pv);

        pos = resultJson.find('{', end);
    }
    return !out.empty();
}

bool MiioClient::GetDeviceInfo(DeviceInfo& out, ErrorKind& err) {
    std::string result;
    if (!Call("miIO.info", "[]", result, err))
        return false;

    out.model      = ExtractJsonStringOr(result, "model");
    out.firmware   = ExtractJsonStringOr(result, "fw_ver");
    out.hardware   = ExtractJsonStringOr(result, "hw_ver");
    out.mac        = ExtractJsonStringOr(result, "mac");
    out.token      = ExtractJsonStringOr(result, "token");
    out.ssid       = ExtractJsonStringOr(result, "ssid");
    ExtractJsonInt(result, "rssi", out.rssi);
    ExtractJsonUInt(result, "uid", out.uid);
    out.valid = !out.model.empty();
    err = ErrorKind::None;
    return out.valid;
}

// ══════════════════════════════════════════════════════════
// 极简 JSON 取值
// ══════════════════════════════════════════════════════════
bool MiioClient::ExtractJsonString(const std::string& json, const char* key,
                                   std::string& out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return false;
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return false;
    out = json.substr(pos, end - pos);
    return true;
}

bool MiioClient::ExtractJsonNumber(const std::string& json, const char* key, double& out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t')) pos++;
    try {
        size_t consumed = 0;
        out = std::stod(json.substr(pos), &consumed);
        return consumed > 0;
    } catch (...) { return false; }
}

bool MiioClient::ExtractJsonInt(const std::string& json, const char* key, int& out) {
    double d = 0;
    if (!ExtractJsonNumber(json, key, d)) return false;
    out = static_cast<int>(d);
    return true;
}

std::string MiioClient::ExtractJsonStringOr(const std::string& json, const char* key) {
    std::string s;
    ExtractJsonString(json, key, s);
    return s;
}

bool MiioClient::ExtractJsonUInt(const std::string& json, const char* key,
                                 unsigned int& out) {
    double d = 0;
    if (!ExtractJsonNumber(json, key, d)) return false;
    out = static_cast<unsigned int>(d);
    return true;
}

} // namespace mp
