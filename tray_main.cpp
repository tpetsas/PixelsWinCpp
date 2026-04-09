#include "pch.h"

#include <string>
#include <windows.h>
#include <shellscalingapi.h>

#pragma comment(lib, "Shcore.lib")

#include "App/Tray/TrayApp.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    // Enable Per-Monitor DPI awareness for sharp text rendering
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t modulePath[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(_countof(modulePath)));
    std::wstring exePath = modulePath;
    const size_t slash = exePath.find_last_of(L"\\/");
    const std::wstring exeFolder = (slash == std::wstring::npos) ? L"." : exePath.substr(0, slash);
    const std::wstring configPath = exeFolder + L"\\pixels.cfg";

    TrayApp app;
    if (!app.initialize(hInstance, configPath))
    {
        return 1;
    }

    return app.runMessageLoop();
}
