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
    bool createStatusWindow();
    void showStatusWindow();
    void updateStatusWindow();
    bool addTrayIcon();
    void removeTrayIcon();
    void refreshTooltip();
    std::wstring buildTooltipText() const;
    void showContextMenu();
    void handleCommand(UINT commandId);

    void doRescan();
    void doSetup();
    void doExportConfig();
    void doImportConfig();
    void doOpenConfigFolder() const;

    bool pickSavePath(std::wstring& outPath) const;
    bool pickOpenPath(std::wstring& outPath) const;
    std::wstring configFolder() const;
    std::wstring exeFolder() const;
    std::wstring buildStatusWindowText() const;
    std::wstring formatDieMenuLine(const DieStatusSnapshot& die, int dieIndex) const;

    HINSTANCE instanceHandle_ = nullptr;
    HWND windowHandle_ = nullptr;
    HWND statusWindowHandle_ = nullptr;
    HWND statusTextHandle_ = nullptr;
    std::wstring windowClassName_ = L"PixelsTrayWindowClass";
    std::wstring statusWindowClassName_ = L"PixelsTrayStatusWindowClass";
    std::wstring configPath_;
    UINT taskbarCreatedMessage_ = 0;

    NOTIFYICONDATAW trayIconData_{};
    bool trayIconAdded_ = false;
    bool configLoaded_ = false;

    std::atomic<bool> stateDirty_ = true;

    std::unique_ptr<PixelsRuntimeService> runtime_;
};
