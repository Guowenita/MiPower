> **公开仓库说明**：本仓库为脱敏后的开源版本，源码、文档、配置模板与预编译
> DLL 中**不含任何真实 token、内网 IP、设备标识或个人路径**。所有设备信息均为
> 醒目占位符（`<...>` 与 RFC 5737 文档地址），使用前请按 `PRIVACY.md` 替换。
> `release/MiPower.dll` 为唯一预编译产物；也可执行 `python build.py release` 从源码构建。

---

# MiPower — TrafficMonitor 米家插座功耗插件

从零重写的 TrafficMonitor 插件，实时采集米家 / 酷控智能插座的功耗。

设计目标：**通用**（不硬编码型号）、**稳定**（错误分级降级）、**可维护**（分层清晰）。

---

## 交付内容速查

| 路径 | 内容 |
|---|---|
| `release/MiPower.dll` | **预编译插件 DLL（x64 / Release）**，唯一二进制产物，直接放入 TrafficMonitor 的 `plugins\` |
| `src/` | 插件本体源码（19 个 `.h` / `.cpp`） |
| `build.py` | 唯一构建入口：`python build.py release` |
| `docs/插件设计方案.md` | 架构、协议、数据流与实现取舍 |
| `README.md` / `PRIVACY.md` | 使用说明 / 脱敏与占位说明 |
| `config/config.example.ini` | 配置模板（**醒目占位值，必须替换**） |
| `config/tokens/plug-1.token.example` | token 占位模板 |
| `skins/0MiPower/` | 插件显示项所需皮肤（含 `plugin_map`） |
| `LICENSE` | MIT 许可 |

> 仓库内所有 `192.0.2.x` 均为 RFC 5737 文档保留地址，`<...>` 均为醒目占位符，
> **不能连接任何真实设备**。按 `PRIVACY.md` 替换后方可使用。

## 快速开始

### 1. 安装

把 `MiPower.dll` 放到 TrafficMonitor 的 `plugins` 目录：

```
<TrafficMonitor目录>\plugins\MiPower.dll
```

### 2. 配置

首次启动会在 `plugins` 目录生成 `config.ini` 和 `tokens` 文件夹：

```
plugins\
  ├─ MiPower.dll
  ├─ config.ini              ← 主配置（UTF-8 BOM）
  └─ tokens\
      └─ plug-1.token        ← 设备 token（一行 32 位 hex）
```

**编辑 `config.ini`**：

```ini
[Device.plug-1]
IP=192.0.2.1
Name=Example Plug
TokenFile=plug-1.token
Enabled=1
```

**填入 token** —— 把 32 位 hex 写进 `tokens/plug-1.token`：

```
<YOUR_DEVICE_TOKEN_32_HEX>
```

> token 获取方法见文末「如何获取 token」。
> 修改 token 文件后插件会在 5 秒内**自动热重载**，无需重启 TrafficMonitor。

### 3. 启用显示项

TrafficMonitor → 选项 → 显示项设置 → 勾选「米家插座功率」。

---

## 配置项说明

### `[General]`

| 键 | 默认 | 说明 |
|---|---|---|
| `SampleIntervalSec` | 2 | 采样间隔（秒），范围 1~60 |
| `RealtimeWindowSec` | 600 | 实时统计窗口（秒） |
| `HistoryRetentionDays` | 7 | 历史保留天数 |
| `PricePerKwh` | 0.65 | 电价（元/kWh），用于电费估算 |
| `EnableHistory` | 1 | 是否落盘历史 |
| `EnableDiagnostics` | 0 | 诊断模式（详细日志） |
| `MaxRetryIntervalSec` | 30 | 重连退避上限 |
| `RequestTimeoutMs` | 3000 | 单次请求超时 |
| `HeartbeatSec` | 30 | 握手心跳间隔 |

### `[Display]`

| 键 | 默认 | 说明 |
|---|---|---|
| `ShowLabel` | 1 | 任务栏显示「功率:」前缀 |
| `ShowUnit` | 1 | 显示 W 单位 |
| `ShowDeviceName` | 0 | 显示设备名（多设备时建议开） |
| `DecimalPlaces` | 1 | 小数位数 0~2 |
| `ShowEnergyInTooltip` | 1 | 悬停显示累计电量 |
| `ShowStatsInTooltip` | 1 | 悬停显示统计 |
| `StaleThresholdSec` | 30 | 超过此秒数标记为陈旧数据 |

### `[Device.<id>]`

| 键 | 说明 |
|---|---|
| `IP` | 设备局域网 IP（必填） |
| `Name` | 显示名 |
| `TokenFile` | token 文件名（相对 tokens 目录） |
| `PropertyMap` | 手动指定属性映射，格式 `siid.piid` 逗号分隔，顺序为 功率,电量,电压,电流,开关 |
| `Enabled` | 是否启用 |

例：`PropertyMap=4.2,4.1,4.3,4.4,2.1`

---

## 架构

```
┌──────────────────────────────────────────────┐
│ UI 层  CMiPowerPlugin / CMiPowerItem          │
│      OptionsDlg（纯 Win32 设置对话框）         │
├──────────────────────────────────────────────┤
│ 服务层  Collector（采集线程 + 退避重连）       │
│        Aggregator（环形缓冲 + 积分 + 缺口）    │
├──────────────────────────────────────────────┤
│ 协议层  MiioClient（miIO 编解码）              │
│        MiotSpecResolver（规格表 + 自动探测）   │
│        TokenProvider（token 加载 + 热重载）    │
├──────────────────────────────────────────────┤
│ 基础层  Crypto（MD5 + AES-128-CBC）           │
└──────────────────────────────────────────────┘
```

### 关键设计

**1. 属性映射不硬编码**

原插件把 `siid=11, piid=2` 写死在 C++ 字符串里，换个型号就失效（`-4003 属性不支持`）。
本插件按四级优先级解析：

```
手动 PropertyMap  →  内置规格表  →  规格缓存  →  主动探测
```

主动探测会遍历 siid 1~20 × piid 1~8，用**批量请求**（一次问多个属性）试探，
再结合「数值波动性 + 量级 + MIoT 服务语义」判定哪个是功率、哪个是电压电流。

内置表已覆盖：`sixwgh.plug.v8icwf`、`cuco.plug.v3`、`chuangmi.plug.v1/v2/v3`、
`chuangmi.plug.m1`、`zimi.plug.v1`、`cuco.plug.v1`、`sixwgh.plug.v1`。

**2. 一次请求取多个属性**

原插件一次只取功率一个属性。本插件一次 `get_properties` 取 5 个属性，
报文量降低约 80%，实测单次往返 **约 70ms**，成功率 100%。

**3. 错误分级降级**

| 错误 | 处理 |
|---|---|
| 超时 / 不可达 | 指数退避重连 1→2→4→8→30s |
| **Token 失效** | **停止自动重试**，明确提示「Token 失效」 |
| 属性不支持 | 自动触发规格重解析 |
| 设备忙 (-9999) | 延迟 1s 重试 |

显示策略：**宁可显示 `--`，也不显示过期数字假装是实时的**。
陈旧数据会标注 `(!)` 并显示最后更新时间。

**4. 数据完整性可追溯**

- 采集失败记录**缺口**（起止时间、失败次数、原因）
- 梯形积分计算本地累计电量
- 与**设备端累计电量**交叉校验 —— 后者由插座自己维护，PC 关机也在计量，是权威基准
- 悬停显示覆盖率与成功/失败计数

**5. 双时间戳**

采样点同时记录 wall-clock（UTC ms，用于持久化）和 monotonic（用于算间隔）。
系统时间被调整不会导致积分出错。

---

## 构建

需要 Visual Studio 2022 Build Tools（C++ 工作负载）与 Windows SDK。

```bash
python build.py release
```

产物为 `build/MiPower.dll`。仓库已提供预编译版 `release/MiPower.dll`，
无需自行构建即可安装使用。

---

## 如何获取 token

新版米家 App 把 token 存在 MMKV 里。**已 root 的安卓手机**可直接提取：

```bash
adb shell "su -c 'cat /data/data/com.xiaomi.smarthome/files/mmkv/mmkv_id_local_control'" > mmkv.bin
```

在 `mmkv.bin` 里搜 `key_local_control_cache_devices`，是一个 JSON 数组，
每个元素含 `localip` 和 `token` 字段。按 IP 匹配即可。

或者用开源工具：[Xiaomi-cloud-tokens-extractor](https://github.com/PiotrMachowski/Xiaomi-cloud-tokens-extractor)

> **注意**：局域网 miIO 握手（UDP 54321）拿不到真实 token —— 新版 MIoT 固件握手
> 回包的 token 字段是全 `FF` 占位符，必须从米家 App 本地库或小米云获取。

---

## 已知限制

- 只支持 UDP 54321 的 miIO 协议设备（米家 / 酷控 WiFi 插座、插排等）
- 不支持 WiFi 蓝牙双模设备走蓝牙回程
- 多设备时各设备独立采集，不做聚合统计
- 历史数据目前只写分钟聚合，未做图表展示

---

## 许可

MIT
