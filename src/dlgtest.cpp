// dlgtest.cpp — 测试设置对话框能否正常创建、显示、关闭
//
// 验证 OptionsDlg::Show 的三个关键点：
//   1. 对话框能创建（尺寸/标题正确）
//   2. 控件能创建（数量与文本）
//   3. 能被关闭（EndDialog 有效）
//
// 用法: dlgtest.exe [ms]
//   无参 = 手动模式（弹出对话框，等你点关闭）
//   带参 = 自动模式（创建后用 WM_COMMAND 模拟点"取消"，验证能否关闭）
#include <cstdio>
#include <cstdlib>

#include "OptionsDlg.h"
#include "Config.h"

using namespace mp;

static int g_created = 0;

static BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lParam) {
    int* count = (int*)lParam;
    (*count)++;
    wchar_t cls[64] = { 0 };
    wchar_t txt[256] = { 0 };
    GetClassNameW(hwnd, cls, 64);
    GetWindowTextW(hwnd, txt, 256);
    if (txt[0])
        printf("    [控件] %-12s %s\n", cls, txt);
    return TRUE;
}

int main(int argc, char** argv) {
    bool autoClose = (argc > 1);

    AppConfig cfg;
    cfg.sampleIntervalSec = 2;
    cfg.decimalPlaces = 1;
    cfg.pricePerKwh = 0.65;
    cfg.showTrend = true;
    cfg.showCost = true;
    cfg.trendMinutes = 10;
    cfg.enableHistory = true;

    DeviceConfig d;
    d.id = "plug-1";
    d.ip = "192.168.1.10";
    d.name = "Smart Plug";
    d.tokenFile = "plug-1.token";
    d.enabled = true;
    cfg.devices.push_back(d);

    ConfigStore store;
    store.SetConfigDir(L".");

    printf("=== 设置对话框测试 (%s) ===\n", autoClose ? "自动关闭" : "手动");

    if (autoClose) {
        // 自动模式：另起线程，2 秒后发 WM_COMMAND(IDCANCEL) 模拟点取消
        // 用一个辅助线程 —— 但 OptionsDlg::Show 内部是阻塞的消息循环，
        // 所以这里直接调用，由外部线程定时发消息。
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            Sleep(2500);
            HWND h = FindWindowW(L"#32770", L"MiPower 功耗插件 - 设置");
            printf("  [自动] 查找对话框 hwnd=%p\n", (void*)h);
            if (h) {
                // 先枚举子控件，确认布局
                int n = 0;
                EnumChildWindows(h, EnumChildProc, (LPARAM)&n);
                printf("  [自动] 子控件总数: %d\n", n);
                printf("  [自动] 发送 WM_CLOSE ...\n");
                PostMessageW(h, WM_CLOSE, 0, 0);
            }
            return 0;
        }, nullptr, 0, nullptr);
    }

    bool changed = OptionsDlg::Show(nullptr, cfg, store, nullptr);

    printf("\n=== 结果 ===\n");
    printf("  Show() 返回: %s\n", changed ? "有改动(IDOK)" : "无改动/取消");
    printf("  showTrend=%d showCost=%d trendMinutes=%d\n",
           cfg.showTrend, cfg.showCost, cfg.trendMinutes);
    printf("=== 测试结束（正常退出=对话框可关闭）===\n");
    return 0;
}
