// dlucalc.cpp — 实测 DLU 与像素的真实换算关系
//
// 方法: 依次创建不同 cx/cy 的空对话框，量出客户区像素尺寸，
//       反推 1 DLU 对应多少像素（因系统字体/DPI 不同，不能硬编码）。
#include <cstdio>
#include <vector>

#include <Windows.h>

#pragma comment(lib, "user32.lib")

static INT_PTR CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) { return FALSE; }

int main() {
    printf("=== DLU -> 像素 实测 ===\n");
    printf("%-10s %-10s %-10s %-12s %-12s\n",
           "cx(DLU)", "cy(DLU)", "窗口W", "客户区W", "客户区H");

    struct Case { short cx, cy; };
    std::vector<Case> cases = {
        {50, 20}, {100, 40}, {110, 50}, {150, 60}, {200, 70}
    };

    for (const auto& c : cases) {
        // 构造最小对话框模板
        const wchar_t* title = L"x";
        size_t tc = wcslen(title) + 1;
        size_t tb = tc * sizeof(wchar_t);
        size_t total = sizeof(DLGTEMPLATE) + sizeof(WORD) * 2 + tb + sizeof(WORD);
        std::vector<BYTE> buf(total, 0);
        BYTE* p = buf.data();
        DLGTEMPLATE* dt = (DLGTEMPLATE*)p;
        dt->style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | DS_FIXEDSYS |
                    WS_POPUP | WS_CAPTION | WS_SYSMENU;
        dt->dwExtendedStyle = 0;
        dt->cdit = 0;
        dt->x = 0; dt->y = 0;
        dt->cx = c.cx; dt->cy = c.cy;
        WORD* w = (WORD*)(p + sizeof(DLGTEMPLATE));
        *w++ = 0; *w++ = 0;
        memcpy(w, title, tb); w += tc;
        *w++ = 9;

        // DialogBoxIndirect 会阻塞，用 WM_INITDIALOG 里量尺寸再 EndDialog
        struct Ctx { int cw, ch; };
        static Ctx ctx;

        // 简化: 直接 CreateWindowEx 不行（无对话框过程），
        // 改用 DialogBoxIndirectParam + 在 WM_INITDIALOG 里量
        // 这里用 trick: 模板不带标题栏, 用 CreateWindowExW 也不行。
        // 用 DialogBoxIndirectParam, 在 proc 里量完立刻 EndDialog
        // proc 是静态的, 用全局变量传
        static short g_cx, g_cy;
        g_cx = c.cx; g_cy = c.cy;
        static int g_cw, g_ch;

        struct L {
            static INT_PTR CALLBACK P(HWND h, UINT m, WPARAM w, LPARAM l) {
                if (m == WM_INITDIALOG) {
                    RECT r{};
                    GetClientRect(h, &r);
                    g_cw = r.right - r.left;
                    g_ch = r.bottom - r.top;
                    EndDialog(h, IDCANCEL);
                    return TRUE;
                }
                return FALSE;
            }
        };
        (void)Proc;

        DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                                (LPCDLGTEMPLATEW)buf.data(), nullptr, L::P, 0);
        printf("%-10d %-10d %-10s %-12d %-12d\n",
               c.cx, c.cy, "-", g_cw, g_ch);
    }
    return 0;
}
