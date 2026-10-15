#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string_view>

#if !defined(_M_X64)
#error XTray supports Windows x64 only.
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"XTray.MainWindow";
constexpr int kInitialClientWidth = 800;
constexpr int kInitialClientHeight = 600;
constexpr int kMinimumClientWidth = 640;
constexpr int kMinimumClientHeight = 400;
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;

int ReportStartupError(bool smokeTest, const wchar_t* operation)
{
    const DWORD error = GetLastError();
    if (smokeTest) {
        std::fwprintf(stderr, L"%ls failed (Win32 error %lu).\n", operation, error);
    } else {
        MessageBoxW(nullptr, operation, L"XTray", MB_OK | MB_ICONERROR);
    }
    return 1;
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(window);
        RECT bounds{
            0,
            0,
            MulDiv(kMinimumClientWidth, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
            MulDiv(kMinimumClientHeight, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)
        };
        if (!AdjustWindowRectExForDpi(&bounds, kWindowStyle, FALSE, 0, dpi)) {
            break;
        }

        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize = {bounds.right - bounds.left, bounds.bottom - bounds.top};

        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
            limits->ptMinTrackSize.x = std::min(
                limits->ptMinTrackSize.x, monitor.rcWork.right - monitor.rcWork.left);
            limits->ptMinTrackSize.y = std::min(
                limits->ptMinTrackSize.y, monitor.rcWork.bottom - monitor.rcWork.top);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* bounds = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, bounds->left, bounds->top,
            bounds->right - bounds->left, bounds->bottom - bounds->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    const bool smokeTest = std::wstring_view(commandLine) == L"--smoke-test";

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        nullptr, IDI_APPLICATION, IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = static_cast<HCURSOR>(LoadImageW(
        nullptr, IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    windowClass.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&windowClass)) {
        return ReportStartupError(smokeTest, L"RegisterClassExW");
    }

    const UINT dpi = GetDpiForSystem();
    RECT bounds{
        0,
        0,
        MulDiv(kInitialClientWidth, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
        MulDiv(kInitialClientHeight, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)
    };
    if (!AdjustWindowRectExForDpi(&bounds, kWindowStyle, FALSE, 0, dpi)) {
        return ReportStartupError(smokeTest, L"AdjustWindowRectExForDpi");
    }

    LONG width = bounds.right - bounds.left;
    LONG height = bounds.bottom - bounds.top;
    RECT workArea{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        width = std::min(width, workArea.right - workArea.left);
        height = std::min(height, workArea.bottom - workArea.top);
    }

    const HWND window = CreateWindowExW(
        0, kWindowClass, L"XTray", kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        width, height, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        return ReportStartupError(smokeTest, L"CreateWindowExW");
    }

    if (smokeTest) {
        // Exercise real window creation and the message loop without displaying a CI window.
        if (!PostMessageW(window, WM_CLOSE, 0, 0)) {
            const int result = ReportStartupError(true, L"PostMessageW");
            DestroyWindow(window);
            return result;
        }
    } else {
        ShowWindow(window, showCommand);
        UpdateWindow(window);
    }

    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return result == -1 ? ReportStartupError(smokeTest, L"GetMessageW")
                        : static_cast<int>(message.wParam);
}
