// 简易 Win32 界面：输入目录、输出目录、右眼 Roll/Pitch/Yaw 校准角。

#include "gui.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "core.h"

namespace {

constexpr wchar_t kWindowClass[] = L"VR180SBSWindow";
constexpr wchar_t kTitle[] = L"VR180 SBS 转换工具";

constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_PROGRESS = WM_APP + 2;
constexpr UINT WM_APP_DONE = WM_APP + 3;

enum : int {
    IDC_HEADER = 1000,
    IDC_LBL_INPUT,
    IDC_EDIT_INPUT,
    IDC_BTN_INPUT,
    IDC_LBL_OUTPUT,
    IDC_EDIT_OUTPUT,
    IDC_BTN_OUTPUT,
    IDC_LBL_ROLL,
    IDC_EDIT_ROLL,
    IDC_LBL_PITCH,
    IDC_EDIT_PITCH,
    IDC_LBL_YAW,
    IDC_EDIT_YAW,
    IDC_LBL_ZOOM,
    IDC_EDIT_ZOOM,
    IDC_LBL_POSX,
    IDC_EDIT_POSX,
    IDC_LBL_POSY,
    IDC_EDIT_POSY,
    IDC_LBL_FOV,
    IDC_EDIT_FOV,
    IDC_LBL_FOV_HINT,
    IDC_LBL_CAL_HINT,
    IDC_LBL_POS_HINT,
    IDC_BTN_START,
    IDC_BTN_STOP,
    IDC_PROGRESS,
    IDC_STATUS,
    IDC_LOG,
};

struct App {
    HINSTANCE hInst = nullptr;
    HWND hwnd = nullptr;

    HWND hHeader = nullptr;
    HWND hLblInput = nullptr;
    HWND hInput = nullptr;
    HWND hBtnInput = nullptr;
    HWND hLblOutput = nullptr;
    HWND hOutput = nullptr;
    HWND hBtnOutput = nullptr;
    HWND hLblRoll = nullptr;
    HWND hRoll = nullptr;
    HWND hLblPitch = nullptr;
    HWND hPitch = nullptr;
    HWND hLblYaw = nullptr;
    HWND hYaw = nullptr;
    HWND hLblZoom = nullptr;
    HWND hZoom = nullptr;
    HWND hLblPosX = nullptr;
    HWND hPosX = nullptr;
    HWND hLblPosY = nullptr;
    HWND hPosY = nullptr;
    HWND hLblFov = nullptr;
    HWND hFov = nullptr;
    HWND hFovHint = nullptr;
    HWND hCalHint = nullptr;
    HWND hPosHint = nullptr;
    HWND hStart = nullptr;
    HWND hStop = nullptr;
    HWND hProgress = nullptr;
    HWND hStatus = nullptr;
    HWND hLog = nullptr;

    HFONT fontUi = nullptr;
    HFONT fontHeader = nullptr;
    HFONT fontLog = nullptr;
    HBRUSH whiteBrush = nullptr;

    UINT dpi = 96;
    bool running = false;
    bool closing = false;
    std::thread worker;
    std::atomic<bool> cancel{false};

    std::wstring exeDir;
};

App g;

// ---------------------------------------------------------------- 小工具

int S(int v) { return MulDiv(v, (int)g.dpi, 96); }

std::wstring Trim(std::wstring s) {
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) ++a;
    size_t b = s.size();
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring GetText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

void SetStatus(const std::wstring& text) {
    if (g.hStatus) SetWindowTextW(g.hStatus, text.c_str());
}

// 设置单行编辑框文本，并把光标移到开头，避免长路径只显示末尾
void SetEditText(HWND edit, const std::wstring& text) {
    SetWindowTextW(edit, text.c_str());
    SendMessageW(edit, EM_SETSEL, 0, 0);
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}

void AppendLog(const std::wstring& text) {
    if (!g.hLog) return;
    std::wstring line = text;
    line += L"\r\n";
    const int len = GetWindowTextLengthW(g.hLog);
    SendMessageW(g.hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g.hLog, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessageW(g.hLog, EM_SCROLLCARET, 0, 0);
}

void MsgBox(const std::wstring& text, UINT flags = MB_ICONWARNING | MB_OK) {
    MessageBoxW(g.hwnd, text.c_str(), kTitle, flags);
}

std::wstring ExeDir() {
    std::wstring buf(MAX_PATH * 4, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
    if (n == 0) return L".";
    buf.resize((size_t)n);
    std::filesystem::path p(buf);
    return p.parent_path().wstring();
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

struct AppSettings {
    std::wstring input;
    std::wstring output;
    std::wstring roll = L"0";
    std::wstring pitch = L"0";
    std::wstring yaw = L"0";
    std::wstring zoom = L"1";
    std::wstring fov = L"208";
    std::wstring posx = L"0";
    std::wstring posy = L"0";
};

std::wstring ConfigPath() { return g.exeDir + L"\\VR180SBS.ini"; }

AppSettings LoadConfig() {
    AppSettings s;
    FILE* f = _wfopen(ConfigPath().c_str(), L"rb");
    if (!f) return s;
    std::string data;
    char buf[4096];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, got);
    fclose(f);

    size_t pos = 0;
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF) {
        pos = 3;
    }
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::wstring key = Trim(FromUtf8(line.substr(0, eq)));
        const std::wstring value = Trim(FromUtf8(line.substr(eq + 1)));
        if (key == L"input") {
            s.input = value;
        } else if (key == L"output") {
            s.output = value;
        } else if (key == L"roll") {
            s.roll = value;
        } else if (key == L"pitch") {
            s.pitch = value;
        } else if (key == L"yaw") {
            s.yaw = value;
        } else if (key == L"zoom" || key == L"scale") {
            // 兼容旧配置里的 scale=
            s.zoom = value;
        } else if (key == L"fov") {
            s.fov = value;
        } else if (key == L"posx") {
            s.posx = value;
        } else if (key == L"posy") {
            s.posy = value;
        }
    }
    if (s.roll.empty()) s.roll = L"0";
    if (s.pitch.empty()) s.pitch = L"0";
    if (s.yaw.empty()) s.yaw = L"0";
    if (s.zoom.empty()) s.zoom = L"1";
    if (s.fov.empty()) s.fov = L"208";
    if (s.posx.empty()) s.posx = L"0";
    if (s.posy.empty()) s.posy = L"0";
    return s;
}

void SaveConfig(const std::wstring& input, const std::wstring& output, double roll, double pitch,
                double yaw, double zoom, double fov, double posX, double posY) {
    char rollBuf[64];
    char pitchBuf[64];
    char yawBuf[64];
    char zoomBuf[64];
    char fovBuf[64];
    char posxBuf[64];
    char posyBuf[64];
    std::snprintf(rollBuf, sizeof(rollBuf), "%.10g", roll);
    std::snprintf(pitchBuf, sizeof(pitchBuf), "%.10g", pitch);
    std::snprintf(yawBuf, sizeof(yawBuf), "%.10g", yaw);
    std::snprintf(zoomBuf, sizeof(zoomBuf), "%.10g", zoom);
    std::snprintf(fovBuf, sizeof(fovBuf), "%.10g", fov);
    std::snprintf(posxBuf, sizeof(posxBuf), "%.10g", posX);
    std::snprintf(posyBuf, sizeof(posyBuf), "%.10g", posY);
    const std::string text = "input=" + ToUtf8(input) + "\r\noutput=" + ToUtf8(output) +
                             "\r\nroll=" + rollBuf + "\r\npitch=" + pitchBuf + "\r\nyaw=" + yawBuf +
                             "\r\nzoom=" + zoomBuf + "\r\nfov=" + fovBuf + "\r\nposx=" + posxBuf +
                             "\r\nposy=" + posyBuf + "\r\n";
    FILE* f = _wfopen(ConfigPath().c_str(), L"wb");
    if (!f) return;
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    fwrite(bom, 1, sizeof(bom), f);
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

std::wstring PickFolder(HWND owner, const std::wstring& initial) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
        return std::wstring();
    }
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    if (!initial.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    std::wstring result;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

// ---------------------------------------------------------------- 字体与控件

HFONT MakeFont(int point, int weight, const std::wstring& face) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(point, (int)g.dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, face.c_str(), LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

void DestroyFonts() {
    if (g.fontUi) DeleteObject(g.fontUi);
    if (g.fontHeader) DeleteObject(g.fontHeader);
    if (g.fontLog) DeleteObject(g.fontLog);
    g.fontUi = g.fontHeader = g.fontLog = nullptr;
}

void CreateFonts() {
    DestroyFonts();
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    std::wstring face = ncm.lfMessageFont.lfFaceName;
    if (face.empty()) face = L"Microsoft YaHei UI";

    g.fontUi = MakeFont(9, FW_NORMAL, face);
    g.fontHeader = MakeFont(12, FW_SEMIBOLD, face);
    g.fontLog = MakeFont(9, FW_NORMAL, L"Consolas");
}

void ApplyFonts() {
    HWND controls[] = {g.hLblInput, g.hInput,    g.hBtnInput, g.hLblOutput, g.hOutput,
                       g.hBtnOutput, g.hLblRoll, g.hRoll,      g.hLblPitch, g.hPitch,
                       g.hLblYaw,    g.hYaw,      g.hLblZoom,  g.hZoom,     g.hLblPosX,
                       g.hPosX,      g.hLblPosY,  g.hPosY,     g.hLblFov,   g.hFov,
                       g.hFovHint,   g.hCalHint,  g.hPosHint,  g.hStart,    g.hStop,
                       g.hStatus,    g.hLog,      g.hProgress};
    for (HWND h : controls) {
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g.fontUi, TRUE);
    }
    if (g.hHeader) SendMessageW(g.hHeader, WM_SETFONT, (WPARAM)g.fontHeader, TRUE);
    if (g.hLog) SendMessageW(g.hLog, WM_SETFONT, (WPARAM)g.fontLog, TRUE);
}

HWND MakeStatic(HWND parent, int id, const wchar_t* text, DWORD extraStyle = 0) {
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | SS_LEFT | extraStyle, 0, 0, 0, 0, parent,
                           (HMENU)(INT_PTR)id, g.hInst, nullptr);
}

HWND MakeButton(HWND parent, int id, const wchar_t* text) {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                           parent, (HMENU)(INT_PTR)id, g.hInst, nullptr);
}

HWND MakeEdit(HWND parent, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0,
                           parent, (HMENU)(INT_PTR)id, g.hInst, nullptr);
}

void CreateControls(HWND hwnd) {
    g.hHeader = MakeStatic(hwnd, IDC_HEADER,
                           L"YUV 双鱼眼 → VR180 SBS（左眼=backFishEye，右眼=frontFishEye）");
    g.hLblInput = MakeStatic(hwnd, IDC_LBL_INPUT, L"输入目录:");
    g.hInput = MakeEdit(hwnd, IDC_EDIT_INPUT);
    g.hBtnInput = MakeButton(hwnd, IDC_BTN_INPUT, L"浏览...");

    g.hLblOutput = MakeStatic(hwnd, IDC_LBL_OUTPUT, L"输出目录:");
    g.hOutput = MakeEdit(hwnd, IDC_EDIT_OUTPUT);
    g.hBtnOutput = MakeButton(hwnd, IDC_BTN_OUTPUT, L"浏览...");

    g.hLblRoll = MakeStatic(hwnd, IDC_LBL_ROLL, L"右眼 Roll(°):");
    g.hRoll = MakeEdit(hwnd, IDC_EDIT_ROLL);
    g.hLblPitch = MakeStatic(hwnd, IDC_LBL_PITCH, L"Pitch(°):");
    g.hPitch = MakeEdit(hwnd, IDC_EDIT_PITCH);
    g.hLblYaw = MakeStatic(hwnd, IDC_LBL_YAW, L"Yaw(°):");
    g.hYaw = MakeEdit(hwnd, IDC_EDIT_YAW);
    g.hLblZoom = MakeStatic(hwnd, IDC_LBL_ZOOM, L"Zoom:");
    g.hZoom = MakeEdit(hwnd, IDC_EDIT_ZOOM);
    g.hCalHint = MakeStatic(hwnd, IDC_LBL_CAL_HINT, L"右手定则 X右/Y上/Z前");

    g.hLblPosX = MakeStatic(hwnd, IDC_LBL_POSX, L"右眼 Position X:");
    g.hPosX = MakeEdit(hwnd, IDC_EDIT_POSX);
    g.hLblPosY = MakeStatic(hwnd, IDC_LBL_POSY, L"Position Y:");
    g.hPosY = MakeEdit(hwnd, IDC_EDIT_POSY);
    g.hPosHint = MakeStatic(hwnd, IDC_LBL_POS_HINT, L"偏移=画面宽/高比例，+X 右 / +Y 上（同 Resolve）");

    g.hLblFov = MakeStatic(hwnd, IDC_LBL_FOV, L"鱼眼 FOV(°):");
    g.hFov = MakeEdit(hwnd, IDC_EDIT_FOV);
    g.hFovHint = MakeStatic(hwnd, IDC_LBL_FOV_HINT,
                            L"全视场角（直径对应的角度），默认 208");

    g.hStart = MakeButton(hwnd, IDC_BTN_START, L"开始转换");
    g.hStop = MakeButton(hwnd, IDC_BTN_STOP, L"停止");
    EnableWindow(g.hStop, FALSE);

    g.hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_BORDER | PBS_SMOOTH, 0, 0, 0, 0,
                                  hwnd, (HMENU)(INT_PTR)IDC_PROGRESS, g.hInst, nullptr);
    SendMessageW(g.hProgress, PBM_SETRANGE32, 0, 100);

    g.hStatus = MakeStatic(hwnd, IDC_STATUS, L"就绪");

    g.hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                                 ES_READONLY | ES_NOHIDESEL,
                             0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LOG, g.hInst, nullptr);
    SendMessageW(g.hLog, EM_SETLIMITTEXT, (WPARAM)0x7FFFFFF0, 0);

    ApplyFonts();
}

void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int W = rc.right;
    const int H = rc.bottom;
    if (W <= 0 || H <= 0) return;

    const int m = S(16);
    const int gap = S(10);
    const int rowH = S(28);
    const int btnW = S(92);
    const int labelW = S(72);

    int y = m;
    MoveWindow(g.hHeader, m, y, W - 2 * m, S(24), TRUE);
    y += S(24) + gap;

    const int editX = m + labelW + S(8);
    const int browseX = W - m - btnW;
    const int editW = std::max(S(80), browseX - gap - editX);

    MoveWindow(g.hLblInput, m, y + S(6), labelW, S(20), TRUE);
    MoveWindow(g.hInput, editX, y, editW, rowH, TRUE);
    MoveWindow(g.hBtnInput, browseX, y, btnW, rowH, TRUE);
    y += rowH + gap;

    MoveWindow(g.hLblOutput, m, y + S(6), labelW, S(20), TRUE);
    MoveWindow(g.hOutput, editX, y, editW, rowH, TRUE);
    MoveWindow(g.hBtnOutput, browseX, y, btnW, rowH, TRUE);
    y += rowH + gap;

    const int calLabelW = S(105);
    MoveWindow(g.hLblRoll, m, y + S(6), calLabelW, S(20), TRUE);
    int x = m + calLabelW + S(8);
    MoveWindow(g.hRoll, x, y, S(80), rowH, TRUE);
    x += S(80) + gap;
    MoveWindow(g.hLblPitch, x, y + S(6), S(62), S(20), TRUE);
    x += S(62) + S(6);
    MoveWindow(g.hPitch, x, y, S(80), rowH, TRUE);
    x += S(80) + gap;
    MoveWindow(g.hLblYaw, x, y + S(6), S(55), S(20), TRUE);
    x += S(55) + S(6);
    MoveWindow(g.hYaw, x, y, S(80), rowH, TRUE);
    x += S(80) + gap;
    MoveWindow(g.hLblZoom, x, y + S(6), S(50), S(20), TRUE);
    x += S(50) + S(6);
    MoveWindow(g.hZoom, x, y, S(70), rowH, TRUE);
    x += S(70) + gap;
    MoveWindow(g.hCalHint, x, y + S(6), std::max(S(120), W - m - x), S(20), TRUE);
    y += rowH + gap;

    MoveWindow(g.hLblPosX, m, y + S(6), S(130), S(20), TRUE);
    x = m + S(130) + S(8);
    MoveWindow(g.hPosX, x, y, S(90), rowH, TRUE);
    x += S(90) + gap;
    MoveWindow(g.hLblPosY, x, y + S(6), S(80), S(20), TRUE);
    x += S(80) + S(6);
    MoveWindow(g.hPosY, x, y, S(90), rowH, TRUE);
    x += S(90) + gap;
    MoveWindow(g.hPosHint, x, y + S(6), std::max(S(120), W - m - x), S(20), TRUE);
    y += rowH + gap;

    MoveWindow(g.hLblFov, m, y + S(6), S(130), S(20), TRUE);
    x = m + S(130) + S(8);
    MoveWindow(g.hFov, x, y, S(90), rowH, TRUE);
    x += S(90) + gap;
    MoveWindow(g.hFovHint, x, y + S(6), std::max(S(120), W - m - x), S(20), TRUE);
    y += rowH + gap;

    const int bigBtnH = S(34);
    const int startW = S(130);
    const int stopW = S(100);
    x = m;
    MoveWindow(g.hStart, x, y, startW, bigBtnH, TRUE);
    x += startW + gap;
    MoveWindow(g.hStop, x, y, stopW, bigBtnH, TRUE);
    x += stopW + gap;
    const int progW = std::max(S(80), W - m - x);
    MoveWindow(g.hProgress, x, y + S(6), progW, S(22), TRUE);
    y += bigBtnH + gap;

    MoveWindow(g.hStatus, m, y, W - 2 * m, S(20), TRUE);
    y += S(22) + S(4);

    const int logH = std::max(S(100), H - m - y);
    MoveWindow(g.hLog, m, y, W - 2 * m, logH, TRUE);

    InvalidateRect(hwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------- 设置

bool ContainsYuv(const std::wstring& dir) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        std::wstring ext = it->path().extension().wstring();
        for (wchar_t& c : ext) {
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        }
        if (ext == L".yuv") return true;
    }
    return false;
}

void LoadSettings() {
    g.exeDir = ExeDir();
    const AppSettings cfg = LoadConfig();

    std::wstring input = cfg.input;
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(input), ec)) {
        input = g.exeDir;
        // 若 exe 所在目录没有 yuv（例如放在 dist 里），向上最多找两级
        if (!ContainsYuv(input)) {
            std::filesystem::path p(g.exeDir);
            for (int i = 0; i < 2; ++i) {
                p = p.parent_path();
                if (p.empty()) break;
                if (ContainsYuv(p.wstring())) {
                    input = p.wstring();
                    break;
                }
            }
        }
    }

    std::wstring output = cfg.output;
    if (!std::filesystem::is_directory(std::filesystem::path(output), ec)) output = input;

    SetEditText(g.hInput, input);
    SetEditText(g.hOutput, output);
    SetWindowTextW(g.hRoll, cfg.roll.c_str());
    SetWindowTextW(g.hPitch, cfg.pitch.c_str());
    SetWindowTextW(g.hYaw, cfg.yaw.c_str());
    SetWindowTextW(g.hZoom, cfg.zoom.c_str());
    SetWindowTextW(g.hFov, cfg.fov.c_str());
    SetWindowTextW(g.hPosX, cfg.posx.c_str());
    SetWindowTextW(g.hPosY, cfg.posy.c_str());

    AppendLog(L"使用说明：");
    AppendLog(L"  1) 输入目录内放 *_frontFishEye.yuv 与 *_backFishEye.yuv（按时间戳前缀配对）");
    AppendLog(L"  2) 程序自动忽略 preFishEye 文件；输出左眼=backFishEye，右眼=frontFishEye");
    AppendLog(L"  3) 右眼用 Roll/Pitch/Yaw + Zoom + Position X/Y 校准，默认 0/0/0/1/0/0");
    AppendLog(L"  4) 鱼眼 FOV 默认 208°，与相机镜头实际全视场角不符时可在此调整");
    AppendLog(L"");
}

void SaveSettings(const std::wstring& input, const std::wstring& output, double roll, double pitch,
                  double yaw, double zoom, double fov, double posX, double posY) {
    SaveConfig(input, output, roll, pitch, yaw, zoom, fov, posX, posY);
}

// ---------------------------------------------------------------- 开始/停止

void OnStart() {
    if (g.running) return;

    std::wstring input = Trim(GetText(g.hInput));
    std::wstring output = Trim(GetText(g.hOutput));

    if (input.empty()) {
        MsgBox(L"请先选择输入目录。");
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(input), ec)) {
        MsgBox(L"输入目录不存在：\n" + input);
        return;
    }
    if (output.empty()) output = input;
    // 先确认输出目录真的可写，避免转换跑完才报“无法创建输出文件”。
    std::wstring dirErr;
    if (!vr180::CheckOutputDir(output, dirErr)) {
        const std::wstring msg = dirErr + L"\n\n是否改为输出到输入目录？\n" + input;
        if (MessageBoxW(g.hwnd, msg.c_str(), kTitle, MB_ICONWARNING | MB_YESNO) != IDYES) {
            SetStatus(L"输出目录不可写");
            return;
        }
        output = input;
        if (!vr180::CheckOutputDir(output, dirErr)) {
            MsgBox(dirErr, MB_ICONERROR | MB_OK);
            return;
        }
    }

    auto parseAngle = [&](HWND edit, const wchar_t* name, double& value) -> bool {
        const std::wstring text = Trim(GetText(edit));
        if (text.empty()) {
            value = 0.0;
            return true;
        }
        wchar_t* end = nullptr;
        value = wcstod(text.c_str(), &end);
        if (end == text.c_str() || *end != L'\0' || !std::isfinite(value)) {
            MsgBox(std::wstring(name) + L" 角度格式不正确，请输入数字，例如 0.5 或 -0.5。");
            return false;
        }
        if (std::fabs(value) > 45.0) {
            MsgBox(std::wstring(name) + L" 角度请设置在 ±45° 以内。");
            return false;
        }
        return true;
    };

    auto parseField = [&](HWND edit, const wchar_t* name, double def, double lo, double hi,
                          const wchar_t* range, double& value) -> bool {
        const std::wstring text = Trim(GetText(edit));
        if (text.empty()) {
            value = def;
            return true;
        }
        wchar_t* end = nullptr;
        value = wcstod(text.c_str(), &end);
        if (end == text.c_str() || *end != L'\0' || !std::isfinite(value)) {
            MsgBox(std::wstring(name) + L" 格式不正确，请输入数字。");
            return false;
        }
        if (value < lo || value > hi) {
            MsgBox(std::wstring(name) + L" 请设置在 " + range + L" 之间。");
            return false;
        }
        return true;
    };

    double roll = 0.0;
    double pitch = 0.0;
    double yaw = 0.0;
    if (!parseAngle(g.hRoll, L"Roll", roll)) return;
    if (!parseAngle(g.hPitch, L"Pitch", pitch)) return;
    if (!parseAngle(g.hYaw, L"Yaw", yaw)) return;

    double zoom = 1.0;
    double fov = 208.0;
    double posX = 0.0;
    double posY = 0.0;
    if (!parseField(g.hZoom, L"Zoom", 1.0, 0.1, 10.0, L"0.1–10", zoom)) return;
    if (!parseField(g.hFov, L"鱼眼 FOV", 208.0, 60.0, 360.0, L"60–360", fov)) return;
    if (!parseField(g.hPosX, L"Position X", 0.0, -10.0, 10.0, L"-10–10", posX)) return;
    if (!parseField(g.hPosY, L"Position Y", 0.0, -10.0, 10.0, L"-10–10", posY)) return;

    SetEditText(g.hInput, input);
    SetEditText(g.hOutput, output);
    SaveSettings(input, output, roll, pitch, yaw, zoom, fov, posX, posY);

    AppendLog(L"");
    AppendLog(vr180::WFormat(L"===== 开始处理：%ls =====", input.c_str()));
    AppendLog(vr180::WFormat(
        L"输出目录：%ls，鱼眼 FOV=%.1f°，右眼 Roll=%.3f° Pitch=%.3f° Yaw=%.3f° Zoom=%.4f "
        L"PosX=%.4f PosY=%.4f",
        output.c_str(), fov, roll, pitch, yaw, zoom, posX, posY));

    g.cancel.store(false);
    g.running = true;
    EnableWindow(g.hStart, FALSE);
    EnableWindow(g.hStop, TRUE);
    SendMessageW(g.hProgress, PBM_SETSTATE, PBST_NORMAL, 0);
    SendMessageW(g.hProgress, PBM_SETPOS, 0, 0);
    SetStatus(L"正在处理...");

    auto opt = std::make_shared<vr180::Options>();
    opt->inputDir = input;
    opt->outputDir = output;
    opt->rollDeg = roll;
    opt->pitchDeg = pitch;
    opt->yawDeg = yaw;
    opt->zoom = zoom;
    opt->fovDeg = fov;
    opt->positionX = posX;
    opt->positionY = posY;

    const HWND hwnd = g.hwnd;
    g.worker = std::thread([hwnd, opt]() {
        vr180::LogSink sink;
        sink.log = [hwnd](const std::wstring& line) {
            PostMessageW(hwnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(line));
        };
        sink.progress = [hwnd](int percent) {
            PostMessageW(hwnd, WM_APP_PROGRESS, (WPARAM)percent, 0);
        };
        std::wstring err;
        const bool ok = vr180::Run(*opt, sink, g.cancel, err);
        PostMessageW(hwnd, WM_APP_DONE, ok ? 1 : 0, (LPARAM)new std::wstring(err));
    });
}

void OnStop() {
    if (!g.running) return;
    g.cancel.store(true);
    EnableWindow(g.hStop, FALSE);
    SetStatus(L"正在停止...");
}

// ---------------------------------------------------------------- 消息处理

void OnDone(bool ok, std::wstring* err) {
    if (g.worker.joinable()) g.worker.join();
    g.running = false;
    EnableWindow(g.hStart, TRUE);
    EnableWindow(g.hStop, FALSE);

    if (ok) {
        SendMessageW(g.hProgress, PBM_SETSTATE, PBST_NORMAL, 0);
        SendMessageW(g.hProgress, PBM_SETPOS, 100, 0);
        SetStatus(L"处理完成");
    } else if (err && *err == L"已取消") {
        SendMessageW(g.hProgress, PBM_SETSTATE, PBST_NORMAL, 0);
        SetStatus(L"已取消");
    } else {
        // 失败时把进度条标红，避免“看起来已经 100% 完成”的误解。
        SendMessageW(g.hProgress, PBM_SETSTATE, PBST_ERROR, 0);
        SetStatus(L"处理失败");
        if (err && !err->empty()) MsgBox(*err, MB_ICONERROR | MB_OK);
    }
}

void OnClose() {
    if (g.closing) return;
    if (g.running) {
        if (MessageBoxW(g.hwnd, L"正在处理，确定要停止并退出吗？", kTitle,
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
        g.closing = true;
        g.cancel.store(true);
        SetStatus(L"正在停止...");
        while (g.running) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(20);
        }
        if (g.worker.joinable()) g.worker.join();
    }
    DestroyWindow(g.hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g.hwnd = hwnd;
            g.dpi = GetDpiForWindow(hwnd);
            if (g.dpi == 0) g.dpi = 96;
            g.whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
            CreateFonts();
            CreateControls(hwnd);
            LoadSettings();
            Layout(hwnd);
            return 0;
        }
        case WM_SIZE:
            Layout(hwnd);
            return 0;
        case WM_DPICHANGED: {
            const UINT newDpi = HIWORD(wParam);
            if (newDpi != 0) g.dpi = newDpi;
            CreateFonts();
            ApplyFonts();
            const RECT* target = (const RECT*)lParam;
            SetWindowPos(hwnd, nullptr, target->left, target->top, target->right - target->left,
                         target->bottom - target->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(hwnd);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = S(900);
            mmi->ptMinTrackSize.y = S(480);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            HWND control = (HWND)lParam;
            if (control == g.hLog) {
                SetBkColor(hdc, RGB(255, 255, 255));
                SetTextColor(hdc, RGB(0, 0, 0));
                return (LRESULT)g.whiteBrush;
            }
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            switch (id) {
                case IDC_BTN_INPUT: {
                    std::wstring dir = PickFolder(hwnd, Trim(GetText(g.hInput)));
                    if (!dir.empty()) SetEditText(g.hInput, dir);
                    return 0;
                }
                case IDC_BTN_OUTPUT: {
                    std::wstring dir = PickFolder(hwnd, Trim(GetText(g.hOutput)));
                    if (!dir.empty()) SetEditText(g.hOutput, dir);
                    return 0;
                }
                case IDC_BTN_START:
                    OnStart();
                    return 0;
                case IDC_BTN_STOP:
                    OnStop();
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_APP_LOG: {
            std::wstring* line = (std::wstring*)lParam;
            if (line) {
                AppendLog(*line);
                delete line;
            }
            return 0;
        }
        case WM_APP_PROGRESS: {
            if (g.running) SendMessageW(g.hProgress, PBM_SETPOS, wParam, 0);
            return 0;
        }
        case WM_APP_DONE: {
            std::wstring* err = (std::wstring*)lParam;
            OnDone(wParam != 0, err);
            delete err;
            return 0;
        }
        case WM_CLOSE:
            OnClose();
            return 0;
        case WM_DESTROY:
            DestroyFonts();
            if (g.whiteBrush) {
                DeleteObject(g.whiteBrush);
                g.whiteBrush = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int RunGui(HINSTANCE hInst, int nCmdShow) {
    g.hInst = hInst;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    const UINT sysDpi = GetDpiForSystem();
    const int w = MulDiv(940, (int)sysDpi, 96);
    const int h = MulDiv(720, (int)sysDpi, 96);
    RECT rc{0, 0, w, h};
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, 0);

    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;
    const int margin = MulDiv(16, (int)sysDpi, 96);
    if (winW > workW - margin * 2) winW = workW - margin * 2;
    if (winH > workH - margin * 2) winH = workH - margin * 2;
    const int posX = work.left + (workW - winW) / 2;
    const int posY = work.top + (workH - winH) / 2;

    HWND hwnd = CreateWindowExW(0, kWindowClass, kTitle, style, posX, posY, winW, winH,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    // GetDpiForSystem() 对 Per-Monitor DPI 进程恒为 96，窗口在 125%/150% 缩放的
    // 显示器上会小于布局所需尺寸，右侧提示文字被截断。这里按窗口实际 DPI 修正一次
    // 初始尺寸（还在 ShowWindow 之前，用户看不到跳动）。
    const UINT winDpi = GetDpiForWindow(hwnd);
    if (winDpi != 0 && winDpi != sysDpi) {
        RECT want{0, 0, MulDiv(940, (int)winDpi, 96), MulDiv(720, (int)winDpi, 96)};
        AdjustWindowRectEx(&want, style, FALSE, 0);
        int nw = want.right - want.left;
        int nh = want.bottom - want.top;
        if (nw > workW - margin * 2) nw = workW - margin * 2;
        if (nh > workH - margin * 2) nh = workH - margin * 2;
        const int nx = work.left + (workW - nw) / 2;
        const int ny = work.top + (workH - nh) / 2;
        SetWindowPos(hwnd, nullptr, nx, ny, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
