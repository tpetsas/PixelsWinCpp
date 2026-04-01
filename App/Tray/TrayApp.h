#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <windows.h>
#include <shellapi.h>

#include "App/Runtime/PixelsRuntimeService.h"

class TrayApp
{
public:
    TrayApp();
    ~TrayApp();

    TrayApp(const TrayApp&) = delete;
    TrayApp& operator=(const TrayApp&) = delete;

    bool initialize(HINSTANCE instanceHandle, const std::wstring& configPath);
    int runMessageLoop();

private:
    static constexpr UINT kTrayCallbackMsg = WM_APP + 1;
    static constexpr UINT_PTR kTooltipTimerId = 1;

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool createMessageWindow();
    bool addTrayIcon();
    void removeTrayIcon();
    void refreshTooltip();
    std::wstring buildTooltipText() const;

    HINSTANCE instanceHandle_ = nullptr;
    HWND windowHandle_ = nullptr;
    std::wstring windowClassName_ = L"PixelsTrayWindowClass";
    std::wstring configPath_;

    NOTIFYICONDATAW trayIconData_{};
    bool trayIconAdded_ = false;

    std::atomic<bool> stateDirty_ = true;

    std::unique_ptr<PixelsRuntimeService> runtime_;
};
