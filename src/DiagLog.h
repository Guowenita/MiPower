// DiagLog.h - 诊断日志（独立编译，供各模块共用）
#pragma once

// 写文件日志，用于定位宿主内的问题（stdout 不可用）
void DiagLogEx(const char* fmt, ...);
