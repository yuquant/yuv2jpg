// VR180 SBS 转换核心：YUV420 双鱼眼 -> VR180 等距柱状 SBS JPEG
//
// 与 yuv_vr180.py 的差异（按需求调整）：
//   1. 扫描时忽略 preFishEye 文件；
//   2. 输出左右眼互换：backFishEye 作为左眼，frontFishEye 作为右眼；
//   3. 右眼（frontFishEye）用 Roll/Pitch/Yaw/Zoom/Position 做校准；
//   4. 不再落盘 .npz LUT，映射表按行即时计算（内存占用小，速度更快）。

#include "core.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace vr180 {
namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- 基础工具

std::wstring ToLower(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    }
    return s;
}

std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return std::wstring();
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int need = MultiByteToWideChar(cp, flags, s, -1, nullptr, 0);
    if (need <= 0) {
        cp = CP_ACP;
        flags = 0;
        need = MultiByteToWideChar(cp, flags, s, -1, nullptr, 0);
    }
    if (need <= 0) return std::wstring();
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(cp, flags, s, -1, out.data(), need);
    out.resize((size_t)need - 1);
    return out;
}

std::wstring FileNameOf(const std::wstring& path) {
    return std::filesystem::path(path).filename().wstring();
}

bool FileSizeOf(const std::wstring& path, uint64_t& size) {
    std::error_code ec;
    auto v = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) return false;
    size = (uint64_t)v;
    return true;
}

// ---------------------------------------------------------------- 权限诊断

// 把 Win32 错误码变成中文系统提示（去掉结尾换行）。
std::wstring Win32ErrorText(DWORD code) {
    if (code == 0) return L"未知错误";
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                   (LPWSTR)&buf, 0, nullptr);
    std::wstring s;
    if (n != 0 && buf != nullptr) s.assign(buf, n);
    if (buf != nullptr) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    if (s.empty()) s = WFormat(L"错误码 %lu", (unsigned long)code);
    return s;
}

// 当前进程的完整性级别（Medium = 0x2000；低于它说明被沙箱/低完整性标签限制）。
DWORD ProcessIntegrityRid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;
    DWORD need = 0;
    DWORD rid = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &need);
    if (need > 0) {
        std::vector<BYTE> buf(need);
        if (GetTokenInformation(token, TokenIntegrityLevel, buf.data(), need, &need)) {
            auto* label = (TOKEN_MANDATORY_LABEL*)buf.data();
            PSID sid = label->Label.Sid;
            const UINT8 count = *GetSidSubAuthorityCount(sid);
            if (count > 0) rid = *GetSidSubAuthority(sid, count - 1);
        }
    }
    CloseHandle(token);
    return rid;
}

bool IsLowIntegrity() {
    const DWORD rid = ProcessIntegrityRid();
    return rid != 0 && rid < SECURITY_MANDATORY_MEDIUM_RID;
}

// 低完整性进程写不了普通文件夹——这是“改名/复制/沙箱”常见的坑，给出可直接照做的办法。
std::wstring LowIntegrityHint() {
    return L"程序当前以“低完整性（沙箱）”级别运行，无法写入普通文件夹。"
           L"解决办法：把 VR180SBS.exe 复制到普通文件夹（例如 D:\\tools）后再运行，"
           L"或对本程序所在文件夹执行 icacls \"<文件夹>\" /setintegritylevel (OI)(CI)Medium /T";
}

uint64_t Isqrt64(uint64_t n) {
    if (n == 0) return 0;
    uint64_t r = (uint64_t)std::sqrt((double)n);
    while ((r + 1) <= n / (r + 1)) ++r;
    while (r > n / r) --r;
    return r;
}

// ---------------------------------------------------------------- 图像容器

struct ImageRGB {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;

    void Alloc(int width, int height) {
        w = width;
        h = height;
        px.assign((size_t)width * (size_t)height * 3, 0);
    }
    void Release() {
        w = h = 0;
        std::vector<uint8_t>().swap(px);
    }
    uint8_t* Row(int y) { return px.data() + (size_t)y * (size_t)w * 3; }
    const uint8_t* Row(int y) const { return px.data() + (size_t)y * (size_t)w * 3; }
};

inline uint8_t Clamp8(float v) {
    int i = (int)(v + 0.5f);
    if (i < 0) return 0;
    if (i > 255) return 255;
    return (uint8_t)i;
}

// ---------------------------------------------------------------- 分辨率推断

bool GuessResolution(uint64_t size, int& W, int& H, int& frames) {
    W = H = 0;
    frames = 0;
    if (size == 0) return false;

    uint64_t wh = size * 2 / 3;
    uint64_t side = Isqrt64(wh);
    if (side > 0 && side * side == wh && (side % 2) == 0 && side <= 0x7fffffffULL) {
        uint64_t frame = side * side * 3 / 2;
        if (frame > 0 && size % frame == 0 && size / frame <= 0x7fffffffULL) {
            W = (int)side;
            H = (int)side;
            frames = (int)(size / frame);
            return true;
        }
    }

    static const int kCommon[][2] = {
        {3840, 2160}, {1920, 1080}, {1280, 720},  {1024, 768},
        {1280, 960},  {640, 480},   {800, 600},   {2560, 1440},
        {3840, 3840}, {1024, 1024}, {1280, 1280}, {2048, 2048},
        {2160, 2160}, {2880, 2880}, {4096, 4096},
    };
    for (const auto& s : kCommon) {
        uint64_t frame = (uint64_t)s[0] * (uint64_t)s[1] * 3 / 2;
        if (frame > 0 && size % frame == 0 && size / frame <= 0x7fffffffULL) {
            W = s[0];
            H = s[1];
            frames = (int)(size / frame);
            return true;
        }
    }

    // 小文件暴力枚举（与 Python 版本保持一致的行为）
    if (size < 50ULL * 1024 * 1024) {
        uint64_t target = size * 2 / 3;
        uint64_t limit = Isqrt64(target) * 2;
        for (uint64_t w = 2; w < limit; w += 2) {
            if (target % w != 0) continue;
            uint64_t h = target / w;
            if ((h % 2) == 0 && w * h == target && w <= 0x7fffffffULL && h <= 0x7fffffffULL) {
                W = (int)w;
                H = (int)h;
                frames = 1;
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------- YUV 读取

bool LoadYuvFirstFrame(const std::wstring& path, int W, int H, bool i420, ImageRGB& out,
                       std::wstring& err) {
    const uint64_t frameSize = (uint64_t)W * (uint64_t)H * 3 / 2;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        err = L"无法打开文件: " + path;
        return false;
    }
    std::vector<uint8_t> buf((size_t)frameSize);
    size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got != buf.size()) {
        err = WFormat(L"文件不完整（需要 %llu 字节，实际 %llu 字节）: %ls",
                      (unsigned long long)frameSize, (unsigned long long)got, path.c_str());
        return false;
    }

    out.Alloc(W, H);
    const uint8_t* yp = buf.data();
    const uint8_t* uv = buf.data() + (size_t)W * (size_t)H;

    auto convertPixel = [](int Y, int U, int V, uint8_t* d) {
        const float yy = 1.164f * (float)(Y - 16);
        const float u = (float)(U - 128);
        const float v = (float)(V - 128);
        // 输出 RGB（stb_image_write 需要 RGB 顺序）
        d[0] = Clamp8(yy + 1.596f * v);                 // R
        d[1] = Clamp8(yy - 0.391f * u - 0.813f * v);    // G
        d[2] = Clamp8(yy + 2.018f * u);                 // B
    };

    if (!i420) {
        // NV12: UV 交错，2x2 共享一组 UV
        for (int y = 0; y < H; y += 2) {
            uint8_t* d0 = out.Row(y);
            uint8_t* d1 = out.Row(y + 1);
            const uint8_t* uvr = uv + (size_t)(y / 2) * (size_t)W;
            const uint8_t* yr0 = yp + (size_t)y * (size_t)W;
            const uint8_t* yr1 = yp + (size_t)(y + 1) * (size_t)W;
            for (int x = 0; x < W; x += 2) {
                const int U = (int)uvr[x];
                const int V = (int)uvr[x + 1];
                uint8_t* p00 = d0 + (size_t)x * 3;
                uint8_t* p10 = p00 + 3;
                uint8_t* p01 = d1 + (size_t)x * 3;
                uint8_t* p11 = p01 + 3;
                convertPixel(yr0[x], U, V, p00);
                convertPixel(yr0[x + 1], U, V, p10);
                convertPixel(yr1[x], U, V, p01);
                convertPixel(yr1[x + 1], U, V, p11);
            }
        }
    } else {
        // I420: Y 平面 + U 平面 + V 平面
        const int cw = W / 2;
        const uint8_t* up = uv;
        const uint8_t* vp = uv + (size_t)cw * (size_t)(H / 2);
        for (int y = 0; y < H; y += 2) {
            uint8_t* d0 = out.Row(y);
            uint8_t* d1 = out.Row(y + 1);
            const uint8_t* ur = up + (size_t)(y / 2) * (size_t)cw;
            const uint8_t* vr = vp + (size_t)(y / 2) * (size_t)cw;
            const uint8_t* yr0 = yp + (size_t)y * (size_t)W;
            const uint8_t* yr1 = yp + (size_t)(y + 1) * (size_t)W;
            for (int x = 0; x < W; x += 2) {
                const int U = (int)ur[x / 2];
                const int V = (int)vr[x / 2];
                convertPixel(yr0[x], U, V, d0 + (size_t)x * 3);
                convertPixel(yr0[x + 1], U, V, d0 + (size_t)x * 3 + 3);
                convertPixel(yr1[x], U, V, d1 + (size_t)x * 3);
                convertPixel(yr1[x + 1], U, V, d1 + (size_t)x * 3 + 3);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- 并行工具

template <typename Fn>
void ParallelRows(int rows, int threads, std::atomic<bool>& cancel, Fn fn) {
    if (rows <= 0) return;
    if (threads <= 1 || rows < 8) {
        for (int y = 0; y < rows; ++y) {
            if (cancel.load(std::memory_order_relaxed)) return;
            fn(y);
        }
        return;
    }
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    pool.reserve((size_t)threads);
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&]() {
            for (;;) {
                int y = next.fetch_add(1, std::memory_order_relaxed);
                if (y >= rows) break;
                if (cancel.load(std::memory_order_relaxed)) break;
                fn(y);
            }
        });
    }
    for (auto& th : pool) th.join();
}

// ---------------------------------------------------------------- 双线性采样

inline void SampleBilinear(const ImageRGB& src, float fx, float fy, uint8_t* out) {
    const int w = src.w;
    const int h = src.h;
    if (!(fx >= 0.f && fy >= 0.f && fx <= (float)(w - 1) && fy <= (float)(h - 1))) {
        // 边界外：按 BORDER_CONSTANT(0) 处理
        int x0 = (int)std::floor((double)fx);
        int y0 = (int)std::floor((double)fy);
        float ax = fx - (float)x0;
        float ay = fy - (float)y0;
        const uint8_t* p00 = (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) ? src.Row(y0) + (size_t)x0 * 3 : nullptr;
        const uint8_t* p10 = (x0 + 1 >= 0 && x0 + 1 < w && y0 >= 0 && y0 < h) ? src.Row(y0) + (size_t)(x0 + 1) * 3 : nullptr;
        const uint8_t* p01 = (x0 >= 0 && x0 < w && y0 + 1 >= 0 && y0 + 1 < h) ? src.Row(y0 + 1) + (size_t)x0 * 3 : nullptr;
        const uint8_t* p11 = (x0 + 1 >= 0 && x0 + 1 < w && y0 + 1 >= 0 && y0 + 1 < h) ? src.Row(y0 + 1) + (size_t)(x0 + 1) * 3 : nullptr;
        const float w00 = (1.f - ax) * (1.f - ay);
        const float w10 = ax * (1.f - ay);
        const float w01 = (1.f - ax) * ay;
        const float w11 = ax * ay;
        for (int c = 0; c < 3; ++c) {
            float v = 0.f;
            if (p00) v += (float)p00[c] * w00;
            if (p10) v += (float)p10[c] * w10;
            if (p01) v += (float)p01[c] * w01;
            if (p11) v += (float)p11[c] * w11;
            out[c] = Clamp8(v);
        }
        return;
    }

    int x0 = (int)fx;
    int y0 = (int)fy;
    const float ax = fx - (float)x0;
    const float ay = fy - (float)y0;
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const uint8_t* p00 = src.Row(y0) + (size_t)x0 * 3;
    const uint8_t* p10 = src.Row(y0) + (size_t)x1 * 3;
    const uint8_t* p01 = src.Row(y1) + (size_t)x0 * 3;
    const uint8_t* p11 = src.Row(y1) + (size_t)x1 * 3;
    const float w00 = (1.f - ax) * (1.f - ay);
    const float w10 = ax * (1.f - ay);
    const float w01 = (1.f - ax) * ay;
    const float w11 = ax * ay;
    for (int c = 0; c < 3; ++c) {
        out[c] = Clamp8((float)p00[c] * w00 + (float)p10[c] * w10 + (float)p01[c] * w01 +
                        (float)p11[c] * w11);
    }
}

// ---------------------------------------------------------------- 右眼姿态校准

// 相机坐标系：X 向右、Y 向上、Z 向前（光轴）。
// 旋转顺序 R = Rz(roll) * Rx(pitch) * Ry(yaw)，均为右手定则。
// 输出全景方向 d 对应的鱼眼相机方向为 R^T * d。
void BuildRotationMatrix(double rollDeg, double pitchDeg, double yawDeg, double R[3][3]) {
    const double r = rollDeg * kPi / 180.0;
    const double p = pitchDeg * kPi / 180.0;
    const double y = yawDeg * kPi / 180.0;
    const double cr = std::cos(r), sr = std::sin(r);
    const double cp = std::cos(p), sp = std::sin(p);
    const double cy = std::cos(y), sy = std::sin(y);

    // R = Rz(roll) * Rx(pitch) * Ry(yaw)
    R[0][0] = cr * cy - sr * sp * sy;
    R[0][1] = -sr * cp;
    R[0][2] = cr * sy + sr * sp * cy;
    R[1][0] = sr * cy + cr * sp * sy;
    R[1][1] = cr * cp;
    R[1][2] = sr * sy - cr * sp * cy;
    R[2][0] = -cp * sy;
    R[2][1] = sp;
    R[2][2] = cp * cy;
}

// ---------------------------------------------------------------- 鱼眼 -> VR180 映射表

struct EquirectMap {
    int srcW = 0;
    int srcH = 0;
    int outW = 0;
    int outH = 0;
    float cxf = 0.f;
    float cyf = 0.f;
    float scale = 0.f;
    bool rotated = false;
    float rot[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
    std::vector<float> sinLon, cosLon, sinLat, cosLat;

    void Init(int sw, int sh, int ow, int oh, double fovDeg,
              const double rotation[3][3] = nullptr, double zoom = 1.0) {
        srcW = sw;
        srcH = sh;
        outW = ow;
        outH = oh;
        cxf = (float)(sw / 2.0);
        cyf = (float)(sh / 2.0);
        const double radius = (double)std::min(sw, sh) / 2.0;
        const double fov = fovDeg * kPi / 180.0;
        scale = (float)(radius / (fov / 2.0) * zoom);

        rotated = (rotation != nullptr);
        if (rotated) {
            // 保存 R^T：把输出全景方向变换到鱼眼相机方向
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) rot[i][j] = (float)rotation[j][i];
            }
        }

        sinLon.resize((size_t)ow);
        cosLon.resize((size_t)ow);
        sinLat.resize((size_t)oh);
        cosLat.resize((size_t)oh);

        const int dw = std::max(ow - 1, 1);
        const int dh = std::max(oh - 1, 1);
        for (int x = 0; x < ow; ++x) {
            const double lon = ((double)x / (double)dw - 0.5) * kPi;
            sinLon[(size_t)x] = (float)std::sin(lon);
            cosLon[(size_t)x] = (float)std::cos(lon);
        }
        for (int y = 0; y < oh; ++y) {
            const double lat = (0.5 - (double)y / (double)dh) * kPi;
            sinLat[(size_t)y] = (float)std::sin(lat);
            cosLat[(size_t)y] = (float)std::cos(lat);
        }
    }

    // 生成第 y 行的源坐标。使用 (theta/sin(theta)) 等价形式，
    // 避免逐像素 atan2 + sin + cos。
    void Row(int y, float* mx, float* my) const {
        const float cl = cosLat[(size_t)y];
        const float sl = sinLat[(size_t)y];

        if (!rotated) {
            for (int x = 0; x < outW; ++x) {
                const float X = cl * sinLon[(size_t)x];
                const float Y = sl;
                float Z = cl * cosLon[(size_t)x];
                if (Z > 1.f) Z = 1.f;
                if (Z < -1.f) Z = -1.f;
                const float theta = acosf(Z);
                const float st = sqrtf(std::max(0.f, 1.f - Z * Z));
                if (st > 1e-9f) {
                    const float k = scale * theta / st;
                    mx[x] = cxf + k * X;
                    my[x] = cyf + k * Y;
                } else {
                    mx[x] = cxf;
                    my[x] = cyf;
                }
            }
            return;
        }

        for (int x = 0; x < outW; ++x) {
            const float X = cl * sinLon[(size_t)x];
            const float Y = sl;
            const float Z = cl * cosLon[(size_t)x];
            const float xr = rot[0][0] * X + rot[0][1] * Y + rot[0][2] * Z;
            const float yr = rot[1][0] * X + rot[1][1] * Y + rot[1][2] * Z;
            float zr = rot[2][0] * X + rot[2][1] * Y + rot[2][2] * Z;
            if (zr > 1.f) zr = 1.f;
            if (zr < -1.f) zr = -1.f;
            const float theta = acosf(zr);
            const float st = sqrtf(std::max(0.f, 1.f - zr * zr));
            if (st > 1e-9f) {
                const float k = scale * theta / st;
                mx[x] = cxf + k * xr;
                my[x] = cyf + k * yr;
            } else {
                mx[x] = cxf;
                my[x] = cyf;
            }
        }
    }
};

// 将 src 重映射到 dst 的 [xoff, xoff+outW) 列，并做垂直翻转
void RemapInto(const ImageRGB& src, const EquirectMap& map, ImageRGB& dst, int xoff,
               std::atomic<bool>& cancel, int threads, const std::function<void()>& tick) {
    ParallelRows(map.outH, threads, cancel, [&](int y) {
        thread_local std::vector<float> tmx;
        thread_local std::vector<float> tmy;
        if ((int)tmx.size() < map.outW) {
            tmx.resize((size_t)map.outW);
            tmy.resize((size_t)map.outW);
        }
        map.Row(y, tmx.data(), tmy.data());
        const int dstY = dst.h - 1 - y;  // 垂直翻转（修正上下颠倒）
        uint8_t* drow = dst.Row(dstY) + (size_t)xoff * 3;
        for (int x = 0; x < map.outW; ++x) {
            SampleBilinear(src, tmx[(size_t)x], tmy[(size_t)x], drow + (size_t)x * 3);
        }
        tick();
    });
}

// 将 src 平移 (dx 向右, dyUp 向上) 后写入 dst 的 [xoff, xoff+src.w)，越界填黑。
// 对应 Resolve Transform 的 Position：+X 向右，+Y 向上，单位是画面宽/高的比例。
void TranslateInto(const ImageRGB& src, ImageRGB& dst, int xoff, double dx, double dyUp,
                   std::atomic<bool>& cancel, int threads, const std::function<void()>& tick) {
    ParallelRows(src.h, threads, cancel, [&](int y) {
        uint8_t* drow = dst.Row(y) + (size_t)xoff * 3;
        const float sy = (float)((double)y + dyUp);
        for (int x = 0; x < src.w; ++x) {
            const float sx = (float)((double)x - dx);
            SampleBilinear(src, sx, sy, drow + (size_t)x * 3);
        }
        tick();
    });
}

// ---------------------------------------------------------------- JPEG 输出

void JpegWriteCallback(void* context, void* data, int size) {
    fwrite(data, 1, (size_t)size, (FILE*)context);
}

bool SaveJpeg(const std::wstring& path, const ImageRGB& img, int quality, std::wstring& err) {
    // 先写同目录临时文件，成功后再原子替换目标文件：
    // 避免写一半失败留下损坏的 JPG，也能把“目标文件被占用/只读”与“目录不可写”区分开。
    const std::wstring tmp = path + L".tmp";
    SetLastError(0);
    errno = 0;
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        DWORD code = GetLastError();
        if (code == 0) code = (errno == EACCES) ? ERROR_ACCESS_DENIED : ERROR_WRITE_FAULT;
        err = WFormat(L"无法创建输出文件: %ls（原因: %ls）", path.c_str(),
                      Win32ErrorText(code).c_str());
        if (IsLowIntegrity()) err += L"；" + LowIntegrityHint();
        _wremove(tmp.c_str());
        return false;
    }

    const int ok =
        stbi_write_jpg_to_func(JpegWriteCallback, f, img.w, img.h, 3, img.px.data(), quality);
    const int writeError = std::ferror(f);
    const int closeError = std::fclose(f);
    if (!ok || writeError != 0 || closeError != 0) {
        err = L"JPEG 编码/写盘失败: " + path;
        if (writeError != 0) err += L"（磁盘空间不足？）";
        _wremove(tmp.c_str());
        return false;
    }

    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD code = GetLastError();
        // 目标文件可能是只读的：清掉只读属性再试一次。
        if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) {
            const DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0) {
                SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                if (MoveFileExW(tmp.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    return true;
                }
                code = GetLastError();
            }
        }
        err = WFormat(L"无法写入输出文件: %ls（原因: %ls）", path.c_str(),
                      Win32ErrorText(code).c_str());
        _wremove(tmp.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 配对扫描

bool IsAsciiDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

std::wstring PrefixOf(const std::wstring& fileName) {
    std::wstring stem = fileName;
    size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem.resize(dot);

    size_t i = 0;
    while (i < stem.size() && !IsAsciiDigit(stem[i])) ++i;
    size_t j = i;
    while (j < stem.size() && IsAsciiDigit(stem[j])) ++j;
    if (j - i >= 6) return stem.substr(i, j - i);

    const std::wstring low = ToLower(stem);
    size_t p = low.find(L"frontfish");
    if (p == std::wstring::npos) p = low.find(L"backfish");
    if (p == std::wstring::npos) p = low.find(L"front");
    if (p == std::wstring::npos) p = low.find(L"back");
    if (p != std::wstring::npos) {
        std::wstring pre = stem.substr(0, p);
        while (!pre.empty() &&
               (pre.back() == L'_' || pre.back() == L'-' || pre.back() == L' ')) {
            pre.pop_back();
        }
        if (!pre.empty()) return pre;
    }
    return stem;
}

struct Job {
    std::wstring prefix;
    std::wstring front;
    std::wstring back;
};

// ---------------------------------------------------------------- 单对处理

bool ProcessPair(const Options& opt, const Job& job, int progBase, int progSpan,
                 int threads, const LogSink& sink, std::atomic<bool>& cancel,
                 std::wstring& err) {
    auto log = [&](const std::wstring& s) {
        if (sink.log) sink.log(L"  " + s);
    };
    auto report = [&](int local) {
        if (sink.progress) sink.progress(progBase + progSpan * local / 100);
    };
    auto fail = [&](const std::wstring& s) -> bool {
        err = s;
        log(L"[错误] " + s);
        return false;
    };

    const auto t0 = std::chrono::steady_clock::now();

    uint64_t sizeF = 0;
    uint64_t sizeB = 0;
    if (!FileSizeOf(job.front, sizeF)) return fail(L"无法读取文件: " + job.front);
    if (!FileSizeOf(job.back, sizeB)) return fail(L"无法读取文件: " + job.back);

    int Wf = 0, Hf = 0, nf = 0, Wb = 0, Hb = 0, nb = 0;
    if (!GuessResolution(sizeF, Wf, Hf, nf)) {
        return fail(WFormat(L"无法推断分辨率: %ls (%llu 字节)", FileNameOf(job.front).c_str(),
                            (unsigned long long)sizeF));
    }
    if (!GuessResolution(sizeB, Wb, Hb, nb)) {
        return fail(WFormat(L"无法推断分辨率: %ls (%llu 字节)", FileNameOf(job.back).c_str(),
                            (unsigned long long)sizeB));
    }

    log(WFormat(L"front: %ls  %dx%d, %d 帧", FileNameOf(job.front).c_str(), Wf, Hf, nf));
    log(WFormat(L"back : %ls  %dx%d, %d 帧", FileNameOf(job.back).c_str(), Wb, Hb, nb));
    if (Wf != Wb || Hf != Hb) {
        log(WFormat(L"[警告] 两只眼睛分辨率不一致 (front %dx%d / back %dx%d)，以 front 为准",
                    Wf, Hf, Wb, Hb));
    }

    ImageRGB frontImg;
    std::wstring loadErr;
    if (!LoadYuvFirstFrame(job.front, Wf, Hf, opt.i420, frontImg, loadErr)) return fail(loadErr);
    report(6);
    if (cancel.load()) return fail(L"已取消");

    ImageRGB backImg;
    if (!LoadYuvFirstFrame(job.back, Wb, Hb, opt.i420, backImg, loadErr)) return fail(loadErr);
    report(12);
    if (cancel.load()) return fail(L"已取消");

    // 左右眼互换：backFishEye -> 左眼，frontFishEye -> 右眼
    ImageRGB leftImg = std::move(backImg);

    const bool hasRotation = std::fabs(opt.rollDeg) > 1e-9 || std::fabs(opt.pitchDeg) > 1e-9 ||
                             std::fabs(opt.yawDeg) > 1e-9;
    const bool hasZoom = std::fabs(opt.zoom - 1.0) > 1e-9;
    const bool hasPosition =
        std::fabs(opt.positionX) > 1e-9 || std::fabs(opt.positionY) > 1e-9;
    if (hasRotation || hasZoom || hasPosition) {
        log(WFormat(
            L"右眼校准: Roll=%.3f°, Pitch=%.3f°, Yaw=%.3f°, Zoom=%.4f, PosX=%.4f, PosY=%.4f",
            opt.rollDeg, opt.pitchDeg, opt.yawDeg, opt.zoom, opt.positionX, opt.positionY));
    } else {
        log(L"右眼校准: 未启用（Roll/Pitch/Yaw=0, Zoom=1, Position=0）");
    }
    report(15);

    // 单眼输出为正方形：out_h = front 高，out_w = front 高
    const int outH = Hf;
    const int outW = Hf;
    log(WFormat(L"鱼眼→VR180 (FOV=%.1f°)，单眼 %dx%d，SBS %dx%d", opt.fovDeg, outW, outH,
                outW * 2, outH));

    EquirectMap mapLeft;
    mapLeft.Init(Wf, Hf, outW, outH, opt.fovDeg);

    EquirectMap mapRight;
    double rightRotation[3][3];
    BuildRotationMatrix(opt.rollDeg, opt.pitchDeg, opt.yawDeg, rightRotation);
    mapRight.Init(Wf, Hf, outW, outH, opt.fovDeg, hasRotation ? rightRotation : nullptr,
                  opt.zoom);

    ImageRGB sbs;
    sbs.Alloc(outW * 2, outH);

    const int totalRows = outH * 2 + (hasPosition ? outH : 0);
    std::atomic<int> rowsDone{0};
    std::atomic<int> lastPct{-1};
    auto tick = [&]() {
        const int done = rowsDone.fetch_add(1, std::memory_order_relaxed) + 1;
        int p = 15 + (int)((long long)72 * done / totalRows);
        if (p > 87) p = 87;
        int prev = lastPct.load(std::memory_order_relaxed);
        if (p != prev && lastPct.compare_exchange_strong(prev, p, std::memory_order_relaxed)) {
            report(p);
        }
    };

    // 左半幅 = back（不旋转），右半幅 = front（按 Roll/Pitch/Yaw/Zoom/Position 校准）
    RemapInto(leftImg, mapLeft, sbs, 0, cancel, threads, tick);
    if (cancel.load()) return fail(L"已取消");

    if (hasPosition) {
        // Position 是屏幕空间的 2D 平移（同 Resolve Transform）：先得到未平移的右眼，
        // 再按画面宽/高的比例平移，越界填黑。
        ImageRGB rightEq;
        rightEq.Alloc(outW, outH);
        RemapInto(frontImg, mapRight, rightEq, 0, cancel, threads, tick);
        if (cancel.load()) return fail(L"已取消");
        TranslateInto(rightEq, sbs, outW, opt.positionX * outW, opt.positionY * outH, cancel,
                      threads, tick);
        if (cancel.load()) return fail(L"已取消");
        rightEq.Release();
    } else {
        RemapInto(frontImg, mapRight, sbs, outW, cancel, threads, tick);
        if (cancel.load()) return fail(L"已取消");
    }

    leftImg.Release();
    frontImg.Release();
    report(90);

    const std::filesystem::path outPath =
        std::filesystem::path(opt.outputDir) / (job.prefix + L"_VR180_SBS.jpg");
    log(L"正在编码 JPEG 并写入: " + outPath.wstring());
    report(92);
    if (!SaveJpeg(outPath.wstring(), sbs, opt.jpegQuality, err)) {
        log(L"[错误] " + err);
        return false;
    }

    uint64_t outSize = 0;
    FileSizeOf(outPath.wstring(), outSize);
    report(100);

    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    log(WFormat(L"完成: %dx%d, %.2f MB, %.1f 秒", sbs.w, sbs.h,
                (double)outSize / (1024.0 * 1024.0), (double)ms / 1000.0));
    return true;
}

}  // namespace

// ---------------------------------------------------------------- 输出目录预检

bool CheckOutputDir(const std::wstring& dir, std::wstring& error) {
    error.clear();
    if (dir.empty()) {
        error = L"输出目录为空";
        return false;
    }

    std::error_code ec;
    SetLastError(0);
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    if (ec) {
        const DWORD code = GetLastError();
        const std::wstring reason =
            code != 0 ? Win32ErrorText(code) : Utf8ToWide(ec.message().c_str());
        error = WFormat(L"无法创建输出目录: %ls（原因: %ls）", dir.c_str(), reason.c_str());
        if (IsLowIntegrity()) error += L"；" + LowIntegrityHint();
        return false;
    }

    // 真正试写一个临时文件，确认目录可写（而不是等 2 秒转换完才报错）。
    const std::wstring probe =
        (std::filesystem::path(dir) /
         (L".vr180sbs_write_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
          std::to_wstring(GetCurrentThreadId()) + L".tmp"))
            .wstring();
    SetLastError(0);
    errno = 0;
    FILE* f = _wfopen(probe.c_str(), L"wb");
    if (!f) {
        DWORD code = GetLastError();
        if (code == 0) code = (errno == EACCES) ? ERROR_ACCESS_DENIED : ERROR_WRITE_FAULT;
        error = WFormat(L"输出目录不可写: %ls（原因: %ls）", dir.c_str(),
                        Win32ErrorText(code).c_str());
        if (IsLowIntegrity()) error += L"；" + LowIntegrityHint();
        return false;
    }
    std::fputc(0, f);
    std::fclose(f);
    _wremove(probe.c_str());
    return true;
}

// ---------------------------------------------------------------- 批量入口

bool Run(const Options& opt, const LogSink& sink, std::atomic<bool>& cancel,
         std::wstring& error) {
    error.clear();
    auto log = [&](const std::wstring& s) {
        if (sink.log) sink.log(s);
    };

    if (opt.inputDir.empty()) {
        error = L"输入目录为空";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path inDir(opt.inputDir);
    if (!std::filesystem::is_directory(inDir, ec)) {
        error = L"输入目录不存在: " + opt.inputDir;
        return false;
    }

    const std::wstring outDirStr = opt.outputDir.empty() ? opt.inputDir : opt.outputDir;
    if (!CheckOutputDir(outDirStr, error)) return false;

    int threads = opt.threads;
    if (threads <= 0) {
        threads = (int)std::thread::hardware_concurrency();
        if (threads <= 0) threads = 4;
    }
    if (threads > 32) threads = 32;

    log(WFormat(L"输入目录: %ls", inDir.wstring().c_str()));
    log(WFormat(L"输出目录: %ls", std::filesystem::path(outDirStr).wstring().c_str()));
    log(WFormat(L"参数: FOV=%.1f°, 右眼 Roll=%.3f° Pitch=%.3f° Yaw=%.3f° Zoom=%.4f PosX=%.4f PosY=%.4f, 格式=%ls, 线程=%d",
                opt.fovDeg, opt.rollDeg, opt.pitchDeg, opt.yawDeg, opt.zoom, opt.positionX,
                opt.positionY, opt.i420 ? L"I420" : L"NV12", threads));
    log(L"输出规则: 左眼=backFishEye，右眼=frontFishEye（按 Roll/Pitch/Yaw/Zoom/Position 校准）；自动忽略 preFishEye 文件");

    // ---- 扫描目录 ----
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> groups;  // prefix -> (front, back)
    int totalYuv = 0;
    int preCount = 0;
    int otherCount = 0;

    std::filesystem::directory_iterator it(inDir, ec);
    if (ec) {
        error = L"无法读取输入目录: " + opt.inputDir;
        return false;
    }
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const std::filesystem::path p = it->path();
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        if (ToLower(p.extension().wstring()) != L".yuv") continue;

        ++totalYuv;
        const std::wstring name = p.filename().wstring();
        const std::wstring low = ToLower(name);

        if (low.find(L"prefish") != std::wstring::npos ||
            low.find(L"pre_fish") != std::wstring::npos) {
            ++preCount;
            continue;
        }

        const std::wstring prefix = PrefixOf(name);
        auto& slot = groups[prefix];
        if (low.find(L"front") != std::wstring::npos) {
            if (slot.first.empty()) {
                slot.first = p.wstring();
            } else {
                log(WFormat(L"[提示] 前缀 %ls 有多个 front 文件，已使用 %ls", prefix.c_str(),
                            FileNameOf(slot.first).c_str()));
            }
        } else if (low.find(L"back") != std::wstring::npos) {
            if (slot.second.empty()) {
                slot.second = p.wstring();
            } else {
                log(WFormat(L"[提示] 前缀 %ls 有多个 back 文件，已使用 %ls", prefix.c_str(),
                            FileNameOf(slot.second).c_str()));
            }
        } else {
            ++otherCount;
        }
    }

    if (totalYuv == 0) {
        error = L"输入目录中没有 *.yuv 文件: " + opt.inputDir;
        return false;
    }

    std::vector<Job> jobs;
    for (const auto& kv : groups) {
        const bool hasF = !kv.second.first.empty();
        const bool hasB = !kv.second.second.empty();
        if (hasF && hasB) {
            jobs.push_back(Job{kv.first, kv.second.first, kv.second.second});
        } else {
            log(WFormat(L"[跳过] 前缀 %ls 缺少 %ls", kv.first.c_str(),
                        hasF ? L"backFishEye" : L"frontFishEye"));
        }
    }

    log(WFormat(L"扫描: %d 个 .yuv（忽略 preFishEye %d 个，无法识别 %d 个），配对 %d 对",
                totalYuv, preCount, otherCount, (int)jobs.size()));
    if (jobs.empty()) {
        error = L"未找到 frontFishEye/backFishEye 配对";
        return false;
    }

    // ---- 逐对处理 ----
    int okCount = 0;
    int failCount = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
        if (cancel.load(std::memory_order_relaxed)) break;

        const int base = (int)((long long)i * 100 / (long long)jobs.size());
        const int span = (int)((long long)100 / (long long)jobs.size());
        log(WFormat(L"=== [%d/%d] %ls ===", (int)i + 1, (int)jobs.size(), jobs[i].prefix.c_str()));

        std::wstring perr;
        bool ok = false;
        try {
            ok = ProcessPair(opt, jobs[i], base, span, threads, sink, cancel, perr);
        } catch (const std::bad_alloc&) {
            perr = L"内存不足（图像过大）";
            log(L"[错误] " + perr);
        } catch (const std::exception& e) {
            perr = Utf8ToWide(e.what());
            log(L"[错误] " + perr);
        } catch (...) {
            perr = L"未知错误";
            log(L"[错误] " + perr);
        }

        if (ok) {
            ++okCount;
        } else {
            ++failCount;
            if (!perr.empty()) error = perr;
        }
    }

    if (cancel.load(std::memory_order_relaxed)) {
        error = L"已取消";
    } else if (okCount == 0) {
        if (error.empty()) error = L"没有成功处理任何配对";
    } else {
        error.clear();
    }

    log(WFormat(L"处理结束: 成功 %d 对，失败 %d 对", okCount, failCount));
    return okCount > 0;
}

std::wstring WFormat(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vswprintf(buf, sizeof(buf) / sizeof(buf[0]), fmt, ap);
    va_end(ap);
    if (n < 0) {
        buf[sizeof(buf) / sizeof(buf[0]) - 1] = L'\0';
        return buf;
    }
    if (n >= (int)(sizeof(buf) / sizeof(buf[0]))) n = (int)(sizeof(buf) / sizeof(buf[0])) - 1;
    buf[n] = L'\0';
    return buf;
}

}  // namespace vr180
