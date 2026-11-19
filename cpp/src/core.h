#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace vr180 {

// 处理参数（界面 / 命令行共用）
struct Options {
    std::wstring inputDir;       // 输入目录，扫描 *.yuv
    std::wstring outputDir;      // 输出目录，保存 *_VR180_SBS.jpg
    double rollDeg = 0.0;        // 右眼 Roll 校准角（度），绕 Z 轴，右手定则
    double pitchDeg = 0.0;       // 右眼 Pitch 校准角（度），绕 X 轴，右手定则
    double yawDeg = 0.0;         // 右眼 Yaw 校准角（度），绕 Y 轴，右手定则
    double zoom = 1.0;           // 右眼缩放，1 = 不变，>1 放大，<1 缩小
    double positionX = 0.0;      // 右眼 X 偏移比例（1=画面宽），+X 向右（同 Resolve）
    double positionY = 0.0;      // 右眼 Y 偏移比例（1=画面高），+Y 向上（同 Resolve）
    double fovDeg = 208.0;       // 鱼眼 FOV（全视场角），默认 208
    bool i420 = false;           // false = NV12（默认），true = I420
    int jpegQuality = 95;        // JPEG 质量
    int threads = 0;             // 0 = 自动
};

struct LogSink {
    std::function<void(const std::wstring&)> log;  // 可跨线程调用
    std::function<void(int)> progress;             // 0..100，可跨线程调用
};

// 批量处理：扫描 inputDir 里的 frontFishEye/backFishEye 配对，
// 忽略 preFishEye 文件，输出左右眼互换后的 VR180 SBS JPEG。
// 返回 true 表示至少成功处理了一对；error 非空时给出总体错误。
bool Run(const Options& opt, const LogSink& sink, std::atomic<bool>& cancel,
         std::wstring& error);

// 检查输出目录是否存在、可写；失败时 error 给出带系统错误原因和修复建议的中文说明。
// GUI 在开始前调用它，可以在长时间转换之前就发现问题。
bool CheckOutputDir(const std::wstring& dir, std::wstring& error);

std::wstring WFormat(const wchar_t* fmt, ...);

}  // namespace vr180
