# 脱敏与占位说明

本仓库是公开版本，**不含任何真实设备凭据或个人/设备标识**。所有示例值均为
醒目占位符；仓库内容不能连接任何真实设备，使用前必须替换。

## 占位符对照

| 类别 | 仓库中的占位值 | 你需要替换为 |
|---|---|---|
| 设备 token | `<YOUR_DEVICE_TOKEN_32_HEX>` | 你自己设备的 32 位十六进制 token |
| 设备 IP | `192.0.2.1`（RFC 5737 文档保留地址） | 设备局域网 IP |
| 测试用 IP | `192.0.2.2` / `192.0.2.3` | 仅测试代码使用，无需替换 |
| 设备 DID | `<DEVICE_DID>` / `<DEVICE_DID_HEX>` | 文档示例，无需替换 |
| 设备 MAC | `<DEVICE_MAC>` | 文档示例，无需替换 |
| Wi-Fi SSID | `<WIFI_SSID>` | 文档示例，无需替换 |
| 米家账号 UID | `<ACCOUNT_UID>` | 文档示例，无需替换 |
| 设备名 | `Example Plug` | 自定义显示名 |

## 仍然保留的内容（非隐私）

- 设备型号 `sixwgh.plug.v8icwf` 等公开 MIoT 规格标识（内置规格表依赖）
- 属性映射 `4.2,4.1,4.3,4.4,2.1` 等公开协议语义
- miIO / MIoT 报文格式、错误码与 token 获取的通用方法

## 防御措施

- `.gitignore` 排除 `*.token`、真实 `config.ini`、构建中间产物、日志与备份
- `config/config.example.ini` 与 `config/tokens/plug-1.token.example` 是可提交的模板
- 源码默认 IP 指向不可路由的文档保留地址，未配置时不会误连任何设备

## 使用前的必要替换

1. 把 `config/config.example.ini` 复制为 TrafficMonitor 的 `plugins/config.ini`
2. 把 `IP` 改为你的设备 IP
3. 在 `plugins/tokens/` 写入你自己设备的 `plug-1.token`
4. 在 TrafficMonitor「选项 → 显示项设置」勾选「米家插座功率」
