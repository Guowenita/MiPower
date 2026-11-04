// Collector.h - 采集线程：采样、退避重连、token 热重载
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "Aggregator.h"
#include "Config.h"
#include "MiioClient.h"
#include "MiotSpec.h"

namespace mp {

// ─── 健康状态 ──────────────────────────────────────────────
enum class HealthState {
    Unknown,
    Connecting,
    Online,
    Degraded,       // 能连上但部分属性不可读
    Offline,
    AuthFailed      // token 无效
};

const wchar_t* HealthStateText(HealthState s);

struct DeviceHealth {
    HealthState state = HealthState::Unknown;
    int      consecutiveFailures = 0;
    int      totalFailures = 0;
    int      totalSuccess = 0;
    uint64_t lastSuccessMs = 0;
    uint64_t lastErrorMs = 0;
    std::string lastError;
    std::string model;
    std::string firmware;
    double   lastWatts = 0.0;
    double   lastVoltage = 0.0;
    double   lastCurrent = 0.0;
    bool     switchOn = false;
    double   deviceEnergyKwh = 0.0;
    bool     specResolved = false;
    std::string specDiagnostics;
    std::string tokenFilePath;
    std::string tokenStatus;   // "已加载" / "文件缺失" / "格式错误" / "已失效"
    double   successRate = 0.0;
};

// ─── 采集器 ────────────────────────────────────────────────
class Collector {
public:
    Collector();
    ~Collector();

    void Start(const AppConfig& cfg, const ConfigStore& store);
    void Stop();
    // 只置停止标志，不等待线程结束 —— 用于 DLL 卸载等不能阻塞的场景
    void StopNoWait();

    // 运行时热应用新配置（设置框提交后调用，可从 UI 线程调用）。
    // 只投递一份待生效配置，真正的切换由采集线程在循环内完成，
    // 这样 m_cfg 始终只被采集线程读写，无需为它加锁。
    void ApplyConfig(const AppConfig& cfg);

    DeviceHealth GetHealth() const;
    PowerAggregator::Snapshot GetSnapshot() const;
    double GetCoverage() const;
    PowerStats GetStats(int windowSec) const;
    double GetIntegratedKwh() const;
    // 功耗与累计电量关联展示
    std::vector<PowerAggregator::TrendPoint> GetPowerTrend(int windowSec,
                                                            size_t maxPoints = 60) const;
    double GetEnergyDeltaKwh() const;

    // 设置主窗口句柄（用于弹出提示）
    void SetHostWindow(void* hwnd) { m_hostWnd = hwnd; }

private:
    void ThreadLoop();
    bool DoSample();
    void ApplyTokenIfChanged();
    void ApplyPendingConfig();   // 仅在采集线程内执行

    AppConfig      m_cfg;        // 仅采集线程读写
    ConfigStore    m_store;

    // UI 线程投递的新配置；m_cfgDirty 置位后采集线程会立刻中断等待并取用
    std::mutex        m_pendingMutex;
    AppConfig         m_pendingCfg;
    std::atomic<bool> m_cfgDirty{ false };
    MiioClient     m_client;
    MiotSpecResolver m_spec;
    PropertyMap    m_propMap;

    std::thread       m_thread;
    std::atomic<bool> m_stop{ false };

    std::atomic<int>  m_failStreak{ 0 };
    int               m_backoffSec = 1;

    ULONGLONG m_tokenWriteTime = 0;
    std::string m_currentToken;

    std::atomic<HealthState> m_state{ HealthState::Unknown };
    std::atomic<int>  m_totalFail{ 0 };
    std::atomic<int>  m_totalOk{ 0 };
    std::atomic<uint64_t> m_lastOkTick{ 0 };
    std::atomic<int>  m_consecFail{ 0 };
    std::atomic<bool> m_authFailed{ false };
    std::atomic<bool> m_specResolved{ false };
    std::atomic<bool> m_hasData{ false };
    std::atomic<double> m_lastWatts{ 0.0 };
    std::atomic<double> m_lastVoltage{ 0.0 };
    std::atomic<double> m_lastCurrent{ 0.0 };
    std::atomic<bool> m_switchOn{ false };
    std::atomic<double> m_deviceEnergy{ 0.0 };

    mutable std::mutex m_healthMutex;
    std::string m_model, m_firmware, m_lastError, m_specDiag, m_tokenStatus, m_tokenPath;
    std::atomic<uint64_t> m_lastErrTick{ 0 };

    PowerAggregator m_agg;

    void* m_hostWnd = nullptr;
};

} // namespace mp
