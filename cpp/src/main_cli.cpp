// 命令行版本（便于批量脚本调用）：
//   VR180SBS_cli.exe --input D:\in --output D:\out --rot 0.6
// 不传参数时处理当前目录。

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

#include "core.h"

namespace {

std::atomic<bool>* g_cancel = nullptr;

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (g_cancel) g_cancel->store(true);
        return TRUE;
    }
    return FALSE;
}

void ConsoleOut(const std::wstring& text) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        DWORD written = 0;
        WriteConsoleW(h, text.c_str(), (DWORD)text.size(), &written, nullptr);
        return;
    }
    const int need =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    std::string utf8((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], need, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

void Line(const std::wstring& text) { ConsoleOut(text + L"\r\n"); }

void Usage() {
    Line(L"VR180 SBS 命令行版");
    Line(L"用法: VR180SBS_cli.exe [选项]");
    Line(L"  -i, --input <目录>    输入目录（默认当前目录）");
    Line(L"  -o, --output <目录>   输出目录（默认与输入相同）");
    Line(L"      --roll <角度>     右眼 Roll 校准角，默认 0");
    Line(L"      --pitch <角度>    右眼 Pitch 校准角，默认 0");
    Line(L"      --yaw <角度>      右眼 Yaw 校准角，默认 0");
    Line(L"      --zoom <系数>     右眼缩放，1=不变，>1 放大，<1 缩小（默认 1）");
    Line(L"      --posx <比例>     右眼 Position X，1=画面宽，+向右（默认 0）");
    Line(L"      --posy <比例>     右眼 Position Y，1=画面高，+向上（默认 0）");
    Line(L"      --fov <角度>      鱼眼 FOV，默认 208");
    Line(L"      --quality <1-100> JPEG 质量，默认 95");
    Line(L"      --threads <数量>  线程数，默认自动");
    Line(L"      --i420            输入为 I420（默认 NV12）");
    Line(L"  -h, --help            显示帮助");
}

std::wstring GetCurrentDir() {
    std::wstring buf(MAX_PATH, L'\0');
    DWORD n = GetCurrentDirectoryW((DWORD)buf.size(), &buf[0]);
    if (n == 0) return L".";
    if (n > buf.size()) {
        buf.resize(n);
        GetCurrentDirectoryW(n, &buf[0]);
    }
    buf.resize(n);
    return buf;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    vr180::Options opt;
    opt.inputDir = GetCurrentDir();
    opt.outputDir = opt.inputDir;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& dst) -> bool {
            if (i + 1 >= argc) {
                Line(L"缺少参数值: " + a);
                return false;
            }
            dst = argv[++i];
            return true;
        };
        std::wstring value;
        if (a == L"-i" || a == L"--input") {
            if (!next(value)) return 2;
            opt.inputDir = value;
        } else if (a == L"-o" || a == L"--output") {
            if (!next(value)) return 2;
            opt.outputDir = value;
        } else if (a == L"--roll") {
            if (!next(value)) return 2;
            opt.rollDeg = _wtof(value.c_str());
        } else if (a == L"--pitch") {
            if (!next(value)) return 2;
            opt.pitchDeg = _wtof(value.c_str());
        } else if (a == L"--yaw") {
            if (!next(value)) return 2;
            opt.yawDeg = _wtof(value.c_str());
        } else if (a == L"--zoom" || a == L"--scale") {
            if (!next(value)) return 2;
            opt.zoom = _wtof(value.c_str());
        } else if (a == L"--posx") {
            if (!next(value)) return 2;
            opt.positionX = _wtof(value.c_str());
        } else if (a == L"--posy") {
            if (!next(value)) return 2;
            opt.positionY = _wtof(value.c_str());
        } else if (a == L"--fov") {
            if (!next(value)) return 2;
            opt.fovDeg = _wtof(value.c_str());
        } else if (a == L"--quality") {
            if (!next(value)) return 2;
            opt.jpegQuality = _wtoi(value.c_str());
            if (opt.jpegQuality < 1) opt.jpegQuality = 1;
            if (opt.jpegQuality > 100) opt.jpegQuality = 100;
        } else if (a == L"--threads") {
            if (!next(value)) return 2;
            opt.threads = _wtoi(value.c_str());
        } else if (a == L"--i420") {
            opt.i420 = true;
        } else if (a == L"--nv12") {
            opt.i420 = false;
        } else if (a == L"-h" || a == L"--help" || a == L"/?") {
            Usage();
            return 0;
        } else {
            Line(L"未知参数: " + a);
            Usage();
            return 2;
        }
    }

    std::atomic<bool> cancel{false};
    g_cancel = &cancel;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    vr180::LogSink sink;
    sink.log = [](const std::wstring& s) { Line(L"  " + s); };
    int lastBucket = -1;
    sink.progress = [&](int percent) {
        const int bucket = percent / 5;
        if (bucket != lastBucket) {
            lastBucket = bucket;
            Line(vr180::WFormat(L"进度 %d%%", percent));
        }
    };

    const auto t0 = std::chrono::steady_clock::now();
    std::wstring error;
    const bool ok = vr180::Run(opt, sink, cancel, error);
    const auto sec =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count() /
        1000.0;

    if (!ok && !error.empty()) Line(L"错误: " + error);
    Line(vr180::WFormat(L"总用时 %.1f 秒", sec));
    return ok ? 0 : 1;
}
