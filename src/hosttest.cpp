// hosttest.cpp - 模拟 TrafficMonitor 宿主加载插件，验证插件 DLL 完整链路
// 用法: hosttest.exe <plugins_dir>
#include <Windows.h>

#include <cstdio>
#include <string>

#include "PluginInterface.h"

static std::wstring g_configDir;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲，便于定位卡点
    std::string dirArg;
    if (argc >= 2) {
        dirArg = argv[1];
    } else {
        char* env = getenv("PLUGDIR");
        if (env) dirArg = env;
    }
    if (dirArg.empty()) {
        printf("用法: hosttest.exe <plugins_dir>  (或设置 PLUGDIR 环境变量)\n");
        return 1;
    }
    // 转成宽字符（分配 n 个，含结尾 NUL）
    int n = MultiByteToWideChar(CP_UTF8, 0, dirArg.c_str(), -1, nullptr, 0);
    if (n <= 0) { printf("[FAIL] 路径转换失败\n"); return 1; }
    std::wstring wbuf(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, dirArg.c_str(), -1, &wbuf[0], n);
    wbuf.resize(n - 1);           // 去掉结尾 NUL
    g_configDir = wbuf;

    printf("=== MiPower 宿主模拟测试 ===\n");
    printf("配置目录: %ls\n\n", g_configDir.c_str());

    // 1) 加载 DLL
    std::wstring dllPath = g_configDir + L"\\MiPower.dll";
    HMODULE h = LoadLibraryW(dllPath.c_str());
    if (!h) {
        printf("[FAIL] LoadLibrary 失败, err=%lu\n", GetLastError());
        return 1;
    }
    printf("[OK] DLL 加载成功\n");

    // 2) 取导出函数
    typedef ITMPlugin* (*GetInstanceFn)();
    GetInstanceFn getInstance = (GetInstanceFn)GetProcAddress(h, "TMPluginGetInstance");
    if (!getInstance) {
        printf("[FAIL] 未找到导出函数 TMPluginGetInstance\n");
        return 1;
    }
    printf("[OK] 找到导出函数 TMPluginGetInstance\n");

    ITMPlugin* plugin = getInstance();
    if (!plugin) {
        printf("[FAIL] getInstance 返回空\n");
        return 1;
    }
    printf("[OK] 插件实例: API version %d\n", plugin->GetAPIVersion());

    // 3) 插件信息
    const wchar_t* names[] = { L"名称", L"描述", L"作者", L"版权", L"版本" };
    for (int i = 0; i < 5; i++) {
        const wchar_t* v = plugin->GetInfo((ITMPlugin::PluginInfoIndex)i);
        printf("  %s: %ls\n", names[i], v ? v : L"(null)");
    }

    // 4) 模拟宿主传配置目录
    printf("\n[INFO] 调用 OnExtenedInfo(EI_CONFIG_DIR) ...\n");
    plugin->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, g_configDir.c_str());

    // 5) 触发数据采集
    printf("[INFO] 调用 DataRequired() 启动采集线程 ...\n");
    plugin->DataRequired();

    // 6) 等待并读取显示项
    IPluginItem* item = plugin->GetItem(0);
    if (!item) {
        printf("[FAIL] GetItem(0) 返回空\n");
        return 1;
    }
    printf("[OK] 显示项: %ls (id=%ls)\n", item->GetItemName(), item->GetItemId());

    printf("\n=== 连续读取 12 秒 ===\n");
    for (int i = 0; i < 12; i++) {
        Sleep(1000);
        const wchar_t* label = item->GetItemLableText();
        const wchar_t* value = item->GetItemValueText();
        size_t lenL = label ? wcslen(label) : 0;
        size_t lenV = value ? wcslen(value) : 0;
        // 宽字符输出到 UTF-8，控制台中文不乱码
        auto toUtf8 = [](const wchar_t* w) -> std::string {
            if (!w) return "";
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n <= 1) return "";
            std::string s(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
            s.resize(n - 1);
            return s;
        };
        printf("  [%2ds] %s | %s\n", i + 1, toUtf8(label).c_str(), toUtf8(value).c_str());
    }

    // 7) Tooltip
    printf("\n=== Tooltip ===\n");
    const wchar_t* tip = plugin->GetTooltipInfo();
    if (tip) {
        // 转成 UTF-8 输出
        int m = WideCharToMultiByte(CP_UTF8, 0, tip, -1, nullptr, 0, nullptr, nullptr);
        std::string s(m > 1 ? m - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, tip, -1, &s[0], m, nullptr, nullptr);
        printf("%s\n", s.c_str());
    } else {
        printf("(null)\n");
    }

    printf("\n[OK] 测试完成\n");
    return 0;
}
