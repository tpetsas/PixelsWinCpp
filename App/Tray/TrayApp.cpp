#include "pch.h"
#include "App/Tray/TrayApp.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include <commdlg.h>
#include <shellapi.h>

#include "App/ConfigManager.h"
#include "App/Tray/TrayResources.h"
#include "Systemic/Pixels/PixelScanner.h"

namespace
{
    constexpr int kSetupListId = 3001;
    constexpr int kSetupScanButtonId = 3002;
    constexpr int kSetupSaveButtonId = 3003;

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
    stopSetupScan();

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

    if (statusWindowHandle_)
    {
        DestroyWindow(statusWindowHandle_);
        statusWindowHandle_ = nullptr;
    }

    if (setupWindowHandle_)
    {
        DestroyWindow(setupWindowHandle_);
        setupWindowHandle_ = nullptr;
    }

    statusWindowHandle_ = nullptr;
    statusTextHandle_ = nullptr;
    setupListHandle_ = nullptr;
    setupScanButtonHandle_ = nullptr;
    setupSaveButtonHandle_ = nullptr;
}

bool TrayApp::initialize(HINSTANCE instanceHandle, const std::wstring& configPath)
{
    instanceHandle_ = instanceHandle;
    configPath_ = configPath;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

    if (!createMessageWindow())
    {
        return false;
    }

    if (!createStatusWindow())
    {
        return false;
    }

    if (!createSetupWindow())
    {
        return false;
    }

    if (!addTrayIcon())
    {
        return false;
    }

    runtime_ = std::make_unique<PixelsRuntimeService>(nullptr, [this]()
    {
        stateDirty_ = true;
    });

    std::string error;
    configLoaded_ = runtime_->loadConfig(narrowAscii(configPath_), error);
    if (!configLoaded_)
    {
        std::wstring msg =
            L"No valid pixels.cfg was found.\n\n"
            L"Let's set up your dice now.\n"
            L"We will start scanning immediately.\n\n"
            L"Tip: by default, the first two discovered dice are pre-selected; just click 'Save Setup'.\n\n"
            L"Config path:\n" + configPath_;
        MessageBoxW(windowHandle_, msg.c_str(), L"Pixels Tray - First Time Setup", MB_ICONINFORMATION | MB_OK);
        showSetupWindow();
    }
    else if (!runtime_->start())
    {
        MessageBoxW(windowHandle_, L"Failed to start runtime service.", L"Pixels Tray", MB_ICONERROR | MB_OK);
    }

    SetTimer(windowHandle_, kTooltipTimerId, 1200, nullptr);
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
    if (taskbarCreatedMessage_ != 0 && msg == taskbarCreatedMessage_)
    {
        addTrayIcon();
        refreshTooltip();
        return 0;
    }

    switch (msg)
    {
    case WM_TIMER:
        if (wParam == kTooltipTimerId && stateDirty_)
        {
            refreshTooltip();
            updateStatusWindow();
            stateDirty_ = false;
        }
        return 0;
    case WM_COMMAND:
        if (hwnd == setupWindowHandle_)
        {
            const UINT commandId = LOWORD(wParam);
            if (commandId == kSetupScanButtonId)
            {
                startSetupScan();
                return 0;
            }
            if (commandId == kSetupSaveButtonId)
            {
                saveSetupSelection();
                return 0;
            }
        }

        handleCommand(LOWORD(wParam));
        return 0;
    case kSetupRefreshMsg:
        refreshSetupList();
        return 0;
    case kTrayCallbackMsg:
        if (lParam == WM_LBUTTONDBLCLK)
        {
            showStatusWindow();
        }
        else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
        {
            showContextMenu();
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
    case WM_CLOSE:
        if (hwnd == statusWindowHandle_)
        {
            ShowWindow(statusWindowHandle_, SW_HIDE);
            return 0;
        }
        if (hwnd == setupWindowHandle_)
        {
            stopSetupScan();
            ShowWindow(setupWindowHandle_, SW_HIDE);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
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

bool TrayApp::createStatusWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayApp::windowProc;
    wc.hInstance = instanceHandle_;
    wc.lpszClassName = statusWindowClassName_.c_str();

    if (!RegisterClassExW(&wc))
    {
        return false;
    }

    statusWindowHandle_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        statusWindowClassName_.c_str(),
        L"Pixels Status",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        460,
        320,
        nullptr,
        nullptr,
        instanceHandle_,
        this);

    if (!statusWindowHandle_)
    {
        return false;
    }

    statusTextHandle_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        10,
        10,
        420,
        260,
        statusWindowHandle_,
        nullptr,
        instanceHandle_,
        nullptr);

    return statusTextHandle_ != nullptr;
}

bool TrayApp::createSetupWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayApp::windowProc;
    wc.hInstance = instanceHandle_;
    wc.lpszClassName = setupWindowClassName_.c_str();

    if (!RegisterClassExW(&wc))
    {
        return false;
    }

    setupWindowHandle_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        setupWindowClassName_.c_str(),
        L"Pixels Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        560,
        420,
        nullptr,
        nullptr,
        instanceHandle_,
        this);

    if (!setupWindowHandle_)
    {
        return false;
    }

    CreateWindowExW(
        0,
        L"STATIC",
        L"Scan and select 1 or 2 dice, then click Save Setup:",
        WS_CHILD | WS_VISIBLE,
        12,
        12,
        520,
        20,
        setupWindowHandle_,
        nullptr,
        instanceHandle_,
        nullptr);

    setupListHandle_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"LISTBOX",
        L"",
        WS_CHILD | WS_VISIBLE | LBS_EXTENDEDSEL | WS_VSCROLL | LBS_NOTIFY,
        12,
        40,
        520,
        290,
        setupWindowHandle_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSetupListId)),
        instanceHandle_,
        nullptr);

    setupScanButtonHandle_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"Scan",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12,
        340,
        100,
        30,
        setupWindowHandle_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSetupScanButtonId)),
        instanceHandle_,
        nullptr);

    setupSaveButtonHandle_ = CreateWindowExW(
        0,
        L"BUTTON",
        L"Save Setup",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        122,
        340,
        120,
        30,
        setupWindowHandle_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSetupSaveButtonId)),
        instanceHandle_,
        nullptr);

    return (setupListHandle_ != nullptr && setupScanButtonHandle_ != nullptr && setupSaveButtonHandle_ != nullptr);
}

void TrayApp::showStatusWindow()
{
    if (!statusWindowHandle_)
    {
        return;
    }

    updateStatusWindow();
    ShowWindow(statusWindowHandle_, SW_SHOWNORMAL);
    SetForegroundWindow(statusWindowHandle_);
}

void TrayApp::showSetupWindow()
{
    if (!setupWindowHandle_)
    {
        return;
    }

    ShowWindow(setupWindowHandle_, SW_SHOWNORMAL);
    SetForegroundWindow(setupWindowHandle_);
    startSetupScan();
}

void TrayApp::startSetupScan()
{
    stopSetupScan();

    {
        std::lock_guard<std::mutex> lock(setupMutex_);
        setupDiscoveredById_.clear();
        setupListPixelIds_.clear();
    }

    if (setupListHandle_)
    {
        SendMessageW(setupListHandle_, LB_RESETCONTENT, 0, 0);
    }

    Systemic::Pixels::PixelScanner::ScannedPixelListener listener = [this](const std::shared_ptr<const Systemic::Pixels::ScannedPixel>& scannedPixel)
    {
        if (!scannedPixel)
        {
            return;
        }

        const uint32_t id = static_cast<uint32_t>(scannedPixel->pixelId());

        {
            std::lock_guard<std::mutex> lock(setupMutex_);
            auto& die = setupDiscoveredById_[id];
            die.pixelId = id;
            die.name = scannedPixel->name();
            die.rssi = scannedPixel->rssi();
            die.battery = scannedPixel->batteryLevel();
            die.face = scannedPixel->currentFace();
        }

        PostMessageW(windowHandle_, kSetupRefreshMsg, 0, 0);
    };

    setupScanner_ = std::make_unique<Systemic::Pixels::PixelScanner>(listener);
    setupScanner_->start();
}

void TrayApp::stopSetupScan()
{
    if (!setupScanner_)
    {
        return;
    }

    try
    {
        setupScanner_->stop();
    }
    catch (...)
    {
    }
    setupScanner_.reset();
}

void TrayApp::refreshSetupList()
{
    if (!setupListHandle_)
    {
        return;
    }

    std::vector<SetupDiscoveredDie> dice;
    {
        std::lock_guard<std::mutex> lock(setupMutex_);
        dice.reserve(setupDiscoveredById_.size());
        for (const auto& kvp : setupDiscoveredById_)
        {
            dice.push_back(kvp.second);
        }
    }

    std::sort(dice.begin(), dice.end(), [](const SetupDiscoveredDie& a, const SetupDiscoveredDie& b)
    {
        return a.pixelId < b.pixelId;
    });

    SendMessageW(setupListHandle_, LB_RESETCONTENT, 0, 0);
    setupListPixelIds_.clear();

    for (const auto& die : dice)
    {
        std::wstringstream line;
        line << L"0x" << std::hex << std::uppercase << die.pixelId << std::dec
             << L"  name=" << die.name
             << L"  rssi=" << die.rssi
             << L"  battery=" << die.battery << L"%"
             << L"  face=" << die.face;

        SendMessageW(setupListHandle_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.str().c_str()));
        setupListPixelIds_.push_back(die.pixelId);
    }

    // First-time friendly default: pre-select up to 2 discovered dice.
    const size_t defaultSelected = std::min<size_t>(2, setupListPixelIds_.size());
    for (size_t i = 0; i < defaultSelected; ++i)
    {
        SendMessageW(setupListHandle_, LB_SETSEL, TRUE, static_cast<LPARAM>(i));
    }
}

void TrayApp::saveSetupSelection()
{
    if (!setupListHandle_)
    {
        return;
    }

    const int selectedCount = static_cast<int>(SendMessageW(setupListHandle_, LB_GETSELCOUNT, 0, 0));
    if (selectedCount < 1 || selectedCount > 2)
    {
        MessageBoxW(setupWindowHandle_, L"Select exactly 1 or 2 dice.", L"Pixels Setup", MB_OK | MB_ICONWARNING);
        return;
    }

    std::vector<int> selectedIndices(static_cast<size_t>(selectedCount));
    const int copied = static_cast<int>(SendMessageW(
        setupListHandle_,
        LB_GETSELITEMS,
        static_cast<WPARAM>(selectedIndices.size()),
        reinterpret_cast<LPARAM>(selectedIndices.data())));

    if (copied != selectedCount)
    {
        MessageBoxW(setupWindowHandle_, L"Could not read selected items.", L"Pixels Setup", MB_OK | MB_ICONERROR);
        return;
    }

    PixelsConfig cfg;
    for (const int idx : selectedIndices)
    {
        if (idx < 0 || static_cast<size_t>(idx) >= setupListPixelIds_.size())
        {
            MessageBoxW(setupWindowHandle_, L"Invalid selection index.", L"Pixels Setup", MB_OK | MB_ICONERROR);
            return;
        }
        cfg.pixelIds.push_back(setupListPixelIds_[static_cast<size_t>(idx)]);
    }

    std::string error;
    if (!ConfigManager::save(narrowAscii(configPath_), cfg, error))
    {
        MessageBoxW(setupWindowHandle_, toWide("Failed to save pixels.cfg: " + error).c_str(), L"Pixels Setup", MB_OK | MB_ICONERROR);
        return;
    }

    stopSetupScan();
    ShowWindow(setupWindowHandle_, SW_HIDE);

    runtime_->stop();
    configLoaded_ = runtime_->loadConfig(narrowAscii(configPath_), error);
    if (!configLoaded_)
    {
        MessageBoxW(windowHandle_, toWide("Saved, but failed to reload config: " + error).c_str(), L"Pixels Tray", MB_OK | MB_ICONERROR);
        return;
    }

    if (!runtime_->start())
    {
        MessageBoxW(windowHandle_, L"Setup saved, but runtime failed to start.", L"Pixels Tray", MB_OK | MB_ICONERROR);
        return;
    }

    stateDirty_ = true;
    updateStatusWindow();
    MessageBoxW(windowHandle_, L"Setup saved successfully and runtime restarted.", L"Pixels Tray", MB_OK | MB_ICONINFORMATION);
}

void TrayApp::updateStatusWindow()
{
    if (!statusWindowHandle_ || !statusTextHandle_)
    {
        return;
    }

    const std::wstring text = buildStatusWindowText();
    SetWindowTextW(statusTextHandle_, text.c_str());
}

bool TrayApp::addTrayIcon()
{
    trayIconData_ = {};
    trayIconData_.cbSize = sizeof(trayIconData_);
    trayIconData_.hWnd = windowHandle_;
    trayIconData_.uID = 1;
    trayIconData_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    trayIconData_.uCallbackMessage = kTrayCallbackMsg;

    trayIconData_.hIcon = static_cast<HICON>(LoadImageW(
        instanceHandle_,
        MAKEINTRESOURCEW(IDI_D20_APP_ICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    if (!trayIconData_.hIcon)
    {
        trayIconData_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }

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

    if (!configLoaded_)
    {
        return L"Pixels tray: configure dice (right-click)";
    }

    const auto snapshots = runtime_->snapshotDice();
    if (snapshots.empty())
    {
        return runtime_->isRunning() ? L"Pixels tray: waiting for dice" : L"Pixels tray: stopped";
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

        ss << L"D" << (i + 1) << L":" << statusToWide(die.status);
        if (die.hasPixel)
        {
            ss << L" b" << die.batteryLevel << L"% f" << die.currentFace;
        }
        if (die.hasLastRoll)
        {
            ss << L" r" << die.lastRollFace;
        }
    }

    std::wstring text = ss.str();
    if (text.size() > 120)
    {
        text.resize(120);
    }
    return text;
}

std::wstring TrayApp::buildStatusWindowText() const
{
    std::wstringstream ss;
    ss << L"Pixels Tray Status\r\n\r\n";

    if (!runtime_)
    {
        ss << L"Runtime unavailable.";
        return ss.str();
    }

    ss << L"Running: " << (runtime_->isRunning() ? L"yes" : L"no") << L"\r\n";
    ss << L"Scanning: " << (runtime_->isScanning() ? L"yes" : L"no") << L"\r\n";
    ss << L"Config: " << (configLoaded_ ? L"loaded" : L"missing/invalid") << L"\r\n\r\n";

    const auto snapshots = runtime_->snapshotDice();
    if (snapshots.empty())
    {
        ss << L"No configured dice found in runtime.";
        return ss.str();
    }

    for (size_t i = 0; i < snapshots.size(); ++i)
    {
        const auto& die = snapshots[i];
        ss << L"Die " << (i + 1) << L" (" << toWide(die.label) << L")\r\n";
        ss << L"  Status: " << statusToWide(die.status) << L"\r\n";
        ss << L"  Battery: " << die.batteryLevel << L"%" << (die.isCharging ? L" (charging)" : L"") << L"\r\n";
        ss << L"  Current face: " << die.currentFace << L"\r\n";
        if (die.hasLastRoll)
        {
            ss << L"  Last roll: " << die.lastRollFace << L"\r\n";
        }
        else
        {
            ss << L"  Last roll: n/a\r\n";
        }

        ss << L"  Recent rolls: ";
        if (die.recentRollFaces.empty())
        {
            ss << L"n/a";
        }
        else
        {
            for (size_t r = 0; r < die.recentRollFaces.size(); ++r)
            {
                if (r > 0)
                {
                    ss << L", ";
                }
                ss << die.recentRollFaces[r];
            }
        }
        ss << L"\r\n\r\n";
    }

    return ss.str();
}

std::wstring TrayApp::formatDieMenuLine(const DieStatusSnapshot& die, int dieIndex) const
{
    std::wstringstream ss;
    ss << L"Die " << dieIndex << L": " << statusToWide(die.status)
       << L", batt " << die.batteryLevel << L"%, face " << die.currentFace;
    if (die.hasLastRoll)
    {
        ss << L", last " << die.lastRollFace;
    }
    return ss.str();
}

void TrayApp::showContextMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }

    const auto snapshots = runtime_ ? runtime_->snapshotDice() : std::vector<DieStatusSnapshot>{};
    if (!snapshots.empty())
    {
        for (size_t i = 0; i < snapshots.size(); ++i)
        {
            const auto& die = snapshots[i];
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, formatDieMenuLine(die, static_cast<int>(i + 1)).c_str());
            if (!die.recentRollFaces.empty())
            {
                std::wstringstream rs;
                rs << L"  Recent: ";
                for (size_t r = 0; r < die.recentRollFaces.size(); ++r)
                {
                    if (r > 0)
                    {
                        rs << L", ";
                    }
                    rs << die.recentRollFaces[r];
                }
                AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, rs.str().c_str());
            }
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(menu, MF_STRING, IDM_TRAY_RESCAN, L"Rescan dice");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SETUP, L"Setup dice");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXPORT_CFG, L"Export pixels.cfg");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_IMPORT_CFG, L"Import pixels.cfg");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_FOLDER, L"Open config folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, L"Quit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(windowHandle_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, windowHandle_, nullptr);
    DestroyMenu(menu);
}

void TrayApp::handleCommand(UINT commandId)
{
    switch (commandId)
    {
    case IDM_TRAY_RESCAN:
        doRescan();
        break;
    case IDM_TRAY_SETUP:
        doSetup();
        break;
    case IDM_TRAY_EXPORT_CFG:
        doExportConfig();
        break;
    case IDM_TRAY_IMPORT_CFG:
        doImportConfig();
        break;
    case IDM_TRAY_OPEN_FOLDER:
        doOpenConfigFolder();
        break;
    case IDM_TRAY_QUIT:
        DestroyWindow(windowHandle_);
        break;
    default:
        break;
    }
}

void TrayApp::doRescan()
{
    if (!runtime_)
    {
        return;
    }

    if (!configLoaded_)
    {
        std::string error;
        configLoaded_ = runtime_->loadConfig(narrowAscii(configPath_), error);
        if (!configLoaded_)
        {
            MessageBoxW(windowHandle_, toWide("Cannot rescan: " + error).c_str(), L"Pixels Tray", MB_ICONWARNING | MB_OK);
            return;
        }
    }

    runtime_->stop();
    if (!runtime_->start())
    {
        MessageBoxW(windowHandle_, L"Rescan failed to start runtime.", L"Pixels Tray", MB_ICONERROR | MB_OK);
    }
    stateDirty_ = true;
}

void TrayApp::doSetup()
{
    showSetupWindow();
}

void TrayApp::doExportConfig()
{
    std::wstring outPath;
    if (!pickSavePath(outPath))
    {
        return;
    }

    if (!CopyFileW(configPath_.c_str(), outPath.c_str(), FALSE))
    {
        MessageBoxW(windowHandle_, L"Failed to export pixels.cfg.", L"Pixels Tray", MB_ICONERROR | MB_OK);
        return;
    }

    MessageBoxW(windowHandle_, L"Config exported successfully.", L"Pixels Tray", MB_ICONINFORMATION | MB_OK);
}

void TrayApp::doImportConfig()
{
    std::wstring inPath;
    if (!pickOpenPath(inPath))
    {
        return;
    }

    if (!CopyFileW(inPath.c_str(), configPath_.c_str(), FALSE))
    {
        MessageBoxW(windowHandle_, L"Failed to import pixels.cfg.", L"Pixels Tray", MB_ICONERROR | MB_OK);
        return;
    }

    runtime_->stop();
    std::string error;
    configLoaded_ = runtime_->loadConfig(narrowAscii(configPath_), error);
    if (!configLoaded_)
    {
        MessageBoxW(windowHandle_, toWide("Imported file is invalid: " + error).c_str(), L"Pixels Tray", MB_ICONERROR | MB_OK);
        return;
    }

    if (!runtime_->start())
    {
        MessageBoxW(windowHandle_, L"Config imported, but runtime failed to start.", L"Pixels Tray", MB_ICONERROR | MB_OK);
        return;
    }

    stateDirty_ = true;
    MessageBoxW(windowHandle_, L"Config imported and runtime restarted.", L"Pixels Tray", MB_ICONINFORMATION | MB_OK);
}

void TrayApp::doOpenConfigFolder() const
{
    const std::wstring folder = configFolder();
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool TrayApp::pickSavePath(std::wstring& outPath) const
{
    wchar_t buffer[MAX_PATH * 4]{};
    std::wstring defaultPath = configFolder() + L"\\pixels.cfg";
    wcsncpy_s(buffer, _countof(buffer), defaultPath.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = windowHandle_;
    ofn.lpstrFilter = L"Config file (*.cfg)\0*.cfg\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(_countof(buffer));
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"cfg";

    if (!GetSaveFileNameW(&ofn))
    {
        return false;
    }

    outPath = buffer;
    return true;
}

bool TrayApp::pickOpenPath(std::wstring& outPath) const
{
    wchar_t buffer[MAX_PATH * 4]{};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = windowHandle_;
    ofn.lpstrFilter = L"Config file (*.cfg)\0*.cfg\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(_countof(buffer));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"cfg";
    ofn.lpstrInitialDir = configFolder().c_str();

    if (!GetOpenFileNameW(&ofn))
    {
        return false;
    }

    outPath = buffer;
    return true;
}

std::wstring TrayApp::configFolder() const
{
    const size_t slash = configPath_.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return L".";
    }
    return configPath_.substr(0, slash);
}
