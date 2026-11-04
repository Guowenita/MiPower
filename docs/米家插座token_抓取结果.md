> **本文为脱敏版**：设备 IP、token、DID、MAC、SSID、UID 均为示例占位值，
> 仅用于说明提取方法与协议要点，不代表任何真实设备。

# 米家插座 Token 抓取结果

**抓取时间**：2026-11-03 15:25
**状态**：✅ 已验证通过（设备 `miIO.info` 回显 token 完全一致）

---

## 设备配置（填入 TrafficMonitor 插件）

| 项目 | 值 |
|------|-----|
| **设备 IP** | `192.168.1.10` |
| **Token** | `00112233445566778899aabbccddeeff` |
| 名称（建议） | 示例智能插座（WiFi计量） |
| Device ID (DID) | `1234567890` (dec) = `0x499602d2` |

---

## 设备信息（miIO.info 实测）

| 项 | 值 |
|----|-----|
| 型号 | `sixwgh.plug.v8icwf`（酷控 WiFi 插座 v8） |
| 固件 | `1.0.3_0004` |
| 硬件 | `BL602` |
| MAC | `00:11:22:33:44:55` |
| SSID / RSSI | `Example-SSID` / -54 |
| UID | 1000000000 |

---

## ⚠️ 重要：功率属性与插件 README 不一致

插件 README 写的是 `siid=11, piid=2`（功率），那是 **`cuco.plug.v3`** 型号的规格。
本机是 **`sixwgh.plug.v8icwf`**，官方 MIoT spec（miot-spec.org）实际布局：

| siid.piid | 属性 | 单位 | 实测值 |
|-----------|------|------|--------|
| 2.1 | 开关 on/off | bool | True |
| 4.1 | power-consumption 累计电量 | 0.01 kWh | 3062 |
| **4.2** | **electric-power 实时功率** | **W** | **37 ~ 53** |
| 4.3 | voltage 电压 | V | 236 |
| 4.4 | electric-current 电流 | 0.01 A | 156 ~ 224 |

实测 `siid=11, piid=2` 返回错误码 **-4003（属性不支持）**。
**插件若硬编码 11/2 会读不到功率，需改为 `4/2`**，或让插件按型号动态映射。

---

## Token 来源与提取方法

新版米家 App（`com.xiaomi.smarthome`）已不再用老的 `databases/miio2.db`，
token 存放在 MMKV 库中：

```
/data/user/0/com.xiaomi.smarthome/files/mmkv/mmkv_id_local_control
  └─ key: key_local_control_cache_devices
      └─ JSON 数组 → 按 localip 匹配 → token 字段
```

提取命令（root 手机）：
```bash
adb shell "su -c 'cat /data/user/0/com.xiaomi.smarthome/files/mmkv/mmkv_id_local_control'" \
  > mmkv.bin
```

局域网 miIO 握手（UDP 54321）在此设备上**拿不到真实 token** ——
新版 MIoT 固件握手回包的 token 字段是全 `FF` 占位符。

---

## 复现脚本（工作区）

| 文件 | 用途 |
|------|------|
| `read_power.py` | **主脚本** —— 读设备信息 + 实时功率（python-miio） |
| `parse_mmkv.js` | 解析 MMKV 提取 token |
| `extract_token.sh` | root 端提取流程（含 SQLite 兜底扫描） |
| `verify_miio.py` | python-miio 交叉验证 |
| `probe_props.py` / `sample_props.py` | siid/piid 遍历与采样定位功率属性 |
| `miio_probe.js` / `miio_scan.js` | 纯 Node 实现 miIO 握手 + 局域网广播扫描 |

运行：
```bash
python read_power.py 192.168.1.10 00112233445566778899aabbccddeeff
```

---

## miIO 协议备忘（本次踩坑）

```
key      = md5(token_hex的 utf-8 字节)
iv       = md5(key + token_hex)        ← 容易错，我最初写成 iv=key 导致解密失败
checksum = md5(key + 明文payload)
加密     = AES-128-CBC(payload)
报文     = 2131 + uint16LE(明文len+32) + ffffffff + 00000000 + 00000000 + checksum(16B) + 密文
```
- 握手包：`2131` + `0020` + 28×`ff`；回包 bytes 8..12 = device id（小端）
- `get_properties` 的 did 用**十进制字符串**（`"1234567890"`），不是 hex
- 老协议 `get_prop` 在此 MIoT 设备上不可用

---

## 复现命令：提取 token

```bash
# 1. 从手机提取（需 root）
adb shell "su -c 'cat /data/user/0/com.xiaomi.smarthome/files/mmkv/mmkv_id_local_control'" > mmkv.bin

# 2. 解析
node parse_mmkv.js mmkv.bin 192.168.1.10

# 3. 验证
python read_power.py 192.168.1.10 <token>
```
