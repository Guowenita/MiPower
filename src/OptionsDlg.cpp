// OptionsDlg.cpp - 设置对话框
//
// 实现方式: 内存中的 DLGTEMPLATE + DialogBoxIndirectParam
//
// 为什么不用之前的 CreateWindowExW(L"#32770") 方案:
//   1. #32770 必须由 DialogBox/DialogBoxIndirect 配合模板创建，
//      手工 CreateWindowEx 建出来的 #32770 没有默认对话框过程，
//      导致尺寸异常、控件布局错乱。
//   2. EndDialog() 只对 DialogBox 系列创建的对话框有效，
//      所以之前那个窗口点了 X / 取消 都关不掉。
//   3. 控件的 hInstance 应传本 DLL 的模块句柄，
//      传 GetModuleHandleW(nullptr) 会拿到宿主 EXE，样式不一致。
#include "OptionsDlg.h"

#include <commctrl.h>
#include <shellapi.h>
#include <mutex>
#include <sstream>
#include <vector>

#include "MiPowerPlugin.h"
#include "MiotSpec.h"

#pragma comment(lib, "comctl32.lib")

namespace mp {

namespace {

// 控件 ID
enum {
    IDC_STATIC = 1000,
    IDC_IP, IDC_TOKEN, IDC_NAME, IDC_INTERVAL, IDC_DECIMAL,
    IDC_SHOWLABEL, IDC_SHOWUNIT, IDC_SHOWDEVNAME, IDC_SHOWENERGY, IDC_SHOWSTATS,
    IDC_HISTORY, IDC_PRICE, IDC_PROPMAP, IDC_STATUS,
    IDC_TEST, IDC_OK, IDC_CANCEL, IDC_OPENTOKEN, IDC_APPLY,
    IDC_SHOWTREND, IDC_SHOWCOST, IDC_TRENDMIN
};

// ─── 单位说明（踩坑记录）─────────────────────────────────────
//
// 1. 对话框的 cx/cy 用 DLU（Dialog Unit）。
// 2. 但在 WM_INITDIALOG 里用 CreateWindowExW 动态创建的子控件，
//    坐标用的是父窗口客户区的「像素」，不是 DLU！
//    （只有写进对话框模板里的控件才用 DLU）
// 3. 因此两边单位不同，必须把 cx/cy 换算成能容下像素布局的尺寸，
//    否则控件会溢出到对话框外面 —— 表现为「界面显示不对」。
//
// 换算率依赖 DPI（实测本机 1 DLU = 2px，高 DPI 下会变），
// 所以硬编码不可靠 —— 必须在运行时量。
//
// 办法: 先用一个「探针对话框」量出本机的 1 DLU 等于多少像素，
//       再据此计算真正对话框的 cx/cy。
//
// 探针实现: 建一个 10x10 DLU 的空对话框，读它的客户区像素尺寸，
//           除以 10 就得到每 DLU 的像素数。

struct DluScale {
    int pxPerDlu = 2;    // 默认值（实测本机为 2）
};

// 量出本机 DLU->像素 的比例
DluScale MeasureDluScale() {
    DluScale s;
    const wchar_t* title = L"";
    size_t tc = 1;
    size_t tb = tc * sizeof(wchar_t);
    size_t total = sizeof(DLGTEMPLATE) + sizeof(WORD) * 2 + tb + sizeof(WORD);
    std::vector<BYTE> buf(total, 0);
    BYTE* p = buf.data();
    DLGTEMPLATE* dt = reinterpret_cast<DLGTEMPLATE*>(p);
    dt->style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | DS_FIXEDSYS |
                WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt->dwExtendedStyle = 0;
    dt->cdit = 0;
    dt->x = 0; dt->y = 0;
    dt->cx = 10; dt->cy = 10;
    WORD* w = reinterpret_cast<WORD*>(p + sizeof(DLGTEMPLATE));
    *w++ = 0; *w++ = 0;
    memcpy(w, title, tb); w += tc;
    *w++ = 9;

    static int g_clientW = 0;
    struct Local {
        static INT_PTR CALLBACK P(HWND h, UINT m, WPARAM, LPARAM) {
            if (m == WM_INITDIALOG) {
                RECT r{};
                GetClientRect(h, &r);
                g_clientW = r.right - r.left;
                EndDialog(h, IDCANCEL);
                return TRUE;
            }
            return FALSE;
        }
    };
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                            reinterpret_cast<LPCDLGTEMPLATEW>(buf.data()),
                            nullptr, Local::P, 0);
    if (g_clientW > 0) {
        int per = g_clientW / 10;
        if (per > 0) s.pxPerDlu = per;
    }
    return s;
}

// 布局设计稿（像素）
// 宽度取 340：实测客户区约 372px（对话框 428 - 边框/标题栏），
// 留 30px 余量，确保最右侧控件不被裁切。
constexpr int kDesignWidthPx  = 340;   // 控件布局宽度（像素）
constexpr int kDesignHeightPx = 330;   // 控件布局高度（像素）

struct DlgState {
    AppConfig* cfg = nullptr;       // 宿主配置：仅在「确定/应用」时加锁写回
    ConfigStore* store = nullptr;
    CMiPowerPlugin* plugin = nullptr;
    bool changed = false;
    HWND hDlg = nullptr;
    HWND hStatus = nullptr;

    // 编辑副本：控件的读取与回写只碰它。
    // 之前 ReadControls 直接改宿主 m_cfg，连「测试连接」都会改内存配置，
    // 点「取消」后内存里的值已经变了，但 ini 没变 —— 状态不一致。
    AppConfig    edit;
    std::wstring editToken;         // token 输入框内容，仅提交时落盘
};

DlgState g_state;

HINSTANCE DllInstance() {
    // 本 DLL 的模块句柄 —— 控件样式/字体会与 DLL 资源一致
    static HINSTANCE inst = nullptr;
    if (!inst) {
        // GetModuleHandleExW 用地址取自身模块句柄
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&DllInstance, &m);
        inst = (HINSTANCE)m;
    }
    return inst;
}

std::wstring WidenS(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(n - 1);
    return w;
}

std::string NarrowS(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.resize(n - 1);
    return s;
}

void SetText(HWND h, const std::wstring& t) { if (h) SetWindowTextW(h, t.c_str()); }

std::wstring GetText(HWND h) {
    if (!h) return L"";
    int len = GetWindowTextLengthW(h);
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(h, &s[0], len + 1);
    s.resize(len);
    return s;
}

void SetCheck(HWND h, bool v) { if (h) SendMessageW(h, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); }
bool GetCheck(HWND h) { return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

// 设置框只编辑第一个启用的设备（与采集器 PickDevice 的取法保持一致）
DeviceConfig* PickDevice(AppConfig& c) {
    for (auto& d : c.devices)
        if (d.enabled) return &d;
    return c.devices.empty() ? nullptr : &c.devices[0];
}

// 动态创建的子控件坐标就是像素，无需换算（保留此函数以便统一调用点）。
void PxToDlu(HWND dlg, int px, int py, int pw, int ph,
             int* dx, int* dy, int* dw, int* dh) {
    (void)dlg;
    *dx = px;
    *dy = py;
    *dw = pw;
    *dh = ph;
}

HWND AddLabel(HWND dlg, int id, int x, int y, int w, const wchar_t* text) {
    int dx, dy, dw, dh;
    PxToDlu(dlg, x, y, w, 20, &dx, &dy, &dw, &dh);
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                           dx, dy, dw, dh, dlg, (HMENU)(INT_PTR)id,
                           DllInstance(), nullptr);
}

HWND AddEdit(HWND dlg, int id, int x, int y, int w, const std::wstring& text, int style = 0) {
    int dx, dy, dw, dh;
    PxToDlu(dlg, x, y, w, 24, &dx, &dy, &dw, &dh);
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style,
                           dx, dy, dw, dh, dlg, (HMENU)(INT_PTR)id,
                           DllInstance(), nullptr);
}

HWND AddCheck(HWND dlg, int id, int x, int y, int w, const wchar_t* text) {
    int dx, dy, dw, dh;
    PxToDlu(dlg, x, y, w, 20, &dx, &dy, &dw, &dh);
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                           dx, dy, dw, dh, dlg, (HMENU)(INT_PTR)id,
                           DllInstance(), nullptr);
}

HWND AddButton(HWND dlg, int id, int x, int y, int w, int h, const wchar_t* text) {
    int dx, dy, dw, dh;
    PxToDlu(dlg, x, y, w, h, &dx, &dy, &dw, &dh);
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                           dx, dy, dw, dh, dlg, (HMENU)(INT_PTR)id,
                           DllInstance(), nullptr);
}

// 从控件读取配置到「编辑副本」，不碰宿主配置、不写文件
void ReadControls(DlgState& st) {
    AppConfig& c = st.edit;
    HWND dlg = st.hDlg;

    DeviceConfig* dev = PickDevice(c);
    if (dev) {
        dev->ip = NarrowS(GetText(GetDlgItem(dlg, IDC_IP)));
        dev->name = NarrowS(GetText(GetDlgItem(dlg, IDC_NAME)));
        dev->propertyMap = NarrowS(GetText(GetDlgItem(dlg, IDC_PROPMAP)));
    }

    // token 只记在编辑状态里，等「确定/应用」才落盘
    st.editToken = GetText(GetDlgItem(dlg, IDC_TOKEN));

    c.sampleIntervalSec = _wtoi(GetText(GetDlgItem(dlg, IDC_INTERVAL)).c_str());
    if (c.sampleIntervalSec < 1) c.sampleIntervalSec = 2;
    c.decimalPlaces     = _wtoi(GetText(GetDlgItem(dlg, IDC_DECIMAL)).c_str());
    if (c.decimalPlaces < 0) c.decimalPlaces = 0;
    if (c.decimalPlaces > 3) c.decimalPlaces = 3;
    c.pricePerKwh       = _wtof(GetText(GetDlgItem(dlg, IDC_PRICE)).c_str());
    if (c.pricePerKwh < 0) c.pricePerKwh = 0;

    c.showLabel       = GetCheck(GetDlgItem(dlg, IDC_SHOWLABEL));
    c.showUnit        = GetCheck(GetDlgItem(dlg, IDC_SHOWUNIT));
    c.showDeviceName  = GetCheck(GetDlgItem(dlg, IDC_SHOWDEVNAME));
    c.showEnergyInTooltip = GetCheck(GetDlgItem(dlg, IDC_SHOWENERGY));
    c.showStatsInTooltip  = GetCheck(GetDlgItem(dlg, IDC_SHOWSTATS));
    c.enableHistory   = GetCheck(GetDlgItem(dlg, IDC_HISTORY));
    c.showTrend       = GetCheck(GetDlgItem(dlg, IDC_SHOWTREND));
    c.showCost        = GetCheck(GetDlgItem(dlg, IDC_SHOWCOST));
    c.trendMinutes    = _wtoi(GetText(GetDlgItem(dlg, IDC_TRENDMIN)).c_str());
    if (c.trendMinutes < 1) c.trendMinutes = 10;
    if (c.trendMinutes > 120) c.trendMinutes = 120;
}

// 提交时把 token 写入文件。
// 之前这一步在 ReadControls 里做，于是「测试连接」甚至「取消」都会改
// token 文件，而内存配置与 ini 没跟着变 —— 三处状态不一致。
void CommitToken(DlgState& st) {
    std::string narrow = NarrowS(st.editToken);
    while (!narrow.empty() && (narrow.back() == '\r' || narrow.back() == '\n' || narrow.back() == ' '))
        narrow.pop_back();
    if (narrow.empty()) return;      // 留空 = 保留原 token 文件（沿用原行为）
    DeviceConfig* dev = PickDevice(st.edit);
    if (!dev) return;
    std::wstring tp = st.store->TokenPath(*dev);
    FILE* f = nullptr;
    if (_wfopen_s(&f, tp.c_str(), L"wb") == 0 && f) {
        fwrite(narrow.data(), 1, narrow.size(), f);
        fclose(f);
    }
}

// ── 测试连接线程 ────────────────────────────────────────────
struct TestCtx {
    AppConfig cfg;
    ConfigStore store;
    HWND hDlg = nullptr;
    std::string result;
    std::string token;      // 用编辑框里（或现有文件里）的 token 实测
};

DWORD WINAPI TestThread(LPVOID param) {
    TestCtx* ctx = (TestCtx*)param;
    AppConfig& c = ctx->cfg;
    std::string result;

    for (auto& d : c.devices) {
        if (!d.enabled) continue;

        MiioClient client;
        // MiioClient::Open 需要真实 token 才能派生 key/iv，传空串必然失败。
        // 之前测试连接写死 ""，所以按钮永远报「无法打开套接字」。
        if (!client.Open(d.ip, ctx->token, c.requestTimeoutMs)) {
            result = "连接失败: 无法打开套接字 (token 无效或缺失)";
            continue;
        }

        ErrorKind err = ErrorKind::None;
        if (!client.Handshake(err)) {
            result = "握手失败 (请检查 token 是否正确)";
            continue;
        }

        MiotSpecResolver resolver;
        PropertyMap map;
        if (!resolver.Resolve(client, map, d.propertyMap)) {
            result = "属性映射解析失败: " + resolver.LastDiagnostics();
            continue;
        }

        DeviceInfo info;
        ErrorKind ie = ErrorKind::None;
        std::string model = map.model;
        if (client.GetDeviceInfo(info, ie))
            model = info.model;

        std::vector<PropertyValue> values;
        ErrorKind pe = ErrorKind::None;
        auto props = map.BuildQueryList();
        if (!client.GetProperties(props, values, pe)) {
            result = "连接成功，但读取属性失败";
            continue;
        }

        double watts = 0;
        bool found = false;
        for (const auto& v : values) {
            if (v.code == 0 && v.siid == map.siidPower && v.piid == map.piidPower
                && v.hasValue) {
                watts = v.value;
                found = true;
            }
        }
        std::ostringstream oss;
        oss << "连接成功  " << model << "  功率 " << watts << " W";
        if (!found) oss << "  (未取到功率值)";
        result = oss.str();
        break;
    }

    if (result.empty()) result = "没有启用的设备";
    PostMessageW(ctx->hDlg, WM_APP + 100, 0, (LPARAM)new std::wstring(WidenS(result)));
    return 0;
}

// ── 对话框过程 ──────────────────────────────────────────────
INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_state.hDlg = hDlg;
            AppConfig& c = g_state.edit;

            // ── 输入区（标签 90px + 输入框 210px = 300px，右边界 312）──
            int y = 10;
            AddLabel(hDlg, IDC_STATIC, 12, y, 96, L"设备 IP:");
            HWND hIp = AddEdit(hDlg, IDC_IP, 104, y - 4, 208, L"");
            y += 28;
            AddLabel(hDlg, IDC_STATIC, 12, y, 96, L"Token:");
            HWND hToken = AddEdit(hDlg, IDC_TOKEN, 104, y - 4, 208, L"");
            y += 28;
            AddLabel(hDlg, IDC_STATIC, 12, y, 96, L"名称:");
            HWND hName = AddEdit(hDlg, IDC_NAME, 104, y - 4, 208, L"");
            y += 28;
            AddLabel(hDlg, IDC_STATIC, 12, y, 96, L"属性映射:");
            HWND hMap = AddEdit(hDlg, IDC_PROPMAP, 104, y - 4, 208, L"");
            y += 28;

            AddLabel(hDlg, IDC_STATIC, 12, y, 96, L"采样间隔(秒)");
            HWND hInt = AddEdit(hDlg, IDC_INTERVAL, 104, y - 4, 56, L"");
            AddLabel(hDlg, IDC_STATIC, 172, y, 70, L"小数位数:");
            HWND hDec = AddEdit(hDlg, IDC_DECIMAL, 240, y - 4, 56, L"");
            y += 28;
            AddLabel(hDlg, IDC_STATIC, 12, y, 96, L"电价(元/kWh)");
            HWND hPrice = AddEdit(hDlg, IDC_PRICE, 104, y - 4, 56, L"");
            y += 28;

            // ── 显示选项（每行两个，各 160px 宽）──
            AddCheck(hDlg, IDC_SHOWLABEL, 14, y, 160, L"显示\"功率:\"标签");
            AddCheck(hDlg, IDC_SHOWUNIT, 178, y, 150, L"显示 W 单位");
            y += 22;
            AddCheck(hDlg, IDC_SHOWDEVNAME, 14, y, 160, L"显示设备名");
            AddCheck(hDlg, IDC_SHOWENERGY, 178, y, 150, L"悬停显示累计电量");
            y += 22;
            AddCheck(hDlg, IDC_SHOWSTATS, 14, y, 160, L"悬停显示统计");
            AddCheck(hDlg, IDC_HISTORY, 178, y, 150, L"保存历史记录");
            y += 26;

            // ── 功耗与电量关联展示 ──
            AddCheck(hDlg, IDC_SHOWTREND, 14, y, 190, L"悬停显示功耗趋势图");
            AddLabel(hDlg, IDC_STATIC, 210, y, 76, L"窗口(分钟):");
            HWND hTrendMin = AddEdit(hDlg, IDC_TRENDMIN, 284, y - 4, 48, L"");
            y += 22;
            AddCheck(hDlg, IDC_SHOWCOST, 14, y, 190, L"显示累计电费");
            y += 26;

            g_state.hStatus = AddLabel(hDlg, IDC_STATUS, 12, y, 320, L"");

            // 填充现有值（一律取编辑副本里的真实配置）
            DeviceConfig* dev = PickDevice(c);
            if (dev) {
                SetText(hIp, WidenS(dev->ip));
                SetText(hName, WidenS(dev->name));
                // 展示真实值：之前这里硬编码 "4.2,4.1,4.3,4.4,2.1"，
                // 配置为空时界面显示它、点确定又把它写回 ini ——
                // 界面显示与实际配置不一致，还悄悄改变了默认配置。
                // 留空表示走自动发现，这正是空 propertyMap 的语义。
                SetText(hMap, WidenS(dev->propertyMap));
                std::wstring tp = g_state.store->TokenPath(*dev);
                std::string tk;
                if (TokenProvider::LoadFromFile(tp, tk))
                    SetText(hToken, WidenS(tk));
            }
            {
                std::wostringstream oss;
                oss << c.sampleIntervalSec;
                SetText(hInt, oss.str());
                oss.str(L""); oss << c.decimalPlaces;
                SetText(hDec, oss.str());
                oss.str(L""); oss << c.pricePerKwh;
                SetText(hPrice, oss.str());
                oss.str(L""); oss << c.trendMinutes;
                SetText(hTrendMin, oss.str());
            }
            SetCheck(GetDlgItem(hDlg, IDC_SHOWLABEL), c.showLabel);
            SetCheck(GetDlgItem(hDlg, IDC_SHOWUNIT), c.showUnit);
            SetCheck(GetDlgItem(hDlg, IDC_SHOWDEVNAME), c.showDeviceName);
            SetCheck(GetDlgItem(hDlg, IDC_SHOWENERGY), c.showEnergyInTooltip);
            SetCheck(GetDlgItem(hDlg, IDC_SHOWSTATS), c.showStatsInTooltip);
            SetCheck(GetDlgItem(hDlg, IDC_HISTORY), c.enableHistory);
            SetCheck(GetDlgItem(hDlg, IDC_SHOWTREND), c.showTrend);
            SetCheck(GetDlgItem(hDlg, IDC_SHOWCOST), c.showCost);

            y += 24;
            AddButton(hDlg, IDC_TEST, 12, y, 76, 26, L"测试连接");
            AddButton(hDlg, IDC_OPENTOKEN, 94, y, 108, 26, L"打开 token 目录");
            AddButton(hDlg, IDC_OK, 210, y, 60, 26, L"确定");
            AddButton(hDlg, IDC_CANCEL, 276, y, 60, 26, L"取消");

            SetFocus(hIp);
            return TRUE;
        }

        case WM_APP + 100: {
            std::wstring* msg = reinterpret_cast<std::wstring*>(lParam);
            if (msg && g_state.hStatus)
                SetText(g_state.hStatus, *msg);
            delete msg;
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            switch (id) {
                case IDC_TEST: {
                    ReadControls(g_state);       // 只改编辑副本，不落盘、不改宿主配置
                    TestCtx* ctx = new TestCtx();
                    ctx->cfg = g_state.edit;
                    ctx->store = *g_state.store;
                    ctx->hDlg = hDlg;
                    // 测试连接必须用输入框里的 token / IP / 属性映射，
                    // 否则测的是旧配置，按钮形同虚设。
                    ctx->token = NarrowS(g_state.editToken);
                    while (!ctx->token.empty() && (ctx->token.back() == '\r' ||
                           ctx->token.back() == '\n' || ctx->token.back() == ' '))
                        ctx->token.pop_back();
                    if (ctx->token.empty()) {
                        DeviceConfig* dev = PickDevice(g_state.edit);
                        if (dev) {
                            std::string tk;
                            if (TokenProvider::LoadFromFile(g_state.store->TokenPath(*dev), tk))
                                ctx->token = tk;
                        }
                    }
                    SetText(g_state.hStatus, L"正在测试连接...");
                    HANDLE th = CreateThread(nullptr, 0, TestThread, ctx, 0, nullptr);
                    if (th) CloseHandle(th);
                    return 0;
                }
                case IDC_OPENTOKEN: {
                    wchar_t path[MAX_PATH] = { 0 };
                    if (!g_state.edit.devices.empty()) {
                        const DeviceConfig& d = g_state.edit.devices[0];
                        std::wstring tp = g_state.store->TokenPath(d);
                        size_t pos = tp.find_last_of(L"\\");
                        if (pos != std::wstring::npos)
                            wcscpy_s(path, tp.substr(0, pos).c_str());
                    }
                    if (path[0]) {
                        SHELLEXECUTEINFOW sei{ sizeof(sei) };
                        sei.fMask = SEE_MASK_FLAG_NO_UI;
                        sei.lpVerb = L"open";
                        sei.lpFile = path;
                        ShellExecuteExW(&sei);
                    }
                    return 0;
                }
                case IDC_OK:
                case IDC_APPLY: {
                    ReadControls(g_state);
                    CommitToken(g_state);
                    // 写回宿主配置必须与显示线程共用同一把锁，
                    // 否则 UI 线程改 m_cfg 时显示线程正持锁读它（数据竞争）。
                    // 同时取一份局部副本用于落盘，Save 不再无锁读共享对象。
                    AppConfig committed;
                    if (g_state.plugin) {
                        std::lock_guard<std::mutex> lk(g_state.plugin->m_mutex);
                        *g_state.cfg = g_state.edit;
                        committed = *g_state.cfg;
                    } else {
                        *g_state.cfg = g_state.edit;
                        committed = *g_state.cfg;
                    }
                    g_state.store->Save(committed);
                    g_state.changed = true;
                    if (id == IDC_OK)
                        EndDialog(hDlg, IDOK);
                    return 0;
                }
                case IDC_CANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return 0;
            }
            return 0;
        }

        case WM_CLOSE:
            EndDialog(hDlg, IDCANCEL);
            return 0;
    }
    return FALSE;
}

// ── 构造内存对话框模板 ──────────────────────────────────────
// 模板只声明窗口样式与标题，所有控件在 WM_INITDIALOG 里动态创建。
//
// 注意: Windows SDK 里只有 DLGTEMPLATE 一个结构体（Unicode 版标题字段是
//       WCHAR），并不存在 DLGTEMPLATEW 类型 —— 只有 LPDLGTEMPLATEW 这样的别名。
//       因此这里按 DLGTEMPLATE 布局手工拼字节，且必须保证 DWORD 对齐。
std::vector<BYTE> BuildTemplate(int pxPerDlu) {
    const wchar_t* title = L"MiPower 功耗插件 - 设置";
    size_t titleChars = wcslen(title) + 1;          // 含结尾 NUL
    size_t titleBytes = titleChars * sizeof(wchar_t);

    // 布局: DLGTEMPLATE(18B) | menu(WORD) | class(WORD) | title(WCHAR[]) | font(WORD)
    // 起始偏移 0 对齐良好（vector<BYTE> 的 data() 由 new 分配，16 字节对齐）
    size_t headerBytes = sizeof(DLGTEMPLATE) + sizeof(WORD) * 2;
    size_t total = headerBytes + titleBytes + sizeof(WORD);
    std::vector<BYTE> buf(total, 0);
    BYTE* p = buf.data();

    DLGTEMPLATE* dt = reinterpret_cast<DLGTEMPLATE*>(p);
    dt->style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | DS_FIXEDSYS |
                WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt->dwExtendedStyle = 0;
    dt->cdit = 0;                 // 无模板控件
    dt->x = 0;
    dt->y = 0;
    // DLU -> 像素: 横向 1 DLU ≈ 4px, 纵向 1 DLU ≈ 8px（8pt 系统字体）
    // 反推: 像素 / 每 DLU 像素数
    // 像素 -> DLU（用实测比例），再留一点余量给边框
    dt->cx = static_cast<short>(kDesignWidthPx / pxPerDlu + 6);
    dt->cy = static_cast<short>(kDesignHeightPx / pxPerDlu + 4);

    WORD* w = reinterpret_cast<WORD*>(p + sizeof(DLGTEMPLATE));
    *w++ = 0;                    // 无菜单
    *w++ = 0;                    // 默认窗口类
    memcpy(w, title, titleBytes); // 标题（宽字符，含 NUL）
    w += titleChars;
    *w++ = 9;                    // 字号 9pt

    return buf;
}

} // namespace

bool OptionsDlg::Show(HWND hParent, AppConfig& cfg, ConfigStore& store,
                      CMiPowerPlugin* plugin) {
    g_state.cfg = &cfg;
    g_state.store = &store;
    g_state.plugin = plugin;
    g_state.changed = false;
    g_state.hDlg = nullptr;
    g_state.hStatus = nullptr;
    g_state.edit = cfg;             // 展示与编辑都基于当前真实配置
    g_state.editToken.clear();

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    DluScale scale = MeasureDluScale();
    std::vector<BYTE> tmpl = BuildTemplate(scale.pxPerDlu);

    // DialogBoxIndirectParam —— 只有它创建的对话框才能被 EndDialog 关闭
    INT_PTR ret = DialogBoxIndirectParamW(DllInstance(),
                                           reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()),
                                           hParent, DlgProc, 0);
    (void)ret;
    // 只要提交过（确定/应用）就算有改动；取消时 changed 恒为 false。
    // 不再看 ret：若「应用」后再取消，配置与文件都已改，也应通知宿主刷新。
    return g_state.changed;
}

} // namespace mp
