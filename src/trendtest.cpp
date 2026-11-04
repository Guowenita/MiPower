// trendtest.cpp — 验证「功耗与累计电量关联展示」
//
// 加载插件 DLL，喂入真实设备数据，等趋势积累后打印完整 tooltip，
// 确认趋势迷你图、区间电量、电费都正确输出。
#include <cstdio>
#include <string>

#include "Config.h"
#include "Collector.h"

using namespace mp;

static std::string NarrowS(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.resize(n - 1);
    return s;
}

int main(int argc, char** argv) {
    std::string ip    = (argc > 1) ? argv[1] : "192.168.1.10";
    std::string token = (argc > 2) ? argv[2] : "00112233445566778899aabbccddeeff";
    int waitSec       = (argc > 3) ? atoi(argv[3]) : 40;

    AppConfig cfg;
    cfg.sampleIntervalSec = 2;
    cfg.realtimeWindowSec = 600;
    cfg.enableHistory = false;
    cfg.pricePerKwh = 0.65;
    cfg.showTrend = true;
    cfg.showCost = true;
    cfg.trendMinutes = 10;

    DeviceConfig d;
    d.id = "plug-1";
    d.ip = ip;
    d.name = "Smart Plug";
    d.tokenFile = "plug-1.token";
    d.enabled = true;
    cfg.devices.push_back(d);

    CreateDirectoryW(L"tokens", nullptr);
    { FILE* f = nullptr; _wfopen_s(&f, L"tokens\\plug-1.token", L"wb");
      if (f) { fwrite(token.data(), 1, token.size(), f); fclose(f); } }

    ConfigStore store;
    store.SetConfigDir(L".");

    Collector collector;
    collector.Start(cfg, store);

    printf("采集 %d 秒以积累趋势...\n", waitSec);
    for (int i = 0; i < waitSec / 2; i++) {
        Sleep(2000);
        auto snap = collector.GetSnapshot();
        printf("  t=%3ds  %6.1f W   device=%.4f kWh   local=%.6f kWh\n",
               (i + 1) * 2, snap.sample.watts,
               collector.GetHealth().deviceEnergyKwh,
               collector.GetIntegratedKwh());
    }

    // ── 复现插件 tooltip 的趋势图部分 ──
    printf("\n=== 功耗与累计电量关联展示（tooltip 实际输出）===\n");
    auto trend = collector.GetPowerTrend(cfg.trendMinutes * 60, 40);

    printf("趋势点数: %zu\n", trend.size());
    if (trend.size() >= 2) {
        double lo = trend.front().watts, hi = trend.front().watts;
        double e0 = trend.front().energyKwh, e1 = trend.back().energyKwh;
        for (const auto& p : trend) {
            if (p.watts < lo) lo = p.watts;
            if (p.watts > hi) hi = p.watts;
        }
        double range = hi - lo;
        if (range < 1.0) { lo -= 0.5; hi += 0.5; range = hi - lo; }

        // 用 UTF-8 字节输出块字符（控制台是 GBK，直接 printf %ls 会乱码）
        static const char* kBlocks[] = {
            "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
            "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
        };
        printf("功耗趋势    ");
        for (const auto& p : trend) {
            double norm = (p.watts - lo) / range;
            int idx = (int)(norm * 7.0 + 0.5);
            if (idx < 0) idx = 0;
            if (idx > 7) idx = 7;
            printf("%s", kBlocks[idx]);
        }
        printf("\n");
        printf("            %.0f ~ %.0f W   (近 %d 分钟)\n", lo, hi, cfg.trendMinutes);

        double dKwh = e1 - e0;
        if (dKwh > 0) {
            printf("            区间电量  %.3f kWh   电费 \u00a5%.2f\n",
                   dKwh, dKwh * cfg.pricePerKwh);
        } else {
            printf("            (区间电量增量 = %.6f kWh, 设备端精度不足以显示)\n", dKwh);
        }
    } else {
        printf("  [!] 趋势数据不足\n");
    }

    double devKwh = collector.GetHealth().deviceEnergyKwh;
    printf("累计电费    \u00a5%.2f  (%.2f 元/kWh)\n", devKwh * cfg.pricePerKwh, cfg.pricePerKwh);
    printf("本地积分电量 %.6f kWh   覆盖率 %.1f%%\n",
           collector.GetIntegratedKwh(), collector.GetCoverage() * 100.0);

    collector.Stop();
    return 0;
}
