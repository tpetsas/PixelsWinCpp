#include "pch.h"

#include <windows.h>

#include "App/Tray/TrayApp.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    TrayApp app;
    if (!app.initialize(hInstance, L"pixels.cfg"))
    {
        return 1;
    }

    return app.runMessageLoop();
}
