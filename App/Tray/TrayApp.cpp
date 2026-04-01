#include "pch.h"
#include "App/Tray/TrayApp.h"

#include <algorithm>
#include <sstream>

#include <shellapi.h>

namespace
{
    std::string narrowAscii(const std::wstring& ws)
    {
        std::string out;
        out.reserve(ws.size());
        for (const wchar_t ch : ws)
        {
            out.push_back((ch >= 0 && ch <= 0x7F) ? static_cast<char>(ch) : '?');
        }
        return out;
    }

    std::wstring toWide(const std::string& s)
    {
        return std::wstring(s.begin(), s.end());
    }

    std::wstring statusToWide(Systemic::Pixels::PixelStatus status)
    {
        using Systemic::Pixels::PixelStatus;
        switch (status)
        {
        case PixelStatus::Disconnected:  return L"disconnected";
        case PixelStatus::Connecting:    return L"connecting";
        case PixelStatus::Identifying:   return L"identifying";
        case PixelStatus::Ready:         return L"ready";
        case PixelStatus::Disconnecting: return L"disconnecting";
        default:                         return L"unknown";
        }
    }
}

TrayApp::TrayApp()
{
}

TrayApp::~TrayApp()
{
    if (runtime_)
    {
        runtime_->stop();
        runtime_.reset();
    }

    removeTrayIcon();

    if (windowHandle_)
    {
        DestroyWindow(windowHandle_);
        windowHandle_ = nullptr;
    }
}

bool TrayApp::initialize(HINSTANCE instanceHandle, const std::wstring& configPath)
{
    instanceHandle_ = instanceHandle;
    configPath_ = configPath;

    if (!createMessageWindow())
    {
        return false;
    }

    if (!addTrayIcon())
    {
        return false;
    }

    std::string error;
    runtime_ = std::make_unique<PixelsRuntimeService>(nullptr, [this]()
    {
        stateDirty_ = true;
    });

    if (!runtime_->loadConfig(narrowAscii(configPath_), error))
    {
        MessageBoxW(windowHandle_, toWide("Failed to load pixels.cfg: " + error).c_str(), L"Pixels Tray", MB_ICONERROR | MB_OK);
        return false;
    }

    if (!runtime_->start())
    {
        MessageBoxW(windowHandle_, L"Failed to start runtime service.", L"Pixels Tray", MB_ICONERROR | MB_OK);
        return false;
    }

    SetTimer(windowHandle_, kTooltipTimerId, 1500, nullptr);
    refreshTooltip();
    return true;
}

int TrayApp::runMessageLoop()
{
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK TrayApp::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    TrayApp* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE)
    {
        const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TrayApp*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (self)
    {
        return self->handleWindowMessage(hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TrayApp::handleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wParam == kTooltipTimerId && stateDirty_)
        {
            refreshTooltip();
            stateDirty_ = false;
        }
        return 0;
    case kTrayCallbackMsg:
        if (lParam == WM_LBUTTONDBLCLK)
        {
            refreshTooltip();
            MessageBoxW(hwnd, buildTooltipText().c_str(), L"Pixels Tray", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTooltipTimerId);
        if (runtime_)
        {
            runtime_->stop();
            runtime_.reset();
        }
        removeTrayIcon();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool TrayApp::createMessageWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayApp::windowProc;
    wc.hInstance = instanceHandle_;
    wc.lpszClassName = windowClassName_.c_str();

    if (!RegisterClassExW(&wc))
    {
        return false;
    }

    windowHandle_ = CreateWindowExW(
        0,
        windowClassName_.c_str(),
        L"Pixels Tray",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        instanceHandle_,
        this);

    return windowHandle_ != nullptr;
}

bool TrayApp::addTrayIcon()
{
    trayIconData_ = {};
    trayIconData_.cbSize = sizeof(trayIconData_);
    trayIconData_.hWnd = windowHandle_;
    trayIconData_.uID = 1;
    trayIconData_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    trayIconData_.uCallbackMessage = kTrayCallbackMsg;
    trayIconData_.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));

    const std::wstring initialTip = L"Pixels tray starting...";
    wcsncpy_s(trayIconData_.szTip, _countof(trayIconData_.szTip), initialTip.c_str(), _TRUNCATE);

    trayIconAdded_ = (Shell_NotifyIconW(NIM_ADD, &trayIconData_) == TRUE);
    return trayIconAdded_;
}

void TrayApp::removeTrayIcon()
{
    if (!trayIconAdded_)
    {
        return;
    }

    Shell_NotifyIconW(NIM_DELETE, &trayIconData_);
    trayIconAdded_ = false;
}

void TrayApp::refreshTooltip()
{
    if (!trayIconAdded_)
    {
        return;
    }

    const std::wstring tip = buildTooltipText();
    wcsncpy_s(trayIconData_.szTip, _countof(trayIconData_.szTip), tip.c_str(), _TRUNCATE);
    trayIconData_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &trayIconData_);
}

std::wstring TrayApp::buildTooltipText() const
{
    if (!runtime_)
    {
        return L"Pixels tray: runtime unavailable";
    }

    const auto snapshots = runtime_->snapshotDice();
    if (snapshots.empty())
    {
        return runtime_->isRunning() ? L"Pixels tray: waiting for config" : L"Pixels tray: stopped";
    }

    std::wstringstream ss;
    ss << L"Pixels ";

    for (size_t i = 0; i < snapshots.size(); ++i)
    {
        const auto& die = snapshots[i];
        if (i > 0)
        {
            ss << L" | ";
        }

        ss << L"D" << (i + 1) << L":";
        ss << statusToWide(die.status);
        if (die.hasPixel)
        {
            ss << L" b" << die.batteryLevel << L"%";
            ss << L" f" << die.currentFace;
        }
    }

    std::wstring text = ss.str();
    if (text.size() > 120)
    {
        text.resize(120);
    }
    return text;
}
