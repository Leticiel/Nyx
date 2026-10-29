#include "tool.hpp"
#include "progress.hpp"
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cmath>

namespace
{
using namespace csystem;

constexpr UINT progress_message = WM_APP + 1;
constexpr UINT finished_message = WM_APP + 2;
constexpr int export_button = 101;
constexpr int build_button = 102;
constexpr int cancel_button = 104;
constexpr int full_mode = 105;
constexpr int partial_mode = 106;
constexpr int theme_button = 107;
constexpr wchar_t theme_settings[] = L"Software\\Leticiel\\Nyx";

struct Palette
{
    COLORREF background, surface, ink, muted, border, accent, selection;
    COLORREF primary, primary_text, disabled, disabled_text, pressed_selection;
    COLORREF pressed_primary, pressed_surface, hovered_primary, hovered_surface, radio_border, error;
};

constexpr Palette light_palette{
    RGB(246,248,249), RGB(255,255,255), RGB(28,38,44), RGB(106,118,126),
    RGB(221,228,232), RGB(25,119,99), RGB(239,248,245),
    RGB(28,38,44), RGB(255,255,255), RGB(241,244,245), RGB(153,164,171), RGB(222,240,233),
    RGB(49,64,71), RGB(241,245,246), RGB(44,60,67), RGB(247,250,249), RGB(180,191,197), RGB(172,61,57)};
constexpr Palette dark_palette{
    RGB(18,22,26), RGB(26,32,37), RGB(235,240,243), RGB(160,174,184),
    RGB(52,64,72), RGB(100,202,174), RGB(29,53,46),
    RGB(100,202,174), RGB(13,33,26), RGB(33,40,45), RGB(113,128,139), RGB(36,66,56),
    RGB(81,180,154), RGB(42,52,59), RGB(122,214,190), RGB(36,44,50), RGB(99,116,128), RGB(242,139,130)};

enum class StatusTone { Neutral, Accent, Error };
constexpr int canvas_width = 760;
constexpr int activity_log_height = (69 * 115 + 50) / 100;
constexpr int activity_box_padding = 8;
constexpr int activity_height_increase = activity_log_height - 69 + activity_box_padding;
constexpr int canvas_height = 560 + activity_height_increase;
constexpr int app_icon = 101;

struct JobResult
{
    bool success = false;
    bool cancelled = false;
    std::wstring message;
};

struct WindowState
{
    HWND window = nullptr;
    HWND theme_control = nullptr;
    HWND theme_tooltip = nullptr;
    HWND export_control = nullptr;
    HWND build_control = nullptr;
    HWND full_control = nullptr;
    HWND partial_control = nullptr;
    HWND cancel_control = nullptr;
    HWND status = nullptr;
    HWND progress = nullptr;
    HWND log = nullptr;
    HFONT font = nullptr;
    HFONT title_font = nullptr;
    HFONT strong_font = nullptr;
    HFONT small_font = nullptr;
    HFONT mono_font = nullptr;
    Palette colors = light_palette;
    HBRUSH background_brush = CreateSolidBrush(colors.background);
    HBRUSH surface_brush = CreateSolidBrush(colors.surface);
    std::vector<HWND> surface_labels;
    std::vector<HWND> muted_labels;
    bool partial = false;
    int hovered_control = 0;
    StatusTone status_tone = StatusTone::Neutral;
    bool dark = false;
    bool remember_theme = true;
    int dpi = 96;
    bool busy = false;
    bool closing = false;
    std::atomic_bool cancelled{false};
    std::thread worker;
    fs::path output_path;
    Image moon_icon;
};

int scale(const WindowState& state, int value)
{
    return MulDiv(value, state.dpi, 96);
}

bool saved_dark_mode()
{
    DWORD dark = 0, size = sizeof(dark);
    return RegGetValueW(HKEY_CURRENT_USER, theme_settings, L"DarkMode", RRF_RT_REG_DWORD,
                         nullptr, &dark, &size) == ERROR_SUCCESS && dark != 0;
}

COLORREF status_color(const WindowState& state)
{
    return state.status_tone == StatusTone::Accent ? state.colors.accent
           : state.status_tone == StatusTone::Error ? state.colors.error : state.colors.muted;
}

void apply_theme(WindowState& state, bool dark, bool remember = false)
{
    const auto colors = dark ? dark_palette : light_palette;
    const auto background_brush = CreateSolidBrush(colors.background);
    const auto surface_brush = CreateSolidBrush(colors.surface);
    if (!background_brush || !surface_brush)
    {
        DeleteObject(background_brush);
        DeleteObject(surface_brush);
        throw Error("Cannot create theme colors");
    }
    const auto old_background = state.background_brush, old_surface = state.surface_brush;
    state.dark = dark;
    state.colors = colors;
    state.moon_icon = {};
    state.background_brush = background_brush;
    state.surface_brush = surface_brush;
    DeleteObject(old_background);
    DeleteObject(old_surface);
    // Attribute 20 is supported by current Windows versions; 19 covers earlier Windows 10.
    const BOOL enabled = dark;
    if (FAILED(DwmSetWindowAttribute(state.window, 20, &enabled, sizeof(enabled))))
        DwmSetWindowAttribute(state.window, 19, &enabled, sizeof(enabled));
    SetWindowTheme(state.log, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SetWindowTheme(state.progress, L"", L"");
    SendMessageW(state.progress, PBM_SETBARCOLOR, 0, colors.accent);
    SendMessageW(state.progress, PBM_SETBKCOLOR, 0, colors.border);
    const auto label = dark ? L"Switch to light mode" : L"Switch to dark mode";
    SetWindowTextW(state.theme_control, label);
    if (state.theme_tooltip)
    {
        TOOLINFOW tool{sizeof(tool)};
        tool.hwnd = state.window;
        tool.uId = reinterpret_cast<UINT_PTR>(state.theme_control);
        tool.lpszText = const_cast<wchar_t*>(label);
        SendMessageW(state.theme_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        SetWindowTheme(state.theme_tooltip, L"", L"");
        SendMessageW(state.theme_tooltip, TTM_SETTIPBKCOLOR, colors.surface, 0);
        SendMessageW(state.theme_tooltip, TTM_SETTIPTEXTCOLOR, colors.ink, 0);
    }
    RedrawWindow(state.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    if (remember)
    {
        const DWORD value = dark;
        RegSetKeyValueW(HKEY_CURRENT_USER, theme_settings, L"DarkMode", REG_DWORD, &value, sizeof(value));
    }
}

void rounded_box(HDC dc, const RECT& rect, COLORREF fill, COLORREF outline, int radius)
{
    const auto brush = CreateSolidBrush(fill);
    const auto pen = CreatePen(PS_SOLID, 1, outline);
    const auto old_brush = SelectObject(dc, brush);
    const auto old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void draw_text(HDC dc, HFONT font, COLORREF color, const wchar_t* text, RECT rect,
               UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER)
{
    const auto old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, flags);
    SelectObject(dc, old_font);
}

void select_mode(WindowState& state, bool partial)
{
    state.partial = partial;
    SendMessageW(state.partial_control, BM_SETCHECK, partial ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.full_control, BM_SETCHECK, partial ? BST_UNCHECKED : BST_CHECKED, 0);
    InvalidateRect(state.partial_control, nullptr, TRUE);
    InvalidateRect(state.full_control, nullptr, TRUE);
}

LRESULT CALLBACK button_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                              UINT_PTR, DWORD_PTR data)
{
    auto& state = *reinterpret_cast<WindowState*>(data);
    const auto id = GetDlgCtrlID(window);
    if (message == WM_GETDLGCODE && (id == partial_mode || id == full_mode))
        return DefSubclassProc(window, message, wparam, lparam) | DLGC_WANTARROWS;
    if (message == WM_MOUSEMOVE && IsWindowEnabled(window) && state.hovered_control != id)
    {
        state.hovered_control = id;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, TRUE);
    }
    else if (message == WM_MOUSELEAVE)
    {
        if (state.hovered_control == id) state.hovered_control = 0;
        InvalidateRect(window, nullptr, TRUE);
    }
    else if (message == WM_SETCURSOR && IsWindowEnabled(window))
    {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    else if (message == WM_KEYDOWN && (id == partial_mode || id == full_mode) &&
             (wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_UP || wparam == VK_DOWN))
    {
        const bool partial = wparam == VK_LEFT || wparam == VK_UP;
        select_mode(state, partial);
        SetFocus(partial ? state.partial_control : state.full_control);
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void set_status(WindowState& state, const wchar_t* text, StatusTone tone)
{
    state.status_tone = tone;
    SetWindowTextW(state.status, text);
    InvalidateRect(state.status, nullptr, TRUE);
}

std::wstring wide(const std::string& text)
{
    try
    {
        const auto converted = utf16(text);
        return {converted.begin(), converted.end()};
    }
    catch (...)
    {
        return {text.begin(), text.end()};
    }
}

void append_log(WindowState& state, const std::wstring& message)
{
    const auto line = message + L"\r\n";
    SendMessageW(state.log, EM_SETSEL, WPARAM(-1), LPARAM(-1));
    SendMessageW(state.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(state.log, EM_SCROLLCARET, 0, 0);
}

std::optional<fs::path> select_folder(HWND owner, bool compile, bool scenario_only)
{
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        throw Error("Cannot open the Windows folder picker");
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(compile ? L"Select a project folder" : L"Select a game folder");
    dialog->SetOkButtonLabel(compile ? L"Compile project"
                                    : scenario_only ? L"Decompile Partial" : L"Decompile Full");
    const auto result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        dialog->Release();
        return std::nullopt;
    }
    IShellItem* item = nullptr;
    if (FAILED(result) || FAILED(dialog->GetResult(&item)))
    {
        dialog->Release();
        throw Error("Cannot read the selected folder");
    }
    PWSTR path = nullptr;
    const auto read_result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    std::optional<fs::path> selected;
    if (SUCCEEDED(read_result))
    {
        selected = fs::path(path);
    }
    CoTaskMemFree(path);
    item->Release();
    dialog->Release();
    if (!selected)
    {
        throw Error("Select a local filesystem folder");
    }
    return selected;
}

void set_busy(WindowState& state, bool busy)
{
    state.busy = busy;
    EnableWindow(state.export_control, !busy);
    EnableWindow(state.build_control, !busy);
    EnableWindow(state.full_control, !busy);
    EnableWindow(state.partial_control, !busy);
    EnableWindow(state.cancel_control, busy);
    SendMessageW(state.progress, PBM_SETMARQUEE, busy, 30);
    ShowWindow(state.cancel_control, busy ? SW_SHOW : SW_HIDE);
    ShowWindow(state.progress, busy ? SW_SHOW : SW_HIDE);
    SetWindowPos(state.status, nullptr, scale(state, 250), scale(state, 386),
                  scale(state, busy ? 370 : 478), scale(state, 24), SWP_NOZORDER | SWP_NOACTIVATE);
}

void start_job(WindowState& state, bool compile)
{
    bool scenario_only = state.partial;
    const auto selected = select_folder(state.window, compile, scenario_only);
    if (!selected)
    {
        return;
    }
    if (compile)
    {
        const auto manifest = Json::parse(read_text(*selected / "project.json"));
        scenario_only = manifest.value("mode", "full") == "scenario";
        select_mode(state, scenario_only);
    }
    state.output_path = (compile ? selected->parent_path() : *selected) /
                        (compile ? L".build" : L".project");
    SetWindowTextW(state.log, L"");
    set_status(state, compile ? L"Compiling project..."
                              : scenario_only ? L"Decompiling Partial..." : L"Decompiling Full...", StatusTone::Accent);
    append_log(state, L"Source: " + selected->wstring());
    append_log(state, L"Output: " + state.output_path.wstring());
    state.cancelled.store(false);
    set_busy(state, true);
    const auto input_path = *selected;
    const auto output_path = state.output_path;
    state.worker = std::thread(
        [&state, compile, scenario_only, input_path, output_path]
        {
            set_job_callbacks({[&state](const std::string& message)
                               {
                                   auto text = std::make_unique<std::wstring>(wide(message));
                                   if (PostMessageW(state.window,
                                                    progress_message,
                                                    0,
                                                    reinterpret_cast<LPARAM>(text.get())))
                                   {
                                       text.release();
                                   }
                               },
                               [&state]
                               {
                                   return state.cancelled.load();
                               }});
            auto result = std::make_unique<JobResult>();
            try
            {
                if (compile)
                {
                    const auto report = build_project(input_path, output_path);
                    result->message = report.value("mode", "full") == "scenario"
                        ? L"Compile complete. Copy all DAT files from .build into your game folder."
                        : L"Compile complete. Your DAT files are ready in .build.";
                }
                else
                {
                    const auto report = scenario_only ? export_scenario_project(input_path, output_path)
                                                      : export_project(input_path, output_path, true);
                    const auto diagnostics = report.at("diagnostics").size();
                    result->message =
                        L"Exported " + std::to_wstring(report.at("scenarios").get<size_t>()) +
                        L" scenarios" + (scenario_only ? std::wstring(L" and config.json.")
                            : L", " + std::to_wstring(report.at("images").get<size_t>()) +
                              L" images and " + std::to_wstring(report.at("audio").get<size_t>()) + L" audio files.");
                    if (diagnostics)
                    {
                        result->message +=
                            L"\r\n" + std::to_wstring(diagnostics) +
                            L" items preserved as raw data. See export-report.json for details.";
                    }
                    if (scenario_only)
                        result->message += L"\r\nEdit scenarios and config.json, then choose Compile project.";
                }
                result->success = true;
            }
            catch (const Cancelled&)
            {
                result->cancelled = true;
                result->message = L"Cancelled. Incomplete output files may remain.";
            }
            catch (const std::exception& error)
            {
                result->message = L"Could not complete: " + wide(error.what());
            }
            set_job_callbacks({});
            if (PostMessageW(
                    state.window, finished_message, 0, reinterpret_cast<LPARAM>(result.get())))
            {
                result.release();
            }
        });
}

HWND control(WindowState& state,
             const wchar_t* type,
             const wchar_t* text,
             DWORD style,
             int x,
             int y,
             int width,
             int height,
             int id = 0)
{
    const auto window = CreateWindowExW(0,
                                        type,
                                        text,
                                        WS_CHILD | WS_VISIBLE | style,
                                        MulDiv(x, state.dpi, 96),
                                        MulDiv(y, state.dpi, 96),
                                        MulDiv(width, state.dpi, 96),
                                        MulDiv(height, state.dpi, 96),
                                        state.window,
                                        reinterpret_cast<HMENU>(INT_PTR(id)),
                                        GetModuleHandleW(nullptr),
                                        nullptr);
    if (!window)
    {
        throw Error("Cannot create a window control");
    }
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    if (std::wstring_view(type) == L"BUTTON")
        SetWindowSubclass(window, button_proc, 1, reinterpret_cast<DWORD_PTR>(&state));
    return window;
}

HFONT make_font(const WindowState& state, int size, int weight = FW_NORMAL,
                const wchar_t* face = L"Segoe UI")
{
    return CreateFontW(-scale(state, size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

void create_controls(WindowState& state)
{
    state.font = make_font(state, 14);
    state.title_font = make_font(state, 28, FW_SEMIBOLD);
    state.strong_font = make_font(state, 15, FW_SEMIBOLD);
    state.small_font = make_font(state, 12);
    state.mono_font = make_font(state, 12, FW_NORMAL, L"Consolas");
    auto label = [&](const wchar_t* text, int x, int y, int width, int height,
                     HFONT font, bool subtle = false, bool on_surface = false)
    {
        const auto item = control(state, L"STATIC", text, SS_LEFT | SS_CENTERIMAGE,
                                  x, y, width, height);
        SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (subtle) state.muted_labels.push_back(item);
        if (on_surface) state.surface_labels.push_back(item);
        return item;
    };
    state.theme_control = control(state, L"BUTTON", L"Switch to dark mode", WS_TABSTOP | BS_OWNERDRAW,
                                   32, 31, 42, 42, theme_button);
    state.theme_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                          WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                          CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                          state.window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (state.theme_tooltip)
    {
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = state.window;
        tool.uId = reinterpret_cast<UINT_PTR>(state.theme_control);
        tool.lpszText = const_cast<wchar_t*>(L"Switch to dark mode");
        SendMessageW(state.theme_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    label(L"Nyx", 84, 27, 500, 38, state.title_font);
    label(L"Modding Tool for Cyberworks Game Engine", 85, 68, 600, 22, state.font, true);
    label(L"Mode", 48, 134, 400, 24, state.strong_font, false, true);
    state.partial_control = control(state, L"BUTTON", L"Partial (Config, scenario)",
                                    WS_TABSTOP | WS_GROUP | BS_OWNERDRAW,
                                    48, 172, 326, 78, partial_mode);
    state.full_control = control(state, L"BUTTON", L"Full (Config, scenario, media)",
                                 WS_TABSTOP | BS_OWNERDRAW,
                                 386, 172, 326, 78, full_mode);
    select_mode(state, false);
    state.export_control = control(state, L"BUTTON", L"Decompile game",
                                    WS_TABSTOP | WS_GROUP | BS_OWNERDRAW,
                                    48, 270, 326, 44, export_button);
    state.build_control = control(state, L"BUTTON", L"Compile project", WS_TABSTOP | BS_OWNERDRAW,
                                   386, 270, 326, 44, build_button);
    label(L"Compile automatically follows your project's export mode.",
          48, 328, 664, 20, state.small_font, true, true);
    label(L"Activity", 32, 386, 160, 24, state.strong_font);
    state.status = control(state, L"STATIC", L"Ready", SS_RIGHT | SS_CENTERIMAGE,
                            250, 386, 478, 24);
    state.cancel_control = control(state, L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW,
                                    636, 381, 92, 32, cancel_button);
    state.progress = control(state, PROGRESS_CLASSW, L"", PBS_MARQUEE, 32, 421, 696, 3);
    SendMessageW(state.progress, PBM_SETBARCOLOR, 0, state.colors.accent);
    SendMessageW(state.progress, PBM_SETBKCOLOR, 0, state.colors.border);
    state.log = control(state, L"EDIT", L"",
                         ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
                         32, 437, 696, activity_log_height);
    SendMessageW(state.log, WM_SETFONT, reinterpret_cast<WPARAM>(state.mono_font), TRUE);
    SendMessageW(state.log, EM_SETLIMITTEXT, 1024 * 1024, 0);
    const auto footer = control(state, L"STATIC", L"Developed by Leticiel", SS_CENTER | SS_CENTERIMAGE,
                                32, 528 + activity_height_increase, 696, 20);
    SendMessageW(footer, WM_SETFONT, reinterpret_cast<WPARAM>(state.small_font), TRUE);
    state.muted_labels.push_back(footer);
    set_busy(state, false);
}

void paint_moon(WindowState& state, HDC dc, int left, int top)
{
    const int width = scale(state, 30), height = scale(state, 28);
    auto& icon = state.moon_icon;
    if (icon.width != uint32_t(width) || icon.height != uint32_t(height))
    {
        icon = {uint32_t(width), uint32_t(height), Bytes(size_t(width) * height * 4, 255)};
        // Sample the curved silhouette at 8x resolution, then blend each pixel
        // against the primary button colors. Cache for the current DPI and theme.
        constexpr int samples = 8;
        for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            int covered = 0;
            for (int sy = 0; sy < samples; ++sy)
            for (int sx = 0; sx < samples; ++sx)
            {
                const double px = 40 + (x + (sx + 0.5) / samples) * 96.0 / state.dpi;
                const double py = 38 + (y + (sy + 0.5) / samples) * 96.0 / state.dpi;
                const double outer = (px - 53) * (px - 53) + (py - 52) * (py - 52);
                const double inner = (px - 59) * (px - 59) + (py - 46) * (py - 46);
                if (outer <= 121 && inner >= 100) ++covered;
            }
            const auto blend = [&](int background, int foreground)
            {
                return uint8_t((background * (64 - covered) + foreground * covered + 32) / 64);
            };
            const size_t at = (size_t(y) * width + x) * 4;
            icon.bgra[at] = blend(GetBValue(state.colors.primary), GetBValue(state.colors.primary_text));
            icon.bgra[at + 1] = blend(GetGValue(state.colors.primary), GetGValue(state.colors.primary_text));
            icon.bgra[at + 2] = blend(GetRValue(state.colors.primary), GetRValue(state.colors.primary_text));
        }
    }
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = width;
    bitmap.bmiHeader.biHeight = -height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, left, top, width, height, 0, 0, 0, height,
                      icon.bgra.data(), &bitmap, DIB_RGB_COLORS);
}

void paint_background(WindowState& state, HDC dc)
{
    const auto& colors = state.colors;
    RECT client;
    GetClientRect(state.window, &client);
    FillRect(dc, &client, state.background_brush);
    const auto rect = [&](int x, int y, int width, int height)
    {
        return RECT{scale(state, x), scale(state, y), scale(state, x + width), scale(state, y + height)};
    };
    rounded_box(dc, rect(24, 112, 712, 256), colors.surface, colors.border, scale(state, 18));
    rounded_box(dc, rect(24, 437 - activity_box_padding, 712,
                         activity_log_height + 2 * activity_box_padding),
                colors.surface, colors.border, scale(state, 18));
    RECT line = rect(32, 517 + activity_height_increase, 696, 1);
    const auto brush = CreateSolidBrush(colors.border);
    FillRect(dc, &line, brush);
    DeleteObject(brush);
}

void paint_radio(const WindowState& state, HDC dc, int x, int y,
                  COLORREF background, COLORREF foreground, bool selected)
{
    const double dpi_scale = state.dpi / 96.0;
    const double outer = 8 * dpi_scale, inner = outer - dpi_scale, dot = 4 * dpi_scale;
    const int half = int(std::ceil(outer)) + 1, side = half * 2;
    Bytes pixels(size_t(side) * side * 4, 255);
    constexpr int samples = 16;
    for (int py = 0; py < side; ++py)
    for (int px = 0; px < side; ++px)
    {
        int covered = 0;
        for (int sy = 0; sy < samples; ++sy)
        for (int sx = 0; sx < samples; ++sx)
        {
            const double dx = px - half + (sx + 0.5) / samples;
            const double dy = py - half + (sy + 0.5) / samples;
            const double distance = dx * dx + dy * dy;
            if ((distance <= outer * outer && distance >= inner * inner) ||
                (selected && distance <= dot * dot)) ++covered;
        }
        const auto blend = [&](int bg, int fg)
        {
            constexpr int count = samples * samples;
            return uint8_t((bg * (count - covered) + fg * covered + count / 2) / count);
        };
        const size_t at = (size_t(py) * side + px) * 4;
        pixels[at] = blend(GetBValue(background), GetBValue(foreground));
        pixels[at + 1] = blend(GetGValue(background), GetGValue(foreground));
        pixels[at + 2] = blend(GetRValue(background), GetRValue(foreground));
    }
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = side;
    bitmap.bmiHeader.biHeight = -side;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, x - half, y - half, side, side, 0, 0, 0, side,
                      pixels.data(), &bitmap, DIB_RGB_COLORS);
}

void paint_button(WindowState& state, const DRAWITEMSTRUCT& item)
{
    const auto& colors = state.colors;
    const bool mode = item.CtlID == partial_mode || item.CtlID == full_mode;
    const bool chosen = mode && ((item.CtlID == partial_mode) == state.partial);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0;
    const bool primary = item.CtlID == export_button;
    const bool hovered = !disabled && state.hovered_control == int(item.CtlID);
    if (item.CtlID == theme_button)
    {
        FillRect(item.hDC, &item.rcItem, state.background_brush);
        rounded_box(item.hDC, item.rcItem, colors.primary,
                     focused || pressed ? colors.primary_text : hovered ? colors.accent : colors.primary, scale(state, 10));
        paint_moon(state, item.hDC, item.rcItem.left + scale(state, 8), item.rcItem.top + scale(state, 7));
        return;
    }
    auto fill = chosen ? colors.selection : primary ? colors.primary : colors.surface;
    if (disabled) fill = colors.disabled;
    else if (pressed) fill = chosen ? colors.pressed_selection : primary ? colors.pressed_primary : colors.pressed_surface;
    else if (hovered && !chosen) fill = primary ? colors.hovered_primary : colors.hovered_surface;
    const auto text_color = disabled ? colors.disabled_text : primary ? colors.primary_text : colors.ink;
    RECT rect = item.rcItem;
    FillRect(item.hDC, &rect, item.CtlID == cancel_button ? state.background_brush : state.surface_brush);
    InflateRect(&rect, -1, -1);
    rounded_box(item.hDC, rect, fill, chosen || focused || hovered ? colors.accent : primary && !disabled ? colors.primary : colors.border,
                 scale(state, 10));
    if (mode)
    {
        RECT heading{rect.left + scale(state, 18), rect.top + scale(state, 12),
                     rect.right - scale(state, 52), rect.top + scale(state, 36)};
        draw_text(item.hDC, state.strong_font, disabled ? colors.muted : colors.ink,
                  item.CtlID == partial_mode ? L"Partial" : L"Full", heading);
        RECT detail = heading;
        detail.top += scale(state, 27); detail.bottom += scale(state, 25);
        draw_text(item.hDC, state.font, colors.muted,
                  item.CtlID == partial_mode ? L"Config, scenario" : L"Config, scenario, media", detail);
        const int x = rect.right - scale(state, 28), y = (rect.top + rect.bottom) / 2;
        paint_radio(state, item.hDC, x, y, fill, chosen ? colors.accent : colors.radio_border, chosen);
    }
    else
    {
        wchar_t text[128]{};
        GetWindowTextW(item.hwndItem, text, 128);
        draw_text(item.hDC, state.font, text_color, text, rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    if (focused)
    {
        RECT focus = rect;
        InflateRect(&focus, -scale(state, 5), -scale(state, 5));
        DrawFocusRect(item.hDC, &focus);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        state = static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
    {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT paint;
        const auto dc = BeginPaint(window, &paint);
        paint_background(*state, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_background(*state, reinterpret_cast<HDC>(wparam));
        return 0;
    case WM_ERASEBKGND:
        paint_background(*state, reinterpret_cast<HDC>(wparam));
        return 1;
    case WM_DRAWITEM:
        paint_button(*state, *reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    {
        const auto dc = reinterpret_cast<HDC>(wparam);
        const auto child = reinterpret_cast<HWND>(lparam);
        const bool on_surface = child == state->log ||
            std::find(state->surface_labels.begin(), state->surface_labels.end(), child) != state->surface_labels.end();
        const bool subtle = child == state->log ||
            std::find(state->muted_labels.begin(), state->muted_labels.end(), child) != state->muted_labels.end();
        SetTextColor(dc, child == state->status ? status_color(*state) : subtle ? state->colors.muted : state->colors.ink);
        SetBkColor(dc, on_surface ? state->colors.surface : state->colors.background);
        return reinterpret_cast<LRESULT>(on_surface ? state->surface_brush : state->background_brush);
    }
    case WM_COMMAND:
        try
        {
            const auto command = LOWORD(wparam);
            if (command == theme_button && HIWORD(wparam) == BN_CLICKED)
            {
                apply_theme(*state, !state->dark, state->remember_theme);
            }
            else if ((command == export_button || command == build_button) && !state->busy)
            {
                start_job(*state, command == build_button);
            }
            else if ((command == partial_mode || command == full_mode) && !state->busy)
            {
                select_mode(*state, command == partial_mode);
            }
            else if (command == cancel_button && state->busy)
            {
                state->cancelled.store(true);
                set_status(*state, L"Cancelling...", StatusTone::Neutral);
                EnableWindow(state->cancel_control, FALSE);
            }
        }
        catch (const std::exception& error)
        {
            append_log(*state, wide(error.what()));
            set_status(*state, L"Could not start. See activity for details.", StatusTone::Error);
            if (!state->worker.joinable())
            {
                set_busy(*state, false);
            }
        }
        return 0;
    case progress_message:
    {
        const std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lparam));
        append_log(*state, *text);
        return 0;
    }
    case finished_message:
    {
        const std::unique_ptr<JobResult> result(reinterpret_cast<JobResult*>(lparam));
        state->worker.join();
        set_busy(*state, false);
        set_status(*state, result->success ? L"Complete" : result->cancelled ? L"Cancelled" : L"Failed",
                    result->success ? StatusTone::Accent : result->cancelled ? StatusTone::Neutral : StatusTone::Error);
        append_log(*state, result->message);
        if (state->closing)
        {
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_CLOSE:
        if (state->busy)
        {
            state->closing = true;
            state->cancelled.store(true);
            EnableWindow(state->cancel_control, FALSE);
            set_status(*state, L"Stopping before closing...", StatusTone::Neutral);
        }
        else
        {
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}
}

namespace
{
void verify_theme_toggle(WindowState& state)
{
    state.remember_theme = false;
    apply_theme(state, false);
    select_mode(state, true);
    SetWindowTextW(state.log, L"Existing activity\r\n");
    set_status(state, L"Decompiling Partial...", StatusTone::Accent);
    set_busy(state, true);
    for (const bool expected : {true, false})
    {
        SendMessageW(state.theme_control, BM_CLICK, 0, 0);
        wchar_t text[128]{};
        GetWindowTextW(state.log, text, 128);
        if (state.dark != expected || !state.busy || !state.partial ||
            !IsWindowEnabled(state.theme_control) || IsWindowEnabled(state.export_control) ||
            std::wstring_view(text) != L"Existing activity\r\n")
            throw Error("Theme switching changed the current job, mode or activity");
        GetWindowTextW(state.status, text, 128);
        if (std::wstring_view(text) != L"Decompiling Partial..." || status_color(state) != state.colors.accent)
            throw Error("Theme switching changed the job status");
        GetWindowTextW(state.theme_control, text, 128);
        if (std::wstring_view(text) != (expected ? L"Switch to light mode" : L"Switch to dark mode"))
            throw Error("Theme button has an incorrect accessible label");
        LOGBRUSH brush{};
        if (!GetObjectW(state.surface_brush, sizeof(brush), &brush) || brush.lbColor != state.colors.surface)
            throw Error("Activity surface did not switch theme");
    }
    set_busy(state, false);
}

void render_preview(WindowState& state, const fs::path& path)
{
    RECT client;
    GetClientRect(state.window, &client);
    const auto width = client.right, height = client.bottom;
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = width;
    bitmap.bmiHeader.biHeight = -height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const auto dc = CreateCompatibleDC(nullptr);
    const auto dib = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !dib) throw Error("Cannot create preview bitmap");
    const auto previous = SelectObject(dc, dib);
    paint_background(state, dc);
    for (auto child = GetWindow(state.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT rect;
        GetWindowRect(child, &rect);
        MapWindowPoints(nullptr, state.window, reinterpret_cast<POINT*>(&rect), 2);
        const int saved = SaveDC(dc);
        SetViewportOrgEx(dc, rect.left, rect.top, nullptr);
        IntersectClipRect(dc, 0, 0, rect.right - rect.left, rect.bottom - rect.top);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
        RestoreDC(dc, saved);
    }
    GdiFlush();
    Image image{uint32_t(width), uint32_t(height), Bytes(size_t(width) * height * 4)};
    std::copy_n(static_cast<const uint8_t*>(pixels), image.bgra.size(), image.bgra.begin());
    for (size_t at = 3; at < image.bgra.size(); at += 4) image.bgra[at] = 255;
    SelectObject(dc, previous);
    DeleteObject(dib);
    DeleteDC(dc);
    write_file(path, encode_png(image));
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int show)
{
    const bool smoke_test = std::wstring_view(arguments) == L"--smoke-test";
    const bool preview_request = std::wstring_view(arguments).starts_with(L"--render-preview");
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
    {
        return 1;
    }
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    WindowState state;
    const auto screen = GetDC(nullptr);
    state.dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(app_icon), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    window_class.hIconSm = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(app_icon), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!window_class.hIcon || !window_class.hIconSm)
    {
        DeleteObject(state.background_brush);
        DeleteObject(state.surface_brush);
        CoUninitialize();
        return 1;
    }
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = L"CsystemModToolkit";
    RegisterClassExW(&window_class);
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT bounds{0, 0, scale(state, canvas_width), scale(state, canvas_height)};
    AdjustWindowRect(&bounds, style, FALSE);
    const auto window = CreateWindowExW(0,
                                        window_class.lpszClassName,
                                        L"Nyx",
                                        style,
                                        CW_USEDEFAULT,
                                        CW_USEDEFAULT,
                                        bounds.right - bounds.left,
                                        bounds.bottom - bounds.top,
                                        nullptr,
                                        nullptr,
                                        instance,
                                        &state);
    int exit_code = 0;
    try
    {
        if (!window)
        {
            throw Error("Cannot create the application window");
        }
        create_controls(state);
        int argument_count = 0;
        const auto argument_list = CommandLineToArgvW(GetCommandLineW(), &argument_count);
        const bool preview = argument_list && argument_count >= 3 &&
                              std::wstring_view(argument_list[1]) == L"--render-preview";
        state.remember_theme = !preview && !smoke_test;
        bool dark = state.remember_theme && saved_dark_mode();
        if (preview)
            for (int i = 3; i < argument_count; ++i)
            {
                const std::wstring_view option(argument_list[i]);
                if (option == L"--partial") select_mode(state, true);
                else if (option == L"--dark") dark = true;
                else if (option == L"--light") dark = false;
            }
        apply_theme(state, dark);
        if (preview)
        {
            render_preview(state, argument_list[2]);
        }
        if (argument_list) LocalFree(argument_list);
        if (smoke_test) verify_theme_toggle(state);
        if (preview || smoke_test)
        {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        else
        {
            ShowWindow(window, show);
        }
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(window, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    catch (const std::exception& error)
    {
        if (smoke_test || preview_request)
            OutputDebugStringW(wide(error.what()).c_str());
        else
            MessageBoxW(nullptr, wide(error.what()).c_str(), L"Nyx", MB_OK | MB_ICONERROR);
        exit_code = 1;
    }
    if (IsWindow(window)) DestroyWindow(window);
    DeleteObject(state.font);
    DeleteObject(state.title_font);
    DeleteObject(state.strong_font);
    DeleteObject(state.small_font);
    DeleteObject(state.mono_font);
    DeleteObject(state.background_brush);
    DeleteObject(state.surface_brush);
    CoUninitialize();
    return exit_code;
}
