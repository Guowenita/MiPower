// MiotSpec.cpp - 型号属性映射解析
#include "MiotSpec.h"
#include "DiagLog.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace mp {

// ══════════════════════════════════════════════════════════
// 内置规格表
// 依据 miot-spec.org 官方 MIoT spec 整理
// ══════════════════════════════════════════════════════════
namespace {

struct BuiltinSpec {
    const char* model;
    int siidPower, piidPower;
    int siidEnergy, piidEnergy;
    int siidVoltage, piidVoltage;
    int siidCurrent, piidCurrent;
    int siidSwitch, piidSwitch;
    const char* note;
};

const BuiltinSpec kBuiltinSpecs[] = {
    // 酷控 / 米家 WiFi 插座系列 —— power-consumption 服务 (siid=4)
    // 功率 electric-power, 电量 power-consumption, 电压 voltage, 电流 electric-current
    { "sixwgh.plug.v8icwf",   4, 2, 4, 1, 4, 3, 4, 4, 2, 1, "酷控 WiFi 插座 v8" },
    { "sixwgh.plug.v1",       4, 2, 4, 1, 4, 3, 4, 4, 2, 1, "酷控 WiFi 插座" },
    { "cuco.plug.v3",        11, 2, 11, 1, 12, 2, 12, 3, 2, 1, "米家智能插座3" },
    { "cuco.plug.v1",         4, 2, 4, 1, 4, 3, 4, 4, 2, 1, "酷控插座" },
    { "chuangmi.plug.v1",     4, 2, 4, 1, 4, 3, 4, 4, 2, 1, "小米智能插座" },
    { "chuangmi.plug.v2",     4, 2, 4, 1, 4, 3, 4, 4, 2, 1, "小米智能插座2" },
    { "chuangmi.plug.v3",     4, 2, 4, 1, 4, 3, 4, 4, 2, 1, "小米智能插座3" },
    { "chuangmi.plug.m1",     4, 2, 4, 1, 4, 3, 4, 4, 2, 1, "小米智能插座(小方)" },
    { "zimi.plug.v1",         4, 2, 4, 1, 4, 3, 4, 4, 2, 1, "紫米智能插座" },
};

} // namespace

std::vector<PropertyRef> PropertyMap::BuildQueryList() const {
    std::vector<PropertyRef> v;
    auto add = [&](int s, int p) {
        if (s >= 0 && p >= 0) v.push_back({ s, p });
    };
    add(siidPower, piidPower);
    add(siidEnergy, piidEnergy);
    add(siidVoltage, piidVoltage);
    add(siidCurrent, piidCurrent);
    add(siidSwitch, piidSwitch);
    return v;
}

// ══════════════════════════════════════════════════════════
// 解析主流程
// ══════════════════════════════════════════════════════════
bool MiotSpecResolver::Resolve(MiioClient& client, PropertyMap& outMap,
                               const std::string& manualOverride) {
    // ① 手动覆盖优先级最高
    if (!manualOverride.empty()) {
        if (ParseManualOverride(manualOverride, outMap)) {
            outMap.resolved = true;
            m_lastDiag = "使用手动覆盖: " + manualOverride;
            return true;
        }
        m_lastDiag = "手动覆盖格式无效: " + manualOverride;
    }

    // ② 读设备型号
    DeviceInfo info;
    ErrorKind err = ErrorKind::None;
    if (!client.GetDeviceInfo(info, err)) {
        DiagLogEx("MiotSpec: GetDeviceInfo failed err=%d", (int)err);
        char buf[128];
        snprintf(buf, sizeof(buf), "读取设备信息失败 (err=%d)", (int)err);
        m_lastDiag = buf;
        return false;
    }
    outMap.model = info.model;

    // ③ 内置规格表
    if (LookupBuiltin(info.model, outMap)) {
        outMap.resolved = true;
        m_lastDiag = "命中内置规格表: " + info.model;
        return true;
    }

    // ④ 缓存
    for (const auto& kv : m_cache) {
        if (kv.first == info.model) {
            outMap = kv.second;
            outMap.model = info.model;
            outMap.resolved = true;
            m_lastDiag = "命中规格缓存: " + info.model;
            return true;
        }
    }

    // ⑤ 主动探测
    if (ProbeProperties(client, outMap)) {
        outMap.model = info.model;
        outMap.resolved = true;
        m_lastDiag = "主动探测成功: " + info.model;

        // 写缓存
        for (auto& kv : m_cache) {
            if (kv.first == info.model) { kv.second = outMap; return true; }
        }
        m_cache.emplace_back(info.model, outMap);
        return true;
    }

    m_lastDiag = "探测失败，型号 " + info.model + " 未知";
    return false;
}

// ══════════════════════════════════════════════════════════
// 内置表查询
// ══════════════════════════════════════════════════════════
bool MiotSpecResolver::LookupBuiltin(const std::string& model, PropertyMap& out) {
    if (model.empty()) return false;
    for (const auto& s : kBuiltinSpecs) {
        if (model == s.model) {
            out.siidPower = s.siidPower;   out.piidPower = s.piidPower;
            out.siidEnergy = s.siidEnergy; out.piidEnergy = s.piidEnergy;
            out.siidVoltage = s.siidVoltage; out.piidVoltage = s.piidVoltage;
            out.siidCurrent = s.siidCurrent; out.piidCurrent = s.piidCurrent;
            out.siidSwitch = s.siidSwitch; out.piidSwitch = s.piidSwitch;
            out.model = model;
            return true;
        }
    }
    return false;
}

// ══════════════════════════════════════════════════════════
// 主动探测
// ══════════════════════════════════════════════════════════
bool MiotSpecResolver::ProbeProperties(MiioClient& client, PropertyMap& out) {
    // 策略：遍历 siid 1..20，每个 siid 一次请求问 piid 1..8
    // 找出所有 code==0 的属性，然后按「量级 + 波动性 + MIoT 语义」判定角色

    struct Candidate {
        int siid, piid;
        double value;
        bool isBool;
    };
    std::vector<Candidate> candidates;

    const int kMaxSiid = 20;
    const int kMaxPiid = 8;

    for (int siid = 1; siid <= kMaxSiid; siid++) {
        std::vector<PropertyRef> props;
        for (int piid = 1; piid <= kMaxPiid; piid++)
            props.push_back({ siid, piid });

        std::vector<PropertyValue> values;
        ErrorKind err = ErrorKind::None;
        if (!client.GetProperties(props, values, err)) {
            // 该 siid 整体不可读，跳过（设备不支持这一服务）
            continue;
        }
        for (const auto& v : values) {
            if (v.code != 0) continue;
            if (v.isBool) {
                candidates.push_back({ v.siid, v.piid, v.boolValue ? 1.0 : 0.0, true });
            } else if (v.hasValue) {
                candidates.push_back({ v.siid, v.piid, v.value, false });
            }
        }
    }

    if (candidates.empty())
        return false;

    // 判定开关：bool 类型且值可能为 true
    // 优先找 siid 较小的 bool（switch 服务通常 siid=2）
    for (const auto& c : candidates) {
        if (c.isBool && out.siidSwitch < 0 && c.siid <= 5) {
            out.siidSwitch = c.siid;
            out.piidSwitch = c.piid;
            break;
        }
    }

    // 判定数值属性：用「波动采样」区分功率与静态量
    // 功率会随负载变化；电压/电流数量级不同
    // 先按量级初筛，再做一次间隔采样验证
    struct NumCand { int siid, piid; double v1; };
    std::vector<NumCand> nums;
    for (const auto& c : candidates) {
        if (!c.isBool) nums.push_back({ c.siid, c.piid, c.value });
    }
    if (nums.empty())
        return false;

    // 二次采样：间隔 1.2 秒，比较哪些属性变化了
    Sleep(1200);
    std::vector<PropertyRef> props2;
    for (const auto& n : nums) props2.push_back({ n.siid, n.piid });
    std::vector<PropertyValue> values2;
    ErrorKind err = ErrorKind::None;
    client.GetProperties(props2, values2, err);

    std::vector<std::pair<NumCand, double>> changed;   // 变化了的属性
    std::vector<NumCand> stable;
    for (size_t i = 0; i < values2.size() && i < nums.size(); i++) {
        if (values2[i].code != 0) continue;
        const NumCand& n = nums[i];
        // 找到对应的第一个候选
        double v1 = n.v1;
        for (const auto& c : candidates) {
            if (c.siid == n.siid && c.piid == n.piid && !c.isBool) { v1 = c.value; break; }
        }
        if (values2[i].hasValue && std::fabs(values2[i].value - v1) > 0.001)
            changed.push_back({ n, values2[i].value });
        else
            stable.push_back(n);
    }

    // 功率：数值、量级合理（0~10000）、且会波动
    for (const auto& ch : changed) {
        if (out.siidPower < 0 && ch.second >= 0 && ch.second <= 10000) {
            out.siidPower = ch.first.siid;
            out.piidPower = ch.first.piid;
            break;
        }
    }

    // 电压：稳定在 100~300 区间
    for (const auto& s : stable) {
        if (out.siidVoltage < 0 && s.v1 >= 100 && s.v1 <= 300) {
            out.siidVoltage = s.siid;
            out.piidVoltage = s.piid;
            break;
        }
    }

    // 电流：与功率同 siid（power-consumption 服务内）的另一个稳定数值
    if (out.siidPower > 0) {
        for (const auto& s : stable) {
            if (out.siidCurrent < 0 && s.siid == out.siidPower && s.piid != out.piidPower) {
                out.siidCurrent = s.siid;
                out.piidCurrent = s.piid;
                break;
            }
        }
    }

    // 电量：单调递增的累计量，通常量级较大（>=100）
    for (const auto& s : stable) {
        if (out.siidEnergy < 0 && s.v1 >= 100) {
            out.siidEnergy = s.siid;
            out.piidEnergy = s.piid;
            break;
        }
    }

    // 若电量与电压/电流撞车，用同 siid 内剩余项补
    if (out.siidPower > 0 && out.siidEnergy < 0) {
        for (const auto& s : stable) {
            if (s.siid == out.siidPower && s.piid != out.piidPower) {
                if (out.siidCurrent < 0) {
                    out.siidCurrent = s.siid;
                    out.piidCurrent = s.piid;
                } else {
                    out.siidEnergy = s.siid;
                    out.piidEnergy = s.piid;
                }
                break;
            }
        }
    }

    // 至少要能拿到功率
    return out.siidPower >= 0;
}

// ══════════════════════════════════════════════════════════
// 手动覆盖解析
// ══════════════════════════════════════════════════════════
bool MiotSpecResolver::ParseManualOverride(const std::string& s, PropertyMap& out) {
    std::vector<std::pair<int, int>> pairs;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item.erase(std::remove_if(item.begin(), item.end(),
                                  [](unsigned char c) { return isspace(c); }), item.end());
        if (item.empty()) continue;
        size_t dot = item.find('.');
        if (dot == std::string::npos) return false;
        try {
            int siid = std::stoi(item.substr(0, dot));
            int piid = std::stoi(item.substr(dot + 1));
            if (siid < 0 || piid < 0) return false;
            pairs.push_back({ siid, piid });
        } catch (...) { return false; }
    }
    if (pairs.empty()) return false;

    if (pairs.size() > 0) { out.siidPower = pairs[0].first;   out.piidPower = pairs[0].second; }
    if (pairs.size() > 1) { out.siidEnergy = pairs[1].first;  out.piidEnergy = pairs[1].second; }
    if (pairs.size() > 2) { out.siidVoltage = pairs[2].first; out.piidVoltage = pairs[2].second; }
    if (pairs.size() > 3) { out.siidCurrent = pairs[3].first; out.piidCurrent = pairs[3].second; }
    if (pairs.size() > 4) { out.siidSwitch = pairs[4].first;  out.piidSwitch = pairs[4].second; }
    return true;
}

// ══════════════════════════════════════════════════════════
// 规格缓存（简单 INI 风格）
// ══════════════════════════════════════════════════════════
bool MiotSpecResolver::LoadCache(const std::wstring& path) {
    m_cache.clear();
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string model = line.substr(0, eq);
        std::string vals = line.substr(eq + 1);
        std::vector<std::string> parts;
        std::stringstream ss(vals);
        std::string p;
        while (std::getline(ss, p, ',')) parts.push_back(p);
        if (parts.size() < 5) continue;
        PropertyMap m;
        try {
            m.siidPower = std::stoi(parts[0]);   m.piidPower = std::stoi(parts[1]);
            m.siidEnergy = std::stoi(parts[2]);  m.piidEnergy = std::stoi(parts[3]);
            m.siidVoltage = std::stoi(parts[4]); m.piidVoltage = std::stoi(parts[5]);
            if (parts.size() > 7) {
                m.siidCurrent = std::stoi(parts[6]); m.piidCurrent = std::stoi(parts[7]);
            }
            if (parts.size() > 9) {
                m.siidSwitch = std::stoi(parts[8]); m.piidSwitch = std::stoi(parts[9]);
            }
        } catch (...) { continue; }
        m.model = model;
        m.resolved = true;
        m_cache.emplace_back(model, m);
    }
    return !m_cache.empty();
}

bool MiotSpecResolver::SaveCache(const std::wstring& path) const {
    if (m_cache.empty()) return false;
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << "# MiPower spec cache: model,power,energy,voltage,current,switch\n";
        for (const auto& kv : m_cache) {
            const PropertyMap& m = kv.second;
            out << kv.first << ","
                << m.siidPower << "," << m.piidPower << ","
                << m.siidEnergy << "," << m.piidEnergy << ","
                << m.siidVoltage << "," << m.piidVoltage << ","
                << m.siidCurrent << "," << m.piidCurrent << ","
                << m.siidSwitch << "," << m.piidSwitch << "\n";
        }
    }
    // 原子替换
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    return true;
}

} // namespace mp
