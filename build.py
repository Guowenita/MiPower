#!/usr/bin/env python3
"""MiPower 构建脚本 —— 直接调用 MSVC cl.exe 编译插件 DLL。

用法：
    python build.py release      # Release（默认）
    python build.py debug        # Debug
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
OUT = os.path.join(ROOT, "build")

VS_ROOTS = [
    r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools",
    r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community",
    r"C:\Program Files\Microsoft Visual Studio\2022\Professional",
    r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
]
SDK_ROOT = r"C:\Program Files (x86)\Windows Kits\10"

# 插件本体的全部编译单元（不含任何测试/开发工具）
SOURCES = [
    "Crypto.cpp", "DiagLog.cpp", "MiioClient.cpp", "MiotSpec.cpp",
    "Config.cpp", "Aggregator.cpp", "Collector.cpp",
    "MiPowerPlugin.cpp", "OptionsDlg.cpp",
]


def find_msvc():
    """定位已安装的 MSVC 工具集，返回 (VS 根目录, 版本号)。"""
    for r in VS_ROOTS:
        m = os.path.join(r, "VC", "Tools", "MSVC")
        if os.path.isdir(m):
            vers = sorted(os.listdir(m))
            if vers:
                return r, vers[-1]
    return None, None


def find_sdk():
    """定位可用的 Windows SDK 版本。"""
    inc = os.path.join(SDK_ROOT, "Include")
    if not os.path.isdir(inc):
        return None
    for v in reversed(sorted(os.listdir(inc))):
        if os.path.isdir(os.path.join(SDK_ROOT, "Lib", v)):
            return v
    return None


def build(cfg="release"):
    """编译 MiPower.dll 到 build/ 目录。"""
    vs, msvc = find_msvc()
    sdk = find_sdk()
    if not vs or not sdk:
        print("[ERROR] 未找到 MSVC 或 Windows SDK")
        return 1

    vs_tools = os.path.join(vs, "VC", "Tools", "MSVC", msvc)
    cl = os.path.join(vs_tools, "bin", "Hostx64", "x64", "cl.exe")
    sdk_inc = os.path.join(SDK_ROOT, "Include", sdk)
    sdk_lib = os.path.join(SDK_ROOT, "Lib", sdk)
    print(f"[INFO] MSVC {msvc} / SDK {sdk}")
    os.makedirs(OUT, exist_ok=True)

    env = dict(os.environ)
    env["INCLUDE"] = os.pathsep.join([
        os.path.join(vs_tools, "include"),
        os.path.join(sdk_inc, "ucrt"), os.path.join(sdk_inc, "shared"),
        os.path.join(sdk_inc, "um"), os.path.join(sdk_inc, "winrt"),
    ])
    env["LIB"] = os.pathsep.join([
        os.path.join(vs_tools, "lib", "x64"),
        os.path.join(sdk_lib, "ucrt", "x64"),
        os.path.join(sdk_lib, "um", "x64"),
    ])
    env["PATH"] = os.path.dirname(cl) + os.pathsep + env.get("PATH", "")

    common = ["/nologo", "/std:c++17", "/W3", "/EHsc", "/utf-8",
              "/DUNICODE", "/D_UNICODE", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
              "/D_CRT_SECURE_NO_WARNINGS"]
    if cfg == "debug":
        common += ["/Od", "/Zi", "/MTd", "/D_DEBUG"]
    else:
        common += ["/O2", "/MT", "/DNDEBUG"]

    args = (common + ["/LD", f"/I{SRC}"]
            + [os.path.join(SRC, s) for s in SOURCES]
            + [f"/Fe:{os.path.join(OUT, 'MiPower.dll')}", "/link", "/nologo",
               "ws2_32.lib", "comctl32.lib", "shell32.lib", "user32.lib",
               "gdi32.lib", "advapi32.lib"])
    p = subprocess.run([cl] + args, cwd=ROOT, env=env,
                       capture_output=True, text=True, errors="replace")
    out = (p.stdout or "") + (p.stderr or "")
    for line in out.splitlines():
        low = line.lower()
        if "error" in low or "warning" in low or ".cpp" in line:
            print("   " + line)
    if p.returncode != 0:
        print("[ERROR] MiPower.dll 编译失败")
        return p.returncode
    path = os.path.join(OUT, "MiPower.dll")
    print(f"[OK] 构建完成: {path} ({os.path.getsize(path) / 1024:.1f} KB)")
    return 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    sys.exit(build(args[0] if args else "release"))
