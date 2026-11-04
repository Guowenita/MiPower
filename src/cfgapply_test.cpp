// cfgapply_test.cpp — 验证「UI 参数修改 → 配置保存 → 运行态生效」全链路
//
// 用法:
//   cfgapply_test.exe save
//   cfgapply_test.exe ui
//   cfgapply_test.exe runtime <真实插件配置目录>   (只读其 token 文件)
//   cfgapply_test.exe all     <真实插件配置目录>
//
// 为什么需要这个测试:
//   之前设置框只改宿主 m_cfg 并写 ini，Collector 却拿着启动时那份
//   AppConfig 副本，采集侧参数（采样间隔/IP/token/属性映射）改了不生效。
//   本测试逐段确认参数确实被传递并应用。
#include <Windows.h>
#include <commctrl.h>

#include <cstdio>
#include <string>
#include <thread>

#include "Config.h"
#include "Collector.h"
#include "OptionsDlg.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace mp;

// 与 OptionsDlg.cpp 匿名枚举保持一致（IDC_STATIC = 1000 起）
enum {
    kStatic = 1000,
    kIp = 1001, kToken = 1002, kName = 1003, kInterval = 1004, kDecimal = 1005,
    kShowLabel = 1006, kShowUnit = 1007, kShowDevName = 1008,
    kShowEnergy = 1009, kShowStats = 1010, kHistory = 1011,
    kPrice = 1012, kPropMap = 1013, kStatus = 1014,
    kTest = 1015, kOk = 1016, kCancel = 1017, kOpenToken = 1018,
    kApply = 1019, kShowTrend = 1020, kShowCost = 1021, kTrendMin = 1022
};

static int g_fail = 0;

static void Check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::string W2U(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.resize(n - 1);
    return s;
}

static std::wstring U2W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(n - 1);
    return w;
}

static std::string ReadFileA(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return "";
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static bool WriteFileA(const std::wstring& path, const std::string& s) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}

static std::wstring MakeTempDir(const wchar_t* tag) {
    wchar_t tmp[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, tmp);
    wchar_t dir[MAX_PATH] = { 0 };
    swprintf_s(dir, L"%sMiPower_%s_%lu", tmp, tag, (unsigned long)GetCurrentProcessId());
    CreateDirectoryW(dir, nullptr);
    std::wstring tokens = std::wstring(dir) + L"\\tokens";
    CreateDirectoryW(tokens.c_str(), nullptr);
    return dir;
}

static DeviceConfig MakeDevice(const std::string& ip, const std::string& propMap) {
    DeviceConfig d;
    d.id = "plug-1";
    d.ip = ip;
    d.name = "Smart Plug";
    d.tokenFile = "plug-1.token";
    d.propertyMap = propMap;
    d.enabled = true;
    return d;
}

static AppConfig MakeEditedConfig(const std::string& ip) {
    AppConfig c;
    c.sampleIntervalSec = 7;
    c.pricePerKwh = 1.25;
    c.showTrend = false;
    c.showCost = true;
    c.trendMinutes = 25;
    c.showLabel = false;
    c.showUnit = false;
    c.showDeviceName = true;
    c.decimalPlaces = 2;
    c.showEnergyInTooltip = false;
    c.showStatsInTooltip = false;
    c.enableHistory = false;
    c.devices.push_back(MakeDevice(ip, "4.2,4.1,4.3,4.4,2.1"));
    return c;
}

static void CheckConfigEquals(const AppConfig& a, const AppConfig& b, const char* tag) {
    char name[160];
    auto one = [&](bool ok, const char* field) {
        snprintf(name, sizeof(name), "%s: %s", tag, field);
        Check(ok, name);
    };
    one(a.sampleIntervalSec == b.sampleIntervalSec, "sampleIntervalSec");
    one(a.pricePerKwh == b.pricePerKwh, "pricePerKwh");
    one(a.showTrend == b.showTrend, "showTrend");
    one(a.showCost == b.showCost, "showCost");
    one(a.trendMinutes == b.trendMinutes, "trendMinutes");
    one(a.showLabel == b.showLabel, "showLabel");
    one(a.showUnit == b.showUnit, "showUnit");
    one(a.showDeviceName == b.showDeviceName, "showDeviceName");
    one(a.decimalPlaces == b.decimalPlaces, "decimalPlaces");
    one(a.showEnergyInTooltip == b.showEnergyInTooltip, "showEnergyInTooltip");
    one(a.showStatsInTooltip == b.showStatsInTooltip, "showStatsInTooltip");
    one(a.enableHistory == b.enableHistory, "enableHistory");
    one(a.devices.size() == b.devices.size(), "devices.size");
    if (a.devices.size() == b.devices.size() && !a.devices.empty()) {
        one(a.devices[0].ip == b.devices[0].ip, "devices[0].ip");
        one(a.devices[0].name == b.devices[0].name, "devices[0].name");
        one(a.devices[0].propertyMap == b.devices[0].propertyMap, "devices[0].propertyMap");
    }
}

// ── Part A: ConfigStore 全字段往返 ─────────────────────────
static void TestSaveRoundTrip() {
    printf("\n=== A. ConfigStore Save -> Load 全字段往返 ===\n");
    std::wstring dir = MakeTempDir(L"save");
    ConfigStore store;
    store.SetConfigDir(dir);

    AppConfig src = MakeEditedConfig("192.168.1.77");
    Check(store.Save(src), "Save() succeeded");

    AppConfig out;
    Check(store.Load(out), "Load() succeeded");
    CheckConfigEquals(src, out, "roundtrip");

    std::string ini = ReadFileA(dir + L"\\config.ini");
    Check(ini.find("SampleIntervalSec=7") != std::string::npos, "ini has SampleIntervalSec=7");
    Check(ini.find("TrendMinutes=25") != std::string::npos, "ini has TrendMinutes=25");
    Check(ini.find("PropertyMap=4.2,4.1,4.3,4.4,2.1") != std::string::npos,
          "ini has PropertyMap");
    Check(ini.find("IP=192.168.1.77") != std::string::npos, "ini has edited IP");
}

// ── Part B: 设置框「确定 / 测试连接+取消」一致性 ───────────
static int g_phase = 0;

static std::wstring CtlText(HWND h, int id) {
    HWND c = GetDlgItem(h, id);
    if (!c) return L"";
    wchar_t buf[512] = { 0 };
    SendMessageW(c, WM_GETTEXT, 512, (LPARAM)buf);
    return buf;
}

static void CtlSetText(HWND h, int id, const wchar_t* s) {
    SendMessageW(GetDlgItem(h, id), WM_SETTEXT, 0, (LPARAM)s);
}

static void CtlSetCheck(HWND h, int id, bool v) {
    SendMessageW(GetDlgItem(h, id), BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
}

static DWORD WINAPI UiThread(LPVOID) {
    Sleep(1200);
    HWND h = nullptr;
    for (int i = 0; i < 40 && !h; ++i) {
        h = FindWindowW(L"#32770", L"MiPower 功耗插件 - 设置");
        if (!h) Sleep(100);
    }
    if (!h) {
        printf("  [FAIL] dialog window not found\n");
        ++g_fail;
        return 1;
    }

    if (g_phase == 1) {
        // 初始展示必须等于真实配置：propertyMap 为空时不应显示硬编码默认值
        std::wstring pm = CtlText(h, kPropMap);
        Check(pm.empty(), "init: propertyMap shows real config (empty), not hardcoded value");

        CtlSetText(h, kIp, L"192.168.1.77");
        CtlSetText(h, kName, L"Verify Plug");
        CtlSetText(h, kPropMap, L"4.2,4.1,4.3,4.4,2.1");
        CtlSetText(h, kToken, L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        CtlSetText(h, kInterval, L"7");
        CtlSetText(h, kDecimal, L"2");
        CtlSetText(h, kPrice, L"1.25");
        CtlSetText(h, kTrendMin, L"25");
        CtlSetCheck(h, kShowLabel, false);
        CtlSetCheck(h, kShowUnit, false);
        CtlSetCheck(h, kShowDevName, true);
        CtlSetCheck(h, kShowEnergy, false);
        CtlSetCheck(h, kShowStats, false);
        CtlSetCheck(h, kHistory, false);
        CtlSetCheck(h, kShowTrend, false);
        CtlSetCheck(h, kShowCost, true);
        Sleep(250);
        PostMessageW(h, WM_COMMAND, MAKEWPARAM(kOk, BN_CLICKED), 0);
    } else if (g_phase == 2) {
        // 改值 -> 点「测试连接」-> 取消：三者都不应污染内存/ini/token 文件
        CtlSetText(h, kInterval, L"99");
        CtlSetText(h, kToken, L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        PostMessageW(h, WM_COMMAND, MAKEWPARAM(kTest, BN_CLICKED), 0);
        Sleep(900);
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
    return 0;
}

static void TestUiCommit() {
    printf("\n=== B. 设置框输入 -> 确定/取消 一致性 ===\n");
    std::wstring dir = MakeTempDir(L"ui");
    ConfigStore store;
    store.SetConfigDir(dir);

    AppConfig cfg;                      // 默认配置，propertyMap 为空
    cfg.devices.push_back(MakeDevice("192.168.1.10", ""));
    store.Save(cfg);
    std::wstring iniPath = dir + L"\\config.ini";
    std::wstring tokenPath = store.TokenPath(cfg.devices[0]);
    WriteFileA(tokenPath, "00112233445566778899aabbccddeeff");

    // ① 确定
    g_phase = 1;
    HANDLE th = CreateThread(nullptr, 0, UiThread, nullptr, 0, nullptr);
    bool changed = OptionsDlg::Show(nullptr, cfg, store, nullptr);
    WaitForSingleObject(th, 5000);
    CloseHandle(th);

    Check(changed, "OK: Show() returns changed=true");
    AppConfig expected = MakeEditedConfig("192.168.1.77");
    expected.devices[0].name = "Verify Plug";
    CheckConfigEquals(expected, cfg, "OK in-memory");

    std::string ini = ReadFileA(iniPath);
    Check(ini.find("SampleIntervalSec=7") != std::string::npos, "OK ini: SampleIntervalSec=7");
    Check(ini.find("DecimalPlaces=2") != std::string::npos, "OK ini: DecimalPlaces=2");
    Check(ini.find("ShowCost=1") != std::string::npos, "OK ini: ShowCost=1");
    Check(ini.find("IP=192.168.1.77") != std::string::npos, "OK ini: edited IP");
    Check(ReadFileA(tokenPath) == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "OK token file written on commit");

    // ② 测试连接 + 取消
    AppConfig snapshot = cfg;
    std::string iniBefore = ini;
    std::string tokenBefore = ReadFileA(tokenPath);
    g_phase = 2;
    th = CreateThread(nullptr, 0, UiThread, nullptr, 0, nullptr);
    bool changed2 = OptionsDlg::Show(nullptr, cfg, store, nullptr);
    WaitForSingleObject(th, 5000);
    CloseHandle(th);

    Check(!changed2, "CANCEL: Show() returns changed=false");
    CheckConfigEquals(snapshot, cfg, "CANCEL in-memory");
    Check(ReadFileA(iniPath) == iniBefore, "CANCEL: ini unchanged");
    Check(ReadFileA(tokenPath) == tokenBefore,
          "TEST+CANCEL: token file unchanged (no side effect)");
}

// ── Part C: 采集器运行时热应用（真实设备）─────────────────
static void TestRuntime(const std::wstring& realCfgDir) {
    printf("\n=== C. Collector::ApplyConfig 运行时生效（真实设备 192.168.1.10）===\n");
    std::string realToken = ReadFileA(realCfgDir + L"\\tokens\\plug-1.token");
    while (!realToken.empty() && (realToken.back() == '\r' || realToken.back() == '\n'))
        realToken.pop_back();
    if (realToken.size() != 32) {
        printf("  [SKIP] token 文件不可用 (%zu bytes)\n", realToken.size());
        return;
    }

    std::wstring dir = MakeTempDir(L"runtime");
    ConfigStore store;
    store.SetConfigDir(dir);
    WriteFileA(store.TokenPath(MakeDevice("192.168.1.10", "")), realToken);

    AppConfig cfg;
    cfg.sampleIntervalSec = 2;
    cfg.requestTimeoutMs = 3000;
    cfg.heartbeatSec = 30;
    cfg.maxRetryIntervalSec = 30;
    cfg.devices.push_back(MakeDevice("192.168.1.10", "4.2,4.1,4.3,4.4,2.1"));

    Collector c;
    c.Start(cfg, store);
    Sleep(6000);                                   // warmup: 规格解析 + 首次采样
    DeviceHealth h0 = c.GetHealth();
    printf("  warmup      : success=%d state=%d watts=%.1f\n",
           h0.totalSuccess, (int)h0.state, h0.lastWatts);
    Check(h0.totalSuccess > 0, "warmup: real device produced samples");

    Sleep(8000);
    DeviceHealth h1 = c.GetHealth();
    int d1 = h1.totalSuccess - h0.totalSuccess;
    printf("  interval=2s : +%d samples / 8s\n", d1);

    AppConfig cfg2 = cfg;
    cfg2.sampleIntervalSec = 1;
    c.ApplyConfig(cfg2);
    Sleep(2000);                                   // 等待配置被采集线程取用
    DeviceHealth h1b = c.GetHealth();
    Sleep(8000);
    DeviceHealth h2 = c.GetHealth();
    int d2 = h2.totalSuccess - h1b.totalSuccess;
    printf("  interval=1s : +%d samples / 8s\n", d2);
    Check(d2 > d1, "ApplyConfig(sampleIntervalSec) changes live sampling rate");

    AppConfig cfg3 = cfg2;
    cfg3.devices[0].ip = "192.168.1.199";          // 局域网内不存在的地址
    c.ApplyConfig(cfg3);
    Sleep(7000);
    DeviceHealth h3 = c.GetHealth();
    printf("  bad IP      : success=%d failures=%d state=%d\n",
           h3.totalSuccess, h3.totalFailures, (int)h3.state);
    Check(h3.totalFailures > h2.totalFailures,
          "ApplyConfig(IP) reconnects and old data stops");

    c.ApplyConfig(cfg2);                           // 改回真实 IP
    Sleep(7000);
    DeviceHealth h4 = c.GetHealth();
    printf("  restore IP  : success=%d failures=%d state=%d watts=%.1f\n",
           h4.totalSuccess, h4.totalFailures, (int)h4.state, h4.lastWatts);
    // 用「成功采样数增量 + 有效功率读数」判定恢复，而不是瞬时 state：
    // state 是每轮采样的健康状态，真实网络偶发抖动会让它短暂处于 Connecting。
    Check(h4.totalSuccess >= h3.totalSuccess + 3 && h4.lastWatts > 0,
          "ApplyConfig(IP back) recovers online sampling");

    // 属性映射改成设备不支持的 siid —— 应立刻重新解析并停止出数
    AppConfig cfg4 = cfg2;
    cfg4.devices[0].propertyMap = "9.1,9.2,9.3,9.4,9.5";
    c.ApplyConfig(cfg4);
    Sleep(7000);
    DeviceHealth h5 = c.GetHealth();
    printf("  bad propMap : success=%d failures=%d state=%d\n",
           h5.totalSuccess, h5.totalFailures, (int)h5.state);
    Check(h5.totalFailures > h4.totalFailures,
          "ApplyConfig(propertyMap) re-resolves spec and takes effect");

    c.ApplyConfig(cfg2);                           // 改回正确映射
    Sleep(7000);
    DeviceHealth h6 = c.GetHealth();
    printf("  restore map : success=%d failures=%d state=%d watts=%.1f\n",
           h6.totalSuccess, h6.totalFailures, (int)h6.state, h6.lastWatts);
    Check(h6.totalSuccess >= h5.totalSuccess + 2,
          "ApplyConfig(propertyMap back) recovers sampling");

    c.Stop();
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string mode = argc > 1 ? argv[1] : "all";
    std::wstring realDir = argc > 2 ? U2W(argv[2]) : L"";

    printf("=== MiPower 配置链路验证 (mode=%s) ===\n", mode.c_str());
    if (mode == "save" || mode == "all") TestSaveRoundTrip();
    if (mode == "ui" || mode == "all") TestUiCommit();
    if (mode == "runtime" || mode == "all") TestRuntime(realDir);

    printf("\n=== 汇总: %s (%d 项失败) ===\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
