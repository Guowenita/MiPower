// dlggeom.cpp — 检查设置对话框的实际尺寸与控件位置，验证布局正确
#include <cstdio>
#include <algorithm>
#include <vector>

#include "OptionsDlg.h"
#include "Config.h"

using namespace mp;

static void Report(HWND dlg) {
    RECT r{};
    GetWindowRect(dlg, &r);
    printf("  对话框尺寸: %d x %d  (屏幕坐标 %d,%d)\n",
           r.right - r.left, r.bottom - r.top, r.left, r.top);

    int idx = 0;
    struct Ctl { HWND h; };
    std::vector<HWND> list;
    // 枚举所有子控件，按 y 排序输出
    struct Item { int y, x, w, h; std::wstring cls, txt; };
    std::vector<Item> items;

    EnumChildWindows(dlg, [](HWND h, LPARAM p) -> BOOL {
        auto* v = (std::vector<Item>*)p;
        RECT r{};
        GetWindowRect(h, &r);
        wchar_t cls[64] = { 0 }, txt[256] = { 0 };
        GetClassNameW(h, cls, 64);
        GetWindowTextW(h, txt, 256);
        Item it{};
        it.y = r.top; it.x = r.left;
        it.w = r.right - r.left; it.h = r.bottom - r.top;
        it.cls = cls; it.txt = txt;
        v->push_back(it);
        return TRUE;
    }, (LPARAM)&items);

    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });

    printf("  控件布局 (共 %zu 个):\n", items.size());
    int maxRight = 0;
    for (const auto& it : items) {
        printf("    y=%3d x=%3d %3dx%-3d %-8s %ls\n",
               it.y, it.x, it.w, it.h, it.cls.c_str(),
               it.txt.empty() ? L"(无文本)" : it.txt.c_str());
        if (it.x + it.w > maxRight) maxRight = it.x + it.w;
    }
    printf("  最右边界: %d (对话框宽 %d) -> %s\n", maxRight,
           r.right - r.left,
           maxRight <= (r.right - r.left) ? "控件未溢出 OK" : "控件溢出 !!");
}

int main() {
    AppConfig cfg;
    cfg.showTrend = true;
    cfg.showCost = true;
    cfg.trendMinutes = 10;
    DeviceConfig d;
    d.id = "plug-1"; d.ip = "192.168.1.10"; d.name = "Smart Plug";
    d.tokenFile = "plug-1.token"; d.enabled = true;
    cfg.devices.push_back(d);

    ConfigStore store;
    store.SetConfigDir(L".");

    printf("=== 设置框几何检查 ===\n");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Sleep(2000);
        HWND h = FindWindowW(L"#32770", L"MiPower 功耗插件 - 设置");
        if (h) {
            printf("--- 布局详情 ---\n");
            Report(h);
            PostMessageW(h, WM_CLOSE, 0, 0);
        } else {
            printf("  [!] 未找到对话框\n");
        }
        return 0;
    }, nullptr, 0, nullptr);

    OptionsDlg::Show(nullptr, cfg, store, nullptr);
    printf("=== 结束 ===\n");
    return 0;
}
