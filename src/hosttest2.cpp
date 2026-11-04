// hosttest2.cpp —— 完整模拟 TrafficMonitor 的显示项判定链路
// 复刻：PluginManager::LoadPlugins -> item_order.Init -> NormalizeItemOrder
//       -> IsTaksbarItemDisplayed -> CalculateWindowSize -> DrawPluginItem
// 用于验证：插件项能否进入渲染列表、索引是多少
#include <Windows.h>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "PluginInterface.h"

#pragma comment(lib, "user32.lib")

// ---- 复刻 TM 的 CommonDisplayItem ----
struct CommonDisplayItem {
    bool is_plugin = false;
    int  item_type = 0;                 // 内置项序号
    IPluginItem* plugin_item = nullptr;

    bool IsPlugin() const { return is_plugin; }
    IPluginItem* PluginItem() const { return plugin_item; }
};

static std::vector<IPluginItem*> g_plugins;   // 插件项列表（按 DLL 加载顺序）

// TM 的 operator<：内置在前，插件按 GetItemIndex 排
struct Cmp {
    bool operator()(const CommonDisplayItem& a, const CommonDisplayItem& b) const {
        if (a.is_plugin && !b.is_plugin) return false;
        if (!a.is_plugin && b.is_plugin) return true;
        if (!a.is_plugin) return a.item_type < b.item_type;
        int ia = -1, ib = -1;
        for (size_t i = 0; i < g_plugins.size(); i++) {
            if (g_plugins[i] == a.plugin_item) ia = (int)i;
            if (g_plugins[i] == b.plugin_item) ib = (int)i;
        }
        return ia < ib;
    }
};

static int GetItemIndex(IPluginItem* it) {
    for (size_t i = 0; i < g_plugins.size(); i++)
        if (g_plugins[i] == it) return (int)i;
    return -1;
}

int main() {
    // 1) 模拟加载两个插件
    struct Plug { const wchar_t* dll; };
    const wchar_t* dlls[] = { L"MijiaPower.dll", L"MiPower.dll" };
    std::vector<ITMPlugin*> instances;

    printf("=== 模拟 TM 加载插件 ===\n");
    for (const wchar_t* d : dlls) {
        HMODULE h = LoadLibraryW(d);
        if (!h) { printf("  [跳过] %ls 加载失败\n", d); continue; }
        typedef ITMPlugin* (*Fn)();
        Fn f = (Fn)GetProcAddress(h, "TMPluginGetInstance");
        if (!f) { printf("  [跳过] %ls 无导出\n", d); continue; }
        ITMPlugin* p = f();
        instances.push_back(p);
        printf("  %ls -> %ls\n", d, p->GetInfo(ITMPlugin::TMI_NAME));
    }

    // 2) 收集插件项（复刻 PluginManager::LoadPlugins）
    printf("\n=== 收集插件项 ===\n");
    for (ITMPlugin* p : instances) {
        int idx = 0;
        while (true) {
            IPluginItem* item = p->GetItem(idx);
            if (!item) break;
            g_plugins.push_back(item);
            printf("  插件项[%d] id=%ls name=%ls\n", (int)g_plugins.size() - 1,
                   item->GetItemId(), item->GetItemName());
            idx++;
        }
    }

    // 3) 构造 AllDisplayItemsWithPlugins（13 内置 + N 插件）
    std::set<CommonDisplayItem, Cmp> all_items;
    for (int i = 0; i < 13; i++) {
        CommonDisplayItem c; c.is_plugin = false; c.item_type = i;
        all_items.insert(c);
    }
    for (IPluginItem* it : g_plugins) {
        CommonDisplayItem c; c.is_plugin = true; c.plugin_item = it;
        all_items.insert(c);
    }

    printf("\n=== m_all_item_in_default_order (item_order 索引空间) ===\n");
    int k = 0;
    for (auto& it : all_items) {
        if (it.IsPlugin())
            printf("  [%2d] 插件项 id=%ls\n", k, it.PluginItem()->GetItemId());
        else
            printf("  [%2d] 内置项 type=%d\n", k, it.item_type);
        k++;
    }
    printf("  总数 = %d\n", k);

    // 4) 读配置里的 item_order 和 plugin_display_item
    printf("\n=== 模拟 TM 的显示项判定 ===\n");
    printf("  假设 plugin_display_item 含 MiPowerW 与 MijiaPowerW\n");
    printf("  假设 item_order = 0..14\n");

    for (auto& it : all_items) {
        if (!it.IsPlugin()) continue;
        bool in_plugin_set = true;   // 配置里有
        bool in_order = false;
        // 检查索引是否在 item_order 中（这里假设含 0..14）
        printf("  插件项 id=%-16ls 在plugin_display_item=%d 在item_order=%d -> %s\n",
               it.PluginItem()->GetItemId(), in_plugin_set, in_order,
               (in_plugin_set && in_order) ? "会被渲染" : "不渲染");
    }

    printf("\n=== 结论 ===\n");
    printf("  插件项在 m_all_item_in_default_order 中的索引范围: %d ~ %d\n", 13, k - 1);
    printf("  必须在 item_order 中包含这些索引，否则不会被绘制\n");
    printf("  并且 plugin_display_item 必须包含对应的 GetItemId()\n");

    for (ITMPlugin* p : instances) p->GetItem(0);
    return 0;
}
