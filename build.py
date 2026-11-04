#!/usr/bin/env python3
"""MiPower 构建脚本 —— 直接调用 cl.exe，绕过 shell 脚本执行策略"""
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


def find_msvc():
    for r in VS_ROOTS:
        m = os.path.join(r, "VC", "Tools", "MSVC")
        if os.path.isdir(m):
            vers = sorted(os.listdir(m))
            if vers:
                return r, vers[-1]
    return None, None


def find_sdk():
    inc = os.path.join(SDK_ROOT, "Include")
    if not os.path.isdir(inc):
        return None
    vers = sorted(os.listdir(inc))
    for v in reversed(vers):
        if os.path.isdir(os.path.join(SDK_ROOT, "Lib", v)):
            return v
    return None


def build(cfg="release", quick=False):
    vs, msvc = find_msvc()
    if not vs:
        print("[ERROR] 未找到 MSVC")
        return 1
    sdk = find_sdk()
    if not sdk:
        print("[ERROR] 未找到 Windows SDK")
        return 1

    vs_tools = os.path.join(vs, "VC", "Tools", "MSVC", msvc)
    cl = os.path.join(vs_tools, "bin", "Hostx64", "x64", "cl.exe")
    sdk_inc = os.path.join(SDK_ROOT, "Include", sdk)
    sdk_lib = os.path.join(SDK_ROOT, "Lib", sdk)

    print(f"[INFO] MSVC {msvc}")
    print(f"[INFO] SDK  {sdk}")

    os.makedirs(OUT, exist_ok=True)

    env = dict(os.environ)
    env["INCLUDE"] = os.pathsep.join([
        os.path.join(vs_tools, "include"),
        os.path.join(sdk_inc, "ucrt"),
        os.path.join(sdk_inc, "shared"),
        os.path.join(sdk_inc, "um"),
        os.path.join(sdk_inc, "winrt"),
    ])
    env["LIB"] = os.pathsep.join([
        os.path.join(vs_tools, "lib", "x64"),
        os.path.join(sdk_lib, "ucrt", "x64"),
        os.path.join(sdk_lib, "um", "x64"),
    ])
    env["PATH"] = os.path.dirname(cl) + os.pathsep + env.get("PATH", "")

    common = [
        "/nologo", "/std:c++17", "/W3", "/EHsc", "/utf-8",
        "/DUNICODE", "/D_UNICODE", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
        "/D_CRT_SECURE_NO_WARNINGS",
    ]
    if cfg == "debug":
        common += ["/Od", "/Zi", "/MTd", "/D_DEBUG"]
    else:
        common += ["/O2", "/MT", "/DNDEBUG"]

    def run(args, title):
        print(f"\n[INFO] {title}")
        print("       " + " ".join(args[:6]) + " ...")
        p = subprocess.run([cl] + args, cwd=ROOT, env=env,
                           capture_output=True, text=True, errors="replace")
        out = (p.stdout or "") + (p.stderr or "")
        # 只打印错误和警告，隐藏路径噪音
        for line in out.splitlines():
            low = line.lower()
            if "error" in low or "warning" in low or line.startswith("MiPower") or ".cpp" in line:
                print("   " + line)
        return p.returncode

    sources = ["Crypto.cpp", "DiagLog.cpp", "MiioClient.cpp", "MiotSpec.cpp", "Config.cpp",
               "Aggregator.cpp", "Collector.cpp", "MiPowerPlugin.cpp", "OptionsDlg.cpp"]

    if quick:
        rc = run(common + ["/LD", f"/I{SRC}"] +
                 [os.path.join(SRC, s) for s in sources] +
                 [f"/Fe:{os.path.join(OUT, 'MiPower.dll')}", "/link", "/nologo",
                  "ws2_32.lib", "comctl32.lib", "shell32.lib", "user32.lib",
                  "gdi32.lib", "advapi32.lib"],
                 "编译 MiPower.dll (quick)")
        if rc != 0:
            print("[ERROR] 编译失败")
            return rc
        print("\n[OK] 构建完成 (quick): MiPower.dll")
        return 0

    # 1) selftest
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "selftest.cpp"),
        os.path.join(SRC, "Crypto.cpp"),
        os.path.join(SRC, "DiagLog.cpp"),
        os.path.join(SRC, "MiioClient.cpp"),
        f"/Fe:{os.path.join(OUT, 'selftest.exe')}",
    ], "编译 selftest.exe")
    if rc != 0:
        print("[ERROR] selftest 编译失败")
        return rc

    # 1.5) 设备联调测试
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "devicetest.cpp"),
        os.path.join(SRC, "Crypto.cpp"),
        os.path.join(SRC, "DiagLog.cpp"),
        os.path.join(SRC, "MiioClient.cpp"),
        os.path.join(SRC, "MiotSpec.cpp"),
        f"/Fe:{os.path.join(OUT, 'devicetest.exe')}",
    ], "编译 devicetest.exe")
    if rc != 0:
        print("[ERROR] devicetest 编译失败")
        return rc

    # 1.6) 宿主模拟测试
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "hosttest.cpp"),
        f"/Fe:{os.path.join(OUT, 'hosttest.exe')}",
    ], "编译 hosttest.exe")
    if rc != 0:
        print("[ERROR] hosttest 编译失败")
        return rc

    # 1.7) 宿主模拟测试 (debug 版，带崩溃检测)
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "hosttest.cpp"),
        "/Zi", "/MDd", "/Od",
        f"/Fe:{os.path.join(OUT, 'hosttest_dbg.exe')}",
        "/link", "/DEBUG",
    ], "编译 hosttest_dbg.exe")
    if rc != 0:
        print("[ERROR] hosttest_dbg 编译失败")
        return rc

    # 1.7b) 自研客户端直测
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "myclienttest.cpp"),
        os.path.join(SRC, "Crypto.cpp"),
        os.path.join(SRC, "DiagLog.cpp"),
        os.path.join(SRC, "MiioClient.cpp"),
        f"/Fe:{os.path.join(OUT, 'myclienttest.exe')}",
    ], "编译 myclienttest.exe")
    if rc != 0:
        print("[ERROR] myclienttest 编译失败")
        return rc

    # 1.75) 功耗-电量关联采样器
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "energy_sampler.cpp"),
        os.path.join(SRC, "Crypto.cpp"),
        os.path.join(SRC, "DiagLog.cpp"),
        os.path.join(SRC, "MiioClient.cpp"),
        os.path.join(SRC, "MiotSpec.cpp"),
        os.path.join(SRC, "Config.cpp"),
        os.path.join(SRC, "Aggregator.cpp"),
        os.path.join(SRC, "Collector.cpp"),
        f"/Fe:{os.path.join(OUT, 'energy_sampler.exe')}",
    ], "编译 energy_sampler.exe")
    if rc != 0:
        print("[ERROR] energy_sampler 编译失败")
        return rc

    # 1.76) 设置对话框测试
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "dlgtest.cpp"),
        os.path.join(SRC, "Crypto.cpp"),
        os.path.join(SRC, "DiagLog.cpp"),
        os.path.join(SRC, "MiioClient.cpp"),
        os.path.join(SRC, "MiotSpec.cpp"),
        os.path.join(SRC, "Config.cpp"),
        os.path.join(SRC, "Aggregator.cpp"),
        os.path.join(SRC, "Collector.cpp"),
        os.path.join(SRC, "OptionsDlg.cpp"),
        f"/Fe:{os.path.join(OUT, 'dlgtest.exe')}",
        "/link", "user32.lib", "gdi32.lib", "comctl32.lib", "shell32.lib", "advapi32.lib",
    ], "编译 dlgtest.exe")
    if rc != 0:
        print("[ERROR] dlgtest 编译失败")
        return rc

    # 1.77) 设置框几何检查
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "dlggeom.cpp"),
        os.path.join(SRC, "Crypto.cpp"),
        os.path.join(SRC, "DiagLog.cpp"),
        os.path.join(SRC, "MiioClient.cpp"),
        os.path.join(SRC, "MiotSpec.cpp"),
        os.path.join(SRC, "Config.cpp"),
        os.path.join(SRC, "Aggregator.cpp"),
        os.path.join(SRC, "Collector.cpp"),
        os.path.join(SRC, "OptionsDlg.cpp"),
        f"/Fe:{os.path.join(OUT, 'dlggeom.exe')}",
        "/link", "user32.lib", "gdi32.lib", "comctl32.lib", "shell32.lib", "advapi32.lib",
    ], "编译 dlggeom.exe")
    if rc != 0:
        print("[ERROR] dlggeom 编译失败")
        return rc

    # 1.77c) 配置链路验证（UI 参数 -> 保存 -> 运行态生效）
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "cfgapply_test.cpp"),
        os.path.join(SRC, "Crypto.cpp"),
        os.path.join(SRC, "DiagLog.cpp"),
        os.path.join(SRC, "MiioClient.cpp"),
        os.path.join(SRC, "MiotSpec.cpp"),
        os.path.join(SRC, "Config.cpp"),
        os.path.join(SRC, "Aggregator.cpp"),
        os.path.join(SRC, "Collector.cpp"),
        os.path.join(SRC, "OptionsDlg.cpp"),
        f"/Fe:{os.path.join(OUT, 'cfgapply_test.exe')}",
        "/link", "user32.lib", "gdi32.lib", "comctl32.lib", "shell32.lib",
        "advapi32.lib", "ws2_32.lib",
    ], "编译 cfgapply_test.exe")
    if rc != 0:
        print("[ERROR] cfgapply_test 编译失败")
        return rc

    # 1.79) DLU 换算实测
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "dlucalc.cpp"),
        f"/Fe:{os.path.join(OUT, 'dlucalc.exe')}",
        "/link", "user32.lib",
    ], "编译 dlucalc.exe")
    if rc != 0:
        print("[ERROR] dlucalc 编译失败")
        return rc

    # 1.78) 趋势图验证
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "trendtest.cpp"),
        os.path.join(SRC, "Crypto.cpp"),
        os.path.join(SRC, "DiagLog.cpp"),
        os.path.join(SRC, "MiioClient.cpp"),
        os.path.join(SRC, "MiotSpec.cpp"),
        os.path.join(SRC, "Config.cpp"),
        os.path.join(SRC, "Aggregator.cpp"),
        os.path.join(SRC, "Collector.cpp"),
        f"/Fe:{os.path.join(OUT, 'trendtest.exe')}",
    ], "编译 trendtest.exe")
    if rc != 0:
        print("[ERROR] trendtest 编译失败")
        return rc

    # 1.8) TM 显示项链链��模拟
    rc = run(common + [
        f"/I{SRC}",
        os.path.join(SRC, "hosttest2.cpp"),
        f"/Fe:{os.path.join(OUT, 'hosttest2.exe')}",
    ], "编译 hosttest2.exe")
    if rc != 0:
        print("[ERROR] hosttest2 编译失败")
        return rc

    # 2) 插件 DLL
    rc = run(common + ["/LD", f"/I{SRC}"] +
             [os.path.join(SRC, s) for s in sources] +
             [f"/Fe:{os.path.join(OUT, 'MiPower.dll')}", "/link", "/nologo",
              "ws2_32.lib", "comctl32.lib", "shell32.lib", "user32.lib",
              "gdi32.lib", "advapi32.lib"],
             "编译 MiPower.dll")
    if rc != 0:
        print("[ERROR] MiPower.dll 编译失败")
        return rc

    print("\n[OK] 构建完成")
    for f in sorted(os.listdir(OUT)):
        if f.endswith((".dll", ".exe")):
            sz = os.path.getsize(os.path.join(OUT, f)) / 1024
            print(f"     {f}  {sz:.1f} KB")
    return 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    quick = "--quick" in sys.argv
    sys.exit(build(args[0] if args else "release", quick))
