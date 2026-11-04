// OptionsDlg.h - 设置对话框
#pragma once
#include <Windows.h>

#include "Config.h"

namespace mp { class ConfigStore; }

class CMiPowerPlugin;

namespace mp {

// 纯 Win32 对话框（不使用资源文件，避免 .rc 编译依赖）
class OptionsDlg {
public:
    // 显示设置对话框。返回 true 表示配置有改动。
    static bool Show(HWND hParent, AppConfig& cfg, ConfigStore& store,
                      CMiPowerPlugin* plugin);
};

} // namespace mp
