# token 文件说明

`plug-1.token.example` 只是**占位示例**，不是任何真实设备的凭据。

使用步骤：

1. 在 TrafficMonitor 的 `plugins/tokens/` 目录下新建 `plug-1.token`
2. 写入你自己设备的 32 位十六进制 token（单行、无空格、无换行）
3. 修改 token 后插件会在数秒内自动热重载，无需重启 TrafficMonitor

获取 token 的方法见根目录 `README.md` 的「如何获取 token」章节。

> 真实的 `*.token` 文件已被 `.gitignore` 排除，请勿提交到版本库。
