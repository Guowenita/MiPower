// devicetest.cpp - 真实设备联调测试（不依赖 TrafficMonitor）
// 用法: devicetest.exe <ip> <token>
#include <cstdio>
#include <cstring>
#include <string>

#include "Crypto.h"
#include "MiioClient.h"
#include "MiotSpec.h"

using namespace mp;

int main(int argc, char** argv) {
    std::string ip = (argc > 1) ? argv[1] : "192.168.1.10";
    std::string token = (argc > 2) ? argv[2] : "00112233445566778899aabbccddeeff";

    printf("=== MiPower 设备联调测试 ===\n");
    printf("目标: %s\n\n", ip.c_str());

    MiioClient client;
    if (!client.Open(ip, token, 3000)) {
        printf("[FAIL] 无法打开连接\n");
        return 1;
    }
    printf("[OK] 连接已建立 (token 派生成功)\n\n");

    // ① 握手
    ErrorKind err = ErrorKind::None;
    if (!client.Handshake(err)) {
        printf("[FAIL] 握手失败: %s\n", ErrorKindText(err));
        return 1;
    }
    printf("[OK] 握手成功\n");

    // ② miIO.info
    DeviceInfo info;
    if (!client.GetDeviceInfo(info, err)) {
        printf("[FAIL] miIO.info 失败: %s\n", ErrorKindText(err));
        return 1;
    }
    printf("[OK] miIO.info:\n");
    printf("     model      = %s\n", info.model.c_str());
    printf("     fw_ver     = %s\n", info.firmware.c_str());
    printf("     hw_ver     = %s\n", info.hardware.c_str());
    printf("     mac        = %s\n", info.mac.c_str());
    printf("     token回显  = %s  %s\n", info.token.c_str(),
           info.token == token ? "(与本地一致)" : "(!! 不一致)");
    printf("     ssid/rssi  = %s / %d\n", info.ssid.c_str(), info.rssi);

    // ③ 规格解析
    printf("\n");
    MiotSpecResolver resolver;
    PropertyMap pm;
    if (!resolver.Resolve(client, pm, std::string())) {
        printf("[FAIL] 属性映射解析失败: %s\n", resolver.LastDiagnostics().c_str());
        return 1;
    }
    printf("[OK] 属性映射: %s\n", resolver.LastDiagnostics().c_str());
    printf("     功率 = %d.%d   电量 = %d.%d   电压 = %d.%d   电流 = %d.%d   开关 = %d.%d\n",
           pm.siidPower, pm.piidPower, pm.siidEnergy, pm.piidEnergy,
           pm.siidVoltage, pm.piidVoltage, pm.siidCurrent, pm.piidCurrent,
           pm.siidSwitch, pm.piidSwitch);

    // ④ 连续采样
    printf("\n=== 连续采样 10 次 ===\n");
    int ok = 0, fail = 0;

    // 先打印一次原始响应，调试用
    {
        std::string raw;
        ErrorKind re = ErrorKind::None;
        if (client.Call("get_properties",
                        "[{\"did\":\"props\",\"siid\":4,\"piid\":2},{\"did\":\"props\",\"siid\":4,\"piid\":1},"
                        "{\"did\":\"props\",\"siid\":4,\"piid\":3},{\"did\":\"props\",\"siid\":4,\"piid\":4},"
                        "{\"did\":\"props\",\"siid\":2,\"piid\":1}]",
                        raw, re)) {
            printf("[DEBUG] 原始 result: %s\n", raw.c_str());
        } else {
            printf("[DEBUG] 原始请求失败: %s\n", ErrorKindText(re));
        }
    }

    for (int i = 0; i < 10; i++) {
        std::vector<PropertyRef> props = pm.BuildQueryList();
        std::vector<PropertyValue> values;
        ErrorKind e = ErrorKind::None;
        if (!client.GetProperties(props, values, e)) {
            printf("  第%2d次 失败: %s\n", i + 1, ErrorKindText(e));
            fail++;
        } else {
            double w = 0, v = 0, c = 0, en = 0;
            bool sw = false, haveW = false;
            for (const auto& pv : values) {
                if (pv.code != 0) continue;
                if (pv.siid == pm.siidPower && pv.piid == pm.piidPower && pv.hasValue) {
                    w = pv.value; haveW = true;
                } else if (pv.siid == pm.siidEnergy && pv.piid == pm.piidEnergy && pv.hasValue) {
                    en = pv.value;
                } else if (pv.siid == pm.siidVoltage && pv.piid == pm.piidVoltage && pv.hasValue) {
                    v = pv.value;
                } else if (pv.siid == pm.siidCurrent && pv.piid == pm.piidCurrent && pv.hasValue) {
                    c = pv.value / 100.0;   // 0.01A -> A
                } else if (pv.siid == pm.siidSwitch && pv.piid == pm.piidSwitch) {
                    sw = pv.boolValue;
                }
            }
            printf("  第%2d次 功率=%6.1f W  电压=%5.1f V  电流=%5.2f A  电量=%8.2f kWh  开关=%s\n",
                   i + 1, w, v, c, en / 100.0, sw ? "开" : "关");
            if (haveW) ok++;
        }
        Sleep(1000);
    }
    printf("\n成功 %d / 失败 %d\n", ok, fail);

    // ⑤ 错误 token 测试（验证 checksum 错误能被识别）
    printf("\n=== 错误 token 测试 ===\n");
    MiioClient bad;
    std::string badToken = "00000000000000000000000000000000";
    if (bad.Open(ip, badToken, 2000)) {
        std::string result;
        ErrorKind be = ErrorKind::None;
        bool r = bad.Call("get_properties", "[{\"did\":\"props\",\"siid\":4,\"piid\":2}]", result, be);
        printf("  错误 token 返回: 成功=%d 错误类型=%s\n", r ? 1 : 0, ErrorKindText(be));
    }

    return (ok == 10) ? 0 : 1;
}
