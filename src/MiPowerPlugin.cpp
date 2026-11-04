// MiPowerPlugin.cpp - TrafficMonitor 插件实现
#include "MiPowerPlugin.h"

#include <cstdio>
#include <iomanip>
#include <sstream>
#include <thread>

#include "DiagLog.h"
#include "OptionsDlg.h"
#include <cstdarg>

using namespace mp;

// ══════════════════════════════════════════════════════════
// 双缓冲文本存储
//
// TrafficMonitor 拿到返回的 const wchar_t* 后会在稍后使用（绘制 tooltip /
// 任务栏文字）。若该指针指向的内存被改写或释放，宿主就会读到野指针 ——
// 在 TM 里表现为进程无崩溃记录地直接消失。
//
// 做法：用两块固定容量的 wchar_t 数组，写入非当前块，然后交换索引。
// 两块内存都是插件对象的成员，生命周期与 DLL 一致，永不释放。
// ══════════════════════════════════════════════════════════
const wchar_t* CMiPowerItem::StoreText(wchar_t buf[2][256], int& idx,
                                       const std::wstring& text) {
    int next = 1 - idx;
    size_t n = text.size();
    if (n >= 256) n = 255;
    memcpy(buf[next], text.c_str(), n * sizeof(wchar_t));
    buf[next][n] = L'\0';
    idx = next;
    return buf[idx];
}

static const wchar_t* StoreTextBig(wchar_t buf[2][2048], int& idx,
                                   const std::wstring& text) {
    int next = 1 - idx;
    size_t n = text.size();
    if (n >= 2048) n = 2047;
    memcpy(buf[next], text.c_str(), n * sizeof(wchar_t));
    buf[next][n] = L'\0';
    idx = next;
    return buf[idx];
}



// ══════════════════════════════════════════════════════════
// 工具
// ══════════════════════════════════════════════════════════
static std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w(n, L'\0');           // 分配 n 个（含结尾 NUL）
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(n - 1);                    // 去掉结尾 NUL
    return w;
}

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n, '\0');             // 分配 n 个（含结尾 NUL）
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.resize(n - 1);
    return s;
}

// ══════════════════════════════════════════════════════════
// CMiPowerItem
// ══════════════════════════════════════════════════════════
const wchar_t* CMiPowerItem::GetItemName() const {
    return L"米家插座功率";
}

const wchar_t* CMiPowerItem::GetItemLableText() const {
    if (!m_plugin) return L"功率:";
    std::lock_guard<std::mutex> lk(m_plugin->m_mutex);
    if (!m_plugin->m_cfg.showLabel)
        return L"";

    if (m_plugin->m_cfg.showDeviceName) {
        std::wstring name = L"功率:";
        for (const auto& d : m_plugin->m_cfg.devices) {
            if (d.enabled && !d.name.empty()) { name = Widen(d.name) + L" 功率:"; break; }
        }
        std::lock_guard<std::mutex> tl(m_textMutex);
        return StoreText(m_labelBuf, m_labelIdx, name);
    }
    return L"功率:";
}

const wchar_t* CMiPowerItem::GetItemValueText() const {
    if (!m_plugin) return L"--";

    ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> tl(m_textMutex);
        // 每秒刷新一次显示文本
        if (now - m_cachedTick < 1000 && m_valueBuf[m_valueIdx][0] != L'\0')
            return m_valueBuf[m_valueIdx];
        m_cachedTick = now;
    }

    // 注意：不能持有 m_mutex 时再调用会加锁的成员函数（递归加锁 = 未定义行为）
    int decimalPlaces;
    {
        std::lock_guard<std::mutex> lk(m_plugin->m_mutex);
        decimalPlaces = m_plugin->m_cfg.decimalPlaces;
    }

    mp::DeviceHealth h = m_plugin->GetHealth();
    bool stale = m_plugin->IsStale();
    std::wstring text;

    if (h.state == HealthState::AuthFailed) {
        text = L"Token失效";
    } else if (stale || h.state == HealthState::Offline) {
        // 陈旧或离线：显示最后已知值 + 标记，不伪装成实时
        std::wostringstream oss;
        oss << std::fixed << std::setprecision(decimalPlaces)
            << h.lastWatts;
        text = oss.str();
        if (m_plugin->ShowUnit()) text += L" W";
        text += L" (!)";
    } else {
        double w = m_plugin->GetCurrentWatts();
        std::wostringstream oss;
        oss << std::fixed << std::setprecision(decimalPlaces) << w;
        text = oss.str();
        if (m_plugin->ShowUnit()) text += L" W";
    }

    std::lock_guard<std::mutex> tl(m_textMutex);
    return StoreText(m_valueBuf, m_valueIdx, text);
}

// ══════════════════════════════════════════════════════════
// CMiPowerPlugin
// ══════════════════════════════════════════════════════════
CMiPowerPlugin::CMiPowerPlugin() {
    m_item.SetPlugin(this);
}

CMiPowerPlugin::~CMiPowerPlugin() {
    DiagLogEx("CMiPowerPlugin destructor");
    // 注意：析构可能发生在 DLL 卸载期（loader lock 内），不能 join 线程
    std::shared_ptr<mp::Collector> c;
    {
        std::lock_guard<std::mutex> lk(m_collectorMutex);
        c = m_collector;
        m_collector.reset();
    }
    if (c) c->StopNoWait();
}

IPluginItem* CMiPowerPlugin::GetItem(int index) {
    if (index == 0) return &m_item;
    return nullptr;
}

void CMiPowerPlugin::DataRequired() {
    EnsureStarted();
}

void CMiPowerPlugin::OnInitialize(ITrafficMonitor* pApp) {
    m_pTM = pApp;
    EnsureStarted();
}

void CMiPowerPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) {
    if (index == EI_CONFIG_DIR && data) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_configDir = data;
        m_started = false;
        m_pendingStart = true;
        DiagLogEx("OnExtenedInfo(EI_CONFIG_DIR): %ls", data);
    }
}

void CMiPowerPlugin::OnMonitorInfo(const MonitorInfo&) {
    EnsureStarted();
}

void CMiPowerPlugin::EnsureStarted() {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_started || m_configDir.empty())
            return;
        m_started = true;

        // 读配置
        m_store.SetConfigDir(m_configDir);
        if (!m_store.Load(m_cfg)) {
            m_cfg = mp::AppConfig();
            DeviceConfig d;
            d.id = "plug-1";
            d.ip = "192.168.1.10";
            d.name = "Smart Plug";
            m_cfg.devices.push_back(d);
        }
        m_staleThresholdSec = m_cfg.staleThresholdSec;
        DiagLogEx("EnsureStarted: config loaded, devices=%zu", m_cfg.devices.size());
    }
    StartCollector();
}

void CMiPowerPlugin::StartCollector() {
    std::lock_guard<std::mutex> lk(m_collectorMutex);
    if (m_collector) return;
    auto c = std::make_shared<Collector>();
    DiagLogEx("StartCollector: creating collector");
    c->Start(m_cfg, m_store);
    m_collector = c;
    DiagLogEx("StartCollector: done");
}

void CMiPowerPlugin::StopCollector() {
    std::shared_ptr<mp::Collector> c;
    {
        std::lock_guard<std::mutex> lk(m_collectorMutex);
        c = m_collector;
        m_collector.reset();
    }
    // 在锁外停止与 join，避免死锁
    if (c) c->Stop();
}

const wchar_t* CMiPowerPlugin::GetInfo(PluginInfoIndex index) {
    switch (index) {
        case TMI_NAME:        return L"MiPower 功耗插件";
        case TMI_DESCRIPTION: return L"实时采集米家/酷控智能插座功耗，支持多属性与历史统计";
        case TMI_AUTHOR:      return L"MiPower";
        case TMI_COPYRIGHT:   return L"MIT";
        case TMI_VERSION:     return L"1.0.0";
        case TMI_URL:         return L"";
        default:              return L"";
    }
}

double CMiPowerPlugin::GetCurrentWatts() const {
    auto c = GetCollector();
    if (!c) return 0.0;
    auto snap = c->GetSnapshot();
    return snap.hasData ? snap.sample.watts : 0.0;
}

bool CMiPowerPlugin::IsStale() const {
    auto c = GetCollector();
    if (!c) return true;
    auto snap = c->GetSnapshot();
    if (!snap.hasData) return true;
    ULONGLONG age = GetTickCount64() - snap.updatedMs;
    // 用原子变量读阈值，避免递归加锁
    int threshold = m_staleThresholdSec.load();
    return age > static_cast<ULONGLONG>(threshold) * 1000;
}

mp::DeviceHealth CMiPowerPlugin::GetHealth() const {
    auto c = GetCollector();
    if (!c) return mp::DeviceHealth();
    return c->GetHealth();
}

double CMiPowerPlugin::GetCoverage() const {
    auto c = GetCollector();
    if (!c) return 0.0;
    return c->GetCoverage();
}

mp::PowerStats CMiPowerPlugin::GetStats(int windowSec) const {
    auto c = GetCollector();
    if (!c) return mp::PowerStats();
    return c->GetStats(windowSec);
}

double CMiPowerPlugin::GetDeviceEnergyKwh() const {
    auto c = GetCollector();
    if (!c) return 0.0;
    return c->GetHealth().deviceEnergyKwh;
}

std::vector<mp::PowerAggregator::TrendPoint> CMiPowerPlugin::GetPowerTrend(
    int windowSec, size_t maxPoints) const {
    auto c = GetCollector();
    if (!c) return {};
    return c->GetPowerTrend(windowSec, maxPoints);
}

double CMiPowerPlugin::GetEnergyDeltaKwh() const {
    auto c = GetCollector();
    if (!c) return 0.0;
    return c->GetEnergyDeltaKwh();
}

double CMiPowerPlugin::GetIntegratedKwh() const {
    auto c = GetCollector();
    if (!c) return 0.0;
    return c->GetIntegratedKwh();
}

std::wstring CMiPowerPlugin::GetStatusText() const {
    mp::DeviceHealth h = GetHealth();
    std::wostringstream oss;
    oss << HealthStateText(h.state);
    if (!h.model.empty()) oss << L" · " << Widen(h.model);
    return oss.str();
}

const wchar_t* CMiPowerPlugin::GetTooltipInfo() {
    ULONGLONG now = GetTickCount64();
    if (now - m_tooltipTick < 1000 && m_tooltipBuf[m_tooltipIdx][0] != L'\0')
        return m_tooltipBuf[m_tooltipIdx];
    m_tooltipTick = now;

    mp::AppConfig cfg;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        cfg = m_cfg;                 // 拷贝一份，避免长时间持锁
    }

    mp::DeviceHealth h = GetHealth();
    std::wostringstream oss;

    // 标题
    std::wstring name = L"米家插座";
    for (const auto& d : cfg.devices) {
        if (d.enabled && !d.name.empty()) { name = Widen(d.name); break; }
    }
    oss << name << L"\n";
    oss << L"───────────────\n";

    // 状态行
    oss << L"状态        " << HealthStateText(h.state);
    if (!h.lastError.empty()) oss << L" (" << Widen(h.lastError) << L")";
    oss << L"\n";

    if (h.state == HealthState::AuthFailed) {
        oss << L"\nToken 已失效，请更新 token 文件后\n插件会自动重新加载：\n"
            << Widen(h.tokenFilePath) << L"\n";
    } else {
        // 当前读数
        oss << L"当前功率    " << std::fixed << std::setprecision(cfg.decimalPlaces)
            << h.lastWatts << L" W\n";
        if (h.lastVoltage > 0)
            oss << L"电压/电流   " << std::fixed << std::setprecision(0)
                << h.lastVoltage << L" V / " << std::setprecision(2) << h.lastCurrent << L" A\n";
        oss << L"开关状态    " << (h.switchOn ? L"开启" : L"关闭") << L"\n";

        // 电量
        if (cfg.showEnergyInTooltip && h.deviceEnergyKwh > 0) {
            oss << L"设备端累计  " << std::fixed << std::setprecision(2)
                << h.deviceEnergyKwh << L" kWh\n";
        }

        // ─── 功耗与累计电量关联展示 ───────────────────────────
        // 迷你趋势图：用 Unicode 块字符画最近 N 分钟的功率曲线
        if (cfg.showTrend) {
            int winSec = cfg.trendMinutes * 60;
            auto trend = GetPowerTrend(winSec, 40);
            if (trend.size() >= 2) {
                double lo = trend.front().watts;
                double hi = trend.front().watts;
                for (const auto& p : trend) {
                    if (p.watts < lo) lo = p.watts;
                    if (p.watts > hi) hi = p.watts;
                }
                double range = hi - lo;
                if (range < 1.0) { lo -= 0.5; hi += 0.5; range = hi - lo; }

                // 8 级块字符
                static const wchar_t* kBlocks[] = {
                    L"\u2581", L"\u2582", L"\u2583", L"\u2584",
                    L"\u2585", L"\u2586", L"\u2587", L"\u2588"
                };
                oss << L"\n功耗趋势    ";
                // trend 是时间升序（旧->新），但 offsetSec 是"距今秒数"（递减）
                // 画图时从旧到新，末尾是最新
                for (const auto& p : trend) {
                    double norm = (p.watts - lo) / range;
                    int idx = static_cast<int>(norm * 7.0 + 0.5);
                    if (idx < 0) idx = 0;
                    if (idx > 7) idx = 7;
                    oss << kBlocks[idx];
                }
                oss << L"\n";
                oss << L"            " << std::fixed << std::setprecision(0)
                    << lo << L" ~ " << hi << L" W"
                    << L"   (近 " << cfg.trendMinutes << L" 分钟)\n";

                // 关联：这段时间消耗了多少电、折合多少钱
                //
                // 注意: 不能用设备端累计电量的差值 —— 米家设备的累计电量
                // 只有 0.01 kWh 精度，短时间内不变，差值恒为 0。
                // 必须用本地梯形积分（精度到 0.001 kWh）。
                double dKwh = GetIntegratedKwh();
                if (dKwh > 0) {
                    oss << L"            累计用电  " << std::fixed << std::setprecision(3)
                        << dKwh << L" kWh";
                    if (cfg.showCost && cfg.pricePerKwh > 0) {
                        oss << L"   电费 ¥" << std::fixed << std::setprecision(2)
                            << (dKwh * cfg.pricePerKwh);
                    }
                    oss << L"\n";
                }
            }
        }

        // 累计电费（基于设备端累计电量）
        if (cfg.showCost && cfg.pricePerKwh > 0 && h.deviceEnergyKwh > 0) {
            oss << L"累计电费    ¥" << std::fixed << std::setprecision(2)
                << (h.deviceEnergyKwh * cfg.pricePerKwh)
                << L"  (" << std::fixed << std::setprecision(2) << cfg.pricePerKwh
                << L" 元/kWh)\n";
        }

        // 统计
        if (cfg.showStatsInTooltip) {
            auto statLine = [&](int sec, const wchar_t* label) {
                mp::PowerStats st = GetStats(sec);
                if (!st.valid) return;
                oss << L"  " << label << L"   min " << std::fixed << std::setprecision(1)
                    << st.minW << L"  avg " << st.avgW << L"  max " << st.maxW << L" W\n";
            };
            oss << L"\n统计\n";
            statLine(600, L"10分钟");
            statLine(3600, L"1小时 ");
            statLine(86400, L"24小时");
        }

        // 数据完整性
        double cov = GetCoverage();
        oss << L"\n数据覆盖率  " << std::fixed << std::setprecision(1)
            << (cov * 100.0) << L"%\n";
        oss << L"成功/失败  " << h.totalSuccess << L" / " << h.totalFailures << L"\n";
    }

    // 设备信息
    if (!h.model.empty()) {
        oss << L"\n型号        " << Widen(h.model) << L"\n";
        if (!h.firmware.empty())
            oss << L"固件        " << Widen(h.firmware) << L"\n";
    }

    std::wstring tip = oss.str();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return StoreTextBig(m_tooltipBuf, m_tooltipIdx, tip);
    }
}

// 设置框「确定/应用」后调用：把已在锁内写回的 m_cfg 推送到运行中的采集器。
// 之前这里只返回 OR_OPTION_CHANGED，而 Collector::Start() 只在启动时拷贝
// 了一份 AppConfig，于是采样间隔 / 设备 IP / token / 属性映射这些
// 采集侧参数改了配置也不会生效 —— 这正是「UI 参数调整未生效」的根因。
void CMiPowerPlugin::ApplyConfigToRuntime() {
    mp::AppConfig cfg;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        cfg = m_cfg;
        m_staleThresholdSec = m_cfg.staleThresholdSec;
    }
    auto c = GetCollector();
    if (c) c->ApplyConfig(cfg);
    DiagLogEx("ApplyConfigToRuntime: pushed interval=%d devices=%zu",
              cfg.sampleIntervalSec, cfg.devices.size());
}

ITMPlugin::OptionReturn CMiPowerPlugin::ShowOptionsDialog(void* hParent) {
    HWND hwnd = static_cast<HWND>(hParent);
    if (!GetCollector()) {
        // 尚未启动（配置目录未知），先加载一次配置
        EnsureStarted();
    }
    if (OptionsDlg::Show(hwnd, m_cfg, m_store, this)) {
        ApplyConfigToRuntime();
        return OR_OPTION_CHANGED;
    }
    return OR_OPTION_UNCHANGED;
}

namespace {

CMiPowerPlugin* g_instance = nullptr;
std::atomic<bool> g_unloading{ false };

} // namespace

// ══════════════════════════════════════════════════════════
// DLL 导出
// ══════════════════════════════════════════════════════════
extern "C" __declspec(dllexport)
ITMPlugin* TMPluginGetInstance() {
    // 故意泄漏：避免 DLL 卸载时执行析构（在 loader lock 内 join 线程会死锁）
    if (!g_instance)
        g_instance = new CMiPowerPlugin();
    return g_instance;
}

// ══════════════════════════════════════════════════════════
// DllMain
//
// 关键点：插件实例是函数内静态对象，其析构会在 DLL 卸载时执行。
// 若在析构里 join 采集线程，会在 loader lock 内等待线程结束 —— 而该线程
// 需要执行 DLL 里的代码才能退出，形成死锁（TM 表现为闪退）。
//
// 解决：卸载阶段只置停止标志，不 join；由采集线程自行退出。
// 同时静态对象改为「泄漏式」单例（new 而不 delete），彻底避免卸载期析构。
// ══════════════════════════════════════════════════════════
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            DiagLogEx("DllMain: DLL_PROCESS_ATTACH");
            break;

        case DLL_PROCESS_DETACH:
            // lpReserved 非空表示进程退出（此时系统会终止所有线程）
            g_unloading = true;
            DiagLogEx("DllMain: DLL_PROCESS_DETACH (lpReserved=%p)", lpReserved);
            break;

        default:
            break;
    }
    return TRUE;
}
