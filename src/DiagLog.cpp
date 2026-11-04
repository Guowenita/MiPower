// DiagLog.cpp - 诊断日志实现
#include "DiagLog.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>

namespace {

// 日志文件大小上限，超过后截断重写，避免无限增长占满磁盘
constexpr DWORD kMaxLogBytes = 512 * 1024;

HANDLE OpenLogFile() {
    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path))
        return INVALID_HANDLE_VALUE;
    wcscat_s(path, L"MiPower_diag.log");

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    // 超限则清空文件重新开始
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > (LONGLONG)kMaxLogBytes) {
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(h);
    }
    return h;
}

} // namespace

void DiagLogEx(const char* fmt, ...) {
    static HANDLE h = INVALID_HANDLE_VALUE;
    static bool tried = false;
    if (!tried) {
        tried = true;
        h = OpenLogFile();
    }
    if (h == INVALID_HANDLE_VALUE) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(h, buf, (DWORD)n, &written, nullptr);
        WriteFile(h, "\r\n", 2, &written, nullptr);
    }
}
