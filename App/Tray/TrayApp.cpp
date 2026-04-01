#include "pch.h"
#include "App/Tray/TrayApp.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include <commdlg.h>
#include <shellapi.h>

#include "App/Tray/TrayResources.h"

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

    bool pathExists(const std::wstring& path)
    {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES;
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
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

    if (!createMessageWindow())
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
            L"Pixels tray started, but no valid pixels.cfg was found.\n\n"
            L"Next steps:\n"
            L"1) Right-click tray icon\n"
            L"2) Click 'Setup dice (CLI)'\n"
            L"3) After setup completes, click 'Rescan dice'\n\n"
            L"Config path:\n" + configPath_ +
            L"\n\nDetails: " + toWide(error);
        MessageBoxW(windowHandle_, msg.c_str(), L"Pixels Tray", MB_ICONWARNING | MB_OK);
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
            stateDirty_ = false;
        }
        return 0;
    case WM_COMMAND:
        handleCommand(LOWORD(wParam));
        return 0;
    case kTrayCallbackMsg:
        if (lParam == WM_LBUTTONDBLCLK)
        {
            refreshTooltip();
            MessageBoxW(hwnd, buildTooltipText().c_str(), L"Pixels Tray", MB_OK | MB_ICONINFORMATION);
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

void TrayApp::showContextMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }

    AppendMenuW(menu, MF_STRING, IDM_TRAY_RESCAN, L"Rescan dice");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SETUP, L"Setup dice (CLI)");
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
    const std::wstring cliPath = exeFolder() + L"\\Pixels.exe";
    if (!pathExists(cliPath))
    {
        MessageBoxW(windowHandle_, L"Pixels.exe not found next to tray app.", L"Pixels Tray", MB_ICONERROR | MB_OK);
        return;
    }

    std::wstring cmd = L"\"" + cliPath + L"\" --setup";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, exeFolder().c_str(), &si, &pi))
    {
        MessageBoxW(windowHandle_, L"Failed to launch setup CLI.", L"Pixels Tray", MB_ICONERROR | MB_OK);
        return;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    MessageBoxW(windowHandle_, L"Setup launched in a new console. After saving config, click Rescan dice.", L"Pixels Tray", MB_ICONINFORMATION | MB_OK);
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

std::wstring TrayApp::exeFolder() const
{
    wchar_t modulePath[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(_countof(modulePath)));
    std::wstring fullPath = modulePath;
    const size_t slash = fullPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return L".";
    }
    return fullPath.substr(0, slash);
}
