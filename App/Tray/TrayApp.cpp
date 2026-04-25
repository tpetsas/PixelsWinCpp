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
    constexpr int kCustomDlgIconId = 3010;
    constexpr int kCustomDlgTextId = 3011;
    constexpr int kCustomDlgOkId = 3012;

    HINSTANCE g_hInstance = nullptr;
    std::wstring g_dlgMessage;

    INT_PTR CALLBACK CustomDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM)
    {
        switch (msg)
        {
        case WM_INITDIALOG:
        {
            // Center the dialog
            RECT rc;
            GetWindowRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
            int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
            SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

            // Set window icon
            HICON hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_D20_APP_ICON));
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));

            // Get client rect to calculate positions
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            int cw = clientRect.right;
            int ch = clientRect.bottom;

            // Layout: icon centered at top, text below centered, button at bottom
            int iconSize = 32;
            int iconX = (cw - iconSize) / 2;
            int iconY = 15;
            int textX = 20;
            int textY = iconY + iconSize + 20;
            int textW = cw - 40;
            int textH = ch - textY - 50;
            int btnWidth = 80;
            int btnHeight = 26;
            int btnX = (cw - btnWidth) / 2;
            int btnY = ch - btnHeight - 12;

            // Create icon control - centered at top
            HWND hIconCtrl = CreateWindowExW(0, L"STATIC", nullptr,
                WS_CHILD | WS_VISIBLE | SS_ICON,
                iconX, iconY, iconSize, iconSize, hwnd, reinterpret_cast<HMENU>(kCustomDlgIconId), g_hInstance, nullptr);
            SendMessageW(hIconCtrl, STM_SETICON, reinterpret_cast<WPARAM>(hIcon), 0);

            // Create text control - centered below icon
            CreateWindowExW(0, L"STATIC", g_dlgMessage.c_str(),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                textX, textY, textW, textH, hwnd, reinterpret_cast<HMENU>(kCustomDlgTextId), g_hInstance, nullptr);

            // Create OK button - centered at bottom
            HWND hBtn = CreateWindowExW(0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                btnX, btnY, btnWidth, btnHeight, hwnd, reinterpret_cast<HMENU>(kCustomDlgOkId), g_hInstance, nullptr);
            SetFocus(hBtn);

            return FALSE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kCustomDlgOkId || LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    void ShowCustomMessageBox(HINSTANCE hInst, HWND hParent, const std::wstring& title, const std::wstring& message)
    {
        g_hInstance = hInst;
        g_dlgMessage = message;

        // Create dialog template in memory
        #pragma pack(push, 4)
        struct {
            DWORD style;
            DWORD dwExtendedStyle;
            WORD cdit;
            short x, y, cx, cy;
            WORD menu, windowClass, title;
            wchar_t titleText[64];
        } dlgTemplate = {};
        #pragma pack(pop)

        dlgTemplate.style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        dlgTemplate.dwExtendedStyle = 0;
        dlgTemplate.cdit = 0;
        dlgTemplate.x = 0;
        dlgTemplate.y = 0;
        dlgTemplate.cx = 220;
        dlgTemplate.cy = 100;
        dlgTemplate.menu = 0;
        dlgTemplate.windowClass = 0;
        dlgTemplate.title = 0;
        wcsncpy_s(dlgTemplate.titleText, title.c_str(), 63);

        DialogBoxIndirectW(hInst, reinterpret_cast<LPCDLGTEMPLATEW>(&dlgTemplate), hParent, CustomDlgProc);
    }

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

    if (rollServer_)
    {
        rollServer_->stop();
        rollServer_.reset();
    }

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

    if (logWindowHandle_)
    {
        DestroyWindow(logWindowHandle_);
        logWindowHandle_ = nullptr;
    }

    statusWindowHandle_ = nullptr;
    statusTextHandle_ = nullptr;
    setupListHandle_ = nullptr;
    setupScanButtonHandle_ = nullptr;
    setupSaveButtonHandle_ = nullptr;
    logTextHandle_ = nullptr;
}

bool TrayApp::initialize(HINSTANCE instanceHandle, const std::wstring& configPath)
{
    instanceHandle_ = instanceHandle;
    configPath_ = configPath;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

    // Set up settings path (pixels.ini next to pixels.cfg)
    const size_t slash = configPath_.find_last_of(L"\\/");
    const std::wstring folder = (slash == std::wstring::npos) ? L"." : configPath_.substr(0, slash);
    settingsPath_ = folder + L"\\pixels.ini";
    logFilePath_ = folder + L"\\pixels_log.txt";

    // Create default INI if missing and load settings
    TraySettings::createDefaultIfMissing(settingsPath_);
    loadSettings();

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

    if (!createLogWindow())
    {
        return false;
    }

    if (!addTrayIcon())
    {
        return false;
    }

    auto logger = [this](const std::string& message)
    {
        appendLog(message);
    };

    // Create the roll server (named pipe endpoint for BG3 mod communication)
    rollServer_ = std::make_unique<RollServer>(logger);

    // Roll observer: forward every roll event to the RollServer
    auto rollObserver = [this](const std::string& label, int face)
    {
        if (rollServer_)
        {
            rollServer_->onRoll(label, face);
        }
    };

    runtime_ = std::make_unique<PixelsRuntimeService>(logger, [this]()
    {
        stateDirty_ = true;
    }, rollObserver);

    std::string error;
    configLoaded_ = runtime_->loadConfig(narrowAscii(configPath_), error);
    if (!configLoaded_)
    {
        ShowCustomMessageBox(instanceHandle_, windowHandle_, L"Pixels Tray",
            L"Welcome to Pixels Tray!\n\nNo dice configuration found.\nLet's set up your dice now.");
        showSetupWindow();
    }
    else if (!runtime_->start())
    {
        MessageBoxW(windowHandle_, L"Failed to start runtime service.", L"Pixels Tray", MB_ICONERROR | MB_OK);
    }

    // Start the roll server — it provides dice snapshot info to determine connected dice count
    rollServer_->start([this]() -> std::vector<DieStatusSnapshot>
    {
        return runtime_ ? runtime_->snapshotDice() : std::vector<DieStatusSnapshot>{};
    });

    SetTimer(windowHandle_, kTooltipTimerId, 1200, nullptr);
    SetTimer(windowHandle_, kNotificationTimerId, 3000, nullptr);
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
            updateLogWindow();
            stateDirty_ = false;
        }
        if (wParam == kNotificationTimerId)
        {
            checkAndShowNotifications();
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
        KillTimer(hwnd, kNotificationTimerId);
        if (rollServer_)
        {
            rollServer_->stop();
            rollServer_.reset();
        }
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
        if (hwnd == logWindowHandle_)
        {
            ShowWindow(logWindowHandle_, SW_HIDE);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (hwnd == logWindowHandle_)
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, RGB(0, 255, 0));
            SetBkColor(hdc, RGB(12, 12, 12));
            static HBRUSH hLogBrush = CreateSolidBrush(RGB(12, 12, 12));
            return reinterpret_cast<LRESULT>(hLogBrush);
        }
        if (hwnd == statusWindowHandle_)
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, RGB(0, 200, 255));
            SetBkColor(hdc, RGB(18, 18, 24));
            static HBRUSH hStatusBrush = CreateSolidBrush(RGB(18, 18, 24));
            return reinterpret_cast<LRESULT>(hStatusBrush);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_SIZE:
        if (hwnd == logWindowHandle_ && logTextHandle_)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            SetWindowPos(logTextHandle_, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER);
            return 0;
        }
        if (hwnd == statusWindowHandle_ && statusTextHandle_)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            SetWindowPos(statusTextHandle_, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER);
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
    wc.hbrBackground = CreateSolidBrush(RGB(18, 18, 24));
    wc.hIcon = LoadIconW(instanceHandle_, MAKEINTRESOURCEW(IDI_D20_APP_ICON));
    wc.hIconSm = LoadIconW(instanceHandle_, MAKEINTRESOURCEW(IDI_D20_APP_ICON));

    if (!RegisterClassExW(&wc))
    {
        return false;
    }

    statusWindowHandle_ = CreateWindowExW(
        0,
        statusWindowClassName_.c_str(),
        L"Pixels Dice Status",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        800,
        nullptr,
        nullptr,
        instanceHandle_,
        this);

    if (!statusWindowHandle_)
    {
        return false;
    }

    statusTextHandle_ = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0,
        0,
        900,
        800,
        statusWindowHandle_,
        nullptr,
        instanceHandle_,
        nullptr);

    HFONT statusFont = CreateFontW(
        24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (statusFont)
    {
        SendMessageW(statusTextHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(statusFont), TRUE);
    }

    return statusTextHandle_ != nullptr;
}

bool TrayApp::createSetupWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayApp::windowProc;
    wc.hInstance = instanceHandle_;
    wc.lpszClassName = setupWindowClassName_.c_str();
    wc.hIcon = LoadIconW(instanceHandle_, MAKEINTRESOURCEW(IDI_D20_APP_ICON));
    wc.hIconSm = LoadIconW(instanceHandle_, MAKEINTRESOURCEW(IDI_D20_APP_ICON));

    if (!RegisterClassExW(&wc))
    {
        return false;
    }

    setupWindowHandle_ = CreateWindowExW(
        0,
        setupWindowClassName_.c_str(),
        L"Pixels Setup",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        560,
        480,
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
        L"Wait until all your dice appear in the list, select them, then click Save Setup:",
        WS_CHILD | WS_VISIBLE,
        12,
        12,
        530,
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
    ShowCustomMessageBox(instanceHandle_, windowHandle_, L"Pixels Tray",
        L"Setup saved!\n\nYour dice are now being connected.");
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
    ss << L"PIXELS DICE STATUS\r\n";
    ss << L"==================\r\n\r\n";

    if (!runtime_)
    {
        ss << L"[X] Runtime unavailable.";
        return ss.str();
    }

    ss << L"SYSTEM\r\n";
    ss << L"------\r\n";
    ss << L"  Running:   " << (runtime_->isRunning() ? L"Yes" : L"No") << L"\r\n";
    ss << L"  Scanning:  " << (runtime_->isScanning() ? L"Yes" : L"No") << L"\r\n";
    ss << L"  Config:    " << (configLoaded_ ? L"Loaded" : L"Missing") << L"\r\n\r\n";

    const auto snapshots = runtime_->snapshotDice();
    if (snapshots.empty())
    {
        ss << L"No configured dice found.";
        return ss.str();
    }

    for (size_t i = 0; i < snapshots.size(); ++i)
    {
        const auto& die = snapshots[i];
        const DiceTheme theme = getDiceTheme(die.label);

        ss << theme.displayName << L" (Die " << (i + 1) << L")\r\n";
        ss << L"------\r\n";

        std::wstring statusIcon;
        switch (die.status)
        {
        case Systemic::Pixels::PixelStatus::Ready:
            statusIcon = L"[OK]";
            break;
        case Systemic::Pixels::PixelStatus::Connecting:
        case Systemic::Pixels::PixelStatus::Identifying:
            statusIcon = L"[..]";
            break;
        case Systemic::Pixels::PixelStatus::Disconnected:
            statusIcon = L"[--]";
            break;
        default:
            statusIcon = L"[??]";
            break;
        }
        ss << L"  Status:     " << statusIcon << L" " << statusToWide(die.status) << L"\r\n";

        std::wstring batteryIcon;
        if (die.isCharging)
        {
            batteryIcon = L"[+]";
        }
        else if (die.batteryLevel > 50)
        {
            batteryIcon = L"[=]";
        }
        else if (die.batteryLevel > 20)
        {
            batteryIcon = L"[-]";
        }
        else
        {
            batteryIcon = L"[!]";
        }
        ss << L"  Battery:    " << batteryIcon << L" " << die.batteryLevel << L"%" << (die.isCharging ? L" (charging)" : L"") << L"\r\n";

        ss << L"  Current:    " << die.currentFace << L"\r\n";

        if (die.hasLastRoll)
        {
            ss << L"  Last Roll:  " << die.lastRollFace << L"\r\n";
        }
        else
        {
            ss << L"  Last Roll:  --\r\n";
        }

        ss << L"  Recent:     ";
        if (die.recentRollFaces.empty())
        {
            ss << L"--";
        }
        else
        {
            for (size_t r = 0; r < die.recentRollFaces.size(); ++r)
            {
                if (r > 0)
                {
                    ss << L" > ";
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
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW_STATUS, L"\U0001F3B2  Show Status");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW_LOGS, L"\U0001F4DC  Show Roll Log");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_RESCAN, L"\U0001F504  Rescan Dice");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SETUP, L"\u2699  Setup Dice...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXPORT_CFG, L"\U0001F4E4  Export Config...");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_IMPORT_CFG, L"\U0001F4E5  Import Config...");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_FOLDER, L"\U0001F4C1  Open Config Folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, L"\u274C  Quit");

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
    case IDM_TRAY_SHOW_STATUS:
        showStatusWindow();
        break;
    case IDM_TRAY_SHOW_LOGS:
        showLogWindow();
        break;
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

bool TrayApp::createLogWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayApp::windowProc;
    wc.hInstance = instanceHandle_;
    wc.lpszClassName = logWindowClassName_.c_str();
    wc.hbrBackground = CreateSolidBrush(RGB(12, 12, 12));
    wc.hIcon = LoadIconW(instanceHandle_, MAKEINTRESOURCEW(IDI_D20_APP_ICON));
    wc.hIconSm = LoadIconW(instanceHandle_, MAKEINTRESOURCEW(IDI_D20_APP_ICON));

    if (!RegisterClassExW(&wc))
    {
        return false;
    }

    logWindowHandle_ = CreateWindowExW(
        0,
        logWindowClassName_.c_str(),
        L"Pixels Roll Log",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        650,
        nullptr,
        nullptr,
        instanceHandle_,
        this);

    if (!logWindowHandle_)
    {
        return false;
    }

    logTextHandle_ = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
        0,
        0,
        900,
        650,
        logWindowHandle_,
        nullptr,
        instanceHandle_,
        nullptr);

    HFONT monoFont = CreateFontW(
        24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (monoFont)
    {
        SendMessageW(logTextHandle_, WM_SETFONT, reinterpret_cast<WPARAM>(monoFont), TRUE);
    }

    return logTextHandle_ != nullptr;
}

void TrayApp::showLogWindow()
{
    if (!logWindowHandle_)
    {
        return;
    }

    updateLogWindow();
    ShowWindow(logWindowHandle_, SW_SHOWNORMAL);
    SetForegroundWindow(logWindowHandle_);
}

void TrayApp::appendLog(const std::string& message)
{
    std::wstring wideMsg = toWide(message);

    // Always write to file if enabled (full logging)
    if (settings_.logToFile)
    {
        writeLogToFile(wideMsg);
    }

    // Only show in UI if it passes the filter
    if (!shouldShowLogMessage(message))
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(logMutex_);
        logLines_.push_back(wideMsg);
        while (logLines_.size() > kMaxLogLines)
        {
            logLines_.pop_front();
        }
    }

    stateDirty_ = true;
}

void TrayApp::updateLogWindow()
{
    if (!logWindowHandle_ || !logTextHandle_)
    {
        return;
    }

    if (!IsWindowVisible(logWindowHandle_))
    {
        return;
    }

    std::wstringstream ss;
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        for (const auto& line : logLines_)
        {
            ss << line << L"\r\n";
        }
    }

    SetWindowTextW(logTextHandle_, ss.str().c_str());

    // Auto-scroll to bottom to show newest messages
    int textLen = GetWindowTextLengthW(logTextHandle_);
    SendMessageW(logTextHandle_, EM_SETSEL, textLen, textLen);
    SendMessageW(logTextHandle_, EM_SCROLLCARET, 0, 0);
}

void TrayApp::checkAndShowNotifications()
{
    if (!runtime_ || !configLoaded_)
    {
        return;
    }

    const auto snapshots = runtime_->snapshotDice();
    if (snapshots.empty())
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    bool allReady = true;
    for (const auto& die : snapshots)
    {
        if (die.status != Systemic::Pixels::PixelStatus::Ready)
        {
            allReady = false;
        }
    }

    if (allReady && !allDiceReadyNotified_)
    {
        allDiceReadyNotified_ = true;
        std::wstringstream msg;
        msg << L"All " << snapshots.size() << L" Pixels dice are connected and ready to roll!";
        showBalloonNotification(L"Pixels Ready!", msg.str());
    }

    for (const auto& die : snapshots)
    {
        if (die.batteryLevel <= kLowBatteryThreshold && !die.isCharging && die.hasPixel)
        {
            if (lowBatteryNotifiedIds_.find(die.targetPixelId) == lowBatteryNotifiedIds_.end())
            {
                lowBatteryNotifiedIds_.insert(die.targetPixelId);
                std::wstringstream msg;
                msg << toWide(die.label) << L" battery is at " << die.batteryLevel << L"%.\nPlease charge it soon!";
                showBalloonNotification(L"Low Battery Warning", msg.str(), NIIF_WARNING);
            }
        }

        if (die.status == Systemic::Pixels::PixelStatus::Disconnected)
        {
            auto it = disconnectTimestamps_.find(die.targetPixelId);
            if (it == disconnectTimestamps_.end())
            {
                disconnectTimestamps_[die.targetPixelId] = now;
            }
            else
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
                if (elapsed >= kDisconnectWarningSeconds)
                {
                    if (disconnectWarningShownIds_.find(die.targetPixelId) == disconnectWarningShownIds_.end())
                    {
                        disconnectWarningShownIds_.insert(die.targetPixelId);
                        std::wstringstream msg;
                        msg << toWide(die.label) << L" has been disconnected for over " << kDisconnectWarningSeconds << L" seconds.\nCheck if the die is nearby or needs charging.";
                        showBalloonNotification(L"Dice Disconnected", msg.str(), NIIF_WARNING);
                    }
                }
            }
        }
        else
        {
            disconnectTimestamps_.erase(die.targetPixelId);
            disconnectWarningShownIds_.erase(die.targetPixelId);
        }
    }
}

void TrayApp::showBalloonNotification(const std::wstring& title, const std::wstring& message, DWORD iconType)
{
    if (!trayIconAdded_)
    {
        return;
    }

    trayIconData_.uFlags = NIF_INFO;
    wcsncpy_s(trayIconData_.szInfoTitle, _countof(trayIconData_.szInfoTitle), title.c_str(), _TRUNCATE);
    wcsncpy_s(trayIconData_.szInfo, _countof(trayIconData_.szInfo), message.c_str(), _TRUNCATE);
    trayIconData_.dwInfoFlags = iconType;
    trayIconData_.uTimeout = 5000;

    Shell_NotifyIconW(NIM_MODIFY, &trayIconData_);
}

TrayApp::DiceTheme TrayApp::getDiceTheme(const std::string& label) const
{
    DiceTheme theme;

    std::string lowerLabel = label;
    for (char& c : lowerLabel)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (lowerLabel.find("aurora") != std::string::npos || lowerLabel.find("sky") != std::string::npos)
    {
        theme.primaryColor = RGB(0, 191, 255);
        theme.secondaryColor = RGB(135, 206, 250);
        theme.textColor = RGB(0, 51, 102);
        theme.displayName = L"Aurora Sky";
    }
    else if (lowerLabel.find("midnight") != std::string::npos || lowerLabel.find("galaxy") != std::string::npos)
    {
        theme.primaryColor = RGB(75, 0, 130);
        theme.secondaryColor = RGB(138, 43, 226);
        theme.textColor = RGB(230, 230, 250);
        theme.displayName = L"Midnight Galaxy";
    }
    else
    {
        theme.primaryColor = RGB(100, 100, 100);
        theme.secondaryColor = RGB(150, 150, 150);
        theme.textColor = RGB(255, 255, 255);
        theme.displayName = L"Pixels Die";
    }

    return theme;
}

void TrayApp::loadSettings()
{
    settings_ = TraySettings::load(settingsPath_);

    if (settings_.logToFile)
    {
        std::wstring path = settings_.logFilePath.empty() ? logFilePath_ : settings_.logFilePath;
        logFile_.open(std::string(path.begin(), path.end()), std::ios::app);
    }
}

void TrayApp::writeLogToFile(const std::wstring& message)
{
    if (!logFile_.is_open())
    {
        return;
    }

    std::string narrow;
    narrow.reserve(message.size());
    for (wchar_t c : message)
        narrow += static_cast<char>(c);
    logFile_ << narrow << std::endl;
}

bool TrayApp::shouldShowLogMessage(const std::string& message) const
{
    if (settings_.debugMode)
    {
        return true;
    }

    // In non-debug mode, only show roll results
    return message.find("Rolled on face") != std::string::npos;
}
