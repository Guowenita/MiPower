// MiotSpec.h - 型号 → 属性映射解析（规格表 + 自动探测 + 缓存 + 手动覆盖）
#pragma once
#include <Windows.h>

#include <string>
#include <vector>

#include "MiioClient.h"

namespace mp {

// ─── 属性映射 ──────────────────────────────────────────────
struct PropertyMap {
    int  siidPower = -1,   piidPower = -1;
    int  siidEnergy = -1,  piidEnergy = -1;
    int  siidVoltage = -1, piidVoltage = -1;
    int  siidCurrent = -1, piidCurrent = -1;
    int  siidSwitch = -1,  piidSwitch = -1;

    std::string model;
    bool resolved = false;

    // 组装本次要查询的属性列表
    std::vector<PropertyRef> BuildQueryList() const;
};

// ─── 规格解析器 ────────────────────────────────────────────
class MiotSpecResolver {
public:
    // 解析型号的属性映射
    // 优先级：手动覆盖 → 内置表 → 缓存 → 主动探测
    bool Resolve(MiioClient& client, PropertyMap& outMap,
                 const std::string& manualOverride = std::string());

    // 从内置规格表查
    static bool LookupBuiltin(const std::string& model, PropertyMap& out);

    // 主动探测：遍历 siid × piid，用 get_properties 批量试探
    static bool ProbeProperties(MiioClient& client, PropertyMap& out);

    // 规格缓存（型号 → 映射）
    bool LoadCache(const std::wstring& path);
    bool SaveCache(const std::wstring& path) const;

    // 手动覆盖解析："4.2,4.1,4.3,4.4,2.1" 顺序为 功率,电量,电压,电流,开关
    static bool ParseManualOverride(const std::string& s, PropertyMap& out);

    // 记录最后一次探测的诊断信息
    const std::string& LastDiagnostics() const { return m_lastDiag; }

private:
    std::vector<std::pair<std::string, PropertyMap>> m_cache;
    std::string m_lastDiag;
};

} // namespace mp
