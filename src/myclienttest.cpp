// 用自研 MiioClient 直接测试，验证协议实现
#include <cstdio>
#include <string>
#include "MiioClient.h"
using namespace mp;
int main(int argc, char** argv) {
    std::string ip = argc > 1 ? argv[1] : "192.168.1.10";
    std::string token = argc > 2 ? argv[2] : "00112233445566778899aabbccddeeff";
    MiioClient c;
    if (!c.Open(ip, token, 3000)) { printf("[FAIL] open\n"); return 1; }
    printf("[OK] open\n");
    ErrorKind e = ErrorKind::None;
    if (!c.Handshake(e)) { printf("[FAIL] handshake err=%d\n", (int)e); return 1; }
    printf("[OK] handshake\n");
    std::string r;
    if (!c.Call("miIO.info", "[]", r, e)) {
        printf("[FAIL] miIO.info err=%d\n", (int)e);
        printf("       raw=%s\n", r.c_str());
        return 1;
    }
    printf("[OK] miIO.info raw=%.200s\n", r.c_str());
    DeviceInfo di;
    if (!c.GetDeviceInfo(di, e)) { printf("[FAIL] GetDeviceInfo err=%d\n", (int)e); return 1; }
    printf("[OK] GetDeviceInfo model=%s fw=%s mac=%s\n",
           di.model.c_str(), di.firmware.c_str(), di.mac.c_str());
    std::vector<PropertyRef> props = { {4,2},{4,1},{4,3},{4,4},{2,1} };
    std::vector<PropertyValue> vals;
    if (!c.GetProperties(props, vals, e)) { printf("[FAIL] GetProperties err=%d\n", (int)e); return 1; }
    printf("[OK] GetProperties:/n");
    for (auto& v : vals)
        printf("   siid=%d piid=%d code=%d val=%.2f bool=%d\n", v.siid, v.piid, v.code, v.value, v.boolValue);
    return 0;
}
