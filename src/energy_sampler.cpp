// energy_sampler.cpp — 用插件同款 Collector 采集功率与累计电量，输出 CSV
//
// 直接复用 MiPower 的 Collector / ConfigStore / Aggregator，
// 保证采样数据与插件运行时完全一致（含梯形积分与缺口记录）。
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "Collector.h"
#include "Config.h"

using namespace mp;

int main(int argc, char** argv) {
    std::string ip    = (argc > 1) ? argv[1] : "192.168.1.10";
    std::string token = (argc > 2) ? argv[2] : "00112233445566778899aabbccddeeff";
    int seconds       = (argc > 3) ? atoi(argv[3]) : 120;
    std::string outCsv = (argc > 4) ? argv[4] : "power_energy_samples.csv";

    AppConfig cfg;
    cfg.sampleIntervalSec = 2;
    cfg.realtimeWindowSec = 600;
    cfg.enableHistory = false;
    cfg.pricePerKwh = 0.65;

    DeviceConfig d;
    d.id = "plug-1";
    d.ip = ip;
    d.name = "Smart Plug";
    d.tokenFile = "plug-1.token";
    d.enabled = true;
    cfg.devices.push_back(d);

    // token 必须放在 <配置目录>\tokens\ 下（与插件运行时一致）
    CreateDirectoryW(L"tokens", nullptr);
    {
        std::ofstream f("tokens/plug-1.token", std::ios::binary);
        f << token;
    }

    ConfigStore store;
    store.SetConfigDir(L".");

    Collector collector;
    collector.Start(cfg, store);

    printf("采样 %d 秒，每 2s 一点...\n", seconds);
    std::ofstream csv(outCsv);
    csv << "elapsed_s,watts,device_kwh,local_kwh,voltage,current,switch\n";

    int points = seconds / 2;
    for (int i = 0; i < points; i++) {
        Sleep(2000);
        auto snap = collector.GetSnapshot();
        auto h    = collector.GetHealth();
        double localKwh = collector.GetIntegratedKwh();
        csv << (i * 2) << ","
            << snap.sample.watts << ","
            << h.deviceEnergyKwh << ","
            << localKwh << ","
            << snap.sample.voltage << ","
            << snap.sample.current << ","
            << (snap.sample.switchOn ? 1 : 0) << "\n";
        csv.flush();
        printf("  [%3d/%d] %6.1f W   device=%.4f kWh   local=%.4f kWh   cov=%.1f%%\n",
               i + 1, points, snap.sample.watts, h.deviceEnergyKwh, localKwh,
               collector.GetCoverage() * 100.0);
    }

    collector.Stop();
    csv.close();
    printf("\n已保存 -> %s\n", outCsv.c_str());
    return 0;
}
