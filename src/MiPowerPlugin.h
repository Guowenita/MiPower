// MiPowerPlugin.h - TrafficMonitor 插件主类与显示项
#pragma once
#include <Windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "PluginInterface.h"
#include "Aggregator.h"
#include "Collector.h"
#include "Config.h"

namespace mp { class Collector; }

// ─────────────────────────────────────────────
// 任务栏显示项
// ─────────────────────────────────────────────
class CMiPowerItem : public IPluginItem {
public:
    void SetPlugin(class CMiPowerPlugin* p) { m_plugin = p; }

    const wchar_t* GetItemName()            const override;
    const wchar_t* GetItemId()              const override { return L"MiPowerW"; }
    const wchar_t* GetItemLableText()       const override;
    const wchar_t* GetItemValueText()       const override;
    const wchar_t* GetItemValueSampleText() const override { return L"9999.9W"; }

private:
    class CMiPowerPlugin* m_plugin = nullptr;

    // 显示文本缓存
    //
    // 关键：TrafficMonitor 拿到的是 const wchar_t* 指针，会在之后使用。
    // 若返回 std::wstring::c_str() 而底层缓冲随后被改写/释放，宿主就读到野指针。
    // 因此改用「固定容量的静态数组 + 双缓冲交换」：
    //   - 指针指向一块永不释放的内存
    //   - 写入时写到备用缓冲，写满后交换，交换后旧缓冲成为备用
    mutable wchar_t m_valueBuf[2][256] = { {}, {} };
    mutable wchar_t m_labelBuf[2][256] = { {}, {} };
    mutable int    m_valueIdx = 0;
    mutable int    m_labelIdx = 0;
    mutable ULONGLONG m_cachedTick = 0;
    mutable std::mutex m_textMutex;

    // 写入双缓冲并返回指针（内存永不释放，宿主可安全持有）
    static const wchar_t* StoreText(wchar_t buf[2][256], int& idx,
                                   const std::wstring& text);
};

// ─────────────────────────────────────────────
// 插件主类
// ─────────────────────────────────────────────
class CMiPowerPlugin : public ITMPlugin {
public:
    CMiPowerPlugin();
    ~CMiPowerPlugin();

    // ── ITMPlugin ──
    IPluginItem*   GetItem(int index) override;
    int            GetItemCount() const { return 1; }
    void           DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    OptionReturn   ShowOptionsDialog(void* hParent) override;
    const wchar_t* GetTooltipInfo() override;
    void           OnInitialize(ITrafficMonitor* pApp) override;
    void           OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;
    void           OnMonitorInfo(const MonitorInfo& monitor_info) override;

    // ── 供显示项访问 ──
    double  GetCurrentWatts() const;
    bool    IsStale() const;
    mp::DeviceHealth GetHealth() const;
    double  GetCoverage() const;
    mp::PowerStats GetStats(int windowSec) const;
    double  GetDeviceEnergyKwh() const;
    double  GetIntegratedKwh() const;
    // 功耗与累计电量关联展示
    std::vector<mp::PowerAggregator::TrendPoint> GetPowerTrend(int windowSec,
                                                                size_t maxPoints = 60) const;
    double  GetEnergyDeltaKwh() const;

    // 状态文本（供 tooltip / 设置框使用）
    std::wstring GetStatusText() const;

    // 读取配置项（不加锁，供已持锁的调用方使用）
    bool ShowUnit() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_cfg.showUnit;
    }

private:
    void StartCollector();
    void StopCollector();
    void EnsureStarted();
    void ApplyConfigToRuntime();   // 设置框提交后把新配置推送到采集器

public:
    // 供显示项访问（friend 或公开成员）
    mutable std::mutex m_mutex;
    mutable mp::AppConfig m_cfg;

    // 采集器访问用独立锁 —— 避免与 m_mutex 嵌套加锁
    // 用 shared_ptr + 弱引用，保证主线程读取时对象不会被并发释放
    std::shared_ptr<mp::Collector> GetCollector() const {
        std::lock_guard<std::mutex> lk(m_collectorMutex);
        return m_collector;
    }

private:
    CMiPowerItem    m_item;
    ITrafficMonitor* m_pTM = nullptr;

    mp::ConfigStore m_store;
    std::shared_ptr<mp::Collector> m_collector;
    mutable std::mutex m_collectorMutex;

    mutable wchar_t m_tooltipBuf[2][2048] = { {}, {} };
    mutable int    m_tooltipIdx = 0;
    mutable ULONGLONG m_tooltipTick = 0;

    std::wstring m_configDir;
    bool m_started = false;
    bool m_pendingStart = false;

    // 陈旧阈值用原子变量，避免读它时需要加锁（防递归加锁）
    std::atomic<int> m_staleThresholdSec{ 30 };
};

// DLL 导出
extern "C" __declspec(dllexport)
ITMPlugin* TMPluginGetInstance();
