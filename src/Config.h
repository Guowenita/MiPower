// Config.h - 配置管理（UTF-8 ini + 独立 token 文件）
#pragma once
#include <Windows.h>

#include <string>
#include <vector>

namespace mp {

// ─── 单个设备配置 ──────────────────────────────────────────
struct DeviceConfig {
    std::string id;            // 逻辑 id
    std::string ip;
    std::string name;
    std::string modelHint;
    std::string tokenFile;     // 相对配置目录；为空则用 <id>.token
    std::string propertyMap;   // 手动属性覆盖 "4.2,4.1,4.3,4.4,2.1"
    bool        enabled = true;

    std::string TokenFileName() const {
        return tokenFile.empty() ? (id + ".token") : tokenFile;
    }
};

// ─── 全局配置 ──────────────────────────────────────────────
struct AppConfig {
    // General
    int    sampleIntervalSec = 2;
    int    realtimeWindowSec = 600;
    int    historyRetentionDays = 7;
    double pricePerKwh = 0.65;
    bool   enableHistory = true;
    bool   enableDiagnostics = false;
    int    maxRetryIntervalSec = 30;
    int    requestTimeoutMs = 3000;
    int    heartbeatSec = 30;

    // Display
    bool   showLabel = true;
    bool   showUnit = true;
    bool   showDeviceName = false;
    int    decimalPlaces = 1;
    bool   showEnergyInTooltip = true;
    bool   showStatsInTooltip = true;
    int    staleThresholdSec = 30;

    // 功耗与累计电量关联展示
    bool   showTrend = true;      // tooltip 画出功率趋势迷你图
    bool   showCost = false;      // 显示累计电费
    int    trendMinutes = 10;     // 趋势图窗口（分钟）

    std::vector<DeviceConfig> devices;

    DeviceConfig* FindDevice(const std::string& id);
};

// ─── 配置存取 ──────────────────────────────────────────────
class ConfigStore {
public:
    // 配置目录（由 TrafficMonitor 通过 EI_CONFIG_DIR 传入）
    void SetConfigDir(const std::wstring& dir) { m_dir = dir; }
    const std::wstring& ConfigDir() const { return m_dir; }

    // 读配置；不存在时写一份默认配置
    bool Load(AppConfig& cfg);

    // 保存
    bool Save(const AppConfig& cfg) const;

    // token 文件路径
    std::wstring TokenPath(const DeviceConfig& dev) const;

    // 规格缓存路径
    std::wstring SpecCachePath() const;

    // 历史文件路径
    std::wstring HistoryPath() const;

private:
    std::wstring m_dir;
    std::wstring IniPath() const;

    static std::string ReadValue(const std::string& content, const std::string& key);
    static bool WriteDefaultConfig(const std::wstring& path);
};

// ─── Token 提供者 ──────────────────────────────────────────
class TokenProvider {
public:
    // 从 token 文件加载；失败时返回 false
    static bool LoadFromFile(const std::wstring& path, std::string& outToken);

    // 校验 32 位十六进制
    static bool Validate(const std::string& token);

    // 取文件最后修改时间（用于热重载检测）
    static ULONGLONG FileWriteTime(const std::wstring& path);
};

} // namespace mp
