// Collector.cpp - 采集线程实现
#include "Collector.h"

#include <cstring>
#include "DiagLog.h"


#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace mp {

// 只有这些字段会影响采集行为。它们变化时必须重开 socket 并重新解析属性映射，
// 否则采集线程会一直拿着启动时那份旧的 IP / token 文件 / 属性表。
static std::string CollectorKey(const AppConfig& c) {
    std::ostringstream oss;
    oss << c.sampleIntervalSec << '|' << c.requestTimeoutMs << '|'
        << c.heartbeatSec << '|' << c.maxRetryIntervalSec << '|';
    for (const auto& d : c.devices) {
        oss << d.enabled << ':' << d.ip << ':' << d.TokenFileName() << ':'
            << d.propertyMap << ';';
    }
    return oss.str();
}

static std::string WideToUtf8(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

const wchar_t* HealthStateText(HealthState s) {
    switch (s) {
        case HealthState::Unknown:    return L"未连接";
        case HealthState::Connecting: return L"连接中";
        case HealthState::Online:     return L"正常";
        case HealthState::Degraded:   return L"部分降级";
        case HealthState::Offline:    return L"离线";
        case HealthState::AuthFailed: return L"Token 失效";
    }
    return L"未知";
}

Collector::Collector() = default;

Collector::~Collector() {
    Stop();
}

void Collector::Start(const AppConfig& cfg, const ConfigStore& store) {
    Stop();
    m_cfg = cfg;
    m_store = store;
    m_stop = false;
    {
        std::lock_guard<std::mutex> lk(m_pendingMutex);
        m_pendingCfg = cfg;
    }
    m_cfgDirty = false;

    m_agg.SetSampleInterval(cfg.sampleIntervalSec);
    m_agg.SetWindow(cfg.realtimeWindowSec);
    m_agg.SetRetentionDays(cfg.historyRetentionDays);

    m_spec.LoadCache(store.SpecCachePath());

    m_thread = std::thread(&Collector::ThreadLoop, this);
}

void Collector::Stop() {
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
}

void Collector::StopNoWait() {
    // 仅置标志。线程可能在 recvfrom 上阻塞（最多 3 秒超时），
    // 进程退出时会被系统回收，这里不能阻塞等待。
    m_stop = true;
}

void Collector::ApplyConfig(const AppConfig& cfg) {
    {
        std::lock_guard<std::mutex> lk(m_pendingMutex);
        m_pendingCfg = cfg;
    }
    // 置位后采集线程会立即结束当前等待并在下一轮循环取用，
    // 不用等旧的采样间隔 / 退避时长走完。
    m_cfgDirty = true;
}

void Collector::ApplyPendingConfig() {
    AppConfig next;
    {
        std::lock_guard<std::mutex> lk(m_pendingMutex);
        next = m_pendingCfg;
    }

    const bool reconnect = (CollectorKey(next) != CollectorKey(m_cfg));
    m_cfg = next;

    // 聚合器侧的窗口/间隔参数立即生效，且保留已有历史与积分，不重建聚合器。
    m_agg.SetSampleInterval(m_cfg.sampleIntervalSec);
    m_agg.SetWindow(m_cfg.realtimeWindowSec);
    m_agg.SetRetentionDays(m_cfg.historyRetentionDays);

    if (reconnect) {
        // IP / token 文件 / 属性映射 / 超时变了：强制重新握手、重新解析规格。
        // 清空 token 时间戳才会让 ApplyTokenIfChanged() 真正重开 socket。
        m_specResolved = false;
        m_tokenWriteTime = 0;
        m_currentToken.clear();
        m_failStreak = 0;
        m_backoffSec = 1;
        ApplyTokenIfChanged();
    }
    DiagLogEx("Collector::ApplyPendingConfig applied interval=%d ip=%s reconnect=%d",
              m_cfg.sampleIntervalSec,
              m_cfg.devices.empty() ? "(none)" : m_cfg.devices[0].ip.c_str(),
              reconnect ? 1 : 0);
}

void Collector::ApplyTokenIfChanged() {
    // 诊断
    {
        DiagLogEx("Collector::ApplyTokenIfChanged enter");
    }
    // 只处理第一个启用的设备（多设备场景在后续版本扩展）
    const DeviceConfig* dev = nullptr;
    for (const auto& d : m_cfg.devices) {
        if (d.enabled) { dev = &d; break; }
    }
    if (!dev) return;

    std::wstring path = m_store.TokenPath(*dev);
    {
        std::lock_guard<std::mutex> lk(m_healthMutex);
        m_tokenPath = dev->TokenFileName();
    }

    ULONGLONG wt = TokenProvider::FileWriteTime(path);
    DiagLogEx("Collector: token path=%ls wt=%llu", path.c_str(), (unsigned long long)wt);
    if (wt == 0) {
        std::lock_guard<std::mutex> lk(m_healthMutex);
        m_tokenStatus = "token 文件缺失";
        return;
    }
    if (wt == m_tokenWriteTime && !m_currentToken.empty())
        return;   // 未变化

    std::string token;
    if (!TokenProvider::LoadFromFile(path, token)) {
        std::lock_guard<std::mutex> lk(m_healthMutex);
        m_tokenStatus = "token 格式无效";
        return;
    }

    m_tokenWriteTime = wt;
    m_currentToken = token;
    {
        std::lock_guard<std::mutex> lk(m_healthMutex);
        m_tokenStatus = "已加载";
    }

    ErrorKind err = ErrorKind::None;
    if (!m_client.Open(dev->ip, token, m_cfg.requestTimeoutMs)) {
        std::lock_guard<std::mutex> lk(m_healthMutex);
        m_tokenStatus = "连接初始化失败";
        return;
    }
    (void)err;
    m_authFailed = false;
    m_failStreak = 0;
    m_backoffSec = 1;
}

bool Collector::DoSample() {
    const DeviceConfig* dev = nullptr;
    for (const auto& d : m_cfg.devices) {
        if (d.enabled) { dev = &d; break; }
    }
    if (!dev) return false;

    ErrorKind err = ErrorKind::None;

    // ① 解析属性映射（首次或型号变化时）
    if (!m_specResolved.load()) {
        m_state = HealthState::Connecting;
        PropertyMap pm;
        if (m_spec.Resolve(m_client, pm, dev->propertyMap)) {
            m_propMap = pm;
            m_specResolved = true;
            {
                std::lock_guard<std::mutex> lk(m_healthMutex);
                m_model = pm.model;
                m_specDiag = m_spec.LastDiagnostics();
            }
            m_spec.SaveCache(m_store.SpecCachePath());
        } else {
            {
                std::lock_guard<std::mutex> lk(m_healthMutex);
                m_specDiag = m_spec.LastDiagnostics();
                m_lastError = "无法确定属性映射";
            }
            m_state = HealthState::Degraded;
            return false;
        }
    }

    // ② 读取设备信息（型号/固件），低频
    {
        static ULONGLONG lastInfoTick = 0;
        ULONGLONG now = GetTickCount64();
        if (lastInfoTick == 0 || (now - lastInfoTick) > 300000) {   // 5 分钟一次
            DeviceInfo info;
            ErrorKind ie = ErrorKind::None;
            if (m_client.GetDeviceInfo(info, ie)) {
                std::lock_guard<std::mutex> lk(m_healthMutex);
                m_model = info.model;
                m_firmware = info.firmware;
                // 设备回显 token 与本地一致 —— 确认 token 有效
                if (!info.token.empty() && info.token != m_currentToken) {
                    m_authFailed = true;
                    m_state = HealthState::AuthFailed;
                    std::lock_guard<std::mutex> lk(m_healthMutex);
                    m_tokenStatus = "token 与设备不匹配";
                    return false;
                }
            }
            lastInfoTick = now;
        }
    }

    // ③ 批量读属性
    std::vector<PropertyRef> props = m_propMap.BuildQueryList();
    if (props.empty()) {
        m_state = HealthState::Degraded;
        return false;
    }

    std::vector<PropertyValue> values;
    if (!m_client.GetProperties(props, values, err)) {
        if (err == ErrorKind::PropertyUnsupported) {
            // 属性不再支持 —— 重新解析规格
            m_specResolved = false;
            m_state = HealthState::Degraded;
        }
        return false;
    }

    // ④ 组装采样
    PowerSample s;
    s.wallClockMs = (uint64_t)time(nullptr) * 1000;
    s.monotonicMs = GetTickCount64();
    s.valid = true;

    bool havePower = false;
    for (const auto& v : values) {
        if (v.code != 0) continue;
        if (v.siid == m_propMap.siidPower && v.piid == m_propMap.piidPower) {
            if (v.hasValue) { s.watts = v.value; havePower = true; }
        } else if (v.siid == m_propMap.siidEnergy && v.piid == m_propMap.piidEnergy) {
            if (v.hasValue) s.energyDevice = v.value / 100.0;   // 0.01kWh → kWh
        } else if (v.siid == m_propMap.siidVoltage && v.piid == m_propMap.piidVoltage) {
            if (v.hasValue) s.voltage = v.value;
        } else if (v.siid == m_propMap.siidCurrent && v.piid == m_propMap.piidCurrent) {
            if (v.hasValue) s.current = v.value / 100.0;   // 0.01A → A
        } else if (v.siid == m_propMap.siidSwitch && v.piid == m_propMap.piidSwitch) {
            if (v.isBool) s.switchOn = v.boolValue;
        }
    }

    if (!havePower) {
        // 属性可读但没拿到功率值 —— 可能是型号变了
        m_specResolved = false;
        m_state = HealthState::Degraded;
        return false;
    }

    DiagLogEx("Collector: sample watts=%.1f V=%.0f A=%.2f E=%.2f kWh",
              s.watts, s.voltage, s.current, s.energyDevice);
    m_agg.AddSample(s);
    m_lastWatts = s.watts;
    m_lastVoltage = s.voltage;
    m_lastCurrent = s.current;
    m_switchOn = s.switchOn;
    m_deviceEnergy = s.energyDevice;
    m_hasData = true;
    m_totalOk++;
    m_consecFail = 0;
    m_lastOkTick = GetTickCount64();
    m_state = HealthState::Online;
    m_authFailed = false;
    m_failStreak = 0;
    m_backoffSec = 1;
    return true;
}

void Collector::ThreadLoop() {
    ApplyTokenIfChanged();

    uint64_t gapStart = 0;
    int gapFailCount = 0;
    bool inGap = false;
    ULONGLONG lastHeartbeat = GetTickCount64();

    // 所有等待都切成 100ms 小片：配置一提交就立即返回，
    // 不用等旧的采样间隔 / 退避时长（最长可达 30 秒）走完。
    auto waitOrConfigChange = [this](int seconds) {
        for (int i = 0; i < seconds * 10 && !m_stop.load() && !m_cfgDirty.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };

    while (!m_stop.load()) {
        // 设置框提交的新配置在这里落到运行态（只由采集线程写 m_cfg）
        if (m_cfgDirty.exchange(false))
            ApplyPendingConfig();

        // token 文件变化检测（热重载）
        static ULONGLONG lastCheck = 0;
        ULONGLONG nowTick = GetTickCount64();
        if (lastCheck == 0 || (nowTick - lastCheck) > 5000) {
            lastCheck = nowTick;
            ULONGLONG wt = 0;
            for (const auto& d : m_cfg.devices) {
                if (d.enabled) { wt = TokenProvider::FileWriteTime(m_store.TokenPath(d)); break; }
            }
            if (wt != 0 && wt != m_tokenWriteTime) {
                ApplyTokenIfChanged();
                m_specResolved = false;
                m_failStreak = 0;
                m_backoffSec = 1;
            }
        }

        bool ok = DoSample();

        if (!ok) {
            ErrorKind err = ErrorKind::None;
            // 区分错误类型
            if (m_authFailed.load()) {
                err = ErrorKind::ChecksumMismatch;
            } else {
                // 用一次轻量 Ping 判断可达性
                ErrorKind pe = ErrorKind::None;
                if (!m_client.Ping(pe))
                    err = pe;
                else
                    err = ErrorKind::ProtocolError;
            }

            m_totalFail++;
            m_consecFail++;
            m_lastErrTick = GetTickCount64();
            {
                std::lock_guard<std::mutex> lk(m_healthMutex);
                m_lastError = WideToUtf8(ErrorKindText(err));
                if (err == ErrorKind::ChecksumMismatch)
                    m_tokenStatus = "token 已失效";
            }

            // Token 失效 —— 停止自动重试，等用户更新文件
            if (err == ErrorKind::ChecksumMismatch) {
                m_state = HealthState::AuthFailed;
                // 退避到最大，但不要完全停（用户更新文件后会恢复）
                waitOrConfigChange(std::max(5, m_cfg.maxRetryIntervalSec));
                continue;
            }

            if (m_state != HealthState::AuthFailed)
                m_state = (m_consecFail.load() >= 3) ? HealthState::Offline
                                                    : HealthState::Connecting;

            // 记录缺口起点
            if (!inGap) {
                inGap = true;
                gapStart = (uint64_t)time(nullptr) * 1000;
                gapFailCount = 0;
            }
            gapFailCount++;

            // 指数退避
            int sleepSec = m_backoffSec;
            m_backoffSec = std::min(m_backoffSec * 2, m_cfg.maxRetryIntervalSec);
            waitOrConfigChange(sleepSec);
            continue;
        }

        // 成功 —— 关闭缺口
        if (inGap) {
            uint64_t gapEnd = (uint64_t)time(nullptr) * 1000;
            m_agg.AddGap(gapStart, gapEnd, gapFailCount, "连接中断");
            inGap = false;
        }

        // 心跳：保持会话
        nowTick = GetTickCount64();
        if (m_cfg.heartbeatSec > 0 &&
            (nowTick - lastHeartbeat) > static_cast<ULONGLONG>(m_cfg.heartbeatSec) * 1000) {
            lastHeartbeat = nowTick;
            ErrorKind he = ErrorKind::None;
            m_client.Ping(he);
        }

        // 等待下一个采样周期（配置变更时立即中断）
        int interval = std::max(1, m_cfg.sampleIntervalSec);
        waitOrConfigChange(interval);
    }
}

DeviceHealth Collector::GetHealth() const {
    DeviceHealth h;
    h.state = m_state.load();
    h.totalFailures = m_totalFail.load();
    h.totalSuccess = m_totalOk.load();
    h.lastSuccessMs = m_lastOkTick.load();
    h.lastErrorMs = m_lastErrTick.load();
    h.lastWatts = m_lastWatts.load();
    h.lastVoltage = m_lastVoltage.load();
    h.lastCurrent = m_lastCurrent.load();
    h.switchOn = m_switchOn.load();
    h.deviceEnergyKwh = m_deviceEnergy.load();
    h.specResolved = m_specResolved.load();
    std::lock_guard<std::mutex> lk(m_healthMutex);
    h.model = m_model;
    h.firmware = m_firmware;
    h.lastError = m_lastError;
    h.specDiagnostics = m_specDiag;
    h.tokenStatus = m_tokenStatus;
    h.tokenFilePath = m_tokenPath;
    if (h.totalSuccess + h.totalFailures > 0)
        h.successRate = static_cast<double>(h.totalSuccess) /
                         (h.totalSuccess + h.totalFailures);
    return h;
}

PowerAggregator::Snapshot Collector::GetSnapshot() const {
    return m_agg.GetSnapshot();
}

double Collector::GetCoverage() const {
    return m_agg.GetCoverage();
}

PowerStats Collector::GetStats(int windowSec) const {
    return m_agg.GetStats(windowSec);
}

std::vector<PowerAggregator::TrendPoint> Collector::GetPowerTrend(
    int windowSec, size_t maxPoints) const {
    return m_agg.GetPowerTrend(windowSec, maxPoints);
}

double Collector::GetEnergyDeltaKwh() const {
    return m_agg.GetEnergyDeltaKwh();
}

double Collector::GetIntegratedKwh() const {
    return m_agg.GetIntegratedKwh();
}

} // namespace mp
