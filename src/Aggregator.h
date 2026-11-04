// Aggregator.h - 采样聚合、积分、缺口记录与统计
#pragma once
#include <Windows.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace mp {

// ─── 采样点 ────────────────────────────────────────────────
struct PowerSample {
    uint64_t wallClockMs = 0;   // UTC 毫秒
    uint64_t monotonicMs = 0;   // 单调时钟毫秒
    double   watts = 0.0;
    double   voltage = 0.0;
    double   current = 0.0;
    bool     switchOn = false;
    double   energyDevice = 0.0;   // 设备端累计 (kWh)
    bool     valid = true;
};

// ─── 缺口 ──────────────────────────────────────────────────
struct SampleGap {
    uint64_t startMs = 0;
    uint64_t endMs = 0;
    int      failCount = 0;
    std::string reason;
};

// ─── 分钟聚合桶 ────────────────────────────────────────────
struct MinuteBucket {
    uint64_t minuteTs = 0;    // 分钟起始 Unix 秒
    double   avgW = 0.0;
    double   maxW = 0.0;
    double   minW = 0.0;
    double   sumWh = 0.0;
    int      sampleCount = 0;
};

// ─── 统计结果 ──────────────────────────────────────────────
struct PowerStats {
    double minW = 0, maxW = 0, avgW = 0, lastW = 0;
    double integratedKwh = 0;      // 窗口内积分电量 (kWh)
    size_t sampleCount = 0;
    size_t gapCount = 0;
    double coverage = 0.0;         // 数据覆盖率 (有效时长 / 总时长)
    bool   valid = false;
};

// ─── 聚合器 ────────────────────────────────────────────────
class PowerAggregator {
public:
    PowerAggregator();

    // 采样间隔（秒），用于计算积分
    // 这三个 setter 现在会在「设置框提交后」从采集线程被调用，
    // 而 GetStats()/RollMinute() 会在 UI 线程/采集线程读同一批字段，
    // 因此必须与读侧共用 m_mutex，否则运行时改参数会与统计产生数据竞争。
    void SetSampleInterval(int sec) { std::lock_guard<std::mutex> lk(m_mutex); m_intervalSec = sec; }
    // 实时窗口（秒）
    void SetWindow(int sec) { std::lock_guard<std::mutex> lk(m_mutex); m_windowSec = sec; }
    // 历史保留天数
    void SetRetentionDays(int days) { std::lock_guard<std::mutex> lk(m_mutex); m_retentionDays = days; }

    void AddSample(const PowerSample& s);
    void AddGap(uint64_t startMs, uint64_t endMs, int failCount, const std::string& reason);

    // 当前瞬时功率（最后一次采样）
    double GetCurrentWatts() const;
    bool   HasData() const;

    // 窗口统计
    PowerStats GetStats(int windowSec) const;

    // 自进程启动以来的累计积分 (kWh)
    double GetIntegratedKwh() const;

    // 设备端累计电量（权威）
    double GetDeviceEnergyKwh() const;

    // 数据覆盖率
    double GetCoverage() const;

    // ─── 功耗与累计电量关联展示 ─────────────────────────────
    // 取最近 windowSec 秒内的功率曲线（用于迷你趋势图）
    // 返回按时间升序的 (相对秒, 瓦) 序列
    struct TrendPoint {
        double offsetSec = 0;    // 距今的秒数（0 = 最新）
        double watts = 0;
        double energyKwh = 0;   // 该时刻的设备端累计电量
    };
    std::vector<TrendPoint> GetPowerTrend(int windowSec, size_t maxPoints = 60) const;

    // 自进程启动以来的累计电量变化量 (kWh) —— 用于关联展示
    double GetEnergyDeltaKwh() const;

    // 分钟桶（用于持久化）
    std::vector<MinuteBucket> GetMinuteBuckets() const;

    // 载入持久化的分钟桶
    void LoadMinuteBuckets(const std::vector<MinuteBucket>& buckets);

    // 快照（供 UI 无锁读取）
    struct Snapshot {
        PowerSample sample;
        bool        hasData = false;
        uint64_t    updatedMs = 0;     // 单调时钟
    };
    Snapshot GetSnapshot() const;

private:
    mutable std::mutex m_mutex;
    std::deque<PowerSample> m_realtime;
    std::deque<SampleGap>   m_gaps;
    std::deque<MinuteBucket> m_minutes;

    int      m_intervalSec = 2;
    int      m_windowSec = 600;
    int      m_retentionDays = 7;

    double   m_integratedKwh = 0.0;
    double   m_deviceEnergy = 0.0;
    bool     m_hasPrev = false;
    PowerSample m_prev;

    // 快照用原子共享（简单起见用 mutex 保护，读取极快）
    Snapshot m_snapshot;

    void RollMinute(const PowerSample& s);
};

} // namespace mp
