// Config.cpp - 配置管理与 token 加载
#include "Config.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace mp {

DeviceConfig* AppConfig::FindDevice(const std::string& id) {
    for (auto& d : devices)
        if (d.id == id) return &d;
    return nullptr;
}

// ══════════════════════════════════════════════════════════
// ConfigStore
// ══════════════════════════════════════════════════════════
std::wstring ConfigStore::IniPath() const {
    return m_dir.empty() ? L"" : (m_dir + L"\\config.ini");
}

std::wstring ConfigStore::TokenPath(const DeviceConfig& dev) const {
    if (m_dir.empty()) return L"";
    return m_dir + L"\\tokens\\" + std::wstring(dev.TokenFileName().begin(),
                                                dev.TokenFileName().end());
}

std::wstring ConfigStore::SpecCachePath() const {
    return m_dir.empty() ? L"" : (m_dir + L"\\spec_cache.txt");
}

std::wstring ConfigStore::HistoryPath() const {
    return m_dir.empty() ? L"" : (m_dir + L"\\power_history.dat");
}

// 从整份 ini 文本中找 key（兼容 [Section] 无关的扁平查找）
std::string ConfigStore::ReadValue(const std::string& content, const std::string& key) {
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        // 去掉 \r 与注释
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        size_t hash = line.find(';');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            size_t b = s.find_first_not_of(" \t");
            size_t e = s.find_last_not_of(" \t");
            s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
        };
        trim(k); trim(v);
        if (k == key) return v;
    }
    return "";
}

bool ConfigStore::WriteDefaultConfig(const std::wstring& path) {
    std::wstring dir = path.substr(0, path.find_last_of(L"\\"));
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\tokens").c_str(), nullptr);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    // UTF-8 BOM，保证 Windows API 读取中文不乱码
    out << "\xEF\xBB\xBF";
    out << "[General]\n"
        << "SampleIntervalSec=2\n"
        << "RealtimeWindowSec=600\n"
        << "HistoryRetentionDays=7\n"
        << "PricePerKwh=0.65\n"
        << "ShowTrend=1\n"
        << "ShowCost=0\n"
        << "TrendMinutes=10\n"
        << "EnableHistory=1\n"
        << "EnableDiagnostics=0\n"
        << "MaxRetryIntervalSec=30\n"
        << "RequestTimeoutMs=3000\n"
        << "HeartbeatSec=30\n"
        << "\n[Display]\n"
        << "ShowLabel=1\n"
        << "ShowUnit=1\n"
        << "ShowDeviceName=0\n"
        << "DecimalPlaces=1\n"
        << "ShowEnergyInTooltip=1\n"
        << "ShowStatsInTooltip=1\n"
        << "StaleThresholdSec=30\n"
        << "\n[Device.plug-1]\n"
        << "IP=192.168.1.10\n"
        << "Name=Smart Plug\n"
        << "TokenFile=plug-1.token\n"
        << "Enabled=1\n";
    return true;
}

bool ConfigStore::Load(AppConfig& cfg) {
    if (m_dir.empty())
        return false;

    std::wstring iniPath = IniPath();
    HANDLE h = CreateFileW(iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // 首次运行：写默认配置
        WriteDefaultConfig(iniPath);
        cfg = AppConfig();
        cfg.devices.push_back({ "plug-1", "192.168.1.10", "Smart Plug", "", "", "", true });
        return true;
    }

    char buf[65536] = { 0 };
    DWORD read = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);
    std::string content(buf, read);
    // 去 BOM
    if (content.size() >= 3 && (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
        content = content.substr(3);

    auto getInt = [&](const char* k, int def) -> int {
        std::string v = ReadValue(content, k);
        if (v.empty()) return def;
        try { return std::stoi(v); } catch (...) { return def; }
    };
    auto getBool = [&](const char* k, bool def) -> bool {
        std::string v = ReadValue(content, k);
        if (v.empty()) return def;
        return v == "1" || v == "true" || v == "TRUE" || v == "yes";
    };
    auto getDouble = [&](const char* k, double def) -> double {
        std::string v = ReadValue(content, k);
        if (v.empty()) return def;
        try { return std::stod(v); } catch (...) { return def; }
    };

    cfg.sampleIntervalSec     = getInt("SampleIntervalSec", 2);
    cfg.realtimeWindowSec     = getInt("RealtimeWindowSec", 600);
    cfg.historyRetentionDays  = getInt("HistoryRetentionDays", 7);
    cfg.pricePerKwh           = getDouble("PricePerKwh", 0.65);
    cfg.showTrend             = getBool("ShowTrend", true);
    cfg.showCost              = getBool("ShowCost", false);
    cfg.trendMinutes          = (int)getDouble("TrendMinutes", 10);
    if (cfg.trendMinutes < 1) cfg.trendMinutes = 1;
    if (cfg.trendMinutes > 120) cfg.trendMinutes = 120;
    cfg.enableHistory         = getBool("EnableHistory", true);
    cfg.enableDiagnostics     = getBool("EnableDiagnostics", false);
    cfg.maxRetryIntervalSec   = getInt("MaxRetryIntervalSec", 30);
    cfg.requestTimeoutMs      = getInt("RequestTimeoutMs", 3000);
    cfg.heartbeatSec          = getInt("HeartbeatSec", 30);

    cfg.showLabel             = getBool("ShowLabel", true);
    cfg.showUnit              = getBool("ShowUnit", true);
    cfg.showDeviceName        = getBool("ShowDeviceName", false);
    cfg.decimalPlaces         = getInt("DecimalPlaces", 1);
    cfg.showEnergyInTooltip   = getBool("ShowEnergyInTooltip", true);
    cfg.showStatsInTooltip    = getBool("ShowStatsInTooltip", true);
    cfg.staleThresholdSec     = getInt("StaleThresholdSec", 30);

    // 解析 [Device.xxx] 段
    cfg.devices.clear();
    std::istringstream in(content);
    std::string line, curSection;
    DeviceConfig cur;
    bool inDevice = false;
    auto flush = [&]() {
        if (inDevice && !cur.id.empty()) cfg.devices.push_back(cur);
        inDevice = false;
    };
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            flush();
            size_t end = line.find(']');
            curSection = line.substr(1, (end == std::string::npos ? line.size() : end) - 1);
            if (curSection.rfind("Device.", 0) == 0) {
                inDevice = true;
                cur = DeviceConfig();
                cur.id = curSection.substr(7);
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            size_t b = s.find_first_not_of(" \t");
            size_t e = s.find_last_not_of(" \t");
            s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
        };
        trim(k); trim(v);

        if (inDevice) {
            if (k == "IP")            cur.ip = v;
            else if (k == "Name")      cur.name = v;
            else if (k == "ModelHint") cur.modelHint = v;
            else if (k == "TokenFile") cur.tokenFile = v;
            else if (k == "PropertyMap") cur.propertyMap = v;
            else if (k == "Enabled")   cur.enabled = (v == "1" || v == "true");
        }
    }
    flush();

    if (cfg.devices.empty()) {
        DeviceConfig d;
        d.id = "plug-1";
        d.ip = "192.168.1.10";
        d.name = "Smart Plug";
        cfg.devices.push_back(d);
    }
    return true;
}

bool ConfigStore::Save(const AppConfig& cfg) const {
    if (m_dir.empty()) return false;
    std::wstring path = IniPath();
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << "\xEF\xBB\xBF";
        out << "[General]\n"
            << "SampleIntervalSec=" << cfg.sampleIntervalSec << "\n"
            << "RealtimeWindowSec=" << cfg.realtimeWindowSec << "\n"
            << "HistoryRetentionDays=" << cfg.historyRetentionDays << "\n"
            << "PricePerKwh=" << cfg.pricePerKwh << "\n"
            << "ShowTrend=" << (cfg.showTrend ? 1 : 0) << "\n"
            << "ShowCost=" << (cfg.showCost ? 1 : 0) << "\n"
            << "TrendMinutes=" << cfg.trendMinutes << "\n"
            << "EnableHistory=" << (cfg.enableHistory ? 1 : 0) << "\n"
            << "EnableDiagnostics=" << (cfg.enableDiagnostics ? 1 : 0) << "\n"
            << "MaxRetryIntervalSec=" << cfg.maxRetryIntervalSec << "\n"
            << "RequestTimeoutMs=" << cfg.requestTimeoutMs << "\n"
            << "HeartbeatSec=" << cfg.heartbeatSec << "\n"
            << "\n[Display]\n"
            << "ShowLabel=" << (cfg.showLabel ? 1 : 0) << "\n"
            << "ShowUnit=" << (cfg.showUnit ? 1 : 0) << "\n"
            << "ShowDeviceName=" << (cfg.showDeviceName ? 1 : 0) << "\n"
            << "DecimalPlaces=" << cfg.decimalPlaces << "\n"
            << "ShowEnergyInTooltip=" << (cfg.showEnergyInTooltip ? 1 : 0) << "\n"
            << "ShowStatsInTooltip=" << (cfg.showStatsInTooltip ? 1 : 0) << "\n"
            << "StaleThresholdSec=" << cfg.staleThresholdSec << "\n";
        for (const auto& d : cfg.devices) {
            out << "\n[Device." << d.id << "]\n"
                << "IP=" << d.ip << "\n"
                << "Name=" << d.name << "\n"
                << "ModelHint=" << d.modelHint << "\n"
                << "TokenFile=" << d.TokenFileName() << "\n"
                << "PropertyMap=" << d.propertyMap << "\n"
                << "Enabled=" << (d.enabled ? 1 : 0) << "\n";
        }
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    return true;
}

// ══════════════════════════════════════════════════════════
// TokenProvider
// ══════════════════════════════════════════════════════════
bool TokenProvider::Validate(const std::string& token) {
    if (token.size() != 32) return false;
    for (char c : token) {
        if (!isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool TokenProvider::LoadFromFile(const std::wstring& path, std::string& outToken) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::string content;
    std::getline(in, content);
    // 去 BOM 与空白
    if (content.size() >= 3 && (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
        content = content.substr(3);
    auto trim = [](std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
    };
    trim(content);
    if (!Validate(content)) return false;
    outToken = content;
    return true;
}

ULONGLONG TokenProvider::FileWriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        return 0;
    ULARGE_INTEGER t;
    t.LowPart = attr.ftLastWriteTime.dwLowDateTime;
    t.HighPart = attr.ftLastWriteTime.dwHighDateTime;
    return t.QuadPart;
}

} // namespace mp
