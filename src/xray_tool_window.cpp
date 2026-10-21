#include "xray_tool_window.h"
#include "ui_layout.h"
#include "resource.h"
#include "svg_icons.h"
#include "text_view.h"

#include <richedit.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>

namespace xtray {
namespace {

constexpr wchar_t kToolClass[] = L"XTray.XrayTool";
constexpr int kOutputId = 4101, kCopyId = 4102, kStatusId = 4103, kWordWrapId = 4104;
constexpr DWORD kToolStyle = WS_OVERLAPPEDWINDOW;

std::wstring DisplayOutput(std::string_view bytes)
{
    // Convert only after the command completes, so split UTF-8 sequences remain intact.
    const auto text = FromUtf8(bytes);
    std::wstring result;
    result.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\n' && (index == 0 || text[index - 1] != L'\r')) {
            result += L'\r';
        }
        result += text[index];
    }
    return result;
}

struct TextStream {
    std::wstring_view remaining;
    static DWORD CALLBACK Read(DWORD_PTR cookie, LPBYTE buffer, LONG count, LONG* copied)
    {
        auto& stream = *reinterpret_cast<TextStream*>(cookie);
        const auto characters = (std::min)(stream.remaining.size(), static_cast<size_t>(count) / sizeof(wchar_t));
        const auto bytes = characters * sizeof(wchar_t);
        std::memcpy(buffer, stream.remaining.data(), bytes);
        stream.remaining.remove_prefix(characters);
        *copied = static_cast<LONG>(bytes);
        return 0;
    }
};

} // namespace

XrayToolWindow::~XrayToolWindow() { Close(); }

const wchar_t* XrayToolWindow::Title() const
{
    return kind_ == XrayToolKind::Check ? Text(L"Check Xray configuration", L"检查 Xray 配置")
        : kind_ == XrayToolKind::Preview ? Text(L"Preview Xray configuration", L"预览 Xray 配置")
        : Text(L"Show Xray version", L"显示 Xray 版本");
}

std::wstring XrayToolWindow::Status() const
{
    if (!result_) {
        return kind_ == XrayToolKind::Check ? Text(L"Checking configuration...", L"正在检查配置…")
            : Text(L"Reading Xray output...", L"正在读取 Xray 输出…");
    }
    if (result_->cancelled) { return Text(L"Cancelled.", L"已取消。"); }
    if (!result_->error.empty()) { return Text(L"The command could not be completed. See details below.", L"命令未能完成，详情见下方。"); }
    const bool success = result_->exitCode == 0;
    const auto text = kind_ == XrayToolKind::Check
        ? success ? Text(L"Configuration check passed", L"配置检查通过")
            : Text(L"Configuration check failed", L"配置检查失败")
        : success ? Text(L"Completed", L"已完成") : Text(L"Command failed", L"命令执行失败");
    return std::wstring(text) + Text(L" (exit code ", L"（退出码 ") +
        (result_->exitCode ? std::to_wstring(*result_->exitCode) : L"?") + Text(L").", L"）。");
}

void XrayToolWindow::Open(HWND owner, XrayToolKind kind, const Settings& settings,
    bool settingsValid, bool dark, bool quiet)
{
    if (window_ != nullptr && task_.Running()) {
        if (!quiet) {
            ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(window_);
        }
        return;
    }
    owner_ = owner;
    quiet_ = quiet;
    kind_ = kind;
    dark_ = dark;
    result_.reset();
    document_.clear();
    error_ = nullptr;
    if (window_ == nullptr) {
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.lpfnWndProc = Procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.lpszClassName = kToolClass;
        if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error(ToUtf8(WindowsError(Text(L"Cannot register the result window", L"注册结果窗口失败"))));
        }
        const UINT dpi = GetDpiForWindow(owner);
        RECT bounds{0, 0, MulDiv(720, static_cast<int>(dpi), 96), MulDiv(480, static_cast<int>(dpi), 96)};
        if (!AdjustWindowRectExForDpi(&bounds, kToolStyle, FALSE, WS_EX_CONTROLPARENT, dpi)) {
            throw std::runtime_error("Cannot calculate the Xray result window size.");
        }
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT parent{};
        GetWindowRect(owner, &parent);
        const LONG width = (std::min)(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left);
        const LONG height = (std::min)(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top);
        const LONG x = std::clamp(parent.left + (parent.right - parent.left - width) / 2,
            monitor.rcWork.left, monitor.rcWork.right - width);
        const LONG y = std::clamp(parent.top + (parent.bottom - parent.top - height) / 2,
            monitor.rcWork.top, monitor.rcWork.bottom - height);
        const HWND created = CreateWindowExW(WS_EX_CONTROLPARENT, kToolClass, Title(), kToolStyle,
            x, y, width, height, owner, nullptr, type.hInstance, this);
        if (created == nullptr) {
            if (error_) { std::rethrow_exception(error_); }
            throw std::runtime_error(ToUtf8(WindowsError(Text(L"Cannot create the result window", L"创建结果窗口失败"))));
        }
    }
    RefreshLanguage();
    SetAppearance(dark);
    SetDocument();
    if (!task_.Start(kind, settings, settingsValid)) {
        throw std::runtime_error("The Xray tool is already running.");
    }
    if (SetTimer(window_, 1, 50, nullptr) == 0) {
        Close();
        throw std::runtime_error(ToUtf8(WindowsError(Text(L"Cannot start the result timer", L"启动结果计时器失败"))));
    }
    if (!quiet) {
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window_);
    }
}

void XrayToolWindow::Initialize()
{
    theme_.Attach(window_);
    const auto child = [this](const wchar_t* type, DWORD extended, DWORD style, int id) {
        const HWND control = CreateWindowExW(extended, type, L"", WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (control == nullptr) { throw std::runtime_error("Cannot create a result window control."); }
        return control;
    };
    status_ = child(L"STATIC", 0, SS_LEFT | SS_NOPREFIX, kStatusId);
    note_ = child(L"STATIC", 0, SS_LEFT | SS_NOPREFIX, 0);
    copy_ = child(L"BUTTON", 0, WS_TABSTOP | BS_ICON, kCopyId);
    wordWrapButton_ = child(L"BUTTON", 0, WS_TABSTOP | BS_ICON | BS_AUTOCHECKBOX | BS_PUSHLIKE, kWordWrapId);
    output_ = child(MSFTEDIT_CLASS, WS_EX_CLIENTEDGE,
        ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL | ES_AUTOVSCROLL | ES_NOHIDESEL | ES_DISABLENOSCROLL |
        WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, kOutputId);
    SendMessageW(output_, EM_EXLIMITTEXT, 0, (std::numeric_limits<LONG>::max)() - 1);
    SendMessageW(output_, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
    close_ = child(L"BUTTON", 0, WS_TABSTOP, IDCANCEL);
    wordWrap_ = false;
    SetTextWordWrap(output_, false);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        window_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (tooltip_ == nullptr) { throw std::runtime_error("Cannot create result button tooltips."); }
    for (const auto control : {copy_, wordWrapButton_}) {
        TOOLINFOW tool{};
        tool.cbSize = TTTOOLINFOW_V2_SIZE;
        tool.hwnd = window_;
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.uId = reinterpret_cast<UINT_PTR>(control);
        tool.lpszText = const_cast<wchar_t*>(L"");
        if (!SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
            throw std::runtime_error("Cannot register result button tooltips.");
        }
    }
    SetFonts();
}

void XrayToolWindow::SetFonts()
{
    NONCLIENTMETRICSW metrics{sizeof(NONCLIENTMETRICSW)};
    const UINT dpi = GetDpiForWindow(window_);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        throw std::runtime_error("Cannot read the system UI font.");
    }
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    wcscpy_s(metrics.lfMessageFont.lfFaceName, L"Consolas");
    metrics.lfMessageFont.lfHeight = -MulDiv(10, static_cast<int>(dpi), 72);
    HFONT outputFont = CreateFontIndirectW(&metrics.lfMessageFont);
    if (font == nullptr || outputFont == nullptr) {
        if (font != nullptr) { DeleteObject(font); }
        if (outputFont != nullptr) { DeleteObject(outputFont); }
        throw std::runtime_error("Cannot create result window fonts.");
    }
    for (const auto control : {status_, note_, copy_, wordWrapButton_, close_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    SendMessageW(output_, WM_SETFONT, reinterpret_cast<WPARAM>(outputFont), TRUE);
    if (font_ != nullptr) { DeleteObject(font_); }
    if (outputFont_ != nullptr) { DeleteObject(outputFont_); }
    font_ = font;
    outputFont_ = outputFont;
    theme_.SetFont(font_);
    struct Icons {
        std::array<HICON, 2> values{};
        ~Icons() { for (const auto icon : values) { if (icon != nullptr) { DestroyIcon(icon); } } }
    } next;
    for (size_t index = 0; index < next.values.size(); ++index) {
        const int size = UiScale(window_, index == 0 ? 16 : 32);
        next.values[index] = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_XTRAY), IMAGE_ICON, size, size, 0));
        if (next.values[index] == nullptr) {
            throw std::runtime_error("Cannot load result window icons.");
        }
    }
    next.values.swap(icons_);
    SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icons_[0]));
    SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icons_[1]));
    UpdateToolbar();
}

void XrayToolWindow::RefreshLanguage()
{
    if (window_ == nullptr) { return; }
    SetWindowTextW(window_, Title());
    SetWindowTextW(status_, Status().c_str());
    SetWindowTextW(note_, kind_ == XrayToolKind::Preview
        ? Text(L"Preview of the saved configuration sources on disk; not the running process's in-memory configuration.",
            L"预览已保存配置来源在磁盘上的当前内容，不代表运行中进程的内存配置。")
        : Text(L"Uses the saved settings. The running Xray instance is unaffected.",
            L"使用已保存设置执行，不影响正在运行的 Xray 实例。"));
    SetWindowTextW(copy_, Text(L"Copy all", L"复制全部"));
    SetWindowTextW(close_, Text(L"Close", L"关闭"));
    UpdateToolbar();
    Layout();
}

void XrayToolWindow::Layout()
{
    if (close_ == nullptr || output_ == nullptr || IsIconic(window_)) { return; }
    RECT client{};
    GetClientRect(window_, &client);
    const int margin = UiScale(window_, 16), gap = UiScale(window_, 8);
    const int width = (std::max)(1, static_cast<int>(client.right) - margin * 2);
    int y = margin;
    const int statusHeight = (std::max)(1L, ControlTextSize(status_, width).cy);
    MoveWindow(status_, margin, y, width, statusHeight, TRUE);
    y += statusHeight + gap;
    const int button = UiScale(window_, 32);
    const int noteWidth = (std::max)(1, width - button * 2 - gap * 2);
    const int noteHeight = (std::max)(1L, ControlTextSize(note_, noteWidth).cy);
    const int rowHeight = (std::max)(noteHeight, button);
    MoveWindow(note_, margin, y + (rowHeight - noteHeight) / 2, noteWidth, noteHeight, TRUE);
    MoveWindow(copy_, client.right - margin - button * 2 - gap,
        y + (rowHeight - button) / 2, button, button, TRUE);
    MoveWindow(wordWrapButton_, client.right - margin - button,
        y + (rowHeight - button) / 2, button, button, TRUE);
    y += rowHeight + gap;
    const int height = ControlTextSize(close_).cy + UiScale(window_, 12);
    const int bottom = (std::max)(y + gap, static_cast<int>(client.bottom) - margin - height);
    const int closeWidth = TextButtonWidth(close_);
    MoveWindow(close_, client.right - margin - closeWidth, bottom, closeWidth, height, TRUE);
    MoveWindow(output_, margin, y, width, (std::max)(1, bottom - gap - y), TRUE);
}

void XrayToolWindow::ApplyTextColors()
{
    if (output_ == nullptr) { return; }
    CHARRANGE selected{};
    POINT scroll{};
    SendMessageW(output_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selected));
    SendMessageW(output_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_BACKCOLOR;
    format.crTextColor = theme_.Colors().text;
    format.crBackColor = theme_.Colors().logBackground;
    SendMessageW(output_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    SendMessageW(output_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
    SendMessageW(output_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
}

void XrayToolWindow::SetAppearance(bool dark)
{
    dark_ = dark;
    if (window_ != nullptr) {
        theme_.Apply(dark);
        UpdateToolbar();
        ApplyTextColors();
    }
}

void XrayToolWindow::SetDocument()
{
    TextStream text{document_};
    EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&text), 0, TextStream::Read};
    SendMessageW(output_, EM_STREAMIN, SF_TEXT | SF_UNICODE, reinterpret_cast<LPARAM>(&stream));
    if (stream.dwError != 0 || !text.remaining.empty()) {
        throw std::runtime_error(Text("Cannot display the complete tool output; Copy all still contains the full result.",
            "无法完整显示工具输出；“复制全部”仍包含完整结果。"));
    }
    CHARRANGE start{0, 0};
    SendMessageW(output_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&start));
    SendMessageW(output_, EM_SCROLLCARET, 0, 0);
    ApplyTextColors();
    EnableWindow(copy_, !document_.empty());
}

void XrayToolWindow::Poll()
{
    if (auto result = task_.TakeResult()) {
        KillTimer(window_, 1);
        result_ = std::move(*result);
        document_ = L"Xray: " + result_->executable.native() + L"\r\n\r\n";
        document_ += DisplayOutput(result_->output);
        std::string{}.swap(result_->output);
        if (!result_->error.empty()) {
            if (!document_.empty() && document_.back() != L'\n') { document_ += L"\r\n"; }
            document_ += result_->error;
        }
        RefreshLanguage();
        SetDocument();
    }
}

void XrayToolWindow::CopyAll() const
{
    CopyTextToClipboard(window_, document_);
}

void XrayToolWindow::UpdateToolbar()
{
    if (wordWrapButton_ == nullptr) { return; }
    SetWindowTextW(wordWrapButton_, WordWrapCaption(wordWrap_));
    SendMessageW(wordWrapButton_, BM_SETCHECK, wordWrap_ ? BST_CHECKED : BST_UNCHECKED, 0);
    for (const auto& [control, caption] : std::array{
        std::pair{copy_, Text(L"Copy all", L"复制全部")},
        std::pair{wordWrapButton_, WordWrapCaption(wordWrap_)}}) {
        TOOLINFOW tool{};
        tool.cbSize = TTTOOLINFOW_V2_SIZE;
        tool.hwnd = window_;
        tool.uId = reinterpret_cast<UINT_PTR>(control);
        tool.lpszText = const_cast<wchar_t*>(caption);
        SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    const auto& colors = theme_.Colors();
    const UINT dpi = GetDpiForWindow(window_);
    if (dpi == toolIconDpi_ && colors.text == toolIconColor_) { return; }
    struct Icons {
        std::array<HICON, 4> values{};
        ~Icons() { for (const auto icon : values) { if (icon != nullptr) { DestroyIcon(icon); } } }
    } next;
    SvgIconRenderer renderer;
    const int size = UiScale(window_, 20);
    const std::array kinds{SvgIcon::CopyAll, SvgIcon::WordWrap};
    const std::array controls{copy_, wordWrapButton_};
    for (size_t index = 0; index < kinds.size(); ++index) {
        next.values[index * 2] = renderer.CreateIcon(kinds[index], size, colors.text);
        next.values[index * 2 + 1] = renderer.CreateIcon(kinds[index], size, colors.disabled);
        SetButtonIcons(controls[index], next.values[index * 2], next.values[index * 2 + 1], size);
    }
    next.values.swap(toolIcons_);
    toolIconDpi_ = dpi;
    toolIconColor_ = colors.text;
}

bool XrayToolWindow::Translate(MSG& message) const
{
    return window_ != nullptr && IsDialogMessageW(window_, &message) != FALSE;
}

void XrayToolWindow::Close()
{
    task_.Cancel();
    static_cast<void>(task_.TakeResult());
    if (window_ != nullptr) { DestroyWindow(window_); }
    result_.reset();
    std::wstring{}.swap(document_);
}

LRESULT CALLBACK XrayToolWindow::Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<XrayToolWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<XrayToolWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) { return DefWindowProcW(window, message, wParam, lParam); }
    try {
        LRESULT themed = 0;
        if (self->theme_.Handle(message, wParam, lParam, themed)) { return themed; }
        switch (message) {
        case WM_CREATE:
            self->Initialize();
            return 0;
        case WM_SIZE:
            self->Layout();
            return 0;
        case WM_DPICHANGED: {
            const auto* bounds = reinterpret_cast<RECT*>(lParam);
            self->SetFonts();
            self->theme_.Apply(self->dark_, true);
            self->ApplyTextColors();
            SetWindowPos(window, nullptr, bounds->left, bounds->top,
                bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            RECT bounds{0, 0, UiScale(window, 360), UiScale(window, 260)};
            AdjustWindowRectExForDpi(&bounds, kToolStyle, FALSE, WS_EX_CONTROLPARENT, GetDpiForWindow(window));
            MONITORINFO monitor{sizeof(MONITORINFO)};
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = {
                (std::min)(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left),
                (std::min)(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top)};
            return 0;
        }
        case WM_TIMER:
            self->Poll();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kCopyId && HIWORD(wParam) == BN_CLICKED) {
                self->CopyAll();
            } else if (LOWORD(wParam) == kWordWrapId && HIWORD(wParam) == BN_CLICKED) {
                const bool enabled = SendMessageW(self->wordWrapButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SetTextWordWrap(self->output_, enabled);
                self->wordWrap_ = enabled;
                self->UpdateToolbar();
            } else if (LOWORD(wParam) == IDCANCEL) {
                self->Close();
            }
            return 0;
        case WM_CLOSE:
            self->Close();
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            self->task_.Cancel();
            static_cast<void>(self->task_.TakeResult());
            self->theme_.Detach();
            for (auto& icon : self->icons_) {
                if (icon != nullptr) { DestroyIcon(icon); icon = nullptr; }
            }
            for (auto& icon : self->toolIcons_) {
                if (icon != nullptr) { DestroyIcon(icon); icon = nullptr; }
            }
            if (IsWindow(self->tooltip_)) { DestroyWindow(self->tooltip_); }
            self->tooltip_ = nullptr;
            self->toolIconDpi_ = 0;
            self->toolIconColor_ = CLR_INVALID;
            self->wordWrap_ = false;
            self->wordWrapButton_ = nullptr;
            if (self->font_ != nullptr) { DeleteObject(self->font_); self->font_ = nullptr; }
            if (self->outputFont_ != nullptr) { DeleteObject(self->outputFont_); self->outputFont_ = nullptr; }
            self->status_ = self->note_ = self->output_ = self->copy_ = self->close_ = nullptr;
            self->result_.reset();
            std::wstring{}.swap(self->document_);
            self->window_ = nullptr;
            return 0;
        }
    } catch (...) {
        self->error_ = std::current_exception();
        if (message == WM_CREATE) { return -1; }
        self->task_.Cancel();
        EnableWindow(self->copy_, !self->document_.empty());
        const auto text = Text(L"Cannot complete this operation. The full captured result can still be copied.",
            L"无法完成此操作，仍可复制已捕获的完整结果。");
        SetWindowTextW(self->status_, text);
        if (self->quiet_) {
            try { std::rethrow_exception(self->error_); }
            catch (const std::exception& exception) {
                std::fprintf(stderr, "Smoke: Xray result window operation failed: %s\n", exception.what());
            } catch (...) {
                std::fwprintf(stderr, L"Smoke: Xray result window operation failed (unknown error).\n");
            }
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool SmokeTestXrayTools(HWND owner, const std::filesystem::path& root)
{
    std::fwprintf(stderr, L"Smoke: independent Xray tool result window, complete output, copy, themes, and cancellation.\n");
    const auto savedLanguage = CurrentLanguage();
    struct RestoreLanguage { Language language; ~RestoreLanguage() { ApplyLanguage(language); } } restore{savedLanguage};
    XrayToolWindow tool(root);
    Settings settings;
    settings.executable = (root / L"XTray.Tests.exe").native();
    settings.configFiles = {L".\\tool-ok.conf"};
    const auto text = [](HWND control) {
        const int length = GetWindowTextLengthW(control);
        std::wstring result(static_cast<size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(control, result.data(), static_cast<int>(result.size()));
        result.resize(static_cast<size_t>(copied));
        return result;
    };
    const auto wait = [&] {
        const auto deadline = GetTickCount64() + 4000;
        while (tool.Running() && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (!tool.Translate(message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            Sleep(5);
        }
        SendMessageW(tool.Window(), WM_TIMER, 1, 0);
        return !tool.Running();
    };
    const auto fail = [](const wchar_t* reason) {
        std::fwprintf(stderr, L"Smoke: Xray tool result failed: %ls.\n", reason);
        return false;
    };
    HWND reused = nullptr;
    for (const auto language : {Language::English, Language::Chinese}) {
        ApplyLanguage(language);
        for (const bool dark : {false, true}) {
            tool.Open(owner, XrayToolKind::Check, settings, true, dark, true);
            if (reused != nullptr && tool.Window() != reused) { return fail(L"result window reuse"); }
            reused = tool.Window();
            if (!IsWindowEnabled(owner) || !wait()) { return fail(L"responsive modeless check"); }
            const auto output = GetDlgItem(tool.Window(), kOutputId);
            if (text(tool.Window()) != Text(L"Check Xray configuration", L"检查 Xray 配置") ||
                text(GetDlgItem(tool.Window(), kStatusId)).find(Text(L"passed", L"通过")) == std::wstring::npos ||
                text(output).find(L"中文 tool stdout") == std::wstring::npos ||
                text(output).find(L"tool stderr") == std::wstring::npos) {
                return fail(L"original output, title, or check status");
            }
            CHARRANGE selected{0, 4};
            SendMessageW(output, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            SendMessageW(output, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
            if (format.crTextColor != PaletteFor(dark).text) { return fail(L"readable result text theme"); }
            const HWND copy = GetDlgItem(tool.Window(), kCopyId);
            const HWND wrap = GetDlgItem(tool.Window(), kWordWrapId);
            RECT copyBounds{}, wrapBounds{}, outputBounds{}, closeBounds{};
            GetWindowRect(copy, &copyBounds);
            GetWindowRect(wrap, &wrapBounds);
            GetWindowRect(output, &outputBounds);
            GetWindowRect(GetDlgItem(tool.Window(), IDCANCEL), &closeBounds);
            if ((GetWindowLongPtrW(copy, GWL_STYLE) & BS_ICON) == 0 ||
                (GetWindowLongPtrW(wrap, GWL_STYLE) & (BS_PUSHLIKE | BS_AUTOCHECKBOX)) !=
                    (BS_PUSHLIKE | BS_AUTOCHECKBOX) ||
                copyBounds.right + UiScale(copy, 8) != wrapBounds.left ||
                copyBounds.top != wrapBounds.top || wrapBounds.right != outputBounds.right ||
                wrapBounds.bottom >= outputBounds.top || copyBounds.top >= closeBounds.top ||
                SendMessageW(wrap, BM_GETCHECK, 0, 0) != BST_UNCHECKED ||
                !SmokeTestNativeControl(copy) || !SmokeTestNativeControl(wrap)) {
                return fail(L"native copy/wrap toolbar, default state, alignment, or theme");
            }
        }
    }
    settings.configFiles = {L".\\tool-large.conf"};
    tool.Open(owner, XrayToolKind::Preview, settings, true, false, true);
    if (!wait()) { return fail(L"large preview completion"); }
    const HWND output = GetDlgItem(tool.Window(), kOutputId);
    const auto before = text(output);
    const auto style = GetWindowLongPtrW(output, GWL_STYLE);
    if (before.size() < 500000 || before.find(L"tool line 0:") == std::wstring::npos ||
        before.find(L"tool line 11999:") == std::wstring::npos ||
        !before.ends_with(L"tool output end") || std::count(before.begin(), before.end(), L'\n') < 12000 ||
        (style & (ES_READONLY | WS_VSCROLL | WS_HSCROLL)) != (ES_READONLY | WS_VSCROLL | WS_HSCROLL)) {
        std::fwprintf(stderr, L"Smoke: tool preview details (characters=%zu, lines=%zu, style=%llx, first=%d, last=%d, tail=%d).\n",
            before.size(), static_cast<size_t>(std::count(before.begin(), before.end(), L'\n')),
            static_cast<unsigned long long>(style), before.find(L"tool line 0:") != std::wstring::npos,
            before.find(L"tool line 11999:") != std::wstring::npos, before.ends_with(L"tool output end"));
        return fail(L"complete scrollable preview beyond the runtime log limit");
    }
    const LONG end = GetWindowTextLengthW(output);
    CHARRANGE tail{end, end};
    SendMessageW(output, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&tail));
    SendMessageW(output, EM_SCROLLCARET, 0, 0);
    SendMessageW(output, WM_VSCROLL, SB_BOTTOM, 0);
    SendMessageW(output, WM_HSCROLL, SB_RIGHT, 0);
    POINT scrolled{};
    SendMessageW(output, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scrolled));
    if (scrolled.x <= 0 || scrolled.y <= 0) {
        std::fwprintf(stderr, L"Smoke: tool scroll positions (x=%ld, y=%ld).\n", scrolled.x, scrolled.y);
        return fail(L"native horizontal and vertical result scrolling");
    }
    CHARRANGE selection{0, 4};
    SendMessageW(output, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    tool.SetAppearance(true);
    tool.RefreshLanguage();
    if (text(output) != before) { return fail(L"theme or language changed the captured output"); }
    SendMessageW(tool.Window(), WM_COMMAND, MAKEWPARAM(kCopyId, BN_CLICKED), 0);
    if (!OpenClipboard(owner)) { return fail(L"clipboard unavailable"); }
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const auto copied = data != nullptr ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
    const bool completeCopy = copied != nullptr && std::wstring_view(copied).size() > 500000 &&
        std::wstring_view(copied).find(L"tool line 11999:") != std::wstring::npos &&
        std::wstring_view(copied).ends_with(L"tool output end");
    if (copied != nullptr) { GlobalUnlock(data); }
    CloseClipboard();
    if (!completeCopy) { return fail(L"Copy all copied only a selection or truncated the result"); }
    const HWND wrap = GetDlgItem(tool.Window(), kWordWrapId);
    SendMessageW(wrap, BM_CLICK, 0, 0);
    CHARRANGE afterWrap{};
    SendMessageW(output, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&afterWrap));
    SendMessageW(output, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scrolled));
    if (SendMessageW(wrap, BM_GETCHECK, 0, 0) != BST_CHECKED ||
        text(output) != before || afterWrap.cpMin != selection.cpMin ||
        afterWrap.cpMax != selection.cpMax || scrolled.x != 0 ||
        !SmokeTestNativeControl(wrap)) {
        return fail(L"tool word wrapping changed the complete output or selection");
    }
    SendMessageW(tool.Window(), WM_COMMAND, MAKEWPARAM(kCopyId, BN_CLICKED), 0);
    if (!OpenClipboard(owner)) { return fail(L"clipboard unavailable after wrapping"); }
    const HANDLE wrappedData = GetClipboardData(CF_UNICODETEXT);
    const auto wrappedCopy = wrappedData != nullptr ? static_cast<const wchar_t*>(GlobalLock(wrappedData)) : nullptr;
    const bool sameCopy = wrappedCopy != nullptr && std::wstring_view(wrappedCopy).find(L"tool line 11999:") != std::wstring::npos &&
        std::wstring_view(wrappedCopy).ends_with(L"tool output end") &&
        static_cast<size_t>(std::count(wrappedCopy, wrappedCopy + std::wcslen(wrappedCopy), L'\n')) ==
            static_cast<size_t>(std::count(before.begin(), before.end(), L'\n'));
    if (wrappedCopy != nullptr) { GlobalUnlock(wrappedData); }
    CloseClipboard();
    if (!sameCopy) { return fail(L"word wrapping inserted line breaks into copied output"); }

    settings.configFiles = {L".\\tool-fail.conf"};
    tool.Open(owner, XrayToolKind::Check, settings, true, true, true);
    if (SendMessageW(GetDlgItem(tool.Window(), kWordWrapId), BM_GETCHECK, 0, 0) != BST_CHECKED) {
        return fail(L"tool word wrap was lost during the same window lifetime");
    }
    if (!wait() || text(GetDlgItem(tool.Window(), kStatusId)).find(L"23") == std::wstring::npos ||
        text(GetDlgItem(tool.Window(), kOutputId)).find(L"expected failure") == std::wstring::npos) {
        return fail(L"failure exit code and original diagnostic");
    }
    settings.configFiles.clear();
    tool.Open(owner, XrayToolKind::Version, settings, true, false, true);
    if (!wait() || text(GetDlgItem(tool.Window(), kOutputId)).find(L"Xray test-version") == std::wstring::npos) {
        return fail(L"version query without configuration");
    }
    RECT resultBounds{};
    GetWindowRect(tool.Window(), &resultBounds);
    const UINT dpi = GetDpiForWindow(tool.Window());
    SendMessageW(tool.Window(), WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&resultBounds));
    for (const auto size : {ICON_SMALL, ICON_BIG}) {
        const auto icon = reinterpret_cast<HICON>(SendMessageW(tool.Window(), WM_GETICON, size, 0));
        ICONINFO information{};
        const bool valid = icon != nullptr && GetIconInfo(icon, &information) != FALSE;
        if (information.hbmColor != nullptr) { DeleteObject(information.hbmColor); }
        if (information.hbmMask != nullptr) { DeleteObject(information.hbmMask); }
        if (!valid) { return fail(L"independent window icons after DPI refresh"); }
    }
    settings.configFiles = {L".\\tool-hold.conf"};
    tool.Open(owner, XrayToolKind::Preview, settings, true, true, true);
    const HWND active = tool.Window();
    const auto heading = text(active);
    tool.Open(owner, XrayToolKind::Version, settings, true, true, true);
    if (!tool.Running() || tool.Window() != active || text(active) != heading) {
        return fail(L"one active auxiliary task");
    }
    SendMessageW(active, WM_CLOSE, 0, 0);
    if (tool.Window() != nullptr || tool.Running() || !IsWindowEnabled(owner)) {
        return fail(L"closing the result cancels only its task");
    }
    tool.Open(owner, XrayToolKind::Version, settings, true, false, true);
    if (!wait() || SendMessageW(GetDlgItem(tool.Window(), kWordWrapId), BM_GETCHECK, 0, 0) != BST_UNCHECKED) {
        return fail(L"closing and reopening the result must reset word wrap to off");
    }
    return true;
}

} // namespace xtray
