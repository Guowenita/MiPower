// Aggregator.cpp - 采样聚合与统计
#include "Aggregator.h"
#include "DiagLog.h"

#include <algorithm>
#include <cmath>

namespace mp {

PowerAggregator::PowerAggregator() = default;

void PowerAggregator::AddSample(const PowerSample& s) {
    std::lock_guard<std::mutex> lk(m_mutex);

    // 梯形积分：比左端点矩形积分更平滑，总量一致
    if (m_hasPrev) {
        double dtHours = (s.monotonicMs - m_prev.monotonicMs) / 3600000.0;
        // 异常间隔（>10 倍标称间隔）视为缺口，不积分
        // 标称采样间隔（秒 → 小时）
        double nominal = m_intervalSec / 36000.0;
        // 采样间隔超过标称值的 3 倍视为「跳变/缺口」，不积分；
        // 否则一律积分。阈值放大 3 倍是为了容忍定时器抖动，
        // 否则正常采样会被误判成缺口，导致积分恒为 0。
        // 积分判据: 采样间隔只要在合理范围内就积分。
        //
        // 坑: 不能用「标称间隔 × N」当阈值 —— 实际采样间隔 = 标称间隔 + 网络往返，
        // 实测标称 2s 时实际 2.25s，用 2s×3=6s 当上限会把正常采样误判成缺口，
        // 导致积分恒为 0。
        // 正确做法: 上限取「标称间隔 + 固定容差」，且缺口由 AddGap 显式记录。
        const double kExtraToleranceH = 5.0 / 3600.0;   // 5 秒容差
        if (dtHours > 0 && dtHours <= nominal + kExtraToleranceH) {
            m_integratedKwh += (m_prev.watts + s.watts) / 2.0 * dtHours / 1000.0;
        }
    }
    m_prev = s;
    m_hasPrev = true;
    m_deviceEnergy = s.energyDevice;

    m_realtime.push_back(s);

    // 按窗口裁剪
    uint64_t cutoff = s.monotonicMs - static_cast<uint64_t>(m_windowSec) * 1000;
    while (!m_realtime.empty() && m_realtime.front().monotonicMs < cutoff)
        m_realtime.pop_front();

    // 裁剪缺口
    uint64_t gapCutoff = s.wallClockMs - static_cast<uint64_t>(m_windowSec) * 1000;
    while (!m_gaps.empty() && m_gaps.front().endMs < gapCutoff)
        m_gaps.pop_front();

    RollMinute(s);

    m_snapshot.sample = s;
    m_snapshot.hasData = true;
    m_snapshot.updatedMs = GetTickCount64();
}

void PowerAggregator::AddGap(uint64_t startMs, uint64_t endMs, int failCount,
                             const std::string& reason) {
    std::lock_guard<std::mutex> lk(m_mutex);
    SampleGap g;
    g.startMs = startMs;
    g.endMs = endMs;
    g.failCount = failCount;
    g.reason = reason;
    m_gaps.push_back(g);

    // 缺口期间积分不可信，标记为未建立有效连接
    m_hasPrev = false;
}

void PowerAggregator::RollMinute(const PowerSample& s) {
    uint64_t minute = s.wallClockMs / 60000;
    if (!m_minutes.empty() && m_minutes.back().minuteTs == minute) {
        MinuteBucket& b = m_minutes.back();
        b.sumWh += s.watts * (m_intervalSec / 3600.0);
        b.avgW = (b.avgW * b.sampleCount + s.watts) / (b.sampleCount + 1);
        b.maxW = std::max(b.maxW, s.watts);
        b.minW = std::min(b.minW, s.watts);
        b.sampleCount++;
    } else {
        MinuteBucket b;
        b.minuteTs = minute;
        b.avgW = s.watts;
        b.maxW = s.watts;
        b.minW = s.watts;
        b.sumWh = s.watts * (m_intervalSec / 3600.0);
        b.sampleCount = 1;
        m_minutes.push_back(b);
    }

    // 按保留天数裁剪
    uint64_t cutoffMin = (s.wallClockMs / 60000) -
                         static_cast<uint64_t>(m_retentionDays) * 24 * 60;
    while (!m_minutes.empty() && m_minutes.front().minuteTs < cutoffMin)
        m_minutes.pop_front();
}

double PowerAggregator::GetCurrentWatts() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_snapshot.hasData ? m_snapshot.sample.watts : 0.0;
}

bool PowerAggregator::HasData() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_snapshot.hasData;
}

PowerAggregator::Snapshot PowerAggregator::GetSnapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_snapshot;
}

PowerStats PowerAggregator::GetStats(int windowSec) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    PowerStats st;

    if (m_realtime.empty())
        return st;

    uint64_t now = m_snapshot.updatedMs;
    uint64_t cutoff = now - static_cast<uint64_t>(windowSec) * 1000;

    std::vector<const PowerSample*> pts;
    double sum = 0;
    for (const auto& p : m_realtime) {
        if (p.monotonicMs < cutoff) continue;
        pts.push_back(&p);
        sum += p.watts;
    }
    if (pts.empty())
        return st;

    st.minW = pts[0]->watts;
    st.maxW = pts[0]->watts;
    for (const auto* p : pts) {
        st.minW = std::min(st.minW, p->watts);
        st.maxW = std::max(st.maxW, p->watts);
    }
    st.avgW = sum / static_cast<double>(pts.size());
    st.lastW = m_snapshot.sample.watts;
    st.sampleCount = pts.size();
    st.valid = true;

    // 窗口内积分
    double integral = 0;
    for (size_t i = 1; i < pts.size(); i++) {
        double dtHours = (pts[i]->monotonicMs - pts[i - 1]->monotonicMs) / 3600000.0;
        if (dtHours > 0 && dtHours < 1.0)
            integral += (pts[i - 1]->watts + pts[i]->watts) / 2.0 * dtHours / 1000.0;
    }
    st.integratedKwh = integral;

    // 缺口统计
    uint64_t gapMs = 0;
    for (const auto& g : m_gaps) {
        if (g.endMs < cutoff) continue;
        st.gapCount++;
        gapMs += (g.endMs > g.startMs) ? (g.endMs - g.startMs) : 0;
    }

    // 覆盖率：有效时长 / 窗口总时长
    double windowMs = static_cast<double>(windowSec) * 1000.0;
    if (windowMs > 0) {
        double covered = windowMs - static_cast<double>(gapMs);
        st.coverage = std::max(0.0, std::min(1.0, covered / windowMs));
    }

    return st;
}

double PowerAggregator::GetIntegratedKwh() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_integratedKwh;
}

double PowerAggregator::GetDeviceEnergyKwh() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_deviceEnergy;
}

double PowerAggregator::GetCoverage() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_realtime.empty()) return 0.0;
    uint64_t now = m_snapshot.updatedMs;
    uint64_t cutoff = now - static_cast<uint64_t>(m_windowSec) * 1000;
    uint64_t gapMs = 0;
    for (const auto& g : m_gaps) {
        if (g.endMs < cutoff) continue;
        gapMs += (g.endMs > g.startMs) ? (g.endMs - g.startMs) : 0;
    }
    double windowMs = static_cast<double>(m_windowSec) * 1000.0;
    if (windowMs <= 0) return 0.0;
    return std::max(0.0, std::min(1.0, (windowMs - gapMs) / windowMs));
}

std::vector<PowerAggregator::TrendPoint> PowerAggregator::GetPowerTrend(
    int windowSec, size_t maxPoints) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<TrendPoint> out;
    if (m_realtime.empty() || windowSec <= 0 || maxPoints == 0)
        return out;

    uint64_t now = m_snapshot.updatedMs;
    uint64_t cutoff = now - static_cast<uint64_t>(windowSec) * 1000;

    // 收集窗口内的样本
    std::vector<const PowerSample*> inWindow;
    for (const auto& s : m_realtime) {
        if (s.monotonicMs >= cutoff && s.valid)
            inWindow.push_back(&s);
    }
    if (inWindow.empty())
        return out;

    // 均匀抽稀到 maxPoints 个（保留首尾）
    //
    // 实现要点: 直接按目标下标算出该取哪个源样本，
    // 不要用「跳过」的方式做 —— 那样会因为重复下标导致漏采。
    const size_t n = inWindow.size();
    out.reserve(n <= maxPoints ? n : maxPoints);

    if (n <= maxPoints) {
        for (const auto* s : inWindow) {
            TrendPoint p;
            p.offsetSec = static_cast<double>(now - s->monotonicMs) / 1000.0;
            p.watts = s->watts;
            p.energyKwh = s->energyDevice;
            out.push_back(p);
        }
    } else {
        for (size_t k = 0; k < maxPoints; k++) {
            size_t idx = (k * (n - 1)) / (maxPoints - 1);
            if (idx >= n) idx = n - 1;
            const PowerSample* s = inWindow[idx];
            TrendPoint p;
            p.offsetSec = static_cast<double>(now - s->monotonicMs) / 1000.0;
            p.watts = s->watts;
            p.energyKwh = s->energyDevice;
            out.push_back(p);
        }
    }
    return out;
}

double PowerAggregator::GetEnergyDeltaKwh() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_realtime.size() < 2) return 0.0;
    // 窗口内首尾的设备端累计电量差
    uint64_t cutoff = m_snapshot.updatedMs - static_cast<uint64_t>(m_windowSec) * 1000;
    const PowerSample* first = nullptr;
    const PowerSample* last = nullptr;
    for (const auto& s : m_realtime) {
        if (s.monotonicMs < cutoff) continue;
        if (!first) first = &s;
        last = &s;
    }
    if (!first || !last || first == last) return 0.0;
    double delta = last->energyDevice - first->energyDevice;
    return delta > 0 ? delta : 0.0;
}

std::vector<MinuteBucket> PowerAggregator::GetMinuteBuckets() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return std::vector<MinuteBucket>(m_minutes.begin(), m_minutes.end());
}

void PowerAggregator::LoadMinuteBuckets(const std::vector<MinuteBucket>& buckets) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_minutes.clear();
    for (const auto& b : buckets)
        m_minutes.push_back(b);
}

} // namespace mp
