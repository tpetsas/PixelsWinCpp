#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include "App/Runtime/PixelsRuntimeService.h"
#include "App/Tray/TraySettings.h"

namespace Systemic::Pixels
{
    class PixelScanner;
}

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
    static constexpr UINT kSetupRefreshMsg = WM_APP + 20;

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool createMessageWindow();
    bool createStatusWindow();
    bool createSetupWindow();
    void showStatusWindow();
    void updateStatusWindow();
    void showSetupWindow();
    void startSetupScan();
    void stopSetupScan();
    void refreshSetupList();
    void saveSetupSelection();
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
    std::wstring buildStatusWindowText() const;
    std::wstring formatDieMenuLine(const DieStatusSnapshot& die, int dieIndex) const;

    HINSTANCE instanceHandle_ = nullptr;
    HWND windowHandle_ = nullptr;
    HWND statusWindowHandle_ = nullptr;
    HWND statusTextHandle_ = nullptr;
    HWND setupWindowHandle_ = nullptr;
    HWND setupListHandle_ = nullptr;
    HWND setupScanButtonHandle_ = nullptr;
    HWND setupSaveButtonHandle_ = nullptr;
    std::wstring windowClassName_ = L"PixelsTrayWindowClass";
    std::wstring statusWindowClassName_ = L"PixelsTrayStatusWindowClass";
    std::wstring setupWindowClassName_ = L"PixelsTraySetupWindowClass";
    std::wstring configPath_;
    UINT taskbarCreatedMessage_ = 0;

    NOTIFYICONDATAW trayIconData_{};
    bool trayIconAdded_ = false;
    bool configLoaded_ = false;

    std::atomic<bool> stateDirty_ = true;

    std::unique_ptr<PixelsRuntimeService> runtime_;

    struct SetupDiscoveredDie
    {
        uint32_t pixelId = 0;
        std::wstring name;
        int rssi = 0;
        int battery = 0;
        int face = 0;
    };

    std::unique_ptr<Systemic::Pixels::PixelScanner> setupScanner_;
    mutable std::mutex setupMutex_;
    std::map<uint32_t, SetupDiscoveredDie> setupDiscoveredById_;
    std::vector<uint32_t> setupListPixelIds_;

    // Log window
    HWND logWindowHandle_ = nullptr;
    HWND logTextHandle_ = nullptr;
    std::wstring logWindowClassName_ = L"PixelsTrayLogWindowClass";
    mutable std::mutex logMutex_;
    std::deque<std::wstring> logLines_;
    static constexpr size_t kMaxLogLines = 100;

    bool createLogWindow();
    void showLogWindow();
    void appendLog(const std::string& message);
    void updateLogWindow();

    // Notification system
    static constexpr UINT_PTR kNotificationTimerId = 2;
    static constexpr int kLowBatteryThreshold = 10;
    static constexpr int kDisconnectWarningSeconds = 30;

    bool allDiceReadyNotified_ = false;
    std::set<uint32_t> lowBatteryNotifiedIds_;
    std::map<uint32_t, std::chrono::steady_clock::time_point> disconnectTimestamps_;
    std::set<uint32_t> disconnectWarningShownIds_;

    void checkAndShowNotifications();
    void showBalloonNotification(const std::wstring& title, const std::wstring& message, DWORD iconType = NIIF_INFO);

    // Dice color themes (for Aurora Sky and Midnight Galaxy)
    struct DiceTheme
    {
        COLORREF primaryColor;
        COLORREF secondaryColor;
        COLORREF textColor;
        std::wstring displayName;
    };

    DiceTheme getDiceTheme(const std::string& label) const;

    // Settings
    TraySettings settings_;
    std::wstring settingsPath_;
    std::wstring logFilePath_;
    std::ofstream logFile_;

    void loadSettings();
    void writeLogToFile(const std::wstring& message);
    bool shouldShowLogMessage(const std::string& message) const;
};
